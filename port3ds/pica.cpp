// PS2 GS shapes on the New 3DS GPU (PICA200), through citro3d.
//
// The PS2 draws one shape at a time and the GPU is happiest with long runs of triangles, so shapes collect in a batch until a
// GS setting changes. Each PS2 frame buffer gets a render target, keyed by its address, so page flipping and simple
// render-to-texture work. The PICA has no palettes, so textures are decoded from GS memory into a cache.
//
// Differences from the real GS that are accepted for now: no destination alpha test, no dithering, no mipmaps, and reads
// of the frame buffer by the game see old GS memory. Wrap modes with a region clamp to the edge of the whole texture.
//
// A full PS2 picture is drawn at the top screen's own 400x240, so it goes up without scaling. With the 3D slider up, such a
// target has a separate texture for each eye. Every batch is drawn twice, shifted sideways by its
// depth (see the "eye" uniform in gs.v.pica).
#include "pica.hpp"
#include "g3model.hpp"
#include "movie3ds.hpp"
#include "prof.hpp"

#include "gs/gs.hpp"
#include "gs/queue.hpp"
#include "ps2/system.hpp"

#include <3ds.h>
#include <citro3d.h>

#include "gs_v_shbin.h"

#include <algorithm>
#include <atomic>
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
std::atomic<std::uint32_t> g_ticks[Count];
}

extern "C" bool FesOverlapInit(std::size_t);
extern "C" bool FesOverlapBegin(bool, bool);
extern "C" void FesOverlapCommandStats(u32*, unsigned*);
extern "C" void FesOverlapWait();
extern "C" u64 FesOverlapWaitTicks();
extern "C" u32 FesOverlapWordsLeft();
extern "C" void FesOverlapEnd(u8);

namespace movie { bool overlap_needs_wait(); }

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
constexpr int kScreenWidth = 400, kScreenHeight = 240;
constexpr int kMaxTargets = 8;
constexpr int kMaxTextures = 512;
constexpr std::size_t kCommandBuffer = 0x100000;
constexpr std::size_t kLinearReserve = 256u * 1024u;

struct Target {
    bool used = false;
    std::uint32_t fbp = 0;   // in pages
    int gs_width = 0, gs_height = 0;
    float sx = 1.0f, sy = 1.0f;
    int width = 0;       // of one eye's picture in the texture
    C3D_Tex tex;
    C3D_Tex right_tex;
    C3D_RenderTarget *rt = nullptr;
    C3D_RenderTarget *right_rt = nullptr;
    std::uint32_t last_used = 0;
};

struct Texture {
    bool used = false;
    std::uint32_t tbp = 0, tbw = 0, psm = 0, tw = 0, th = 0, clut = 0, texa = 0;
    int half = 0;
    int uniform_alpha = -2;   // -2 not checked, -1 varies
    std::uint32_t stamp = 0, hash = 0, sample = 0;
    std::uint32_t sample_stamp = 0, sample_tbp = UINT32_MAX, sample_tbw = 0;
    std::uint32_t last_used = 0;
    C3D_Tex tex;
};

bool g_active = false;
bool g_frame_open = false;
DVLB_s *g_dvlb = nullptr;
shaderProgram_s g_program;
C3D_AttrInfo g_attr;
C3D_BufInfo g_buf;
int g_uniform_scale = -1, g_uniform_offset = -1, g_uniform_eye = -1;
// The g3 half of the shader: proj to lcol are 15 registers in a row, then bones; isg3 picks the half.
int g_uniform_proj = -1, g_uniform_bones = -1, g_bool_g3 = -1, g_bool_skin = -1, g_bool_ortho = -1;
C3D_AttrInfo g_g3_attr;
// What is set for g3 batches: their vertex layout and shader half (until flush_batch puts the GS ones back), the buffer they
// are bound at, the skin half, and the culling (the GS path draws with none).
bool g_g3_bound = false, g_g3_skin = false, g_g3_ortho = false;
bool g_unif_valid = false;
std::uint32_t g_unif_bits[15][4]{};
std::uint32_t g_pica_bone_gen[::g3::kGpuBones]{};

struct HwCache {
    bool texenv_valid = false, blend_valid = false, tests_valid = false, tests_skip = false;
    bool scissor_valid = false, offset_valid = false, eye_valid = false, clip_valid = false;
    bool tex_bind_valid = false;
    C3D_Tex *bound_tex0 = nullptr;
    std::uint8_t bound_tex_flags = 0;
    std::uint64_t texenv_key = 0;
    bool blend_abe = false;
    std::uint64_t blend_alpha = 0;
    std::uint64_t tests_test = 0, tests_zbuf = 0;
    std::uint32_t tests_fbmsk = 0;
    C3D_RenderTarget *drawn_rt = nullptr;
    u32 applied_scissor[4]{};
    float applied_shift = 0.0f;
    const Target *clip_target = nullptr;
    std::uint64_t clip_scissor_reg = 0;
    int clip_ofx = 0, clip_ofy = 0;
    float clip_eye_now = 0.0f;
    void invalidate() {
        texenv_valid = blend_valid = tests_valid = false;
        scissor_valid = offset_valid = eye_valid = clip_valid = false;
        tex_bind_valid = false;
        bound_tex0 = nullptr;
        drawn_rt = nullptr;
    }
} g_hw;
const void *g_g3_buffer = nullptr;
GPU_CULLMODE g_g3_cull = GPU_CULL_NONE;

DrawVertex *g_ring = nullptr;
std::uint16_t *g_indices = nullptr;
std::uint32_t g_ring_used = 0, g_flushed_to = 0;
std::uint32_t g_index_used = 0, g_index_first = 0, g_index_flushed = 0;
std::uint32_t g_index_base = 0, g_bound_base = 0;

constexpr std::uint32_t kSliceVertices = kRingVertices / 2;
static_assert((kSliceVertices * sizeof(std::uint16_t)) % 32 == 0);
DrawVertex *g_vertex_storage = nullptr;
std::uint16_t *g_index_storage = nullptr;
std::uint32_t g_slice_capacity = kRingVertices;
unsigned g_slices = 3, g_slice = 0;
bool g_overlap_ready = false, g_overlap_picture = false;
unsigned g_overflow_waits = 0;
u64 g_profile_begin = 0;

C3D_RenderTarget *g_screen = nullptr, *g_screen_right = nullptr;
void *g_depth = nullptr, *g_right_depth = nullptr;
// Stereo: g_stereo says new full-size targets have a second eye. g_eye_shift is the sideways shift, in clip units, between the
// nearest and the farthest depth; g_eye_now is that for the batch being collected, zero for flat shapes such as menus.
bool g_stereo = false, g_two_screens = false;
std::uint32_t g_stereo_strength = 5;
float g_eye_shift = 0.0f, g_eye_now = 0.0f;
// The depth that sits on the screen's surface: the scene lies behind it, and anything nearer (the game draws some of its panels
// with a depth test, right at the front) stays on it. Measured in the lobby, the dorm and a battle: nearly all of a scene's
// corners fall in the far three sixteenths of the depth range.
constexpr float kEyeDepth = -0.1875f;
u32 g_scissor[4];
int g_clip_left, g_clip_right, g_clip_top, g_clip_bottom;
bool g_in_depth = false;
std::uint32_t g_depth_seen[16];   // test aid ("debug 8"): how many 3D triangle corners fell in each sixteenth of the depth range
Target g_targets[kMaxTargets];
Target *g_current = nullptr, *g_source = nullptr;   // drawn on, and read as the texture, by the batch being collected
float g_bound_sx = 0.0f, g_bound_sy = 0.0f;
Texture g_textures[kMaxTextures];
constexpr unsigned kTextureHashBuckets = 256;
std::uint16_t g_texture_hash_head[kTextureHashBuckets], g_texture_hash_next[kMaxTextures];

std::uint8_t g_morton_x[64], g_morton_y[64];
std::uint32_t g_page_stamp[512];
std::uint32_t g_stamp = 1;
std::uint32_t g_clut_stamp = 1;
// A hash of what was last uploaded to each GS block address. This identifies a texture exactly, where a hash of whole pages
// changes whenever a neighbour sharing the page does.
std::uint32_t g_upload_hash[16384];
// An upload to a block address can come in pieces (a texture page sent as several rectangles). g_upload_stamp[dbp] is the
// stamp of the piece at the corner that began the current upload there, and every block keeps the start stamp of the upload
// that last wrote it (0 for anything else), so a small texture inside the first piece is known to be that upload's.
std::uint32_t g_upload_stamp[16384];
std::uint32_t g_block_stamp[16384];
std::uint32_t g_block_upload[16384];
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
Texture *g_alpha_texture = nullptr;
bool g_alpha_from_texture = false;
int g_alpha_function = 0;
float g_u_scale = 1.0f, g_v_scale = 1.0f, g_v_offset = 0.0f;
float g_s_scale = 1.0f, g_t_scale = 1.0f;
bool g_fst = false, g_iip = false;
int g_ofx = 0, g_ofy = 0;
bool g_skip_batch = false;

// Counters for tuning.
std::uint32_t g_stat_reused = 0, g_stat_missing = 0;
std::uint32_t g_stat_g3 = 0, g_stat_g3_declined = 0, g_stat_unif = 0;
std::uint32_t g_words_setup = 0, g_words_flush = 0, g_words_bind = 0, g_words_draw = 0;   // command words by part (test aid)
inline std::uint32_t cmd_words() { return gpuCmdBufOffset; }   // g3 batches drawn, and handed back for want of command list room
std::uint32_t g_stat_draws = 0, g_stat_shapes = 0, g_stat_state = 0, g_stat_decoded = 0, g_stat_decode_texels = 0, g_stat_splits = 0;
std::uint32_t g_shapes_since_present = 0;
std::uint32_t g_setup_seen = 0;
std::uint32_t g_last_triangle_serial = 0, g_last_triangle_setup = 0, g_last_triangle_end = 0;
std::uint32_t g_last_triangle_colour = 0;
constexpr std::uint32_t kSkip = 1, kSkipping = 2, kDrawNothing = 4;
std::uint32_t g_next_picture = 0, g_game_vblanks = 0;
std::uint32_t g_draw_every = 0, g_picture = 0, g_skipped_in_row = 0, g_skip_run = 0;
std::atomic<std::uint32_t> g_stat_skipped{0}, g_stat_forced{0};
u64 g_round_start = 0, g_round_blocked = 0, g_round_idle = 0;
std::uint32_t g_seen_blocked = 0, g_seen_idle = 0;
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
    if (g_index_used > g_index_flushed) {
        GSPGPU_FlushDataCache(g_indices + g_index_flushed, (g_index_used - g_index_flushed) * sizeof(*g_indices));
        g_index_flushed = g_index_used;
    }
}

void set_eye(bool right);

// Return 1 if the whole batch passes, 0 if it fails, and -1 if it needs both draws.
// Only classify constant alpha inputs with an unambiguous combiner result.
int uniform_alpha_pass(const std::uint16_t *indices, std::uint32_t count) {
    int texel = 0;
    if (g_alpha_from_texture) {
        if (!g_alpha_texture) return -1;   // another render target
        int &alpha = g_alpha_texture->uniform_alpha;
        if (alpha == -2) {
            const auto &tex = g_alpha_texture->tex;
            const auto *pixels = static_cast<const std::uint32_t *>(tex.data);
            alpha = pixels[0] & 255;
            const std::uint32_t n = static_cast<std::uint32_t>(tex.width) * tex.height;
            for (std::uint32_t i = 1; i < n; ++i) {
                if (static_cast<int>(pixels[i] & 255) == alpha) continue;
                alpha = -1;
                break;
            }
        }
        if (alpha < 0) return -1;
        texel = alpha;
    }
    int vertex = 0;
    if (!g_alpha_from_texture || g_alpha_function == 0 || g_alpha_function == 2) {
        const DrawVertex *base = g_ring + g_index_base;
        vertex = base[indices[0]].a;
        for (std::uint32_t i = 1; i < count; ++i)
            if (base[indices[i]].a != vertex) return -1;
    }
    int low = vertex * 2, high = low;
    if (g_alpha_from_texture) {
        switch (g_alpha_function) {
        case 0: {
            const int product = texel * vertex;
            // Account for quantization before the two combiner scales.
            low = (product >> 8) * 4;
            high = ((product + 254) / 255) * 4;
            break;
        }
        case 2: low = high = 2 * (texel + vertex); break;
        default: low = high = 2 * texel; break;
        }
    }
    low = std::min(low, 255);
    high = std::min(high, 255);
    const int reference = g_split.reference;
    switch (g_split.pass) {
    case GPU_LESS: return high < reference ? 1 : low >= reference ? 0 : -1;
    case GPU_LEQUAL: return high <= reference ? 1 : low > reference ? 0 : -1;
    case GPU_EQUAL:
    case GPU_NOTEQUAL:
        if (low == high || reference < low || reference > high)
            return (low == reference) == (g_split.pass == GPU_EQUAL);
        return -1;
    case GPU_GEQUAL: return low >= reference ? 1 : high < reference ? 0 : -1;
    case GPU_GREATER: return low > reference ? 1 : high <= reference ? 0 : -1;
    default: return -1;
    }
}

void flush_g3();

