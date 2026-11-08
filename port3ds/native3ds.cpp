// Game functions the 3DS build runs natively: plain work that gains a lot from a few lines of ARM-friendly C.
// port3ds/natives.txt names them, and eerecomp --natives puts a call at the top of each. They live in namespace gen because the
// generated code declares them where it calls them. Each returns false for anything it does not cover, and the game's code runs.
// The video routines were compared with the game's own code on every call for about 2,000 frames of the opening movie.
#include "ps2/generated.hpp"
#ifdef PS2_CHECK_NATIVES
#include "ps2/system.hpp"
#include <cstdio>
#endif

#include <bit>
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
    u32 last_even[Groups], last_odd[Groups];
    u32 rounding = 0x00020002u;
    const auto copy_line = [&] {
        constexpr int width = Groups * 4;
        std::memcpy(line, a, width);
        std::memcpy(line + width, b, width);
        if constexpr (Groups == 2)
            std::memcpy(line + 16, line, 8);   // repeat the first half when the chroma window wraps
    };
    const auto samples = [](const u8 *p, u32 &x, u32 &y) {
        const u32 w = word(p);
        x = w & kEven;
        y = (w >> 8) & kEven;
        if constexpr (HalfX) {
            const u32 n = word(p + 1);
#ifdef __arm__
            __asm__("uxtab16 %0, %0, %2\n\tuxtab16 %1, %1, %2, ror #8"
                    : "+&r"(x), "+&r"(y) : "r"(n));
#else
            x += n & kEven;
            y += (n >> 8) & kEven;
#endif
        }
    };
    if constexpr (HalfY) {
        copy_line();
        for (int g = 0; g < Groups; ++g)
            samples(line + sa + g * 4, last_even[g], last_odd[g]);
        a += stride;
        b += stride;
    }
    for (int pass = 0; pass < 2; ++pass) {
        const int rows = pass ? rows2 : rows1;
        if (pass) {
            a += gap;
            b += gap;
        }
        for (int i = 0; i < rows; ++i) {
            copy_line();
            a += stride;
            b += stride;
            const u8 *p = line + sa;
            auto *d = reinterpret_cast<u32 *>(to);
            for (int g = 0; g < Groups; ++g, p += 4, d += 2) {
                u32 x, y;
                samples(p, x, y);
                if constexpr (HalfY) {
                    const u32 prev_x = last_even[g], prev_y = last_odd[g];
                    last_even[g] = x;
                    last_odd[g] = y;
                    if constexpr (HalfX) {
                        x = ((prev_x + x + rounding) >> 2) & kEven;
                        y = ((prev_y + y + rounding) >> 2) & kEven;
                    } else {
                        x = ((prev_x + x + 0x00010001u) >> 1) & kEven;
                        y = ((prev_y + y + 0x00010001u) >> 1) & kEven;
                    }
                } else if constexpr (HalfX) {
                    x = ((x + 0x00010001u) >> 1) & kEven;
                    y = ((y + 0x00010001u) >> 1) & kEven;
                }
                u32 n0 = (x & 0xFFFFu) | (y << 16), n1 = (x >> 16) | (y & 0xFFFF0000u);
                if constexpr (Average) {
                    n0 = ((d[0] + n0 + 0x00010001u) >> 1) & kEven;
                    n1 = ((d[1] + n1 + 0x00010001u) >> 1) & kEven;
                }
                d[0] = n0;
                d[1] = n1;
            }
            to += stride * 2;
            if constexpr (Groups == 4 && Average) rounding = 0x00010001u;
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

namespace {

using Float [[gnu::may_alias]] = float;

constexpr float from_bits(u32 x) { return std::bit_cast<float>(x); }

// A 16-bit float from an animation key: sign, 4 bits of exponent starting at 2^-15, 11 bits of mantissa.
inline u32 half_float(const u8 *p, std::int32_t &spent) {
    std::uint16_t u;
    std::memcpy(&u, p, 2);
    const u32 sign = static_cast<u32>(u & 0x8000u) << 16;
    if (!(u & 0x7FFFu)) {
        spent += 6;
        return sign;
    }
    spent += 15;
    return sign | (((u & 0x7800u) << 12) + 0x38000000u) | ((u & 0x7FFu) << 12);
}

// a / b as the FPU does it. A denormal b counts as zero.
inline float divide(Cpu &c, float a, float b) {
    if (fp::exponent(std::bit_cast<u32>(b))) [[likely]] return a / b;
    const fp::Result r = fp::div(std::bit_cast<u32>(a), std::bit_cast<u32>(b));
    if (r.invalid) c.fcr31 |= 0x30000u;
    return std::bit_cast<float>(r.value);
}

// The sine series the game uses, for an angle whose square is known.
inline float sine(float a, float a2) {
    float s = from_bits(0xB2D72F34u) + from_bits(0x2F2EC9D3u) * a2;
    s = from_bits(0x3638EF1Bu) + a2 * s;
    s = from_bits(0xB9500D01u) + a2 * s;
    s = from_bits(0x3C088889u) + a2 * s;
    s = from_bits(0xBE2AAAABu) + a2 * s;
    return a + (a2 * a) * s;
}

// The two halves of the rational approximation in the game's arc cosine.
inline float acos_top(float z) {
    float p = from_bits(0x3A4F7F04u) + from_bits(0x3811EF08u) * z;
    p = from_bits(0xBD241146u) + z * p;
    p = from_bits(0x3E4E0AA8u) + z * p;
    p = from_bits(0xBEA6B090u) + z * p;
    return z * (from_bits(0x3E2AAAABu) + z * p);
}

inline float acos_bottom(float z) {
    float q = from_bits(0xBF303361u) + from_bits(0x3D9DC62Eu) * z;
    q = from_bits(0x4001572Du) + z * q;
    q = from_bits(0xC019D139u) + z * q;
    return 1.0f + z * q;
}

}

// Blends two animation keys, once for each bone of each character in every game frame: rotations by spherical interpolation, with
// the arc cosine and three sines inlined by the game's compiler, positions in a straight line. It was the most expensive function
// in the lobby, about a tenth of the game's code. Same operations in the same order, except that a zero the game adds to a constant
// before a multiply and accumulate is left out. The registers the game's version changes are all caller-saved, so these stay as they are.
bool native_pose_blend(Cpu &c) {
    const u32 to_at = c.r[4].w[0], from_at = c.r[5].w[0], next_at = c.r[6].w[0], base_at = c.r[7].w[0];
    if ((to_at | from_at | next_at | base_at) & 3u) return false;
    if (to_at + 36u > base_at && base_at + 24u > to_at) return false;
    u8 *to = range(to_at, 36);
    const u8 *from = range(from_at, 22), *next = range(next_at, 22), *base = range(base_at, 24);
    if (!to || !from || !next || !base) return false;

    std::int32_t spent = 4 + 4;   // the stack frame and four register loads along the way
    u32 q0[4], q1[4], p0[3], p1[3];
    for (int i = 0; i < 4; ++i) q0[i] = half_float(from + 8 + 2 * i, spent);
    for (int i = 0; i < 3; ++i) p0[i] = half_float(from + 16 + 2 * i, spent);
    for (int i = 0; i < 4; ++i) q1[i] = half_float(next + 8 + 2 * i, spent);
    for (int i = 0; i < 3; ++i) p1[i] = half_float(next + 16 + 2 * i, spent);

    const auto f = [](u32 x) { return std::bit_cast<float>(x); };
    const float t0 = *reinterpret_cast<const Float *>(from + 4);
    float t = divide(c, f(c.f[12]) - t0, *reinterpret_cast<const Float *>(next + 4) - t0);
    float dot = f(q0[1]) * f(q1[1]) + f(q0[0]) * f(q1[0]);
    dot += f(q0[2]) * f(q1[2]);
    dot += f(q0[3]) * f(q1[3]);

    const auto *b = reinterpret_cast<const Float *>(base);
    const float x = (0.0f + f(p0[0]) + t * (f(p1[0]) - f(p0[0]))) * b[3] + b[0];
    const float y = (0.0f + f(p0[1]) + t * (f(p1[1]) - f(p0[1]))) * b[4] + b[1];
    const float z = (0.0f + f(p0[2]) + t * (f(p1[2]) - f(p0[2]))) * b[5] + b[2];
    spent += 62 + 9;

    bool close = false;
    {
        u32 bits = std::bit_cast<u32>(dot);
        if (fp::key(bits) < 0) {
            for (u32 &q : q1) q ^= fp::kSign;
            dot = -dot;
            bits ^= fp::kSign;
            spent += 5;
        }
        close = !(fp::key(bits) < static_cast<std::int32_t>(0x3F7FBE77u));
    }

    float w0 = 1.0f - t, w1 = t;
    if (!close) {
        const u32 bits = std::bit_cast<u32>(dot), magnitude = bits & fp::kAbs;
        if (static_cast<std::int32_t>(bits) < 0 || static_cast<std::int32_t>(magnitude) >= 0x3F800000) return false;   // not after the sign flip above
        float angle;
        spent += 8 + 4;
        if (static_cast<std::int32_t>(magnitude) >= 0x3F000000) {
            const float a = 0.5f * (1.0f - dot);
            const float s = __builtin_sqrtf(a);
            const float r = divide(c, acos_top(a), acos_bottom(a));
            const float high = from_bits(std::bit_cast<u32>(s) & 0xFFFFF000u);
            const float low = divide(c, a - high * high, s + high);
            angle = 2.0f * (high + (low + r * s));
            spent += 2 + 75 + 1;
        } else if (static_cast<std::int32_t>(magnitude) < 0x23000001) {
            angle = from_bits(0x3FC90FDBu);
            spent += 5 + 5;
        } else {
            const float a = dot * dot;
            const float r = divide(c, acos_top(a), acos_bottom(a));
            angle = from_bits(0x3FC90FDAu) - (dot - (from_bits(0x33A22168u) - dot * r));
            spent += 5 + 63 + 1;
        }
        const float over_sine = divide(c, 1.0f, sine(angle, angle * angle));
        const float a0 = w0 * angle, a1 = t * angle;
        w0 = sine(a0, a0 * a0) * over_sine;
        w1 = sine(a1, a1 * a1) * over_sine;
        spent += 68;
    }

    auto *out = reinterpret_cast<Float *>(to);
    out[2] = w1 * f(q1[0]) + w0 * f(q0[0]);
    out[3] = w1 * f(q1[1]) + w0 * f(q0[1]);
    out[4] = w1 * f(q1[2]) + w0 * f(q0[2]);
    out[5] = w1 * f(q1[3]) + w0 * f(q0[3]);
    out[6] = x;
    out[7] = y;
    out[8] = z;
    c.fcr31 = close ? (c.fcr31 & ~kFpuCondition) : (c.fcr31 | kFpuCondition);
    c.budget -= spent + 17 + 5;
    return true;
}

namespace {

// One row of the game's matrix products: basis 0, 1 and 2 scaled by a row's x, y and z, and basis 3 added to the last row. The
// lanes are kept apart so that the FPU, which takes 8 cycles for a result but starts one each cycle, works on all four at once.
template <int Lanes, bool Last>
[[gnu::always_inline]] inline void product_row(Float *to, const Float *row, const Float *basis) {
    const float x = row[0], y = row[1], z = row[2];
    float a = basis[0] * x, b = basis[1] * x, d = basis[2] * x, e = Lanes == 4 ? basis[3] * x : 0.0f;
    a += basis[4] * y;
    b += basis[5] * y;
    d += basis[6] * y;
    if (Lanes == 4) e += basis[7] * y;
    a += basis[8] * z;
    b += basis[9] * z;
    d += basis[10] * z;
    if (Lanes == 4) e += basis[11] * z;
    if (Last) {
        a += basis[12];
        b += basis[13];
        d += basis[14];
        if (Lanes == 4) e += basis[15];
    }
    to[0] = a;
    to[1] = b;
    to[2] = d;
    if (Lanes == 4) to[3] = e;
}

}

// The matrix product the game calls most. x, y and z of each row only: the w values it writes out are whatever vf9 to vf12 held.
bool native_matrix_product(Cpu &c) {
    const u32 to_at = c.r[4].w[0], rows_at = c.r[5].w[0], basis_at = c.r[6].w[0];
    if ((to_at | rows_at | basis_at) & 15u) return false;
    u8 *to = range(to_at, 64);
    const u8 *rows = range(rows_at, 64), *basis = range(basis_at, 64);
    if (!to || !rows || !basis || c.vu0.vf[0].w[3] != fp::kOne) return false;

    std::memcpy(&c.vu0.vf[1], rows, 64);
    std::memcpy(&c.vu0.vf[5], basis, 64);
    const auto *a = reinterpret_cast<const Float *>(&c.vu0.vf[1]);
    const auto *b = reinterpret_cast<const Float *>(&c.vu0.vf[5]);
    auto *r = reinterpret_cast<Float *>(&c.vu0.vf[9]);
    product_row<3, false>(r, a, b);
    product_row<3, false>(r + 4, a + 4, b);
    product_row<3, false>(r + 8, a + 8, b);
    product_row<3, false>(reinterpret_cast<Float *>(&c.vu0.acc), a + 12, b);
    auto *acc = reinterpret_cast<const Float *>(&c.vu0.acc);
    r[12] = acc[0] + b[12];
    r[13] = acc[1] + b[13];
    r[14] = acc[2] + b[14];
    const u32 flags_a = c.vu0.vf[1].w[3], flags_b = c.vu0.vf[5].w[3];
    std::memcpy(to, &c.vu0.vf[9], 64);
    const u32 flags = flags_a & flags_b;
    std::memcpy(to + 12, &flags, 4);
    c.r[3].d[0] = flags_a;
    c.r[2].d[0] = c.r[4].d[0];
    c.budget -= 31;
    return true;
}

// The rotation matrix of a quaternion, with the position after it. It belongs to the same animation step as native_pose_blend.
bool native_pose_matrix(Cpu &c) {
    const u32 to_at = c.r[4].w[0], from_at = c.r[5].w[0];
    if ((to_at | from_at) & 3u) return false;
    u8 *to_bytes = range(to_at, 64);
    const u8 *from_bytes = range(from_at, 36);
    if (!to_bytes || !from_bytes) return false;
    auto *to = reinterpret_cast<Float *>(to_bytes);
    const auto *from = reinterpret_cast<const Float *>(from_bytes);
    const float x = from[2], y = from[3], z = from[4], w = from[5];
    const float xx = x * x, yy = y * y, zz = z * z, yz = y * z, zx = z * x, xy = x * y, wz = w * z, wx = w * x, wy = w * y;
    to[0] = 1.0f - 2.0f * (yy + zz);
    to[1] = 2.0f * (xy + wz);
    to[2] = 2.0f * (zx - wy);
    to[4] = 2.0f * (xy - wz);
    to[5] = 1.0f - 2.0f * (xx + zz);
    to[6] = 2.0f * (yz + wx);
    to[8] = 2.0f * (zx + wy);
    to[9] = 2.0f * (yz - wx);
    to[10] = 1.0f - 2.0f * (xx + yy);
    to[12] = 0.0f;
    to[13] = 0.0f;
    to[14] = 0.0f;
    const u32 three = 3;
    std::memcpy(to_bytes + 12, &three, 4);
    to[12] = from[6];   // read after the writes above, as the game does
    to[13] = from[7];
    to[14] = from[8];
    c.budget -= 60;
    return true;
}

// Functions named native_loop_ run at the top of a loop in the game and do all of its passes but the last. The game's own code then
// does that one, which leaves every register as the game would, and they need not fit the whole loop's exit.

// A bone's final matrix: its two local matrices multiplied together, then by the model's matrix already in vf13 to vf16.
void native_loop_bone_matrices(Cpu &c) {
    const u32 to_at = c.r[4].w[0], end = c.r[1].w[0], basis_at = c.r[5].w[0], rows_at = c.r[6].w[0], stride = c.r[2].w[0];
    const u32 passes = (end - to_at) >> 6;
    if (end <= to_at || ((end - to_at) & 63u) || passes < 2 || stride > 0x1000u) return;
    if (((to_at | basis_at | rows_at | stride) & 15u) || c.vu0.vf[0].w[3] != fp::kOne) return;
    const u32 count = passes - 1, reach = (count - 1) * stride + 64;
    u8 *to = range(to_at, count * 64);
    const u8 *rows = range(rows_at, reach), *basis = range(basis_at, reach);
    if (!to || !rows || !basis) return;
    alignas(16) float model[16], part[16];
    std::memcpy(model, &c.vu0.vf[13], 64);
    const auto *m = reinterpret_cast<const Float *>(model);
    for (u32 i = 0; i < count; ++i, to += 64, rows += stride, basis += stride) {
        const auto *b = reinterpret_cast<const Float *>(basis);
        const auto *a = reinterpret_cast<const Float *>(rows);
        auto *p = reinterpret_cast<Float *>(part);
        auto *t = reinterpret_cast<Float *>(to);
        product_row<4, false>(p, a, b);
        product_row<4, false>(p + 4, a + 4, b);
        product_row<4, false>(p + 8, a + 8, b);
        product_row<4, true>(p + 12, a + 12, b);
        product_row<4, false>(t, p, m);
        product_row<4, false>(t + 4, p + 4, m);
        product_row<4, false>(t + 8, p + 8, m);
        product_row<4, true>(t + 12, p + 12, m);
    }
    set32(c.r[4], to_at + count * 64);
    set32(c.r[5], basis_at + count * stride);
    set32(c.r[6], rows_at + count * stride);
    c.budget -= static_cast<std::int32_t>(count) * 43;
}

// The copy loop of a structure assignment: two words each pass, counted in r4, from r6 to r5.
void native_loop_copy_pairs(Cpu &c) {
    const auto count = c.r[4].w[0];
    if (static_cast<std::int32_t>(count) < 2 || count > 0x10000u) return;
    const u32 to_at = c.r[5].w[0], from_at = c.r[6].w[0], bytes = (count - 1) * 8;
    if (((to_at | from_at) & 3u) || to_at - from_at < bytes) return;   // copying forward over itself repeats words
    u8 *to = range(to_at, bytes);
    const u8 *from = range(from_at, bytes);
    if (!to || !from) return;
    std::memmove(to, from, bytes);
    set32(c.r[4], 1u);
    set32(c.r[5], to_at + bytes);
    set32(c.r[6], from_at + bytes);
    c.budget -= static_cast<std::int32_t>(count - 1) * 9;
}

// CRI ADX sound with its key stream, decoded on the game core whenever music or voices play: 1.5 to 6% of the game's code. Each
// 18-byte frame is a big-endian scale, mixed with the key, and 16 bytes of two 4-bit samples each, predicted from the last two.
// A scale with the top bit set ends the data and leaves the history as it was. The game's instruction counts are charged as they
// would have run; a clamped sample costs 3 more.
namespace {

using s16 = std::int16_t;
using s32 = std::int32_t;

inline s32 half16(const u8 *p) {
    s16 v;
    std::memcpy(&v, p, 2);
    return v;
}

inline void put16(u8 *p, s32 v) {
    p[0] = static_cast<u8>(v);
    p[1] = static_cast<u8>(v >> 8);
}

inline bool apart(u32 a, u32 n, u32 b, u32 m) { return a + n <= b || b + m <= a; }

struct Adx {
    s32 coef1, coef2, multiplier, increment, table[16];
    // The frame's scale from its first two bytes and the key, which moves on, or -1 at the end of the data.
    s32 scale(const u8 *frame, u32 &key) const {
        const s32 word = static_cast<s16>((frame[0] << 8) | frame[1]);
        if (word & 0x8000) return -1;
        const s32 scale = static_cast<s32>((static_cast<u32>(word) ^ key) & 0x1FFFu) + 1;
        key = (static_cast<u32>(static_cast<s32>(key) * multiplier) + static_cast<u32>(increment)) & 0x7FFFu;
        return scale;
    }
    s32 clamp(s32 v, s32 &spent) const {
        if (static_cast<u32>(v + 0x8000) <= 0xFFFFu) return v;
        spent += 3;
        return v > 0x7FFF ? 0x7FFF : -0x8000;
    }
    s32 predict(s32 h1, s32 h2) const { return static_cast<s32>(static_cast<u32>(coef1 * h1) + static_cast<u32>(coef2 * h2)) >> 12; }
    // 32 samples from the 16 bytes after the scale, h1 the last sample and h2 the one before.
    void frame(const u8 *in, s32 scale, s32 &h1, s32 &h2, u8 *out, s32 &spent) const {
        for (int i = 0; i < 16; ++i) {
            const u8 b = in[i];
            const s32 a = clamp(static_cast<s32>(static_cast<u32>(predict(h1, h2)) + static_cast<u32>((static_cast<std::int8_t>(b) >> 4) * scale)), spent);
            const s32 z = clamp(static_cast<s32>(static_cast<u32>(predict(a, h1)) + static_cast<u32>(table[b & 15] * scale)), spent);
            put16(out + i * 4, a);
            put16(out + i * 4 + 2, z);
            h2 = a;
            h1 = z;
        }
    }
};

// The two-sample history of a channel, last sample first.
struct History {
    s32 h1, h2;
    explicit History(const u8 *p) : h1(half16(p)), h2(half16(p + 2)) {}
    void put(u8 *p) const { put16(p, h1), put16(p + 2, h2); }
};

const u8 *adx_table() { return range(0x007C1A40u, 64); }

#ifdef PS2_CHECK_NATIVES
// What the game's code should leave, worked out by the native, which then lets the game's code run; the check at the function's
// last block compares. Each output is one channel's samples.
struct AdxCheck {
    bool armed;
    u32 out_at[2], hist_at[2], key_at, channels, bytes, result;
    u8 out[2][64 * 256], hist[2][4], key[2];
    std::int64_t spent, start, budget;
    u32 calls, bad, budget_off;
} g_adx_check[2];

void check_adx(Cpu &c, AdxCheck &e, const char *name) {
    if (!e.armed) return;
    e.armed = false;
    ++e.calls;
    bool same = c.r[2].w[0] == e.result && std::memcmp(range(e.key_at, 2), e.key, 2) == 0;
    for (u32 n = 0; n < e.channels; ++n)
        same = same && (!e.bytes || std::memcmp(range(e.out_at[n], e.bytes), e.out[n], e.bytes) == 0) &&
               std::memcmp(range(e.hist_at[n], 4), e.hist[n], 4) == 0;
    if (!same && ++e.bad <= 20)
        std::fprintf(stderr, "[check] %s call %lu differs: result %lu/%lu\n", name, static_cast<unsigned long>(e.calls),
                     static_cast<unsigned long>(c.r[2].w[0]), static_cast<unsigned long>(e.result));
    const std::int64_t game = static_cast<std::int64_t>(ps2::now()) - e.start + 11;   // the last block is still to run
    if (game != e.spent && ++e.budget_off <= 20)
        std::fprintf(stderr, "[check] %s call %lu (%lu bytes) charged %lld, the game %lld; budget %lld before, %ld after\n", name,
                     static_cast<unsigned long>(e.calls), static_cast<unsigned long>(e.bytes), static_cast<long long>(e.spent),
                     static_cast<long long>(game), static_cast<long long>(e.budget), static_cast<long>(c.budget));
    if (e.calls % 500 == 0)
        std::fprintf(stderr, "[check] %s %lu calls, %lu differ, %lu charged wrong\n", name, static_cast<unsigned long>(e.calls),
                     static_cast<unsigned long>(e.bad), static_cast<unsigned long>(e.budget_off));
}
#endif

}

// One channel. a0 frames, a1 how many, a2 16-bit samples out, a3 the history, t0 and t1 the prediction coefficients, t2 the key,
// t3 its multiplier, and the first stack argument its increment. Returns how many frames it decoded.
bool native_adx_frames(Cpu &c) {
    const u32 src_at = c.r[4].w[0], dst_at = c.r[6].w[0], hist_at = c.r[7].w[0], key_at = c.r[10].w[0];
    const auto count = static_cast<s32>(c.r[5].w[0]);
    if (count <= 0 || count > 256 || ((src_at | dst_at | hist_at | key_at) & 1u)) return false;
    const u32 frames = static_cast<u32>(count), in_bytes = frames * 18, out_bytes = frames * 64;
    const u8 *src = range(src_at, in_bytes), *stack = range(c.r[29].w[0], 2), *table = adx_table();
    u8 *dst = range(dst_at, out_bytes), *hist = range(hist_at, 4), *key = range(key_at, 2);
    if (!src || !dst || !hist || !key || !stack || !table) return false;
    if (!apart(dst_at, out_bytes, src_at, in_bytes) || !apart(dst_at, out_bytes, hist_at, 4) || !apart(dst_at, out_bytes, key_at, 2) ||
        !apart(hist_at, 4, key_at, 2))
        return false;
    Adx adx{static_cast<s16>(c.r[8].w[0]), static_cast<s16>(c.r[9].w[0]), static_cast<s16>(c.r[11].w[0]), half16(stack), {}};
    std::memcpy(adx.table, table, sizeof adx.table);
#ifdef PS2_CHECK_NATIVES
    AdxCheck &e = g_adx_check[0];
    u8 *out = e.out[0], *const key_out = e.key, *const hist_out = e.hist[0];
    std::memcpy(key_out, key, 2);
    std::memcpy(hist_out, hist, 4);
#else
    u8 *out = dst;
    u8 *const key_out = key, *const hist_out = hist;
#endif
    History h(hist);
    u32 k = static_cast<std::uint16_t>(half16(key));
    s32 spent = 25 + 3;
    u32 done = 0;
    for (; done < frames; ++done, src += 18, out += 64) {
        const s32 scale = adx.scale(src, k);
        if (scale < 0) break;
        put16(key_out, static_cast<s32>(k));
        adx.frame(src + 2, scale, h.h1, h.h2, out, spent);
        spent += 9 + 15 + 16 * 43 + 4;
    }
    if (done < frames) spent += 9 + 11;
    else h.put(hist_out), spent += 3 + 11;
    const u32 result = done < frames ? done : frames;
#ifdef PS2_CHECK_NATIVES
    e.armed = true;
    e.out_at[0] = dst_at, e.hist_at[0] = hist_at, e.key_at = key_at, e.channels = 1, e.bytes = done * 64, e.result = result;
    e.spent = spent, e.start = static_cast<std::int64_t>(ps2::now()), e.budget = c.budget;
    return false;
#else
    set32(c.r[2], result);
    c.budget -= spent;
    return true;
#endif
}

// Two channels, their frames in turn, each with its own history; the key moves on with every frame. Reached by a jump from the
// function at 0x0053AD00. a0 frames, a1 how many of both channels, a2 and t0 the left and right samples out, a3 and t1 their
// histories, t2 and t3 the prediction coefficients; on the stack the key's address, its multiplier and its increment. Returns a1,
// or twice the pairs decoded if the data ends.
bool native_adx_stereo(Cpu &c) {
    const u32 src_at = c.r[4].w[0], out_at[2] = {c.r[6].w[0], c.r[8].w[0]}, hist_at[2] = {c.r[7].w[0], c.r[9].w[0]};
    const auto total = static_cast<s32>(c.r[5].w[0]);
    const s32 pairs = total / 2;
    const u8 *stack = range(c.r[29].w[0], 18), *table = adx_table();
    if (pairs <= 0 || pairs > 256 || !stack || !table) return false;
    const u32 key_at = word(stack);
    if ((src_at | out_at[0] | out_at[1] | hist_at[0] | hist_at[1] | key_at) & 1u) return false;
    const u32 n = static_cast<u32>(pairs), in_bytes = n * 36, out_bytes = n * 64;
    const u8 *src = range(src_at, in_bytes);
    u8 *dst[2] = {range(out_at[0], out_bytes), range(out_at[1], out_bytes)}, *hist[2] = {range(hist_at[0], 4), range(hist_at[1], 4)};
    u8 *key = range(key_at, 2);
    if (!src || !dst[0] || !dst[1] || !hist[0] || !hist[1] || !key) return false;
    for (int i = 0; i < 2; ++i)
        if (!apart(out_at[i], out_bytes, src_at, in_bytes) || !apart(out_at[i], out_bytes, hist_at[0], 4) ||
            !apart(out_at[i], out_bytes, hist_at[1], 4) || !apart(out_at[i], out_bytes, key_at, 2) || !apart(hist_at[i], 4, key_at, 2))
            return false;
    if (!apart(out_at[0], out_bytes, out_at[1], out_bytes) || !apart(hist_at[0], 4, hist_at[1], 4)) return false;
    Adx adx{static_cast<s16>(c.r[10].w[0]), static_cast<s16>(c.r[11].w[0]), half16(stack + 8), half16(stack + 16), {}};
    std::memcpy(adx.table, table, sizeof adx.table);
#ifdef PS2_CHECK_NATIVES
    AdxCheck &e = g_adx_check[1];
    u8 *out[2] = {e.out[0], e.out[1]}, *const key_out = e.key, *const hist_out[2] = {e.hist[0], e.hist[1]};
    std::memcpy(key_out, key, 2);
    std::memcpy(hist_out[0], hist[0], 4);
    std::memcpy(hist_out[1], hist[1], 4);
#else
    u8 *out[2] = {dst[0], dst[1]};
    u8 *const key_out = key, *const hist_out[2] = {hist[0], hist[1]};
#endif
    History h[2] = {History(hist[0]), History(hist[1])};
    u32 k = static_cast<std::uint16_t>(half16(key));
    s32 spent = 34;
    u32 done = 0;
    bool ended = false;
    for (; done < n; ++done, src += 36, out[0] += 64, out[1] += 64) {
        const s32 left = adx.scale(src, k);
        if (left < 0) {
            spent += 13 + 11;
            ended = true;
            break;
        }
        put16(key_out, static_cast<s32>(k));
        const s32 right = adx.scale(src + 18, k);
        if (right < 0) {
            spent += 13 + 24 + 11;
            ended = true;
            break;
        }
        put16(key_out, static_cast<s32>(k));
        adx.frame(src + 2, left, h[0].h1, h[0].h2, out[0], spent);
        adx.frame(src + 20, right, h[1].h1, h[1].h2, out[1], spent);
        spent += 13 + 24 + 15 + 16 * 87 + 7;
    }
    if (!ended) h[0].put(hist_out[0]), h[1].put(hist_out[1]), spent += 18;
    const u32 result = ended ? done * 2 : static_cast<u32>(total);
#ifdef PS2_CHECK_NATIVES
    e.armed = true;
    e.out_at[0] = out_at[0], e.out_at[1] = out_at[1], e.hist_at[0] = hist_at[0], e.hist_at[1] = hist_at[1], e.key_at = key_at;
    e.channels = 2, e.bytes = done * 64, e.result = result, e.spent = spent, e.start = static_cast<std::int64_t>(ps2::now()), e.budget = c.budget;
    return false;
#else
    set32(c.r[2], result);
    c.budget -= spent;
    return true;
#endif
}

#ifdef PS2_CHECK_NATIVES
// Listed in natives.txt only for a check build, at the last block of each decoder.
void native_loop_adx_frames_check(Cpu &c) { check_adx(c, g_adx_check[0], "adx"); }
void native_loop_adx_stereo_check(Cpu &c) { check_adx(c, g_adx_check[1], "adx stereo"); }
#endif

}
