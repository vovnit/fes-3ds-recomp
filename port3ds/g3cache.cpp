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
//
// Around an entry's n vertices lie copies of its first vertex (one, before) and of its last (one after, two if n is odd), so
// an even number in all. Two entries that follow each other in the arena are then one strip: the triangles between have no
// area, and the second starts at an even place. Entries made in drawing order usually do follow each other.
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

constexpr u32 kSets = 2048;        // two entries each
constexpr u32 kEntries = kSets * 2;

struct Entry {
    std::uint64_t key;
    u32 start;      // first vertex in the arena
    u32 picture;    // last picture it was drawn in
    u32 sig;        // sampled words of its streams
    std::uint16_t n;
    bool used;
    std::uint8_t alpha_min, alpha_max;
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
    const std::uintptr_t off = static_cast<std::uintptr_t>(p - ps2::g_ram);
    return off <= ps2::kRamSize && bytes <= ps2::kRamSize - off;
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

inline u32 span_of(u32 n) { return n + 2u + (n & 1u); }   // with the copies before and after

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
            if (!e.used || v + 1u < e.start || v + 1u >= e.start + span_of(e.n)) {   // taken by an entry since gone or moved
                g_cover[v] = 0;
                continue;
            }
            if (live(e)) {
                blocked_end = e.start - 1u + span_of(e.n);
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
    const bool stats = g_mode == 2;
    if (stats) {
        ++g_c.batches;
        g_c.verts += n;
    }
    // Strips with all their streams in PS2 RAM (a skin batch's weights too, not left in VU memory).
    if (plan.dual || plan.list || plan.uv_xform || !plan.direct || !in.streams || n > 256u || (plan.skin && (!g_skin || !(in.streams->mask & 16u)))) {
        if (stats) g_c.no_key += n;
        return -1;
    }
    const g3::DirectStreams &ds = *in.streams;
    u32 h0 = 0x811C9DC5u, h1 = 0x9E3779B9u;
    auto hash_stream = [&](const g3::DirectStream &s, u32 bytes, u32 idx) -> bool {
        if (!in_ram(s.src, bytes)) return false;
        const u32 off = static_cast<u32>(s.src - ps2::g_ram);
        const u32 fmt = static_cast<u32>(s.vn) | (static_cast<u32>(s.vl) << 8) | (static_cast<u32>(s.usn) << 16) | (idx << 24);
        h0 = (h0 ^ off) * 0x01000193u;
        h1 = (h1 ^ fmt) * 0x9E3779B1u ^ (off >> 13);
        return true;
    };
    if (!hash_stream(ds.s[0], n * (plan.pos_w << 2), 0u) ||
        !hash_stream(ds.s[1], n * (plan.uv_w << 2), 1u) ||
        !hash_stream(ds.s[2], n << 2, 2u) ||
        (plan.lights && !hash_stream(ds.s[3], n * plan.nrm_step, 3u)) ||
        (plan.skin && !hash_stream(ds.s[4], n << 4, 4u))) {
        if (stats) g_c.no_key += n;
        return -1;
    }
    const u32 meta0 = n | (plan.pos_w << 12) | (plan.uv_w << 16) | (plan.nrm_step << 20) | (static_cast<u32>(plan.lights) << 24) | (static_cast<u32>(plan.skin) << 25);
    const u32 meta1 = static_cast<u32>(in.program);
    h0 = (h0 ^ meta0) * 0x01000193u;
    h1 = (h1 ^ meta1) * 0x9E3779B1u;
    const std::uint64_t key = (static_cast<std::uint64_t>(h1) << 32) | h0;
    const u32 sig = sample(ds, n);

    const u32 set = ((h0 ^ (h0 >> 12) ^ h1 ^ (h1 >> 16)) & (kSets - 1u)) * 2u;
    Entry *way[2] = {&g_entries[set], &g_entries[set + 1u]};
    for (Entry *e : way) {
        if (!e->used || e->key != key || e->n != n) continue;
        if (e->sig != sig) {   // the data changed under the key: convert again
            if (stats) ++g_c.stale;
            drop(*e);
            break;
        }
        if (plan.skin) {
            // Its vertices name slots: the bones go back into them. If two that shared a slot now differ, this time it is
            // drawn the usual way (the entry may still be in the GPU's hands, so it stays as it is).
            if (!g3::restore_bones(in.mem, e->bones, e->bone_count, g)) {
                if (stats) g_c.bones_split += n;
                return -1;
            }
            if (stats) g_c.skin_hit += n;
        } else {
            g.slots = 0;
        }
        e->picture = g_picture;
        g.alpha_min = e->alpha_min;
        g.alpha_max = e->alpha_max;
        if (stats) g_c.hit += n;
        return static_cast<int>(e->start);
    }
    // A new entry: the free way, else the one drawn longest ago (not a live one).
    Entry *e = !way[0]->used ? way[0] : !way[1]->used ? way[1] : way[0]->picture <= way[1]->picture ? way[0] : way[1];
    if (e->used && live(*e)) {
        if (stats) g_c.no_room += n;
        return -1;
    }
    const u32 index = static_cast<u32>(e - g_entries);
    if (e->used) drop(*e);
    const u32 span = span_of(n), room = take(span, index);
    if (room == UINT32_MAX) {
        if (stats) g_c.no_room += n;
        return -1;
    }
    const u32 at = room + 1u;
    fill_rc = g3::gpu_batch_fill(in, g, plan, g_arena + at);
    if (fill_rc != 0) {
        for (u32 v = room; v < room + span; ++v) g_cover[v] = 0;
        return -1;
    }
    g_arena[room] = g_arena[at];
    for (u32 v = at + n; v < room + span; ++v) g_arena[v] = g_arena[at + n - 1u];
    *e = Entry{key, at, g_picture, sig, static_cast<std::uint16_t>(n), true, g.alpha_min, g.alpha_max, 0, {}};
    if (plan.skin) {
        const g3::BoneUse *uses;
        const int count = g3::last_bones(uses);
        if (count < 0) e->key = 0;   // too many bones to put back: drawn from here this time, never found again
        else {
            e->bone_count = static_cast<std::int8_t>(count);
            std::memcpy(e->bones, uses, static_cast<std::size_t>(count) * sizeof(g3::BoneUse));
        }
    }
    if (room < g_dirty_lo) g_dirty_lo = room;
    if (room + span > g_dirty_hi) g_dirty_hi = room + span;
    if (stats) g_c.fresh += n;
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
