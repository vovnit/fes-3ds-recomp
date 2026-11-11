// Widescreen for the iOS build (the Screen setting in ios_main.mm). Three parts work together:
//   1. ios_main.mm widens the game's 3D view (the x field of view at 007cad1c, as in the PC window's widescreen mode).
//   2. The picture is shown at that wider shape: present() in the iOS copy of gpu.cpp reads portios_aspect() (CMakeLists.txt).
//   3. Everything flat the game draws (menus, text, portraits, the HUD, movies) is narrowed toward the middle here, before the
//      GPU renderer draws it, by exactly as much as step 2 widens the picture. So it ends up where and as wide as in 4:3,
//      while the 3D scene fills the screen.
// The game still draws one 640-wide picture; nothing in the PS2 memory layout changes. What counts as flat (see portios_widen):
//   * only drawing into the picture that is shown (not into textures, shadows or other work buffers)
//   * P3 draws its 2D layer through the same path as the 3D scene (depth tested, perspective texture coordinates), but at one
//     fixed distance: all corners of a triangle share one q and one depth. In front of everything (depth 0xF000 and up) for
//     menus, text and the HUD; textured at a large q (0.05) for logo screens. The scene's own triangles vary, or sit far back.
//   * not a copy of the picture itself (screen effects that read back what was drawn), and not an untextured shape across
//     the whole width (fades, black bars, the clear at the start of a frame): those must keep covering the whole screen
// Narrowed drawing is also clipped to the 4:3 part, so menu parts the game keeps just past the edge stay out of sight.
// Checked in the Simulator (11 November 2026): field with HUD, dialogue with portraits, battle, level up, Velvet Room, an FMV,
// the logo screen. P3FES_WIDE_LOG=1 prints how one frame in 120 was drawn, by kind, to Documents/log.txt.
#include "gs/gs.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

using ps2::gs::State;
using ps2::gs::Vertex;

std::atomic<float> g_portios_aspect{4.0f / 3.0f};   // shape of the picture on screen; ios_main.mm sets it

