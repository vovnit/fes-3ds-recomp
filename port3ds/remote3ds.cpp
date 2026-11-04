// Wireless test link. One connection at a time, one text command per line, answered with "OK ..." or "ERR ...":
//   PING                     frame, milliseconds since start and free memory
//   LS <folder>              one "<size> <name>" line per entry, then END
//   GET <path>               "OK <size>", then the bytes
//   PUT <path> <size> <crc>  "GO", then the bytes, then "OK" once they are checked and in place
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
std::uint32_t g_crc = 0, g_wanted_crc = 0;
std::uint32_t g_crc_table[256];
std::uint32_t g_ticks = 0;
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

void drop_client() {
    close_file();
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

void start_put(const char *path, std::uint64_t size, std::uint32_t crc) {
    g_is_program = !std::strcmp(path, kProgram);
    if (!g_is_program && std::strcmp(path, kSettings)) return say("ERR not allowed\n");
    if (size > kMostBytes || (g_is_program && size < 4)) return say("ERR wrong size\n");
    std::snprintf(g_target, sizeof g_target, "sdmc:%s", path);
    std::snprintf(g_partial, sizeof g_partial, "%s.part", g_target);
    g_file = std::fopen(g_partial, "wb");
    if (!g_file) return say("ERR cannot write there\n");
    g_left = size;
    g_wanted_crc = crc;
    g_crc = 0;
    g_first_block = true;
    g_receiving = true;
    say("GO\n");
}

void finish_put() {
    close_file();
    g_receiving = false;
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
    say(std::rename(g_partial, g_target) == 0 ? "OK\n" : "ERR could not put the file in place\n");
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
    } else if (!std::strcmp(line, "PUT") && rest) {
        char *more = nullptr, *size = std::strchr(rest, ' ');
        if (!size) return say("ERR needs a path, a size and a checksum\n");
        *size++ = 0;
        const std::uint64_t bytes = std::strtoull(size, &more, 10);
        start_put(rest, bytes, static_cast<std::uint32_t>(std::strtoul(more, nullptr, 16)));
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
    if (sent > 0) g_chunk_at += static_cast<std::size_t>(sent);
    else if (!would_block()) drop_client();
}

void receive_more() {
    const std::size_t want = g_left < kChunk ? static_cast<std::size_t>(g_left) : kChunk;
    const ssize_t got = want ? recv(g_client, g_chunk, want, 0) : 0;
    if (got > 0) {
        if (g_first_block && g_is_program && (got < 4 || std::memcmp(g_chunk, "3DSX", 4))) {
            say("ERR not a 3DSX program, nothing changed\n");
            return drop_client();
        }
        g_first_block = false;
        add_to_crc(g_chunk, static_cast<std::size_t>(got));
        if (std::fwrite(g_chunk, 1, static_cast<std::size_t>(got), g_file) != static_cast<std::size_t>(got)) return drop_client();
        g_left -= static_cast<std::uint64_t>(got);
    } else if (want && (got == 0 || !would_block())) {
        return drop_client();
    }
    if (!g_left) finish_put();
}

void read_commands(std::uint64_t frame) {
    const ssize_t got = recv(g_client, g_line + g_line_used, sizeof g_line - 1 - g_line_used, 0);
    if (got == 0 || (got < 0 && !would_block())) return drop_client();
    if (got > 0) g_line_used += static_cast<std::size_t>(got);
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
        if (g_is_program && (g_line_used < 4 || std::memcmp(g_line, "3DSX", 4))) {
            say("ERR not a 3DSX program, nothing changed\n");
            return drop_client();
        }
        g_first_block = false;
        add_to_crc(g_line, g_line_used);
        std::fwrite(g_line, 1, g_line_used, g_file);
        g_left -= g_line_used;
        g_line_used = 0;
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
        if (g_client >= 0) fcntl(g_client, F_SETFL, fcntl(g_client, F_GETFL, 0) | O_NONBLOCK);
        return;
    }
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
