// Wireless test link. One connection at a time, one text command per line, answered with "OK ..." or "ERR ...":
//   PING                     frame, milliseconds since start and free memory
//   LS <folder>              one "<size> <name>" line per entry, then END
//   GET <path>               "OK <size>", then the bytes
//   PUT <path> <size> <crc>  "GO", then the bytes, then "OK" once they are checked and in place
//   PUTZ <path> <size> <crc> <unpacked size>  the same with zlib-packed bytes; size is packed, the checksum is of the unpacked file
//   PAD <hex bits> <frames>  hold PS2 pad buttons for that many game frames
//   SHOTS <n>, DRAW <n>      change the screenshot and picture skipping settings while running
//   QUIT, RUN                leave to the Homebrew Menu, or leave and start /3ds/p3fes.3dsx again
// There is no password, on purpose: anyone on the network may replace the game. So it stays narrow. PUT writes only
// /3ds/p3fes.3dsx and /p3fes/args.txt, a program must start like a 3DSX and match its checksum, and the old one is kept as
// /3ds/p3fes.3dsx.bak. It reads only logs, screenshots and crash reports: never the disc, the boot program, saved states
// or Luma's backups. "link 0" in args.txt turns the link off.
#include "remote3ds.hpp"

#include "pica.hpp"

#include <3ds.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

namespace remote {
namespace {

constexpr int kPort = 5577;
constexpr std::size_t kSocketMemory = 0x40000;
constexpr std::size_t kChunk = 0x8000;
constexpr std::uint32_t kPollWhileIdle = 15;   // vblanks between looks for a new connection
constexpr std::uint32_t kRetryListening = 120;
constexpr const char *kProgram = "/3ds/p3fes.3dsx";
constexpr const char *kSettings = "/p3fes/args.txt";
constexpr std::uint64_t kMostBytes = 64u << 20;   // more than the code region of a 3DS program
// A transfer keeps reading or sending for this long each vblank instead of making one call. The socket service hands over a few
// kilobytes a call, so one call per vblank held uploads to about 225 KB/s.
constexpr u64 kBusyTicks = CPU_TICKS_PER_MSEC * 10;
// A connection that goes quiet this long is dropped. When the console's Wi-Fi drops, the other end's close never arrives, and
// without this the link would keep waiting on that connection and never take a new one.
constexpr u64 kQuietTransfer = CPU_TICKS_PER_MSEC * 20000;
constexpr u64 kQuietCommands = CPU_TICKS_PER_MSEC * 120000;

u32 *g_memory = nullptr;
int g_listener = -1, g_client = -1, g_last_error = 0;
bool g_up = false;
bool g_sending = false, g_receiving = false;
char g_line[640];
std::size_t g_line_used = 0;
char g_chunk[kChunk];
std::size_t g_chunk_at = 0, g_chunk_end = 0;
FILE *g_file = nullptr;
std::uint64_t g_left = 0;
char g_target[64], g_partial[72];
bool g_is_program = false, g_first_block = false;
bool g_packed = false;   // the bytes arriving are zlib-packed
z_stream g_zip{};
char g_unpacked[kChunk];
// The card writes small pieces slowly: uploads went to it about 5 KB at a time, and writing took 41.7 of 44 seconds.
char g_write_buffer[0x20000];
std::uint64_t g_unpacked_left = 0;
std::uint32_t g_crc = 0, g_wanted_crc = 0;
std::uint32_t g_crc_table[256];
std::uint32_t g_ticks = 0;
u64 g_heard = 0;   // when the client last sent or took bytes
struct UploadTimes { u64 start, receive, unpack, write, wait; std::uint32_t calls; } g_times{};   // written to log.txt per upload
volatile std::uint16_t g_buttons = 0;
volatile std::uint64_t g_buttons_until = 0;
volatile std::uint32_t g_hold_frames = 0;
bool g_exit = false, g_restart = false;

bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK; }

void say(const char *text) {
    std::size_t left = std::strlen(text);
    int tries = 2000;
    while (left && g_client >= 0) {
        const ssize_t sent = send(g_client, text, left, 0);
        if (sent > 0) {
            text += sent;
            left -= static_cast<std::size_t>(sent);
        } else if (sent < 0 && would_block() && --tries > 0) {
            svcSleepThread(1000000);
        } else {
            return;
        }
    }
}

void close_file() {
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
}

void end_unpacking() {
    if (g_packed) inflateEnd(&g_zip);
    g_packed = false;
}

void log_times(const char *how) {
    const auto ms = [](u64 ticks) { return static_cast<unsigned long>(ticks / CPU_TICKS_PER_MSEC); };
    std::fprintf(stderr, "[link] upload %s after %lu ms: receive %lu ms in %lu calls, unpack %lu, write %lu, waiting %lu\n", how,
                 ms(svcGetSystemTick() - g_times.start), ms(g_times.receive), static_cast<unsigned long>(g_times.calls),
                 ms(g_times.unpack), ms(g_times.write), ms(g_times.wait));
}

void drop_client(const char *why = nullptr) {
    if (why && g_client >= 0) std::fprintf(stderr, "[link] connection dropped: %s\n", why);
    if (g_receiving) log_times("stopped");
    close_file();
    end_unpacking();
    if (g_receiving) std::remove(g_partial);
    if (g_client >= 0) close(g_client);
    g_client = -1;
    g_sending = g_receiving = false;
    g_line_used = 0;
}

void add_to_crc(const char *data, std::size_t size) {
    if (!g_crc_table[1]) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            g_crc_table[i] = c;
        }
    }
    std::uint32_t c = ~g_crc;
    for (std::size_t i = 0; i < size; ++i) c = g_crc_table[(c ^ static_cast<std::uint8_t>(data[i])) & 0xFF] ^ (c >> 8);
    g_crc = ~c;
}