namespace {

std::uint32_t g_shown[2] = {~0u, ~0u};   // the last two buffers shown (block addresses): the game draws into one while the other is shown
constexpr int kTargets = 8;
std::uint32_t g_targets[kTargets] = {}, g_last_targets[kTargets] = {};   // buffers drawn into this frame and the one before
int g_ntargets = 0, g_nlast = 0;
int g_clip_lo = 0, g_clip_hi = 0;   // set for narrowed drawing: the 4:3 part of the picture it is limited to
bool g_clip = false;

const bool g_log = std::getenv("P3FES_WIDE_LOG") != nullptr;
std::uint64_t g_frames = 0;
struct Row {
    std::uint64_t key;
    int n;
    float x0, y0, x1, y1, q0, q1;
    std::uint32_t z0, z1;
};
Row g_rows[96];
int g_nrows = 0;

bool is_target(std::uint32_t block) {
    for (int i = 0; i < g_ntargets; ++i)
        if (g_targets[i] == block) return true;
    for (int i = 0; i < g_nlast; ++i)
        if (g_last_targets[i] == block) return true;
    return false;
}

void note_target(std::uint32_t block) {
    for (int i = 0; i < g_ntargets; ++i)
        if (g_targets[i] == block) return;
    if (g_ntargets < kTargets) g_targets[g_ntargets++] = block;
}

// what: 0 left alone (not the shown picture), 1 3D, 2 narrowed, 3 whole width kept, 4 copy of a drawn buffer
void note(std::uint32_t fbp, std::uint32_t fbw, std::uint32_t prim, std::uint64_t attr, std::uint64_t test, std::uint32_t tbp, int what,
          float x0, float y0, float x1, float y1, float q0, float q1, std::uint32_t z0, std::uint32_t z1) {
    const std::uint64_t key = static_cast<std::uint64_t>(fbp) | (static_cast<std::uint64_t>(fbw) << 14) | (static_cast<std::uint64_t>(prim) << 20) |
                              (((attr >> 4) & 1ull) << 23) | (((attr >> 8) & 1ull) << 24) | (((attr >> 6) & 1ull) << 25) |
                              (((test >> 16) & 7ull) << 26) | (static_cast<std::uint64_t>(what) << 29) |
                              (((attr >> 4) & 1ull) ? static_cast<std::uint64_t>(tbp) << 32 : 0);
    for (int i = 0; i < g_nrows; ++i) {
        Row &r = g_rows[i];
        if (r.key != key) continue;
        ++r.n;
        r.x0 = std::min(r.x0, x0);
        r.y0 = std::min(r.y0, y0);
        r.x1 = std::max(r.x1, x1);
        r.y1 = std::max(r.y1, y1);
        r.q0 = std::min(r.q0, q0);
        r.q1 = std::max(r.q1, q1);
        r.z0 = std::min(r.z0, z0);
        r.z1 = std::max(r.z1, z1);
        return;
    }
    if (g_nrows < static_cast<int>(sizeof g_rows / sizeof g_rows[0])) g_rows[g_nrows++] = {key, 1, x0, y0, x1, y1, q0, q1, z0, z1};
}

void print_rows() {
    static const char *what[] = {"other", "3d", "narrow", "whole", "copy"};
    std::fprintf(stderr, "[wide] frame %llu, shown %x/%x, aspect %.3f\n", static_cast<unsigned long long>(g_frames), g_shown[0], g_shown[1],
                 g_portios_aspect.load());
    for (int i = 0; i < g_nrows; ++i) {
        const Row &r = g_rows[i];
        const std::uint64_t k = r.key;
        std::fprintf(stderr, "[wide]  %-6s fb %4llx w%-2llu prim %llu tme %llu fst %llu abe %llu zte %llu ztst %llu tex %4llx  n %4d  x %.0f..%.0f y %.0f..%.0f q %g..%g z %x..%x\n",
                     what[(k >> 29) & 7], k & 0x3FFF, (k >> 14) & 0x3F, (k >> 20) & 7, (k >> 23) & 1, (k >> 24) & 1, (k >> 25) & 1, (k >> 26) & 1,
                     (k >> 27) & 3, k >> 32, r.n, r.x0, r.x1, r.y0, r.y1, r.q0, r.q1, r.z0, r.z1);
    }
}

}  // namespace

// draw() calls this with the scissor's left and right columns: narrowed drawing stays inside the 4:3 part, so what the game
// keeps out of sight past the right or left edge (it parks some menu parts there) does not slide into view
void portios_clip(int &x0, int &x1) {
    if (!g_clip) return;
    x0 = std::max(x0, g_clip_lo);
    x1 = std::min(x1, g_clip_hi);
}

float portios_aspect() { return g_portios_aspect.load(std::memory_order_relaxed); }

// present() calls this with the buffer it shows
void portios_presented(std::uint32_t fbp) {
    if (fbp != g_shown[0]) {
        g_shown[1] = g_shown[0];
        g_shown[0] = fbp;
    }
    std::copy(g_targets, g_targets + g_ntargets, g_last_targets);
    g_nlast = g_ntargets;
    g_ntargets = 0;
    ++g_frames;
    if (g_log) {   // the game draws at 30 frames a second, so a picture is shown twice: print the next one that was drawn
        static bool due = false;
        if (g_frames % 120 == 0) due = true;
        if (due && g_nrows) {
            print_rows();
            due = false;
        }
        g_nrows = 0;
    }
}

