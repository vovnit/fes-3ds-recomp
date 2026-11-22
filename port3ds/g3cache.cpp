// The converted-vertex cache (see g3cache.hpp).
//
// Key: where each vertex stream the batch reads lies in PS2 RAM, its unpack format, the vertex count and how the batch
// reads them (positions' and texture coordinates' widths, normals' step, lights, program). A check build that hashed the
// converted vertices of every batch found the same key always gave the same vertices in the lobby, lockers and first
// battle (no change at all in 60 million vertices), so a kept entry is trusted, with a few sampled words of its positions
// and texture coordinates compared on every use as a guard against a model loaded where another one was.
//
// Arena: entries take consecutive vertices from a pointer that goes round. Space is taken back from entries not drawn in
// the current or the previous picture (the GPU may still be drawing the previous one); a live entry in the way is stepped
// over, and after a few steps the batch is drawn the usual way.
#include "g3cache.hpp"

#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace g3cache {

int g_mode = 1;
int g_skin = 1;
int g_index = 0;

namespace {

using u32 = std::uint32_t;

constexpr u32 kSets = 4096;        // two entries each
constexpr u32 kEntries = kSets * 2;

struct Entry {
    std::uint64_t key;
    u32 start;      // first vertex in the arena
    u32 picture;    // last picture it was drawn in
    u32 sig;        // sampled words of its streams
    std::uint16_t n;
    bool used;
    bool alpha_uniform;
    std::uint8_t alpha_val;
    std::int8_t bone_count;                // skin: how many of bones[] its vertices use
    g3::BoneUse bones[g3::kMaxBoneUses];   // skin: each bone's VU address and the palette slot its vertices name
};

struct Counts {
    std::uint64_t batches, verts, no_key, hit, fresh, no_room, stale, evicted, skin_hit, bones_split;
};

g3::GpuVertex *g_arena = nullptr;
u32 g_capacity = 0;
Entry *g_entries = nullptr;
std::uint16_t *g_cover = nullptr;   // per arena vertex: the entry (index + 1) that last took it, 0 for none
u32 g_next = 0;                     // where the next entry goes
u32 g_picture = 1;
u32 g_dirty_lo = UINT32_MAX, g_dirty_hi = 0;
Counts g_c{};
std::uint64_t g_next_report = 0;

inline bool in_ram(const std::uint8_t *p, u32 bytes) {
    return p >= ps2::g_ram && p + bytes <= ps2::g_ram + ps2::kRamSize;
}

inline u32 stream_bytes(const g3::DirectStream &s, u32 n) { return n * (s.vn + 1u) * (4u >> s.vl); }

// Positions at the start, middle and end, and the first texture coordinate.
u32 sample(const g3::DirectStreams &ds, u32 n) {
    const u32 pos = stream_bytes(ds.s[0], n) & ~3u;
    const std::uint8_t *p = ds.s[0].src;
    u32 a, b, c, d;
    std::memcpy(&a, p, 4);
    std::memcpy(&b, p + ((pos / 2u) & ~3u), 4);
    std::memcpy(&c, p + pos - 4u, 4);
    std::memcpy(&d, ds.s[1].src, 4);
    return ((((0x811C9DC5u ^ a) * 0x01000193u ^ b) * 0x01000193u ^ c) * 0x01000193u ^ d) * 0x01000193u;
}

inline bool live(const Entry &e) { return e.used && e.picture + 1u >= g_picture; }

void drop(Entry &e) {
    e.used = false;
    ++g_c.evicted;
}

// n consecutive vertices, or UINT32_MAX. Entries in the way that are not live are dropped; a live one is stepped over.
u32 take(u32 n, u32 owner) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (g_next + n > g_capacity) g_next = 0;
        const u32 at = g_next;
        u32 blocked_end = 0;
        for (u32 v = at; v < at + n; ++v) {
            const u32 o = g_cover[v];
            if (!o) continue;
            Entry &e = g_entries[o - 1u];
            if (!e.used || v < e.start || v >= e.start + e.n) {   // taken by an entry since gone or moved
                g_cover[v] = 0;
                continue;
            }
            if (live(e)) {
                blocked_end = e.start + e.n;
                break;
            }
            drop(e);
        }
        if (blocked_end) {
            g_next = blocked_end;
            continue;
        }
        for (u32 v = at; v < at + n; ++v) g_cover[v] = static_cast<std::uint16_t>(owner + 1u);
        g_next = at + n;
        return at;
    }
    return UINT32_MAX;
}

#define U(x) static_cast<unsigned long long>(x)
void report(std::uint64_t frame) {
    std::fprintf(stderr, "[g3cache] f%llu: %llu batches %llu verts: no key %llu, kept %llu (skin %llu), converted %llu, no room %llu, stale %llu, "
                 "bones split %llu; entries dropped %llu\n",
                 U(frame), U(g_c.batches), U(g_c.verts), U(g_c.no_key), U(g_c.hit), U(g_c.skin_hit), U(g_c.fresh), U(g_c.no_room), U(g_c.stale),
                 U(g_c.bones_split), U(g_c.evicted));
    g_c = Counts{};
}
#undef U

}   // namespace

void init(g3::GpuVertex *arena, u32 capacity) {
    g_entries = static_cast<Entry *>(std::calloc(kEntries, sizeof(Entry)));
    g_cover = static_cast<std::uint16_t *>(std::calloc(capacity, sizeof(std::uint16_t)));
    if (!arena || !g_entries || !g_cover || capacity > 65536u) {
        std::free(g_entries);
        std::free(g_cover);
        g_entries = nullptr;
        g_cover = nullptr;
        std::fprintf(stderr, "[g3cache] off: no memory\n");
        return;
    }
    g_arena = arena;
    g_capacity = capacity;
}