bool under(const char *path, const char *folder) { return !std::strncmp(path, folder, std::strlen(folder)); }

bool plain(const char *path) { return path[0] == '/' && !std::strstr(path, "..") && std::strlen(path) < 280; }

bool is_game_data(const char *path) {
    const char *name = std::strrchr(path, '/') + 1;
    const std::size_t length = std::strlen(name);
    return !std::strncmp(name, "disc.", 5) || !std::strncmp(name, "SLUS_", 5) ||
           (length > 6 && !std::strcmp(name + length - 6, ".state"));
}

bool may_read(const char *path) {
    return plain(path) && ((under(path, "/p3fes/") && !is_game_data(path)) || under(path, "/luma/dumps/"));
}

bool may_list(const char *path) {
    return plain(path) && (!std::strcmp(path, "/p3fes") || under(path, "/p3fes/") || under(path, "/luma/dumps"));
}

void list_folder(const char *path) {
    char full[320], line[340];
    std::snprintf(full, sizeof full, "sdmc:%s", path);
    DIR *folder = opendir(full);
    if (!folder) return say("ERR no such folder\n");
    say("OK\n");
    while (const dirent *entry = readdir(folder)) {
        if (entry->d_name[0] == '.') continue;
        char item[600];
        std::snprintf(item, sizeof item, "%s/%s", full, entry->d_name);
        struct stat info {};
        stat(item, &info);
        std::snprintf(line, sizeof line, "%llu %s%s\n", static_cast<unsigned long long>(info.st_size), entry->d_name,
                      S_ISDIR(info.st_mode) ? "/" : "");
        say(line);
    }
    closedir(folder);
    say("END\n");
}

void start_get(const char *path) {
    char full[320], line[40];
    std::snprintf(full, sizeof full, "sdmc:%s", path);
    struct stat info {};
    g_file = stat(full, &info) == 0 ? std::fopen(full, "rb") : nullptr;
    if (!g_file) return say("ERR cannot read that file\n");
    std::snprintf(line, sizeof line, "OK %llu\n", static_cast<unsigned long long>(info.st_size));
    say(line);
    g_left = static_cast<std::uint64_t>(info.st_size);
    g_chunk_at = g_chunk_end = 0;
    g_sending = true;
}

// A saved state may go straight into the game's folder, under a plain name.
bool may_put_state(const char *path) {
    constexpr std::size_t kFolder = 7, kEnding = 6;   // "/p3fes/" and ".state"
    const std::size_t length = std::strlen(path);
    return length > kFolder + kEnding && !std::strncmp(path, "/p3fes/", kFolder) && !std::strcmp(path + length - kEnding, ".state") &&
           !std::strchr(path + kFolder, '/') && !std::strstr(path, "..");
}