void flush_batch() {
    PS2_PROF_EXTRA(1);
    flush_g3();
    const std::uint32_t count = g_index_used - g_index_first;
    if (!count) return;
    if (g_g3_bound) {
        C3D_SetAttrInfo(&g_attr);
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, g_bool_g3, false);
        if (g_g3_cull != GPU_CULL_NONE) C3D_CullFace(GPU_CULL_NONE);
        g_g3_cull = GPU_CULL_NONE;
        g_g3_bound = false;
        g_g3_buffer = nullptr;
        g_bound_base = UINT32_MAX;
    }
    if (g_bound_base != g_index_base) {
        BufInfo_Init(&g_buf);
        BufInfo_Add(&g_buf, g_ring + g_index_base, sizeof(DrawVertex), 4, 0x3210);
        C3D_SetBufInfo(&g_buf);
        g_bound_base = g_index_base;
    }
    const auto *indices = g_indices + g_index_first;
    const int pass = g_split.on ? uniform_alpha_pass(indices, count) : -1;
    const bool split = g_split.on && pass < 0;
    if (pass >= 0) {
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_DepthTest(g_split.depth, g_split.depth_test,
                      static_cast<GPU_WRITEMASK>(pass ? g_split.pass_writes : g_split.fail_writes));
    }
    if (split) {
        C3D_AlphaTest(true, g_split.pass, g_split.reference);
        C3D_DepthTest(g_split.depth, g_split.depth_test, static_cast<GPU_WRITEMASK>(g_split.pass_writes));
        C3D_DrawElements(GPU_TRIANGLES, static_cast<int>(count), C3D_UNSIGNED_SHORT, indices);
        C3D_AlphaTest(true, g_split.fail, g_split.reference);
        C3D_DepthTest(g_split.depth, g_split.depth_test, static_cast<GPU_WRITEMASK>(g_split.fail_writes));
    }
    C3D_DrawElements(GPU_TRIANGLES, static_cast<int>(count), C3D_UNSIGNED_SHORT, indices);
    if (g_current && g_current->right_rt) {
        set_eye(true);
        if (split) {
            C3D_AlphaTest(true, g_split.pass, g_split.reference);
            C3D_DepthTest(g_split.depth, g_split.depth_test, static_cast<GPU_WRITEMASK>(g_split.pass_writes));
            C3D_DrawElements(GPU_TRIANGLES, static_cast<int>(count), C3D_UNSIGNED_SHORT, indices);
            C3D_AlphaTest(true, g_split.fail, g_split.reference);
            C3D_DepthTest(g_split.depth, g_split.depth_test, static_cast<GPU_WRITEMASK>(g_split.fail_writes));
        }
        C3D_DrawElements(GPU_TRIANGLES, static_cast<int>(count), C3D_UNSIGNED_SHORT, indices);
        set_eye(false);
    }
    g_index_first = g_index_used;
    ++g_stat_draws;
    if (C3D_GetCmdBufUsage() > 0.75f) {
        flush_ring();
        C3D_FrameSplit(GX_CMDLIST_FLUSH);
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

std::uint32_t g_clut8_stamp = 0, g_clut8_hash = 0;
std::uint32_t g_clut4_stamp[16]{}, g_clut4_hash[16]{};

std::uint32_t clut_hash(const State &st, std::uint32_t psm, std::uint32_t csa) {
    if (psm == PSMT8 || psm == PSMT8H) {
        if (g_clut8_stamp == g_clut_stamp) return g_clut8_hash;
        const std::uint32_t *p = st.clut;
        std::uint32_t h = 2166136261u;
        for (std::uint32_t i = 0; i < 256u; ++i) h = (h ^ p[i]) * 16777619u;
        g_clut8_stamp = g_clut_stamp;
        g_clut8_hash = h;
        return h;
    }
    if (psm == PSMT4 || psm == PSMT4HL || psm == PSMT4HH) {
        const std::uint32_t bank = csa & 15u;
        if (g_clut4_stamp[bank] == g_clut_stamp) return g_clut4_hash[bank];
        const std::uint32_t *p = st.clut + bank * 16u;
        std::uint32_t h = 2166136261u;
        for (std::uint32_t i = 0; i < 16u; ++i) h = (h ^ p[i]) * 16777619u;
        g_clut4_stamp[bank] = g_clut_stamp;
        g_clut4_hash[bank] = h;
        return h;
    }
    return 2166136261u;
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
    PS2_PROF_EXTRA(2);
    const u64 start = svcGetSystemTick();
    const int w = e.tex.width, h = e.tex.height;
    const int gw = 1 << e.tw, gh = 1 << e.th;
    const std::uint32_t psm = e.psm, bp = e.tbp, bw = e.tbw;
    const bool clut16 = !(cpsm == PSMCT32 || cpsm == PSMCT24);
    auto *out = static_cast<std::uint32_t *>(e.tex.data);
    const std::uint32_t *clut = st.clut;
    if ((psm == PSMCT32 || psm == PSMCT24) && e.half <= 1 && (w << e.half) == gw && (h << e.half) == gh) {
        // A 32-bit texture whose size (halved or not) fills its GS area: each 8x8 tile reads one GS page, at offsets that depend only
        // on where the tile sits in the page. The per-texel path costs two software divisions and a call per texel, which made a
        // movie picture take 34 ms.
        static std::uint16_t offsets[2][32][64];
        static bool made = false;
        if (!made) {
            static const std::uint8_t kBlock[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23},
                                                      {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
            static const std::uint8_t kColumn[2][8] = {{0, 1, 4, 5, 8, 9, 12, 13}, {2, 3, 6, 7, 10, 11, 14, 15}};
            for (int half = 0; half < 2; ++half)
                for (int pos = 0; pos < 32; ++pos)
                    for (int i = 0; i < 64; ++i) {
                        const int x = ((pos & 7) * 8 + g_morton_x[i]) << half, y = ((pos >> 3) * 8 + g_morton_y[i]) << half;
                        offsets[half][pos][i] = (x < 64 && y < 32)
                            ? static_cast<std::uint16_t>(kBlock[y / 8][x / 8] * 256 + ((y & 7) / 2) * 64 + kColumn[y & 1][x & 7] * 4) : 0;
                    }
            made = true;
        }
        const std::uint8_t *mem = memory();
        const std::uint32_t per_row = bw ? bw : 1u, alpha = ta0 << 24, mask = kMemorySize - 1u, sh = e.half;
        for (int ty = 0; ty < h; ty += 8) {
            const std::uint32_t sy = static_cast<std::uint32_t>(ty) << sh;
            const std::uint32_t row_page = bp * 256u + (sy / 32u) * per_row * 8192u;
            for (int tx = 0; tx < w; tx += 8) {
                const std::uint32_t sx = static_cast<std::uint32_t>(tx) << sh, page = row_page + (sx / 64u) * 8192u;
                const std::uint16_t *at = offsets[sh][((sy & 31u) >> sh) + ((sx & 63u) >> (3u + sh))];
                std::uint32_t *tile = out + (ty * w + tx * 8);
                for (int i = 0; i < 64; ++i) {
                    std::uint32_t raw;
                    std::memcpy(&raw, mem + ((page + at[i]) & mask), 4);
                    if (psm == PSMCT24) {
                        raw &= 0xFFFFFFu;
                        raw |= (aem && !raw) ? 0u : alpha;
                    }
                    tile[i] = to_gpu(raw);
                }
            }
        }
    } else
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

unsigned texture_hash_bucket(std::uint32_t hash) {
    return (hash ^ (hash >> 16)) & (kTextureHashBuckets - 1);
}

void index_texture(Texture &e) {
    if (!e.hash) return;
    const auto slot = static_cast<std::uint16_t>(&e - g_textures + 1);
    auto *link = &g_texture_hash_head[texture_hash_bucket(e.hash)];
    // Keep slot order so collisions and stale samples are tried as before.
    while (*link && *link < slot) link = &g_texture_hash_next[*link - 1];
    g_texture_hash_next[slot - 1] = *link;
    *link = slot;
}

void free_texture(Texture &e) {
    if (g_overlap_picture && e.last_used + 1u == g_frame) FesOverlapWait();
    if (e.hash) {
        const auto slot = static_cast<std::uint16_t>(&e - g_textures + 1);
        auto *link = &g_texture_hash_head[texture_hash_bucket(e.hash)];
        while (*link && *link != slot) link = &g_texture_hash_next[*link - 1];
        if (*link) *link = g_texture_hash_next[slot - 1];
    }
    if (g_hw.bound_tex0 == &e.tex) {
        g_hw.tex_bind_valid = false;
        g_hw.bound_tex0 = nullptr;
    }
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
        for (auto i = g_texture_hash_head[texture_hash_bucket(hash)]; i; i = g_texture_hash_next[i - 1]) {
            Texture &e = g_textures[i - 1];
            if (e.hash == hash && same_picture(e)) {
                // The hash can be stale or shared, so a small texture is checked against GS memory: a wrong one showed as black letters.
                if (tw <= 6 && th <= 6) {
                    std::uint32_t tb0 = tbp, tb1 = tbp + 32u;
                    upload_blocks(psm, tbp, tbw, 0u, 0u, 1u << tw, 1u << th, tb0, tb1);
                    if (tb1 > 16384u) tb1 = 16384u;
                    std::uint32_t newest = 0, nb = tb0;
                    for (std::uint32_t b = tb0; b < tb1; ++b)
                        if (g_block_stamp[b] > newest) newest = g_block_stamp[b], nb = b;
                    const bool own = g_upload_stamp[tbp] && g_block_upload[nb] == g_upload_stamp[tbp];
                    if (!own && (e.sample_stamp != newest || e.sample_tbp != tbp || e.sample_tbw != tbw)) {
                        flush_deferred_pages(pages.first, pages.count);
                        if (e.sample != sample_texels(tbp, tbw, psm, tw, th)) {
                            static int reports = 0;
                            if (reports < 40) {
                                ++reports;
                                std::fprintf(stderr, "[stale] f%llu tbp %lu was tbp %lu psm %02lX hash %08lX\n",
                                             static_cast<unsigned long long>(ps2::frame_count()), static_cast<unsigned long>(tbp),
                                             static_cast<unsigned long>(e.tbp), static_cast<unsigned long>(psm), static_cast<unsigned long>(hash));
                            }
                            continue;
                        }
                        e.sample_stamp = newest;
                        e.sample_tbp = tbp;
                        e.sample_tbw = tbw;
                    }
                }
                e.last_used = g_frame;
                ++g_stat_reused;
                return &e;
            }
        }
        for (Texture &e : g_textures) {
            if (!e.used) {
                slot = &e;
                break;
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
        flush_deferred_pages(pages.first, pages.count);
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
    slot->uniform_alpha = -2;
    slot->clut = clut;
    slot->texa = texa;
    slot->stamp = g_stamp;
    slot->hash = hash;
    slot->sample_stamp = pages_stamp(pages);
    slot->sample_tbp = tbp;
    slot->sample_tbw = tbw;
    slot->last_used = g_frame;
    index_texture(*slot);
    flush_deferred_pages(pages.first, pages.count);
    decode(*slot, st, cpsm, csa, ta0, ta1, aem);
    return slot;
}

int fit_size(int gs_size, int largest) {
    int size = 8;
    while (size < gs_size && size < largest) size *= 2;
    return size;
}

// citro3d stops the program if a render target is deleted while a frame is open, and the frame's commands may still draw on it or
// read it. Keep a target given up mid-frame until the next picture. C3D_RenderTargetDelete waits for queued GPU work. The new
// target needs VRAM of its own meanwhile; if there is none, its shapes are left out for that one frame.
struct Retired {
    C3D_RenderTarget *rt;
    C3D_Tex tex;
};
Retired g_retired[kMaxTargets * 2];
int g_retired_count = 0;

void delete_retired() {
    for (int i = 0; i < g_retired_count; ++i) {
        C3D_RenderTargetDelete(g_retired[i].rt);
        C3D_TexDelete(&g_retired[i].tex);
    }
    g_retired_count = 0;
}

bool release_target(Target &t) {
    if (g_hw.bound_tex0 == &t.tex || g_hw.bound_tex0 == &t.right_tex) {
        g_hw.tex_bind_valid = false;
        g_hw.bound_tex0 = nullptr;
    }
    if (g_frame_open) {
        if (g_retired_count + (t.right_rt ? 2 : 1) > kMaxTargets * 2) return false;
        g_retired[g_retired_count++] = Retired{t.rt, t.tex};
        if (t.right_rt) g_retired[g_retired_count++] = Retired{t.right_rt, t.right_tex};
    } else {
        C3D_RenderTargetDelete(t.rt);
        C3D_TexDelete(&t.tex);
        if (t.right_rt) {
            C3D_RenderTargetDelete(t.right_rt);
            C3D_TexDelete(&t.right_tex);
        }
    }
    t.rt = nullptr;
    t.right_rt = nullptr;
    t.used = false;
    return true;
}

// A target's texture is the true size of the PS2 picture, up to 512x256. A smaller picture sampled as a texture must clamp at
// its own edge, as it does on the GS, not run on into unused texels. A picture wider than the top screen is drawn at the top
// screen's size instead: 640x448 becomes 400x240, which goes up without scaling.
void set_scale(Target &t, int gs_width, int gs_height) {
    t.gs_width = gs_width;
    t.gs_height = gs_height;
    t.sx = std::min(1.0f, static_cast<float>(std::min(t.width, kScreenWidth)) / static_cast<float>(gs_width));
    t.sy = std::min(1.0f, static_cast<float>(std::min<int>(t.tex.height, kScreenHeight)) / static_cast<float>(gs_height));
}

bool create_target(Target &t, std::uint32_t fbp, int gs_width, int gs_height) {
    const int width = fit_size(gs_width, kTargetWidth), height = fit_size(gs_height, kTargetHeight);
    const bool full = width == kTargetWidth && height == kTargetHeight;
    if (!C3D_TexInitVRAM(&t.tex, static_cast<u16>(width), static_cast<u16>(height), GPU_RGBA8)) return false;
    t.rt = C3D_RenderTargetCreateFromTex(&t.tex, GPU_TEXFACE_2D, 0, full ? C3D_DEPTHTYPE(-1) : C3D_DEPTHTYPE(GPU_RB_DEPTH24));
    if (!t.rt) {
        C3D_TexDelete(&t.tex);
        return false;
    }
    if (full) C3D_FrameBufDepth(&t.rt->frameBuf, g_depth, GPU_RB_DEPTH24);
    C3D_RenderTargetClear(t.rt, C3D_CLEAR_ALL, 0, 0);
    if (full && g_stereo) {
        if (!g_right_depth) g_right_depth = vramAlloc(C3D_CalcDepthBufSize(width, height, GPU_RB_DEPTH24));
        if (g_right_depth && C3D_TexInitVRAM(&t.right_tex, static_cast<u16>(width), static_cast<u16>(height), GPU_RGBA8)) {
            t.right_rt = C3D_RenderTargetCreateFromTex(&t.right_tex, GPU_TEXFACE_2D, 0, C3D_DEPTHTYPE(-1));
            if (t.right_rt) {
                C3D_FrameBufDepth(&t.right_rt->frameBuf, g_right_depth, GPU_RB_DEPTH24);
                C3D_RenderTargetClear(t.right_rt, C3D_CLEAR_ALL, 0, 0);
            } else C3D_TexDelete(&t.right_tex);
        }
    }
    t.used = true;
    t.fbp = fbp;
    t.width = width;
    set_scale(t, gs_width, gs_height);
    t.last_used = g_frame;
    return true;
}

Target *find_target(std::uint32_t fbp, int gs_width, int gs_height) {
    Target *free_slot = nullptr, *oldest = nullptr;
    for (Target &t : g_targets) {
        if (t.used && t.fbp == fbp) {
            if (gs_width > t.gs_width || gs_height > t.gs_height) {
                const int w = std::max(gs_width, t.gs_width), h = std::max(gs_height, t.gs_height);
                if (fit_size(w, kTargetWidth) != t.width || fit_size(h, kTargetHeight) != t.tex.height) {
                    if (g_current == &t) g_current = nullptr;
                    if (!release_target(t)) return nullptr;
                    return create_target(t, fbp, w, h) ? &t : nullptr;
                }
                set_scale(t, w, h);
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
    if (!release_target(*oldest)) return nullptr;
    return create_target(*oldest, fbp, gs_width, gs_height) ? oldest : nullptr;
}

void set_eye(bool right) {
    const Target &t = *g_current;
    C3D_RenderTarget *want_rt = (t.right_rt && right) ? t.right_rt : t.rt;
    if (g_hw.drawn_rt != want_rt) {
        C3D_FrameDrawOn(want_rt);
        g_hw.drawn_rt = want_rt;
        g_hw.scissor_valid = false;
    }
    if (t.right_rt && g_source && g_source->right_rt) {
        C3D_Tex *et = right ? &g_source->right_tex : &g_source->tex;
        C3D_TexBind(0, et);
        g_hw.bound_tex0 = et;
        g_hw.tex_bind_valid = false;
    }
    if (!g_hw.scissor_valid || std::memcmp(g_hw.applied_scissor, g_scissor, sizeof g_scissor) != 0) {
        C3D_SetScissor(GPU_SCISSOR_NORMAL, g_scissor[0], g_scissor[1], g_scissor[2], g_scissor[3]);
        std::memcpy(g_hw.applied_scissor, g_scissor, sizeof g_scissor);
        g_hw.scissor_valid = true;
    }
    if (!g_hw.offset_valid) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g_uniform_offset, -1.0f, 1.0f, 0.0f, 0.0f);
        g_hw.offset_valid = true;
    }
    const float shift = t.right_rt ? (right ? g_eye_now : -g_eye_now) : 0.0f;
    if (!g_hw.eye_valid || g_hw.applied_shift != shift) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g_uniform_eye, shift, -shift * kEyeDepth, kEyeDepth, 0.0f);
        g_hw.applied_shift = shift;
        g_hw.eye_valid = true;
    }
}

void bind_target(Target *t) {
    const bool changed = g_current != t;
    if (changed) {
        C3D_FrameDrawOn(t->rt);
        g_hw.drawn_rt = t->rt;
        g_hw.scissor_valid = false;
    }
    if (changed || t->sx != g_bound_sx || t->sy != g_bound_sy) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g_uniform_scale, 2.0f * t->sx / t->tex.width, -2.0f * t->sy / t->tex.height, 1.0f, 0.0f);
        g_bound_sx = t->sx;
        g_bound_sy = t->sy;
        g_hw.clip_valid = false;
    }
    g_current = t;
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
    const std::uint64_t key = (static_cast<std::uint64_t>(g_fog_colour) << 8) | (g_fog_on ? 16u : 0u) |
                              (textured ? (1u | (static_cast<std::uint64_t>(tfx & 3) << 1) | (tcc ? 8u : 0u)) : 0u);
    if (g_hw.texenv_valid && g_hw.texenv_key == key) return;
    g_hw.texenv_valid = true;
    g_hw.texenv_key = key;
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
    if (g_hw.blend_valid && g_hw.blend_abe == abe && (!abe || g_hw.blend_alpha == alpha)) return;
    g_hw.blend_valid = true;
    g_hw.blend_abe = abe;
    g_hw.blend_alpha = alpha;
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
    const std::uint32_t fbmsk = static_cast<std::uint32_t>(c.frame >> 32);
    if (g_hw.tests_valid && !g_hw.tests_skip && g_hw.tests_test == c.test && g_hw.tests_zbuf == c.zbuf && g_hw.tests_fbmsk == fbmsk) return;
    g_hw.tests_valid = true;
    g_hw.tests_skip = false;
    g_hw.tests_test = c.test;
    g_hw.tests_zbuf = c.zbuf;
    g_hw.tests_fbmsk = fbmsk;
    const bool ate = c.test & 1u;
    const int atst = static_cast<int>(c.test >> 1) & 7;
    const std::uint32_t aref = static_cast<std::uint32_t>(c.test >> 4) & 0xFFu;
    const int afail = static_cast<int>(c.test >> 12) & 3;
    const bool zte = (c.test >> 16) & 1u;
    const int ztst = static_cast<int>(c.test >> 17) & 3;
    const bool zmsk = (c.zbuf >> 32) & 1u;

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
        else g_skip_batch = true, g_hw.tests_skip = true;
    } else if (ate && atst == 1) {
        test = false;
    } else if (ate && afail != 0) {
        test = false;   // nothing is discarded: see the two passes below
    }
    static const GPU_TESTFUNC kAlphaTest[8] = {GPU_NEVER, GPU_ALWAYS, GPU_LESS, GPU_LEQUAL, GPU_EQUAL, GPU_GEQUAL, GPU_GREATER, GPU_NOTEQUAL};
    static const GPU_TESTFUNC kDepthTest[4] = {GPU_NEVER, GPU_ALWAYS, GPU_GEQUAL, GPU_GREATER};
    if (zwrite) writes |= GPU_WRITE_DEPTH;
    const bool depth_on = (zte || zwrite) && !(g_debug & 1u);
    const GPU_TESTFUNC depth_fn = zte ? kDepthTest[ztst] : GPU_ALWAYS;
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
            g_split = {true, depth_on, kAlphaTest[atst], kOpposite[atst], depth_fn, std::min<int>(static_cast<int>(aref * 2u), 255), writes, fail_writes};
        }
    }
    if (!g_split.on) {
        C3D_AlphaTest(test, kAlphaTest[atst], static_cast<int>(std::min<std::uint32_t>(aref * 2u, 255u)));
        C3D_DepthTest(depth_on, depth_fn, static_cast<GPU_WRITEMASK>(writes));
    }
}

