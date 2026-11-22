// Game functions the 3DS build runs natively: plain work that gains a lot from a few lines of ARM-friendly C.
// port3ds/natives.txt names them, and eerecomp --natives puts a call at the top of each. They live in namespace gen because the
// generated code declares them where it calls them. Each returns false for anything it does not cover, and the game's code runs.
// The video routines were compared with the game's own code on every call for about 2,000 frames of the opening movie.
#include "ps2/generated.hpp"
#include "pica.hpp"
#ifdef PS2_CHECK_NATIVES
#include "ps2/system.hpp"
#include <cstdio>
#endif

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace gen {

using namespace ps2;
using u8 = std::uint8_t;
using u32 = std::uint32_t;

// Lossy cuts, on unless args.txt says lossy 0. The game works out a picture during the one before it, so these go by whether the
// next picture will be skipped. They leave out work whose results only that picture would use: the bone animation below, and
// the engine's render pipelines (lossy_return, for port3ds/lossy.txt). A skipped picture is never drawn, and the next shown one does all of it again.
bool g_lossy = true;

namespace {

inline bool hidden() {
    if (!g_lossy || !ps2::gs::pica::g_after_hidden) return false;
    ps2::gs::pica::g_render_left_out = true;
    return true;
}

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

// The function names below come from the public symbol lists of two related projects (snaku/Persona3-FES-Decompilation and
// ppkkkkk/p3fes-recompiled), matched to the 8 recorded scene profiles: together these four were 3 to 5% of the game's
// instructions in every scene.

// Bytes from a to the end of the memory area it is in, as range() sees it.
static u32 room(u32 a) {
    if ((a >> 29) == 1) a &= 0x0FFFFFFFu;
    if (a < kRamSize) return kRamSize - a;
    if ((a >> 14) == (kScratchpadBase >> 14)) return kScratchpadSize - (a & (kScratchpadSize - 1));
    return 0;
}

// _sceSDC and _sceIDC, the SDK's data-cache write-back and invalidate: both walk the 64 lines of a cache way and read the
// tag of each. The runtime has no cache and its translation drops the cache instructions, so the walk only changes scratch
// registers. This leaves them as the walk's last turn does and charges the walk's instructions (about 1,700 per call).
bool native_cache_walk(Cpu &c) {
    const std::uint64_t mask = 0xFFFFFFFFFFFFF000ull;   // a3 after lui 0xFFFF / ori 0xF000
    const std::uint64_t tag = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(mfc0(c, 28u, 0u)))) & mask;
    const std::uint64_t lo = c.r[4].d[0], hi = c.r[5].d[0];
    // The 64 line addresses increase within one aligned page, even for a sign-extended tag.
    // Each line costs 25 instructions below lo, 29 above hi, and 35 inside the range.
    const std::uint64_t last = tag + 63u * 64u;
    const u32 below = lo <= tag ? 0u : lo > last ? 64u : (static_cast<u32>(lo - tag) + 63u) >> 6;
    const u32 through_hi = hi < tag ? 0u : hi >= last ? 64u : 1u + (static_cast<u32>(hi - tag) >> 6);
    const u32 above = 64u - std::max(below, through_hi);   // below takes priority for reversed ranges
    const std::int32_t charge = 6 + 64 * 35 - 10 * below - 6 * above;
    c.r[2].d[0] = 0;
    set32(c.r[3], hi < last);
    set32(c.r[6], 4096u);
    c.r[7].d[0] = mask;
    c.budget -= charge;
    return true;
}

// strcmp: 0 for equal strings, otherwise the difference of the first two bytes that differ, as unsigned bytes.
bool native_strcmp(Cpu &c) {
    const u32 a = c.r[4].w[0], b = c.r[5].w[0];
    const u32 n = std::min(room(a), room(b));
    if (!n) return false;
    const u8 *p = range(a, n), *q = range(b, n);
    u32 i = 0;
    while (i < n && p[i] == q[i] && p[i]) ++i;
    if (i == n) return false;   // ran to the end of memory: let the game's code do what it does there
    set32(c.r[2], static_cast<u32>(p[i]) - static_cast<u32>(q[i]));
    set32(c.r[3], p[i]);
    c.budget -= ((a | b) & 7u) ? 10 + static_cast<std::int32_t>(8 * i) : 25 + static_cast<std::int32_t>(i + (i >> 1));
    return true;
}

