// Plays the 400x240 copies of the game's movies. Three parts, on three threads:
//   disc_read (whichever thread reads the disc) swaps the movie's video sectors for the cheap black movie with markers;
//   watch (the game's thread, inside the IPU) reads the marker of each picture the game is about to show;
//   a decoder thread keeps the wanted picture of the .p3v file ready in a texture, and picture() hands it to the drawing thread.
#include "movie3ds.hpp"

#include "mpeg1.hpp"

#include <3ds.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace movie {

namespace {

constexpr std::uint32_t kSector = 2048;
constexpr int kMovies = 64;
constexpr int kMarkBits = 32;
constexpr std::uint32_t kMagic = 0xB2;
constexpr std::uint32_t kNothing = 0xFFFFFFFFu;
constexpr int kWidth = 400, kHeight = 240, kTexWidth = 512, kTexHeight = 256;

struct Cut {
    std::uint32_t first, count, at, entries, pictures;
    std::uint16_t width, height;
    char name[20];
    std::uint32_t number;
};
static_assert(sizeof(Cut) == 48);

struct Entry {
    std::uint32_t sector, blob;
    std::uint16_t length, flags;
    std::uint32_t pts;
};
static_assert(sizeof(Entry) == 16);

Cut g_cuts[kMovies];
bool g_has_copy[kMovies];
bool g_marker_row_only = false;
int g_cut_count = 0;
std::FILE *g_cut_file = nullptr;
int g_loaded = -1;            // whose entries are in g_entries
Entry *g_entries = nullptr;

// What the game last asked to show: movie number in the top half, picture in the bottom half.
std::atomic<std::uint32_t> g_want{kNothing};
std::atomic<u64> g_seen{0};

struct Slot {
    C3D_Tex tex;
    std::uint32_t movie = kNothing;
    int index = -1;
    bool shown = false, before = false;   // on show now, and on show before that: the GPU may still be reading either
    bool busy = false;                    // the decoder is writing it
};
Slot g_slots[4];
std::atomic<bool> g_textures{false};
LightLock g_lock;
Thread g_thread = nullptr;
std::atomic<bool> g_run{false};

bool playing_now() {
    const u64 seen = g_seen.load();
    return seen && svcGetSystemTick() - seen < static_cast<u64>(SYSCLOCK_ARM11) / 4;   // a picture comes every 33 ms while one plays
}

void put_pts(std::uint8_t *to, std::uint32_t t) {
    to[0] = static_cast<std::uint8_t>(0x21 | ((t >> 30) & 3u) << 1);
    to[1] = static_cast<std::uint8_t>(t >> 22);
    to[2] = static_cast<std::uint8_t>(((t >> 15) & 0x7Fu) << 1 | 1u);
    to[3] = static_cast<std::uint8_t>(t >> 7);
    to[4] = static_cast<std::uint8_t>((t & 0x7Fu) << 1 | 1u);
}

bool load_entries(int m) {
    if (g_loaded == m) return true;
    std::free(g_entries);
    g_loaded = -1;
    const std::size_t bytes = static_cast<std::size_t>(g_cuts[m].entries) * sizeof(Entry);
    g_entries = static_cast<Entry *>(std::malloc(bytes ? bytes : 1));
    if (!g_entries) return false;
    if (std::fseek(g_cut_file, static_cast<long>(g_cuts[m].at), SEEK_SET) || std::fread(g_entries, 1, bytes, g_cut_file) != bytes) return false;
    g_loaded = m;
    return true;
}

// The same change as Cut.sector in tools/p3fes_3ds_movies.py.
void change_sector(int m, std::uint32_t rel, std::uint8_t *sec) {
    if (sec[12] != 0 || sec[13] != 0 || sec[14] != 1 || sec[15] != 0xE0) return;
    const std::uint32_t packet = (static_cast<std::uint32_t>(sec[16]) << 8) | sec[17];
    if (packet < 12 || 18 + packet > kSector || !load_entries(m)) return;
    const std::uint32_t room = packet - 12;
    std::uint32_t lo = 0, hi = g_cuts[m].entries;
    while (lo < hi) {
        const std::uint32_t mid = (lo + hi) / 2;
        if (g_entries[mid].sector < rel) lo = mid + 1;
        else hi = mid;
    }
    if (lo == g_cuts[m].entries || g_entries[lo].sector != rel || g_entries[lo].length > room) {
        sec[15] = 0xBE;   // nothing for the game here: a padding packet, which its reader drops
        return;
    }
    const Entry &e = g_entries[lo];
    if (std::fseek(g_cut_file, static_cast<long>(e.blob), SEEK_SET) || std::fread(sec + 30, 1, e.length, g_cut_file) != e.length) {
        sec[15] = 0xBE;
        return;
    }
    sec[16] = static_cast<std::uint8_t>((12u + e.length) >> 8);
    sec[17] = static_cast<std::uint8_t>(12u + e.length);
    if (e.flags & 1u) {
        std::memset(sec + 18, 0xFF, 5);
        sec[23] = 0x60;
        sec[24] = 0x2E;
        put_pts(sec + 25, e.pts);
    } else {
        std::memset(sec + 18, 0xFF, 9);
        sec[27] = 0x60;
        sec[28] = 0x2E;
        sec[29] = 0x0F;
    }
    const std::uint32_t rest = room - e.length;
    std::uint8_t *after = sec + 30 + e.length;
    if (rest >= 7) {
        after[0] = after[1] = 0;
        after[2] = 1;
        after[3] = 0xBE;
        after[4] = static_cast<std::uint8_t>((rest - 6) >> 8);
        after[5] = static_cast<std::uint8_t>(rest - 6);
        after[6] = 0x0F;
        std::memset(after + 7, 0xFF, rest - 7);
    } else {
        std::memset(after, 0, rest);
    }
}

// YUV 4:2:0 to the GPU's 16-bit texture layout: 8x8 tiles, each in Z order.
struct Colour {
    int luma[256], red[256], green_u[256], green_v[256], blue[256];
};
Colour *g_colour = nullptr;

inline std::uint16_t pack(int y, int r, int g, int b) {
    r = (y + r) >> 8;
    g = (y + g) >> 8;
    b = (y + b) >> 8;
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return static_cast<std::uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

void convert(const mpeg1::Decoder &d, C3D_Tex &tex) {
    const Colour &c = *g_colour;
    auto *out = static_cast<std::uint16_t *>(tex.data);
    const std::uint8_t *luma = d.luma(), *blue = d.blue(), *red = d.red();
    const int w = d.mb_width * 16, cw = w / 2;
    static const std::uint8_t kQuad[16][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}, {2, 0}, {3, 0}, {2, 1}, {3, 1},
                                              {0, 2}, {1, 2}, {0, 3}, {1, 3}, {2, 2}, {3, 2}, {2, 3}, {3, 3}};
    for (int ty = 0; ty < kHeight; ty += 8)
        for (int tx = 0; tx < kWidth; tx += 8) {
            std::uint16_t *tile = out + (ty * kTexWidth + tx * 8);
            for (int q = 0; q < 16; ++q) {
                const int qx = kQuad[q][0], qy = kQuad[q][1];
                const std::size_t ci = static_cast<std::size_t>((ty / 2 + qy) * cw + tx / 2 + qx);
                const int u = blue[ci], v = red[ci];
                const int r = c.red[v], g = c.green_u[u] + c.green_v[v], b = c.blue[u];
                const std::uint8_t *y0 = luma + (ty + qy * 2) * w + tx + qx * 2;
                tile[q * 4 + 0] = pack(c.luma[y0[0]], r, g, b);
                tile[q * 4 + 1] = pack(c.luma[y0[1]], r, g, b);
                tile[q * 4 + 2] = pack(c.luma[y0[w]], r, g, b);
                tile[q * 4 + 3] = pack(c.luma[y0[w + 1]], r, g, b);
            }
        }
    GSPGPU_FlushDataCache(tex.data, kTexWidth * kHeight * 2);
}

struct Reader {
    std::FILE *file = nullptr;
    std::uint32_t movie = kNothing;
    std::uint32_t count = 0, step = 1, key = 15;
    std::uint32_t *offsets = nullptr;
    std::uint8_t *unit = nullptr;
    std::size_t unit_room = 0;
    long position = -1;
    char buffer[16384];