void set_scissor(const Context &c, const Target &t) {
    const int x0 = static_cast<int>(c.scissor & 0x7FFu), x1 = static_cast<int>((c.scissor >> 16) & 0x7FFu);
    const int y0 = static_cast<int>((c.scissor >> 32) & 0x7FFu), y1 = static_cast<int>((c.scissor >> 48) & 0x7FFu);
    const int tw = t.width, th = t.tex.height;
    const int left = std::clamp(static_cast<int>(std::floor(static_cast<float>(x0) * t.sx)), 0, tw);
    const int right = std::clamp(static_cast<int>(std::ceil(static_cast<float>(x1 + 1) * t.sx)), 0, tw);
    const int top = std::clamp(static_cast<int>(std::floor(static_cast<float>(y0) * t.sy)), 0, th);
    const bool full_height = y0 == 0 && y1 >= t.gs_height - 1;
    // A full-height GS scissor includes the entire scaled picture at the top
    // of the texture. Cover the backing texture too so PICA keeps those rows.
    const int bottom = full_height ? th :
        std::clamp(static_cast<int>(std::ceil(static_cast<float>(y1 + 1) * t.sy)), 0, th);
    g_scissor[0] = static_cast<u32>(left);
    g_scissor[1] = static_cast<u32>(top);
    g_scissor[2] = static_cast<u32>(right);
    g_scissor[3] = static_cast<u32>(bottom);
}

void dump_batch(const State &st, const Context &c, std::uint64_t attr, std::uint32_t verts) {
    const std::uint32_t tbp = static_cast<std::uint32_t>(c.tex0) & 0x3FFFu;
    if (verts) {
        float lo[6] = {1e9f, 1e9f, 1e9f, 1e9f, 1e9f, 1e9f}, hi[6] = {-1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f};
        int alpha_max = 0;
        for (std::uint32_t i = g_index_first; i < g_index_used; ++i) {
            const DrawVertex &v = g_ring[g_index_base + g_indices[i]];
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
    if (g_dump_frame && ps2::frame_count() >= g_dump_frame && ps2::frame_count() < g_dump_frame + 2) dump_batch(st, c, attr, g_index_used - g_index_first);
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
    bind_target(t);

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
    // Shapes drawn with a real depth test are the 3D scene. Everything else (menus, text, fades) stays on the screen's surface.
    const bool in_depth = ((c.test >> 16) & 1u) && ((c.test >> 17) & 3u) >= 2u;
    g_eye_now = in_depth ? g_eye_shift : 0.0f;
    g_in_depth = in_depth;
    if (!g_hw.clip_valid || g_hw.clip_target != t || g_hw.clip_scissor_reg != c.scissor || g_hw.clip_ofx != g_ofx || g_hw.clip_ofy != g_ofy || g_hw.clip_eye_now != g_eye_now) {
        set_scissor(c, *t);
        // Invert the viewport transform for a conservative reject in GS 12.4
        // coordinates. Leave a pixel for rounding and expand for either eye.
        const float eye_margin = t->right_rt ? g_eye_now * -kEyeDepth * t->width * 0.5f : 0.0f;
        g_clip_left = g_ofx + static_cast<int>(std::floor((g_scissor[0] - 1.0f - eye_margin) * 16.0f / t->sx));
        g_clip_right = g_ofx + static_cast<int>(std::ceil((g_scissor[2] + 1.0f + eye_margin) * 16.0f / t->sx));
        g_clip_top = g_ofy + static_cast<int>(std::floor((g_scissor[1] - 1.0f) * 16.0f / t->sy));
        g_clip_bottom = g_ofy + static_cast<int>(std::ceil((g_scissor[3] + 1.0f) * 16.0f / t->sy));
        g_hw.clip_valid = true;
        g_hw.clip_target = t;
        g_hw.clip_scissor_reg = c.scissor;
        g_hw.clip_ofx = g_ofx;
        g_hw.clip_ofy = g_ofy;
        g_hw.clip_eye_now = g_eye_now;
    }
    g_source = nullptr;

    g_textured = false;
    g_alpha_from_texture = false;
    g_alpha_texture = nullptr;
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
            g_source = source;
            g_u_scale = source->sx / source->tex.width;
            g_v_scale = -source->sy / source->tex.height;
            g_v_offset = 1.0f;
            g_s_scale = static_cast<float>(1 << tw) * source->sx / source->tex.width;
            g_t_scale = static_cast<float>(1 << th) * source->sy / source->tex.height;
        } else if (Texture *e = find_texture(st, c, !g_fst)) {
            g_alpha_texture = e;
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
            const std::uint8_t flags = (bilinear ? 1u : 0u) | (wms == 0 ? 2u : 0u) | (wmt == 0 ? 4u : 0u);
            if (!g_hw.tex_bind_valid || g_hw.bound_tex0 != tex || g_hw.bound_tex_flags != flags) {
                const GPU_TEXTURE_FILTER_PARAM filter = bilinear ? GPU_LINEAR : GPU_NEAREST;
                C3D_TexSetFilter(tex, filter, filter);
                C3D_TexSetWrap(tex, wms == 0 ? GPU_REPEAT : GPU_CLAMP_TO_EDGE, wmt == 0 ? GPU_REPEAT : GPU_CLAMP_TO_EDGE);
                if (source && source->right_rt) {
                    C3D_TexSetFilter(&source->right_tex, filter, filter);
                    C3D_TexSetWrap(&source->right_tex, wms == 0 ? GPU_REPEAT : GPU_CLAMP_TO_EDGE, wmt == 0 ? GPU_REPEAT : GPU_CLAMP_TO_EDGE);
                }
                C3D_TexBind(0, tex);
                g_hw.tex_bind_valid = true;
                g_hw.bound_tex0 = tex;
                g_hw.bound_tex_flags = flags;
            }
            g_textured = true;
            g_alpha_from_texture = tcc;
            g_alpha_function = tfx;
            set_texenv(true, tfx, tcc);
        } else {
            g_skip_batch = true;
            ++g_stat_missing;
        }
    } else {
        if (!g_hw.tex_bind_valid || g_hw.bound_tex0 != nullptr) {
            C3D_TexBind(0, nullptr);
            g_hw.tex_bind_valid = true;
            g_hw.bound_tex0 = nullptr;
        }
        set_texenv(false, 0, false);
    }
    set_eye(false);
}