// memset: returns its first argument.
bool native_memset(Cpu &c) {
    const u32 to = c.r[4].w[0], n = c.r[6].w[0];
    if (n) {
        u8 *d = range(to, n);
        if (!d) return false;
        std::memset(d, static_cast<int>(c.r[5].w[0] & 0xFFu), n);
    }
    c.r[2].d[0] = c.r[4].d[0];
    c.budget -= 10 + static_cast<std::int32_t>(n >> 3);
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

namespace {

[[gnu::always_inline]] inline bool pose_blend_one(Cpu &c, u32 to_at, u32 from_at, u32 next_at, u32 base_at, const u8 *base,
                                                  float cur_time, std::int32_t &spent, bool &close_out) {
    if ((to_at | from_at | next_at) & 3u) return false;
    if (to_at + 36u > base_at && base_at + 24u > to_at) return false;
    u8 *to = range(to_at, 36);
    const u8 *from = range(from_at, 22), *next = range(next_at, 22);
    if (!to || !from || !next) return false;

    spent += 4 + 4;   // the stack frame and four register loads along the way
    u32 q0[4], q1[4], p0[3], p1[3];
    for (int i = 0; i < 4; ++i) q0[i] = half_float(from + 8 + 2 * i, spent);
    for (int i = 0; i < 3; ++i) p0[i] = half_float(from + 16 + 2 * i, spent);
    for (int i = 0; i < 4; ++i) q1[i] = half_float(next + 8 + 2 * i, spent);
    for (int i = 0; i < 3; ++i) p1[i] = half_float(next + 16 + 2 * i, spent);

    const auto f = [](u32 x) { return std::bit_cast<float>(x); };
    const float t0 = *reinterpret_cast<const Float *>(from + 4);
    float t = divide(c, cur_time - t0, *reinterpret_cast<const Float *>(next + 4) - t0);
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

    if (hidden()) {
        close_out = close;
        spent += 17 + 5 + (close ? 0 : 160);
        return true;
    }
    float w0 = 1.0f - t, w1 = t;
    if (!close) {
        const u32 bits = std::bit_cast<u32>(dot), magnitude = bits & fp::kAbs;
        if (static_cast<std::int32_t>(bits) < 0 || static_cast<std::int32_t>(magnitude) >= 0x3F800000) return false;
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
    close_out = close;
    spent += 17 + 5;
    return true;
}

}   // namespace

// Blends two animation keys, once for each bone of each character in every game frame: rotations by spherical interpolation, with
// the arc cosine and three sines inlined by the game's compiler, positions in a straight line. It was the most expensive function
// in the lobby, about a tenth of the game's code. Same operations in the same order, except that a zero the game adds to a constant
// before a multiply and accumulate is left out. The registers the game's version changes are all caller-saved, so these stay as they are.
bool native_pose_blend(Cpu &c) {
    const u32 to_at = c.r[4].w[0], from_at = c.r[5].w[0], next_at = c.r[6].w[0], base_at = c.r[7].w[0];
    if (base_at & 3u) return false;
    const u8 *base = range(base_at, 24);
    if (!base) return false;
    std::int32_t spent = 0;
    bool close = false;
    if (!pose_blend_one(c, to_at, from_at, next_at, base_at, base, std::bit_cast<float>(c.f[12]), spent, close)) return false;
    c.fcr31 = close ? (c.fcr31 & ~kFpuCondition) : (c.fcr31 | kFpuCondition);
    c.budget -= spent;
    return true;
}

// Inner interpolation loop of RtAnimInterpolatorAddAnimTime (0x004B7240, L_004B7450): blends num_nodes - 1 bones inline when cb == 0x004F7560.
void native_loop_anim_interp(Cpu &c) {
    const auto num_nodes = static_cast<std::int32_t>(c.r[17].w[0]);
    const auto idx = static_cast<std::int32_t>(c.r[19].w[0]);
    if (idx < 0 || num_nodes - idx < 2 || num_nodes > 512) return;
    const u32 interp_at = c.r[16].w[0];
    if (interp_at & 3u) return;
    const u8 *interp = range(interp_at, 76u);
    if (!interp) return;
    if (word(interp + 68u) != 0x004F7560u) return;
    const u32 kf_step = word(interp + 36u);
    if ((kf_step & 3u) || kf_step < 36u || kf_step > 256u) return;
    const u32 anim_at = word(interp);
    if (anim_at & 3u) return;
    const u8 *anim = range(anim_at, 24u);
    if (!anim) return;
    const u32 base_at = word(anim + 20u);
    if (base_at & 3u) return;
    const u8 *base = range(base_at, 24u);
    if (!base) return;
    float cur_time;
    std::memcpy(&cur_time, interp + 4u, 4u);
    u32 cur_at = c.r[18].w[0];
    const std::int32_t run_count = num_nodes - idx - 1;
    std::int32_t spent = 0;
    std::int32_t done = 0;
    bool last_close = false;
    for (; done < run_count; ++done, cur_at += kf_step) {
        if (cur_at & 3u) break;
        const u8 *slot = range(cur_at, 8u);
        if (!slot) break;
        std::int32_t iter_spent = 0;
        if (!pose_blend_one(c, cur_at, word(slot), word(slot + 4u), base_at, base, cur_time, iter_spent, last_close)) break;
        spent += iter_spent + 13;
    }
    if (done > 0) {
        set32(c.r[18], cur_at);
        set32(c.r[19], static_cast<u32>(idx + done));
        c.fcr31 = last_close ? (c.fcr31 & ~kFpuCondition) : (c.fcr31 | kFpuCondition);
        c.budget -= spent;
    }
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
    if (hidden()) {
        c.budget -= 60;
        return true;
    }
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

// Hot copies of the functions in port3ds/lossy.txt start with this (tools/p3fes_eehot.py --lossy). For a picture that will be
// skipped they return at once, with v0 = 0, or with v0 = a0 where the list says so; otherwise the copy runs as usual. The list:
// - 0x004F2E70, the engine's pipeline runner (RenderWare's RxPipelineExecute, it seems), returns a0 when it succeeds. It sets up a few
//   globals and calls the pipeline's first node, which runs the rest. Every model is drawn through it, and nothing but the drawing
//   reads what the nodes make.
// With it left out of three vblanks in four on the Mac, in the lobby, lockers and the first battle, every picture after such a gap
// matched a full run exactly. Two more passed that test but are off in the list: 0x004916D0 (the scene's draw lists) turns the
// dorm's floor white after a fade-in, and 0x004CF4C0 (called while objects are updated and drawn) changes a small patch of a face
// in the lockers on the 3DS. Leaving out 0x00311730 or 0x00466EF0 changed later pictures on the Mac.
bool lossy_return(Cpu &c, bool keep_a0) {
    if (!hidden()) return false;
    c.r[2].d[0] = keep_a0 ? c.r[4].d[0] : 0;
    c.budget -= 40;
    c.pc = c.r[31].w[0];
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
    for (u32 i = hidden() ? count : 0; i < count; ++i, to += 64, rows += stride, basis += stride) {
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

namespace {

[[gnu::always_inline]] inline void mul_ltm_matrix(u8 *to_b, const u8 *rows_b, const u8 *basis_b) {
    alignas(16) float tmp[16];
    const auto *a = reinterpret_cast<const Float *>(rows_b);
    const auto *b = reinterpret_cast<const Float *>(basis_b);
    const bool aliased = to_b == rows_b || to_b == basis_b;
    auto *r = aliased ? reinterpret_cast<Float *>(tmp) : reinterpret_cast<Float *>(to_b);
    product_row<3, false>(r, a, b);
    product_row<3, false>(r + 4, a + 4, b);
    product_row<3, false>(r + 8, a + 8, b);
    product_row<3, true>(r + 12, a + 12, b);
    const u32 flags = word(rows_b + 12) & word(basis_b + 12);
    if (aliased) {
        auto *d = reinterpret_cast<Float *>(to_b);
        d[0] = r[0]; d[1] = r[1]; d[2] = r[2];
        d[4] = r[4]; d[5] = r[5]; d[6] = r[6];
        d[8] = r[8]; d[9] = r[9]; d[10] = r[10];
        d[12] = r[12]; d[13] = r[13]; d[14] = r[14];
    }
    std::memcpy(to_b + 12, &flags, 4);
}

bool sync_hierarchy_impl(u32 root_child, u32 root_flags, std::int32_t &spent) {
    struct Item {
        u32 frame;
        u32 flags;
    };
    Item stk[128];
    int sp = 0;
    if (root_child) stk[sp++] = Item{root_child, root_flags};
    while (sp > 0) {
        u32 f = stk[--sp].frame;
        const u32 in_flags = stk[sp].flags;
        while (f) {
            if (f & 15u) return false;
            u8 *fp = range(f, 164u);
            if (!fp) return false;
            const u32 f_flags = in_flags | fp[3];
            spent += 11;
            if (f_flags & 4u) {
                const u32 parent = word(fp + 4);
                if (parent & 15u) return false;
                const u8 *pp = range(parent, 144u);
                if (!pp) return false;
                mul_ltm_matrix(fp + 80, fp + 16, pp + 80);
                fp[3] &= static_cast<u8>(~4u);
                spent += 41;
            }
            const u32 child = word(fp + 152);
            const u32 next = word(fp + 156);
            if (child) {
                if (next) {
                    if (sp >= 128) return false;
                    stk[sp++] = Item{next, in_flags};
                }
                if (sp >= 128) return false;
                stk[sp++] = Item{child, f_flags};
                break;
            }
            f = next;
        }
    }
    return true;
}

bool sync_hierarchy_root_impl(u8 *rp, std::int32_t &spent) {
    const u32 r_flags = rp[3];
    spent += 25;
    if (r_flags & 4u) {
        std::memcpy(rp + 80, rp + 16, 64);
        spent += 69;
    }
    const u32 child = word(rp + 152);
    if (child && !sync_hierarchy_impl(child, r_flags, spent)) return false;
    rp[3] = static_cast<u8>(r_flags & ~5u);
    return true;
}

}

// RenderWare _rwFrameSyncHierarchyLTM(frame, flags): updates dirty LTM matrices across a frame subtree directly in RAM.
bool native_sync_hierarchy_ltm(Cpu &c) {
    if (c.vu0.vf[0].w[3] != fp::kOne) return false;
    const u32 frame = c.r[4].w[0], flags = c.r[5].w[0];
    std::int32_t spent = 22;
    if (frame && !sync_hierarchy_impl(frame, flags, spent)) return false;
    c.budget -= spent;
    return true;
}

// RenderWare root LTM sync (0x004CF4C0): copies root modelling -> LTM when dirty, syncs children, clears root dirty flags.
bool native_sync_hierarchy_root(Cpu &c) {
    if (c.vu0.vf[0].w[3] != fp::kOne) return false;
    const u32 root = c.r[4].w[0];
    if (root & 15u) return false;
    u8 *rp = range(root, 164u);
    if (!rp) return false;
    std::int32_t spent = 0;
    if (!sync_hierarchy_root_impl(rp, spent)) return false;
    c.budget -= spent;
    return true;
}

// RenderWare RwFrameGetLTM (0x004CB2F0): syncs root hierarchy if bit 0 is set, returns frame + 80.
bool native_frame_get_ltm(Cpu &c) {
    const u32 frame = c.r[4].w[0];
    if (frame & 15u) return false;
    const u8 *fp = range(frame, 164u);
    if (!fp) return false;
    const u32 root = word(fp + 160);
    if (root & 15u) return false;
    u8 *rp = range(root, 164u);
    if (!rp) return false;
    std::int32_t spent = 15;
    if (rp[3] & 1u) {
        if (c.vu0.vf[0].w[3] != fp::kOne) return false;
        if (!sync_hierarchy_root_impl(rp, spent)) return false;
        spent += 4;
    }
    set32(c.r[2], frame + 80u);
    c.budget -= spent;
    return true;
}

// The other bone matrix loop in 0x0046B510 (at L_0046B5A4): fetches each bone frame's LTM, multiplies invBone * LTM * invModel.
void native_loop_skin_bones(Cpu &c) {
    const std::int32_t i0 = static_cast<std::int32_t>(c.r[18].w[0]);
    const std::int32_t n = static_cast<std::int32_t>(c.r[20].w[0]);
    if (i0 < 0 || n - i0 < 2 || n > 256 || c.vu0.vf[0].w[3] != fp::kOne) return;
    const u32 count = static_cast<u32>(n - 1 - i0);
    const u32 tbl_at = c.r[19].w[0], inv_at = c.r[16].w[0], dst_at = c.r[17].w[0], model_at = c.r[29].w[0] + 240u;
    if ((tbl_at | inv_at | dst_at | model_at) & 15u) return;
    const u8 *tbl = range(tbl_at + static_cast<u32>(i0) * 16u, count * 16u);
    const u8 *inv = range(inv_at + static_cast<u32>(i0) * 64u, count * 64u);
    u8 *dst = range(dst_at + static_cast<u32>(i0) * 64u, count * 64u);
    const u8 *mod_b = range(model_at, 64u);
    if (!tbl || !inv || !dst || !mod_b) return;
    alignas(16) float model[16], part[16];
    std::memcpy(model, mod_b, 64u);
    const auto *m = reinterpret_cast<const Float *>(model);
    const bool skip_math = hidden();
    std::int32_t spent = 0;
    u32 k = 0;
    for (; k < count; ++k) {
        const u32 frame = word(tbl + k * 16u + 12u);
        if (frame & 15u) break;
        const u8 *fp = range(frame, 164u);
        if (!fp) break;
        const u32 root = word(fp + 160u);
        if (root & 15u) break;
        u8 *rp = range(root, 164u);
        if (!rp) break;
        if (rp[3] & 1u) {
            if (!sync_hierarchy_root_impl(rp, spent)) break;
            spent += 4;
        }
        if (!skip_math) {
            const auto *a = reinterpret_cast<const Float *>(inv + k * 64u);
            const auto *b = reinterpret_cast<const Float *>(fp + 80u);
            auto *p = reinterpret_cast<Float *>(part);
            auto *t = reinterpret_cast<Float *>(dst + k * 64u);
            product_row<4, false>(p, a, b);
            product_row<4, false>(p + 4, a + 4, b);
            product_row<4, false>(p + 8, a + 8, b);
            product_row<4, true>(p + 12, a + 12, b);
            product_row<4, false>(t, p, m);
            product_row<4, false>(t + 4, p + 4, m);
            product_row<4, false>(t + 8, p + 8, m);
            product_row<4, true>(t + 12, p + 12, m);
        }
        spent += 78;
    }
    if (k) {
        set32(c.r[18], static_cast<u32>(i0) + k);
        c.budget -= spent;
    }
}

// Local-space bone matrix loop in 0x0046B510 (at L_0046B6CC): dst[k] = inv[k] * pose[k] for k in 0..count-2.
void native_loop_bone_matrices_local(Cpu &c) {
    const u32 end = c.r[1].w[0], dst_at = c.r[3].w[0], pose_at = c.r[4].w[0], inv_at = c.r[5].w[0];
    const u32 span = end - dst_at;
    if (span < 128u || (span & 63u) || span > 256u * 64u || ((dst_at | pose_at | inv_at) & 15u) || c.vu0.vf[0].w[3] != fp::kOne) return;
    const u32 count = (span >> 6) - 1u, bytes = count << 6;
    const u8 *inv = range(inv_at, bytes), *pose = range(pose_at, bytes);
    u8 *dst = range(dst_at, bytes);
    if (!inv || !pose || !dst) return;
    if (!hidden()) {
        for (u32 k = 0; k < count; ++k) {
            const auto *a = reinterpret_cast<const Float *>(inv + k * 64u);
            const auto *b = reinterpret_cast<const Float *>(pose + k * 64u);
            auto *t = reinterpret_cast<Float *>(dst + k * 64u);
            product_row<4, false>(t, a, b);
            product_row<4, false>(t + 4, a + 4, b);
            product_row<4, false>(t + 8, a + 8, b);
            product_row<4, true>(t + 12, a + 12, b);
        }
    }
    set32(c.r[3], dst_at + bytes);
    set32(c.r[4], pose_at + bytes);
    set32(c.r[5], inv_at + bytes);
    c.budget -= static_cast<std::int32_t>(count) * 30;
}

// RenderWare RwMatrixInvert (0x004C32A0): fast path for identity and orthonormal matrices; general matrices fall back to hot_004C32A0.
bool native_matrix_invert(Cpu &c) {
    constexpr u32 gp = 0x007D2CF0u;
    if (c.r[28].w[0] != gp) return false;
    const u32 dst_at = c.r[4].w[0], src_at = c.r[5].w[0];
    if ((dst_at | src_at) & 3u) return false;
    const u8 *src = range(src_at, 64u);
    u8 *dst = range(dst_at, 64u);
    if (!src || !dst) return false;
    const u32 off = word(g_ram + gp - 17432u);
    const u8 *gp_flags = range(0x00960070u + off + 4u, 4u);
    if (!gp_flags) return false;
    const u32 sflags = word(src + 12u);
    if ((word(gp_flags) & sflags & 0x00020000u) != 0u) {
        if (dst != src) std::memcpy(dst, src, 64u);
        set32(c.r[2], dst_at);
        c.budget -= 80;
        return true;
    }
    if ((sflags & 3u) != 3u) return false;
    float s[16];
    std::memcpy(s, src, 64u);
    const float r0 = s[0], r1 = s[1], r2 = s[2];
    const float u0 = s[4], u1 = s[5], u2 = s[6];
    const float a0 = s[8], a1 = s[9], a2 = s[10];
    const float p0 = s[12], p1 = s[13], p2 = s[14];
    float d[16];
    d[0] = r0; d[1] = u0; d[2] = a0;
    d[4] = r1; d[5] = u1; d[6] = a1;
    d[8] = r2; d[9] = u2; d[10] = a2;
    d[12] = -((p1 * r1 + p0 * r0) + p2 * r2);
    d[13] = -((p1 * u1 + p0 * u0) + p2 * u2);
    d[14] = -((p1 * a1 + p0 * a0) + p2 * a2);
    std::memcpy(dst, d, 12u);
    const u32 three = 3u;
    std::memcpy(dst + 12u, &three, 4u);
    std::memcpy(dst + 16u, d + 4, 12u);
    std::memcpy(dst + 32u, d + 8, 12u);
    std::memcpy(dst + 48u, d + 12, 12u);
    set32(c.r[2], dst_at);
    c.budget -= 59;
    return true;
}

// RenderWare RpAtomicGetWorldBoundingSphere (0x004912B0): skips dirty-check and transform call overhead.
bool native_atomic_world_sphere(Cpu &c) {
    const u32 atomic_at = c.r[4].w[0];
    if (atomic_at & 3u) return false;
    u8 *at = range(atomic_at, 80u);
    if (!at || (word(at + 76u) & 2u)) return false;
    const u32 frame_at = word(at + 4u);
    if (frame_at & 15u) return false;
    const u8 *fp = range(frame_at, 164u);
    if (!fp) return false;
    const u32 root_at = word(fp + 160u);
    if (root_at & 15u) return false;
    u8 *rp = range(root_at, 164u);
    if (!rp) return false;
    const u32 rflags = rp[3];
    if (!(rflags & 3u) && !(at[3] & 1u)) {
        set32(c.r[2], atomic_at + 44u);
        c.budget -= 28;
        return true;
    }
    std::int32_t spent = 45;
    if (rflags & 1u) {
        if (c.vu0.vf[0].w[3] != fp::kOne) return false;
        if (!sync_hierarchy_root_impl(rp, spent)) return false;
    }
    float m[16], lc[4];
    std::memcpy(m, fp + 80u, 64u);
    std::memcpy(lc, at + 28u, 16u);   // center (x, y, z) + radius at +40
    float wc[4];
    wc[0] = lc[0] * m[0] + lc[1] * m[4] + lc[2] * m[8] + m[12];
    wc[1] = lc[0] * m[1] + lc[1] * m[5] + lc[2] * m[9] + m[13];
    wc[2] = lc[0] * m[2] + lc[1] * m[6] + lc[2] * m[10] + m[14];
    if ((word(fp + 92u) & 3u) == 3u) {
        wc[3] = lc[3];
    } else {
        const float s0 = m[1] * m[1] + m[0] * m[0] + m[2] * m[2];
        const float s1 = m[5] * m[5] + m[4] * m[4] + m[6] * m[6];
        const float s2 = m[9] * m[9] + m[8] * m[8] + m[10] * m[10];
        const float mx = std::max(s0, std::max(s1, s2));
        wc[3] = lc[3] * std::sqrt(mx);
        spent += 30;
    }
    std::memcpy(at + 44u, wc, 16u);
    at[3] &= static_cast<u8>(~1u);
    set32(c.r[2], atomic_at + 44u);
    c.budget -= spent;
    return true;
}

// RenderWare scratchpad DMA packet allocator (0x004D59D0): checks room in the 8 KB scratchpad + MFIFO ring, flushes via ch8
// when needed, and manages DIRECT-mode header tags. Falls back to game code on ring full or non-SPR modes.
namespace {

bool dma_packet_can_alloc(u32 a0, u32 a1) {
    constexpr u32 gp = 0x007D2CF0u;
    if (a0 & 0x7FFFFFFFu) return false;
    const u32 mode = word(g_ram + gp - 17212u);
    if ((mode & 0x7FFFFFFFu) != 4353u) return false;
    const bool need_hdr = ((~mode) & 0x80000000u & a0) != 0u;
    const u32 bytes = (a1 + (need_hdr ? 5u : 4u)) << 4;
    const u32 spr_base = word(g_ram + gp - 17124u);
    const u32 spr_cur = word(g_ram + gp - 17148u);
    const u32 ring_cur = word(g_ram + gp - 17128u);
    const u32 ring_end = word(g_ram + gp - 17180u);
    const u32 used = spr_cur - spr_base;
    const u32 projected = ring_cur + used + bytes;
    if ((projected | ring_end) >= 0x80000000u || projected >= ring_end) return false;
    const bool flush1 = (bytes + used) >= 8193u && spr_cur != 0u && used != 0u;
    if ((flush1 || need_hdr) && (ld32(0x1000D000u) & 256u)) return false;
    return true;
}

bool dma_packet_alloc_impl(u32 a0, u32 a1, std::int32_t &spent) {
    constexpr u32 gp = 0x007D2CF0u;
    if (a0 & 0x7FFFFFFFu) return false;
    const u32 mode = word(g_ram + gp - 17212u);
    if ((mode & 0x7FFFFFFFu) != 4353u) return false;
    const bool need_hdr = ((~mode) & 0x80000000u & a0) != 0u;
    const u32 bytes = (a1 + (need_hdr ? 5u : 4u)) << 4;
    u32 spr_base = word(g_ram + gp - 17124u);
    u32 spr_cur = word(g_ram + gp - 17148u);
    u32 ring_cur = word(g_ram + gp - 17128u);
    const u32 ring_end = word(g_ram + gp - 17180u);
    const u32 used = spr_cur - spr_base;
    const u32 projected = ring_cur + used + bytes;
    if ((projected | ring_end) >= 0x80000000u || projected >= ring_end) return false;

    const bool flush1 = (bytes + used) >= 8193u && spr_cur != 0u && used != 0u;
    if ((flush1 || need_hdr) && (ld32(0x1000D000u) & 256u)) return false;

    auto put_w = [](u32 a, u32 v) { std::memcpy(g_ram + a, &v, 4); };
    spent += 52;
    auto flush_spr = [&](u32 flush_bytes) {
        const u32 sb = word(g_ram + gp - 17124u), rc = word(g_ram + gp - 17128u);
        st32(0x1000E010u, 256u);
        st32(0x1000D080u, sb);
        st32(0x1000D010u, rc);
        st32(0x1000D020u, flush_bytes >> 4);
        st32(0x1000D000u, 256u);
        const u32 p208 = word(g_ram + gp - 17208u);
        if (p208 && (p208 & ~16383u) == 0x70000000u) put_w(gp - 17208u, (rc + (p208 - sb)) | 0x20000000u);
        put_w(gp - 17128u, rc + flush_bytes);
        const u32 next_sb = sb ^ 8192u;
        put_w(gp - 17124u, next_sb);
        put_w(gp - 17148u, next_sb);
        spent += 33;
    };

    if (flush1) {
        flush_spr(used);
        spr_base = word(g_ram + gp - 17124u);
        spr_cur = word(g_ram + gp - 17148u);
        ring_cur = word(g_ram + gp - 17128u);
    }

    put_w(gp - 17184u, ring_cur + (spr_cur - spr_base) + bytes);
    const u32 mbit = mode & 0x80000000u;
    if (mbit != a0) {
        if ((mbit & ~a0) != 0u) {
            const u32 tag_at = word(g_ram + gp - 17204u);
            const u32 qwc_m1 = (((ring_cur - tag_at) + (spr_cur - spr_base)) >> 4) - 1u;
            if (qwc_m1 == 0u) {
                ring_cur -= 16u;
                put_w(gp - 17128u, ring_cur);
            } else {
                Reg128 q{};
                q.w[0] = 0x10000000u | qwc_m1;
                q.w[3] = 0x50000000u | qwc_m1;
                st128((tag_at | 0x20000000u) & ~15u, q);
            }
            put_w(gp - 17204u, 0u);
            put_w(gp - 17212u, mode & 0x7FFFFFFFu);
            spent += 25;
        }
        const u32 cur_mode = word(g_ram + gp - 17212u);
        if (((~cur_mode) & 0x80000000u & a0) != 0u) {
            if (spr_cur != 0u && spr_cur != spr_base) {
                flush_spr(spr_cur - spr_base);
                ring_cur = word(g_ram + gp - 17128u);
            }
            put_w(gp - 17204u, ring_cur);
            put_w(gp - 17128u, ring_cur + 16u);
            put_w(gp - 17212u, cur_mode | 0x80000000u);
            spent += 18;
        }
    }
    return true;
}

}   // namespace

bool native_dma_packet_alloc(Cpu &c) {
    if (c.r[28].w[0] != 0x007D2CF0u) return false;
    std::int32_t spent = 0;
    if (!dma_packet_alloc_impl(c.r[4].w[0], c.r[5].w[0], spent)) return false;
    set32(c.r[2], 1u);
    c.budget -= spent;
    return true;
}

// Material integer user-data lookup by name (0x001A6400): replaces an O(N^2) 5-call loop over RenderWare's RpUserDataArray.
bool native_find_material_userdata(Cpu &c) {
    constexpr u32 gp = 0x007D2CF0u;
    if (c.r[28].w[0] != gp) return false;
    const u32 a0 = c.r[4].w[0], a1 = c.r[5].w[0];
    if (a1 >= kRamSize) return false;
    const u32 off = word(g_ram + gp - 17680u);
    const u8 *hdr = range(a0 + off, 8u);
    if (!hdr) return false;
    const auto total = static_cast<std::int32_t>(word(hdr));
    if (total <= 0) {
        set32(c.r[2], 0u);
        c.budget -= 35;
        return true;
    }
    if (total > 256) return false;
    const u32 base_at = word(hdr + 4u);
    const u8 *base = range(base_at, static_cast<u32>(total) * 16u);
    if (!base) return false;
    std::int32_t valid = 0;
    for (std::int32_t k = 0; k < total; ++k)
        if (word(base + static_cast<u32>(k) * 16u + 12u) != 0u) ++valid;
    u32 result = 0u;
    const u8 *needle = g_ram + a1;
    for (std::int32_t i = 0; i < valid; ++i) {
        const u32 entry_off = static_cast<u32>(i) * 16u;
        const u32 name_at = word(base + entry_off);
        if (name_at >= kRamSize) return false;
        const u8 *p = g_ram + name_at;
        const u32 lim = kRamSize - (name_at > a1 ? name_at : a1);
        u32 j = 0;
        while (j < lim && p[j] == needle[j] && p[j] != 0) ++j;
        if (j == lim) return false;
        if (p[j] == needle[j] && word(base + entry_off + 4u) == 1u) {
            const u32 data_at = word(base + entry_off + 12u);
            const u8 *dp = range(data_at, 4u);
            if (!dp) return false;
            result = word(dp);
            break;
        }
    }
    set32(c.r[2], result);
    c.budget -= 35 + valid * (25 + total * 7);
    return true;
}

namespace {

bool skin_bone_dma_can_run(u32 arg_at, u32 &extra_qwc) {
    const u8 *arg = range(arg_at, 48u);
    if (!arg) return false;
    const u32 g_at = word(g_ram + 0x0095DE38u);
    const u8 *g = range(g_at, 24u);
    if (!g) return false;
    const u32 x5 = word(arg + 12u), x3 = word(arg + 44u);
    const auto diff = static_cast<std::int32_t>(x3 - (x5 + 16u));
    const auto x19 = static_cast<std::int32_t>((static_cast<std::int64_t>(0x2AAAAAAB) * diff) >> 33) +
                     static_cast<std::int32_t>(static_cast<u32>(diff) >> 31);
    const u32 x16 = word(g);
    if (x16 > 256u) return false;
    const u32 tbl_a_at = word(g + 16u);
    const u8 *entry_a = range(tbl_a_at + static_cast<u32>(x19 << 1), 2u);
    if (!entry_a) return false;
    const u32 start_idx = entry_a[0], run_count = entry_a[1];
    const u32 tbl_b_at = word(g + 20u), remap_at = word(g + 12u);
    const u32 mat_base_at = word(g_ram + 0x0095DE1Cu);
    if (run_count > 0) {
        const u8 *tbl_b = range(tbl_b_at + (start_idx << 1), run_count * 2u);
        if (!tbl_b || mat_base_at >= kRamSize || remap_at >= kRamSize) return false;
        for (u32 r = 0; r < run_count; ++r) {
            const u32 sb = tbl_b[r * 2u], cnt = tbl_b[r * 2u + 1u];
            if (sb + cnt > kRamSize - remap_at || (sb + cnt) * 64u > kRamSize - mat_base_at) return false;
            for (u32 k = 0; k < cnt; ++k)
                if (g_ram[remap_at + sb + k] >= x16) return false;
        }
    }
    extra_qwc = (x16 << 2) + 3u;
    return dma_packet_can_alloc(0u, extra_qwc);
}

bool skin_bone_dma_impl(u32 arg_at, std::int32_t &spent) {
    constexpr u32 gp = 0x007D2CF0u;
    const u8 *arg = range(arg_at, 48u);
    if (!arg) return false;
    const u32 g_at = word(g_ram + 0x0095DE38u);
    const u8 *g = range(g_at, 24u);
    if (!g) return false;
    const u32 x5 = word(arg + 12u), x3 = word(arg + 44u);
    const auto diff = static_cast<std::int32_t>(x3 - (x5 + 16u));
    const auto x19 = static_cast<std::int32_t>((static_cast<std::int64_t>(0x2AAAAAAB) * diff) >> 33) +
                     static_cast<std::int32_t>(static_cast<u32>(diff) >> 31);
    const u32 x16 = word(g);
    if (x16 > 256u) return false;
    const u32 x18 = x16 << 2, x20 = x18 + 1u;
    const u32 tbl_a_at = word(g + 16u);
    const u8 *entry_a = range(tbl_a_at + static_cast<u32>(x19 << 1), 2u);
    if (!entry_a) return false;
    const u32 start_idx = entry_a[0], run_count = entry_a[1];
    const u32 tbl_b_at = word(g + 20u), remap_at = word(g + 12u);
    const u32 mat_base_at = word(g_ram + 0x0095DE1Cu);
    const u8 *tbl_b = nullptr;
    if (run_count > 0) {
        tbl_b = range(tbl_b_at + (start_idx << 1), run_count * 2u);
        if (!tbl_b || mat_base_at >= kRamSize || remap_at >= kRamSize) return false;
        for (u32 r = 0; r < run_count; ++r) {
            const u32 sb = tbl_b[r * 2u], cnt = tbl_b[r * 2u + 1u];
            if (sb + cnt > kRamSize - remap_at || (sb + cnt) * 64u > kRamSize - mat_base_at) return false;
            for (u32 k = 0; k < cnt; ++k)
                if (g_ram[remap_at + sb + k] >= x16) return false;
        }
    }
    if (!dma_packet_alloc_impl(0u, x20 + 2u, spent)) return false;
    const u32 spr = word(g_ram + gp - 17148u);
    const u32 packet_bytes = (2u + (x16 << 2)) * 16u;
    u8 *pkt = range(spr & ~15u, packet_bytes);
    if (!pkt) return false;
    const u32 hdr0[4] = {0x10000000u | x20, 0u, 0u, 0u};
    const u32 hdr1[4] = {0u, 0u, 0x01000101u, 0x6C0002BCu | ((x18 & 255u) << 16)};
    std::memcpy(pkt, hdr0, 16u);
    std::memcpy(pkt + 16u, hdr1, 16u);
    u8 *dst_base = pkt + 32u;
    for (u32 r = 0; r < run_count; ++r) {
        const u32 sb = tbl_b[r * 2u], cnt = tbl_b[r * 2u + 1u];
        for (u32 k = 0; k < cnt; ++k) {
            const u32 dst_slot = g_ram[remap_at + sb + k];
            std::memcpy(dst_base + dst_slot * 64u, g_ram + mat_base_at + (sb + k) * 64u, 64u);
            spent += 21;
        }
        spent += 11;
    }
    const u32 next_spr = spr + 32u + (x16 << 6);
    std::memcpy(g_ram + gp - 17148u, &next_spr, 4u);
    spent += 76;
    return true;
}

}   // namespace

// RenderWare skin bone-matrix DMA packet builder (0x0046B8E0): allocates a scratchpad packet and copies remapped 64-byte bone matrices.
bool native_skin_bone_dma(Cpu &c) {
    constexpr u32 gp = 0x007D2CF0u;
    if (c.r[28].w[0] != gp) return false;
    std::int32_t spent = 0;
    if (!skin_bone_dma_impl(c.r[4].w[0], spent)) return false;
    set32(c.r[2], 1u);
    c.budget -= spent;
    return true;
}

// RenderWare matrix + VU constants DMA packet builder (0x004EC2B0).
bool native_rw_dma_matrix_packet(Cpu &c) {
    constexpr u32 gp = 0x007D2CF0u;
    if (c.r[28].w[0] != gp) return false;
    const u32 a0 = c.r[4].w[0], a1 = c.r[5].w[0], a2 = c.r[6].w[0];
    if (a2 > 512u) return false;
    const u8 *mat = a0 ? range(a0, 60u) : nullptr;
    if (a0 && !mat) return false;
    const u8 *extra = a2 ? range(a1 & ~15u, a2 * 16u) : nullptr;
    if (a2 && !extra) return false;
    const u32 x2 = a0 ? 6u : 0u;
    const u32 x21 = a0 ? 1008u : 1014u;
    const u32 x17 = x2 + 4u;
    const u32 x16 = x17 + 3u + a2;
    std::int32_t spent = 0;
    if (!dma_packet_alloc_impl(0u, x16 + 2u, spent)) return false;
    const u32 spr = word(g_ram + gp - 17148u);
    const u32 total_bytes = (3u + x2 + 5u + a2) * 16u;
    u8 *dst = range(spr & ~15u, total_bytes);
    if (!dst) return false;
    const u32 tag[4] = {0x10000000u | x16, 0u, 0u, 0u};
    const u32 vif[4] = {0u, 0u, 0x01000404u, 0x6C000000u | (x17 << 16) | x21};
    std::memcpy(dst, tag, 16u);
    std::memcpy(dst + 16u, g_ram + 0x009659C0u, 16u);
    std::memcpy(dst + 32u, vif, 16u);
    u32 off = 48u;
    if (mat) {
        for (u32 r = 0; r < 4u; ++r) {
            const u32 row[4] = {word(mat + r * 16u), word(mat + r * 16u + 4u), word(mat + r * 16u + 8u), word(mat + r * 16u + 8u)};
            std::memcpy(dst + off, row, 16u);
            off += 16u;
        }
        std::memset(dst + off, 0, 32u);
        off += 32u;
    }
    std::memcpy(dst + off, g_ram + 0x00965970u, 80u);
    off += 80u;
    if (extra) {
        std::memcpy(dst + off, extra, a2 * 16u);
        off += a2 * 16u;
    }
    const u32 next_spr = spr + off;
    std::memcpy(g_ram + gp - 17148u, &next_spr, 4u);
    set32(c.r[2], 1u);
    c.budget -= spent + 80 + (a0 ? 39 : 0) + static_cast<std::int32_t>(a2) * 7;
    return true;
}

// RenderWare PS2 GS raster VRAM check/address update (0x004F0F30). Falls back on rare texture upload.
namespace {

bool rw_tex_check_impl(u32 a0, std::int32_t &spent) {
    constexpr u32 gp = 0x007D2CF0u;
    if (!a0) return false;
    const u32 ext_off = word(g_ram + gp - 16968u);
    const u8 *ras = range(a0, 36u);
    const u8 *ext = range(a0 + ext_off, 92u);
    if (!ras || !ext) return false;
    if (a0 == word(g_ram + 0x0077F224u)) { spent += 12; return true; }
    if (ras[32] & 0x80u) { spent += 16; return true; }
    if (word(ras + 4u) == 0u) { spent += 19; return true; }
    if (word(ext + 88u) != 0u) { spent += 22; return true; }
    return false;
}

bool rw_tex_needs_upload(u32 a0) {
    constexpr u32 gp = 0x007D2CF0u;
    if (!a0) return true;
    const u32 ext_off = word(g_ram + gp - 16968u);
    const u8 *ras = range(a0, 36u);
    const u8 *ext = range(a0 + ext_off, 92u);
    if (!ras || !ext) return true;
    return a0 != word(g_ram + 0x0077F224u) && !(ras[32] & 0x80u) && word(ras + 4u) != 0u && word(ext + 88u) == 0u;
}

bool rw_tex_dma_impl(u32 a0, std::uint64_t a1_64, u32 &ret, std::int32_t &spent) {
    constexpr u32 gp = 0x007D2CF0u;
    if (!a0) return false;
    const u32 a1 = static_cast<u32>(a1_64);
    const u32 ext_off = word(g_ram + gp - 16968u);
    const u32 ext_at = a0 + ext_off;
    const u8 *ras = range(a0, 36u);
    u8 *ext = range(ext_at, 92u);
    if (!ras || !ext) return false;
    if (a0 != word(g_ram + 0x0077F224u) && !(ras[32] & 0x80u) && word(ras + 4u) != 0u && word(ext + 88u) == 0u)
        return false;   // rare upload callback

    const u32 cache_at = word(ext + 88u);
    const u32 vram_mgr = word(g_ram + 0x0077F200u);
    if (cache_at != 0u && vram_mgr != 0u) {
        const u8 *cache = range(cache_at, 8u);
        if (!cache) return false;
        const u32 tbp = word(cache + 4u) >> 6;
        const u32 w8 = (word(ext + 8u) & ~0x3FFFu) | (tbp & 0x3FFFu);
        const u32 cbp = (tbp + word(ext + 16u)) & 0x3FFFu;
        const u32 w12 = (word(ext + 12u) & 0xFFF8001Fu) | (cbp << 5);
        std::memcpy(ext + 8u, &w8, 4u);
        std::memcpy(ext + 12u, &w12, 4u);
    }

    const u32 rtype = ras[32] & 7u;
    if (rtype != 2u && cache_at == 0u) {
        ret = 0u;
        spent += 34;
        return true;
    }

    if (word(g_ram + 0x0077F224u) == a0 && word(g_ram + gp - 16948u) != 0xFFFFFFFFu &&
        static_cast<std::int64_t>(static_cast<std::int32_t>(word(g_ram + 0x0077F228u))) == static_cast<std::int64_t>(a1_64)) {
        if (rtype != 2u || word(ras) == a0) {
            ret = 1u;
            spent += 48;
            return true;
        }
    }

    const u32 nqwc = (a1_64 == 0ull) ? 6u : 5u;
    if (!dma_packet_can_alloc(0x80000000u, nqwc)) return false;

    std::memcpy(g_ram + 0x0077F224u, &a0, 4u);
    std::memcpy(g_ram + 0x0077F228u, &a1, 4u);
    if (cache_at != 0u && vram_mgr != 0u) {
        const auto diff = static_cast<std::int32_t>(cache_at - word(g_ram + 0x0077F214u));
        const u32 idx = static_cast<u32>(static_cast<std::int32_t>((static_cast<std::int64_t>(0x2AAAAAAB) * diff) >> 33) +
                                         static_cast<std::int32_t>(static_cast<u32>(diff) >> 31));
        std::memcpy(g_ram + (a1_64 == 0ull ? 0x0077F22Cu : 0x0077F230u), &idx, 4u);
    }
    if (ext[23] == 0u && word(g_ram + gp - 16856u) == 0u && vram_mgr != 0u) {
        const u32 z = 0u;
        std::memcpy(g_ram + gp - 17208u, &z, 4u);
    }

    if (!dma_packet_alloc_impl(0x80000000u, nqwc, spent)) return false;
    const u32 spr = word(g_ram + gp - 17148u);
    u8 *dst = range(spr & ~15u, nqwc * 16u);
    if (!dst) return false;

    auto put_q = [](u8 *p, std::uint64_t lo, std::uint64_t hi) {
        std::memcpy(p, &lo, 8u);
        std::memcpy(p + 8u, &hi, 8u);
    };

    const std::uint64_t gif_lo = (a1_64 == 0ull ? 5ull : 4ull) | 0x8000ull | (0x10000000ull << 32);
    put_q(dst, gif_lo, 14ull);
    put_q(dst + 16u, 0ull, 63ull);

    const u32 tex0_lo = word(ext + 8u);
    const u32 tex0_hi = word(ext + 12u);
    std::uint64_t tex0 = static_cast<std::uint64_t>(tex0_lo) | (static_cast<std::uint64_t>(tex0_hi) << 32);
    if (rtype == 2u) {
        const u32 fb_addr = (word(g_ram + gp - 17016u) & 1u) ? 0x00964B60u : 0x009649F0u;
        std::uint64_t fb;
        std::memcpy(&fb, g_ram + fb_addr, 8u);
        const std::uint64_t fbp = ((fb & 511ull) << 5) + (tex0_lo & 16383u);
        tex0 = (tex0 & 0xFFFFFFFFFFFFC000ull) | fbp;
    }
    put_q(dst + 32u, tex0, a1_64 == 0ull ? 6ull : 7ull);
    u32 off = 48u;
    if (a1_64 == 0ull) {
        std::uint64_t tex1;
        std::memcpy(&tex1, g_ram + gp - 17040u, 8u);
        std::uint16_t raw20;
        std::memcpy(&raw20, ext + 20u, 2u);
        const std::uint64_t k = static_cast<std::uint64_t>(raw20 & 4095u) << 32;
        const std::uint64_t l = static_cast<std::uint64_t>((raw20 >> 12) & 3u) << 19;
        tex1 = (tex1 & 0xFFFFF000FFE7FDE3ull) | k | l | static_cast<std::uint64_t>(ext[22]);
        std::memcpy(g_ram + gp - 17040u, &tex1, 8u);
        put_q(dst + off, tex1, 20ull);
        off += 16u;
    }

    std::uint64_t mip1 = static_cast<std::uint64_t>(word(ext + 24u)) | (static_cast<std::uint64_t>(word(ext + 28u)) << 32);
    std::uint64_t mip2 = static_cast<std::uint64_t>(word(ext + 32u)) | (static_cast<std::uint64_t>(word(ext + 36u)) << 32);
    if (vram_mgr != 0u) {
        const std::uint64_t tbp = tex0_lo & 16383u;
        const std::uint64_t add = tbp | (tbp << 20) | (tbp << 40);
        mip1 += add;
        mip2 += add;
    }
    put_q(dst + off, mip1, a1_64 == 0ull ? 52ull : 53ull);
    put_q(dst + off + 16u, mip2, a1_64 == 0ull ? 54ull : 55ull);

    const u32 next_spr = spr + nqwc * 16u;
    std::memcpy(g_ram + gp - 17148u, &next_spr, 4u);
    const u32 z = 0u;
    std::memcpy(g_ram + gp - 16856u, &z, 4u);
    ret = 1u;
    spent += 150;
    return true;
}

bool rw_dma_ring_can_push() {
    constexpr u32 gp = 0x007D2CF0u;
    std::uint16_t wr, cap, rd;
    std::memcpy(&wr, g_ram + gp - 17268u, 2u);
    std::memcpy(&cap, g_ram + gp - 20292u, 2u);
    std::memcpy(&rd, g_ram + gp - 17264u, 2u);
    std::uint16_t next_wr = static_cast<std::uint16_t>(wr + 1u);
    if (next_wr == cap) next_wr = 0u;
    if (next_wr == rd) return false;
    return range(word(g_ram + gp - 17260u) + static_cast<u32>(wr) * 4u, 4u) != nullptr;
}

bool rw_dma_ring_push_impl(u32 val, std::int32_t &spent) {
    constexpr u32 gp = 0x007D2CF0u;
    std::uint16_t wr, cap, rd;
    std::memcpy(&wr, g_ram + gp - 17268u, 2u);
    std::memcpy(&cap, g_ram + gp - 20292u, 2u);
    std::memcpy(&rd, g_ram + gp - 17264u, 2u);
    std::uint16_t next_wr = static_cast<std::uint16_t>(wr + 1u);
    if (next_wr == cap) next_wr = 0u;
    if (next_wr == rd) return false;
    u8 *slot = range(word(g_ram + gp - 17260u) + static_cast<u32>(wr) * 4u, 4u);
    if (!slot) return false;
    std::memcpy(slot, &val, 4u);
    std::memcpy(g_ram + gp - 17268u, &next_wr, 2u);
    spent += 25;
    return true;
}

inline void rw_tex_filter_impl(std::uint64_t mode) {
    constexpr u32 gp = 0x007D2CF0u;
    std::uint64_t tex1;
    std::memcpy(&tex1, g_ram + gp - 17040u, 8u);
    std::uint64_t bits = 0ull;
    switch (mode) {
    case 6ull: bits = 352ull; break;
    case 5ull: bits = 192ull; break;
    case 4ull: bits = 288ull; break;
    case 3ull: bits = 128ull; break;
    case 2ull: bits = 96ull; break;
    default: break;
    }
    tex1 = (tex1 & ~480ull) | bits;
    std::memcpy(g_ram + gp - 17040u, &tex1, 8u);
}

}   // namespace

bool native_rw_tex_check(Cpu &c) {
    if (c.r[28].w[0] != 0x007D2CF0u) return false;
    std::int32_t spent = 0;
    if (!rw_tex_check_impl(c.r[4].w[0], spent)) return false;
    c.budget -= spent;
    return true;
}

bool native_rw_tex_dma(Cpu &c) {
    if (c.r[28].w[0] != 0x007D2CF0u) return false;
    u32 ret = 0u;
    std::int32_t spent = 0;
    if (!rw_tex_dma_impl(c.r[4].w[0], c.r[5].d[0], ret, spent)) return false;
    set32(c.r[2], ret);
    c.budget -= spent;
    return true;
}

// RenderWare DMA ring push (0x004D51C0): enqueues a0 when the ring has room; falls back to flush when half-full.
bool native_rw_dma_ring_push(Cpu &c) {
    if (c.r[28].w[0] != 0x007D2CF0u) return false;
    std::int32_t spent = 0;
    if (!rw_dma_ring_push_impl(c.r[4].w[0], spent)) return false;
    c.budget -= spent;
    return true;
}

// RenderWare TEX1 filter bits helper (0x0049CA50).
bool native_rw_tex_filter(Cpu &c) {
    if (c.r[28].w[0] != 0x007D2CF0u) return false;
    rw_tex_filter_impl(c.r[4].d[0]);
    c.budget -= 14;
    return true;
}

// RenderWare u8 RGBA -> float4 scratchpad writer (0x0049CAB0).
bool native_rw_mat_color_u8(Cpu &c) {
    constexpr u32 gp = 0x007D2CF0u;
    if (c.r[28].w[0] != gp) return false;
    const u8 *src = range(c.r[4].w[0], 4u);
    const u32 spr = word(g_ram + gp - 17148u);
    u8 *dst = range(spr & ~15u, 16u);
    if (!src || !dst) return false;
    const float scale = std::bit_cast<float>(c.f[12]);
    const float ascale = std::bit_cast<float>(0x3B011B50u);
    const float out[4] = {static_cast<float>(src[0]) * scale, static_cast<float>(src[1]) * scale,
                          static_cast<float>(src[2]) * scale, static_cast<float>(src[3]) * ascale};
    std::memcpy(dst, out, 16u);
    const u32 next_spr = spr + 16u;
    std::memcpy(g_ram + gp - 17148u, &next_spr, 4u);
    c.budget -= 44;
    return true;
}

// RenderWare f32 RGB * 255 + f12 scratchpad writer (0x0049CBD0).
bool native_rw_mat_color_f32(Cpu &c) {
    constexpr u32 gp = 0x007D2CF0u;
    if (c.r[28].w[0] != gp) return false;
    const u8 *src = range(c.r[4].w[0], 12u);
    const u32 spr = word(g_ram + gp - 17148u);
    u8 *dst = range(spr & ~15u, 16u);
    if (!src || !dst) return false;
    float rgb[3];
    std::memcpy(rgb, src, 12u);
    const float k255 = std::bit_cast<float>(0x437F0001u);
    const float out[4] = {rgb[0] * k255, rgb[1] * k255, rgb[2] * k255, std::bit_cast<float>(c.f[12])};
    std::memcpy(dst, out, 16u);
    const u32 next_spr = spr + 16u;
    std::memcpy(g_ram + gp - 17148u, &next_spr, 4u);
    c.budget -= 22;
    return true;
}

namespace {

// RenderWare IM3D DMA packet header builder helper (0x004EC100).
bool rw_im3d_header_impl(u32 a0, u32 a1, u32 a2, u32 a3, std::uint64_t t0_64, std::int32_t &spent) {
    constexpr u32 gp = 0x007D2CF0u;
    const u32 t0 = static_cast<u32>(t0_64);
    const u32 cur_prog = word(g_ram + gp - 16888u);
    const bool same_prog = static_cast<std::int64_t>(static_cast<std::int32_t>(cur_prog)) == static_cast<std::int64_t>(t0_64);
    u32 qwc_need = a0 * ((a2 + 15u) >> 4) + 22u + a1 * a2;
    if (same_prog) qwc_need -= 1u;
    if (!dma_packet_alloc_impl(0u, qwc_need, spent)) return false;
    const u32 spr = word(g_ram + gp - 17148u);
    const u32 out_bytes = same_prog ? 128u : 144u;
    u8 *dst = range(spr & ~15u, out_bytes);
    if (!dst) return false;
    auto put_q = [](u8 *p, std::uint64_t lo, std::uint64_t hi) {
        std::memcpy(p, &lo, 8u);
        std::memcpy(p + 8u, &hi, 8u);
    };
    std::memcpy(dst, g_ram + 0x00965880u, 16u);
    put_q(dst + 16u, 0x01000404ull, 0x11000000ull);
    std::memcpy(dst + 32u, g_ram + 0x00965A00u, 16u);
    std::memcpy(dst + 48u, g_ram + 0x00965A20u, 16u);
    std::uint64_t m;
    std::memcpy(&m, g_ram + gp - 20272u, 8u);
    const u32 bits = static_cast<u32>(m | static_cast<std::uint64_t>(a3)) & 2047u;
    const u32 w = (word(g_ram + 0x009658A4u) & 0xFC007FFFu) | (bits << 15);
    std::memcpy(g_ram + 0x009658A4u, &w, 4u);
    std::memcpy(dst + 64u, g_ram + 0x009658A0u, 16u);
    std::memcpy(dst + 80u, g_ram + ((m & 16u) ? 0x009659F0u : 0x009659E0u), 16u);
    std::memcpy(dst + 96u, g_ram + 0x00965880u, 16u);
    std::memcpy(dst + 112u, g_ram + 0x009659B0u, 16u);
    if (!same_prog) {
        put_q(dst + 128u, 0x50000000ull | (static_cast<std::uint64_t>(t0) << 32), 0ull);
        std::memcpy(g_ram + gp - 16888u, &t0, 4u);
    }
    const u32 next_spr = spr + out_bytes;
    std::memcpy(g_ram + gp - 17148u, &next_spr, 4u);
    spent += same_prog ? 85 : 97;
    return true;
}

}   // namespace

// RenderWare IM3D DMA packet header builder (0x004EC100).
bool native_rw_im3d_header(Cpu &c) {
    constexpr u32 gp = 0x007D2CF0u;
    if (c.r[28].w[0] != gp) return false;
    std::int32_t spent = 0;
    if (!rw_im3d_header_impl(c.r[4].w[0], c.r[5].w[0], c.r[6].w[0], c.r[7].w[0], c.r[8].d[0], spent)) return false;
    set32(c.r[2], 1u);
    c.budget -= spent;
    return true;
}

// RenderWare IM3D triangle-strip submitter (0x004ED570): builds header, batched 3x64-byte strip triangles, and 0x004EC4C0 trailer natively.
bool native_rw_im3d_tristrip(Cpu &c) {
    constexpr u32 gp = 0x007D2CF0u;
    if (c.r[28].w[0] != gp) return false;
    const std::uint64_t nverts = c.r[7].d[0];
    if (nverts < 3ull) {
        c.budget -= 35;
        return true;
    }
    if (nverts > 1026ull) return false;
    const u32 tris = static_cast<u32>(nverts - 2ull);
    const u32 max_chunk = word(g_ram + gp - 16868u);
    if (max_chunk == 0u || tris > max_chunk) return false;
    const u32 desc_at = c.r[4].w[0];
    const u32 verts_at = c.r[6].w[0];
    if ((desc_at & 3u) || (verts_at & 15u)) return false;
    const u8 *desc = range(desc_at, 12u);
    const u8 *verts = range(verts_at, (tris + 2u) * 64u);
    if (!desc || !verts) return false;
    if (word(desc) != 0x004EC100u) return false;
    const u32 prog = word(desc + 8u);
    const std::uint64_t prog64 = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(prog)));
    const u32 cur_prog = word(g_ram + gp - 16888u);
    const bool same_prog = cur_prog == prog;
    const u32 nbatches = (tris + 15u) >> 4;
    const u32 qwc_need = 3u * nbatches + 22u + 12u * tris - (same_prog ? 1u : 0u);
    if (!dma_packet_can_alloc(0u, qwc_need)) return false;

    std::int32_t spent = 40;
    if (!rw_im3d_header_impl(3u, 12u, tris, 5u, prog64, spent)) return false;

    u32 spr = word(g_ram + gp - 17148u);
    const u32 body_bytes = (nbatches * 3u + tris * 12u + 4u) * 16u;
    u8 *dst = range(spr & ~15u, body_bytes);
    if (!dst) return false;

    auto put_q = [](u8 *p, std::uint64_t lo, std::uint64_t hi) {
        std::memcpy(p, &lo, 8u);
        std::memcpy(p + 8u, &hi, 8u);
    };

    u32 rem = tris;
    u32 x18 = 1u;
    const u8 *p = verts;
    const u8 *term_src = g_ram + 0x009658E0u;
    while (rem > 0u) {
        const u32 batch = rem < 16u ? rem : 16u;
        const u32 vqwc = batch * 12u;
        put_q(dst, 0x10000000ull | static_cast<std::uint64_t>(vqwc + 2u),
              (0x04000000ull | static_cast<std::uint64_t>(batch)) | (0x01000404ull << 32));
        put_q(dst + 16u, 0xC000C00020000000ull, (0x7C008000ull | (static_cast<std::uint64_t>(vqwc) << 16)) << 32);
        dst += 32u;
        for (u32 i = 0; i < batch; ++i) {
            const u8 *v1 = p + (x18 << 6);
            x18 ^= 3u;
            const u8 *v2 = p + (x18 << 6);
            std::memcpy(dst, p, 64u);
            std::memcpy(dst + 64u, v1, 64u);
            std::memcpy(dst + 128u, v2, 64u);
            p += 64u;
            dst += 192u;
        }
        std::memcpy(dst, term_src, 16u);
        dst += 16u;
        term_src = g_ram + 0x00965850u;
        rem -= batch;
        spent += 38 + static_cast<std::int32_t>(batch) * 82;
    }

    // Inline 0x004EC4C0 trailer (4 qwords = 64 bytes).
    std::memcpy(dst, g_ram + 0x00965880u, 16u);
    put_q(dst + 16u, 0x11000000ull, 0ull);
    std::memcpy(dst + 32u, g_ram + 0x00965870u, 16u);
    std::memcpy(dst + 48u, g_ram + 0x009658F0u, 16u);
    spr += body_bytes;
    std::memcpy(g_ram + gp - 17148u, &spr, 4u);
    c.budget -= spent + 32;
    return true;
}

namespace {

bool rw_mat_dma_can_run(u32 mat_at, u32 extra_qwc) {
    if (!mat_at) return false;
    const u8 *mat = range(mat_at, 72u);
    if (!mat) return false;
    const u32 tex_at = word(mat + 64u);
    if (tex_at) {
        const u8 *tex = range(tex_at, 84u);
        if (!tex) return false;
        const u32 ras_at = word(tex);
        if (ras_at) {
            if (!range(ras_at, 36u)) return false;
            if (rw_tex_needs_upload(ras_at)) return false;
        }
    }
    const u32 col_at = word(mat + 60u);
    const u32 p4_at = word(mat + 4u);
    const u32 p48_at = word(mat + 48u);
    const u8 *col = range(col_at, 12u);
    const u8 *p4 = range(p4_at, 28u);
    const u8 *p48 = range(p48_at, 4u);
    if (!col || !p4 || !p48) return false;
    const u32 x8_at = word(p48);
    if (!range(x8_at, 36u)) return false;
    const u8 b40 = mat[40];
    const u8 idx = static_cast<u8>(((b40 & 32u) >> 5) | ((b40 & 24u) >> 2));
    const u32 prog_tbl = word(p4 + 24u);
    if (!range(prog_tbl + static_cast<u32>(idx) * 4u, 4u)) return false;
    if (!dma_packet_can_alloc(0x80000000u, 26u) || !dma_packet_can_alloc(0u, 26u + extra_qwc)) return false;
    if (!rw_dma_ring_can_push()) return false;
    return true;
}

bool rw_mat_dma_impl(Cpu &c, u32 mat_at, std::int32_t &spent) {
    constexpr u32 gp = 0x007D2CF0u;
    if (!mat_at) return false;
    u8 *mat = range(mat_at, 72u);
    if (!mat) return false;
    const u32 tex_at = word(mat + 64u);
    const u8 *tex = nullptr;
    u32 ras_at = 0u;
    const u8 *ras = nullptr;
    if (tex_at) {
        tex = range(tex_at, 84u);
        if (!tex) return false;
        ras_at = word(tex);
        if (ras_at) {
            ras = range(ras_at, 36u);
            if (!ras) return false;
            if (rw_tex_needs_upload(ras_at)) return false;
        }
    }
    const u32 col_at = word(mat + 60u);
    const u8 *col = range(col_at, 12u);
    const u32 p4_at = word(mat + 4u);
    const u8 *p4 = range(p4_at, 28u);
    const u32 p48_at = word(mat + 48u);
    const u8 *p48 = range(p48_at, 4u);
    if (!col || !p4 || !p48) return false;
    const u32 x8_at = word(p48);
    u8 *x8 = range(x8_at, 36u);
    if (!x8) return false;
    const u8 b40 = mat[40];
    const u8 idx = static_cast<u8>(((b40 & 32u) >> 5) | ((b40 & 24u) >> 2));
    const u32 prog_tbl = word(p4 + 24u);
    const u8 *prog_p = range(prog_tbl + static_cast<u32>(idx) * 4u, 4u);
    if (!prog_p) return false;
    if (!dma_packet_can_alloc(0x80000000u, 26u) || !dma_packet_can_alloc(0u, 26u)) return false;
    if (!rw_dma_ring_can_push()) return false;

    if (tex_at && ras_at) {
        if (!rw_tex_check_impl(ras_at, spent)) return false;
    }

    u32 has_tex_alpha = 0u;
    if (ras_at) {
        const u32 fmt = ras[35] & 15u;
        has_tex_alpha = (fmt == 1u || fmt == 5u) ? 1u : 0u;
    }
    std::memcpy(g_ram + gp - 16952u, &has_tex_alpha, 4u);

    if (ras_at != word(g_ram + gp - 16948u)) {
        std::uint64_t m;
        std::memcpy(&m, g_ram + gp - 20272u, 8u);
        if (ras_at == 0u) {
            m &= ~16ull;
            std::memcpy(g_ram + gp - 20272u, &m, 8u);
        } else {
            m |= 16ull;
            std::memcpy(g_ram + gp - 20272u, &m, 8u);
            u32 ret = 0u;
            if (!rw_tex_dma_impl(ras_at, 0ull, ret, spent)) return false;
        }
        std::memcpy(g_ram + gp - 16948u, &ras_at, 4u);
        if (tex_at) {
            const u32 w80 = word(tex + 80u);
            rw_tex_filter_impl(static_cast<std::uint64_t>(w80 & 0xFFu));
            std::uint64_t clamp = 0ull;
            if (((w80 >> 8) & 0xFu) == 3u) clamp |= 1ull;
            if (((w80 >> 12) & 0xFu) == 3u) clamp |= 4ull;
            std::memcpy(g_ram + gp - 17048u, &clamp, 8u);
            spent += 28;
        }
    }

    {
        std::uint64_t test1;
        std::memcpy(&test1, g_ram + gp - 17072u, 8u);
        if (has_tex_alpha) test1 |= 1ull;
        else test1 &= ~1ull;
        std::memcpy(g_ram + gp - 17072u, &test1, 8u);
    }

    if (!dma_packet_alloc_impl(0u, 18u, spent)) return false;
    const u32 spr = word(g_ram + gp - 17148u);
    u8 *dst = range(spr & ~15u, 288u);
    if (!dst) return false;
    auto put_q = [](u8 *p, std::uint64_t lo, std::uint64_t hi) {
        std::memcpy(p, &lo, 8u);
        std::memcpy(p + 8u, &hi, 8u);
    };

    put_q(dst + 0u, 0x1000000Full, 0ull);
    put_q(dst + 16u, 0ull, 0x01000404ull | (0x6C0103FAull << 32));

    const u32 has_mat_alpha = (mat[71] != 255u) ? 1u : 0u;
    std::memcpy(g_ram + gp - 16956u, &has_mat_alpha, 4u);
    std::uint64_t g_prim;
    std::memcpy(&g_prim, g_ram + gp - 20272u, 8u);
    if (has_mat_alpha | has_tex_alpha) g_prim |= 64ull;
    else g_prim &= ~64ull;
    std::memcpy(g_ram + gp - 20272u, &g_prim, 8u);

    std::uint64_t tag_lo, tag_hi;
    std::memcpy(&tag_lo, g_ram + 0x009658A0u, 8u);
    std::memcpy(&tag_hi, g_ram + 0x009658A8u, 8u);
    const u32 pbits = static_cast<u32>(g_prim | static_cast<std::uint64_t>(mat[41])) & 2047u;
    tag_lo = (tag_lo & 0xFC007FFFFFFFFFFFull) | (static_cast<std::uint64_t>(pbits) << 47);
    std::memcpy(g_ram + 0x009658A0u, &tag_lo, 8u);
    put_q(dst + 32u, tag_lo, tag_hi);
    put_q(dst + 48u, 0ull, 0x01000404ull | (0x6C0103FBull << 32));

    {
        const float cscale = (g_prim & 16u) ? std::bit_cast<float>(0x3B011B50u) : std::bit_cast<float>(0x3B808081u);
        const float ascale = std::bit_cast<float>(0x3B011B50u);
        const float rgba[4] = {static_cast<float>(mat[68]) * cscale, static_cast<float>(mat[69]) * cscale,
                               static_cast<float>(mat[70]) * cscale, static_cast<float>(mat[71]) * ascale};
        std::memcpy(dst + 64u, rgba, 16u);
    }
    put_q(dst + 80u, 0ull, 0x01000404ull | (0x6C0103FCull << 32));
    {
        float rgb[3], f12;
        std::memcpy(rgb, col, 12u);
        std::memcpy(&f12, mat + 28u, 4u);
        const float k255 = std::bit_cast<float>(0x437F0001u);
        const float out[4] = {rgb[0] * k255, rgb[1] * k255, rgb[2] * k255, f12};
        std::memcpy(dst + 96u, out, 16u);
    }
    put_q(dst + 112u, 0ull, 0x01000404ull | (0x6C0303FDull << 32));

    u32 x2 = b40 & 7u;
    const bool use_alt = (word(g_ram + gp - 16876u) != 0u && !(x2 & 4u)) ||
                         (word(g_ram + gp - 20232u) != 0u && (x2 & 4u));
    if (use_alt) {
        x2 |= 8u;
        std::memcpy(dst + 128u, g_ram + 0x00965930u, 32u);
    } else {
        std::memcpy(dst + 128u, g_ram + 0x00965910u, 32u);
    }

    const u32 x4 = (word(g_ram + gp - 16964u) == 3u) ? 32u : 0u;
    std::memcpy(g_ram + gp - 16884u, &x4, 4u);
    put_q(dst + 160u, static_cast<std::uint64_t>(x2),
          static_cast<std::uint64_t>(x4) | (static_cast<std::uint64_t>(word(g_ram + gp - 16880u)) << 32));

    std::uint64_t test1;
    std::memcpy(&test1, g_ram + gp - 17072u, 8u);
    if (tex_at) {
        std::uint64_t tex1, clamp1;
        std::memcpy(&tex1, g_ram + gp - 17040u, 8u);
        std::memcpy(&clamp1, g_ram + gp - 17048u, 8u);
        put_q(dst + 176u, 0ull, 0x50000004ull << 32);
        put_q(dst + 192u, 32771ull | (0x10000000ull << 32), 14ull);
        put_q(dst + 208u, test1, 71ull);
        put_q(dst + 224u, tex1, 20ull);
        put_q(dst + 240u, clamp1, 8ull);
    } else {
        put_q(dst + 176u, 0ull, 0ull);
        put_q(dst + 192u, 0ull, 0ull);
        put_q(dst + 208u, 0ull, 0x50000002ull << 32);
        put_q(dst + 224u, 32769ull | (0x10000000ull << 32), 14ull);
        put_q(dst + 240u, test1, 71ull);
    }
    u32 off = 256u;

    mat[43] = idx;
    const u32 prog = word(prog_p);
    if (word(g_ram + gp - 16888u) != prog) {
        put_q(dst + off, 0x50000000ull | (static_cast<std::uint64_t>(prog) << 32), 0ull);
        std::memcpy(g_ram + gp - 16888u, &prog, 4u);
        off += 16u;
    }
    const u32 x7 = word(p4 + 20u);
    const u32 x3_32 = word(x8 + 32u);
    put_q(dst + off, 0x50000000ull | (static_cast<std::uint64_t>(x3_32) << 32),
          0x03000000ull | (static_cast<std::uint64_t>(x7 | 0x02000000u) << 32));
    off += 16u;
    const u32 next_spr = spr + off;
    std::memcpy(g_ram + gp - 17148u, &next_spr, 4u);

    set_interrupts(c, false);
    const u32 cnt1 = word(x8 + 24u) + 1u;
    std::memcpy(x8 + 24u, &cnt1, 4u);
    set_interrupts(c, true);
    const u32 cnt2 = word(x8 + 28u) + 1u;
    std::memcpy(x8 + 28u, &cnt2, 4u);
    if (cnt2 == 1u) {
        if (!rw_dma_ring_push_impl(x8_at + 24u, spent)) return false;
    }

    spent += 230;
    return true;
}

}   // namespace

// RenderWare material + GS state DMA packet builder (0x004A0E00).
bool native_rw_mat_dma(Cpu &c) {
    constexpr u32 gp = 0x007D2CF0u;
    if (c.r[28].w[0] != gp) return false;
    std::int32_t spent = 0;
    if (!rw_mat_dma_impl(c, c.r[4].w[0], spent)) return false;
    set32(c.r[2], 1u);
    c.budget -= spent;
    return true;
}

// RenderWare PS2All material cache check (0x004A0C20): validates mesh vertex count and flags against cached DMA entry.
bool native_rw_mat_setup(Cpu &c) {
    const u32 p_at = c.r[4].w[0];
    if (p_at & 3u) return false;
    u8 *p = range(p_at, 60u);
    if (!p) return false;
    const u32 p12_at = word(p + 12u), p48_at = word(p + 48u), p44_at = word(p + 44u);
    if ((p12_at | p48_at | p44_at) & 3u) return false;
    const u8 *p12 = range(p12_at, 4u);
    const u8 *p48 = range(p48_at, 4u);
    const u8 *p44 = range(p44_at, 8u);
    if (!p12 || !p48 || !p44) return false;
    const u32 cache_at = word(p48);
    if (cache_at & 3u) return false;
    const u8 *cache = range(cache_at, 48u);
    if (!cache) return false;
    const u32 gflags = word(p12);
    u32 count = word(p44 + 4u);
    if (gflags & 2u) count = (count - 2u) * 3u;
    u32 mflags = word(p + 52u);
    if (count != word(cache + 36u)) {
        mflags |= 8u;
        std::memcpy(p + 52u, &mflags, 4u);
    }
    if ((mflags & 8u) == 0u) {
        std::memcpy(p + 56u, &gflags, 4u);
        if (gflags != word(cache + 44u)) {
            mflags |= 8u;
            std::memcpy(p + 52u, &mflags, 4u);
        }
    }
    set32(c.r[2], 1u);
    c.budget -= 28;
    return true;
}

// RenderWare skin bridge wrapper (0x00472130): uploads skin bone matrices if needed, then submits material DMA.
bool native_rw_skin_mat_dma(Cpu &c) {
    constexpr u32 gp = 0x007D2CF0u;
    if (c.r[28].w[0] != gp) return false;
    const u32 mat_at = c.r[4].w[0];
    const bool has_skin = word(g_ram + 0x0095DE38u) != 0u;
    u32 extra_qwc = 0u;
    if (has_skin && !skin_bone_dma_can_run(mat_at, extra_qwc)) return false;
    if (!rw_mat_dma_can_run(mat_at, extra_qwc)) return false;
    std::int32_t spent = 15;
    if (has_skin && !skin_bone_dma_impl(mat_at, spent)) return false;
    if (!rw_mat_dma_impl(c, mat_at, spent)) return false;
    set32(c.r[2], 1u);
    c.budget -= spent;
    return true;
}

// RenderWare MatFX bridge dispatcher (0x0046EA80): fast path when MatFX index == 0 (calls 0x004A0E00 directly).
bool native_rw_matfx_bridge(Cpu &c) {
    constexpr u32 gp = 0x007D2CF0u;
    if (c.r[28].w[0] != gp) return false;
    const u32 mat_at = c.r[4].w[0];
    const u8 *mat = range(mat_at, 48u);
    if (!mat) return false;
    const u32 mesh_at = word(mat + 44u);
    const u8 *mesh = range(mesh_at, 12u);
    if (!mesh) return false;
    const u32 m_at = word(mesh + 8u);
    const u32 off = word(g_ram + gp - 17848u);
    const u8 *ext_ptr = range(m_at + off, 4u);
    if (!ext_ptr) return false;
    const u32 ext_at = word(ext_ptr);
    u32 idx = 0u;
    if (ext_at != 0u) {
        const u8 *ext = range(ext_at + 128u, 4u);
        if (!ext) return false;
        idx = word(ext);
    }
    if (idx != 0u) return false;
    std::int32_t spent = 28;
    if (!rw_mat_dma_impl(c, mat_at, spent)) return false;
    set32(c.r[2], 1u);
    c.budget -= spent;
    return true;
}

// Inner mesh loop of _rxPS2AllNode (0x0049F5D0, L_0049F7AC): runs clean non-dirty meshes up to num_meshes - 1 natively.
void native_loop_rw_mesh(Cpu &c) {
    constexpr u32 gp = 0x007D2CF0u;
    if (c.r[28].w[0] != gp) return;
    const u32 sp = c.r[29].w[0];
    if (sp & 3u) return;
    u8 *frame = range(sp + 160u, 192u);
    if (!frame) return;
    const u32 mh_at = word(frame);   // sp + 160
    const u8 *mh = range(mh_at, 8u);
    if (!mh) return;
    std::uint16_t num_meshes;
    std::memcpy(&num_meshes, mh + 4u, 2u);
    u32 idx = c.r[21].w[0];
    if (idx + 1u >= static_cast<u32>(num_meshes)) return;

    const u32 x20 = c.r[20].w[0];
    if ((x20 & 3u) || !range(x20, 28u)) return;
    const u32 pipe_override = word(g_ram + x20 + 24u);
    const u32 def_pipe = word(frame + 16u);   // sp + 176
    const u32 base_flags = word(frame + 132u);   // sp + 292
    const u8 b312 = frame[152u] & 31u;   // sp + 312
    const u8 use_mat_col = frame[154u];   // sp + 314
    const u32 mat_arg_at = sp + 272u;   // frame + 112
    const u32 head_base_off = word(g_ram + gp - 17408u);
    const u8 *head_ptr_p = range(0x00960070u + head_base_off + 36u, 4u);
    if (!head_ptr_p) return;
    const u32 lru_head_at = word(head_ptr_p);
    if ((lru_head_at & 3u) || !range(lru_head_at, 4u)) return;

    u32 x18 = c.r[18].w[0];
    u32 x19 = c.r[19].w[0];
    u32 x22 = c.r[22].w[0];
    u32 x30 = c.r[30].w[0];
    std::int32_t spent = 0;
    u32 done = 0u;
    const u32 limit = static_cast<u32>(num_meshes) - 1u;

    while (idx < limit) {
        if ((x18 | x22) & 3u) break;
        const u8 *mesh = range(x18, 12u);
        const u8 *slot_p = range(x22 + 4u, 4u);
        if (!mesh || !slot_p) break;
        const u32 mat_at = word(mesh + 8u);
        if ((mat_at & 3u) || !range(mat_at, 16u)) break;
        const u8 *mat_p = g_ram + mat_at;

        u32 pipe = pipe_override;
        if (pipe == 0u) {
            pipe = word(mat_p + 8u);
            if (pipe == 0u) pipe = def_pipe;
        }
        u32 next_x19 = x19;
        if (pipe != x30) {
            if ((pipe & 3u) || !range(pipe, 12u)) break;
            const u32 nodes_at = word(g_ram + pipe + 8u);
            if ((nodes_at & 3u) || !range(nodes_at, 24u)) break;
            next_x19 = word(g_ram + nodes_at + 20u);
        }
        if ((next_x19 & 3u) || !range(next_x19, 20u)) break;
        const u32 mat_setup_cb = word(g_ram + next_x19);
        const u32 bridge_cb = word(g_ram + next_x19 + 12u);
        const u32 render_cb = word(g_ram + next_x19 + 16u);
        if (render_cb != 0u) break;

        // Pre-check bridge support (004A0E00, 00472130, or 0046EA80 with idx==0).
        int bridge_kind = 0;
        if (bridge_cb == 0x004A0E00u) bridge_kind = 1;
        else if (bridge_cb == 0x00472130u) bridge_kind = 2;
        else if (bridge_cb == 0x0046EA80u) {
            const u32 mfx_off = word(g_ram + gp - 17848u);
            const u8 *ep = range(mat_at + mfx_off, 4u);
            if (!ep) break;
            const u32 ea = word(ep);
            if (ea != 0u) {
                const u8 *e = range(ea + 128u, 4u);
                if (!e || word(e) != 0u) break;
            }
            bridge_kind = 3;
        } else {
            break;
        }

        // Pre-check cache/dirty status before mutating stack or LRU.
        u32 cache_at = 0u;
        u32 new_mflags = base_flags;
        u32 new_gflags = 0u;
        bool write_gflags = false;
        if (base_flags != 1u) {
            new_mflags |= 1u;
            cache_at = word(slot_p);
            if (cache_at == 0u || (cache_at & 3u) || !range(cache_at, 48u)) break;
            if (mat_setup_cb != 0u) {
                if (mat_setup_cb != 0x004A0C20u) break;
                const u32 p12_at = word(frame + 124u);   // sp + 284
                if ((p12_at & 3u) || !range(p12_at, 4u)) break;
                const u32 gflags = word(g_ram + p12_at);
                u32 count = word(mesh + 4u);
                if (gflags & 2u) count = (count - 2u) * 3u;
                if (count != word(g_ram + cache_at + 36u)) new_mflags |= 8u;
                if ((new_mflags & 8u) == 0u) {
                    new_gflags = gflags;
                    write_gflags = true;
                    if (gflags != word(g_ram + cache_at + 44u)) new_mflags |= 8u;
                }
            }
            if (new_mflags & 14u) break;
        }

        // Temporarily check bridge preconditions with the exact fields rw_mat_dma / skin_bone_dma will read.
        // We write the stack struct (sp+276, sp+315..343) only after validating pointers above; if bridge_can_run fails
        // we restore nothing because stack fields sp+315..343 are dead on entry to L_0049F7AC!
        const u32 prev_x19 = x19;
        const u32 prev_x30 = x30;
        const u32 prev_sp276 = word(frame + 116u);
        if (pipe != x30) {
            x30 = pipe;
            x19 = next_x19;
            std::memcpy(frame + 116u, &x19, 4u);
        }
        const u32 slot_addr = x22 + 4u;
        const u32 surf_at = mat_at + 12u;
        const u32 tex_at = word(mat_p);
        std::memcpy(frame + 156u, &x18, 4u);        // sp + 316
        std::memcpy(frame + 160u, &slot_addr, 4u);  // sp + 320
        std::memcpy(frame + 164u, &new_mflags, 4u); // sp + 324
        const u32 zero = 0u;
        std::memcpy(frame + 168u, &zero, 4u);       // sp + 328
        if (write_gflags) std::memcpy(frame + 168u, &new_gflags, 4u);
        frame[155u] = b312;                          // sp + 315
        std::memcpy(frame + 172u, &surf_at, 4u);    // sp + 332
        std::memcpy(frame + 176u, &tex_at, 4u);     // sp + 336
        if (use_mat_col != 0u) {
            std::memcpy(frame + 180u, mat_p + 4u, 4u);
        } else {
            std::memcpy(frame + 180u, g_ram + gp - 20712u, 4u);
        }

        const bool has_skin = (bridge_kind == 2) && (word(g_ram + 0x0095DE38u) != 0u);
        u32 extra_qwc = 0u;
        if ((has_skin && !skin_bone_dma_can_run(mat_arg_at, extra_qwc)) || !rw_mat_dma_can_run(mat_arg_at, extra_qwc)) {
            x19 = prev_x19;
            x30 = prev_x30;
            std::memcpy(frame + 116u, &prev_sp276, 4u);
            break;
        }

        // LRU move-to-front (L_0049F8C8).
        if (base_flags != 1u) {
            const u32 next_e = word(g_ram + cache_at);
            if (next_e != 0u) {
                const u32 prev_e = word(g_ram + cache_at + 4u);
                const u32 first_e = word(g_ram + lru_head_at);
                if (!((prev_e | next_e | first_e) & 3u) && range(prev_e, 4u) && range(next_e, 8u) && range(first_e, 8u)) {
                    std::memcpy(g_ram + prev_e, &next_e, 4u);
                    std::memcpy(g_ram + next_e + 4u, &prev_e, 4u);
                    std::memcpy(g_ram + cache_at, &first_e, 4u);
                    std::memcpy(g_ram + cache_at + 4u, &lru_head_at, 4u);
                    std::memcpy(g_ram + first_e + 4u, &cache_at, 4u);
                    std::memcpy(g_ram + lru_head_at, &cache_at, 4u);
                    spent += 35;
                }
            }
            if (mat_setup_cb != 0u) spent += 28;
        }

        std::int32_t iter_spent = 85;
        if (has_skin) {
            if (!skin_bone_dma_impl(mat_arg_at, iter_spent)) break;
            iter_spent += 15;
        } else if (bridge_kind == 3) {
            iter_spent += 28;
        }
        if (!rw_mat_dma_impl(c, mat_arg_at, iter_spent)) break;

        spent += iter_spent;
        x18 += 12u;
        x22 += 4u;
        ++idx;
        ++done;
    }

    if (done > 0u) {
        set32(c.r[18], x18);
        set32(c.r[19], x19);
        set32(c.r[21], idx);
        set32(c.r[22], x22);
        set32(c.r[30], x30);
        c.budget -= spent;
    }
}

// RenderWare RwRenderStateSet (0x004D6B90): fast switch for the common PS2 GS render states; falls back for jump-table cases (9,10,11,29).
bool native_rw_render_state_set(Cpu &c) {
    constexpr u32 gp = 0x007D2CF0u;
    if (c.r[28].w[0] != gp) return false;
    const std::uint64_t state = c.r[4].d[0];
    const std::uint64_t val = c.r[5].d[0];
    auto emit_ad1 = [&](std::uint64_t reg_val, std::uint64_t reg_addr, std::int32_t &spent) -> bool {
        if (!dma_packet_alloc_impl(0x80000000u, 2u, spent)) return false;
        const u32 spr = word(g_ram + gp - 17148u);
        u8 *dst = range(spr & ~15u, 32u);
        if (!dst) return false;
        const std::uint64_t q0_lo = 32769ull | (0x10000000ull << 32);
        const std::uint64_t q0_hi = 14ull;
        std::memcpy(dst, &q0_lo, 8u);
        std::memcpy(dst + 8u, &q0_hi, 8u);
        std::memcpy(dst + 16u, &reg_val, 8u);
        std::memcpy(dst + 24u, &reg_addr, 8u);
        const u32 next_spr = spr + 32u;
        std::memcpy(g_ram + gp - 17148u, &next_spr, 4u);
        return true;
    };

    std::int32_t spent = 0;
    switch (state) {
    case 1ull: {   // rwRENDERSTATETEXTURERASTER
        const u32 ras_at = static_cast<u32>(val);
        const u8 *ras = nullptr;
        if (val != 0ull) {
            ras = range(ras_at, 36u);
            if (!ras || rw_tex_needs_upload(ras_at)) return false;
        }
        if (!dma_packet_can_alloc(0x80000000u, 10u)) return false;
        std::memcpy(g_ram + gp - 16948u, &ras_at, 4u);
        u32 x16 = 1u, x2 = 0u;
        std::uint64_t g_prim;
        std::memcpy(&g_prim, g_ram + gp - 20272u, 8u);
        if (val == 0ull) {
            g_prim &= ~16ull;
            std::memcpy(g_ram + gp - 20272u, &g_prim, 8u);
            const u32 z = 0u;
            std::memcpy(g_ram + gp - 16952u, &z, 4u);
        } else {
            if (!rw_tex_check_impl(ras_at, spent)) return false;
            u32 ret = 0u;
            if (!rw_tex_dma_impl(word(g_ram + gp - 16948u), 0ull, ret, spent)) return false;
            if (ret == 0u) {
                g_prim &= ~16ull;
                std::memcpy(g_ram + gp - 20272u, &g_prim, 8u);
                const u32 z = 0u;
                std::memcpy(g_ram + gp - 16952u, &z, 4u);
                std::memcpy(g_ram + gp - 16948u, &z, 4u);
                x16 = 0u;
                x2 = 0u;
            } else {
                g_prim |= 16ull;
                std::memcpy(g_ram + gp - 20272u, &g_prim, 8u);
                const u32 cur_ras = word(g_ram + gp - 16948u);
                const u8 *cr = range(cur_ras, 36u);
                if (!cr) return false;
                x2 = ((cr[35] & 15u) == 6u) ? 0u : 1u;
                std::memcpy(g_ram + gp - 16952u, &x2, 4u);
            }
        }
        if (word(g_ram + gp - 16956u) != 0u || x2 != 0u) g_prim |= 64ull;
        else g_prim &= ~64ull;
        std::memcpy(g_ram + gp - 20272u, &g_prim, 8u);
        std::uint64_t test1;
        std::memcpy(&test1, g_ram + gp - 17072u, 8u);
        if (x2 != 0u && !(test1 & 1ull)) {
            test1 |= 1ull;
            std::memcpy(g_ram + gp - 17072u, &test1, 8u);
            if (!emit_ad1(test1, 71ull, spent)) return false;
        } else if (x2 == 0u && (test1 & 1ull)) {
            test1 &= ~1ull;
            std::memcpy(g_ram + gp - 17072u, &test1, 8u);
            if (!emit_ad1(test1, 71ull, spent)) return false;
        }
        set32(c.r[2], x16);
        c.budget -= spent + 85;
        return true;
    }
    case 2ull: {   // rwRENDERSTATETEXTUREADDRESS
        std::uint64_t clamp;
        if (val == 1ull) clamp = 0ull;
        else if (val == 3ull) clamp = 5ull;
        else { set32(c.r[2], 0u); c.budget -= 68; return true; }
        if (!dma_packet_can_alloc(0x80000000u, 2u)) return false;
        std::memcpy(g_ram + gp - 17048u, &clamp, 8u);
        if (!emit_ad1(clamp, 8ull, spent)) return false;
        set32(c.r[2], 1u);
        c.budget -= spent + 90;
        return true;
    }
    case 3ull: {   // rwRENDERSTATETEXTUREADDRESSU
        if (val != 1ull && val != 3ull) { set32(c.r[2], 0u); c.budget -= 68; return true; }
        if (!dma_packet_can_alloc(0x80000000u, 2u)) return false;
        std::uint64_t clamp;
        std::memcpy(&clamp, g_ram + gp - 17048u, 8u);
        if (val == 1ull) clamp &= ~3ull;
        else clamp |= 1ull;
        std::memcpy(g_ram + gp - 17048u, &clamp, 8u);
        if (!emit_ad1(clamp, 8ull, spent)) return false;
        set32(c.r[2], 1u);
        c.budget -= spent + 92;
        return true;
    }
    case 4ull: {   // rwRENDERSTATETEXTUREADDRESSV
        if (val != 1ull && val != 3ull) { set32(c.r[2], 0u); c.budget -= 68; return true; }
        if (!dma_packet_can_alloc(0x80000000u, 2u)) return false;
        std::uint64_t clamp;
        std::memcpy(&clamp, g_ram + gp - 17048u, 8u);
        if (val == 1ull) clamp &= ~12ull;
        else clamp |= 4ull;
        std::memcpy(g_ram + gp - 17048u, &clamp, 8u);
        if (!emit_ad1(clamp, 8ull, spent)) return false;
        set32(c.r[2], 1u);
        c.budget -= spent + 92;
        return true;
    }
    case 5ull: {   // rwRENDERSTATETEXTUREPERSPECTIVE
        if (val == 0ull) { set32(c.r[2], 0u); c.budget -= 62; return true; }
        std::uint64_t g_prim;
        std::memcpy(&g_prim, g_ram + gp - 20272u, 8u);
        g_prim &= ~256ull;
        std::memcpy(g_ram + gp - 20272u, &g_prim, 8u);
        set32(c.r[2], 1u);
        c.budget -= 66;
        return true;
    }
    case 6ull: {   // rwRENDERSTATEZTESTENABLE
        if (!dma_packet_can_alloc(0x80000000u, 2u)) return false;
        std::uint64_t test1;
        std::memcpy(&test1, g_ram + gp - 17072u, 8u);
        test1 = (test1 & ~0x70000ull) | (val ? 0x50000ull : 0x30000ull);
        std::memcpy(g_ram + gp - 17072u, &test1, 8u);
        if (!emit_ad1(test1, 71ull, spent)) return false;
        set32(c.r[2], 1u);
        c.budget -= spent + 82;
        return true;
    }
    case 7ull: {   // rwRENDERSTATESHADEMODE
        if (val != 1ull && val != 2ull) { set32(c.r[2], 0u); c.budget -= 56; return true; }
        std::uint64_t g_prim;
        std::memcpy(&g_prim, g_ram + gp - 20272u, 8u);
        if (val == 1ull) g_prim &= ~8ull;
        else g_prim |= 8ull;
        std::memcpy(g_ram + gp - 20272u, &g_prim, 8u);
        set32(c.r[2], 1u);
        c.budget -= 60;
        return true;
    }
    case 8ull: {   // rwRENDERSTATEZWRITEENABLE
        if (!dma_packet_can_alloc(0x80000000u, 3u)) return false;
        std::uint64_t zbuf, test1;
        std::memcpy(&zbuf, g_ram + gp - 17088u, 8u);
        std::memcpy(&test1, g_ram + gp - 17072u, 8u);
        if (val != 0ull) {
            zbuf &= ~0x100000000ull;
            if (!(test1 & 0x10000ull)) {
                test1 = (test1 & ~0x70000ull) | 0x30000ull;
                std::memcpy(g_ram + gp - 17072u, &test1, 8u);
            }
        } else {
            zbuf |= 0x100000000ull;
        }
        std::memcpy(g_ram + gp - 17088u, &zbuf, 8u);
        if (!dma_packet_alloc_impl(0x80000000u, 3u, spent)) return false;
        const u32 spr = word(g_ram + gp - 17148u);
        u8 *dst = range(spr & ~15u, 48u);
        if (!dst) return false;
        const std::uint64_t q0_lo = 32770ull | (0x10000000ull << 32);
        const std::uint64_t q0_hi = 14ull, r78 = 78ull, r71 = 71ull;
        std::memcpy(dst, &q0_lo, 8u);
        std::memcpy(dst + 8u, &q0_hi, 8u);
        std::memcpy(dst + 16u, &zbuf, 8u);
        std::memcpy(dst + 24u, &r78, 8u);
        std::memcpy(dst + 32u, &test1, 8u);
        std::memcpy(dst + 40u, &r71, 8u);
        const u32 next_spr = spr + 48u;
        std::memcpy(g_ram + gp - 17148u, &next_spr, 4u);
        set32(c.r[2], 1u);
        c.budget -= spent + 85;
        return true;
    }
    case 12ull: {   // rwRENDERSTATEVERTEXALPHAENABLE
        const u32 v = val ? 1u : 0u;
        std::memcpy(g_ram + gp - 16956u, &v, 4u);
        std::uint64_t g_prim;
        std::memcpy(&g_prim, g_ram + gp - 20272u, 8u);
        if (v || word(g_ram + gp - 16952u)) g_prim |= 64ull;
        else g_prim &= ~64ull;
        std::memcpy(g_ram + gp - 20272u, &g_prim, 8u);
        set32(c.r[2], 1u);
        c.budget -= 42;
        return true;
    }
    case 13ull:
        set32(c.r[2], 0u);
        c.budget -= 16;
        return true;
    case 14ull: {   // rwRENDERSTATEFOGENABLE
        std::uint64_t g_prim;
        std::memcpy(&g_prim, g_ram + gp - 20272u, 8u);
        if (val) {
            g_prim |= 32ull;
            g_ram[gp - 16892u] |= 1u;
        } else {
            g_prim &= ~32ull;
            g_ram[gp - 16892u] &= static_cast<u8>(~1u);
        }
        std::memcpy(g_ram + gp - 20272u, &g_prim, 8u);
        set32(c.r[2], 1u);
        c.budget -= 40;
        return true;
    }
    case 15ull: {   // rwRENDERSTATEFOGCOLOR
        if (!dma_packet_can_alloc(0x80000000u, 2u)) return false;
        const u32 v32 = static_cast<u32>(val);
        const std::uint64_t fc = ((v32 & 0x00FF0000u) >> 16) | (v32 & 0x0000FF00u) | ((v32 & 0x000000FFu) << 16);
        std::memcpy(g_ram + gp - 17056u, &fc, 8u);
        if (!emit_ad1(fc, 61ull, spent)) return false;
        set32(c.r[2], 1u);
        c.budget -= spent + 64;
        return true;
    }
    case 16ull: {   // rwRENDERSTATEFOGTYPE
        if (val != 1ull) { set32(c.r[2], 0u); c.budget -= 34; return true; }
        const u32 p = 0x00965610u;
        std::memcpy(g_ram + gp - 16924u, &p, 4u);
        set32(c.r[2], 1u);
        c.budget -= 38;
        return true;
    }
    case 20ull: {   // rwRENDERSTATECULLMODE
        if (val == 1ull) g_ram[gp - 16892u] &= static_cast<u8>(~32u);
        else g_ram[gp - 16892u] |= 32u;
        const u32 v32 = static_cast<u32>(val);
        std::memcpy(g_ram + gp - 16964u, &v32, 4u);
        set32(c.r[2], 1u);
        c.budget -= 34;
        return true;
    }
    case 30ull: {   // rwRENDERSTATEALPHATESTFUNCTIONREF
        if (!dma_packet_can_alloc(0x80000000u, 2u)) return false;
        const auto iv = static_cast<std::int32_t>(static_cast<u32>(val));
        const u32 aref = static_cast<u32>(static_cast<std::int32_t>(std::bit_cast<float>(0x3F008088u) * static_cast<float>(iv)));
        std::uint64_t test1;
        std::memcpy(&test1, g_ram + gp - 17072u, 8u);
        test1 = (test1 & ~4080ull) | static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(aref << 4)));
        std::memcpy(g_ram + gp - 17072u, &test1, 8u);
        if (!emit_ad1(test1, 71ull, spent)) return false;
        set32(c.r[2], 1u);
        c.budget -= spent + 54;
        return true;
    }
    case 9ull: {   // rwRENDERSTATETEXTUREFILTER
        static constexpr std::int16_t kFilterBits[7] = {-1, 0, 96, 128, 288, 192, 352};
        if (val >= 7ull || kFilterBits[val] < 0) { set32(c.r[2], 0u); c.budget -= 30; return true; }
        if (!dma_packet_can_alloc(0x80000000u, 2u)) return false;
        std::uint64_t tex1;
        std::memcpy(&tex1, g_ram + gp - 17040u, 8u);
        tex1 = (tex1 & ~480ull) | static_cast<std::uint64_t>(kFilterBits[val]);
        std::memcpy(g_ram + gp - 17040u, &tex1, 8u);
        if (!emit_ad1(tex1, 20ull, spent)) return false;
        set32(c.r[2], 1u);
        c.budget -= spent + 64;
        return true;
    }
    case 10ull: case 11ull: {   // rwRENDERSTATESRCBLEND / rwRENDERSTATEDESTBLEND
        static constexpr std::int8_t kBlendMap[9] = {-1, 0, 1, -1, -1, 2, 3, 4, 5};
        if (val >= 9ull || kBlendMap[val] < 0) { set32(c.r[2], 0u); c.budget -= 30; return true; }
        const u32 idx = static_cast<u32>(kBlendMap[val]);
        const u32 src_idx = state == 10ull ? idx : word(g_ram + gp - 20264u);
        const u32 dst_idx = state == 11ull ? idx : word(g_ram + gp - 20260u);
        if (src_idx >= 6u || dst_idx >= 6u) return false;
        const auto a = static_cast<std::int32_t>(word(g_ram + 0x0077EF80u + dst_idx * 24u + src_idx * 4u));
        if (a < 0) return false;
        if (!dma_packet_can_alloc(0x80000000u, 2u)) return false;
        std::memcpy(g_ram + gp - (state == 10ull ? 20264u : 20260u), &idx, 4u);
        std::uint64_t alpha64 = static_cast<std::uint64_t>(static_cast<u32>(a));
        if (src_idx == 1u && dst_idx == 1u) alpha64 |= 128ull << 32;
        std::memcpy(g_ram + gp - 17032u, &alpha64, 8u);
        if (!emit_ad1(alpha64, 66ull, spent)) return false;
        set32(c.r[2], 1u);
        c.budget -= spent + 72;
        return true;
    }
    case 29ull: {   // rwRENDERSTATEALPHATESTFUNCTION
        static constexpr std::int8_t kAlphaFn[9] = {-1, 1, 5, 9, 7, 13, 15, 11, 2};
        if (val >= 9ull || kAlphaFn[val] < 0) { set32(c.r[2], 0u); c.budget -= 30; return true; }
        if (!dma_packet_can_alloc(0x80000000u, 2u)) return false;
        std::uint64_t test1;
        std::memcpy(&test1, g_ram + gp - 17072u, 8u);
        test1 = (test1 & ~15ull) | static_cast<std::uint64_t>(kAlphaFn[val]);
        if (test1 & 1ull) test1 &= ~12288ull;
        std::memcpy(g_ram + gp - 17072u, &test1, 8u);
        if (!emit_ad1(test1, 71ull, spent)) return false;
        set32(c.r[2], 1u);
        c.budget -= spent + 64;
        return true;
    }
    default:
        set32(c.r[2], 0u);
        c.budget -= 66;
        return true;
    }
}

// RwMatrixScale (0x004C33D0): replace (0), preconcat (1), postconcat (2).
bool native_matrix_scale(Cpu &c) {
    const std::uint64_t op = c.r[6].d[0];
    if (op > 2ull) return false;
    const u32 m_at = c.r[4].w[0], s_at = c.r[5].w[0];
    u8 *mp = range(m_at, 64u);
    const u8 *sp = range(s_at, 12u);
    if (!mp || !sp) return false;
    float s[3];
    std::memcpy(s, sp, 12u);
    if (op == 0ull) {
        const u32 fl = word(mp + 12u) & 0xFFFDFFFCu;
        std::memset(mp, 0, 60u);
        std::memcpy(mp + 0u, &s[0], 4u);
        std::memcpy(mp + 12u, &fl, 4u);
        std::memcpy(mp + 20u, &s[1], 4u);
        std::memcpy(mp + 40u, &s[2], 4u);
        c.budget -= 45;
    } else if (op == 1ull) {
        float m[12];
        std::memcpy(m, mp, 48u);
        m[0] *= s[0]; m[1] *= s[0]; m[2] *= s[0];
        m[4] *= s[1]; m[5] *= s[1]; m[6] *= s[1];
        m[8] *= s[2]; m[9] *= s[2]; m[10] *= s[2];
        std::memcpy(mp, m, 48u);
        const u32 fl = word(mp + 12u) & 0xFFFDFFFCu;
        std::memcpy(mp + 12u, &fl, 4u);
        c.budget -= 52;
    } else {
        float m[16];
        std::memcpy(m, mp, 64u);
        m[0] *= s[0]; m[1] *= s[1]; m[2] *= s[2];
        m[4] *= s[0]; m[5] *= s[1]; m[6] *= s[2];
        m[8] *= s[0]; m[9] *= s[1]; m[10] *= s[2];
        m[12] *= s[0]; m[13] *= s[1]; m[14] *= s[2];
        std::memcpy(mp, m, 64u);
        const u32 fl = word(mp + 12u) & 0xFFFDFFFCu;
        std::memcpy(mp + 12u, &fl, 4u);
        c.budget -= 61;
    }
    set32(c.r[2], m_at);
    return true;
}

// 0x004CB270 (RwFrameUpdateObjects): marks frame and its root dirty and links root into 0x0096012C dirty list if needed.
static inline void rw_frame_update_objects_impl(u32 frame_at) {
    const u32 root_at = word(g_ram + frame_at + 160u);
    const u8 rfl = g_ram[root_at + 3u];
    if ((rfl & 3u) == 0u) {
        const u32 head = 0x0096012Cu;
        const u32 first = word(g_ram + head);
        const u32 node_at = root_at + 8u;
        std::memcpy(g_ram + node_at, &first, 4u);
        std::memcpy(g_ram + root_at + 12u, &head, 4u);
        std::memcpy(g_ram + first + 4u, &node_at, 4u);
        std::memcpy(g_ram + head, &node_at, 4u);
    }
    g_ram[root_at + 3u] = static_cast<u8>(rfl | 3u);
    g_ram[frame_at + 3u] = static_cast<u8>(g_ram[frame_at + 3u] | 12u);
}

bool native_rw_frame_update_objects(Cpu &c) {
    const u32 frame_at = c.r[4].w[0];
    if ((frame_at & 3u) || !range(frame_at, 164u)) return false;
    const u32 root_at = word(g_ram + frame_at + 160u);
    if ((root_at & 3u) || !range(root_at, 16u)) return false;
    const u8 rfl = g_ram[root_at + 3u];
    if ((rfl & 3u) == 0u) {
        const u32 first = word(g_ram + 0x0096012Cu);
        if ((first & 3u) || !range(first, 8u)) return false;
        c.budget -= 29;
    } else {
        c.budget -= 15;
    }
    rw_frame_update_objects_impl(frame_at);
    set32(c.r[2], frame_at);
    return true;
}

// 0x00466EF0 (RpHAnimHierarchyUpdateMatrices): walks bone hierarchy, converts quaternion+pos keyframes to matrices,
// multiplies by parent matrix stack, and updates attached RwFrames without per-bone call/spill overhead.
bool native_hanim_update_matrices(Cpu &c) {
    const u32 h_at = c.r[4].w[0];
    if ((h_at & 3u) || c.vu0.vf[0].w[3] != fp::kOne) return false;
    const u8 *h_p = range(h_at, 36u);
    if (!h_p) return false;
    const u32 flags = word(h_p);
    if (flags & 2u) return false;   // sub-hierarchy fallback path (0x00466730)

    const std::int32_t num_nodes = static_cast<std::int32_t>(word(h_p + 4u));
    const u32 out_mats_at = word(h_p + 8u);
    const u32 node_info_at = word(h_p + 16u);
    const u32 interp_at = word(h_p + 32u);
    if ((interp_at & 3u) || !range(interp_at, 76u)) return false;
    const u32 cb = word(g_ram + interp_at + 60u);
    const u32 kf_step = word(g_ram + interp_at + 36u);
    if (num_nodes > 0) {
        if ((cb != 0x00464760u && cb != 0x004F7470u) || num_nodes > 256) return false;
        if ((out_mats_at & 15u) || (node_info_at & 3u) || ((interp_at + 76u) & 3u) || (kf_step & 3u)) return false;
        if (!range(out_mats_at, static_cast<std::size_t>(num_nodes) * 64u)) return false;
        if (!range(node_info_at, static_cast<std::size_t>(num_nodes) * 16u)) return false;
        if (!range(interp_at + 76u, static_cast<std::size_t>(num_nodes - 1) * kf_step + 36u)) return false;
    }

    const bool update_ltm = (flags & 8192u) != 0u;
    const bool update_mod = (flags & 4096u) != 0u;
    const bool local_space = (flags & 16384u) != 0u;

    // Pre-validate root frame if update_ltm will touch it at L_004670D0.
    u32 h_rf_at = 0u, h_rf_root = 0u;
    if (update_ltm) {
        h_rf_at = word(h_p + 20u);
        if ((h_rf_at & 3u) || !range(h_rf_at, 164u)) return false;
        h_rf_root = word(g_ram + h_rf_at + 160u);
        if ((h_rf_root & 3u) || !range(h_rf_root, 16u)) return false;
        if ((g_ram[h_rf_root + 3u] & 3u) == 0u) {
            const u32 first = word(g_ram + 0x0096012Cu);
            if ((first & 3u) || !range(first, 8u)) return false;
        }
    }

    // Resolve initial parent matrix and root LTM (matches L_00466F40..L_004670D0).
    alignas(16) u8 root_mat[64];
    alignas(16) u8 ident_mat[64];
    auto make_ident = [](u8 *m) {
        std::memset(m, 0, 64u);
        const u32 one = 0x3F800000u, fl = 0x00020003u;
        std::memcpy(m + 0u, &one, 4u);
        std::memcpy(m + 12u, &fl, 4u);
        std::memcpy(m + 20u, &one, 4u);
        std::memcpy(m + 40u, &one, 4u);
    };

    const u32 vf10_w = c.vu0.vf[10].w[3], vf11_w = c.vu0.vf[11].w[3], vf12_w = c.vu0.vf[12].w[3];
    auto mat_mul = [vf10_w, vf11_w, vf12_w](u8 *to_b, const u8 *a_b, const u8 *b_b) {
        const auto *a = reinterpret_cast<const Float *>(a_b);
        const auto *b = reinterpret_cast<const Float *>(b_b);
        auto *r = reinterpret_cast<Float *>(to_b);
        product_row<3, false>(r, a, b);
        product_row<3, false>(r + 4, a + 4, b);
        product_row<3, false>(r + 8, a + 8, b);
        product_row<3, true>(r + 12, a + 12, b);
        const u32 fl = word(a_b + 12u) & word(b_b + 12u);
        std::memcpy(to_b + 12u, &fl, 4u);
        std::memcpy(to_b + 28u, &vf10_w, 4u);
        std::memcpy(to_b + 44u, &vf11_w, 4u);
        std::memcpy(to_b + 60u, &vf12_w, 4u);
    };

    bool need_root_ltm = false;
    u32 ltm_frame = 0u;
    const u8 *parent_mat = nullptr;

    if ((flags & 1u) && word(h_p + 28u) != 0xFFFFFFFFu) {
        const u32 p_idx = word(h_p + 28u);
        const u32 p_h_at = word(h_p + 24u);
        if ((p_h_at & 3u) || !range(p_h_at, 24u)) return false;
        const u32 pm_at = word(g_ram + p_h_at + 8u) + (p_idx << 6);
        if ((pm_at & 15u) || !range(pm_at, 64u)) return false;
        parent_mat = g_ram + pm_at;
        if (update_ltm && local_space) {
            ltm_frame = word(g_ram + p_h_at + 20u);
            need_root_ltm = true;
        }
    } else if (local_space) {
        make_ident(ident_mat);
        parent_mat = ident_mat;
        if (update_ltm) {
            ltm_frame = word(h_p + 20u);
            need_root_ltm = true;
        }
    } else {
        ltm_frame = word(h_p + 20u);
        need_root_ltm = true;
        parent_mat = root_mat;
    }

    std::int32_t spent = 45;
    if (need_root_ltm) {
        u32 par = 0u;
        if (ltm_frame != 0u) {
            if ((ltm_frame & 3u) || !range(ltm_frame, 8u)) return false;
            par = word(g_ram + ltm_frame + 4u);
        }
        if (par == 0u) {
            make_ident(root_mat);
            spent += 25;
        } else {
            if ((par & 3u) || !range(par, 164u)) return false;
            const u32 par_root = word(g_ram + par + 160u);
            if ((par_root & 3u) || !range(par_root, 4u)) return false;
            if ((g_ram[par_root + 3u] & 3u) == 0u) {
                std::memcpy(root_mat, g_ram + par + 80u, 64u);
                spent += 75;
            } else {
                std::memcpy(root_mat, g_ram + par + 16u, 64u);
                spent += 70;
                u32 f = word(g_ram + par + 4u);
                int depth = 0;
                while (f != 0u) {
                    if ((f & 3u) || !range(f, 80u) || ++depth > 64) return false;
                    alignas(16) u8 tmp[64];
                    std::memcpy(tmp, root_mat, 64u);
                    mat_mul(root_mat, tmp, g_ram + f + 16u);
                    f = word(g_ram + f + 4u);
                    spent += 95;
                }
            }
        }
    }

    // Pre-validate attached node frames before any writes to guest memory.
    if (num_nodes > 0 && (update_mod || update_ltm)) {
        for (std::int32_t i = 0; i < num_nodes; ++i) {
            const u32 pf = word(g_ram + node_info_at + static_cast<u32>(i) * 16u + 12u);
            if (pf != 0u) {
                if ((pf & 3u) || !range(pf, 164u)) return false;
                if (update_mod && !update_ltm) {
                    const u32 pr = word(g_ram + pf + 160u);
                    if ((pr & 3u) || !range(pr, 16u)) return false;
                    if ((g_ram[pr + 3u] & 3u) == 0u) {
                        const u32 first = word(g_ram + 0x0096012Cu);
                        if ((first & 3u) || !range(first, 8u)) return false;
                    }
                }
            }
        }
    }

    if (update_ltm) {
        const u8 rfl = g_ram[h_rf_root + 3u];
        if ((rfl & 3u) == 0u) {
            const u32 head = 0x0096012Cu;
            const u32 first = word(g_ram + head);
            const u32 node_at = h_rf_root + 8u;
            std::memcpy(g_ram + node_at, &first, 4u);
            std::memcpy(g_ram + h_rf_root + 12u, &head, 4u);
            std::memcpy(g_ram + first + 4u, &node_at, 4u);
            std::memcpy(g_ram + head, &node_at, 4u);
            g_ram[h_rf_root + 3u] = static_cast<u8>(rfl | 2u);
            spent += 25;
        }
    }

    const u8 *mat_stack[64];
    int sp_idx = 0;
    alignas(16) u8 local_mat[64];
    std::memset(local_mat, 0, 64u);
    const u32 three = 3u;
    std::memcpy(local_mat + 12u, &three, 4u);

    const u8 *kf = g_ram + interp_at + 76u;
    u8 *out_mat = g_ram + out_mats_at;
    const u8 *node = g_ram + node_info_at;

    for (std::int32_t i = 0; i < num_nodes; ++i, kf += kf_step, out_mat += 64u, node += 16u) {
        const auto *kf_f = reinterpret_cast<const Float *>(kf);
        auto *lm = reinterpret_cast<Float *>(local_mat);
        const float x = kf_f[2], y = kf_f[3], z = kf_f[4], w = kf_f[5];
        const float xx = x * x, yy = y * y, zz = z * z;
        const float yz = y * z, zx = z * x, xy = x * y;
        const float wz = w * z, wx = w * x, wy = w * y;
        lm[0] = 1.0f - 2.0f * (yy + zz);
        lm[1] = 2.0f * (xy + wz);
        lm[2] = 2.0f * (zx - wy);
        lm[4] = 2.0f * (xy - wz);
        lm[5] = 1.0f - 2.0f * (xx + zz);
        lm[6] = 2.0f * (yz + wx);
        lm[8] = 2.0f * (zx + wy);
        lm[9] = 2.0f * (yz - wx);
        lm[10] = 1.0f - 2.0f * (xx + yy);
        lm[12] = kf_f[6];
        lm[13] = kf_f[7];
        lm[14] = kf_f[8];

        mat_mul(out_mat, local_mat, parent_mat);

        const u32 pf = word(node + 12u);
        if (pf != 0u) {
            if (update_mod) {
                std::memcpy(g_ram + pf + 16u, local_mat, 64u);
                if (!update_ltm) rw_frame_update_objects_impl(pf);
            }
            if (update_ltm) {
                if (!local_space) std::memcpy(g_ram + pf + 80u, out_mat, 64u);
                else mat_mul(g_ram + pf + 80u, out_mat, root_mat);
                g_ram[pf + 3u] = static_cast<u8>((g_ram[pf + 3u] & ~4u) | 8u);
            }
        }

        const u32 nf = word(node + 8u) & 3u;
        if (nf == 2u) {
            if (sp_idx < 64) mat_stack[sp_idx++] = parent_mat;
            parent_mat = out_mat;
        } else if (nf == 1u) {
            if (sp_idx > 0) parent_mat = mat_stack[--sp_idx];
        } else if (nf == 0u) {
            parent_mat = out_mat;
        }
    }

    spent += num_nodes * 135 + 15;
    set32(c.r[2], 1u);
    c.budget -= spent;
    return true;
}

// Inner triangle-strip copy loop of 0x004ED570 (L_004ED6E8): copies 3 x 64-byte vertices per triangle into scratchpad.
void native_loop_im3d_copy_tri(Cpu &c) {
    constexpr u32 gp = 0x007D2CF0u;
    if (c.r[28].w[0] != gp) return;
    const u32 n = c.r[9].w[0];
    if (n == 0u || n > 64u) return;
    u32 x18 = c.r[18].w[0];
    if (x18 != 1u && x18 != 2u) return;
    u32 src_at = c.r[20].w[0];
    u32 spr_at = c.r[12].w[0];
    if (spr_at != word(g_ram + gp - 17148u) || ((src_at | spr_at) & 15u)) return;
    const u8 *src_base = range(src_at, (n + 2u) * 64u);
    u8 *dst = range(spr_at, n * 192u);
    if (!src_base || !dst) return;
    const u8 *p = src_base;
    for (u32 i = 0; i < n; ++i) {
        const u8 *v1 = p + (x18 << 6);
        x18 ^= 3u;
        const u8 *v2 = p + (x18 << 6);
        std::memcpy(dst, p, 64u);
        std::memcpy(dst + 64u, v1, 64u);
        std::memcpy(dst + 128u, v2, 64u);
        p += 64u;
        dst += 192u;
    }
    src_at += n * 64u;
    spr_at += n * 192u;
    std::memcpy(g_ram + gp - 17148u, &spr_at, 4u);
    set32(c.r[12], spr_at);
    set32(c.r[20], src_at);
    set32(c.r[18], x18);
    set32(c.r[9], 0u);
    c.budget -= static_cast<std::int32_t>(n) * 82;
}

// Keyframe advance + node search loop of RtAnimInterpolatorAddAnimTime (0x004B7240, L_004B7350).
void native_loop_anim_advance(Cpu &c) {
    const u32 anim_at = c.r[16].w[0];
    u32 pnext = c.r[4].w[0];
    const u32 end_at = c.r[6].w[0];
    const auto num_nodes = static_cast<std::int32_t>(c.r[17].w[0]);
    auto idx = static_cast<std::int32_t>(c.r[7].w[0]);
    if (num_nodes <= 0 || num_nodes > 512 || idx < 0 || idx >= num_nodes) return;
    if (pnext >= end_at) return;
    u8 *anim_hdr = range(anim_at, 76u);
    if (!anim_hdr) return;
    if (word(anim_hdr + 8u) != pnext) return;
    const u32 stride = word(anim_hdr + 36u);
    const u32 kf_size = word(anim_hdr + 40u);
    if (stride < 8u || stride > 256u || kf_size < 4u || kf_size > 256u) return;
    u8 *nodes = range(anim_at + 76u, static_cast<u32>(num_nodes) * stride);
    if (!nodes) return;
    float cur_time;
    std::memcpy(&cur_time, anim_hdr + 4u, 4u);
    std::int32_t spent = 0;
    u32 advanced = 0;
    while (pnext + kf_size < end_at && advanced < 1024u) {
        const u8 *kf = range(pnext, 4u);
        if (!kf) break;
        const u32 prev_at = word(kf);
        const u8 *prev = range(prev_at, 8u);
        if (!prev) break;
        float prev_time;
        std::memcpy(&prev_time, prev + 4u, 4u);
        if (!(prev_time <= cur_time)) break;
        std::int32_t found = -1;
        std::int32_t cur = idx;
        for (std::int32_t step = 0; step < num_nodes; ++step) {
            const u32 off = static_cast<u32>(cur) * stride;
            const std::int32_t next_cur = (cur + 1 < num_nodes) ? cur + 1 : 0;
            if (word(nodes + off + 4u) == prev_at) {
                found = cur;
                idx = next_cur;
                spent += (step + 1) * 10;
                break;
            }
            cur = next_cur;
        }
        if (found < 0) break;
        u8 *node = nodes + static_cast<u32>(found) * stride;
        std::memcpy(node, &prev_at, 4u);
        std::memcpy(node + 4u, &pnext, 4u);
        pnext += kf_size;
        std::memcpy(anim_hdr + 8u, &pnext, 4u);
        spent += 17;
        ++advanced;
    }
    if (advanced) {
        set32(c.r[4], pnext);
        set32(c.r[7], static_cast<u32>(idx));
        c.budget -= spent;
    }
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