// draw() in the iOS copy of gpu.cpp starts with this: it returns the vertices to draw, either v itself or narrowed copies in out
const Vertex *portios_widen(const State &st, const Vertex *v, int count, Vertex *out) {
    const float k = (4.0f / 3.0f) / g_portios_aspect.load(std::memory_order_relaxed);
    if (k > 0.999f && !g_log) return v;
    std::uint64_t attr = st.prim;
    if (!(st.prmodecont & 1u)) attr = (st.prim & 7u) | (st.prmode & ~7ull);
    const std::uint32_t prim = static_cast<std::uint32_t>(st.prim) & 7u;
    const ps2::gs::Context &c = st.ctx[(attr >> 9) & 1u];
    const std::uint32_t fbp = static_cast<std::uint32_t>(c.frame & 0x1FFu) * 32, fbw = static_cast<std::uint32_t>(c.frame >> 16) & 0x3Fu;
    const std::uint64_t test = c.test;
    const bool depth = ((test >> 16) & 1u) && ((test >> 17) & 3u) >= 2;
    const bool textured = (attr >> 4) & 1u;
    const std::uint32_t tbp = static_cast<std::uint32_t>(c.tex0 & 0x3FFFu);
    const int ofx = static_cast<int>(c.xyoffset & 0xFFFFu), ofy = static_cast<int>((c.xyoffset >> 32) & 0xFFFFu);
    const int n = prim == 0 ? 1 : prim == 6 || prim == 1 || prim == 2 ? 2 : 3;
    if (count < n) return v;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, q0 = 1e9f, q1 = -1e9f;
    std::uint32_t z0 = ~0u, z1 = 0;
    for (int i = 0; i < n; ++i) {
        q0 = std::min(q0, v[i].q);
        q1 = std::max(q1, v[i].q);
        z0 = std::min(z0, v[i].z);
        z1 = std::max(z1, v[i].z);
        const float x = (static_cast<float>(v[i].x) - static_cast<float>(ofx)) / 16.0f, y = (static_cast<float>(v[i].y) - static_cast<float>(ofy)) / 16.0f;
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
    }
    if (depth) note_target(fbp);
    const float width = static_cast<float>((fbw ? fbw : 10) * 64);
    // The game's 2D layer (menus, text, the HUD, movies, logo screens) is drawn through the same 3D path as the scene, but flat:
    // every corner at one distance (one q, one depth). It is either in front of everything (depth 0xF000 and up; the scene uses
    // less) or, for full-screen pictures, textured at a fixed q far above any the scene uses (the scene's q stays under 0.003:
    // 1/150, 1/35, 0.01 and 0.05 are seen for 2D). Screen tints and clears are flat and far back but untextured, so they stay
    // as they are, across the whole width.
    const bool flat = q0 == q1 && z0 == z1;
    const bool layer2d = flat && (z0 >= 0xF000u || (textured && q0 >= 0.004f));
    int what;
    if (fbp != g_shown[0] && fbp != g_shown[1]) what = 0;
    else if (textured && (is_target(tbp) || tbp == g_shown[0] || tbp == g_shown[1])) what = 4;
    else if (!layer2d) what = 1;
    else if (!textured && x0 <= 1.0f && x1 >= width - 1.0f) what = 3;
    else what = 2;
    if (g_log) note(fbp, fbw, prim, attr, test, tbp, what, x0, y0, x1, y1, q0, q1, z0, z1);
    g_clip = false;
    if (what != 2 || k > 0.999f) return v;
    const float mid = width * 0.5f;
    g_clip = true;
    g_clip_lo = static_cast<int>(std::ceil(mid - mid * k));
    g_clip_hi = static_cast<int>(std::floor(mid + mid * k)) - 1;
    for (int i = 0; i < count; ++i) {
        out[i] = v[i];
        if (i >= n) continue;
        const float x = (static_cast<float>(v[i].x) - static_cast<float>(ofx)) / 16.0f;
        const float nx = mid + (x - mid) * k;
        out[i].x = static_cast<std::uint32_t>(std::clamp(std::lround(nx * 16.0f) + ofx, 0l, 65535l));
    }
    return out;
}