bool ensure_state(const State &st, const Context &c, std::uint64_t attr) {
    PS2_PROF_EXTRA(3);
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

inline float gs_x(const Vertex &v) { return static_cast<float>(static_cast<int>(v.x) - g_ofx) * (1.0f / 16.0f); }
inline float gs_y(const Vertex &v) { return static_cast<float>(static_cast<int>(v.y) - g_ofy) * (1.0f / 16.0f); }

template <bool triangle>
inline void write_vertex(DrawVertex *o, float x, float y, const Vertex &z_from, const Vertex &colour, const Vertex &tex, float q) {
    o->x = x;
    o->y = y;
    o->z = std::max(-static_cast<float>(z_from.z) * g_z_scale, -1.0f);   // the GS clamps at the top of the buffer's range
    std::memcpy(&o->r, &colour.r, sizeof(std::uint32_t));
    o->fog = z_from.f;
    const float iq = g_fst ? 1.0f : 1.0f / (q > 1e-6f ? q : 1e-6f);
    o->w = iq;
    if (triangle && !g_textured) {
        o->s = o->t = 0.0f;
    } else if (g_fst) {
        o->s = (static_cast<float>(tex.u) / 16.0f) * g_u_scale;
        o->t = g_v_offset + (static_cast<float>(tex.v) / 16.0f) * g_v_scale;
    } else {
        o->s = tex.s * iq * g_s_scale;
        o->t = g_v_offset - tex.t * iq * g_t_scale;
    }
}

void use_geometry_slice(unsigned slice, std::uint32_t capacity) {
    flush_g3();   // nothing should wait here (callers flush first), but its vertices are in the slice being left
    g_slice = slice;
    g_slice_capacity = capacity;
    g_ring = g_vertex_storage + slice * kSliceVertices;
    g_indices = g_index_storage + slice * kSliceVertices;
    g_ring_used = g_flushed_to = 0;
    g_index_used = g_index_first = g_index_flushed = g_index_base = 0;
    g_bound_base = UINT32_MAX;
    g_last_triangle_end = 0;
}

bool reserve_geometry(std::uint32_t indices) {
    if (g_slice_capacity - g_index_used >= indices) return true;
    if (!g_overlap_picture || g_slices == 3) return false;
    flush_batch();
    flush_ring();
    FesOverlapWait();
    ++g_overflow_waits;
    g_slices = 3;
    use_geometry_slice(g_slice ^ 1u, kSliceVertices);
    return true;
}

// Indices are relative to one span of the vertex ring. A full span ends the current batch.
void reserve_indices(unsigned vertices) {
    if (g_ring_used - g_index_base + vertices <= 65536u) return;
    flush_batch();
    g_index_base = g_ring_used;
    g_last_triangle_end = 0;
}

std::uint16_t vertex_index(std::uint32_t vertex) {
    return static_cast<std::uint16_t>(vertex - g_index_base);
}

bool same_position(const Vertex &a, const Vertex &b) {
    // Depth affects the stereo offset, and Q affects the shader's clip-space position.
    return a.x == b.x && a.y == b.y && a.z == b.z && a.q == b.q;
}

bool outside_scissor(const Vertex *v) {
    const int x0 = static_cast<int>(v[0].x), x1 = static_cast<int>(v[1].x), x2 = static_cast<int>(v[2].x);
    const int y0 = static_cast<int>(v[0].y), y1 = static_cast<int>(v[1].y), y2 = static_cast<int>(v[2].y);
    return (x0 < g_clip_left && x1 < g_clip_left && x2 < g_clip_left) ||
           (x0 > g_clip_right && x1 > g_clip_right && x2 > g_clip_right) ||
           (y0 < g_clip_top && y1 < g_clip_top && y2 < g_clip_top) ||
           (y0 > g_clip_bottom && y1 > g_clip_bottom && y2 > g_clip_bottom);
}

void add_triangle(const Vertex *v, unsigned type, std::uint32_t serial) {
    // Strip connectors often repeat a vertex. Identical projected vertices have no area in either eye.
    if (same_position(v[0], v[1]) || same_position(v[1], v[2]) || same_position(v[0], v[2]) || outside_scissor(v)) {
        g_last_triangle_end = 0;
        return;
    }
    reserve_indices(3);
    const bool reuse = type >= 4 && g_index_used >= 3 && g_last_triangle_end == g_index_used &&
        g_last_triangle_setup == g_setup_seen && g_last_triangle_serial + 1u == serial;
    auto *indices = g_indices + g_index_used;
    std::uint32_t colour = 0;
    if (!g_iip) std::memcpy(&colour, &v[2].r, sizeof colour);
    if (reuse) {
        const auto *previous = indices - 3;
        indices[0] = previous[type == 4 ? 1 : 0];
        indices[1] = previous[2];
        if (!g_iip && colour != g_last_triangle_colour) {
            // A flat triangle takes the colour of its last vertex.
            for (unsigned i = 0; i < 2; ++i) {
                DrawVertex &vertex = g_ring[g_ring_used];
                std::memcpy(&vertex, g_ring + g_index_base + indices[i], sizeof vertex);
                std::memcpy(&vertex.r, &colour, sizeof colour);
                indices[i] = vertex_index(g_ring_used++);
            }
        }
        DrawVertex *vertex = g_ring + g_ring_used;
        write_vertex<true>(vertex, gs_x(v[2]), gs_y(v[2]), v[2], v[2], v[2], v[2].q);
        indices[2] = vertex_index(g_ring_used++);
    } else {
        DrawVertex *o = g_ring + g_ring_used;
        const std::uint16_t first = vertex_index(g_ring_used);
        for (unsigned i = 0; i < 3; ++i) {
            write_vertex<true>(o + i, gs_x(v[i]), gs_y(v[i]), v[i], g_iip ? v[i] : v[2], v[i], v[i].q);
            indices[i] = static_cast<std::uint16_t>(first + i);
        }
        g_ring_used += 3;
    }
    if ((g_debug & 8u) && g_in_depth) ++g_depth_seen[std::min(15, static_cast<int>(-g_ring[g_ring_used - 1].z * 16.0f))];
    g_index_used += 3;
    g_last_triangle_end = g_index_used;
    g_last_triangle_serial = serial;
    g_last_triangle_setup = g_setup_seen;
    g_last_triangle_colour = colour;
}

void add_quad(float x0, float y0, float x1, float y1, const Vertex &a, const Vertex &b) {
    reserve_indices(4);
    DrawVertex *o = g_ring + g_ring_used;
    write_vertex<false>(o + 0, x0, y0, b, b, a, b.q);
    write_vertex<false>(o + 2, x1, y1, b, b, b, b.q);
    std::memcpy(o + 1, o + 0, sizeof(DrawVertex));
    o[1].y = y1;
    o[1].t = o[2].t;
    std::memcpy(o + 3, o + 2, sizeof(DrawVertex));
    o[3].y = y0;
    o[3].t = o[0].t;
    constexpr unsigned order[] = {0, 1, 2, 0, 2, 3};
    for (unsigned corner : order)
        g_indices[g_index_used++] = vertex_index(g_ring_used + corner);
    g_ring_used += 4;
}

// One picture onto one of the top screen's two buffers: u0 to u1 across, the top row at v = 1 and the bottom at v1.
void show_texture(C3D_RenderTarget *screen, C3D_Tex *tex, float u0, float u1, float v1, GPU_TEXTURE_FILTER_PARAM filter) {
    flush_g3();
    g_hw.invalidate();
    C3D_FrameDrawOn(screen);
    g_current = nullptr;
    g_split.on = false;
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_uniform_scale, 1.0f, 1.0f, 1.0f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_uniform_offset, 0.0f, 0.0f, 0.0f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_uniform_eye, 0.0f, 0.0f, -1.0f, 0.0f);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_TexSetFilter(tex, filter, filter);
    C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexBind(0, tex);
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_TexEnvInit(C3D_GetTexEnv(1));
    set_fog(false, 0);

    if (!reserve_geometry(6)) return;
    reserve_indices(4);
    // The top screen is mounted sideways: the screen's left edge is the buffer's top.
    const float px[4] = {1.0f, 1.0f, -1.0f, -1.0f};    // screen top left, top right, bottom left, bottom right
    const float py[4] = {1.0f, -1.0f, 1.0f, -1.0f};
    const float su[4] = {u0, u1, u0, u1};
    const float sv[4] = {1.0f, 1.0f, v1, v1};
    const int order[6] = {0, 1, 2, 2, 1, 3};
    DrawVertex *o = g_ring + g_ring_used;
    for (int i = 0; i < 4; ++i)
        o[i] = DrawVertex{px[i], py[i], -0.5f, 1.0f, 255, 255, 255, 255, su[i], sv[i], 255, {0, 0, 0}};
    for (int corner : order)
        g_indices[g_index_used++] = vertex_index(g_ring_used + corner);
    g_ring_used += 4;
    flush_batch();
    g_key_valid = false;
}

void show_target(Target *t, const DisplayRegs &r, int circuit) {
    const std::uint64_t disp = r.display[circuit];
    const std::uint32_t magh = (static_cast<std::uint32_t>(disp >> 23) & 0xFu) + 1, magv = (static_cast<std::uint32_t>(disp >> 27) & 3u) + 1;
    const float dw = static_cast<float>((static_cast<std::uint32_t>(disp >> 32) & 0xFFFu) + 1) / static_cast<float>(magh);
    float dh = static_cast<float>((static_cast<std::uint32_t>(disp >> 44) & 0x7FFu) + 1) / static_cast<float>(magv);
    if (dw < 1.0f || dh < 1.0f) return;
    const float across = dw * t->sx, down = dh * t->sy;
    const float u1 = across / t->tex.width, v1 = 1.0f - down / t->tex.height;
    // A picture that is already the screen's size goes up texel for texel.
    const bool exact = std::fabs(across - kScreenWidth) < 0.5f && std::fabs(down - kScreenHeight) < 0.5f;
    const GPU_TEXTURE_FILTER_PARAM filter = exact ? GPU_NEAREST : GPU_LINEAR;
    show_texture(g_screen, &t->tex, 0.0f, u1, v1, filter);
    if (g_two_screens) show_texture(g_screen_right, t->right_rt ? &t->right_tex : &t->tex, 0.0f, u1, v1, filter);
}

u8 *g_shot_pixels = nullptr;

void save_shot(C3D_RenderTarget *screen, const char *ending) {
    u8 *pixels = g_shot_pixels;
    if (!pixels) return;
    const u32 dims = GX_BUFFER_DIM(240, 400);
    const u32 flags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                      GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    // Outside an open frame citro3d waits for this transfer before returning.
    C3D_SyncDisplayTransfer(static_cast<u32 *>(screen->frameBuf.colorBuf), dims, reinterpret_cast<u32 *>(pixels), dims, flags);
    GSPGPU_InvalidateDataCache(pixels, 240 * 400 * 3);
    char name[64];
    if (g_debug & 32u) {
        const char *suffix = g_overlap_picture ? (g_debug & 128u ? "stock_small" : "stock") : "stock_control";
        std::snprintf(name, sizeof name, "sdmc:/p3fes/shots/frame_%05llu%s_%s.ppm",
                      static_cast<unsigned long long>(g_frame_number), ending, suffix);
    } else {
        std::snprintf(name, sizeof name, "sdmc:/p3fes/shots/frame_%05llu%s.ppm",
                      static_cast<unsigned long long>(g_frame_number), ending);
    }
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

// Start recording at the first shape of a drawn picture. The overlap path can record in the other slice of the geometry
// rings while the GPU finishes the preceding picture. Submission still follows the preceding display transfer.
void begin_frame() {
    g_last_triangle_end = 0;
    delete_retired();
    // The 3D slider is read once a frame. Moving it from or to zero changes the shape of the full-size targets, so they are made
    // anew: one picture is drawn onto empty ones.
    const float slider = g_debug & 32u ? 1.0f : osGet3DSliderState();
    g_two_screens = slider > 0.0f;
    const bool stereo = g_two_screens && g_stereo_strength;
    if (stereo != g_stereo) {
        for (Target &t : g_targets)
            if (t.used) release_target(t);
        g_stereo = stereo;
    }
    // At the slider's top and strength 5 the farthest things sit 10 screen pixels apart between the eyes: 5 each way.
    g_eye_shift = stereo ? slider * static_cast<float>(g_stereo_strength) * (0.005f / -kEyeDepth) : 0.0f;
    const u64 start = svcGetSystemTick();
    const unsigned previous = g_slices;
    g_overlap_picture = g_frame && g_overlap_ready && !(g_debug & 64u) && FesOverlapBegin(previous == 3, g_debug & 128u);
    if (g_overlap_picture) {
        const unsigned slice = previous == 1 ? 1 : 0;
        use_geometry_slice(slice, kSliceVertices);
        g_slices = 1u << slice;
    } else {
        C3D_FrameBegin(0);
        use_geometry_slice(0, kRingVertices);
        g_slices = 3;
    }
    g_profile_begin += svcGetSystemTick() - start;
    g_frame_open = true;
    g_current = nullptr;
    g_key_valid = false;
    g_unif_valid = false;
    g_hw.invalidate();
    std::memset(g_pica_bone_gen, 0, sizeof g_pica_bone_gen);
    ++g_frame;
}

}

