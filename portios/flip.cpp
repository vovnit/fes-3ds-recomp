// Faster drawing in the iOS build: the GPU renderer's "flip_fix" (a texture drawn 1:1 whose coordinate runs backwards is moved
// one texel) is worked out here, once per triangle, instead of for every pixel of it in the shader.
//
// The shader (gs.wgsl, raster pass) used to take each pixel inside a triangle, work out the texture's slope across the
// triangle from its three corners, and when the slope is -1 along an axis, move that pixel's texture coordinate back by
// one texel. The slope is the same for every pixel of the triangle, and the move (16 for fixed-point coordinates, q/size
// otherwise) is linear in what is interpolated, so moving each corner's coordinate by its own amount moves every pixel by
// exactly what the shader moved it. CMakeLists.txt takes the per-pixel block out of the iOS copy of the shader and calls
// portios_flip() where draw() queues a triangle.
//
// Measured with the native build (M1 Pro, 2x, GPU timestamps, 100 frames each): the raster pass went from 13.1 to 12.1 ms
// a frame in the school lobby, 21.3 to 17.6 ms in the locker room, 15.6 to 12.7 ms in a battle. Frames compared with the old
// shader: identical in the battle, elsewhere at most 62 pixels of 1.1 million off by 1/255 (float rounding).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

float as_float(std::uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

std::uint32_t as_bits(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

}  // namespace

// rec: a triangle's record as draw() builds it (corners at words 4-9, 1/area at 10, s/t/q of corner i at 15+3i to 17+3i);
// setup: its setup words (0 flags, 15 texture size as log2: width in the low byte, height in the next)
void portios_flip(std::uint32_t *rec, const std::uint32_t *setup) {
    const std::uint32_t flags = setup[0];
    if (!(flags & 2u)) return;   // not textured
    const bool fst = flags & 16u;
    const float tw = static_cast<float>(1u << (setup[15] & 0xFFu)), th = static_cast<float>(1u << (setup[15] >> 8));
    const float x0 = as_float(rec[4]), y0 = as_float(rec[5]), x1 = as_float(rec[6]), y1 = as_float(rec[7]), x2 = as_float(rec[8]), y2 = as_float(rec[9]);
    const float inv = as_float(rec[10]);
    float tu[3], tv[3];
    for (int i = 0; i < 3; ++i) {
        const float s = as_float(rec[15 + i * 3]), t = as_float(rec[16 + i * 3]);
        if (fst) {
            tu[i] = s / 16.0f;
            tv[i] = t / 16.0f;
        } else {
            float q = as_float(rec[17 + i * 3]);
            if (q == 0.0f) q = 1.0f;
            tu[i] = s / q * tw;
            tv[i] = t / q * th;
        }
    }
    const float ux = ((tu[1] - tu[0]) * (y2 - y0) - (tu[2] - tu[0]) * (y1 - y0)) * inv;
    const float uy = ((tu[2] - tu[0]) * (x1 - x0) - (tu[1] - tu[0]) * (x2 - x0)) * inv;
    const float vx = ((tv[1] - tv[0]) * (y2 - y0) - (tv[2] - tv[0]) * (y1 - y0)) * inv;
    const float vy = ((tv[2] - tv[0]) * (x1 - x0) - (tv[1] - tv[0]) * (x2 - x0)) * inv;
    const float gu = std::fabs(ux) >= std::fabs(uy) ? ux : uy;
    const float gv = std::fabs(vx) >= std::fabs(vy) ? vx : vy;
    const bool flip_u = gu < 0.0f && std::fabs(std::fabs(gu) - 1.0f) < 0.05f;
    const bool flip_v = gv < 0.0f && std::fabs(std::fabs(gv) - 1.0f) < 0.05f;
    if (!flip_u && !flip_v) return;
    for (int i = 0; i < 3; ++i) {
        const float q = as_float(rec[17 + i * 3]);   // the shader moved by the interpolated q (not the q == 0 fallback)
        if (flip_u) rec[15 + i * 3] = as_bits(as_float(rec[15 + i * 3]) - (fst ? 16.0f : q / tw));
        if (flip_v) rec[16 + i * 3] = as_bits(as_float(rec[16 + i * 3]) - (fst ? 16.0f : q / th));
    }
}
