// Game functions the 3DS build runs natively: plain work that gains a lot from a few lines of ARM-friendly C.
// port3ds/natives.txt names them, and eerecomp --natives puts a call at the top of each. They live in namespace gen because the
// generated code declares them where it calls them. Each returns false for anything it does not cover, and the game's code runs.
// The video routines were compared with the game's own code on every call for about 2,000 frames of the opening movie.
#include "ps2/generated.hpp"

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

}