    void close() {
        if (file) std::fclose(file);
        std::free(offsets);
        file = nullptr;
        offsets = nullptr;
        movie = kNothing;
        position = -1;
    }

    bool open(std::uint32_t number) {
        close();
        char name[48];
        std::snprintf(name, sizeof name, "sdmc:/p3fes/movies/%02lu.p3v", static_cast<unsigned long>(number));
        file = std::fopen(name, "rb");
        if (!file) return false;
        std::setvbuf(file, buffer, _IOFBF, sizeof buffer);
        std::uint8_t head[16];
        if (std::fread(head, 1, 16, file) != 16 || std::memcmp(head, "P3V1", 4)) return close(), false;
        std::uint16_t vblanks, spacing;
        std::memcpy(&vblanks, head + 8, 2);
        std::memcpy(&spacing, head + 10, 2);
        std::memcpy(&count, head + 12, 4);
        step = vblanks >= 4 ? 2 : 1;
        key = spacing ? spacing : 1;
        if (!count || count > 100000) return close(), false;
        offsets = static_cast<std::uint32_t *>(std::malloc((count + 1) * 4));
        if (!offsets || std::fread(offsets, 4, count + 1, file) != count + 1) return close(), false;
        movie = number;
        return true;
    }

    const std::uint8_t *read(std::uint32_t n, std::size_t &size) {
        size = offsets[n + 1] - offsets[n];
        if (size > (1u << 20)) return nullptr;
        if (size > unit_room) {
            std::free(unit);
            unit_room = size + 4096;
            unit = static_cast<std::uint8_t *>(std::malloc(unit_room));
            if (!unit) return unit_room = 0, nullptr;
        }
        if (position != static_cast<long>(offsets[n]) && std::fseek(file, static_cast<long>(offsets[n]), SEEK_SET)) return nullptr;
        if (std::fread(unit, 1, size, file) != size) return position = -1, nullptr;
        position = static_cast<long>(offsets[n + 1]);
        return unit;
    }
};

void publish(const mpeg1::Decoder &d, std::uint32_t movie, int index) {
    // Into a slot the drawing side is done with: an empty one, or else the oldest picture.
    LightLock_Lock(&g_lock);
    Slot *to = nullptr;
    if (g_textures)
        for (Slot &s : g_slots) {
            if (s.shown || s.before) continue;
            if (!to || (to->movie == movie && (s.movie != movie || s.index < to->index))) to = &s;
        }
    if (to) {
        to->movie = kNothing;
        to->busy = true;
    }
    LightLock_Unlock(&g_lock);
    if (!to) return;   // the drawing thread has not made the textures yet
    convert(d, to->tex);
    LightLock_Lock(&g_lock);
    to->movie = movie;
    to->index = index;
    to->busy = false;
    LightLock_Unlock(&g_lock);
}

void decoder_main(void *) {
    static Reader reader;
    static mpeg1::Decoder decoder;
    int done = -1;   // the last picture decoded
    u64 idle_since = 0;
    while (g_run.load()) {
        const std::uint32_t want = g_want.load();
        if (want == kNothing || !playing_now()) {
            if (reader.file) {
                if (!idle_since) idle_since = svcGetSystemTick();
                else if (svcGetSystemTick() - idle_since > static_cast<u64>(SYSCLOCK_ARM11) * 2) reader.close();
            }
            svcSleepThread(5000000);
            continue;
        }
        idle_since = 0;
        const std::uint32_t movie = want >> 16;
        if (reader.movie != movie) {
            if (!reader.open(movie)) {
                svcSleepThread(100000000);
                continue;
            }
            decoder.forget();
            done = -1;
        }
        const int target = static_cast<int>(std::min<std::uint32_t>((want & 0xFFFFu) / reader.step, reader.count - 1));
        int next = done + 1;
        // Out of step with the game (a start in the middle, a jump, or decoding fell behind): from the key picture before.
        if (done < 0 || target < done - 2 || target >= done + static_cast<int>(reader.key)) {
            next = target - target % static_cast<int>(reader.key);
            decoder.forget();
        }
        if (next > target + 1 || next >= static_cast<int>(reader.count)) {
            svcSleepThread(2000000);   // one ahead already
            continue;
        }
        std::size_t size = 0;
        const std::uint8_t *unit = reader.read(static_cast<std::uint32_t>(next), size);
        if (!unit || !decoder.decode(unit, size) || decoder.width != kWidth || decoder.height != kHeight) {
            std::fprintf(stderr, "[movie] picture %d of movie %lu does not decode\n", next, static_cast<unsigned long>(movie));
            decoder.forget();
            done = -1;
            svcSleepThread(200000000);
            continue;
        }
        done = next;
        // While catching up, only the last one is worth showing. Slots go by the game's picture numbers.
        if (done >= target) {
            while (g_run.load() && !g_textures && playing_now()) svcSleepThread(2000000);
            publish(decoder, movie, done * static_cast<int>(reader.step));
        }
    }
    reader.close();
}

}