bool active() { return g_active; }
void set_present_every(std::uint32_t vblanks) { g_present_every = vblanks; }
void set_shot_every(std::uint32_t frames) { g_shot_every = frames; }
bool g_after_hidden = false, g_render_left_out = false;
void set_draw_every(std::uint32_t pictures) { g_draw_every = pictures; }
void set_stats(bool on) { g_stats = on; }
void set_dump_frame(std::uint64_t frame) { g_dump_frame = frame; }
void set_hide(std::uint32_t from, std::uint32_t to) { g_hide_from = from, g_hide_to = to; }
void set_skip_attr(std::uint32_t attr) { g_skip_attr = attr; }
void set_debug(std::uint32_t bits) { g_debug = bits; }
void set_stereo(std::uint32_t strength) { g_stereo_strength = std::min<std::uint32_t>(strength, 10u); }
std::uint32_t stereo() { return g_stereo_strength; }

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
    g_uniform_eye = shaderInstanceGetUniformLocation(g_program.vertexShader, "eye");
    g_uniform_proj = shaderInstanceGetUniformLocation(g_program.vertexShader, "proj");
    g_uniform_bones = shaderInstanceGetUniformLocation(g_program.vertexShader, "bones");
    {
        // A bool uniform's location counts from the float ones' base (b0 is 0x68), which C3D_BoolUnifSet takes off itself.
        const int lcol = shaderInstanceGetUniformLocation(g_program.vertexShader, "lcol");
        const int b = shaderInstanceGetUniformLocation(g_program.vertexShader, "isg3");
        const int skin = shaderInstanceGetUniformLocation(g_program.vertexShader, "isskin");
        const int ortho = shaderInstanceGetUniformLocation(g_program.vertexShader, "isortho");
        if (g_uniform_proj >= 0 && lcol == g_uniform_proj + 12 && g_uniform_bones == g_uniform_proj + 15 && b >= 0x68 && b < 0x78 &&
            skin >= 0x68 && skin < 0x78 && ortho >= 0x68 && ortho < 0x78) {
            g_bool_g3 = b;
            g_bool_skin = skin;
            g_bool_ortho = ortho;
        }
        std::fprintf(stderr, "[pica] g3 shader: proj %d lcol %d bones %d isg3 %d isortho %d%s\n", g_uniform_proj, lcol, g_uniform_bones, b, ortho,
                     g_bool_g3 >= 0 ? "" : " (not usable)");
    }
    if (g_bool_g3 >= 0) {
        // Every bone slot starts at zero: a weight of zero may point at a slot no batch has filled yet.
        C3D_FVec *b = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g_uniform_bones, 3 * static_cast<int>(::g3::kGpuBones));
        std::memset(b, 0, 3 * ::g3::kGpuBones * sizeof *b);
    }
    AttrInfo_Init(&g_g3_attr);
    AttrInfo_AddLoader(&g_g3_attr, 0, GPU_FLOAT, 3);           // position
    AttrInfo_AddLoader(&g_g3_attr, 1, GPU_FLOAT, 2);           // uv
    AttrInfo_AddLoader(&g_g3_attr, 2, GPU_UNSIGNED_BYTE, 4);   // prelit colour
    AttrInfo_AddLoader(&g_g3_attr, 3, GPU_BYTE, 4);            // normal
    AttrInfo_AddLoader(&g_g3_attr, 4, GPU_FLOAT, 4);           // bone weights
    AttrInfo_AddLoader(&g_g3_attr, 5, GPU_UNSIGNED_BYTE, 4);   // bones' first rows

    AttrInfo_Init(&g_attr);
    AttrInfo_AddLoader(&g_attr, 0, GPU_FLOAT, 4);
    AttrInfo_AddLoader(&g_attr, 1, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(&g_attr, 2, GPU_FLOAT, 2);
    AttrInfo_AddLoader(&g_attr, 3, GPU_UNSIGNED_BYTE, 4);
    C3D_SetAttrInfo(&g_attr);

    g_vertex_storage = g_ring = static_cast<DrawVertex *>(linearAlloc(kRingBytes));
    g_index_storage = g_indices = static_cast<std::uint16_t *>(linearAlloc(kRingVertices * sizeof(*g_indices)));
    g_overlap_ready = FesOverlapInit(kCommandBuffer);
    std::fprintf(stderr, "[overlap] recording buffer %s\n", g_overlap_ready ? "ready" : "unavailable");
    if (!g_ring || !g_indices) {
        std::fprintf(stderr, "[pica] no memory for vertex or index data\n");
        return false;
    }
    g_shot_pixels = static_cast<u8 *>(linearAlloc(240 * 400 * 3));
    BufInfo_Init(&g_buf);
    BufInfo_Add(&g_buf, g_ring, sizeof(DrawVertex), 4, 0x3210);
    C3D_SetBufInfo(&g_buf);

    g_depth = vramAlloc(C3D_CalcDepthBufSize(kTargetWidth, kTargetHeight, GPU_RB_DEPTH24));
    g_screen = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, -1);
    g_screen_right = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, -1);
    if (!g_depth || !g_screen || !g_screen_right) {
        std::fprintf(stderr, "[pica] no video memory\n");
        return false;
    }
    constexpr u32 kToScreen = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                              GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    C3D_RenderTargetSetOutput(g_screen, GFX_TOP, GFX_LEFT, kToScreen);
    C3D_RenderTargetSetOutput(g_screen_right, GFX_TOP, GFX_RIGHT, kToScreen);
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
    if (g_frame_open) {
        flush_batch();
        flush_ring();
        FesOverlapEnd(0);
        g_frame_open = false;
    }
    FesOverlapWait();
    C3D_Fini();
}

namespace {

// Every register that can change the batch moves g_setup_generation, so a run of shapes checks just this one number.
void setup_batch(const State &st, std::uint64_t clamp) {
    if (!g_key_valid || g_setup_seen != g_setup_generation) {
        g_setup_seen = g_setup_generation;
        std::uint64_t attr = st.prim;
        if (!(st.prmodecont & 1u)) attr = (st.prim & 7u) | (st.prmode & ~7ull);
        const unsigned context = (attr >> 9) & 1u;
        const Context &c = st.ctx[context];
        if (context == ((st.prim >> 9) & 1u) && c.clamp != clamp) {
            Context adjusted = c;
            adjusted.clamp = clamp;
            ensure_state(st, adjusted, attr);
        } else {
            ensure_state(st, c, attr);
        }
    }
}

// A g3 strip drawn as flush_batch draws its triangles, with an alpha test split into its two passes when split is true.
void draw_strip(int first, int count, bool split) {
    if (split) {
        C3D_AlphaTest(true, g_split.pass, g_split.reference);
        C3D_DepthTest(g_split.depth, g_split.depth_test, static_cast<GPU_WRITEMASK>(g_split.pass_writes));
        C3D_DrawArrays(GPU_TRIANGLE_STRIP, first, count);
        C3D_AlphaTest(true, g_split.fail, g_split.reference);
        C3D_DepthTest(g_split.depth, g_split.depth_test, static_cast<GPU_WRITEMASK>(g_split.fail_writes));
    }
    C3D_DrawArrays(GPU_TRIANGLE_STRIP, first, count);
}

int uniform_alpha_pass_g3(bool alpha_uniform, std::uint8_t alpha_val, float mat_alpha) {
    int texel = 0;
    if (g_alpha_from_texture) {
        if (!g_alpha_texture) return -1;
        int &alpha = g_alpha_texture->uniform_alpha;
        if (alpha == -2) {
            const auto &tex = g_alpha_texture->tex;
            const auto *pixels = static_cast<const std::uint32_t *>(tex.data);
            alpha = pixels[0] & 255;
            const std::uint32_t n = static_cast<std::uint32_t>(tex.width) * tex.height;
            for (std::uint32_t i = 1; i < n; ++i) {
                if (static_cast<int>(pixels[i] & 255) == alpha) continue;
                alpha = -1;
                break;
            }
        }
        if (alpha < 0) return -1;
        texel = alpha;
    }
    int vlo = 0, vhi = 0;
    if (!g_alpha_from_texture || g_alpha_function == 0 || g_alpha_function == 2) {
        if (!alpha_uniform) return -1;
        const float vf = static_cast<float>(alpha_val) * mat_alpha;
        vlo = std::clamp(static_cast<int>(std::floor(vf)), 0, 255);
        vhi = std::clamp(static_cast<int>(std::ceil(vf)), 0, 255);
    }
    int low = vlo * 2, high = vhi * 2;
    if (g_alpha_from_texture) {
        switch (g_alpha_function) {
        case 0:
            low = ((texel * vlo) >> 8) * 4;
            high = ((texel * vhi + 254) / 255) * 4;
            break;
        case 2:
            low = 2 * (texel + vlo);
            high = 2 * (texel + vhi);
            break;
        default:
            low = high = 2 * texel;
            break;
        }
    }
    low = std::min(low, 255);
    high = std::min(high, 255);
    const int reference = g_split.reference;
    switch (g_split.pass) {
    case GPU_LESS: return high < reference ? 1 : low >= reference ? 0 : -1;
    case GPU_LEQUAL: return high <= reference ? 1 : low > reference ? 0 : -1;
    case GPU_EQUAL:
    case GPU_NOTEQUAL:
        if (low == high || reference < low || reference > high)
            return (low == reference) == (g_split.pass == GPU_EQUAL);
        return -1;
    case GPU_GEQUAL: return low >= reference ? 1 : high < reference ? 0 : -1;
    case GPU_GREATER: return low > reference ? 1 : high <= reference ? 0 : -1;
    default: return -1;
    }
}

// A uniform written only when it changes: citro3d keeps a copy of every uniform and uploads only the dirty ones, and batches
// of one model mostly share their projection, lights and bones. Each one written costs the command list 5 words.
inline void put_unif(int id, const float *v) {
    C3D_FVec &u = C3D_FVUnif[GPU_VERTEX_SHADER][id];
    if (u.x == v[0] && u.y == v[1] && u.z == v[2] && u.w == v[3]) return;
    u.x = v[0], u.y = v[1], u.z = v[2], u.w = v[3];
    C3D_FVUnifDirty[GPU_VERTEX_SHADER][id] = true;
    ++g_stat_unif;
}

// Command words a g3 batch may need at most (all uniforms, the attribute and buffer setup, both eyes' draws), and what must
// stay free after it for the rest of the frame's shapes: the frame's command list has one fixed buffer.
constexpr u32 kG3Words = 1024, kFrameReserveWords = 24 * 1024;

}

bool g3_ready() { return g_bool_g3 >= 0; }

namespace {

// g3 batches waiting to be drawn as one strip: consecutive batches with the same state, uniforms and culling are joined
// with degenerate triangles, since every draw call costs the command list about 150 words and the GPU a framebuffer flush.
// Their vertices lie in the ring from vertex `first` (48-byte units from g_ring). A GS shape, a state change, or a later batch
// that needs different uniforms or a bone slot these vertices use draws them first.
struct G3Pending {
    bool on = false;
    std::uint32_t first = 0, count = 0, slots = 0;
    float mat_alpha = 1.0f;
    bool alpha_uniform = true;
    std::uint8_t alpha_val = 0;
} g_g3_pending;

void flush_g3() {
    if (!g_g3_pending.on) return;
    g_g3_pending.on = false;
    const int first = static_cast<int>(g_g3_pending.first), count = static_cast<int>(g_g3_pending.count);
    const std::uint32_t w0 = cmd_words();
    const int pass = g_split.on ? uniform_alpha_pass_g3(g_g3_pending.alpha_uniform, g_g3_pending.alpha_val, g_g3_pending.mat_alpha) : -1;
    const bool split = g_split.on && pass < 0;
    if (pass >= 0) {
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_DepthTest(g_split.depth, g_split.depth_test,
                      static_cast<GPU_WRITEMASK>(pass ? g_split.pass_writes : g_split.fail_writes));
    }
    draw_strip(first, count, split);
    if (g_current && g_current->right_rt) {
        set_eye(true);
        draw_strip(first, count, split);
        set_eye(false);
    }
    const std::uint32_t w1 = cmd_words();
    if (w1 >= w0) g_words_draw += w1 - w0;
    ++g_stat_draws;
    if (C3D_GetCmdBufUsage() > 0.75f) {
        flush_ring();
        C3D_FrameSplit(GX_CMDLIST_FLUSH);
        ++g_stat_splits;
    }
}

inline bool same_unif(int id, const float *v) {
    const C3D_FVec &u = C3D_FVUnif[GPU_VERTEX_SHADER][id];
    return u.x == v[0] && u.y == v[1] && u.z == v[2] && u.w == v[3];
}

}

