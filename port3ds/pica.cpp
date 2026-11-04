// PS2 GS shapes on the New 3DS GPU (PICA200), through citro3d.
//
// The PS2 draws one shape at a time and the GPU is happiest with long runs of triangles, so shapes collect in a batch until a
// GS setting changes. Each PS2 frame buffer gets a render target, keyed by its address, so page flipping and simple
// render-to-texture work. The PICA has no palettes, so textures are decoded from GS memory into a cache.
//
// Differences from the real GS that are accepted for now: no destination alpha test, no dithering, no mipmaps, and reads
// of the frame buffer by the game see old GS memory. Wrap modes with a region clamp to the edge of the whole texture.
#include "pica.hpp"
#include "prof.hpp"

#include "gs/gs.hpp"
#include "gs/queue.hpp"
#include "ps2/system.hpp"

#include <3ds.h>
#include <citro3d.h>

#include "gs_v_shbin.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ps2 {
std::uint64_t now();
std::uint64_t idle_cycles();
}

namespace ps2::prof {
std::uint64_t g_ticks[Count];
}

namespace ps2::gs::pica {

namespace {

struct DrawVertex {
    float x, y, z, w;
    std::uint8_t r, g, b, a;
    float s, t;
    std::uint8_t fog, unused[3];
};

constexpr std::uint32_t kRingBytes = 11u << 19;   // 5.5 MB: the assembly hall has 54,000 triangles
constexpr std::uint32_t kRingVertices = kRingBytes / sizeof(DrawVertex);
constexpr int kTargetWidth = 512, kTargetHeight = 256;
constexpr int kMaxTargets = 8;
constexpr int kMaxTextures = 512;
constexpr std::size_t kCommandBuffer = 0x100000;
constexpr std::size_t kLinearReserve = 256u * 1024u;

struct Target {
    bool used = false;
    std::uint32_t fbp = 0;   // in pages
    int gs_width = 0, gs_height = 0;
    float sx = 1.0f, sy = 1.0f;
    C3D_Tex tex;
    C3D_RenderTarget *rt = nullptr;
    std::uint32_t last_used = 0;
};

struct Texture {
    bool used = false;
    std::uint32_t tbp = 0, tbw = 0, psm = 0, tw = 0, th = 0, clut = 0, texa = 0;
    int half = 0;
    std::uint32_t stamp = 0, hash = 0, sample = 0;
    std::uint32_t last_used = 0;
    C3D_Tex tex;
};

bool g_active = false;
bool g_frame_open = false;
u32 *g_frame_commands = nullptr;
DVLB_s *g_dvlb = nullptr;
shaderProgram_s g_program;
C3D_AttrInfo g_attr;
C3D_BufInfo g_buf;
int g_uniform_scale = -1, g_uniform_offset = -1;

DrawVertex *g_ring = nullptr;
std::uint32_t g_ring_used = 0, g_batch_first = 0, g_flushed_to = 0;

C3D_RenderTarget *g_screen = nullptr;
void *g_depth = nullptr;
Target g_targets[kMaxTargets];
Target *g_current = nullptr;
Texture g_textures[kMaxTextures];

std::uint8_t g_morton_x[64], g_morton_y[64];
std::uint32_t g_page_stamp[512];
std::uint32_t g_stamp = 1;
std::uint32_t g_clut_stamp = 1;
// A hash of what was last uploaded to each GS block address. This identifies a texture exactly, where a hash of whole pages
// changes whenever a neighbour sharing the page does.
std::uint32_t g_upload_hash[16384];
std::uint32_t g_upload_target = 0;
bool g_uploading = false;
C3D_Tex g_fog_ramp;
bool g_fog_on = false;
std::uint32_t g_fog_colour = 0;
std::uint32_t g_frame = 1;
std::uint32_t g_vblanks = 0, g_present_every = 2, g_shot_every = 0;
bool g_stats = false;
bool g_ring_warned = false;
std::uint64_t g_dump_frame = 0;
std::uint32_t g_skip_attr = 0;
std::uint32_t g_hide_from = 0, g_hide_to = 0, g_batch_number = 0;   // test aid: leave out these batches of each picture
bool g_draw_nothing = false, g_skipping = false, g_skip_longer = false;
std::uint32_t g_skip_now = 0;
std::uint32_t g_debug = 0;   // test aid: 1 no depth test, 2 no blending

// Batch state: what the vertices being collected assume.
std::uint64_t g_key[13];
std::uint32_t g_key_clut = 0, g_key_stamp = 0;
bool g_key_valid = false;
bool g_textured = false;
float g_u_scale = 1.0f, g_v_scale = 1.0f, g_v_offset = 0.0f;
float g_s_scale = 1.0f, g_t_scale = 1.0f;
bool g_fst = false, g_iip = false;
int g_ofx = 0, g_ofy = 0;
bool g_skip_batch = false;

// Counters for tuning.
std::uint32_t g_stat_reused = 0, g_stat_missing = 0;
std::uint32_t g_stat_draws = 0, g_stat_shapes = 0, g_stat_state = 0, g_stat_decoded = 0, g_stat_decode_texels = 0, g_stat_splits = 0;
std::uint32_t g_shapes_since_present = 0;
std::uint32_t g_setup_seen = 0;
constexpr std::uint32_t kSkip = 1, kSkipping = 2, kDrawNothing = 4;
std::uint32_t g_next_picture = 0, g_game_vblanks = 0;
std::uint32_t g_draw_every = 0, g_picture = 0, g_skipped_in_row = 0, g_skip_run = 0, g_stat_skipped = 0;
u64 g_round_start = 0, g_round_blocked = 0, g_round_idle = 0, g_seen_blocked = 0, g_seen_idle = 0;
std::uint32_t g_frame_number = 0;
std::uint32_t g_stat_prim[8], g_stat_upload = 0;
u64 g_stat_decode_ticks = 0;

constexpr int page_height(std::uint32_t psm) {
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 64;
    case PSMT8: case PSMT8H: return 64;
    case PSMT4: case PSMT4HL: case PSMT4HH: return 128;
    default: return 32;
    }
}

constexpr int page_width(std::uint32_t psm) {
    switch (psm) {
    case PSMT8: case PSMT8H: return 128;
    case PSMT4: case PSMT4HL: case PSMT4HH: return 128;
    default: return 64;
    }
}

// A batch whose pixels write different things by alpha: see set_tests.
struct Split {
    bool on, depth;
    GPU_TESTFUNC pass, fail, depth_test;
    int reference, pass_writes, fail_writes;
} g_split{};

void flush_ring() {
    if (g_ring_used > g_flushed_to) {
        GSPGPU_FlushDataCache(g_ring + g_flushed_to, (g_ring_used - g_flushed_to) * sizeof(DrawVertex));
        g_flushed_to = g_ring_used;
    }
}

void flush_batch() {
    const std::uint32_t count = g_ring_used - g_batch_first;
    if (!count) return;
    if (g_split.on) {
        C3D_AlphaTest(true, g_split.pass, g_split.reference);
        C3D_DepthTest(g_split.depth, g_split.depth_test, static_cast<GPU_WRITEMASK>(g_split.pass_writes));
        C3D_DrawArrays(GPU_TRIANGLES, static_cast<int>(g_batch_first), static_cast<int>(count));
        C3D_AlphaTest(true, g_split.fail, g_split.reference);
        C3D_DepthTest(g_split.depth, g_split.depth_test, static_cast<GPU_WRITEMASK>(g_split.fail_writes));
    }
    C3D_DrawArrays(GPU_TRIANGLES, static_cast<int>(g_batch_first), static_cast<int>(count));
    g_batch_first = g_ring_used;
    ++g_stat_draws;
    if (C3D_GetCmdBufUsage() > 0.75f) {
        flush_ring();
        C3D_FrameSplit(0);
        ++g_stat_splits;
    }
}

std::uint32_t expand16(std::uint32_t c, std::uint32_t ta0, std::uint32_t ta1, bool aem) {
    const std::uint32_t r = (c & 0x1Fu) << 3, g = ((c >> 5) & 0x1Fu) << 3, b = ((c >> 10) & 0x1Fu) << 3;
    const std::uint32_t a = (c & 0x8000u) ? ta1 : ((aem && !(c & 0x7FFFu)) ? 0u : ta0);
    return r | (g << 8) | (b << 16) | (a << 24);
}

// GS colour 0xAABBGGRR to the GPU's RGBA8, which reads as 0xRRGGBBAA.
inline std::uint32_t to_gpu(std::uint32_t c) { return __builtin_bswap32(c); }

std::uint32_t clut_hash(const State &st, std::uint32_t psm, std::uint32_t csa) {
    const std::uint32_t *p = st.clut;
    std::uint32_t n = 0;
    if (psm == PSMT8 || psm == PSMT8H) n = 256;
    else if (psm == PSMT4 || psm == PSMT4HL || psm == PSMT4HH) {
        p += (csa & 15u) * 16u;
        n = 16;
    }
    std::uint32_t h = 2166136261u;
    for (std::uint32_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

// A quick look at a small texture in GS memory, to tell whether a cached copy is really that picture.
std::uint32_t sample_texels(std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, std::uint32_t tw, std::uint32_t th) {
    const std::uint32_t w = 1u << tw, h = 1u << th, step_x = w > 8u ? w / 8u : 1u, step_y = h > 8u ? h / 8u : 1u;
    std::uint32_t sum = 0x811C9DC5u;
    for (std::uint32_t y = 0; y < h; y += step_y)
        for (std::uint32_t x = 0; x < w; x += step_x) sum = (sum ^ read_pixel(psm, bp, bw, x, y)) * 16777619u;
    return sum;
}

void decode(Texture &e, const State &st, std::uint32_t cpsm, std::uint32_t csa, std::uint32_t ta0, std::uint32_t ta1, bool aem) {
    const u64 start = svcGetSystemTick();
    const int w = e.tex.width, h = e.tex.height;
    const int gw = 1 << e.tw, gh = 1 << e.th;
    const std::uint32_t psm = e.psm, bp = e.tbp, bw = e.tbw;
    const bool clut16 = !(cpsm == PSMCT32 || cpsm == PSMCT24);
    auto *out = static_cast<std::uint32_t *>(e.tex.data);
    const std::uint32_t *clut = st.clut;
    for (int ty = 0; ty < h; ty += 8) {
        for (int tx = 0; tx < w; tx += 8) {
            std::uint32_t *tile = out + (ty * w + tx * 8);
            for (int i = 0; i < 64; ++i) {
                const std::uint32_t x = static_cast<std::uint32_t>(((tx + g_morton_x[i]) << e.half) % gw), y = static_cast<std::uint32_t>(((ty + g_morton_y[i]) << e.half) % gh);
                const std::uint32_t raw = read_pixel(psm, bp, bw, x, y);
                std::uint32_t c;
                switch (psm) {
                case PSMCT32: case PSMZ32: c = raw; break;
                case PSMCT24: case PSMZ24: c = (raw & 0xFFFFFFu) | (((aem && !(raw & 0xFFFFFFu)) ? 0u : ta0) << 24); break;
                case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: c = expand16(raw, ta0, ta1, aem); break;
                case PSMT8: case PSMT8H: {
                    const std::uint32_t v = clut[raw & 255u];
                    c = clut16 ? expand16(v, ta0, ta1, aem) : v;
                    break;
                }
                case PSMT4: case PSMT4HL: case PSMT4HH: {
                    const std::uint32_t v = clut[((csa & 15u) * 16u + (raw & 15u)) & 511u];
                    c = clut16 ? expand16(v, ta0, ta1, aem) : v;
                    break;
                }
                default: c = raw; break;
                }
                tile[i] = to_gpu(c);
            }
        }
    }
    C3D_TexFlush(&e.tex);
    if (w <= 64 && h <= 64) {
        e.sample = sample_texels(bp, bw, psm, e.tw, e.th);
        // A small texture of one solid visible colour: how the black letters look when they are made.
        bool solid = (out[0] & 0xFFu) != 0;
        for (int i = 1; solid && i < w * h; ++i) solid = out[i] == out[0];
        static int reports = 0;
        if (solid && reports < 40) {
            ++reports;
            std::fprintf(stderr, "[solid] f%llu tbp %lu psm %02lX %dx%d texel %08lX raw %08lX clut %08lX csa %lu hash %08lX\n",
                         static_cast<unsigned long long>(ps2::frame_count()), static_cast<unsigned long>(bp), static_cast<unsigned long>(psm),
                         w, h, static_cast<unsigned long>(out[0]), static_cast<unsigned long>(read_pixel(psm, bp, bw, 0, 0)),
                         static_cast<unsigned long>(e.clut), static_cast<unsigned long>(csa), static_cast<unsigned long>(e.hash));
        }
    }
    ++g_stat_decoded;
    g_stat_decode_texels += static_cast<std::uint32_t>(w * h);
    g_stat_decode_ticks += svcGetSystemTick() - start;
}

struct PageRange {
    std::uint32_t first, count;
};

PageRange pages_of(std::uint32_t tbp, std::uint32_t tbw, std::uint32_t psm, std::uint32_t th) {
    const std::uint32_t first = tbp / 32u;
    const std::uint32_t rows = ((1u << th) + static_cast<std::uint32_t>(page_height(psm)) - 1u) / static_cast<std::uint32_t>(page_height(psm));
    const std::uint32_t wide = std::max<std::uint32_t>(tbw, 1u) * 64u / static_cast<std::uint32_t>(page_width(psm));
    const std::uint32_t count = rows * std::max<std::uint32_t>(wide, 1u) + 1u;
    return {first, std::min<std::uint32_t>(count, 512u - std::min<std::uint32_t>(first, 512u))};
}

std::uint32_t pages_stamp(PageRange r) {
    std::uint32_t newest = 0;
    for (std::uint32_t p = r.first; p < r.first + r.count; ++p) newest = std::max(newest, g_page_stamp[p]);
    return newest;
}

// PS2 games send the same textures to the GS again and again, because its memory is so small. A hash of the raw pages is far
// cheaper than decoding, so a repeated upload costs only this.
std::uint32_t pages_hash(PageRange r) {
    const auto *p = reinterpret_cast<const std::uint32_t *>(memory() + r.first * 8192u);
    const std::uint32_t words = r.count * 2048u;
    std::uint32_t h = 0x811C9DC5u;
    for (std::uint32_t i = 0; i < words; ++i) h = ((h << 5) | (h >> 27)) ^ p[i];
    return h;
}

void free_texture(Texture &e) {
    C3D_TexDelete(&e.tex);
    e.used = false;
}

// Textures still queued for this picture must stay. Those not used since the last picture go first. If that is not enough, those
// used only by the last picture go too: the GPU may still be reading one, which costs at most a flicker, where a texture that
// does not fit leaves its shape out altogether.
bool free_oldest() {
    Texture *oldest = nullptr;
    for (Texture &e : g_textures)
        if (e.used && e.last_used < g_frame && (!oldest || e.last_used < oldest->last_used)) oldest = &e;
    if (!oldest) return false;
    free_texture(*oldest);
    return true;
}

bool make_room(std::size_t bytes) {
    while (linearSpaceFree() < bytes + kLinearReserve)
        if (!free_oldest()) return false;
    return true;
}

// A 400x240 screen cannot show a 256x256 texture on a 3D surface at full detail, and full size would fill memory in one scene.
// Menus and text use exact texel coordinates and keep every pixel.
Texture *find_texture(const State &st, const Context &c, bool three_d) {
    const std::uint32_t tbp = static_cast<std::uint32_t>(c.tex0) & 0x3FFFu;
    const std::uint32_t tbw = static_cast<std::uint32_t>(c.tex0 >> 14) & 0x3Fu;
    const std::uint32_t psm = static_cast<std::uint32_t>(c.tex0 >> 20) & 0x3Fu;
    const std::uint32_t tw = std::min<std::uint32_t>(static_cast<std::uint32_t>(c.tex0 >> 26) & 0xFu, 10u);
    const std::uint32_t th = std::min<std::uint32_t>(static_cast<std::uint32_t>(c.tex0 >> 30) & 0xFu, 10u);
    const std::uint32_t cpsm = static_cast<std::uint32_t>(c.tex0 >> 51) & 0xFu;
    const std::uint32_t csa = static_cast<std::uint32_t>(c.tex0 >> 56) & 0x1Fu;
    const std::uint32_t ta0 = static_cast<std::uint32_t>(st.texa) & 0xFFu, ta1 = static_cast<std::uint32_t>(st.texa >> 32) & 0xFFu;
    const bool aem = (st.texa >> 15) & 1u;
    const std::uint32_t clut = clut_hash(st, psm, csa) ^ cpsm;
    const std::uint32_t texa = ta0 | (ta1 << 8) | (static_cast<std::uint32_t>(aem) << 16);
    const int half = three_d && (tw >= 8 || th >= 8) ? 1 : 0;
    const auto same_picture = [&](const Texture &e) { return e.psm == psm && e.tw == tw && e.th == th && e.clut == clut && e.texa == texa && e.half == half; };
    const PageRange pages = pages_of(tbp, tbw, psm, th);
    Texture *slot = nullptr;
    // This game moves its textures around GS memory every frame, so the same picture turns up at a new address each time. When
    // the bytes uploaded to this address are known, they identify the picture wherever it is.
    std::uint32_t hash = g_upload_hash[tbp];
    if (hash) {
        for (Texture &e : g_textures) {
            if (!e.used) {
                if (!slot) slot = &e;
            } else if (e.hash == hash && same_picture(e)) {
                // The hash can be stale or shared, so a small texture is checked against GS memory: a wrong one showed as black letters.
                if (tw <= 6 && th <= 6 && e.sample != sample_texels(tbp, tbw, psm, tw, th)) {
                    static int reports = 0;
                    if (reports < 40) {
                        ++reports;
                        std::fprintf(stderr, "[stale] f%llu tbp %lu was tbp %lu psm %02lX hash %08lX\n",
                                     static_cast<unsigned long long>(ps2::frame_count()), static_cast<unsigned long>(tbp),
                                     static_cast<unsigned long>(e.tbp), static_cast<unsigned long>(psm), static_cast<unsigned long>(hash));
                    }
                    continue;
                }
                e.last_used = g_frame;
                ++g_stat_reused;
                return &e;
            }
        }
    } else {
        const std::uint32_t newest = pages_stamp(pages);
        for (Texture &e : g_textures) {
            if (!e.used) {
                if (!slot) slot = &e;
            } else if (e.stamp >= newest && e.tbp == tbp && e.tbw == tbw && same_picture(e)) {
                e.last_used = g_frame;
                return &e;
            }
        }
        // Something was written over these pages, which may still be the same picture.
        hash = pages_hash(pages);
        for (Texture &e : g_textures)
            if (e.used && e.hash == hash && e.tbp == tbp && e.tbw == tbw && same_picture(e)) {
                e.stamp = g_stamp;
                e.last_used = g_frame;
                ++g_stat_reused;
                return &e;
            }
    }
    if (g_dump_frame && ps2::frame_count() >= g_dump_frame && ps2::frame_count() < g_dump_frame + 4)
        std::fprintf(stderr, "[miss] f%llu tbp %lu psm %02lX %lux%lu half %d clut %08lX upload %08lX page %08lX\n", static_cast<unsigned long long>(ps2::frame_count()), static_cast<unsigned long>(tbp),
                     static_cast<unsigned long>(psm), 1ul << tw, 1ul << th, half, static_cast<unsigned long>(clut), static_cast<unsigned long>(g_upload_hash[tbp]), static_cast<unsigned long>(pages_hash(pages)));
    const int w = std::max(8, (1 << tw) >> half), h = std::max(8, (1 << th) >> half);
    const std::size_t bytes = static_cast<std::size_t>(w) * h * 4;
    if (!make_room(bytes)) return nullptr;
    if (!slot) {
        Texture *oldest = nullptr;
        for (Texture &e : g_textures)
            if (e.last_used + 1 < g_frame && (!oldest || e.last_used < oldest->last_used)) oldest = &e;
        if (!oldest) return nullptr;
        free_texture(*oldest);
        slot = oldest;
    }
    // After the menus the free memory can be in pieces too small for a big texture, however much of it there is.
    while (!C3D_TexInit(&slot->tex, static_cast<u16>(w), static_cast<u16>(h), GPU_RGBA8))
        if (!free_oldest()) return nullptr;
    slot->used = true;
    slot->tbp = tbp;
    slot->tbw = tbw;
    slot->psm = psm;
    slot->tw = tw;
    slot->th = th;
    slot->half = half;
    slot->clut = clut;
    slot->texa = texa;
    slot->stamp = g_stamp;
    slot->hash = hash;
    slot->last_used = g_frame;
    decode(*slot, st, cpsm, csa, ta0, ta1, aem);
    return slot;
}

int fit_size(int gs_size, int largest) {
    int size = 8;
    while (size < gs_size && size < largest) size *= 2;
    return size;
}

void release_target(Target &t) {
    C3D_RenderTargetDelete(t.rt);
    C3D_TexDelete(&t.tex);
    t.rt = nullptr;
    t.used = false;
}

// A target's texture is the true size of the PS2 picture, up to 512x256. A smaller picture sampled as a texture must clamp at
// its own edge, as it does on the GS, not run on into unused texels.
bool create_target(Target &t, std::uint32_t fbp, int gs_width, int gs_height) {
    const int width = fit_size(gs_width, kTargetWidth), height = fit_size(gs_height, kTargetHeight);
    if (!C3D_TexInitVRAM(&t.tex, static_cast<u16>(width), static_cast<u16>(height), GPU_RGBA8)) return false;
    const bool full = width == kTargetWidth && height == kTargetHeight;
    t.rt = C3D_RenderTargetCreateFromTex(&t.tex, GPU_TEXFACE_2D, 0, full ? C3D_DEPTHTYPE(-1) : C3D_DEPTHTYPE(GPU_RB_DEPTH24));
    if (!t.rt) {
        C3D_TexDelete(&t.tex);
        return false;
    }
    if (full) C3D_FrameBufDepth(&t.rt->frameBuf, g_depth, GPU_RB_DEPTH24);
    C3D_RenderTargetClear(t.rt, C3D_CLEAR_ALL, 0, 0);
    t.used = true;
    t.fbp = fbp;
    t.gs_width = gs_width;
    t.gs_height = gs_height;
    t.sx = std::min(1.0f, static_cast<float>(width) / static_cast<float>(gs_width));
    t.sy = std::min(1.0f, static_cast<float>(height) / static_cast<float>(gs_height));
    t.last_used = g_frame;
    return true;
}

Target *find_target(std::uint32_t fbp, int gs_width, int gs_height) {
    Target *free_slot = nullptr, *oldest = nullptr;
    for (Target &t : g_targets) {
        if (t.used && t.fbp == fbp) {
            if (gs_width > t.gs_width || gs_height > t.gs_height) {
                const int w = std::max(gs_width, t.gs_width), h = std::max(gs_height, t.gs_height);
                if (fit_size(w, kTargetWidth) != t.tex.width || fit_size(h, kTargetHeight) != t.tex.height) {
                    if (g_current == &t) g_current = nullptr;
                    release_target(t);
                    return create_target(t, fbp, w, h) ? &t : nullptr;
                }
                t.gs_width = w;
                t.gs_height = h;
                t.sx = std::min(1.0f, static_cast<float>(t.tex.width) / static_cast<float>(w));
                t.sy = std::min(1.0f, static_cast<float>(t.tex.height) / static_cast<float>(h));
            }
            t.last_used = g_frame;
            return &t;
        }
        if (!t.used && !free_slot) free_slot = &t;
        if (t.used && (!oldest || t.last_used < oldest->last_used)) oldest = &t;
    }
    if (free_slot && create_target(*free_slot, fbp, gs_width, gs_height)) return free_slot;
    if (!oldest) return nullptr;
    if (g_current == oldest) g_current = nullptr;
    release_target(*oldest);
    return create_target(*oldest, fbp, gs_width, gs_height) ? oldest : nullptr;
}

void bind_target(Target *t) {
    g_current = t;
    C3D_FrameDrawOn(t->rt);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_uniform_scale, 2.0f * t->sx / t->tex.width, -2.0f * t->sy / t->tex.height, 1.0f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_uniform_offset, -1.0f, 1.0f, 0.0f, 0.0f);
}

// GS fog mixes each pixel towards a colour by a value that comes with each vertex. The GPU has only one colour to carry per vertex,
// so that value travels as a texture coordinate into a ramp on the second texture unit, whose alpha is the coordinate again.
void set_fog(bool on, std::uint32_t colour) {
    C3D_TexEnv *env = C3D_GetTexEnv(2);
    C3D_TexEnvInit(env);
    if (on) {
        C3D_TexBind(1, &g_fog_ramp);
        C3D_TexEnvSrc(env, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_TEXTURE1);
        C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
        C3D_TexEnvColor(env, colour);
        C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    } else {   // the ramp stays bound: nothing reads it, and citro3d cannot unbind this unit
        C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    }
}

void set_texenv(bool textured, int tfx, bool tcc) {
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    if (!textured) {
        C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    } else {
        C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        if (tfx == 1) {
            C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
        } else {
            C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
            C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_2);
        }
        if (!tcc) {
            C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
        } else if (tfx == 0) {
            C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
            C3D_TexEnvScale(env, C3D_Alpha, GPU_TEVSCALE_2);
        } else if (tfx == 2) {
            C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(env, C3D_Alpha, GPU_ADD);
        } else {
            C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
        }
    }
    // The GS treats alpha 0x80 as 1.0, so alpha doubles here, with values above 0x80 saturating.
    C3D_TexEnv *second = C3D_GetTexEnv(1);
    C3D_TexEnvInit(second);
    C3D_TexEnvSrc(second, C3D_Both, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
    C3D_TexEnvFunc(second, C3D_Both, GPU_REPLACE);
    C3D_TexEnvScale(second, C3D_Alpha, GPU_TEVSCALE_2);
    set_fog(g_fog_on, g_fog_colour);
}

void set_blend(bool abe, std::uint64_t alpha) {
    if (!abe) {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        return;
    }
    const int a = static_cast<int>(alpha) & 3, b = static_cast<int>(alpha >> 2) & 3, c = static_cast<int>(alpha >> 4) & 3, d = static_cast<int>(alpha >> 6) & 3;
    const std::uint32_t fix = static_cast<std::uint32_t>(alpha >> 32) & 0xFFu;
    GPU_BLENDFACTOR cf = GPU_SRC_ALPHA, cinv = GPU_ONE_MINUS_SRC_ALPHA;
    if (c == 1) cf = GPU_DST_ALPHA, cinv = GPU_ONE_MINUS_DST_ALPHA;
    else if (c == 2) {
        cf = GPU_CONSTANT_ALPHA, cinv = GPU_ONE_MINUS_CONSTANT_ALPHA;
        C3D_BlendingColor(std::min<std::uint32_t>(fix * 2u, 255u) << 24);
    }
    enum { Cs = 0, Cd = 1, Zero = 2 };
    GPU_BLENDEQUATION eq = GPU_BLEND_ADD;
    GPU_BLENDFACTOR src = cf, dst = cinv;
    if (a == b) {   // (A-A)*C + D is just D
        src = d == Cs ? GPU_ONE : GPU_ZERO;
        dst = d == Cd ? GPU_ONE : GPU_ZERO;
    } else if (a == Cs && b == Cd && d == Cd) {
        src = cf, dst = cinv;
    } else if (a == Cs && b == Zero && d == Cd) {
        src = cf, dst = GPU_ONE;
    } else if (a == Cs && b == Zero && d == Zero) {
        src = cf, dst = GPU_ZERO;
    } else if (a == Zero && b == Cs && d == Cd) {
        eq = GPU_BLEND_REVERSE_SUBTRACT, src = cf, dst = GPU_ONE;
    } else if (a == Cd && b == Cs && d == Cs) {
        src = cinv, dst = cf;
    } else if (a == Cd && b == Zero && d == Zero) {
        src = GPU_ZERO, dst = cf;
    } else if (a == Zero && b == Cd && d == Cd) {
        src = GPU_ZERO, dst = cinv;
    } else if (a == Cd && b == Zero && d == Cd) {
        src = GPU_ZERO, dst = GPU_ONE;
    }
    C3D_AlphaBlend(eq, GPU_BLEND_ADD, src, dst, GPU_ONE, GPU_ZERO);
}

void set_tests(const Context &c, bool abe) {
    (void)abe;
    const bool ate = c.test & 1u;
    const int atst = static_cast<int>(c.test >> 1) & 7;
    const std::uint32_t aref = static_cast<std::uint32_t>(c.test >> 4) & 0xFFu;
    const int afail = static_cast<int>(c.test >> 12) & 3;
    const bool zte = (c.test >> 16) & 1u;
    const int ztst = static_cast<int>(c.test >> 17) & 3;
    const bool zmsk = (c.zbuf >> 32) & 1u;
    const std::uint32_t fbmsk = static_cast<std::uint32_t>(c.frame >> 32);

    int writes = GPU_WRITE_ALPHA | GPU_WRITE_BLUE | GPU_WRITE_GREEN | GPU_WRITE_RED;
    if ((fbmsk & 0xFFu) == 0xFFu) writes &= ~GPU_WRITE_RED;
    if ((fbmsk & 0xFF00u) == 0xFF00u) writes &= ~GPU_WRITE_GREEN;
    if ((fbmsk & 0xFF0000u) == 0xFF0000u) writes &= ~GPU_WRITE_BLUE;
    if ((fbmsk & 0xFF000000u) == 0xFF000000u) writes &= ~GPU_WRITE_ALPHA;
    bool zwrite = zte && !zmsk;
    bool test = ate;
    if (ate && atst == 0) {   // "never pass" is how the GS writes only colour or only depth
        test = false;
        if (afail == 1) zwrite = false;
        else if (afail == 2) writes = 0;
        else if (afail == 3) zwrite = false, writes &= ~GPU_WRITE_ALPHA;
        else g_skip_batch = true;
    } else if (ate && atst == 1) {
        test = false;
    } else if (ate && afail != 0) {
        test = false;   // nothing is discarded: see the two passes below
    }
    static const GPU_TESTFUNC kAlphaTest[8] = {GPU_NEVER, GPU_ALWAYS, GPU_LESS, GPU_LEQUAL, GPU_EQUAL, GPU_GEQUAL, GPU_GREATER, GPU_NOTEQUAL};
    C3D_AlphaTest(test, kAlphaTest[atst], static_cast<int>(std::min<std::uint32_t>(aref * 2u, 255u)));
    static const GPU_TESTFUNC kDepthTest[4] = {GPU_NEVER, GPU_ALWAYS, GPU_GEQUAL, GPU_GREATER};
    if (zwrite) writes |= GPU_WRITE_DEPTH;
    C3D_DepthTest((zte || zwrite) && !(g_debug & 1u), zte ? kDepthTest[ztst] : GPU_ALWAYS, static_cast<GPU_WRITEMASK>(writes));
    g_split.on = false;
    if (ate && atst >= 2 && afail != 0) {
        // A pixel that fails the alpha test still writes part of itself on the GS. Nearly every model here works that way: solid
        // pixels write depth, see-through ones only colour. The GPU cannot split a pixel, so such a batch is drawn twice, once
        // for each kind. One draw would leave depth behind the see-through parts, and whatever comes later is hidden there.
        int fail_writes = writes;
        if (afail == 1) fail_writes &= ~GPU_WRITE_DEPTH;
        else if (afail == 2) fail_writes &= GPU_WRITE_DEPTH;
        else fail_writes &= ~(GPU_WRITE_DEPTH | GPU_WRITE_ALPHA);
        static const GPU_TESTFUNC kOpposite[8] = {GPU_ALWAYS, GPU_NEVER, GPU_GEQUAL, GPU_GREATER, GPU_NOTEQUAL, GPU_LESS, GPU_LEQUAL, GPU_EQUAL};
        if (fail_writes != writes) {
            g_split = {true, (zte || zwrite) && !(g_debug & 1u), kAlphaTest[atst], kOpposite[atst], zte ? kDepthTest[ztst] : GPU_ALWAYS, std::min<int>(static_cast<int>(aref * 2u), 255), writes, fail_writes};
        }
    }
}

void set_scissor(const Context &c, const Target &t) {
    const int x0 = static_cast<int>(c.scissor & 0x7FFu), x1 = static_cast<int>((c.scissor >> 16) & 0x7FFu);
    const int y0 = static_cast<int>((c.scissor >> 32) & 0x7FFu), y1 = static_cast<int>((c.scissor >> 48) & 0x7FFu);
    const int tw = t.tex.width, th = t.tex.height;
    const int left = std::clamp(static_cast<int>(std::floor(static_cast<float>(x0) * t.sx)), 0, tw);
    const int right = std::clamp(static_cast<int>(std::ceil(static_cast<float>(x1 + 1) * t.sx)), 0, tw);
    const int top = std::clamp(static_cast<int>(std::floor(static_cast<float>(y0) * t.sy)), 0, th);
    const int bottom = std::clamp(static_cast<int>(std::ceil(static_cast<float>(y1 + 1) * t.sy)), 0, th);
    C3D_SetScissor(GPU_SCISSOR_NORMAL, static_cast<u32>(left), static_cast<u32>(top), static_cast<u32>(right), static_cast<u32>(bottom));
}

void dump_batch(const State &st, const Context &c, std::uint64_t attr, std::uint32_t verts) {
    const std::uint32_t tbp = static_cast<std::uint32_t>(c.tex0) & 0x3FFFu;
    if (verts) {
        float lo[6] = {1e9f, 1e9f, 1e9f, 1e9f, 1e9f, 1e9f}, hi[6] = {-1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f};
        int alpha_max = 0;
        for (std::uint32_t i = g_batch_first; i < g_ring_used; ++i) {
            const DrawVertex &v = g_ring[i];
            const float f[6] = {v.x, v.y, v.z, v.w, v.s, v.t};
            for (int k = 0; k < 6; ++k) {
                lo[k] = std::min(lo[k], f[k]);
                hi[k] = std::max(hi[k], f[k]);
            }
            alpha_max = std::max<int>(alpha_max, v.a);
        }
        std::fprintf(stderr, "[range] x %.0f..%.0f y %.0f..%.0f z %.3f..%.3f w %.3g..%.3g s %.2f..%.2f t %.2f..%.2f amax %d\n", lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], lo[3], hi[3], lo[4], hi[4], lo[5], hi[5], alpha_max);
    }
    std::fprintf(stderr, "[wrap] %s wms %llu wmt %llu minu %llu maxu %llu minv %llu maxv %llu tw %llu th %llu\n", (attr & 0x10u) ? "on" : "off", static_cast<unsigned long long>(c.clamp & 3u), static_cast<unsigned long long>((c.clamp >> 2) & 3u), static_cast<unsigned long long>((c.clamp >> 4) & 0x3FFu),
                 static_cast<unsigned long long>((c.clamp >> 14) & 0x3FFu), static_cast<unsigned long long>((c.clamp >> 24) & 0x3FFu), static_cast<unsigned long long>((c.clamp >> 34) & 0x3FFu), 1ull << ((c.tex0 >> 26) & 0xFu), 1ull << ((c.tex0 >> 30) & 0xFu));
    std::fprintf(stderr, "[batch] #%lu v%lu attr %03llX fb %llu fbw %llu fpsm %02llX mask %08llX zb %llu z %llX | tex %s tbp %lu tbw %llu psm %02llX %llux%llu tcc %llu tfx %llu cbp %llu cpsm %llX csa %llu | alpha %d%d%d%d fix %llu test %05llX scissor %llu-%llu,%llu-%llu\n",
                 static_cast<unsigned long>(g_batch_number), static_cast<unsigned long>(verts), static_cast<unsigned long long>(attr & 0x7FFu), static_cast<unsigned long long>(c.frame & 0x1FFu),
                 static_cast<unsigned long long>((c.frame >> 16) & 0x3Fu), static_cast<unsigned long long>((c.frame >> 24) & 0x3Fu), static_cast<unsigned long long>(c.frame >> 32),
                 static_cast<unsigned long long>(c.zbuf & 0x1FFu), static_cast<unsigned long long>(c.zbuf >> 24), (attr & 0x10u) ? "on" : "off", static_cast<unsigned long>(tbp),
                 static_cast<unsigned long long>((c.tex0 >> 14) & 0x3Fu), static_cast<unsigned long long>((c.tex0 >> 20) & 0x3Fu), 1ull << ((c.tex0 >> 26) & 0xFu), 1ull << ((c.tex0 >> 30) & 0xFu),
                 static_cast<unsigned long long>((c.tex0 >> 34) & 1u), static_cast<unsigned long long>((c.tex0 >> 35) & 3u), static_cast<unsigned long long>((c.tex0 >> 37) & 0x3FFFu),
                 static_cast<unsigned long long>((c.tex0 >> 51) & 0xFu), static_cast<unsigned long long>((c.tex0 >> 56) & 0x1Fu), static_cast<int>(c.alpha & 3u), static_cast<int>((c.alpha >> 2) & 3u),
                 static_cast<int>((c.alpha >> 4) & 3u), static_cast<int>((c.alpha >> 6) & 3u), static_cast<unsigned long long>((c.alpha >> 32) & 0xFFu), static_cast<unsigned long long>(c.test & 0xFFFFFu),
                 static_cast<unsigned long long>(c.scissor & 0x7FFu), static_cast<unsigned long long>((c.scissor >> 16) & 0x7FFu), static_cast<unsigned long long>((c.scissor >> 32) & 0x7FFu),
                 static_cast<unsigned long long>((c.scissor >> 48) & 0x7FFu));
}

float z_scale_of(std::uint64_t zbuf) {
    switch (static_cast<std::uint32_t>(zbuf >> 24) & 0xFu) {
    case 0: return 1.0f / 4294967296.0f;
    case 1: return 1.0f / 16777216.0f;
    default: return 1.0f / 65536.0f;
    }
}

float g_z_scale = 1.0f;

void apply_state(const State &st, const Context &c, std::uint64_t attr) {
    if (g_dump_frame && ps2::frame_count() >= g_dump_frame && ps2::frame_count() < g_dump_frame + 2) dump_batch(st, c, attr, g_ring_used - g_batch_first);
    flush_batch();
    ++g_stat_state;
    ++g_batch_number;
    g_z_scale = z_scale_of(c.zbuf);
    g_skip_batch = (g_skip_attr && (attr & 0x7FFu) == g_skip_attr) || (g_hide_from && g_batch_number >= g_hide_from && g_batch_number < g_hide_to);
    if (g_skip_batch) return;
    const std::uint32_t fbp = static_cast<std::uint32_t>(c.frame) & 0x1FFu;
    const std::uint32_t fbw = static_cast<std::uint32_t>(c.frame >> 16) & 0x3Fu;
    // The frame buffer's width is exact. Its height is not stored anywhere, and games often leave the scissor at the GS's
    // maximum of 2048, so a scissor taller than any real picture says nothing: fall back to the height of a full PS2 screen.
    const int width = std::max(static_cast<int>(fbw) * 64, 64);
    int height = static_cast<int>(((c.scissor >> 48) & 0x7FFu) + 1);
    if (height > 512) height = 448;
    Target *t = find_target(fbp, width, height);
    if (!t) {
        g_skip_batch = true;
        return;
    }
    if (t != g_current) bind_target(t);
    else bind_target(t);   // the scale may have changed

    const bool tme = attr & 0x10u;
    const bool abe = attr & 0x40u;
    g_fst = attr & 0x100u;
    g_fog_on = attr & 0x20u;
    g_fog_colour = static_cast<std::uint32_t>(st.fogcol) & 0xFFFFFFu;
    g_iip = attr & 0x8u;
    g_ofx = static_cast<int>(c.xyoffset & 0xFFFFu);
    g_ofy = static_cast<int>((c.xyoffset >> 32) & 0xFFFFu);
    set_tests(c, abe);
    // With PABE, a pixel without the top alpha bit set is not blended. One with it set has alpha of 1.0 or more, so a blend by
    // source alpha gives the source colour anyway. Either way the pixel comes out solid.
    const bool by_source_alpha = ((c.alpha >> 4) & 3u) == 0u;
    set_blend(abe && !((st.pabe & 1u) && by_source_alpha) && !(g_debug & 2u), c.alpha);
    set_scissor(c, *t);

    g_textured = false;
    if (tme) {
        const std::uint32_t tbp = static_cast<std::uint32_t>(c.tex0) & 0x3FFFu;
        const std::uint32_t tw = std::min<std::uint32_t>(static_cast<std::uint32_t>(c.tex0 >> 26) & 0xFu, 10u);
        const std::uint32_t th = std::min<std::uint32_t>(static_cast<std::uint32_t>(c.tex0 >> 30) & 0xFu, 10u);
        const int tfx = static_cast<int>(c.tex0 >> 35) & 3;
        const bool tcc = (c.tex0 >> 34) & 1u;
        C3D_Tex *tex = nullptr;
        Target *source = nullptr;
        if (!(tbp & 31u))
            for (Target &o : g_targets)
                if (o.used && o.fbp == tbp / 32u && &o != t) source = &o;
        if (source) {
            // Drawing with a picture that was rendered earlier.
            tex = &source->tex;
            g_u_scale = source->sx / source->tex.width;
            g_v_scale = -source->sy / source->tex.height;
            g_v_offset = 1.0f;
            g_s_scale = static_cast<float>(1 << tw) * source->sx / source->tex.width;
            g_t_scale = static_cast<float>(1 << th) * source->sy / source->tex.height;
        } else if (Texture *e = find_texture(st, c, !g_fst)) {
            tex = &e->tex;
            const float full_width = static_cast<float>(e->tex.width << e->half), full_height = static_cast<float>(e->tex.height << e->half);
            g_u_scale = 1.0f / full_width;
            g_v_scale = -1.0f / full_height;
            g_v_offset = 1.0f;
            g_s_scale = static_cast<float>(1 << tw) / full_width;
            g_t_scale = static_cast<float>(1 << th) / full_height;
        }
        if (tex) {
            const int wms = static_cast<int>(c.clamp) & 3, wmt = static_cast<int>(c.clamp >> 2) & 3;
            const bool bilinear = (c.tex1 >> 5) & 1u;
            const GPU_TEXTURE_FILTER_PARAM filter = bilinear ? GPU_LINEAR : GPU_NEAREST;
            C3D_TexSetFilter(tex, filter, filter);
            C3D_TexSetWrap(tex, wms == 0 ? GPU_REPEAT : GPU_CLAMP_TO_EDGE, wmt == 0 ? GPU_REPEAT : GPU_CLAMP_TO_EDGE);
            C3D_TexBind(0, tex);
            g_textured = true;
            set_texenv(true, tfx, tcc);
        } else {
            g_skip_batch = true;
            ++g_stat_missing;
        }
    } else {
        C3D_TexBind(0, nullptr);
        set_texenv(false, 0, false);
    }
}

bool ensure_state(const State &st, const Context &c, std::uint64_t attr) {
    const std::uint64_t key[13] = {st.fogcol, attr & ~7ull, c.frame, c.zbuf, c.test, c.alpha, c.scissor, c.tex0, c.tex1, c.clamp, st.texa, c.xyoffset, st.pabe};
    if (!g_key_valid || g_key_clut != g_clut_stamp || g_key_stamp != g_stamp || std::memcmp(key, g_key, sizeof key)) {
        std::memcpy(g_key, key, sizeof key);
        g_key_clut = g_clut_stamp;
        g_key_stamp = g_stamp;
        g_key_valid = true;
        apply_state(st, c, attr);
    }
    return !g_skip_batch;
}

inline float gs_x(const Vertex &v) { return (static_cast<float>(v.x) - static_cast<float>(g_ofx)) / 16.0f; }
inline float gs_y(const Vertex &v) { return (static_cast<float>(v.y) - static_cast<float>(g_ofy)) / 16.0f; }

inline void write_vertex(DrawVertex *o, float x, float y, const Vertex &z_from, const Vertex &colour, const Vertex &tex) {
    o->x = x;
    o->y = y;
    o->z = std::max(-static_cast<float>(z_from.z) * g_z_scale, -1.0f);   // the GS clamps at the top of the buffer's range
    o->r = colour.r;
    o->g = colour.g;
    o->b = colour.b;
    o->a = colour.a;
    o->fog = z_from.f;
    if (g_fst) {
        o->w = 1.0f;
        o->s = (static_cast<float>(tex.u) / 16.0f) * g_u_scale;
        o->t = g_v_offset + (static_cast<float>(tex.v) / 16.0f) * g_v_scale;
    } else {
        const float q = tex.q > 1e-6f ? tex.q : 1e-6f;
        const float iq = 1.0f / q;
        o->w = iq;
        o->s = tex.s * iq * g_s_scale;
        o->t = g_v_offset - tex.t * iq * g_t_scale;
    }
}

void add_triangle(const Vertex *v) {
    if (kRingVertices - g_ring_used < 3) return;
    DrawVertex *o = g_ring + g_ring_used;
    for (int i = 0; i < 3; ++i) write_vertex(o + i, gs_x(v[i]), gs_y(v[i]), v[i], g_iip ? v[i] : v[2], v[i]);
    if (!g_textured) o[0].s = o[1].s = o[2].s = o[0].t = o[1].t = o[2].t = 0.0f;
    g_ring_used += 3;
}

void add_quad(float x0, float y0, float x1, float y1, const Vertex &a, const Vertex &b) {
    if (kRingVertices - g_ring_used < 6) return;
    DrawVertex *o = g_ring + g_ring_used;
    // Top left, bottom left, bottom right, top left, bottom right, top right. Texture coordinates follow the two corners.
    Vertex tl = a, br = b;
    tl.q = br.q = b.q;
    write_vertex(o + 0, x0, y0, b, b, tl);
    write_vertex(o + 3, x0, y0, b, b, tl);
    write_vertex(o + 2, x1, y1, b, b, br);
    write_vertex(o + 4, x1, y1, b, b, br);
    write_vertex(o + 1, x0, y1, b, b, tl);
    o[1].t = o[2].t;
    write_vertex(o + 5, x1, y0, b, b, br);
    o[5].t = o[0].t;
    g_ring_used += 6;
}

void show_target(Target *t, const DisplayRegs &r, int circuit) {
    const std::uint64_t disp = r.display[circuit];
    const std::uint32_t magh = (static_cast<std::uint32_t>(disp >> 23) & 0xFu) + 1, magv = (static_cast<std::uint32_t>(disp >> 27) & 3u) + 1;
    const float dw = static_cast<float>((static_cast<std::uint32_t>(disp >> 32) & 0xFFFu) + 1) / static_cast<float>(magh);
    float dh = static_cast<float>((static_cast<std::uint32_t>(disp >> 44) & 0x7FFu) + 1) / static_cast<float>(magv);
    if (dw < 1.0f || dh < 1.0f) return;
    C3D_FrameDrawOn(g_screen);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_uniform_scale, 1.0f, 1.0f, 1.0f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_uniform_offset, 0.0f, 0.0f, 0.0f, 0.0f);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_TexSetFilter(&t->tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&t->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexBind(0, &t->tex);
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_TexEnvInit(C3D_GetTexEnv(1));
    set_fog(false, 0);

    if (kRingVertices - g_ring_used < 6) return;
    const float u1 = dw * t->sx / t->tex.width, v1 = 1.0f - dh * t->sy / t->tex.height;
    // The top screen is mounted sideways: the screen's left edge is the buffer's top.
    const float px[4] = {1.0f, 1.0f, -1.0f, -1.0f};    // screen top left, top right, bottom left, bottom right
    const float py[4] = {1.0f, -1.0f, 1.0f, -1.0f};
    const float su[4] = {0.0f, u1, 0.0f, u1};
    const float sv[4] = {1.0f, 1.0f, v1, v1};
    const int order[6] = {0, 1, 2, 2, 1, 3};
    DrawVertex *o = g_ring + g_ring_used;
    for (int i = 0; i < 6; ++i) {
        const int k = order[i];
        o[i] = DrawVertex{px[k], py[k], -0.5f, 1.0f, 255, 255, 255, 255, su[k], sv[k], 255, {0, 0, 0}};
    }
    g_batch_first = g_ring_used;
    g_ring_used += 6;
    flush_batch();
}

u8 *g_shot_pixels = nullptr;

void save_shot() {
    u8 *pixels = g_shot_pixels;
    if (!pixels) return;
    const u32 dims = GX_BUFFER_DIM(240, 400);
    const u32 flags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                      GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    C3D_SyncDisplayTransfer(static_cast<u32 *>(g_screen->frameBuf.colorBuf), dims, reinterpret_cast<u32 *>(pixels), dims, flags);
    GSPGPU_InvalidateDataCache(pixels, 240 * 400 * 3);
    char name[64];
    std::snprintf(name, sizeof name, "sdmc:/p3fes/shots/frame_%05llu.ppm", static_cast<unsigned long long>(g_frame_number));
    std::FILE *f = std::fopen(name, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n400 240\n255\n");
    static u8 row[400 * 3];
    for (int y = 0; y < 240; ++y) {
        for (int x = 0; x < 400; ++x) {
            const u8 *p = pixels + (x * 240 + (239 - y)) * 3;
            row[x * 3 + 0] = p[2];
            row[x * 3 + 1] = p[1];
            row[x * 3 + 2] = p[0];
        }
        std::fwrite(row, 1, sizeof row, f);
    }
    std::fclose(f);
}

// The GPU starts on a picture only when C3D_FrameEnd runs, and C3D_FrameBegin then waits until it has finished. So the next frame
// opens when something is next drawn: until then the skipped pictures carry on while the GPU works, and nothing waits for a vblank.
void begin_frame() {
    C3D_FrameBegin(0);
    g_frame_open = true;
    GPUCMD_GetBuffer(&g_frame_commands, nullptr, nullptr);
    g_ring_used = g_batch_first = g_flushed_to = 0;
    g_current = nullptr;
    g_key_valid = false;
    ++g_frame;
}

}

bool active() { return g_active; }
void set_present_every(std::uint32_t vblanks) { g_present_every = vblanks; }
void set_shot_every(std::uint32_t frames) { g_shot_every = frames; }
void set_draw_every(std::uint32_t pictures) { g_draw_every = pictures; }
void set_stats(bool on) { g_stats = on; }
void set_dump_frame(std::uint64_t frame) { g_dump_frame = frame; }
void set_hide(std::uint32_t from, std::uint32_t to) { g_hide_from = from, g_hide_to = to; }
void set_skip_attr(std::uint32_t attr) { g_skip_attr = attr; }
void set_debug(std::uint32_t bits) { g_debug = bits; }

bool init() {
    if (!C3D_Init(kCommandBuffer)) {
        std::fprintf(stderr, "[pica] could not start the GPU\n");
        return false;
    }
    g_dvlb = DVLB_ParseFile(reinterpret_cast<u32 *>(const_cast<u8 *>(gs_v_shbin)), gs_v_shbin_size);
    shaderProgramInit(&g_program);
    shaderProgramSetVsh(&g_program, &g_dvlb->DVLE[0]);
    C3D_BindProgram(&g_program);
    g_uniform_scale = shaderInstanceGetUniformLocation(g_program.vertexShader, "scale");
    g_uniform_offset = shaderInstanceGetUniformLocation(g_program.vertexShader, "offset");

    AttrInfo_Init(&g_attr);
    AttrInfo_AddLoader(&g_attr, 0, GPU_FLOAT, 4);
    AttrInfo_AddLoader(&g_attr, 1, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(&g_attr, 2, GPU_FLOAT, 2);
    AttrInfo_AddLoader(&g_attr, 3, GPU_UNSIGNED_BYTE, 4);
    C3D_SetAttrInfo(&g_attr);

    g_ring = static_cast<DrawVertex *>(linearAlloc(kRingBytes));
    if (!g_ring) {
        std::fprintf(stderr, "[pica] no memory for vertices\n");
        return false;
    }
    g_shot_pixels = static_cast<u8 *>(linearAlloc(240 * 400 * 3));
    BufInfo_Init(&g_buf);
    BufInfo_Add(&g_buf, g_ring, sizeof(DrawVertex), 4, 0x3210);
    C3D_SetBufInfo(&g_buf);

    g_depth = vramAlloc(C3D_CalcDepthBufSize(kTargetWidth, kTargetHeight, GPU_RB_DEPTH24));
    g_screen = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, -1);
    if (!g_depth || !g_screen) {
        std::fprintf(stderr, "[pica] no video memory\n");
        return false;
    }
    C3D_RenderTargetSetOutput(g_screen, GFX_TOP, GFX_LEFT,
                              GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                  GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    C3D_CullFace(GPU_CULL_NONE);

    for (int i = 0; i < 64; ++i) {
        int x = 0, y = 0;
        for (int b = 0; b < 3; ++b) {
            x |= ((i >> (b * 2)) & 1) << b;
            y |= ((i >> (b * 2 + 1)) & 1) << b;
        }
        g_morton_x[i] = static_cast<std::uint8_t>(x);
        g_morton_y[i] = static_cast<std::uint8_t>(y);
    }
    if (!C3D_TexInit(&g_fog_ramp, 256, 8, GPU_A8)) return false;
    C3D_TexSetFilter(&g_fog_ramp, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&g_fog_ramp, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    for (int tile = 0; tile < 32; ++tile)
        for (int i = 0; i < 64; ++i) static_cast<std::uint8_t *>(g_fog_ramp.data)[tile * 64 + i] = static_cast<std::uint8_t>(tile * 8 + g_morton_x[i]);
    C3D_TexFlush(&g_fog_ramp);
    g_active = true;
    begin_frame();
    std::fprintf(stderr, "[pica] ready, %lu KB of linear memory free, %lu KB of video memory free\n", static_cast<unsigned long>(linearSpaceFree() >> 10),
                 static_cast<unsigned long>(vramSpaceFree() >> 10));
    return true;
}

void shutdown() {
    if (!g_active) return;
    g_active = false;
    if (g_frame_open) C3D_FrameEnd(0);
    C3D_Fini();
}

void draw(const State &st, const Vertex *v, int count) {
    if (g_draw_nothing || queue::g_quiet) return;
    if (!g_frame_open) begin_frame();
    // Every register that can change the batch moves g_setup_generation, so a run of shapes checks just this one number.
    if (!g_key_valid || g_setup_seen != g_setup_generation) {
        g_setup_seen = g_setup_generation;
        std::uint64_t attr = st.prim;
        if (!(st.prmodecont & 1u)) attr = (st.prim & 7u) | (st.prmode & ~7ull);
        ensure_state(st, st.ctx[(attr >> 9) & 1u], attr);
    }
    if (g_skip_batch) return;
    ++g_stat_shapes;
    ++g_stat_prim[st.prim & 7u];
    ++g_shapes_since_present;
    if (kRingVertices - g_ring_used < 8) {
        if (!g_ring_warned) std::fprintf(stderr, "[pica] vertex ring full, dropping shapes\n");
        g_ring_warned = true;
        return;
    }
    switch (st.prim & 7u) {
    case 0: {
        const float x = gs_x(v[0]), y = gs_y(v[0]);
        add_quad(x, y, x + 1.0f, y + 1.0f, v[0], v[0]);
        break;
    }
    case 1: case 2: {
        const float x0 = gs_x(v[0]), y0 = gs_y(v[0]), x1 = gs_x(v[1]), y1 = gs_y(v[1]);
        add_quad(std::min(x0, x1), std::min(y0, y1), std::max(x0, x1) + 1.0f, std::max(y0, y1) + 1.0f, v[0], v[1]);
        break;
    }
    case 3: case 4: case 5:
        if (count >= 3) add_triangle(v);
        break;
    case 6:
        add_quad(gs_x(v[0]), gs_y(v[0]), gs_x(v[1]), gs_y(v[1]), v[0], v[1]);
        break;
    default:
        break;
    }
}

void note_data(const std::uint8_t *data, std::size_t bytes) {
    if (!g_uploading) return;
    using word = std::uint32_t __attribute__((may_alias, aligned(4)));
    const word *w = reinterpret_cast<const word *>(data);
    std::uint32_t h = g_upload_hash[g_upload_target];
    for (std::size_t n = bytes / 8; n; --n, w += 2) h = (((h << 5) | (h >> 27)) ^ w[0] ^ ((w[1] << 11) | (w[1] >> 21))) * 0x9E3779B1u;
    g_upload_hash[g_upload_target] = h ? h : 1u;
    g_stat_upload += static_cast<std::uint32_t>(bytes);
}

void note_transfer(const State &s, std::uint64_t dir) {
    g_uploading = false;
    if ((dir & 3u) == 1u) return;   // a read from GS memory changes nothing
    const std::uint32_t dbp = static_cast<std::uint32_t>(s.bitbltbuf >> 32) & 0x3FFFu;
    const std::uint32_t dbw = static_cast<std::uint32_t>(s.bitbltbuf >> 48) & 0x3Fu;
    const std::uint32_t dpsm = static_cast<std::uint32_t>(s.bitbltbuf >> 56) & 0x3Fu;
    const std::uint32_t dy = static_cast<std::uint32_t>(s.trxpos >> 48) & 0x7FFu;
    const std::uint32_t rrh = static_cast<std::uint32_t>(s.trxreg >> 32) & 0xFFFu;
    const std::uint32_t height = static_cast<std::uint32_t>(page_height(dpsm));
    const std::uint32_t wide = std::max<std::uint32_t>(std::max<std::uint32_t>(dbw, 1u) * 64u / static_cast<std::uint32_t>(page_width(dpsm)), 1u);
    const std::uint32_t pages = ((dy + rrh) / height + 1u) * wide;
    const std::uint32_t first = dbp / 32u;
    if ((dir & 3u) == 0u) {
        // A transfer to the corner starts a new picture; later ones add to it.
        const std::uint32_t pos = static_cast<std::uint32_t>(s.trxpos >> 32);
        if (!pos) g_upload_hash[dbp] = 0x811C9DC5u;
        g_upload_hash[dbp] ^= pos * 0x01000193u + static_cast<std::uint32_t>(s.trxreg) + static_cast<std::uint32_t>(s.trxreg >> 32) * 31u;
        g_upload_target = dbp;
        g_uploading = true;
    } else {
        g_upload_hash[dbp] = 0;
    }
    ++g_stamp;
    for (std::uint32_t p = first; p < first + pages && p < 512u; ++p) g_page_stamp[p] = g_stamp;
}

void note_clut() { ++g_clut_stamp; }

namespace {

// With the game on one core and the drawing on another, a queue of about one picture sits between them. The game is held up only
// when that queue is full, so time spent waiting on either side of it says which side is short of time. One drawn picture and the
// skipped ones after it make a round: when the game had to wait, the next round skips one more, up to one drawn in four, and when
// the drawing side sat idle it skips one fewer.
void choose_next_picture() {
    constexpr std::uint32_t kMostSkipped = 3;
    ++g_picture;
    bool skip = false;
    if (g_draw_every > 1) {
        skip = g_picture % g_draw_every != 0;
    } else if (!g_draw_every && queue::vu_active()) {
        if (g_skipped_in_row >= g_skip_now) {
            const u64 now = svcGetSystemTick();
            const u64 round = now - g_round_start, tick_share = round / 8;
            if (g_round_blocked > tick_share && g_skip_run < kMostSkipped) ++g_skip_run;
            else if (g_round_idle > tick_share * 2 && g_skip_run) --g_skip_run;
            g_round_start = now;
            g_round_blocked = g_round_idle = 0;
            g_skipped_in_row = 0;
            // Most scenes swap between two frame buffers, and some blend the last picture into the next. Skipping an odd number of
            // pictures every time would draw only into one buffer, and the other's old contents would show through as noise. So
            // an odd run alternates one less and one more, which averages the same and draws into each buffer in turn.
            g_skip_longer = !g_skip_longer;
            g_skip_now = g_skip_run & 1u ? g_skip_run + (g_skip_longer ? 1u : 0u) - (g_skip_longer ? 0u : 1u) : g_skip_run;
        } else {
            skip = true;
        }
    }
    g_skipped_in_row = skip ? g_skipped_in_row + 1 : 0;
    g_stat_skipped += skip;
    const bool test_aid = skip && (g_debug & 4u);   // skip the drawing but still run the vector programs
    queue::g_ee_skip = skip && !test_aid;
    g_next_picture = (skip && !test_aid ? kSkip : 0u) | (test_aid ? kDrawNothing : 0u) | (g_skip_run || g_draw_every > 1 ? kSkipping : 0u);
}

void show_picture(std::uint32_t frame, const DisplayRegs &r);

}

// The game thread decides, as it starts each picture, so it can leave out what a skipped picture will not use.
std::uint32_t next_picture() {
    if (!g_present_every || ++g_game_vblanks % g_present_every) return g_next_picture;
    using namespace ps2::prof;
    const u64 blocked = g_ticks[Full], idle = g_ticks[Wait];   // the stats line clears these
    if (blocked > g_seen_blocked) g_round_blocked += blocked - g_seen_blocked;
    if (idle > g_seen_idle) g_round_idle += idle - g_seen_idle;
    g_seen_blocked = blocked;
    g_seen_idle = idle;
    choose_next_picture();
    return g_next_picture;
}

void vblank(std::uint32_t frame, const DisplayRegs &r, std::uint32_t picture) {
    g_frame_number = frame;
    ++g_vblanks;
    if (!g_present_every || g_vblanks % g_present_every) return;
    if (g_shapes_since_present) show_picture(frame, r);
    g_skipping = picture & kSkipping;
    g_draw_nothing = picture & kDrawNothing;
    queue::g_skip_picture = picture & kSkip;
}

namespace {

void show_picture(std::uint32_t frame, const DisplayRegs &r) {
    PS2_PROF(ps2::prof::Present);
    const int circuit = (r.pmode & 2u) ? 1 : (r.pmode & 1u) ? 0 : -1;
    flush_batch();
    if (circuit >= 0) {
        const std::uint32_t fbp = static_cast<std::uint32_t>(r.dispfb[circuit]) & 0x1FFu;
        for (Target &t : g_targets)
            if (t.used && t.fbp == fbp) {
                // Many scenes swap between two frame buffers, drawing into the one not on show. Drawing one picture in two or four
                // would then always land on the same buffer, and the one on show would never change. So when pictures are
                // being skipped, a buffer of the same size that was just drawn goes on show instead.
                Target *show = &t;
                if (g_skipping && t.last_used != g_frame)
                    for (Target &o : g_targets)
                        if (o.used && o.last_used == g_frame && o.tex.width == t.tex.width && o.tex.height == t.tex.height) show = &o;
                show_target(show, r, circuit);
                break;
            }
    }
    flush_ring();
    if (g_stats) {
        static u64 last = svcGetSystemTick();
        const u64 now = svcGetSystemTick();
        const auto ms = [](u64 ticks) { return static_cast<unsigned long>(ticks / static_cast<u64>(CPU_TICKS_PER_MSEC)); };
        using namespace ps2::prof;
        const u64 total = now - last;
        u64 known = g_ticks[Vu] + g_ticks[Upload] + g_ticks[Iop] + g_ticks[Ipu];   // Gif is inside Vu
        static std::uint64_t last_interpreted = 0, last_clock = 0, last_idle = 0;
        static std::uint32_t last_frame = 0;
        std::fprintf(stderr, "[pica] %lu game frames in %lu ms, %lu ms each, %lu pictures skipped\n", static_cast<unsigned long>(frame - last_frame), ms(total), frame != last_frame ? ms(total) / static_cast<unsigned long>(frame - last_frame) : 0ul, static_cast<unsigned long>(g_stat_skipped));
        last_frame = frame;
        g_stat_skipped = 0;
        const std::uint64_t interpreted = ps2::vu1_interpreted_calls();
        const std::uint64_t clock = ps2::now(), idled = ps2::idle_cycles();
        std::fprintf(stderr, "[pica] ee %llu K cycles, %llu K idle | prims pt %lu ln %lu strip %lu tri %lu tstrip %lu fan %lu sprite %lu | upload %lu KB\n", static_cast<unsigned long long>((clock - last_clock) / 1000u), static_cast<unsigned long long>((idled - last_idle) / 1000u),
                     static_cast<unsigned long>(g_stat_prim[0]), static_cast<unsigned long>(g_stat_prim[1]), static_cast<unsigned long>(g_stat_prim[2]), static_cast<unsigned long>(g_stat_prim[3]), static_cast<unsigned long>(g_stat_prim[4]), static_cast<unsigned long>(g_stat_prim[5]), static_cast<unsigned long>(g_stat_prim[6]), static_cast<unsigned long>(g_stat_upload >> 10));
        std::fprintf(stderr, "[pica] vif %lu (with vu) dma %lu events %lu (with iop, present) kernel %lu present %lu | vu thread idle %lu, ee thread blocked %lu\n", ms(g_ticks[Vif]), ms(g_ticks[Dma]), ms(g_ticks[Events]), ms(g_ticks[Kernel]), ms(g_ticks[Present]), ms(g_ticks[Wait]), ms(g_ticks[Full]));
        last_clock = clock;
        last_idle = idled;
        std::fprintf(stderr, "[pica] frame %llu: %lu ms: game %lu vu %lu (gif %lu of it, interpreted calls %lu) upload %lu draw %lu iop %lu (spu %lu) ipu %lu | %lu shapes %lu draws %lu changes %lu splits %lu decoded %lu texels %lu reused %lu missing %lu verts, %lu KB free\n",
                     static_cast<unsigned long long>(frame), ms(total), ms(total > known ? total - known : 0), ms(g_ticks[Vu]), ms(g_ticks[Gif]), static_cast<unsigned long>(interpreted - last_interpreted), ms(g_ticks[Upload]), ms(g_ticks[Draw]), ms(g_ticks[Iop]),
                     ms(g_ticks[Spu]), ms(g_ticks[Ipu]), static_cast<unsigned long>(g_stat_shapes), static_cast<unsigned long>(g_stat_draws), static_cast<unsigned long>(g_stat_state),
                     static_cast<unsigned long>(g_stat_splits), static_cast<unsigned long>(g_stat_decoded), static_cast<unsigned long>(g_stat_decode_texels), static_cast<unsigned long>(g_stat_reused), static_cast<unsigned long>(g_stat_missing),
                     static_cast<unsigned long>(g_ring_used), static_cast<unsigned long>(linearSpaceFree() >> 10));
        last_interpreted = interpreted;
        std::memset(g_ticks, 0, sizeof g_ticks);
        last = svcGetSystemTick();
    }
    g_batch_number = 0;
    g_stat_reused = g_stat_missing = 0;
    g_stat_shapes = g_stat_draws = g_stat_state = g_stat_splits = g_stat_decoded = g_stat_decode_texels = 0;
    g_stat_decode_ticks = 0;
    g_shapes_since_present = 0;
    std::memset(g_stat_prim, 0, sizeof g_stat_prim);
    g_stat_upload = 0;
    // With no flags, C3D_FrameEnd would flush the whole of linear memory. Textures and the vertex ring are flushed as they are
    // written, so only the commands are left.
    u32 *commands;
    u32 offset;
    GPUCMD_GetBuffer(&commands, nullptr, &offset);
    GSPGPU_FlushDataCache(g_frame_commands, static_cast<u32>(commands + offset - g_frame_commands) * sizeof(u32));
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    g_frame_open = false;
    if (g_shot_every && frame % g_shot_every < g_present_every) {
        begin_frame();   // the picture has to be finished before it can be copied
        save_shot();
    }
}

}

}
