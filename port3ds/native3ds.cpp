// Game functions the 3DS build runs natively: plain work that gains a lot from a few lines of ARM-friendly C.
// port3ds/natives.txt names them, and eerecomp --natives puts a call at the top of each. They live in namespace gen because the
// generated code declares them where it calls them. Each returns false for anything it does not cover, and the game's code runs.
// The video routines were compared with the game's own code on every call for about 2,000 frames of the opening movie.
#include "ps2/generated.hpp"

#include <cstring>

namespace gen {

using namespace ps2;
using u8 = std::uint8_t;
using u32 = std::uint32_t;

namespace {

constexpr u32 kEven = 0x00FF00FFu;

// A host pointer to n bytes of guest memory, or null if they are not all in main RAM or all in the scratchpad.
u8 *range(u32 a, u32 n) {
    if ((a >> 29) == 1) a &= 0x0FFFFFFFu;   // the uncached views of main RAM
    if (a < kRamSize) return n <= kRamSize - a ? g_ram + a : nullptr;
    if ((a >> 14) == (kScratchpadBase >> 14)) {
        const u32 at = a & (kScratchpadSize - 1);
        return n <= kScratchpadSize - at ? g_spr + at : nullptr;
    }
    return nullptr;
}

inline u32 word(const u8 *p) {
    u32 v;
    std::memcpy(&v, p, 4);
    return v;
}

// One plane of a motion compensation routine. The reference frames are stored by macroblock, so a row comes from two blocks side
// by side (a and b) and starts sa bytes into the first. After rows1 rows the source moves on by gap, for rows2 more. Output is one
// 16-bit sample per pixel, as the IPU's macroblocks are, so that the prediction and the residual can be added up.
template <int Groups, bool HalfX, bool HalfY, bool Average>
void plane(const u8 *a, const u8 *b, u8 *to, u32 sa, int rows1, int rows2, u32 stride, u32 gap) {
    alignas(16) u8 line[32];
    u32 last_even[Groups], last_odd[Groups], even[Groups], odd[Groups];
    u32 rounding = 0x00020002u;
    const auto fetch = [&] {
        if (Groups == 4) {
            std::memcpy(line, a, 16);
            std::memcpy(line + 16, b, 16);
        } else {
            std::memcpy(line, a, 8);
            std::memcpy(line + 8, b, 8);
            std::memcpy(line + 16, line, 16);   // the game rotates these 16 bytes
        }
        const u8 *p = line + sa;
        for (int g = 0; g < Groups; ++g, p += 4) {
            const u32 w = word(p);
            u32 x = w & kEven, y = (w >> 8) & kEven;
            if (HalfX) {
                const u32 n = word(p + 1);
                x += n & kEven;
                y += (n >> 8) & kEven;
            }
            even[g] = x;
            odd[g] = y;
        }
    };
    const auto emit = [&] {
        auto *d = reinterpret_cast<u32 *>(to);
        for (int g = 0; g < Groups; ++g, d += 2) {
            u32 x = even[g], y = odd[g];
            if (HalfX && HalfY) {
                const u32 sx = x, sy = y;
                x = ((last_even[g] + x + rounding) >> 2) & kEven;
                y = ((last_odd[g] + y + rounding) >> 2) & kEven;
                last_even[g] = sx;
                last_odd[g] = sy;
            } else if (HalfX || HalfY) {
                const u32 sx = x, sy = y;
                x = ((x + (HalfY ? last_even[g] : 0u) + 0x00010001u) >> 1) & kEven;
                y = ((y + (HalfY ? last_odd[g] : 0u) + 0x00010001u) >> 1) & kEven;
                if (HalfY) {
                    last_even[g] = sx;
                    last_odd[g] = sy;
                }
            }
            u32 n0 = (x & 0xFFFFu) | (y << 16), n1 = (x >> 16) | (y & 0xFFFF0000u);
            if (Average) {
                n0 = ((d[0] + n0 + 0x00010001u) >> 1) & kEven;
                n1 = ((d[1] + n1 + 0x00010001u) >> 1) & kEven;
            }
            d[0] = n0;
            d[1] = n1;
        }
        to += stride * 2;
        if (Groups == 4 && Average) rounding = 0x00010001u;   // the game's routine loses its rounding constant after one row
    };
    if (HalfY) {
        fetch();
        for (int g = 0; g < Groups; ++g) {
            last_even[g] = even[g];
            last_odd[g] = odd[g];
        }
    }
    for (int pass = 0; pass < 2; ++pass) {
        const int rows = pass ? rows2 : rows1;
        if (pass) {
            a += gap;
            b += gap;
        }
        for (int i = 0; i < rows; ++i) {
            if (HalfY) {
                a += stride;
                b += stride;
                fetch();
            } else {
                fetch();
                a += stride;
                b += stride;
            }
            emit();
        }
    }
}

// a0 points at: destination, x offset, rows, rows after the block boundary, line step, the two source blocks.
template <bool Chroma, bool HalfX, bool HalfY, bool Average>
bool motion(Cpu &c) {
    const u8 *args = range(c.r[4].w[0], 28);
    if (!args) return false;
    const u32 dst = word(args) & ~15u, sa = word(args + 4) & 15u, stride = word(args + 16);
    const int rows1 = static_cast<int>(word(args + 8)), rows2 = static_cast<int>(word(args + 12));
    u32 src0 = word(args + 20), src1 = word(args + 24);
    if (!Chroma) {
        src0 &= ~15u;
        src1 &= ~15u;
    }
    if (rows1 < 0 || rows2 < 0 || rows1 + rows2 > 32 || stride > 64 || (!HalfY && !rows1)) return false;
    const u32 rows = static_cast<u32>(rows1 + rows2), gap = Chroma ? 320u : 128u;
    const u32 used = (rows + 1) * stride + gap + (Chroma ? 72u : 16u);
    const u8 *a = range(src0, used), *b = range(src1, used);
    u8 *to = range(dst, rows * stride * 2 + (Chroma ? 144u : 32u));
    if (!a || !b || !to) return false;
    if (Chroma) {
        plane<2, HalfX, HalfY, Average>(a, b, to, sa, rows1, rows2, stride, gap);
        plane<2, HalfX, HalfY, Average>(a + 64, b + 64, to + 128, sa, rows1, rows2, stride, gap);
    } else {
        plane<4, HalfX, HalfY, Average>(a, b, to, sa, rows1, rows2, stride, gap);
    }
    c.sa = sa;
    c.budget -= 40 + static_cast<std::int32_t>(rows) * 8;
    return true;
}

inline u32 add_lanes(u32 a, u32 b) {
#ifdef __arm__
    u32 r;
    __asm__("uadd16 %0, %1, %2" : "=r"(r) : "r"(a), "r"(b) : "cc");
    return r;
#else
    return ((a + b) & 0xFFFFu) | (((a >> 16) + (b >> 16)) << 16);
#endif
}

inline u32 clamp_lanes(u32 v) {   // signed 16-bit lanes to 0..255
#ifdef __arm__
    u32 r;
    __asm__("usat16 %0, #8, %1" : "=r"(r) : "r"(v) : "cc");
    return r;
#else
    const auto one = [](std::int16_t x) { return static_cast<u32>(x < 0 ? 0 : x > 255 ? 255 : x); };
    return one(static_cast<std::int16_t>(v)) | (one(static_cast<std::int16_t>(v >> 16)) << 16);
#endif
}

inline u32 pack_lanes(u32 a, u32 b) { return ((a | (a >> 8)) & 0xFFFFu) | ((b | (b >> 8)) << 16); }

}

#define MOTION(name, chroma, x, y, average) \
    bool native_##name(Cpu &c) { return motion<chroma, x, y, average>(c); }
MOTION(put_luma, false, false, false, false)
MOTION(put_chroma, true, false, false, false)
MOTION(put_luma_y, false, false, true, false)
MOTION(put_chroma_y, true, false, true, false)
MOTION(put_luma_x, false, true, false, false)
MOTION(put_chroma_x, true, true, false, false)
MOTION(put_luma_xy, false, true, true, false)
MOTION(put_chroma_xy, true, true, true, false)
MOTION(avg_luma, false, false, false, true)
MOTION(avg_chroma, true, false, false, true)
MOTION(avg_luma_y, false, false, true, true)
MOTION(avg_chroma_y, true, false, true, true)
MOTION(avg_luma_x, false, true, false, true)
MOTION(avg_chroma_x, true, true, false, true)
MOTION(avg_luma_xy, false, true, true, true)
MOTION(avg_chroma_xy, true, true, true, true)
#undef MOTION

// Prediction (a2) + residual (a1), both 16-bit samples for a whole macroblock, to 8-bit samples at a0.
bool native_add_block(Cpu &c) {
    u8 *d = range(c.r[4].w[0] & ~15u, 384);
    const u8 *r = range(c.r[5].w[0] & ~15u, 768), *p = range(c.r[6].w[0] & ~15u, 768);
    if (!d || !r || !p) return false;
    const auto *x = reinterpret_cast<const u32 *>(r), *y = reinterpret_cast<const u32 *>(p);
    auto *out = reinterpret_cast<u32 *>(d);
    for (int i = 0; i < 96; ++i, x += 2, y += 2)
        out[i] = pack_lanes(clamp_lanes(add_lanes(y[0], x[0])), clamp_lanes(add_lanes(y[1], x[1])));
    c.budget -= 250;
    return true;
}

// The same with no prediction: a1 to a0.
bool native_put_block(Cpu &c) {
    u8 *d = range(c.r[4].w[0] & ~15u, 384);
    const u8 *r = range(c.r[5].w[0] & ~15u, 768);
    if (!d || !r) return false;
    const auto *x = reinterpret_cast<const u32 *>(r);
    auto *out = reinterpret_cast<u32 *>(d);
    for (int i = 0; i < 96; ++i, x += 2) out[i] = pack_lanes(clamp_lanes(x[0]), clamp_lanes(x[1]));
    c.budget -= 200;
    return true;
}

bool native_memcpy(Cpu &c) {
    const u32 to = c.r[4].w[0], from = c.r[5].w[0], n = c.r[6].w[0];
    if (!n) return false;
    u8 *d = range(to, n);
    const u8 *s = range(from, n);
    if (!d || !s || (d < s + n && s < d + n)) return false;
    std::memcpy(d, s, n);
    c.r[2].sd[0] = static_cast<std::int32_t>(to);
    c.budget -= 16 + static_cast<std::int32_t>(n >> 3);
    return true;
}

// Walks a video stream for a start code of the kinds in the mask a2, in a1 bytes from a0. Returns where it starts, or 0.
bool native_find_code(Cpu &c) {
    const u32 from = c.r[4].w[0], n = c.r[5].w[0], mask = c.r[6].w[0];
    const u8 *p = range(from, n);
    if (!p || !n) return false;
    u32 i = 0;
    int state = 0;
    u32 result = 0;
    while (i < n) {
        if (!state) {   // skip whole words with no zero byte
            while (n - i >= 8) {
                const u32 w = word(p + i);
                if ((w - 0x01010101u) & ~w & 0x80808080u) break;
                i += 4;
            }
        }
        const u8 v = p[i++];
        switch (state) {
        case 0: state = !v; break;
        case 1: state = v ? 0 : 2; break;
        case 2: state = v == 1 ? 3 : v ? 0 : 2; break;
        default: {
            const u32 code = 0x100u | v;
            u32 kind = 0;
            if (code == 0x100u) kind = 4;
            else if (code == 0x101u) kind = 3;
            else if (code - 0x102u < 174u) kind = 1;
            else if (code == 0x1B2u) kind = 32;
            else if (code == 0x1B3u) kind = 64;
            else if (code == 0x1B5u) kind = 16;
            else if (code == 0x1B7u) kind = 128;
            else if (code == 0x1B8u) kind = 8;
            if (kind & mask) {
                result = from + i - 4;
                i = n;
            }
            state = 0;
            break;
        }
        }
    }
    c.r[2].sd[0] = static_cast<std::int32_t>(result);
    c.budget -= 30 + static_cast<std::int32_t>(n >> 2);
    return true;
}

}