bool draw_g3(const State &st, std::uint64_t clamp, const ::g3::Gpu &g) {
    PS2_PROF_EXTRA(0);
    if (g_draw_nothing || (!queue::g_split_pica && queue::g_quiet) || g.n < 3) return true;
    if (!g_frame_open) begin_frame();
    if (FesOverlapWordsLeft() < kG3Words + kFrameReserveWords) {
        ++g_stat_g3_declined;
        return false;   // the GS path's shapes need far less of the command list
    }
    std::uint32_t w0 = cmd_words();
    setup_batch(st, clamp);   // a state change draws what waits first (apply_state calls flush_batch)
    std::uint32_t w1 = cmd_words();
    if (w1 >= w0) g_words_setup += w1 - w0;
    if (g_skip_batch) return true;
    ++g_stat_shapes;
    ++g_stat_g3;
    if (g_index_used > g_index_first) flush_batch();   // the GS path's triangles (no g3 batch waits while they do)

    // The uniforms: proj, post, texs, fogp, mat, amb, ldir[3], lcol[3].
    float unif[15][4];
    for (int i = 0; i < 4; ++i) std::memcpy(unif[i], g.proj[i], sizeof unif[i]);
    const float post[4] = {g.offset[0] - static_cast<float>(g_ofx) * (1.0f / 16.0f), g.offset[1] - static_cast<float>(g_ofy) * (1.0f / 16.0f), g.offset[2],
                           -g_z_scale * (g.xyzf ? 1.0f : 16.0f)};
    std::memcpy(unif[4], post, sizeof post);
    const float texs[4] = {g_s_scale, -g_t_scale, g_v_offset, 0.0f};
    std::memcpy(unif[5], texs, sizeof texs);
    const float fogp[4] = {g.fog ? g.fog_scale * (1.0f / 255.0f) : 0.0f,
                           g.fog ? -g.fog_near * g.fog_scale * (1.0f / 255.0f) : static_cast<float>((st.fog >> 56) & 0xFFu) * (1.0f / 255.0f),
                           g.clipper ? 0.0f : -1.0f, g.clipper ? -1e15f : 0.0f};
    std::memcpy(unif[6], fogp, sizeof fogp);
    for (int i = 0; i < 4; ++i) unif[7][i] = g.material[i] * (1.0f / 255.0f);
    std::memcpy(unif[8], g.ambient, sizeof unif[8]);
    for (int i = 0; i < 3; ++i) std::memcpy(unif[9 + i], g.light_dir[i], sizeof unif[9 + i]);
    for (int i = 0; i < 3; ++i) std::memcpy(unif[12 + i], g.light_colour[i], sizeof unif[12 + i]);
    const GPU_CULLMODE cull = g.keep_negative && g.keep_positive ? GPU_CULL_NONE : g.keep_negative ? GPU_CULL_FRONT_CCW : GPU_CULL_BACK_CCW;

    // Can it join the batches waiting? Same culling, shader half and uniforms, and no bone slot they use about to change.
    std::uint32_t unif_u32[15][4];
    std::memcpy(unif_u32, unif, sizeof unif_u32);
    std::uint32_t diff_mask = g_unif_valid ? 0u : 0x7FFFu;
    if (g_unif_valid) {
        for (int i = 0; i < 15; ++i) {
            const std::uint32_t d = (g_unif_bits[i][0] ^ unif_u32[i][0]) | (g_unif_bits[i][1] ^ unif_u32[i][1]) |
                                    (g_unif_bits[i][2] ^ unif_u32[i][2]) | (g_unif_bits[i][3] ^ unif_u32[i][3]);
            if (d) diff_mask |= 1u << i;
        }
    }
    const bool shared_uniforms = diff_mask == 0u;
    const std::uint32_t slots = g.skin ? (g.slots | 1u) : 0u;  // zero-weight lanes read slot zero
    if (g_g3_pending.on) {
        bool join = shared_uniforms && g.skin == g_g3_skin && g.ortho == g_g3_ortho && cull == g_g3_cull;
        for (auto overlap = g_g3_pending.slots & slots; overlap && join; overlap &= overlap - 1u) {
            const unsigned s = __builtin_ctz(overlap);
            join = g.bone_gen[s] == g_pica_bone_gen[s];
        }
        if (!join) flush_g3();
    }

    // Room: the vertices, up to three joining ones, and the rounding to whole 96-byte steps, in the ring's 32-byte slots.
    // The GS path counts on the ring never being ahead of the indices (its room check looks at indices only).
    const std::uint32_t worst = (g_ring_used + 2u) / 3u * 3u + (g.n + 4u) / 2u * 3u;
    if (!reserve_geometry(worst > g_index_used ? worst - g_index_used : 1u)) {   // a move to the other slice draws what waits
        if (!g_ring_warned) std::fprintf(stderr, "[pica] vertex ring full, dropping shapes\n");
        g_ring_warned = true;
        return true;
    }
    auto *ring = reinterpret_cast<::g3::GpuVertex *>(g_ring);
    std::uint32_t at;   // where this batch's own vertices go, in 48-byte units
    if (g_g3_pending.on) {
        // The strip goes on: the last vertex again and this batch's first, so the triangles between have no area, and once
        // more if needed so that this batch's strip starts at an even place, where the GPU's alternating winding starts.
        std::uint32_t end = g_g3_pending.first + g_g3_pending.count;
        ring[end] = ring[end - 1u];
        ring[end + 1u] = g.v[0];
        end += 2u;
        if ((end - g_g3_pending.first) & 1u) ring[end++] = g.v[0];
        at = end;
        g_g3_pending.count = at + g.n - g_g3_pending.first;
        g_g3_pending.slots |= slots;
        g_g3_pending.alpha_uniform = g_g3_pending.alpha_uniform && g.alpha_uniform && (g_g3_pending.alpha_val == g.alpha_val);
    } else {
        // A new strip at a multiple of 96 bytes (3 slots, 2 vertices) from the ring's start, so that one buffer bound at the
        // start with a 48-byte stride serves every g3 batch of the slice.
        at = (g_ring_used + 2u) / 3u * 2u;
        g_g3_pending = G3Pending{true, at, g.n, slots, g.material[3], g.alpha_uniform, g.alpha_val};
    }
    std::memcpy(ring + at, g.v, g.n * sizeof(::g3::GpuVertex));
    g_ring_used = ((at + g.n) * 3u + 1u) / 2u;
    g_index_used = std::max(g_index_used, g_ring_used);
    g_index_first = g_index_used;
    g_last_triangle_end = 0;

    w0 = cmd_words();
    // The g3 vertex layout, buffer and shader half stay set across g3 batches; flush_batch puts the GS ones back.
    if (!g_g3_bound) {
        C3D_SetAttrInfo(&g_g3_attr);
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, g_bool_g3, true);
        g_g3_bound = true;
        g_bound_base = UINT32_MAX;
    }
    if (g_g3_buffer != g_ring) {
        BufInfo_Init(&g_buf);
        BufInfo_Add(&g_buf, g_ring, sizeof(::g3::GpuVertex), 6, 0x543210);
        C3D_SetBufInfo(&g_buf);
        g_g3_buffer = g_ring;
    }
    if (g.skin != g_g3_skin) {
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, g_bool_skin, g.skin);
        g_g3_skin = g.skin;
    }
    if (g.ortho != g_g3_ortho) {
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, g_bool_ortho, g.ortho);
        g_g3_ortho = g.ortho;
    }
    // The GS leaves out the triangles whose ADC bit the program set: here the GPU's culling does it. With every other
    // triangle turned round as (k-1, k-2, k), the program's negative facing is an area above zero with y down; the y axis
    // flips on the way to clip space, so it is clockwise there.
    if (cull != g_g3_cull) {
        C3D_CullFace(cull);
        g_g3_cull = cull;
    }
    // Only what changed is sent.
    if (diff_mask) {
        for (std::uint32_t m = diff_mask; m; m &= m - 1u) {
            const int i = __builtin_ctz(m);
            C3D_FVec &u = C3D_FVUnif[GPU_VERTEX_SHADER][g_uniform_proj + i];
            u.x = unif[i][0]; u.y = unif[i][1]; u.z = unif[i][2]; u.w = unif[i][3];
            C3D_FVUnifDirty[GPU_VERTEX_SHADER][g_uniform_proj + i] = true;
            ++g_stat_unif;
            g_unif_bits[i][0] = unif_u32[i][0]; g_unif_bits[i][1] = unif_u32[i][1];
            g_unif_bits[i][2] = unif_u32[i][2]; g_unif_bits[i][3] = unif_u32[i][3];
        }
        g_unif_valid = true;
    }
    for (auto remaining = slots; remaining; remaining &= remaining - 1u) {
        const unsigned s = __builtin_ctz(remaining);
        if (g.bone_gen[s] == g_pica_bone_gen[s]) continue;
        const unsigned first = 3u * s;
        for (unsigned i = first; i < first + 3; ++i) {
            C3D_FVec &u = C3D_FVUnif[GPU_VERTEX_SHADER][g_uniform_bones + i];
            u.x = g.bone[i][0]; u.y = g.bone[i][1]; u.z = g.bone[i][2]; u.w = g.bone[i][3];
            C3D_FVUnifDirty[GPU_VERTEX_SHADER][g_uniform_bones + i] = true;
        }
        g_stat_unif += 3;
        g_pica_bone_gen[s] = g.bone_gen[s];
    }
    w1 = cmd_words();
    if (w1 >= w0) g_words_bind += w1 - w0;
    return true;
}

int draw_g3_direct(const State &st, std::uint64_t clamp, const ::g3::Input &in, ::g3::Gpu &g, const ::g3::BatchPlan &plan) {
    PS2_PROF_EXTRA(0);
    if (g_draw_nothing || (!queue::g_split_pica && queue::g_quiet) || g.n < 3) return 0;
    if (!g_frame_open) begin_frame();
    if (FesOverlapWordsLeft() < kG3Words + kFrameReserveWords) {
        ++g_stat_g3_declined;
        return -1;
    }
    std::uint32_t w0 = cmd_words();
    setup_batch(st, clamp);
    std::uint32_t w1 = cmd_words();
    if (w1 >= w0) g_words_setup += w1 - w0;
    if (g_skip_batch) return 0;
    if (g_index_used > g_index_first) flush_batch();

    float unif[15][4];
    for (int i = 0; i < 4; ++i) std::memcpy(unif[i], g.proj[i], sizeof unif[i]);
    unif[4][0] = g.offset[0] - static_cast<float>(g_ofx) * (1.0f / 16.0f);
    unif[4][1] = g.offset[1] - static_cast<float>(g_ofy) * (1.0f / 16.0f);
    unif[4][2] = g.offset[2];
    unif[4][3] = -g_z_scale * (g.xyzf ? 1.0f : 16.0f);
    unif[5][0] = g_s_scale; unif[5][1] = -g_t_scale; unif[5][2] = g_v_offset; unif[5][3] = 0.0f;
    unif[6][0] = g.fog ? g.fog_scale * (1.0f / 255.0f) : 0.0f;
    unif[6][1] = g.fog ? -g.fog_near * g.fog_scale * (1.0f / 255.0f) : static_cast<float>((st.fog >> 56) & 0xFFu) * (1.0f / 255.0f);
    unif[6][2] = g.clipper ? 0.0f : -1.0f;
    unif[6][3] = g.clipper ? -1e15f : 0.0f;
    for (int i = 0; i < 4; ++i) unif[7][i] = g.material[i] * (1.0f / 255.0f);
    std::memcpy(unif[8], g.ambient, sizeof unif[8]);
    for (int i = 0; i < 3; ++i) std::memcpy(unif[9 + i], g.light_dir[i], sizeof unif[9 + i]);
    for (int i = 0; i < 3; ++i) std::memcpy(unif[12 + i], g.light_colour[i], sizeof unif[12 + i]);
    const GPU_CULLMODE cull = g.keep_negative && g.keep_positive ? GPU_CULL_NONE : g.keep_negative ? GPU_CULL_FRONT_CCW : GPU_CULL_BACK_CCW;

    std::uint32_t unif_u32[15][4];
    std::memcpy(unif_u32, unif, sizeof unif_u32);
    std::uint32_t diff_mask = g_unif_valid ? 0u : 0x7FFFu;
    if (g_unif_valid) {
        for (int i = 0; i < 15; ++i) {
            const std::uint32_t d = (g_unif_bits[i][0] ^ unif_u32[i][0]) | (g_unif_bits[i][1] ^ unif_u32[i][1]) |
                                    (g_unif_bits[i][2] ^ unif_u32[i][2]) | (g_unif_bits[i][3] ^ unif_u32[i][3]);
            if (d) diff_mask |= 1u << i;
        }
    }
    const bool shared_uniforms = diff_mask == 0u;
    const bool may_join = g_g3_pending.on && shared_uniforms && g.skin == g_g3_skin && g.ortho == g_g3_ortho && cull == g_g3_cull;
    if (g_g3_pending.on && !may_join) flush_g3();

    const std::uint32_t worst = (g_ring_used + 2u) / 3u * 3u + (g.n + 5u) / 2u * 3u;
    if (!reserve_geometry(worst > g_index_used ? worst - g_index_used : 1u)) {
        if (!g_ring_warned) std::fprintf(stderr, "[pica] vertex ring full, dropping shapes\n");
        g_ring_warned = true;
        return 0;
    }
    auto *ring = reinterpret_cast<::g3::GpuVertex *>(g_ring);
    const std::uint32_t end = g_g3_pending.on ? (g_g3_pending.first + g_g3_pending.count) : 0u;
    const std::uint32_t at = g_g3_pending.on ? ((end + 3u) & ~1u) : ((g_ring_used + 2u) / 3u * 2u);
    const int fill_rc = ::g3::gpu_batch_fill(in, g, plan, ring + at);
    if (fill_rc != 0) return fill_rc;

    ++g_stat_shapes;
    ++g_stat_g3;
    const std::uint32_t slots = g.skin ? (g.slots | 1u) : 0u;
    bool join = g_g3_pending.on;
    for (auto overlap = g_g3_pending.slots & slots; overlap && join; overlap &= overlap - 1u) {
        const unsigned s = __builtin_ctz(overlap);
        join = g.bone_gen[s] == g_pica_bone_gen[s];
    }
    if (g_g3_pending.on && !join) flush_g3();
    if (g_g3_pending.on) {
        ring[end] = ring[end - 1u];
        ring[end + 1u] = ring[at];
        if (at - end == 3u) ring[end + 2u] = ring[at];
        g_g3_pending.count = at + g.n - g_g3_pending.first;
        g_g3_pending.slots |= slots;
        g_g3_pending.alpha_uniform = g_g3_pending.alpha_uniform && g.alpha_uniform && (g_g3_pending.alpha_val == g.alpha_val);
    } else {
        g_g3_pending = G3Pending{true, at, g.n, slots, g.material[3], g.alpha_uniform, g.alpha_val};
    }
    g_ring_used = ((at + g.n) * 3u + 1u) / 2u;
    g_index_used = std::max(g_index_used, g_ring_used);
    g_index_first = g_index_used;
    g_last_triangle_end = 0;

    w0 = cmd_words();
    if (!g_g3_bound) {
        C3D_SetAttrInfo(&g_g3_attr);
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, g_bool_g3, true);
        g_g3_bound = true;
        g_bound_base = UINT32_MAX;
    }
    if (g_g3_buffer != g_ring) {
        BufInfo_Init(&g_buf);
        BufInfo_Add(&g_buf, g_ring, sizeof(::g3::GpuVertex), 6, 0x543210);
        C3D_SetBufInfo(&g_buf);
        g_g3_buffer = g_ring;
    }
    if (g.skin != g_g3_skin) {
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, g_bool_skin, g.skin);
        g_g3_skin = g.skin;
    }
    if (g.ortho != g_g3_ortho) {
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, g_bool_ortho, g.ortho);
        g_g3_ortho = g.ortho;
    }
    if (cull != g_g3_cull) {
        C3D_CullFace(cull);
        g_g3_cull = cull;
    }
    if (diff_mask) {
        for (std::uint32_t m = diff_mask; m; m &= m - 1u) {
            const int i = __builtin_ctz(m);
            C3D_FVec &u = C3D_FVUnif[GPU_VERTEX_SHADER][g_uniform_proj + i];
            u.x = unif[i][0]; u.y = unif[i][1]; u.z = unif[i][2]; u.w = unif[i][3];
            C3D_FVUnifDirty[GPU_VERTEX_SHADER][g_uniform_proj + i] = true;
            ++g_stat_unif;
            g_unif_bits[i][0] = unif_u32[i][0]; g_unif_bits[i][1] = unif_u32[i][1];
            g_unif_bits[i][2] = unif_u32[i][2]; g_unif_bits[i][3] = unif_u32[i][3];
        }
        g_unif_valid = true;
    }
    for (auto remaining = slots; remaining; remaining &= remaining - 1u) {
        const unsigned s = __builtin_ctz(remaining);
        if (g.bone_gen[s] == g_pica_bone_gen[s]) continue;
        const unsigned first = 3u * s;
        for (unsigned i = first; i < first + 3; ++i) {
            C3D_FVec &u = C3D_FVUnif[GPU_VERTEX_SHADER][g_uniform_bones + i];
            u.x = g.bone[i][0]; u.y = g.bone[i][1]; u.z = g.bone[i][2]; u.w = g.bone[i][3];
            C3D_FVUnifDirty[GPU_VERTEX_SHADER][g_uniform_bones + i] = true;
        }
        g_stat_unif += 3;
        g_pica_bone_gen[s] = g.bone_gen[s];
    }
    w1 = cmd_words();
    if (w1 >= w0) g_words_bind += w1 - w0;
    return 0;
}