bool start(int priority, int core) {
    g_cut_file = std::fopen("sdmc:/p3fes/movies/movies.cut", "rb");
    if (!g_cut_file) return false;
    static char buffer[8192];
    std::setvbuf(g_cut_file, buffer, _IOFBF, sizeof buffer);
    char magic[8];
    std::uint32_t count = 0;
    if (std::fread(magic, 1, 8, g_cut_file) != 8 || (std::memcmp(magic, "P3CUT1\0\0", 8) && std::memcmp(magic, "P3CUT2\0\0", 8)) || std::fread(&count, 4, 1, g_cut_file) != 1 || count > kMovies ||
        std::fread(g_cuts, sizeof(Cut), count, g_cut_file) != count) {
        std::fprintf(stderr, "[movie] movies.cut is not usable\n");
        std::fclose(g_cut_file);
        g_cut_file = nullptr;
        return false;
    }
    g_marker_row_only = magic[5] == '2';
    g_cut_count = static_cast<int>(count);
    int copies = 0;
    for (int m = 0; m < g_cut_count; ++m) {
        char name[48];
        std::snprintf(name, sizeof name, "sdmc:/p3fes/movies/%02lu.p3v", static_cast<unsigned long>(g_cuts[m].number));
        if (std::FILE *f = std::fopen(name, "rb")) {
            std::fclose(f);
            g_has_copy[m] = true;
            ++copies;
        }
    }
    std::fprintf(stderr, "[movie] %d movies in movies.cut, %d with a 3DS copy\n", g_cut_count, copies);
    if (!copies) return false;
    g_colour = static_cast<Colour *>(std::malloc(sizeof(Colour)));
    if (!g_colour) return false;
    for (int i = 0; i < 256; ++i) {
        g_colour->luma[i] = 298 * (i - 16) + 128;
        g_colour->red[i] = 409 * (i - 128);
        g_colour->green_u[i] = -100 * (i - 128);
        g_colour->green_v[i] = -208 * (i - 128);
        g_colour->blue[i] = 516 * (i - 128);
    }
    LightLock_Init(&g_lock);
    g_run.store(true);
    g_thread = threadCreate(decoder_main, nullptr, 32u << 10, priority, core, false);
    if (!g_thread) {
        std::fprintf(stderr, "[movie] no decoder thread\n");
        g_run.store(false);
        return false;
    }
    return true;
}