bool ready() { return g_arena != nullptr; }

void begin_picture() { ++g_picture; }

int place(const g3::Input &in, const g3::BatchPlan &plan, g3::Gpu &g, int &fill_rc) {
    fill_rc = 0;
    if (!g_mode || !g_arena) return -1;
    if (g_mode == 2) {
        const std::uint64_t frame = ps2::frame_count();
        if (frame >= g_next_report) {
            if (g_next_report) report(frame);
            g_next_report = frame + 600u;
        }
    }
    const u32 n = g.n;
    ++g_c.batches;
    g_c.verts += n;
    // Strips with all their streams in PS2 RAM (a skin batch's weights too, not left in VU memory).
    if (plan.dual || plan.list || !plan.direct || !in.streams || n > 256u || (plan.skin && (!g_skin || !(in.streams->mask & 16u)))) {
        g_c.no_key += n;
        return -1;
    }
    const g3::DirectStreams &ds = *in.streams;
    const u32 used = 7u | (plan.lights ? 8u : 0u) | (plan.skin ? 16u : 0u);
    std::uint64_t key = 0xCBF29CE484222325ull;
    for (int i = 0; i < 5; ++i) {
        if (!(used & (1u << i))) continue;
        const g3::DirectStream &s = ds.s[i];
        if (!in_ram(s.src, stream_bytes(s, n))) {
            g_c.no_key += n;
            return -1;
        }
        key = (key ^ (static_cast<std::uint64_t>(s.src - ps2::g_ram) | (static_cast<std::uint64_t>(s.vn) << 32) |
                      (static_cast<std::uint64_t>(s.vl) << 40) | (static_cast<std::uint64_t>(s.usn) << 48))) * 0x100000001B3ull;
    }
    key = (key ^ (static_cast<std::uint64_t>(n) | (static_cast<std::uint64_t>(plan.pos_w) << 16) | (static_cast<std::uint64_t>(plan.uv_w) << 24) |
                  (static_cast<std::uint64_t>(plan.nrm_step) << 32) | (static_cast<std::uint64_t>(plan.lights) << 40) |
                  (static_cast<std::uint64_t>(plan.skin) << 41) | (static_cast<std::uint64_t>(in.program) << 48))) * 0x100000001B3ull;
    const u32 sig = sample(ds, n);

    const u32 set = static_cast<u32>((key ^ (key >> 23) ^ (key >> 41)) & (kSets - 1u)) * 2u;
    Entry *way[2] = {&g_entries[set], &g_entries[set + 1u]};
    for (Entry *e : way) {
        if (!e->used || e->key != key || e->n != n) continue;
        if (e->sig != sig) {   // the data changed under the key: convert again
            ++g_c.stale;
            drop(*e);
            break;
        }
        if (plan.skin) {
            // Its vertices name slots: the bones go back into them. If two that shared a slot now differ, this time it is
            // drawn the usual way (the entry may still be in the GPU's hands, so it stays as it is).
            if (!g3::restore_bones(in.mem, e->bones, e->bone_count, g)) {
                g_c.bones_split += n;
                return -1;
            }
            g_c.skin_hit += n;
        } else {
            g.slots = 0;
        }
        e->picture = g_picture;
        g.alpha_uniform = e->alpha_uniform;
        g.alpha_val = e->alpha_val;
        g_c.hit += n;
        return static_cast<int>(e->start);
    }
    // A new entry: the free way, else the one drawn longest ago (not a live one).
    Entry *e = !way[0]->used ? way[0] : !way[1]->used ? way[1] : way[0]->picture <= way[1]->picture ? way[0] : way[1];
    if (e->used && live(*e)) {
        g_c.no_room += n;
        return -1;
    }
    const u32 index = static_cast<u32>(e - g_entries);
    if (e->used) drop(*e);
    const u32 at = take(n, index);
    if (at == UINT32_MAX) {
        g_c.no_room += n;
        return -1;
    }
    fill_rc = g3::gpu_batch_fill(in, g, plan, g_arena + at);
    if (fill_rc != 0) {
        for (u32 v = at; v < at + n; ++v) g_cover[v] = 0;
        return -1;
    }
    *e = Entry{key, at, g_picture, sig, static_cast<std::uint16_t>(n), true, g.alpha_uniform, g.alpha_val, 0, {}};
    if (plan.skin) {
        const g3::BoneUse *uses;
        const int count = g3::last_bones(uses);
        if (count < 0) e->key = 0;   // too many bones to put back: drawn from here this time, never found again
        else {
            e->bone_count = static_cast<std::int8_t>(count);
            std::memcpy(e->bones, uses, static_cast<std::size_t>(count) * sizeof(g3::BoneUse));
        }
    }
    if (at < g_dirty_lo) g_dirty_lo = at;
    if (at + n > g_dirty_hi) g_dirty_hi = at + n;
    g_c.fresh += n;
    return static_cast<int>(at);
}

void take_dirty(u32 &lo, u32 &hi) {
    if (g_dirty_lo >= g_dirty_hi) {
        lo = hi = 0;
        return;
    }
    lo = g_dirty_lo;
    hi = g_dirty_hi;
    g_dirty_lo = UINT32_MAX;
    g_dirty_hi = 0;
}

}   // namespace g3cache