bool draw_im2d_tris(const State &st, std::uint64_t clamp, const std::uint8_t *vu_mem, std::uint32_t top, std::uint32_t count, bool xyzf) {
    PS2_PROF_EXTRA(0);
    if (g_draw_nothing || (!queue::g_split_pica && queue::g_quiet) || count == 0) return true;
    if (!g_frame_open) begin_frame();
    setup_batch(st, clamp);
    if (g_skip_batch) return true;
    if (g_fst) return false;
    flush_g3();
    g_last_triangle_end = 0;

    float off[2], cmul[4];
    std::memcpy(off, vu_mem + 1017u * 16u, 8);
    std::memcpy(cmul, vu_mem + 1019u * 16u, 16);
    const std::uint8_t fog = xyzf ? 0xFFu : static_cast<std::uint8_t>((st.fog >> 56) & 0xFFu);
    const bool iip = g_iip;
    const bool tex = g_textured;
    const float z_scale = g_z_scale;
    const float s_scale = g_s_scale, t_scale = g_t_scale, v_off = g_v_offset;
    const int ofx = g_ofx, ofy = g_ofy;
    const int cl = g_clip_left, cr = g_clip_right, ct = g_clip_top, cb = g_clip_bottom;

    auto ftoi4 = [](float v) -> std::uint32_t {
        const float s = v * 16.0f;
#ifdef __3DS__
        return static_cast<std::uint32_t>(static_cast<std::int32_t>(s));
#else
        if (s >= 2147483648.0f) return 0x7FFFFFFFu;
        if (s <= -2147483648.0f) return 0x80000000u;
        return static_cast<std::uint32_t>(static_cast<std::int32_t>(s));
#endif
    };
    auto ftoi0 = [](float v) -> std::uint8_t {
#ifdef __3DS__
        return static_cast<std::uint8_t>(static_cast<std::int32_t>(v) & 0xFF);
#else
        if (v >= 2147483648.0f) return 0xFFu;
        if (v <= -2147483648.0f) return 0x00u;
        return static_cast<std::uint8_t>(static_cast<std::int32_t>(v) & 0xFF);
#endif
    };

    const std::uint32_t need = count * 3u;
    if (!reserve_geometry(need)) {
        if (!g_ring_warned) std::fprintf(stderr, "[pica] vertex ring full, dropping shapes\n");
        g_ring_warned = true;
        return true;
    }
    reserve_indices(need);
    g_stat_shapes += count;
    g_stat_prim[st.prim & 7u] += count;
    g_shapes_since_present += count;

    for (std::uint32_t ti = 0; ti < count; ++ti) {
        const std::uint32_t base = top + 12u * ti;
        int ix[3], iy[3];
        std::uint32_t uz[3];
        float u0[3], v0[3], q[3];
        std::uint32_t col[3];
        for (int j = 0; j < 3; ++j) {
            const std::uint32_t qv = base + 4u * static_cast<std::uint32_t>(j);
            const auto *vp = reinterpret_cast<const float *>(vu_mem + ((qv + 0u) & 1023u) * 16u);
            const auto *vst = reinterpret_cast<const float *>(vu_mem + ((qv + 1u) & 1023u) * 16u);
            ix[j] = static_cast<int>(ftoi4(vp[0] + off[0]) & 0xFFFFu);
            iy[j] = static_cast<int>(ftoi4(vp[1] + off[1]) & 0xFFFFu);
            const std::uint32_t zraw = ftoi4(vp[2]);
            uz[j] = xyzf ? ((zraw >> 4) & 0xFFFFFFu) : zraw;
            q[j] = vst[2];
            if (tex) {
                u0[j] = vst[0];
                v0[j] = vst[1];
            }
            if (iip || j == 2) {
                const auto *vc = reinterpret_cast<const float *>(vu_mem + ((qv + 2u) & 1023u) * 16u);
                col[j] = static_cast<std::uint32_t>(ftoi0(vc[0] * cmul[0])) |
                         (static_cast<std::uint32_t>(ftoi0(vc[1] * cmul[1])) << 8) |
                         (static_cast<std::uint32_t>(ftoi0(vc[2] * cmul[2])) << 16) |
                         (static_cast<std::uint32_t>(ftoi0(vc[3] * cmul[3])) << 24);
            }
        }
        auto same_pos = [&](int a, int b) { return ix[a] == ix[b] && iy[a] == iy[b] && uz[a] == uz[b] && q[a] == q[b]; };
        if (same_pos(0, 1) || same_pos(1, 2) || same_pos(0, 2)) continue;
        if ((ix[0] < cl && ix[1] < cl && ix[2] < cl) || (ix[0] > cr && ix[1] > cr && ix[2] > cr) ||
            (iy[0] < ct && iy[1] < ct && iy[2] < ct) || (iy[0] > cb && iy[1] > cb && iy[2] > cb))
            continue;
        DrawVertex *o = g_ring + g_ring_used;
        auto *idx = g_indices + g_index_used;
        const std::uint16_t first = vertex_index(g_ring_used);
        const std::uint32_t flat_col = col[2];
        for (int j = 0; j < 3; ++j) {
            o[j].x = static_cast<float>(ix[j] - ofx) * (1.0f / 16.0f);
            o[j].y = static_cast<float>(iy[j] - ofy) * (1.0f / 16.0f);
            o[j].z = std::max(-static_cast<float>(uz[j]) * z_scale, -1.0f);
            const std::uint32_t cc = iip ? col[j] : flat_col;
            std::memcpy(&o[j].r, &cc, 4);
            o[j].fog = fog;
            const float qj = q[j];
            if (qj == 1.0f) {
                o[j].w = 1.0f;
                if (tex) {
                    o[j].s = u0[j] * s_scale;
                    o[j].t = v_off - v0[j] * t_scale;
                } else {
                    o[j].s = o[j].t = 0.0f;
                }
            } else if (qj > 1e-6f) {
                o[j].w = 1.0f / qj;
                if (tex) {
                    o[j].s = u0[j] * s_scale;
                    o[j].t = v_off - v0[j] * t_scale;
                } else {
                    o[j].s = o[j].t = 0.0f;
                }
            } else {
                const float iq = 1.0f / 1e-6f;
                o[j].w = iq;
                if (tex) {
                    o[j].s = (u0[j] * qj) * iq * s_scale;
                    o[j].t = v_off - (v0[j] * qj) * iq * t_scale;
                } else {
                    o[j].s = o[j].t = 0.0f;
                }
            }
            idx[j] = static_cast<std::uint16_t>(first + j);
        }
        g_ring_used += 3u;
        g_index_used += 3u;
    }
    return true;
}

void draw(const State &st, std::uint32_t serial, std::uint64_t clamp) {
    PS2_PROF_EXTRA(0);   // for SAMPLE=1: shapes in, x1 to x3 below are apart
    if (g_draw_nothing || (!queue::g_split_pica && queue::g_quiet)) return;
    if (!g_frame_open) begin_frame();
    setup_batch(st, clamp);
    if (g_skip_batch) return;
    flush_g3();   // g3 batches waiting are older than this shape
    ++g_stat_shapes;
    ++g_stat_prim[st.prim & 7u];
    ++g_shapes_since_present;
    if (!reserve_geometry(8)) {
        if (!g_ring_warned) std::fprintf(stderr, "[pica] vertex ring full, dropping shapes\n");
        g_ring_warned = true;
        return;
    }
    const Vertex *v = st.queue;
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
        if (st.count >= 3) add_triangle(v, static_cast<unsigned>(st.prim & 7u), serial);
        break;
    case 6:
        add_quad(gs_x(v[0]), gs_y(v[0]), gs_x(v[1]), gs_y(v[1]), v[0], v[1]);
        break;
    default:
        break;
    }
}

void note_data(const std::uint8_t *data, std::size_t bytes, std::uint32_t known_hash) {
    if (!g_uploading) return;
    std::uint32_t h = g_upload_hash[g_upload_target];
    if (known_hash) {
        h = (((h << 5) | (h >> 27)) ^ known_hash ^ static_cast<std::uint32_t>(bytes)) * 0x9E3779B1u;
    } else {
        using word = std::uint32_t __attribute__((may_alias, aligned(4)));
        const word *w = reinterpret_cast<const word *>(data);
        for (std::size_t n = bytes / 8; n; --n, w += 2) h = (((h << 5) | (h >> 27)) ^ w[0] ^ ((w[1] << 11) | (w[1] >> 21))) * 0x9E3779B1u;
    }
    g_upload_hash[g_upload_target] = h ? h : 1u;
    g_stat_upload += static_cast<std::uint32_t>(bytes);
}

void note_transfer(const State &s, std::uint64_t dir) {
    g_uploading = false;
    if ((dir & 3u) == 1u) return;   // a read from GS memory changes nothing
    const std::uint32_t dbp = static_cast<std::uint32_t>(s.bitbltbuf >> 32) & 0x3FFFu;
    const std::uint32_t dbw = static_cast<std::uint32_t>(s.bitbltbuf >> 48) & 0x3Fu;
    const std::uint32_t dpsm = static_cast<std::uint32_t>(s.bitbltbuf >> 56) & 0x3Fu;
    const std::uint32_t dx = static_cast<std::uint32_t>(s.trxpos >> 32) & 0x7FFu;
    const std::uint32_t dy = static_cast<std::uint32_t>(s.trxpos >> 48) & 0x7FFu;
    const std::uint32_t rrw = static_cast<std::uint32_t>(s.trxreg) & 0xFFFu;
    const std::uint32_t rrh = static_cast<std::uint32_t>(s.trxreg >> 32) & 0xFFFu;
    const std::uint32_t pw = static_cast<std::uint32_t>(page_width(dpsm));
    const std::uint32_t ph = static_cast<std::uint32_t>(page_height(dpsm));
    const std::uint32_t pw_shift = (pw == 128u) ? 7u : 6u;
    const std::uint32_t ph_shift = (ph == 128u) ? 7u : (ph == 64u ? 6u : 5u);
    const std::uint32_t wide = std::max<std::uint32_t>((std::max<std::uint32_t>(dbw, 1u) << 6) >> pw_shift, 1u);
    const std::uint32_t pages = (((dy + rrh) >> ph_shift) + 1u) * wide;
    const std::uint32_t first = dbp >> 5;
    ++g_stamp;
    if ((dir & 3u) == 0u) {
        // A transfer to the corner starts a new picture; later ones add to it.
        const std::uint32_t pos = static_cast<std::uint32_t>(s.trxpos >> 32);
        if (!pos) g_upload_hash[dbp] = 0x811C9DC5u;
        g_upload_hash[dbp] ^= pos * 0x01000193u + static_cast<std::uint32_t>(s.trxreg) + static_cast<std::uint32_t>(s.trxreg >> 32) * 31u;
        if (!pos || !g_upload_stamp[dbp]) g_upload_stamp[dbp] = g_stamp;
        g_upload_target = dbp;
        g_uploading = true;
    } else {
        g_upload_hash[dbp] = 0;
        g_upload_stamp[dbp] = 0;
    }
    for (std::uint32_t p = first; p < first + pages && p < 512u; ++p) g_page_stamp[p] = g_stamp;
    std::uint32_t b0 = dbp, b1 = dbp;
    if ((dir & 3u) == 0u && dx + rrw <= 2048u && dy + rrh <= 2048u && rrw && rrh) {
        upload_blocks(dpsm, dbp, dbw, dx, dy, rrw, rrh, b0, b1);
    } else {
        b0 = first * 32u;
        b1 = (first + pages) * 32u;
    }
    if (b1 > 16384u) b1 = 16384u;
    const std::uint32_t upload = (dir & 3u) == 0u ? g_upload_stamp[dbp] : 0u;
    for (std::uint32_t b = b0; b < b1; ++b) {
        g_block_stamp[b] = g_stamp;
        g_block_upload[b] = upload;
    }
}