void stop() {
    if (!g_thread) return;
    g_run.store(false);
    threadJoin(g_thread, U64_MAX);
    threadFree(g_thread);
    g_thread = nullptr;
}

void disc_read(std::uint64_t offset, std::uint8_t *data, std::size_t size) {
    if (!g_run.load() || (offset % kSector) || size < kSector) return;
    static int last = 0;
    std::uint32_t sector = static_cast<std::uint32_t>(offset / kSector);
    for (std::size_t done = 0; done + kSector <= size; done += kSector, ++sector) {
        if (sector < g_cuts[last].first || sector - g_cuts[last].first >= g_cuts[last].count) {
            int found = -1;
            for (int m = 0; m < g_cut_count; ++m)
                if (sector >= g_cuts[m].first && sector - g_cuts[m].first < g_cuts[m].count) found = m;
            if (found < 0) return;   // a read lies in one file, so the rest is not a movie either
            last = found;
        }
        if (g_has_copy[last]) change_sector(last, sector - g_cuts[last].first, data + done);
    }
}

// The game converts a picture column by column, top to bottom, in commands of at most 1023 macroblocks: a 640x448 picture
// takes two. So marker block k, in the top row, is macroblock k times the picture's height in macroblocks.
bool watch(const std::uint8_t *raw, unsigned index, unsigned count) {
    constexpr unsigned kMost = 1023, kKept = kMarkBits * 32;
    static std::uint8_t white[kKept / 8];
    static unsigned base = 0, carry = 0;
    static bool ours = false, last_ours = false;
    if (!g_run.load()) return false;
    if (index == 0) {
        base = carry;   // the second command of a picture goes on where the first stopped
        carry = 0;
        if (!base) {
            last_ours = ours;
            ours = false;
            std::memset(white, 0, sizeof white);
        }
    }
    if (index + 1 == count && count == kMost && !base) carry = kMost;
    const unsigned at = base + index;
    if (at < kKept) {
        if (raw[8 * 16 + 8] > 128) white[at >> 3] |= static_cast<std::uint8_t>(1u << (at & 7u));
        // New timing streams carry just the marker row. Older ones also carry the other 22 or 27 rows.
        for (const unsigned rows : {1u, 23u, 28u}) {
            if ((rows == 1) != g_marker_row_only) continue;
            if (rows == 1 && (base || (count != 32u && count != 40u))) continue;  // 512 or 640 pixels wide
            if (at != (kMarkBits - 1) * rows) continue;
            std::uint32_t bits = 0;
            for (unsigned k = 0; k < kMarkBits; ++k) bits = (bits << 1) | ((white[(k * rows) >> 3] >> ((k * rows) & 7u)) & 1u);
            unsigned even = 0, odd = 0;
            for (int i = 0; i < 30; i += 2) {
                even += (bits >> (31 - i)) & 1u;
                odd += (bits >> (30 - i)) & 1u;
            }
            const std::uint32_t value = bits >> 2;
            if ((even & 1u) == ((bits >> 1) & 1u) && (odd & 1u) == (bits & 1u) && (value >> 22) == kMagic) {
                g_want.store(value & 0x3FFFFFu);   // six bits of movie number, sixteen of picture
                g_seen.store(svcGetSystemTick());
                ours = true;
            }
        }
    }
    // By the picture before, since the marker is complete only near the end of this one. So a movie's pictures all come out
    // black, the same every time, and the drawing side has no new texture to make.
    return last_ours;
}