void start_put(const char *path, std::uint64_t size, std::uint32_t crc, bool packed = false, std::uint64_t unpacked = 0) {
    g_is_program = !std::strcmp(path, kProgram);
    if (!g_is_program && std::strcmp(path, kSettings) && !may_put_state(path)) return say("ERR not allowed\n");
    if (size > kMostBytes || (g_is_program && size < 4)) return say("ERR wrong size\n");
    if (packed && (unpacked > kMostBytes || (g_is_program && unpacked < 4))) return say("ERR wrong size\n");
    if (packed) {
        g_zip = z_stream{};
        if (inflateInit(&g_zip) != Z_OK) return say("ERR cannot unpack\n");
        g_packed = true;
        g_unpacked_left = unpacked;
    }
    std::snprintf(g_target, sizeof g_target, "sdmc:%s", path);
    std::snprintf(g_partial, sizeof g_partial, "%s.part", g_target);
    g_file = std::fopen(g_partial, "wb");
    if (!g_file) {
        end_unpacking();
        return say("ERR cannot write there\n");
    }
    std::setvbuf(g_file, g_write_buffer, _IOFBF, sizeof g_write_buffer);
    g_left = size;
    g_wanted_crc = crc;
    g_crc = 0;
    g_first_block = true;
    g_receiving = true;
    g_times = UploadTimes{svcGetSystemTick(), 0, 0, 0, 0, 0};
    say("GO\n");
}

void finish_put() {
    const u64 before = svcGetSystemTick();
    close_file();
    g_times.write += svcGetSystemTick() - before;
    log_times("done");
    g_receiving = false;
    const bool whole = !g_packed || (g_zip.avail_in == 0 && g_unpacked_left == 0 && inflate(&g_zip, Z_FINISH) == Z_STREAM_END);
    end_unpacking();
    if (!whole) {
        std::remove(g_partial);
        return say("ERR the packed file did not unpack to its size, nothing changed\n");
    }
    if (g_crc != g_wanted_crc) {
        std::remove(g_partial);
        return say("ERR checksum does not match, nothing changed\n");
    }
    if (g_is_program) {
        char backup[72];
        std::snprintf(backup, sizeof backup, "%s.bak", g_target);
        std::remove(backup);
        std::rename(g_target, backup);
    } else {
        std::remove(g_target);
    }
    const bool placed = std::rename(g_partial, g_target) == 0;
    std::fprintf(stderr, "[link] %s %s\n", placed ? "wrote" : "could not write", g_target);
    say(placed ? "OK\n" : "ERR could not put the file in place\n");
}

void run_command(char *line, std::uint64_t frame) {
    char *rest = std::strchr(line, ' ');
    if (rest) *rest++ = 0;
    char reply[160];
    if (!std::strcmp(line, "PING")) {
        std::snprintf(reply, sizeof reply, "OK frame %llu ms %llu heap_kb %lu linear_kb %lu\n",
                      static_cast<unsigned long long>(frame),
                      static_cast<unsigned long long>(svcGetSystemTick() / static_cast<u64>(CPU_TICKS_PER_MSEC)),
                      static_cast<unsigned long>(mallinfo().fordblks >> 10),
                      static_cast<unsigned long>(linearSpaceFree() >> 10));
        say(reply);
    } else if (!std::strcmp(line, "LS") && rest) {
        if (may_list(rest)) list_folder(rest);
        else say("ERR not allowed\n");
    } else if (!std::strcmp(line, "GET") && rest) {
        if (may_read(rest)) start_get(rest);
        else say("ERR not allowed\n");
    } else if ((!std::strcmp(line, "PUT") || !std::strcmp(line, "PUTZ")) && rest) {
        char *more = nullptr, *size = std::strchr(rest, ' ');
        if (!size) return say("ERR needs a path, a size and a checksum\n");
        *size++ = 0;
        const std::uint64_t bytes = std::strtoull(size, &more, 10);
        const auto crc = static_cast<std::uint32_t>(std::strtoul(more, &more, 16));
        if (line[3] == 'Z') start_put(rest, bytes, crc, true, std::strtoull(more, nullptr, 10));
        else start_put(rest, bytes, crc);
    } else if (!std::strcmp(line, "PAD") && rest) {
        char *more = nullptr;
        const auto bits = static_cast<std::uint16_t>(std::strtoul(rest, &more, 16));
        g_hold_frames = static_cast<std::uint32_t>(std::strtoul(more, nullptr, 10));
        g_buttons_until = 0;
        g_buttons = bits;
        say("OK\n");
    } else if (!std::strcmp(line, "SHOTS") && rest) {
        mkdir("sdmc:/p3fes/shots", 0777);
        ps2::gs::pica::set_shot_every(static_cast<std::uint32_t>(std::strtoul(rest, nullptr, 10)));
        say("OK\n");
    } else if (!std::strcmp(line, "DRAW") && rest) {
        ps2::gs::pica::set_draw_every(static_cast<std::uint32_t>(std::strtoul(rest, nullptr, 10)));
        say("OK\n");
    } else if (!std::strcmp(line, "QUIT") || !std::strcmp(line, "RUN")) {
        g_restart = !std::strcmp(line, "RUN");
        g_exit = true;
        say("OK\n");
    } else {
        say("ERR unknown command\n");
    }
}