void note_clut() { ++g_clut_stamp; }

namespace {

bool skip_fixed_picture(std::uint32_t picture, std::uint32_t interval) {
    // Alternate gaps of one and three pictures at half rate so both PS2
    // framebuffers get refreshed. The game-side prediction uses this too.
    if (interval == 2) return (picture & 2u) != 0;
    return picture % interval != 0;
}

// Queue pressure measures drawing cost. When building pictures on the game thread is the
// bottleneck, the drawing queue can be idle even though the game misses its vblank budget.
struct PicturePacing {
    u64 start = 0;
    std::uint32_t pictures = 0, cut = 0, minimum_skip = 0, on_time = 0;

    void observe(u64 now, unsigned vblanks, bool could_cut) {
        if (!start) {
            start = now;
            return;
        }
        cut += could_cut;
        if (++pictures < 32) return;
        constexpr u64 frame_ticks = static_cast<u64>(SYSCLOCK_ARM11) * 1001 / 60000;
        const u64 budget = frame_ticks * vblanks * pictures;
        if (!cut) {
            minimum_skip = on_time = 0;
        } else if (now - start > budget + budget / 40) {
            minimum_skip = std::min<std::uint32_t>(8u, g_skip_run + 1u);
            on_time = 0;
        } else if (++on_time >= 3) {
            if (minimum_skip) --minimum_skip;
            on_time = 0;
        }
        start = now;
        pictures = cut = 0;
    }
};

void choose_next_picture() {
    static PicturePacing pacing;
    ++g_picture;
    bool skip = false;
    if (g_draw_every) pacing = {};
    if (g_draw_every > 1) {
        skip = skip_fixed_picture(g_picture, g_draw_every);
    } else if (!g_draw_every && queue::vu_active()) {
        const u64 now = svcGetSystemTick();
        pacing.observe(now, g_present_every, g_render_left_out);
        if (g_skipped_in_row >= g_skip_now) {
            const u64 round = now - g_round_start, tick_share = round / 16;
            if (g_round_blocked > tick_share && g_skip_run < 5) ++g_skip_run;
            else if (g_skip_run && (g_round_idle > tick_share * 8 || g_skip_run > 5)) --g_skip_run;
            g_skip_run = std::max(g_skip_run, pacing.minimum_skip);
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
    // The game builds a picture before this decision is made for it, and the lossy render cut (lossy_return) goes by the
    // guess below. Should a picture it left out come up to be shown after all, it is skipped instead, and the last one stays up.
    if (g_render_left_out && !skip) {
        skip = true;
        g_stat_forced.fetch_add(1, std::memory_order_relaxed);
    }
    g_render_left_out = false;
    g_skipped_in_row = skip ? g_skipped_in_row + 1 : 0;
    if (skip) g_stat_skipped.fetch_add(1, std::memory_order_relaxed);
    const bool test_aid = skip && (g_debug & 4u);   // skip the drawing but still run the vector programs
    queue::g_ee_skip = skip && !test_aid;
    // Whether the picture after this one will be skipped, as things stand. The game works it out during this one.
    if (g_draw_every > 1) g_after_hidden = skip_fixed_picture(g_picture + 1, g_draw_every);
    else g_after_hidden = !g_draw_every && queue::vu_active() && !test_aid && g_skipped_in_row < g_skip_now;
    g_next_picture = (skip && !test_aid ? kSkip : 0u) | (test_aid ? kDrawNothing : 0u) | (g_skip_run || g_draw_every > 1 ? kSkipping : 0u);
}

void show_picture(std::uint32_t frame, const DisplayRegs &r);

}

// The game thread decides, as it starts each picture, so it can leave out what a skipped picture will not use.
std::uint32_t next_picture() {
    if (!g_present_every || ++g_game_vblanks % g_present_every) return g_next_picture;
    using namespace ps2::prof;
    const auto blocked = g_ticks[Full].load(std::memory_order_relaxed);
    const auto idle = g_ticks[Wait].load(std::memory_order_relaxed);
    g_round_blocked += blocked - g_seen_blocked;
    g_round_idle += idle - g_seen_idle;
    g_seen_blocked = blocked;
    g_seen_idle = idle;
    choose_next_picture();
    return g_next_picture;
}

void advance_vu_picture(std::uint32_t picture) {
    queue::g_vu_skip_picture = picture & kSkip;
}

void vblank(std::uint32_t frame, const DisplayRegs &r, std::uint32_t picture) {
    g_frame_number = frame;
    ++g_vblanks;
    age_deferred_uploads();
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
    if (g_overlap_picture && movie::overlap_needs_wait()) FesOverlapWait();
    if (const movie::Picture ours = movie::picture(free_oldest); ours.playing) {
        // One of the build's own movie pictures instead of the game's black one. Until the first is ready, what was up stays up.
        if (ours.tex) {
            show_texture(g_screen, ours.tex, 0.0f, static_cast<float>(kScreenWidth) / 512.0f, 1.0f - static_cast<float>(kScreenHeight) / 256.0f, GPU_NEAREST);
            if (g_two_screens) show_texture(g_screen_right, ours.tex, 0.0f, static_cast<float>(kScreenWidth) / 512.0f, 1.0f - static_cast<float>(kScreenHeight) / 256.0f, GPU_NEAREST);
        }
    } else if (circuit >= 0) {
        const std::uint32_t fbp = static_cast<std::uint32_t>(r.dispfb[circuit]) & 0x1FFu;
        for (Target &t : g_targets)
            if (t.used && t.fbp == fbp) {
                // Many scenes swap between two frame buffers, drawing into the one not on show. Drawing one picture in two or four
                // would then always land on the same buffer, and the one on show would never change. So when pictures are
                // being skipped, a buffer of the same size that was just drawn goes on show instead.
                Target *show = &t;
                if (g_skipping && t.last_used != g_frame)
                    for (Target &o : g_targets)
                        if (o.used && o.last_used == g_frame && o.width == t.width && o.tex.height == t.tex.height) show = &o;
                show_target(show, r, circuit);
                break;
            }
    }
    flush_ring();
    if (g_debug & 8u) {
        std::fprintf(stderr, "[pica] depths, far to near:");
        for (std::uint32_t &n : g_depth_seen) {
            std::fprintf(stderr, " %lu", static_cast<unsigned long>(n));
            n = 0;
        }
        std::fputc('\n', stderr);
    }
    if (g_stats) {
        static u64 last = svcGetSystemTick();
        const u64 now = svcGetSystemTick();
        const auto ms = [](u64 ticks) { return static_cast<unsigned long>(ticks / static_cast<u64>(CPU_TICKS_PER_MSEC)); };
        using namespace ps2::prof;
        const u64 total = now - last;
        static std::uint32_t previous_ticks[Count]{};
        u64 ticks[Count];
        for (int i = 0; i < Count; ++i) {
            const auto current = g_ticks[i].load(std::memory_order_relaxed);
            ticks[i] = current - previous_ticks[i];
            previous_ticks[i] = current;
        }
        u64 known = ticks[Vu] + ticks[Upload] + ticks[Iop] + ticks[Ipu];   // Gif is inside Vu
        static std::uint64_t last_interpreted = 0, last_clock = 0, last_idle = 0;
        static std::uint32_t last_frame = 0;
        std::fprintf(stderr, "[pica] %lu game frames in %lu ms, %lu ms each, %lu pictures skipped (%lu of them with their render left out by mistake)\n", static_cast<unsigned long>(frame - last_frame), ms(total), frame != last_frame ? ms(total) / static_cast<unsigned long>(frame - last_frame) : 0ul, static_cast<unsigned long>(g_stat_skipped.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long>(g_stat_forced.exchange(0, std::memory_order_relaxed)));
        last_frame = frame;
        const std::uint64_t interpreted = ps2::vu1_interpreted_calls();
        const std::uint64_t clock = ps2::now(), idled = ps2::idle_cycles();
        std::fprintf(stderr, "[pica] ee %llu K cycles, %llu K idle | prims pt %lu ln %lu strip %lu tri %lu tstrip %lu fan %lu sprite %lu | upload %lu KB\n", static_cast<unsigned long long>((clock - last_clock) / 1000u), static_cast<unsigned long long>((idled - last_idle) / 1000u),
                     static_cast<unsigned long>(g_stat_prim[0]), static_cast<unsigned long>(g_stat_prim[1]), static_cast<unsigned long>(g_stat_prim[2]), static_cast<unsigned long>(g_stat_prim[3]), static_cast<unsigned long>(g_stat_prim[4]), static_cast<unsigned long>(g_stat_prim[5]), static_cast<unsigned long>(g_stat_prim[6]), static_cast<unsigned long>(g_stat_upload >> 10));
        std::fprintf(stderr, "[pica] vif %lu (with vu) dma %lu events %lu (with iop, present) kernel %lu present %lu | vu thread idle %lu, ee thread blocked %lu\n", ms(ticks[Vif]), ms(ticks[Dma]), ms(ticks[Events]), ms(ticks[Kernel]), ms(ticks[Present]), ms(ticks[Wait]), ms(ticks[Full]));
        last_clock = clock;
        last_idle = idled;
        std::fprintf(stderr, "[pica] frame %llu: %lu ms: game %lu vu %lu (gif %lu of it, interpreted calls %lu) upload %lu draw %lu iop %lu (spu %lu) ipu %lu | %lu shapes %lu draws %lu changes %lu splits %lu decoded %lu texels %lu reused %lu missing %lu verts, %lu KB free\n",
                     static_cast<unsigned long long>(frame), ms(total), ms(total > known ? total - known : 0), ms(ticks[Vu]), ms(ticks[Gif]), static_cast<unsigned long>(interpreted - last_interpreted), ms(ticks[Upload]), ms(ticks[Draw]), ms(ticks[Iop]),
                     ms(ticks[Spu]), ms(ticks[Ipu]), static_cast<unsigned long>(g_stat_shapes), static_cast<unsigned long>(g_stat_draws), static_cast<unsigned long>(g_stat_state),
                     static_cast<unsigned long>(g_stat_splits), static_cast<unsigned long>(g_stat_decoded), static_cast<unsigned long>(g_stat_decode_texels), static_cast<unsigned long>(g_stat_reused), static_cast<unsigned long>(g_stat_missing),
                     static_cast<unsigned long>(g_ring_used), static_cast<unsigned long>(linearSpaceFree() >> 10));
        if (g_stat_g3 || g_stat_g3_declined)
            std::fprintf(stderr, "[pica] g3 on the GPU %lu, handed back %lu, uniform vectors sent %lu; command words left %lu\n",
                         static_cast<unsigned long>(g_stat_g3), static_cast<unsigned long>(g_stat_g3_declined), static_cast<unsigned long>(g_stat_unif),
                         static_cast<unsigned long>(FesOverlapWordsLeft()));
        if (g_stat_g3)
            std::fprintf(stderr, "[pica] g3 command words: setup %lu, GS flush %lu, binding %lu, draw %lu\n", static_cast<unsigned long>(g_words_setup),
                         static_cast<unsigned long>(g_words_flush), static_cast<unsigned long>(g_words_bind), static_cast<unsigned long>(g_words_draw));
        last_interpreted = interpreted;
        last = svcGetSystemTick();
    }
    g_stat_g3 = g_stat_g3_declined = g_stat_unif = 0;
    g_words_setup = g_words_flush = g_words_bind = g_words_draw = 0;
    g_batch_number = 0;
    g_stat_reused = g_stat_missing = 0;
    g_stat_shapes = g_stat_draws = g_stat_state = g_stat_splits = g_stat_decoded = g_stat_decode_texels = 0;
    g_stat_decode_ticks = 0;
    g_shapes_since_present = 0;
    std::memset(g_stat_prim, 0, sizeof g_stat_prim);
    g_stat_upload = 0;
    // GX flushes the finalized command list. Textures and vertex data have already been flushed.
    FesOverlapEnd(GX_CMDLIST_FLUSH);
    g_frame_open = false;
    if ((g_debug & 32u) && (g_frame % 15) == 0) {
        static u64 previous_wait = 0;
        const u64 total_wait = FesOverlapWaitTicks();
        u32 peak; unsigned expansions;
        FesOverlapCommandStats(&peak, &expansions);
        std::fprintf(stderr, "[overlap] frame %lu mode %d begin %.3f wait %.3f record %.3f splits %u cmd_bytes %u expansions %u\n",
                     static_cast<unsigned long>(frame), g_overlap_picture,
                     static_cast<double>(g_profile_begin) / (15 * CPU_TICKS_PER_MSEC),
                     static_cast<double>(total_wait - previous_wait) / (15 * CPU_TICKS_PER_MSEC),
                     C3D_GetProcessingTime(), g_overflow_waits, peak * 4u, expansions);
        previous_wait = total_wait;
        g_profile_begin = 0;
        g_overflow_waits = 0;
    }
    if (g_shot_every && frame % g_shot_every < g_present_every) {
        const bool both = g_two_screens;
        save_shot(g_screen, "");
        if (both) save_shot(g_screen_right, "_r");
    }
}

}

}