namespace {

bool make_textures() {
    int made = 0;
    for (Slot &s : g_slots) {
        if (!C3D_TexInit(&s.tex, kTexWidth, kTexHeight, GPU_RGB565)) break;
        std::memset(s.tex.data, 0, kTexWidth * kTexHeight * 2);
        C3D_TexSetFilter(&s.tex, GPU_NEAREST, GPU_NEAREST);
        C3D_TexSetWrap(&s.tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
        s.movie = kNothing;
        s.shown = s.before = s.busy = false;
        ++made;
    }
    if (made == static_cast<int>(std::size(g_slots))) return true;
    for (int i = 0; i < made; ++i) C3D_TexDelete(&g_slots[i].tex);
    return false;
}

}

bool overlap_needs_wait() {
    return g_textures.load() || (g_run.load() && playing_now());
}

Picture picture(bool (*free_a_texture)()) {
    static u64 last_played = 0;
    if (!g_run.load()) return {false, nullptr};
    if (!playing_now()) {
        // Give the memory back once the movie has been over for two seconds: nothing reads the textures by then.
        if (g_textures && svcGetSystemTick() - last_played > static_cast<u64>(SYSCLOCK_ARM11) * 2) {
            LightLock_Lock(&g_lock);
            bool busy = false;
            for (const Slot &s : g_slots) busy = busy || s.busy;
            if (!busy) g_textures.store(false);
            LightLock_Unlock(&g_lock);
            if (!busy)
                for (Slot &s : g_slots) C3D_TexDelete(&s.tex);
        }
        return {false, nullptr};
    }
    last_played = svcGetSystemTick();
    if (!g_textures) {
        // The lobby fills linear memory with its textures. They are made again when next drawn.
        bool made = make_textures();
        while (!made && free_a_texture()) made = make_textures();
        if (!made) return {true, nullptr};
        g_textures.store(true);
    }
    const std::uint32_t want = g_want.load();
    const std::uint32_t movie = want >> 16;
    const int index = static_cast<int>(want & 0xFFFFu);
    LightLock_Lock(&g_lock);
    Slot *best = nullptr;
    for (Slot &s : g_slots) {
        if (s.movie != movie) continue;
        // The newest one that is not ahead of the game; failing that, the one least ahead.
        if (!best) best = &s;
        else if (s.index <= index ? (best->index > index || s.index > best->index) : (best->index > index && s.index < best->index)) best = &s;
    }
    if (best && !best->shown)
        for (Slot &s : g_slots) {
            s.before = s.shown;
            s.shown = &s == best;
        }
    LightLock_Unlock(&g_lock);
    return {true, best ? &best->tex : nullptr};
}

}