void send_more() {
    if (g_chunk_at == g_chunk_end) {
        const std::size_t want = g_left < kChunk ? static_cast<std::size_t>(g_left) : kChunk;
        g_chunk_at = 0;
        g_chunk_end = want ? std::fread(g_chunk, 1, want, g_file) : 0;
        g_left -= g_chunk_end;
        if (!g_chunk_end) {
            close_file();
            g_sending = false;
            return;
        }
    }
    const ssize_t sent = send(g_client, g_chunk + g_chunk_at, g_chunk_end - g_chunk_at, 0);
    if (sent > 0) {
        g_chunk_at += static_cast<std::size_t>(sent);
        g_heard = svcGetSystemTick();
    } else if (!would_block()) {
        drop_client("send failed");
    }
}

// Bytes of the file itself: checked, counted into the checksum and written.
bool write_plain(const char *data, std::size_t size) {
    if (!size) return true;
    if (g_first_block && g_is_program && (size < 4 || std::memcmp(data, "3DSX", 4))) {
        say("ERR not a 3DSX program, nothing changed\n");
        drop_client();
        return false;
    }
    g_first_block = false;
    add_to_crc(data, size);
    const u64 before = svcGetSystemTick();
    const bool written = std::fwrite(data, 1, size, g_file) == size;
    g_times.write += svcGetSystemTick() - before;
    if (!written) {
        drop_client();
        return false;
    }
    return true;
}

// Bytes as they arrive, packed or not. Returns false if the client was dropped.
bool take(const char *data, std::size_t size) {
    g_left -= size;
    if (!g_packed) return write_plain(data, size);
    g_zip.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data));
    g_zip.avail_in = static_cast<uInt>(size);
    for (;;) {
        g_zip.next_out = reinterpret_cast<Bytef *>(g_unpacked);
        g_zip.avail_out = sizeof g_unpacked;
        const u64 before = svcGetSystemTick();
        const int status = inflate(&g_zip, Z_NO_FLUSH);
        g_times.unpack += svcGetSystemTick() - before;
        const std::size_t made = sizeof g_unpacked - g_zip.avail_out;
        if ((status != Z_OK && status != Z_STREAM_END && status != Z_BUF_ERROR) || made > g_unpacked_left) {
            say("ERR the packed file is damaged, nothing changed\n");
            drop_client();
            return false;
        }
        g_unpacked_left -= made;
        if (!write_plain(g_unpacked, made)) return false;
        if (status == Z_STREAM_END || g_zip.avail_out != 0) return true;   // done, or all input used
    }
}

void receive_more() {
    const u64 until = svcGetSystemTick() + kBusyTicks;
    while (g_receiving && g_left) {
        const std::size_t want = g_left < kChunk ? static_cast<std::size_t>(g_left) : kChunk;
        const u64 before = svcGetSystemTick();
        const ssize_t got = recv(g_client, g_chunk, want, 0);
        const u64 after = svcGetSystemTick();
        g_times.receive += after - before;
        ++g_times.calls;
        if (got > 0) {
            g_heard = after;
            if (!take(g_chunk, static_cast<std::size_t>(got))) return;
        } else if (got == 0 || !would_block()) {
            return drop_client(got == 0 ? "closed during an upload" : "receive failed during an upload");
        } else if (svcGetSystemTick() < until) {
            const u64 before = svcGetSystemTick();
            svcSleepThread(250000);
            g_times.wait += svcGetSystemTick() - before;
        } else {
            break;
        }
        if (svcGetSystemTick() >= until) break;
    }
    if (g_receiving && !g_left) finish_put();
}

void read_commands(std::uint64_t frame) {
    const ssize_t got = recv(g_client, g_line + g_line_used, sizeof g_line - 1 - g_line_used, 0);
    if (got == 0) return drop_client();
    if (got < 0 && !would_block()) return drop_client("receive failed");
    if (got > 0) {
        g_line_used += static_cast<std::size_t>(got);
        g_heard = svcGetSystemTick();
    }
    while (g_client >= 0 && !g_sending && !g_receiving) {
        char *end = static_cast<char *>(std::memchr(g_line, '\n', g_line_used));
        if (!end) {
            if (g_line_used >= sizeof g_line - 1) drop_client();
            return;
        }
        *end = 0;
        const std::size_t length = static_cast<std::size_t>(end - g_line) + 1;
        char line[sizeof g_line];
        std::memcpy(line, g_line, length);
        std::memmove(g_line, g_line + length, g_line_used - length);
        g_line_used -= length;
        run_command(line, frame);
    }
    if (g_receiving && g_line_used) {   // the first bytes of an upload can arrive with the command
        const std::size_t size = g_line_used < g_left ? g_line_used : static_cast<std::size_t>(g_left);
        g_line_used = 0;
        if (!take(g_line, size)) return;
        if (!g_left) finish_put();
    }
}

void open_listener() {
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return;
    const int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kPort);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0 || listen(listener, 1) != 0 ||
        fcntl(listener, F_SETFL, fcntl(listener, F_GETFL, 0) | O_NONBLOCK) != 0) {
        const int why = errno;
        close(listener);
        if (why != g_last_error) std::fprintf(stderr, "[link] cannot listen yet, errno %d\n", why);
        g_last_error = why;
        return;
    }
    g_listener = listener;
    in_addr self{};
    self.s_addr = gethostid();
    std::fprintf(stderr, "[link] listening on %s:%d\n", inet_ntoa(self), kPort);
    std::printf("Wireless link: %s:%d\n", inet_ntoa(self), kPort);
}

}

void start() {
    g_memory = static_cast<u32 *>(memalign(0x1000, kSocketMemory));
    const Result rc = g_memory ? socInit(g_memory, kSocketMemory) : -1;
    if (R_FAILED(rc)) {
        std::fprintf(stderr, "[link] off: no network service, 0x%08lX\n", static_cast<unsigned long>(rc));
        std::free(g_memory);
        g_memory = nullptr;
        return;
    }
    g_up = true;
    open_listener();
}

void poll(std::uint64_t frame) {
    if (!g_up) return;
    if (g_listener < 0) {   // the last run's port can stay taken for a while after a restart
        if (++g_ticks % kRetryListening == 0) open_listener();
        return;
    }
    if (g_client < 0) {
        if (++g_ticks % kPollWhileIdle) return;
        sockaddr_in from{};
        socklen_t size = sizeof from;
        g_client = accept(g_listener, reinterpret_cast<sockaddr *>(&from), &size);
        if (g_client >= 0) {
            fcntl(g_client, F_SETFL, fcntl(g_client, F_GETFL, 0) | O_NONBLOCK);
            g_heard = svcGetSystemTick();
        } else if (!would_block()) {   // a Wi-Fi drop can leave the listener dead: open a new one
            std::fprintf(stderr, "[link] listener failed, errno %d; opening it again\n", errno);
            close(g_listener);
            g_listener = -1;
            g_last_error = 0;
        }
        return;
    }
    if (svcGetSystemTick() - g_heard > (g_sending || g_receiving ? kQuietTransfer : kQuietCommands))
        return drop_client(g_sending || g_receiving ? "transfer stalled" : "idle");
    if (g_sending) send_more();
    else if (g_receiving) receive_more();
    else read_commands(frame);
}

void stop() {
    g_up = false;
    drop_client();
    if (g_listener >= 0) close(g_listener);
    g_listener = -1;
    if (g_memory) {
        socExit();
        std::free(g_memory);
        g_memory = nullptr;
    }
}

bool wants_exit() { return g_exit; }

void chain_restart() {
    if (!g_restart) return;
    Handle loader = 0;
    Result rc = svcConnectToPort(&loader, "hb:ldr");
    if (R_SUCCEEDED(rc)) {
        static const char kPath[] = "/3ds/p3fes.3dsx";
        u32 *command = getThreadCommandBuffer();
        command[0] = IPC_MakeHeader(2, 0, 2);
        command[1] = IPC_Desc_StaticBuffer(sizeof kPath, 0);
        command[2] = reinterpret_cast<u32>(kPath);
        rc = svcSendSyncRequest(loader);
        if (R_SUCCEEDED(rc)) rc = static_cast<Result>(command[1]);
        svcCloseHandle(loader);
    }
    std::fprintf(stderr, "[link] restart: 0x%08lX\n", static_cast<unsigned long>(rc));
    if (R_SUCCEEDED(rc)) aptSetChainloaderToSelf();
}

std::uint16_t buttons(std::uint64_t frame) {
    if (!g_buttons) return 0;
    if (!g_buttons_until) g_buttons_until = frame + g_hold_frames;
    if (frame >= g_buttons_until) {
        g_buttons = 0;
        return 0;
    }
    return g_buttons;
}

}
