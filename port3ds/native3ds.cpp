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
inline u8 *range(u32 a, u32 n) {
    if ((a >> 29) == 1) a &= 0x0FFFFFFFu;   // the uncached views of main RAM
    if (a < kRamSize) return n <= kRamSize - a ? g_ram + a : nullptr;
    if ((a >> 14) == (kScratchpadBase >> 14)) {
        const u32 at = a & (kScratchpadSize - 1);
        return n <= kScratchpadSize - at ? g_spr + at : nullptr;
    }
    return nullptr;
}

// Fast main-RAM-only bounds check (no scratchpad/uncached branches, no out-of-line calls).
[[gnu::always_inline]] inline u8 *ram_ptr(u32 a, u32 n) {
    return (a < kRamSize && n <= kRamSize - a) ? g_ram + a : nullptr;
}

// Fast scratchpad-first bounds check for SPR DMA packet writes (falls back to range() for RAM).
[[gnu::always_inline]] inline u8 *spr_ptr(u32 a, u32 n) {
    if ((a >> 14) == (kScratchpadBase >> 14)) [[likely]] {
        const u32 at = a & (kScratchpadSize - 1);
        return n <= kScratchpadSize - at ? g_spr + at : nullptr;
    }
    return range(a, n);
}

[[gnu::always_inline]] inline u32 word(const u8 *p) {
    u32 v;
    std::memcpy(&v, p, 4);
    return v;
}

[[gnu::always_inline]] inline void put_w(u8 *p, u32 v) { std::memcpy(p, &v, 4); }

[[gnu::always_inline]] inline std::uint16_t half_u16(const u8 *p) {
    std::uint16_t v;
    std::memcpy(&v, p, 2);
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

bool native_memmove(Cpu &c) {
    const u32 to = c.r[4].w[0], from = c.r[5].w[0], n = c.r[6].w[0];
    if (!n) {
        c.r[2].sd[0] = static_cast<std::int32_t>(to);
        c.budget -= 14;
        return true;
    }
    u8 *d = range(to, n);
    const u8 *s = range(from, n);
    if (!d || !s) return false;
    std::memmove(d, s, n);
    c.r[2].sd[0] = static_cast<std::int32_t>(to);
    c.budget -= 18 + static_cast<std::int32_t>(n >> 2);
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
[[gnu::noinline, gnu::cold]] float divide_slow(Cpu &c, float a, float b) {
    const fp::Result r = fp::div(std::bit_cast<u32>(a), std::bit_cast<u32>(b));
    if (r.invalid) c.fcr31 |= 0x30000u;
    return std::bit_cast<float>(r.value);
}

[[gnu::always_inline]] inline float divide(Cpu &c, float a, float b) {
    if (fp::exponent(std::bit_cast<u32>(b))) [[likely]] return a / b;
    return divide_slow(c, a, b);
}

// Hardware single-precision sqrt for proven non-negative inputs (avoids libm errno check / bl sqrtf on ARM11).
[[gnu::always_inline]] inline float hw_sqrtf(float a) {
#ifdef __arm__
    float r;
    __asm__("vsqrt.f32 %0, %1" : "=t"(r) : "t"(a));
    return r;
#else
    return __builtin_sqrtf(a);
#endif
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

[[gnu::noinline]] bool slerp_weights(Cpu &c, float dot, float t, float &w0, float &w1, std::int32_t &spent) {
    const u32 bits = std::bit_cast<u32>(dot), magnitude = bits & fp::kAbs;
    if (static_cast<std::int32_t>(bits) < 0 || static_cast<std::int32_t>(magnitude) >= 0x3F800000) return false;
    float angle;
    spent += 8 + 4;
    if (static_cast<std::int32_t>(magnitude) >= 0x3F000000) {
        const float a = 0.5f * (1.0f - dot);
        const float s = hw_sqrtf(a);
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
    const float a0 = (1.0f - t) * angle, a1 = t * angle;
    w0 = sine(a0, a0 * a0) * over_sine;
    w1 = sine(a1, a1 * a1) * over_sine;
    spent += 68;
    return true;
}

[[gnu::always_inline]] inline bool pose_blend_one(Cpu &c, u8 *to, u32 from_at, u32 next_at, const Float *b,
                                                  float cur_time, std::int32_t &spent, bool &close_out) {
    if ((from_at | next_at) & 3u) return false;
    const u8 *from = ram_ptr(from_at, 22), *next = ram_ptr(next_at, 22);
    if (!from || !next) return false;

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
        if (!slerp_weights(c, dot, t, w0, w1, spent)) return false;
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

struct AnimUndo {
    u32 off, prev, next;
};

[[gnu::noinline, gnu::cold]] void rollback_anim_nodes(u8 *nodes, const AnimUndo *undo, int n_undo) {
    for (int k = n_undo - 1; k >= 0; --k) {
        put_w(nodes + undo[k].off, undo[k].prev);
        put_w(nodes + undo[k].off + 4u, undo[k].next);
    }
}

}   // namespace

// Blends two animation keys, once for each bone of each character in every game frame: rotations by spherical interpolation, with
// the arc cosine and three sines inlined by the game's compiler, positions in a straight line. It was the most expensive function
// in the lobby, about a tenth of the game's code. Same operations in the same order, except that a zero the game adds to a constant
// before a multiply and accumulate is left out. The registers the game's version changes are all caller-saved, so these stay as they are.
bool native_pose_blend(Cpu &c) {
    const u32 to_at = c.r[4].w[0], from_at = c.r[5].w[0], next_at = c.r[6].w[0], base_at = c.r[7].w[0];
    if ((to_at | base_at) & 3u) return false;
    if (to_at + 36u > base_at && base_at + 24u > to_at) return false;
    u8 *to = range(to_at, 36);
    const u8 *base = range(base_at, 24);
    if (!to || !base) return false;
    std::int32_t spent = 0;
    bool close = false;
    if (!pose_blend_one(c, to, from_at, next_at, reinterpret_cast<const Float *>(base), std::bit_cast<float>(c.f[12]), spent, close))
        return false;
    c.fcr31 = close ? (c.fcr31 & ~kFpuCondition) : (c.fcr31 | kFpuCondition);
    c.budget -= spent;
    return true;
}

// Full RtAnimInterpolatorAddAnimTime (0x004B7240): handles non-wrapping forward time step, keyframe advances, and
// interpolation of all num_nodes bones inline when interp_cb == 0x004F7560 and no sub-time/wrap callbacks trigger.
bool native_rt_anim_add_time(Cpu &c) {
    const u32 delta_bits = c.f[12];
    if (fp::key(delta_bits) <= fp::key(0u)) {
        set32(c.r[2], 1u);
        c.budget -= 25;
        return true;
    }
    const u32 interp_at = c.r[4].w[0];
    if (interp_at & 3u) return false;
    u8 *interp = ram_ptr(interp_at, 76u);
    if (!interp) return false;
    const u32 pnext_orig = word(interp + 8u);
    if (pnext_orig == 0u) return false;
    const u32 anim_at = word(interp);
    if (anim_at & 3u) return false;
    const u8 *anim = ram_ptr(anim_at, 24u);
    if (!anim) return false;

    const u32 cur_bits = word(interp + 4u);
    const u32 sub_bits = word(interp + 20u);
    const u32 dur_bits = word(anim + 12u);
    const u32 new_bits = fp::add(cur_bits, delta_bits);
    if (fp::key(new_bits) > fp::key(dur_bits)) return false;   // wraps past end -> needs 0x004B6EC0 reset + end callback
    const bool sub_hit = fp::key(cur_bits) < fp::key(sub_bits) && !(fp::key(new_bits) < fp::key(sub_bits));
    if (sub_hit && word(interp + 12u) != 0u) return false;     // needs sub-time callback

    const auto num_nodes = static_cast<std::int32_t>(word(interp + 44u));
    if (num_nodes < 0 || num_nodes > 512) return false;
    const u32 stride = word(interp + 36u);
    const u32 kf_size = word(interp + 40u);
    if (stride < 36u || stride > 256u || (stride & 3u) || kf_size < 4u || kf_size > 256u) return false;
    const u32 base_at = word(anim + 20u);
    const Float *base_f = nullptr;
    u8 *nodes = nullptr;
    if (num_nodes > 0) {
        if (word(interp + 68u) != 0x004F7560u) return false;
        if (base_at & 3u) return false;
        const u32 nodes_at = interp_at + 76u;
        const u32 nodes_bytes = static_cast<u32>(num_nodes) * stride;
        if (nodes_at + nodes_bytes > base_at && base_at + 24u > nodes_at) return false;
        const u8 *base = ram_ptr(base_at, 24u);
        nodes = ram_ptr(nodes_at, nodes_bytes);
        if (!base || !nodes) return false;
        base_f = reinterpret_cast<const Float *>(base);
    }

    const u32 num_frames = word(anim + 4u);
    const u32 frames_at = word(anim + 16u);
    const u32 end_at = frames_at + num_frames * kf_size;

    AnimUndo undo[16];
    int n_undo = 0;

    std::int32_t spent = 48;
    u32 pnext = pnext_orig;
    std::int32_t search_idx = 0;
    const std::int32_t new_key = fp::key(new_bits);
    while (pnext < end_at) {
        if (pnext & 3u) { rollback_anim_nodes(nodes, undo, n_undo); return false; }
        const u8 *kf = ram_ptr(pnext, 4u);
        if (!kf) { rollback_anim_nodes(nodes, undo, n_undo); return false; }
        const u32 prev_at = word(kf);
        if (prev_at & 3u) { rollback_anim_nodes(nodes, undo, n_undo); return false; }
        const u8 *prev = ram_ptr(prev_at, 8u);
        if (!prev) { rollback_anim_nodes(nodes, undo, n_undo); return false; }
        if (fp::key(word(prev + 4u)) > new_key) {
            spent += 6;
            break;
        }
        if (num_nodes == 0 || n_undo >= 16) { rollback_anim_nodes(nodes, undo, n_undo); return false; }
        std::int32_t found = -1;
        std::int32_t cur = search_idx;
        for (std::int32_t step = 0; step < num_nodes; ++step) {
            const u32 off = static_cast<u32>(cur) * stride;
            const std::int32_t next_cur = (cur + 1 < num_nodes) ? cur + 1 : 0;
            if (word(nodes + off + 4u) == prev_at) {
                found = cur;
                search_idx = next_cur;
                spent += (step + 1) * 10;
                break;
            }
            cur = next_cur;
        }
        if (found < 0) { rollback_anim_nodes(nodes, undo, n_undo); return false; }
        const u32 off = static_cast<u32>(found) * stride;
        undo[n_undo++] = AnimUndo{off, word(nodes + off), word(nodes + off + 4u)};
        put_w(nodes + off, prev_at);
        put_w(nodes + off + 4u, pnext);
        pnext += kf_size;
        spent += 25;
    }

    const float new_time = std::bit_cast<float>(new_bits);
    bool last_close = false;
    u8 *slot = nodes;
    for (std::int32_t i = 0; i < num_nodes; ++i, slot += stride) {
        std::int32_t iter_spent = 0;
        if (!pose_blend_one(c, slot, word(slot), word(slot + 4u), base_f, new_time, iter_spent, last_close)) {
            rollback_anim_nodes(nodes, undo, n_undo);
            return false;
        }
        spent += iter_spent + 13;
    }

    put_w(interp + 4u, new_bits);
    put_w(interp + 8u, pnext);
    if (num_nodes > 0) c.fcr31 = last_close ? (c.fcr31 & ~kFpuCondition) : (c.fcr31 | kFpuCondition);
    set32(c.r[2], 1u);
    c.budget -= spent + 22;
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
    const auto *base_f = reinterpret_cast<const Float *>(base);
    float cur_time;
    std::memcpy(&cur_time, interp + 4u, 4u);
    u32 cur_at = c.r[18].w[0];
    const std::int32_t run_count = num_nodes - idx - 1;
    std::int32_t spent = 0;
    std::int32_t done = 0;
    bool last_close = false;
    for (; done < run_count; ++done, cur_at += kf_step) {
        if (cur_at & 3u) break;
        if (cur_at + 36u > base_at && base_at + 24u > cur_at) break;
        u8 *slot = range(cur_at, 36u);
        if (!slot) break;
        std::int32_t iter_spent = 0;
        if (!pose_blend_one(c, slot, word(slot), word(slot + 4u), base_f, cur_time, iter_spent, last_close)) break;
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

namespace {

[[gnu::always_inline]] inline bool frame_get_ltm_impl(Cpu &c, u32 frame, u32 &ltm_at, std::int32_t &spent) {
    if (frame & 15u) return false;
    const u8 *fp = ram_ptr(frame, 164u);
    if (!fp) return false;
    const u32 root = word(fp + 160u);
    if (root & 15u) return false;
    u8 *rp = ram_ptr(root, 164u);
    if (!rp) return false;
    spent += 15;
    if (rp[3] & 1u) {
        if (c.vu0.vf[0].w[3] != fp::kOne) return false;
        if (!sync_hierarchy_root_impl(rp, spent)) return false;
        spent += 4;
    }
    ltm_at = frame + 80u;
    return true;
}

}   // namespace

// RenderWare RwFrameGetLTM (0x004CB2F0): syncs root hierarchy if bit 0 is set, returns frame + 80.
bool native_frame_get_ltm(Cpu &c) {
    u32 ltm_at = 0u;
    std::int32_t spent = 0;
    if (!frame_get_ltm_impl(c, c.r[4].w[0], ltm_at, spent)) return false;
    set32(c.r[2], ltm_at);
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

namespace {

inline bool matrix_invert_impl(u8 *dst, const u8 *src, std::int32_t &spent) {
    constexpr u32 gp = 0x007D2CF0u;
    const u32 off = word(g_ram + (gp - 17432u));
    const u8 *gp_flags = ram_ptr(0x00960070u + off + 4u, 4u);
    if (!gp_flags) return false;
    const u32 sflags = word(src + 12u);
    if ((word(gp_flags) & sflags & 0x00020000u) != 0u) {
        if (dst != src) std::memcpy(dst, src, 64u);
        spent += 80;
        return true;
    }
    if ((sflags & 3u) != 3u) return false;
    const auto *s = reinterpret_cast<const Float *>(src);
    const float r0 = s[0], r1 = s[1], r2 = s[2];
    const float u0 = s[4], u1 = s[5], u2 = s[6];
    const float a0 = s[8], a1 = s[9], a2 = s[10];
    const float p0 = s[12], p1 = s[13], p2 = s[14];
    auto *d = reinterpret_cast<Float *>(dst);
    d[0] = r0; d[1] = u0; d[2] = a0; put_w(dst + 12u, 3u);
    d[4] = r1; d[5] = u1; d[6] = a1;
    d[8] = r2; d[9] = u2; d[10] = a2;
    d[12] = -((p1 * r1 + p0 * r0) + p2 * r2);
    d[13] = -((p1 * u1 + p0 * u0) + p2 * u2);
    d[14] = -((p1 * a1 + p0 * a0) + p2 * a2);
    spent += 59;
    return true;
}

inline bool atomic_world_sphere_impl(Cpu &c, u32 atomic_at, u32 &sph_at, std::int32_t &spent) {
    if (atomic_at & 3u) return false;
    u8 *at = ram_ptr(atomic_at, 80u);
    if (!at || (word(at + 76u) & 2u)) return false;
    const u32 frame_at = word(at + 4u);
    if (frame_at & 15u) return false;
    const u8 *fp = ram_ptr(frame_at, 164u);
    if (!fp) return false;
    const u32 root_at = word(fp + 160u);
    if (root_at & 15u) return false;
    u8 *rp = ram_ptr(root_at, 164u);
    if (!rp) return false;
    const u32 rflags = rp[3];
    if (!(rflags & 3u) && !(at[3] & 1u)) {
        sph_at = atomic_at + 44u;
        spent += 28;
        return true;
    }
    spent += 45;
    if (rflags & 1u) {
        if (c.vu0.vf[0].w[3] != fp::kOne) return false;
        if (!sync_hierarchy_root_impl(rp, spent)) return false;
    }
    const auto *m = reinterpret_cast<const Float *>(fp + 80u);
    const auto *lc = reinterpret_cast<const Float *>(at + 28u);
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
        wc[3] = lc[3] * hw_sqrtf(mx);
        spent += 30;
    }
    std::memcpy(at + 44u, wc, 16u);
    at[3] &= static_cast<u8>(~1u);
    sph_at = atomic_at + 44u;
    return true;
}

}   // namespace

namespace {

[[gnu::noinline]] void mul_mat4x4(Float *t, const Float *a, const Float *b) {
    product_row<4, false>(t, a, b);
    product_row<4, false>(t + 4, a + 4, b);
    product_row<4, false>(t + 8, a + 8, b);
    product_row<4, true>(t + 12, a + 12, b);
}

}   // namespace

// Full _rpSkinMatrixUpdating (0x0046B510): computes the entire bone matrix palette (identity, local, world, or sub-hierarchy)
// including the final bone, eliminating outer prologue/epilogue and EE<->native bounces.
bool native_rp_skin_matrix_updating(Cpu &c) {
    constexpr u32 gp = 0x007D2CF0u;
    if (c.r[28].w[0] != gp || c.vu0.vf[0].w[3] != fp::kOne) return false;
    const u32 atomic_at = c.r[4].w[0], skin_at = c.r[5].w[0];
    if ((atomic_at | skin_at) & 3u) return false;
    const u32 h_off = word(g_ram + 0x0095DE14u);
    const u32 pal_at = word(g_ram + 0x0095DE1Cu);
    if (pal_at & 15u) return false;
    const u8 *h_slot = ram_ptr(atomic_at + h_off, 4u);
    const u8 *skin = ram_ptr(skin_at, 16u);
    if (!h_slot || !skin) return false;
    const u32 h_at = word(h_slot);
    if (h_at == 0u) {
        const u32 n = word(skin);
        if (n > 256u) return false;
        u8 *dst = n ? ram_ptr(pal_at, n * 64u) : nullptr;
        if (n && !dst) return false;
        for (u32 k = 0; k < n; ++k, dst += 64u) {
            auto *w = reinterpret_cast<u32 *>(dst);
            w[0] = 0x3F800000u; w[1] = 0u; w[2] = 0u; w[3] |= 0x00020003u;
            w[4] = 0u; w[5] = 0x3F800000u; w[6] = 0u;
            w[8] = 0u; w[9] = 0u; w[10] = 0x3F800000u;
            w[12] = 0u; w[13] = 0u; w[14] = 0u;
        }
        set32(c.r[2], pal_at);
        c.budget -= 35 + static_cast<std::int32_t>(n) * 19;
        return true;
    }
    if (h_at & 3u) return false;
    const u8 *h = ram_ptr(h_at, 20u);
    if (!h) return false;
    const u32 h_flags = word(h);
    const u32 n = word(h + 4u);
    const u32 inv_at = word(skin + 12u);
    if (n == 0u || n > 256u || (inv_at & 15u)) return false;
    const u32 bytes = n * 64u;
    const u8 *inv = ram_ptr(inv_at, bytes);
    u8 *dst = ram_ptr(pal_at, bytes);
    if (!inv || !dst) return false;

    if ((h_flags & 2u) == 0u) {
        const u32 pose_at = word(h + 8u);
        if (pose_at & 15u) return false;
        const u8 *pose = ram_ptr(pose_at, bytes);
        if (!pose) return false;
        if (h_flags & 0x4000u) {
            if (!hidden()) {
                for (u32 k = 0; k < n; ++k) {
                    mul_mat4x4(reinterpret_cast<Float *>(dst + k * 64u),
                               reinterpret_cast<const Float *>(inv + k * 64u),
                               reinterpret_cast<const Float *>(pose + k * 64u));
                }
            }
            set32(c.r[2], pal_at);
            c.budget -= 47 + static_cast<std::int32_t>(n) * 30;
            return true;
        }
        const u8 *atomic = ram_ptr(atomic_at, 8u);
        if (!atomic) return false;
        u32 ltm_at = 0u;
        std::int32_t spent = 58;
        if (!frame_get_ltm_impl(c, word(atomic + 4u), ltm_at, spent)) return false;
        alignas(16) u8 inv_mod[64];
        if (!matrix_invert_impl(inv_mod, g_ram + ltm_at, spent)) return false;
        if (!hidden()) {
            const auto *m = reinterpret_cast<const Float *>(inv_mod);
            alignas(16) float part[16];
            auto *p = reinterpret_cast<Float *>(part);
            for (u32 k = 0; k < n; ++k) {
                mul_mat4x4(p, reinterpret_cast<const Float *>(inv + k * 64u), reinterpret_cast<const Float *>(pose + k * 64u));
                mul_mat4x4(reinterpret_cast<Float *>(dst + k * 64u), p, m);
            }
        }
        set32(c.r[2], pal_at);
        c.budget -= spent + static_cast<std::int32_t>(n) * 43;
        return true;
    }

    // Sub-hierarchy path (h_flags & 2): pre-validate all bone frames & roots before mutating palette.
    const u8 *atomic = ram_ptr(atomic_at, 8u);
    const u32 tbl_at = word(h + 16u);
    if (!atomic || (tbl_at & 15u)) return false;
    const u8 *tbl = ram_ptr(tbl_at, n * 16u);
    if (!tbl) return false;
    for (u32 k = 0; k < n; ++k) {
        const u32 bf = word(tbl + k * 16u + 12u);
        if (bf & 15u) return false;
        const u8 *fp = ram_ptr(bf, 164u);
        if (!fp) return false;
        const u32 root = word(fp + 160u);
        if ((root & 15u) || !ram_ptr(root, 164u)) return false;
    }
    u32 ltm_at = 0u;
    std::int32_t spent = 49;
    if (!frame_get_ltm_impl(c, word(atomic + 4u), ltm_at, spent)) return false;
    alignas(16) u8 inv_mod[64];
    if (!matrix_invert_impl(inv_mod, g_ram + ltm_at, spent)) return false;
    const bool skip_math = hidden();
    const auto *m = reinterpret_cast<const Float *>(inv_mod);
    alignas(16) float part[16];
    auto *p = reinterpret_cast<Float *>(part);
    for (u32 k = 0; k < n; ++k) {
        const u32 bf = word(tbl + k * 16u + 12u);
        const u8 *fp = g_ram + bf;
        u8 *rp = g_ram + word(fp + 160u);
        if (rp[3] & 1u) {
            if (!sync_hierarchy_root_impl(rp, spent)) return false;
            spent += 4;
        }
        if (!skip_math) {
            mul_mat4x4(p, reinterpret_cast<const Float *>(inv + k * 64u), reinterpret_cast<const Float *>(fp + 80u));
            mul_mat4x4(reinterpret_cast<Float *>(dst + k * 64u), p, m);
        }
        spent += 78;
    }
    set32(c.r[2], pal_at);
    c.budget -= spent;
    return true;
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
    std::int32_t spent = 0;
    if (!matrix_invert_impl(dst, src, spent)) return false;
    set32(c.r[2], dst_at);
    c.budget -= spent;
    return true;
}

// RenderWare RpAtomicGetWorldBoundingSphere (0x004912B0): skips dirty-check and transform call overhead.
bool native_atomic_world_sphere(Cpu &c) {
    u32 sph_at = 0u;
    std::int32_t spent = 0;
    if (!atomic_world_sphere_impl(c, c.r[4].w[0], sph_at, spent)) return false;
    set32(c.r[2], sph_at);
    c.budget -= spent;
    return true;
}

// RenderWare scratchpad DMA packet allocator (0x004D59D0): checks room in the 8 KB scratchpad + MFIFO ring, flushes via ch8
// when needed, and manages DIRECT-mode header tags. Falls back to game code on ring full or non-SPR modes.
namespace {

[[gnu::always_inline]] inline bool dma_packet_can_alloc(u32 a0, u32 a1) {
    constexpr u32 gp = 0x007D2CF0u;
    if (a0 & 0x7FFFFFFFu) return false;
    const u32 mode = word(g_ram + (gp - 17212u));
    if ((mode & 0x7FFFFFFFu) != 4353u) return false;
    const bool need_hdr = ((~mode) & 0x80000000u & a0) != 0u;
    const u32 bytes = (a1 + (need_hdr ? 5u : 4u)) << 4;
    const u32 spr_base = word(g_ram + (gp - 17124u));
    const u32 spr_cur = word(g_ram + (gp - 17148u));
    const u32 ring_cur = word(g_ram + (gp - 17128u));
    const u32 ring_end = word(g_ram + (gp - 17180u));
    const u32 used = spr_cur - spr_base;
    const u32 projected = ring_cur + used + bytes;
    if ((projected | ring_end) >= 0x80000000u || projected >= ring_end) return false;
    const bool flush1 = (bytes + used) >= 8193u && spr_cur != 0u && used != 0u;
    if ((flush1 || need_hdr) && (ld32(0x1000D000u) & 256u)) return false;
    return true;
}

[[gnu::noinline]] bool dma_packet_alloc_slow(u32 a0, u32 a1, std::int32_t &spent) {
    constexpr u32 gp = 0x007D2CF0u;
    if (a0 & 0x7FFFFFFFu) return false;
    const u32 mode = word(g_ram + (gp - 17212u));
    if ((mode & 0x7FFFFFFFu) != 4353u) return false;
    const bool need_hdr = ((~mode) & 0x80000000u & a0) != 0u;
    const u32 bytes = (a1 + (need_hdr ? 5u : 4u)) << 4;
    u32 spr_base = word(g_ram + (gp - 17124u));
    u32 spr_cur = word(g_ram + (gp - 17148u));
    u32 ring_cur = word(g_ram + (gp - 17128u));
    const u32 ring_end = word(g_ram + (gp - 17180u));
    const u32 used = spr_cur - spr_base;
    const u32 projected = ring_cur + used + bytes;
    if ((projected | ring_end) >= 0x80000000u || projected >= ring_end) return false;

    const bool flush1 = (bytes + used) >= 8193u && spr_cur != 0u && used != 0u;
    if ((flush1 || need_hdr) && (ld32(0x1000D000u) & 256u)) return false;

    auto set_w = [](u32 a, u32 v) { std::memcpy(g_ram + a, &v, 4); };
    spent += 52;
    auto flush_spr = [&](u32 flush_bytes) {
        const u32 sb = word(g_ram + (gp - 17124u)), rc = word(g_ram + (gp - 17128u));
        st32(0x1000E010u, 256u);
        st32(0x1000D080u, sb);
        st32(0x1000D010u, rc);
        st32(0x1000D020u, flush_bytes >> 4);
        st32(0x1000D000u, 256u);
        const u32 p208 = word(g_ram + (gp - 17208u));
        if (p208 && (p208 & ~16383u) == 0x70000000u) set_w(gp - 17208u, (rc + (p208 - sb)) | 0x20000000u);
        set_w(gp - 17128u, rc + flush_bytes);
        const u32 next_sb = sb ^ 8192u;
        set_w(gp - 17124u, next_sb);
        set_w(gp - 17148u, next_sb);
        spent += 33;
    };

    if (flush1) {
        flush_spr(used);
        spr_base = word(g_ram + (gp - 17124u));
        spr_cur = word(g_ram + (gp - 17148u));
        ring_cur = word(g_ram + (gp - 17128u));
    }

    set_w(gp - 17184u, ring_cur + (spr_cur - spr_base) + bytes);
    const u32 mbit = mode & 0x80000000u;
    if (mbit != a0) {
        if ((mbit & ~a0) != 0u) {
            const u32 tag_at = word(g_ram + (gp - 17204u));
            const u32 qwc_m1 = (((ring_cur - tag_at) + (spr_cur - spr_base)) >> 4) - 1u;
            if (qwc_m1 == 0u) {
                ring_cur -= 16u;
                set_w(gp - 17128u, ring_cur);
            } else {
                Reg128 q{};
                q.w[0] = 0x10000000u | qwc_m1;
                q.w[3] = 0x50000000u | qwc_m1;
                st128((tag_at | 0x20000000u) & ~15u, q);
            }
            set_w(gp - 17204u, 0u);
            set_w(gp - 17212u, mode & 0x7FFFFFFFu);
            spent += 25;
        }
        const u32 cur_mode = word(g_ram + (gp - 17212u));
        if (((~cur_mode) & 0x80000000u & a0) != 0u) {
            if (spr_cur != 0u && spr_cur != spr_base) {
                flush_spr(spr_cur - spr_base);
                ring_cur = word(g_ram + (gp - 17128u));
            }
            set_w(gp - 17204u, ring_cur);
            set_w(gp - 17128u, ring_cur + 16u);
            set_w(gp - 17212u, cur_mode | 0x80000000u);
            spent += 18;
        }
    }
    return true;
}

[[gnu::always_inline]] inline bool dma_packet_alloc_impl(u32 a0, u32 a1, std::int32_t &spent) {
    constexpr u32 gp = 0x007D2CF0u;
    if (a0 == 0u) [[likely]] {
        if (word(g_ram + (gp - 17212u)) == 4353u) [[likely]] {
            const u32 bytes = (a1 + 4u) << 4;
            const u32 spr_base = word(g_ram + (gp - 17124u));
            const u32 spr_cur = word(g_ram + (gp - 17148u));
            const u32 ring_cur = word(g_ram + (gp - 17128u));
            const u32 ring_end = word(g_ram + (gp - 17180u));
            const u32 used = spr_cur - spr_base;
            const u32 projected = ring_cur + used + bytes;
            if ((projected | ring_end) < 0x80000000u && projected < ring_end && (bytes + used) < 8193u) [[likely]] {
                put_w(g_ram + (gp - 17184u), projected);
                spent += 52;
                return true;
            }
        }
    }
    return dma_packet_alloc_slow(a0, a1, spent);
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

struct SkinBonePlan {
    u32 x16, run_count, remap_at, mat_base_at, extra_qwc;
    const u8 *tbl_b;
};

[[gnu::noinline]] bool skin_bone_dma_prepare(u32 arg_at, SkinBonePlan &plan) {
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
    const u32 extra_qwc = (x16 << 2) + 3u;
    if (!dma_packet_can_alloc(0u, extra_qwc)) return false;
    plan = SkinBonePlan{x16, run_count, remap_at, mat_base_at, extra_qwc, tbl_b};
    return true;
}

[[gnu::noinline]] bool skin_bone_dma_emit(const SkinBonePlan &plan, std::int32_t &spent) {
    constexpr u32 gp = 0x007D2CF0u;
    const u32 x16 = plan.x16;
    const u32 x18 = x16 << 2, x20 = x18 + 1u;
    if (!dma_packet_alloc_impl(0u, x20 + 2u, spent)) return false;
    const u32 spr = word(g_ram + (gp - 17148u));
    const u32 packet_bytes = (2u + (x16 << 2)) * 16u;
    u8 *pkt = spr_ptr(spr & ~15u, packet_bytes);
    if (!pkt) return false;
    const u32 hdr0[4] = {0x10000000u | x20, 0u, 0u, 0u};
    const u32 hdr1[4] = {0u, 0u, 0x01000101u, 0x6C0002BCu | ((x18 & 255u) << 16)};
    std::memcpy(pkt, hdr0, 16u);
    std::memcpy(pkt + 16u, hdr1, 16u);
    u8 *dst_base = pkt + 32u;
    for (u32 r = 0; r < plan.run_count; ++r) {
        const u32 sb = plan.tbl_b[r * 2u], cnt = plan.tbl_b[r * 2u + 1u];
        for (u32 k = 0; k < cnt; ++k) {
            const u32 dst_slot = g_ram[plan.remap_at + sb + k];
            std::memcpy(dst_base + dst_slot * 64u, g_ram + plan.mat_base_at + (sb + k) * 64u, 64u);
            spent += 21;
        }
        spent += 11;
    }
    const u32 next_spr = spr + 32u + (x16 << 6);
    std::memcpy(g_ram + gp - 17148u, &next_spr, 4u);
    spent += 76;
    return true;
}

bool skin_bone_dma_impl(u32 arg_at, std::int32_t &spent) {
    SkinBonePlan plan;
    return skin_bone_dma_prepare(arg_at, plan) && skin_bone_dma_emit(plan, spent);
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

namespace {

inline bool rw_dma_matrix_packet_impl(const u8 *mat, const u8 *extra, u32 a2, std::int32_t &spent) {
    constexpr u32 gp = 0x007D2CF0u;
    const u32 x2 = mat ? 6u : 0u;
    const u32 x21 = mat ? 1008u : 1014u;
    const u32 x17 = x2 + 4u;
    const u32 x16 = x17 + 3u + a2;
    if (!dma_packet_alloc_impl(0u, x16 + 2u, spent)) return false;
    const u32 spr = word(g_ram + (gp - 17148u));
    const u32 total_bytes = (3u + x2 + 5u + a2) * 16u;
    u8 *dst = spr_ptr(spr & ~15u, total_bytes);
    if (!dst) return false;
    auto *d32 = reinterpret_cast<u32 *>(dst);
    d32[0] = 0x10000000u | x16; d32[1] = 0u; d32[2] = 0u; d32[3] = 0u;
    std::memcpy(dst + 16u, g_ram + 0x009659C0u, 16u);
    d32[8] = 0u; d32[9] = 0u; d32[10] = 0x01000404u; d32[11] = 0x6C000000u | (x17 << 16) | x21;
    u32 off = 48u;
    if (mat) {
        for (u32 r = 0; r < 4u; ++r) {
            auto *dw = reinterpret_cast<u32 *>(dst + off);
            const u32 w2 = word(mat + r * 16u + 8u);
            dw[0] = word(mat + r * 16u);
            dw[1] = word(mat + r * 16u + 4u);
            dw[2] = w2;
            dw[3] = w2;
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
    put_w(g_ram + (gp - 17148u), spr + off);
    spent += 80 + (mat ? 39 : 0) + static_cast<std::int32_t>(a2) * 7;
    return true;
}

}   // namespace

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
    std::int32_t spent = 0;
    if (!rw_dma_matrix_packet_impl(mat, extra, a2, spent)) return false;
    set32(c.r[2], 1u);
    c.budget -= spent;
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

// RenderWare IM3D single-triangle submitter (0x004ECAC0, addCopyVU1Triangle): sibling of 0x004ED570.
bool native_rw_im3d_triangle(Cpu &c) {
    constexpr u32 gp = 0x007D2CF0u;
    if (c.r[28].w[0] != gp) return false;
    if (word(g_ram + gp - 16868u) == 0u) return false;
    const u32 desc_at = c.r[4].w[0];
    const u32 v0_at = c.r[6].w[0], v1_at = c.r[7].w[0], v2_at = c.r[8].w[0];
    if ((desc_at & 3u) || ((v0_at | v1_at | v2_at) & 15u)) return false;
    const u8 *desc = range(desc_at, 12u);
    const u8 *v0 = range(v0_at, 64u), *v1 = range(v1_at, 64u), *v2 = range(v2_at, 64u);
    if (!desc || !v0 || !v1 || !v2 || word(desc) != 0x004EC100u) return false;
    const u32 prog = word(desc + 8u);
    const std::uint64_t prog64 = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(prog)));
    const bool same_prog = word(g_ram + gp - 16888u) == prog;
    const u32 qwc_need = 37u - (same_prog ? 1u : 0u);
    if (!dma_packet_can_alloc(0u, qwc_need)) return false;

    std::int32_t spent = 166;
    if (!rw_im3d_header_impl(3u, 12u, 1u, 5u, prog64, spent)) return false;

    u32 spr = word(g_ram + gp - 17148u);
    u8 *dst = range(spr & ~15u, 304u);
    if (!dst) return false;
    auto put_q = [](u8 *p, std::uint64_t lo, std::uint64_t hi) {
        std::memcpy(p, &lo, 8u);
        std::memcpy(p + 8u, &hi, 8u);
    };

    put_q(dst + 0u, 0x1000000Eull, 0x04000001ull | (0x01000404ull << 32));
    put_q(dst + 16u, 0xC000C00020000000ull, 0x7C0C8000ull << 32);
    std::memcpy(dst + 32u, v0, 64u);
    std::memcpy(dst + 96u, v1, 64u);
    std::memcpy(dst + 160u, v2, 64u);
    std::memcpy(dst + 224u, g_ram + 0x009658E0u, 16u);
    // Inline 0x004EC4C0 trailer (4 qwords = 64 bytes).
    std::memcpy(dst + 240u, g_ram + 0x00965880u, 16u);
    put_q(dst + 256u, 0x11000000ull, 0ull);
    std::memcpy(dst + 272u, g_ram + 0x00965870u, 16u);
    std::memcpy(dst + 288u, g_ram + 0x009658F0u, 16u);
    spr += 304u;
    std::memcpy(g_ram + gp - 17148u, &spr, 4u);
    c.budget -= spent;
    return true;
}

// RenderWare RpSkyRenderStateSet (0x004D7F60): exact PS2 Sky GS state setter (dither/ALPHA_1/TEST_1/fog/cull globals).
bool native_rp_sky_render_state_set(Cpu &c) {
    constexpr u32 gp = 0x007D2CF0u;
    if (c.r[28].w[0] != gp) return false;
    const std::uint64_t state = c.r[4].d[0];
    const std::uint64_t val = c.r[5].d[0];
    auto emit_ad1 = [&](std::uint64_t reg_val, std::uint64_t reg_addr, std::int32_t &spent) -> bool {
        if (!dma_packet_alloc_impl(0x80000000u, 2u, spent)) return false;
        const u32 spr = word(g_ram + gp - 17148u);
        u8 *dst = range(spr & ~15u, 32u);
        if (!dst) return false;
        const std::uint64_t q0_lo = 32769ull | (0x10000000ull << 32), q0_hi = 14ull;
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
    case 1ull: {   // dither / stipple (GS DIMX 0x44)
        if (word(g_ram + gp - 16936u) == 0u) {
            set32(c.r[2], 0u);
            c.budget -= 28;
            return true;
        }
        if (!dma_packet_can_alloc(0x80000000u, 2u)) return false;
        u32 shadow;
        std::uint64_t dimx;
        if (val == 0ull) {
            shadow = 0u;
            dimx = 0ull;
        } else if (val == 0x55555555ull) {
            shadow = 0x55555555u;
            dimx = 0x1212303012123030ull;
        } else {
            shadow = 1u;
            dimx = 0x6071243571603524ull;
        }
        if (!emit_ad1(dimx, 68ull, spent)) return false;
        std::memcpy(g_ram + gp - 16960u, &shadow, 4u);
        set32(c.r[2], 1u);
        c.budget -= spent + 48;
        return true;
    }
    case 2ull: {   // direct GS ALPHA_1 (0x42)
        if (!dma_packet_can_alloc(0x80000000u, 2u)) return false;
        const std::uint64_t alpha64 = (val & 0xFFull) | ((val & 0xFF000000ull) << 8);
        if (!emit_ad1(alpha64, 66ull, spent)) return false;
        std::memcpy(g_ram + gp - 17032u, &alpha64, 8u);
        set32(c.r[2], 1u);
        c.budget -= spent + 48;
        return true;
    }
    case 3ull: {   // direct GS TEST_1 low 16 bits (0x47)
        if (!dma_packet_can_alloc(0x80000000u, 2u)) return false;
        std::uint64_t test1;
        std::memcpy(&test1, g_ram + gp - 17072u, 8u);
        test1 = (test1 & 0xFFFFFFFFFFFF0000ull) | (val & 0xFFFFull);
        if (!emit_ad1(test1, 71ull, spent)) return false;
        std::memcpy(g_ram + gp - 17072u, &test1, 8u);
        set32(c.r[2], 1u);
        c.budget -= spent + 44;
        return true;
    }
    case 4ull: {
        const u32 p_at = static_cast<u32>(val);
        const u8 *p = range(p_at, 4u);
        if (!p) return false;
        const u32 z = 0u, v = word(p);
        std::memcpy(g_ram + gp - 16860u, &z, 4u);
        std::memcpy(g_ram + gp - 16864u, &v, 4u);
        set32(c.r[2], 1u);
        c.budget -= 19;
        return true;
    }
    case 5ull: {
        const u32 p_at = static_cast<u32>(val);
        const u8 *p = range(p_at, 4u);
        if (!p) return false;
        const auto v = static_cast<std::int32_t>(word(p));
        if (v <= 0 || v >= 8) {
            set32(c.r[2], 0u);
            c.budget -= 17;
            return true;
        }
        const u32 u = static_cast<u32>(v);
        std::memcpy(g_ram + gp - 20256u, &u, 4u);
        set32(c.r[2], 1u);
        c.budget -= 20;
        return true;
    }
    default:
        set32(c.r[2], 0u);
        c.budget -= 25;
        return true;
    }
}

// RenderWare RwCameraFrustumTestSphere (0x004C9DF0): 6-plane dot product returning 0 (outside), 1 (touching), or 2 (inside).
bool native_rw_camera_frustum_test_sphere(Cpu &c) {
    const u32 cam_at = c.r[4].w[0], sph_at = c.r[5].w[0];
    if ((cam_at | sph_at) & 3u) return false;
    const u8 *planes = range(cam_at + 148u, 120u);
    const u8 *sph_b = range(sph_at, 16u);
    if (!planes || !sph_b) return false;
    const auto *s = reinterpret_cast<const Float *>(sph_b);
    const float sx = s[0], sy = s[1], sz = s[2];
    const u32 sr_bits = word(sph_b + 12u);
    const std::int32_t sr_key = fp::key(sr_bits);
    const std::int32_t nsr_key = fp::key(sr_bits ^ fp::kSign);
    u32 res = 2u;
    std::int32_t spent = 11;
    for (u32 i = 0; i < 6u; ++i, planes += 20u) {
        const auto *p = reinterpret_cast<const Float *>(planes);
        const float d = ((sy * p[1] + sx * p[0]) + sz * p[2]) - p[3];
        const std::int32_t dk = fp::key(std::bit_cast<u32>(d));
        if (dk > sr_key) {
            set32(c.r[2], 0u);
            c.budget -= spent + 18;
            return true;
        }
        if (dk > nsr_key) {
            res = 1u;
            spent += 23;
        } else {
            spent += 22;
        }
    }
    set32(c.r[2], res);
    c.budget -= spent + 2;
    return true;
}

namespace {

[[gnu::always_inline]] inline void rw_prim_transtype_setup_impl(u8 *obj, u32 flags, u32 frustum_res, std::int32_t &spent) {
    constexpr u32 gp = 0x007D2CF0u;
    const u8 base = g_ram[gp - 16892u];
    u8 prim, tt;
    if (flags & 1u) {
        prim = 4u;
        tt = base;
        spent += 11;
    } else if ((flags & 255u) == 0u) {
        prim = 3u;
        tt = static_cast<u8>(base | 4u);
        spent += 15;
    } else if (flags & 2u) {
        prim = 3u;
        tt = static_cast<u8>(base | 4u);
        spent += 19;
    } else if (flags & 16u) {
        prim = 4u;
        tt = base;
        spent += 22;
    } else if (flags & 8u) {
        prim = 2u;
        tt = static_cast<u8>(base | 16u);
        spent += 26;
    } else {
        prim = 1u;
        tt = static_cast<u8>(base | 20u);
        spent += 25;
    }
    if (frustum_res != 2u) {
        tt = static_cast<u8>(tt | 2u);
        spent += 11;
    } else {
        spent += 7;
    }
    obj[41] = prim;
    obj[40] = tt;
}

}   // namespace

// RenderWare RpAtomicPS2AllPrimTypeTransTypeSetupFunc (0x004A05D0): maps mesh header flags + frustum result to prim/tt bytes.
bool native_rw_prim_transtype_setup(Cpu &c) {
    constexpr u32 gp = 0x007D2CF0u;
    if (c.r[28].w[0] != gp) return false;
    const u32 a0 = c.r[4].w[0];
    if (a0 & 3u) return false;
    u8 *obj = range(a0, 42u);
    if (!obj) return false;
    const u32 p12_at = word(obj + 12u);
    if (p12_at & 3u) return false;
    const u8 *p12 = range(p12_at, 4u);
    if (!p12) return false;
    std::int32_t spent = 0;
    rw_prim_transtype_setup_impl(obj, word(p12), c.r[5].d[0] == 2ull ? 2u : 1u, spent);
    c.budget -= spent;
    return true;
}

// H_Dbprt_Main (0x00104420) outer row loop at L_00104534: skips blank 53-byte rows of sGrid[40][53] at 0x007E1E30 using
// 32-bit word scans, stopping on any non-space byte or the last row (row 39) while preserving all render-state calls.
void native_loop_dbprt_grid(Cpu &c) {
    u32 row = c.r[17].w[0];
    if (row >= 39u) return;
    u32 done = 0u;
    const u8 *p = g_ram + 0x007E1E30u + row * 53u;
    while (row < 39u) {
        u32 diff = static_cast<u32>(p[52] ^ 0x20u);
        for (u32 off = 0u; off < 52u; off += 4u) diff |= word(p + off) ^ 0x20202020u;
        if (diff != 0u) break;
        p += 53u;
        ++row;
        ++done;
    }
    if (done) {
        set32(c.r[17], row);
        c.budget -= static_cast<std::int32_t>(done) * 502;
    }
}

// Byte-clearing loop at 0x004FA8B8 (memclr): clears a1 bytes at a0 to zero.
bool native_memclr(Cpu &c) {
    const std::uint64_t n64 = c.r[5].d[0];
    if (n64 == 0ull) {
        c.budget -= 4;
        return true;
    }
    if (n64 > kRamSize) return false;
    const u32 to = c.r[4].w[0], n = static_cast<u32>(n64);
    u8 *d = range(to, n);
    if (!d) return false;
    std::memset(d, 0, n);
    set32(c.r[2], 0xFFFFFFFFu);
    set32(c.r[4], to + n);
    c.budget -= 6 + static_cast<std::int32_t>(n) * 7;
    return true;
}

// strlen (0x00524388): returns length of NUL-terminated string at a0.
bool native_strlen(Cpu &c) {
    const u32 a = c.r[4].w[0];
    const u32 n = room(a);
    if (!n) return false;
    const u8 *p = range(a, n);
    const void *z = std::memchr(p, 0, n);
    if (!z) return false;
    const u32 len = static_cast<u32>(static_cast<const u8 *>(z) - p);
    set32(c.r[2], len);
    c.budget -= (a & 7u) ? (5 + static_cast<std::int32_t>(len + 1u) * 7)
              : (a & 15u) ? (49 + static_cast<std::int32_t>(len >> 3) * 7)
                          : (52 + static_cast<std::int32_t>(len >> 4) * 9);
    return true;
}

namespace {

struct RwMatPlan {
    u8 *mat;
    const u8 *tex;
    const u8 *ras;
    const u8 *col;
    const u8 *p4;
    u8 *x8;
    u32 tex_at, ras_at, x8_at, prog;
    u8 b40, idx, tex_check_spent;
};

[[gnu::noinline]] bool rw_mat_dma_prepare_parts(u8 *mat, u32 tex_at, u32 col_at, u32 p4_at, u32 x8_at,
                                                u32 extra_qwc, RwMatPlan &plan) {
    constexpr u32 gp = 0x007D2CF0u;
    const u8 *tex = nullptr;
    u32 ras_at = 0u;
    const u8 *ras = nullptr;
    u8 tex_check_spent = 0u;
    if (tex_at) {
        tex = ram_ptr(tex_at, 84u);
        if (!tex) return false;
        ras_at = word(tex);
        if (ras_at) {
            ras = ram_ptr(ras_at, 36u);
            if (!ras) return false;
            const u32 ext_off = word(g_ram + (gp - 16968u));
            const u8 *ext = ram_ptr(ras_at + ext_off, 92u);
            if (!ext) return false;
            if (ras_at == word(g_ram + 0x0077F224u)) {
                tex_check_spent = 12u;
            } else if (ras[32] & 0x80u) {
                tex_check_spent = 16u;
            } else if (word(ras + 4u) == 0u) {
                tex_check_spent = 19u;
            } else if (word(ext + 88u) != 0u) {
                tex_check_spent = 22u;
            } else {
                return false;
            }
            const u32 cache_at = word(ext + 88u);
            if (cache_at != 0u && word(g_ram + 0x0077F200u) != 0u && !ram_ptr(cache_at, 8u)) return false;
        }
    }
    const u8 *col = ram_ptr(col_at, 12u);
    const u8 *p4 = ram_ptr(p4_at, 28u);
    u8 *x8 = ram_ptr(x8_at, 36u);
    if (!col || !p4 || !x8) return false;
    const u8 b40 = mat[40];
    const u8 idx = static_cast<u8>(((b40 & 32u) >> 5) | ((b40 & 24u) >> 2));
    const u32 prog_tbl = word(p4 + 24u);
    const u8 *prog_p = ram_ptr(prog_tbl + static_cast<u32>(idx) * 4u, 4u);
    if (!prog_p) return false;
    const bool tex_change = (ras_at != 0u && ras_at != word(g_ram + (gp - 16948u)));
    if (tex_change && !dma_packet_can_alloc(0x80000000u, 26u)) return false;
    if (!dma_packet_can_alloc(0u, 26u + extra_qwc)) return false;
    if (!rw_dma_ring_can_push()) return false;
    plan = RwMatPlan{mat, tex, ras, col, p4, x8, tex_at, ras_at, x8_at, word(prog_p), b40, idx, tex_check_spent};
    return true;
}

bool rw_mat_dma_prepare(u32 mat_at, u32 extra_qwc, RwMatPlan &plan) {
    if (!mat_at) return false;
    u8 *mat = ram_ptr(mat_at, 72u);
    if (!mat) return false;
    const u8 *p48 = ram_ptr(word(mat + 48u), 4u);
    if (!p48) return false;
    return rw_mat_dma_prepare_parts(mat, word(mat + 64u), word(mat + 60u), word(mat + 4u), word(p48), extra_qwc, plan);
}

[[gnu::noinline]] bool rw_mat_dma_emit(Cpu &c, const RwMatPlan &plan, std::int32_t &spent) {
    constexpr u32 gp = 0x007D2CF0u;
    u8 *const mat = plan.mat;
    const u8 *const tex = plan.tex;
    const u8 *const ras = plan.ras;
    const u8 *const col = plan.col;
    const u8 *const p4 = plan.p4;
    u8 *const x8 = plan.x8;
    const u32 tex_at = plan.tex_at, ras_at = plan.ras_at, x8_at = plan.x8_at;
    const u8 b40 = plan.b40, idx = plan.idx;

    spent += plan.tex_check_spent;

    u32 has_tex_alpha = 0u;
    if (ras_at) {
        const u32 fmt = ras[35] & 15u;
        has_tex_alpha = (fmt == 1u || fmt == 5u) ? 1u : 0u;
    }
    put_w(g_ram + (gp - 16952u), has_tex_alpha);

    if (ras_at != word(g_ram + (gp - 16948u))) {
        std::uint64_t m;
        std::memcpy(&m, g_ram + (gp - 20272u), 8u);
        if (ras_at == 0u) {
            m &= ~16ull;
            std::memcpy(g_ram + (gp - 20272u), &m, 8u);
        } else {
            m |= 16ull;
            std::memcpy(g_ram + (gp - 20272u), &m, 8u);
            u32 ret = 0u;
            if (!rw_tex_dma_impl(ras_at, 0ull, ret, spent)) return false;
        }
        put_w(g_ram + (gp - 16948u), ras_at);
        if (tex_at) {
            const u32 w80 = word(tex + 80u);
            rw_tex_filter_impl(static_cast<std::uint64_t>(w80 & 0xFFu));
            std::uint64_t clamp = 0ull;
            if (((w80 >> 8) & 0xFu) == 3u) clamp |= 1ull;
            if (((w80 >> 12) & 0xFu) == 3u) clamp |= 4ull;
            std::memcpy(g_ram + (gp - 17048u), &clamp, 8u);
            spent += 28;
        }
    }

    std::uint64_t test1;
    std::memcpy(&test1, g_ram + (gp - 17072u), 8u);
    if (has_tex_alpha) test1 |= 1ull;
    else test1 &= ~1ull;
    std::memcpy(g_ram + (gp - 17072u), &test1, 8u);

    if (!dma_packet_alloc_impl(0u, 18u, spent)) return false;
    const u32 spr = word(g_ram + (gp - 17148u));
    u8 *dst = spr_ptr(spr & ~15u, 288u);
    if (!dst) return false;
    auto *d32 = reinterpret_cast<u32 *>(dst);
    auto *df = reinterpret_cast<Float *>(dst);
    auto put_q4 = [](u32 *p, u32 w0, u32 w1, u32 w2, u32 w3) {
        p[0] = w0;
        p[1] = w1;
        p[2] = w2;
        p[3] = w3;
    };
    auto put_q2 = [](u32 *p, std::uint64_t lo, std::uint64_t hi) {
        p[0] = static_cast<u32>(lo);
        p[1] = static_cast<u32>(lo >> 32);
        p[2] = static_cast<u32>(hi);
        p[3] = static_cast<u32>(hi >> 32);
    };

    put_q4(d32 + 0u, 0x1000000Fu, 0u, 0u, 0u);
    put_q4(d32 + 4u, 0u, 0u, 0x01000404u, 0x6C0103FAu);

    const u32 has_mat_alpha = (mat[71] != 255u) ? 1u : 0u;
    put_w(g_ram + (gp - 16956u), has_mat_alpha);
    std::uint64_t g_prim;
    std::memcpy(&g_prim, g_ram + (gp - 20272u), 8u);
    if (has_mat_alpha | has_tex_alpha) g_prim |= 64ull;
    else g_prim &= ~64ull;
    std::memcpy(g_ram + (gp - 20272u), &g_prim, 8u);

    std::uint64_t tag_lo, tag_hi;
    std::memcpy(&tag_lo, g_ram + 0x009658A0u, 8u);
    std::memcpy(&tag_hi, g_ram + 0x009658A8u, 8u);
    const u32 pbits = static_cast<u32>(g_prim | static_cast<std::uint64_t>(mat[41])) & 2047u;
    tag_lo = (tag_lo & 0xFC007FFFFFFFFFFFull) | (static_cast<std::uint64_t>(pbits) << 47);
    std::memcpy(g_ram + 0x009658A0u, &tag_lo, 8u);
    put_q2(d32 + 8u, tag_lo, tag_hi);
    put_q4(d32 + 12u, 0u, 0u, 0x01000404u, 0x6C0103FBu);

    {
        const float cscale = (g_prim & 16u) ? std::bit_cast<float>(0x3B011B50u) : std::bit_cast<float>(0x3B808081u);
        const float ascale = std::bit_cast<float>(0x3B011B50u);
        df[16] = static_cast<float>(mat[68]) * cscale;
        df[17] = static_cast<float>(mat[69]) * cscale;
        df[18] = static_cast<float>(mat[70]) * cscale;
        df[19] = static_cast<float>(mat[71]) * ascale;
    }
    put_q4(d32 + 20u, 0u, 0u, 0x01000404u, 0x6C0103FCu);
    {
        float rgb0, rgb1, rgb2, f12;
        std::memcpy(&rgb0, col + 0u, 4u);
        std::memcpy(&rgb1, col + 4u, 4u);
        std::memcpy(&rgb2, col + 8u, 4u);
        std::memcpy(&f12, mat + 28u, 4u);
        const float k255 = std::bit_cast<float>(0x437F0001u);
        df[24] = rgb0 * k255;
        df[25] = rgb1 * k255;
        df[26] = rgb2 * k255;
        df[27] = f12;
    }
    put_q4(d32 + 28u, 0u, 0u, 0x01000404u, 0x6C0303FDu);

    u32 x2 = b40 & 7u;
    const bool use_alt = (word(g_ram + (gp - 16876u)) != 0u && !(x2 & 4u)) ||
                         (word(g_ram + (gp - 20232u)) != 0u && (x2 & 4u));
    if (use_alt) {
        x2 |= 8u;
        std::memcpy(dst + 128u, g_ram + 0x00965930u, 32u);
    } else {
        std::memcpy(dst + 128u, g_ram + 0x00965910u, 32u);
    }

    const u32 x4 = (word(g_ram + (gp - 16964u)) == 3u) ? 32u : 0u;
    put_w(g_ram + (gp - 16884u), x4);
    put_q4(d32 + 40u, x2, 0u, x4, word(g_ram + (gp - 16880u)));

    if (tex_at) {
        std::uint64_t tex1, clamp1;
        std::memcpy(&tex1, g_ram + (gp - 17040u), 8u);
        std::memcpy(&clamp1, g_ram + (gp - 17048u), 8u);
        put_q4(d32 + 44u, 0u, 0u, 0u, 0x50000004u);
        put_q4(d32 + 48u, 32771u, 0x10000000u, 14u, 0u);
        put_q2(d32 + 52u, test1, 71ull);
        put_q2(d32 + 56u, tex1, 20ull);
        put_q2(d32 + 60u, clamp1, 8ull);
    } else {
        put_q4(d32 + 44u, 0u, 0u, 0u, 0u);
        put_q4(d32 + 48u, 0u, 0u, 0u, 0u);
        put_q4(d32 + 52u, 0u, 0u, 0u, 0x50000002u);
        put_q4(d32 + 56u, 32769u, 0x10000000u, 14u, 0u);
        put_q2(d32 + 60u, test1, 71ull);
    }
    u32 off_w = 64u;

    mat[43] = idx;
    const u32 prog = plan.prog;
    if (word(g_ram + (gp - 16888u)) != prog) {
        put_q4(d32 + off_w, 0x50000000u, prog, 0u, 0u);
        put_w(g_ram + (gp - 16888u), prog);
        off_w += 4u;
    }
    const u32 x7 = word(p4 + 20u);
    const u32 x3_32 = word(x8 + 32u);
    put_q4(d32 + off_w, 0x50000000u, x3_32, 0x03000000u, x7 | 0x02000000u);
    off_w += 4u;
    put_w(g_ram + (gp - 17148u), spr + (off_w << 2));

    // Inline equivalent of set_interrupts(c, false); ++cnt1; set_interrupts(c, true);
    const u32 cnt1 = word(x8 + 24u) + 1u;
    put_w(x8 + 24u, cnt1);
    c.cop0[12] |= 0x10000u;
    const u32 cnt2 = word(x8 + 28u) + 1u;
    put_w(x8 + 28u, cnt2);
    if (cnt2 == 1u) {
        if (!rw_dma_ring_push_impl(x8_at + 24u, spent)) return false;
    }

    spent += 230;
    return true;
}

bool rw_mat_dma_impl(Cpu &c, u32 mat_at, std::int32_t &spent) {
    RwMatPlan plan;
    return rw_mat_dma_prepare(mat_at, 0u, plan) && rw_mat_dma_emit(c, plan, spent);
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
        put_w(p + 52u, mflags);
    }
    if ((mflags & 8u) == 0u) {
        put_w(p + 56u, gflags);
        if (gflags != word(cache + 44u)) {
            mflags |= 8u;
            put_w(p + 52u, mflags);
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
    SkinBonePlan skin_plan;
    u32 extra_qwc = 0u;
    if (has_skin) {
        if (!skin_bone_dma_prepare(mat_at, skin_plan)) return false;
        extra_qwc = skin_plan.extra_qwc;
    }
    RwMatPlan mat_plan;
    if (!rw_mat_dma_prepare(mat_at, extra_qwc, mat_plan)) return false;
    std::int32_t spent = 15;
    if (has_skin && !skin_bone_dma_emit(skin_plan, spent)) return false;
    if (!rw_mat_dma_emit(c, mat_plan, spent)) return false;
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

// Inner mesh loop of _rxPS2AllNode (0x0049F5D0, L_0049F7AC): runs clean non-dirty meshes natively.
// When objectFinalizeCallBack == 0, runs all remaining meshes (including 1-mesh atomics and the final mesh) and exits
// 0x0049F5D0 via L_0049FA64 directly; otherwise stops before the last mesh or any dirty/unsupported mesh and returns false.
bool native_rw_mesh_loop(Cpu &c) {
    constexpr u32 gp = 0x007D2CF0u;
    if (c.r[28].w[0] != gp) return false;
    const u32 sp = c.r[29].w[0];
    if (sp & 3u) return false;
    u8 *frame = range(sp + 160u, 192u);
    if (!frame) return false;
    const u32 mh_at = word(frame);   // sp + 160
    const u8 *mh = ram_ptr(mh_at, 8u);
    if (!mh) return false;
    const u32 num_meshes = static_cast<u32>(half_u16(mh + 4u));
    u32 idx = c.r[21].w[0];
    if (idx >= num_meshes) return false;

    const u32 x20 = c.r[20].w[0];
    if ((x20 & 3u) || !ram_ptr(x20, 28u)) return false;
    const u32 finalize_cb = word(g_ram + x20 + 20u);
    const u8 *epilogue_save = (finalize_cb == 0u && (sp & 15u) == 0u) ? range(sp, 152u) : nullptr;
    const u32 limit = epilogue_save ? num_meshes : (num_meshes - 1u);
    if (idx >= limit) return false;

    const u32 pipe_override = word(g_ram + x20 + 24u);
    const u32 def_pipe = word(frame + 16u);   // sp + 176
    const u32 base_flags = word(frame + 132u);   // sp + 292
    const u8 b312 = frame[152u] & 31u;   // sp + 312
    const u8 use_mat_col = frame[154u];   // sp + 314
    const u32 mat_arg_at = sp + 272u;   // frame + 112
    u8 *const mat_arg = frame + 112u;
    const u32 head_base_off = word(g_ram + (gp - 17408u));
    const u8 *head_ptr_p = ram_ptr(0x00960070u + head_base_off + 36u, 4u);
    if (!head_ptr_p) return false;
    const u32 lru_head_at = word(head_ptr_p);
    if ((lru_head_at & 3u) || !ram_ptr(lru_head_at, 4u)) return false;

    u32 x18 = c.r[18].w[0];
    u32 x19 = c.r[19].w[0];
    u32 x22 = c.r[22].w[0];
    u32 x30 = c.r[30].w[0];
    std::int32_t spent = 0;
    u32 done = 0u;

    while (idx < limit) {
        if ((x18 | x22) & 3u) break;
        const u8 *mesh = ram_ptr(x18, 12u);
        const u8 *slot_p = ram_ptr(x22 + 4u, 4u);
        if (!mesh || !slot_p) break;
        const u32 mat_at = word(mesh + 8u);
        if ((mat_at & 3u) || !ram_ptr(mat_at, 16u)) break;
        const u8 *mat_p = g_ram + mat_at;

        u32 pipe = pipe_override;
        if (pipe == 0u) {
            pipe = word(mat_p + 8u);
            if (pipe == 0u) pipe = def_pipe;
        }
        u32 next_x19 = x19;
        if (pipe != x30) {
            if ((pipe & 3u) || !ram_ptr(pipe, 12u)) break;
            const u32 nodes_at = word(g_ram + pipe + 8u);
            if ((nodes_at & 3u) || !ram_ptr(nodes_at, 24u)) break;
            next_x19 = word(g_ram + nodes_at + 20u);
        }
        if ((next_x19 & 3u) || !ram_ptr(next_x19, 20u)) break;
        const u32 mat_setup_cb = word(g_ram + next_x19);
        const u32 bridge_cb = word(g_ram + next_x19 + 12u);
        const u32 render_cb = word(g_ram + next_x19 + 16u);
        if (render_cb != 0u) break;

        // Pre-check bridge support (004A0E00, 00472130, or 0046EA80 with idx==0).
        int bridge_kind = 0;
        if (bridge_cb == 0x004A0E00u) bridge_kind = 1;
        else if (bridge_cb == 0x00472130u) bridge_kind = 2;
        else if (bridge_cb == 0x0046EA80u) {
            const u32 mfx_off = word(g_ram + (gp - 17848u));
            const u8 *ep = ram_ptr(mat_at + mfx_off, 4u);
            if (!ep) break;
            const u32 ea = word(ep);
            if (ea != 0u) {
                const u8 *e = ram_ptr(ea + 128u, 4u);
                if (!e || word(e) != 0u) break;
            }
            bridge_kind = 3;
        } else {
            break;
        }

        // Pre-check cache/dirty status before mutating stack or LRU.
        const u32 cache_at = word(slot_p);
        u32 new_mflags = base_flags;
        if (base_flags != 1u) {
            new_mflags |= 1u;
            if (cache_at == 0u || (cache_at & 3u) || !ram_ptr(cache_at, 48u)) break;
            if (mat_setup_cb != 0u) {
                if (mat_setup_cb != 0x004A0C20u) break;
                const u32 p12_at = word(frame + 124u);   // sp + 284
                if ((p12_at & 3u) || !ram_ptr(p12_at, 4u)) break;
                const u32 gflags = word(g_ram + p12_at);
                u32 count = word(mesh + 4u);
                if (gflags & 2u) count = (count - 2u) * 3u;
                if (count != word(g_ram + cache_at + 36u)) new_mflags |= 8u;
                if ((new_mflags & 8u) == 0u && gflags != word(g_ram + cache_at + 44u)) new_mflags |= 8u;
            }
            if (new_mflags & 14u) break;
        }

        // Write the stack struct (sp+276, sp+315..343) only after validating pointers above; if prepare fails
        // we restore x19/x30/sp+276 and break because stack fields sp+315..343 are dead on entry to L_0049F7AC.
        const u32 prev_x19 = x19;
        const u32 prev_x30 = x30;
        const u32 prev_sp276 = word(frame + 116u);
        if (pipe != x30) {
            x30 = pipe;
            x19 = next_x19;
            put_w(frame + 116u, x19);
        }
        const u32 surf_at = mat_at + 12u;
        const u32 tex_at = word(mat_p);
        const bool has_skin = (bridge_kind == 2) && (word(g_ram + 0x0095DE38u) != 0u);
        if (has_skin) {
            put_w(frame + 156u, x18);        // sp + 316 (read by skin_bone_dma_prepare)
        }
        frame[155u] = b312;                  // sp + 315
        if (use_mat_col != 0u) {
            std::memcpy(frame + 180u, mat_p + 4u, 4u);
        } else {
            std::memcpy(frame + 180u, g_ram + (gp - 20712u), 4u);
        }

        SkinBonePlan skin_plan;
        u32 extra_qwc = 0u;
        bool ok = true;
        if (has_skin) {
            ok = skin_bone_dma_prepare(mat_arg_at, skin_plan);
            extra_qwc = skin_plan.extra_qwc;
        }
        RwMatPlan mat_plan;
        if (!ok || !rw_mat_dma_prepare_parts(mat_arg, tex_at, surf_at, x19, cache_at, extra_qwc, mat_plan)) {
            x19 = prev_x19;
            x30 = prev_x30;
            put_w(frame + 116u, prev_sp276);
            break;
        }

        // LRU move-to-front (L_0049F8C8). When cache_at is already at the head (cache_at == first_e),
        // unlinking and re-inserting at head is a no-op; mutating with stale first_e would create a self-cycle!
        if (base_flags != 1u) {
            const u32 next_e = word(g_ram + cache_at);
            if (next_e != 0u) {
                const u32 prev_e = word(g_ram + cache_at + 4u);
                const u32 first_e = word(g_ram + lru_head_at);
                if (cache_at == first_e) {
                    spent += 35;
                } else if (!((prev_e | next_e | first_e) & 3u) && ram_ptr(prev_e, 4u) && ram_ptr(next_e, 8u) && ram_ptr(first_e, 8u)) {
                    put_w(g_ram + prev_e, next_e);
                    put_w(g_ram + next_e + 4u, prev_e);
                    put_w(g_ram + cache_at, first_e);
                    put_w(g_ram + cache_at + 4u, lru_head_at);
                    put_w(g_ram + first_e + 4u, cache_at);
                    put_w(g_ram + lru_head_at, cache_at);
                    spent += 35;
                }
            }
            if (mat_setup_cb != 0u) spent += 28;
        }

        std::int32_t iter_spent = 85;
        if (has_skin) {
            if (!skin_bone_dma_emit(skin_plan, iter_spent)) break;
            iter_spent += 15;
        } else if (bridge_kind == 3) {
            iter_spent += 28;
        }
        if (!rw_mat_dma_emit(c, mat_plan, iter_spent)) break;

        spent += iter_spent;
        x18 += 12u;
        x22 += 4u;
        ++idx;
        ++done;
    }

    if (epilogue_save && idx == num_meshes) {
        // Exact L_0049F9A8 -> L_0049F9BC -> L_0049FA64 epilogue of 0x0049F5D0.
        std::memcpy(&c.r[16], epilogue_save + 0u, 16u);
        std::memcpy(&c.r[17], epilogue_save + 16u, 16u);
        std::memcpy(&c.r[18], epilogue_save + 32u, 16u);
        std::memcpy(&c.r[19], epilogue_save + 48u, 16u);
        std::memcpy(&c.r[20], epilogue_save + 64u, 16u);
        std::memcpy(&c.r[21], epilogue_save + 80u, 16u);
        std::memcpy(&c.r[22], epilogue_save + 96u, 16u);
        std::memcpy(&c.r[23], epilogue_save + 112u, 16u);
        std::memcpy(&c.r[30], epilogue_save + 128u, 16u);
        std::memcpy(&c.r[31].d[0], epilogue_save + 144u, 8u);
        set32(c.r[29], sp + 352u);
        set32(c.r[2], 1u);
        c.budget -= spent + 17;
        return true;
    }

    if (done > 0u) {
        set32(c.r[18], x18);
        set32(c.r[19], x19);
        set32(c.r[21], idx);
        set32(c.r[22], x22);
        set32(c.r[30], x30);
        c.budget -= spent;
    }
    return false;
}

namespace {

// Compute stock atomic instance test (0x004A0480).
inline bool rw_atomic_instance_test_impl(const u8 *atomic, u8 *pd, std::int32_t &spent) {
    const u32 geom_at = word(atomic + 24u);
    if (geom_at & 3u) return false;
    const u8 *geom = ram_ptr(geom_at, 28u);
    if (!geom) return false;
    const u32 gflags = word(geom + 8u);
    spent += 8;
    if (gflags & 0x01000000u) return true;
    const u32 mh_at = word(pd + 12u);
    const u32 res_arr_at = word(pd + 16u);
    if ((mh_at | res_arr_at) & 3u) return false;
    const u8 *mh = ram_ptr(mh_at, 8u);
    const u8 *res_arr = ram_ptr(res_arr_at, 8u);
    if (!mh || !res_arr) return false;
    const u32 obj_id = static_cast<u32>(half_u16(mh + 6u)) | ((gflags & 0x9Fu) << 16) | (static_cast<u32>(geom[24u]) << 24);
    u32 obj_inst = word(pd + 20u) & ~1u;
    const u32 res_entry_at = word(res_arr + 4u);
    spent += 18;
    if (res_entry_at != 0u) {
        if (res_entry_at & 3u) return false;
        const u8 *re = ram_ptr(res_entry_at, 68u);
        if (!re) return false;
        const u32 cached_id = word(re + 40u);
        if (obj_id != cached_id) {
            if (((obj_id >> 16) & 0xFFu) == ((cached_id >> 16) & 0xFFu) &&
                ((obj_id >> 24) & 0xFFu) == ((cached_id >> 24) & 0xFFu)) {
                obj_inst |= 4u;
            } else {
                obj_inst |= 8u;
            }
            spent += 16;
        }
        const u32 lock_bits = static_cast<u32>(half_u16(geom + 12u));
        if (lock_bits != 0u) {
            if (lock_bits & 1u) obj_inst |= 8u;
            else obj_inst |= 2u | (lock_bits << 3);
            spent += 10;
        }
        if (word(atomic + 76u) & 1u) {
            std::int16_t start_mt, end_mt;
            std::memcpy(&start_mt, atomic + 80u, 2u);
            std::memcpy(&end_mt, atomic + 82u, 2u);
            if (static_cast<std::int32_t>(word(re + 64u)) != static_cast<std::int32_t>(end_mt) ||
                static_cast<std::int32_t>(word(re + 60u)) != static_cast<std::int32_t>(start_mt)) {
                obj_inst |= 4u;
            }
            spent += 11;
        }
        spent += 12;
    }
    put_w(pd + 24u, obj_id);
    put_w(pd + 20u, obj_inst);
    return true;
}

// Append one light into the PS2All light block at 0x7CE884 / count 0x7CE880 (0x0049FF10).
inline bool rw_add_light_impl(Cpu &c, u32 light_at, const u8 *inv_ltm, float scale, float inv_scale,
                              std::int32_t &spent) {
    constexpr u32 gp = 0x007D2CF0u;
    if (light_at & 3u) return false;
    const u8 *light = ram_ptr(light_at, 44u);
    if (!light) return false;
    u32 cnt = word(g_ram + (gp - 17520u));
    spent += 20;
    if (cnt >= 124u) return true;
    u32 ptr = word(g_ram + (gp - 17516u));
    if (ptr & 3u) return false;
    u8 *dst = ram_ptr(ptr, 128u);
    if (!dst) return false;

    const u32 lframe = word(light + 4u);
    const u8 ltype = light[1u];
    u32 dir_at = 0u, pos_at = 0u;
    if (lframe != 0u) {
        u32 ltm_at = 0u;
        if (!frame_get_ltm_impl(c, lframe, ltm_at, spent)) return false;
        dir_at = ltm_at + 32u;
        pos_at = ltm_at + 48u;
    } else if (ltype == 1u || ltype == 128u || ltype == 129u || ltype == 130u) {
        return false;
    }

    if (cnt == 0u) {
        auto *df = reinterpret_cast<Float *>(dst);
        if (!inv_ltm) {
            auto *dw = reinterpret_cast<u32 *>(dst);
            dw[0] = 0x3F800000u; dw[1] = 0u; dw[2] = 0u; df[3] = scale;
            dw[4] = 0u; dw[5] = 0x3F800000u; dw[6] = 0u; df[7] = scale;
            dw[8] = 0u; dw[9] = 0u; dw[10] = 0x3F800000u; df[11] = scale;
            dw[12] = 0u; dw[13] = 0u; dw[14] = 0u; df[15] = inv_scale;
            spent += 70;
        } else {
            const auto *m = reinterpret_cast<const Float *>(inv_ltm);
            df[0] = m[0];  df[1] = m[1];  df[2] = m[2];  df[3] = scale;
            df[4] = m[4];  df[5] = m[5];  df[6] = m[6];  df[7] = scale;
            df[8] = m[8];  df[9] = m[9];  df[10] = m[10]; df[11] = scale;
            df[12] = m[12]; df[13] = m[13]; df[14] = m[14]; df[15] = inv_scale;
            spent += 80;
        }
        ptr += 64u;
        dst += 64u;
        cnt += 4u;
    }

    std::memcpy(dst, light + 24u, 12u);
    put_w(dst + 12u, static_cast<u32>(ltype));
    spent += 18;
    if (ltype == 2u) {
        ptr += 16u;
        cnt += 1u;
        spent += 16;
    } else if (ltype == 1u) {
        std::memcpy(dst + 16u, g_ram + dir_at, 12u);
        put_w(dst + 28u, 0u);
        ptr += 32u;
        cnt += 2u;
        spent += 41;
    } else if (ltype == 128u) {
        std::memcpy(dst + 16u, g_ram + pos_at, 12u);
        put_w(dst + 28u, word(light + 20u));
        ptr += 32u;
        cnt += 2u;
        spent += 39;
    } else if (ltype == 129u || ltype == 130u) {
        std::memcpy(dst + 16u, g_ram + pos_at, 12u);
        put_w(dst + 28u, word(light + 20u));
        std::memcpy(dst + 32u, g_ram + dir_at, 12u);
        put_w(dst + 44u, word(light + 40u));
        ptr += 48u;
        cnt += 3u;
        spent += 58;
    }
    put_w(g_ram + (gp - 17516u), ptr);
    put_w(g_ram + (gp - 17520u), cnt);
    return true;
}

}   // namespace

// Stock atomic instance test (0x004A0480).
bool native_rw_atomic_instance_test(Cpu &c) {
    const u32 atomic_at = c.r[4].w[0], pd_at = c.r[5].w[0];
    if ((atomic_at | pd_at) & 3u) return false;
    const u8 *atomic = ram_ptr(atomic_at, 84u);
    u8 *pd = ram_ptr(pd_at, 28u);
    if (!atomic || !pd) return false;
    std::int32_t spent = 0;
    if (!rw_atomic_instance_test_impl(atomic, pd, spent)) return false;
    c.budget -= spent;
    return true;
}

// Per-light PS2All light block builder (0x0049FF10).
bool native_rw_add_light(Cpu &c) {
    constexpr u32 gp = 0x007D2CF0u;
    if (c.r[28].w[0] != gp) return false;
    const u32 inv_at = c.r[5].w[0];
    const u8 *inv_ltm = nullptr;
    if (inv_at != 0u) {
        if (inv_at & 3u) return false;
        inv_ltm = range(inv_at, 64u);
        if (!inv_ltm) return false;
    }
    std::int32_t spent = 0;
    if (!rw_add_light_impl(c, c.r[4].w[0], inv_ltm, std::bit_cast<float>(c.f[12]), std::bit_cast<float>(c.f[13]), spent))
        return false;
    c.budget -= spent;
    return true;
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
namespace {

inline void rw_matrix_scale_impl(u8 *mp, const float *s, u32 op) {
    if (op == 0u) {
        const u32 fl = word(mp + 12u) & 0xFFFDFFFCu;
        auto *w = reinterpret_cast<u32 *>(mp);
        w[0] = std::bit_cast<u32>(s[0]); w[1] = 0u; w[2] = 0u; w[3] = fl;
        w[4] = 0u; w[5] = std::bit_cast<u32>(s[1]); w[6] = 0u; w[7] = 0u;
        w[8] = 0u; w[9] = 0u; w[10] = std::bit_cast<u32>(s[2]); w[11] = 0u;
        w[12] = 0u; w[13] = 0u; w[14] = 0u;
    } else if (op == 1u) {
        auto *m = reinterpret_cast<Float *>(mp);
        m[0] = m[0] * s[0]; m[1] = m[1] * s[0]; m[2] = m[2] * s[0];
        m[4] = m[4] * s[1]; m[5] = m[5] * s[1]; m[6] = m[6] * s[1];
        m[8] = m[8] * s[2]; m[9] = m[9] * s[2]; m[10] = m[10] * s[2];
        put_w(mp + 12u, word(mp + 12u) & 0xFFFDFFFCu);
    } else {
        auto *m = reinterpret_cast<Float *>(mp);
        m[0] = m[0] * s[0]; m[1] = m[1] * s[1]; m[2] = m[2] * s[2];
        m[4] = m[4] * s[0]; m[5] = m[5] * s[1]; m[6] = m[6] * s[2];
        m[8] = m[8] * s[0]; m[9] = m[9] * s[1]; m[10] = m[10] * s[2];
        m[12] = m[12] * s[0]; m[13] = m[13] * s[1]; m[14] = m[14] * s[2];
        put_w(mp + 12u, word(mp + 12u) & 0xFFFDFFFCu);
    }
}

// Exact PS2 SDK (fdlibm) single-precision trig/math routines matching 0x0052C000..0x0052E92C and 0x0052ABD8..0x0052B540.
bool ps2_rem_pio2f(float x_in, float &y0, float &y1, std::int32_t &n) {
    static constexpr u32 kNpio2Hw[32] = {
        0x3FC90F00u, 0x40490F00u, 0x4096CB00u, 0x40C90F00u, 0x40FB5300u, 0x4116CB00u, 0x412FED00u, 0x41490F00u,
        0x41623100u, 0x417B5300u, 0x418A3A00u, 0x4196CB00u, 0x41A35C00u, 0x41AFED00u, 0x41BC7E00u, 0x41C90F00u,
        0x41D5A000u, 0x41E23100u, 0x41EEC200u, 0x41FB5300u, 0x4203F200u, 0x420A3A00u, 0x42108300u, 0x4216CB00u,
        0x421D1400u, 0x42235C00u, 0x4229A500u, 0x422FED00u, 0x42363600u, 0x423C7E00u, 0x4242C700u, 0x42490F00u,
    };
    const u32 hx = std::bit_cast<u32>(x_in);
    const u32 ix = hx & 0x7FFFFFFFu;
    if (static_cast<std::int32_t>(ix) <= 0x3F490FD8) {
        y0 = x_in;
        y1 = 0.0f;
        n = 0;
        return true;
    }
    const float pio2_1 = from_bits(0x3FC90F80u), pio2_1t = from_bits(0x37354442u);
    const float pio2_2 = from_bits(0x373543FFu), pio2_2t = from_bits(0x2E85A308u);
    if (static_cast<std::int32_t>(ix) <= 0x4016CBE3) {
        if (static_cast<std::int32_t>(hx) > 0) {
            float r = x_in - pio2_1;
            float w = pio2_1t;
            if ((ix & 0xFFFFFFF0u) == 0x3FC90FD0u) {
                r = r - pio2_2;
                w = pio2_2t;
            }
            y0 = r - w;
            y1 = (r - y0) - w;
            n = 1;
        } else {
            float r = x_in + pio2_1;
            float w = pio2_1t;
            if ((ix & 0xFFFFFFF0u) == 0x3FC90FD0u) {
                r = r + pio2_2;
                w = pio2_2t;
            }
            y0 = r + w;
            y1 = (r - y0) + w;
            n = -1;
        }
        return true;
    }
    if (static_cast<std::int32_t>(ix) > 0x43490F80) return false;
    const float t = from_bits(ix);
    const float invpio2 = from_bits(0x3F22F984u);
    std::int32_t nn = static_cast<std::int32_t>(t * invpio2 + 0.5f);
    const float fn = static_cast<float>(nn);
    float r = t - fn * pio2_1;
    float w = fn * pio2_1t;
    float y_hi = r - w;
    if (nn >= 32 || (ix & 0xFFFFFF00u) == kNpio2Hw[nn - 1]) {
        const std::int32_t i_exp = static_cast<std::int32_t>(ix >> 23);
        std::int32_t j = i_exp - static_cast<std::int32_t>((std::bit_cast<u32>(y_hi) >> 23) & 255u);
        if (j >= 9) {
            float t2 = r;
            w = fn * pio2_2;
            r = t2 - w;
            w = fn * pio2_2t - ((t2 - r) - w);
            y_hi = r - w;
            j = i_exp - static_cast<std::int32_t>((std::bit_cast<u32>(y_hi) >> 23) & 255u);
            if (j >= 26) {
                t2 = r;
                w = fn * from_bits(0x2E85A300u);
                r = t2 - w;
                w = fn * from_bits(0x248D3131u) - ((t2 - r) - w);
                y_hi = r - w;
            }
        }
    }
    float y_lo = (r - y_hi) - w;
    if (static_cast<std::int32_t>(hx) < 0) {
        y_hi = from_bits(std::bit_cast<u32>(y_hi) ^ fp::kSign);
        y_lo = from_bits(std::bit_cast<u32>(y_lo) ^ fp::kSign);
        nn = -nn;
    }
    y0 = y_hi;
    y1 = y_lo;
    n = nn;
    return true;
}

inline float ps2_kcosf(float x, float y) {
    const u32 ix = std::bit_cast<u32>(x) & 0x7FFFFFFFu;
    if (static_cast<std::int32_t>(ix) <= 0x31FFFFFF && static_cast<std::int32_t>(x) == 0) return 1.0f;
    const float z = x * x;
    float r = z * from_bits(0xAD47D74Eu) + from_bits(0x310F74F5u);
    r = z * r + from_bits(0xB493F27Bu);
    r = z * r + from_bits(0x37D00D00u);
    r = z * r + from_bits(0xBAB60B60u);
    r = z * r + from_bits(0x3D2AAAAAu);
    r = z * r;
    const float xy_term = z * r - x * y;
    const float hz = 0.5f * z;
    if (static_cast<std::int32_t>(ix) <= 0x3E999999) return 1.0f - (hz - xy_term);
    const float qx = (static_cast<std::int32_t>(ix) > 0x3F480000) ? from_bits(0x3E900000u) : from_bits(ix - 0x01000000u);
    return (1.0f - qx) - ((hz - qx) - xy_term);
}

inline float ps2_ksinf(float x, float y, int iy) {
    const u32 ix = std::bit_cast<u32>(x) & 0x7FFFFFFFu;
    if (static_cast<std::int32_t>(ix) <= 0x31FFFFFF && static_cast<std::int32_t>(x) == 0) return x;
    const float z = x * x;
    const float v = z * x;
    float r = z * from_bits(0x2F2EC9D2u) + from_bits(0xB2D72F34u);
    r = z * r + from_bits(0x3638EF1Au);
    r = z * r + from_bits(0xB9500D01u);
    r = z * r + from_bits(0x3C088889u);
    if (iy == 0) return x + v * (z * r + from_bits(0xBE2AAAAAu));
    return x - ((z * (0.5f * y - v * r) - y) - v * from_bits(0xBE2AAAAAu));
}

bool ps2_cosf(float x, float &out) {
    const u32 ix = std::bit_cast<u32>(x) & 0x7FFFFFFFu;
    if (static_cast<std::int32_t>(ix) <= 0x3F490FD8) {
        out = ps2_kcosf(x, 0.0f);
        return true;
    }
    float y0, y1;
    std::int32_t n;
    if (!ps2_rem_pio2f(x, y0, y1, n)) return false;
    switch (n & 3) {
    case 0: out = ps2_kcosf(y0, y1); break;
    case 1: out = from_bits(std::bit_cast<u32>(ps2_ksinf(y0, y1, 1)) ^ fp::kSign); break;
    case 2: out = from_bits(std::bit_cast<u32>(ps2_kcosf(y0, y1)) ^ fp::kSign); break;
    default: out = ps2_ksinf(y0, y1, 1); break;
    }
    return true;
}

bool ps2_sinf(float x, float &out) {
    const u32 ix = std::bit_cast<u32>(x) & 0x7FFFFFFFu;
    if (static_cast<std::int32_t>(ix) <= 0x3F490FD8) {
        out = ps2_ksinf(x, 0.0f, 0);
        return true;
    }
    float y0, y1;
    std::int32_t n;
    if (!ps2_rem_pio2f(x, y0, y1, n)) return false;
    switch (n & 3) {
    case 0: out = ps2_ksinf(y0, y1, 1); break;
    case 1: out = ps2_kcosf(y0, y1); break;
    case 2: out = from_bits(std::bit_cast<u32>(ps2_ksinf(y0, y1, 1)) ^ fp::kSign); break;
    default: out = from_bits(std::bit_cast<u32>(ps2_kcosf(y0, y1)) ^ fp::kSign); break;
    }
    return true;
}

// Exact PS2 SDK acosf (0x0052ABD8).
[[gnu::always_inline]] inline void acosf_poly_pq(float z, float &p_out, float &q_out) {
    float p = z * from_bits(0x3811EF08u) + from_bits(0x3A4F7F04u);
    p = z * p + from_bits(0xBD241146u);
    p = z * p + from_bits(0x3E4E0AA8u);
    p = z * p + from_bits(0xBEA6B08Fu);
    p = z * p + from_bits(0x3E2AAAAAu);
    p_out = z * p;
    float q = z * from_bits(0x3D9DC62Du) + from_bits(0xBF303360u);
    q = z * q + from_bits(0x4001572Cu);
    q = z * q + from_bits(0xC019D138u);
    q_out = z * q + 1.0f;
}

[[gnu::noinline]] float ps2_acosf(Cpu &c, float x) {
    const u32 hx = std::bit_cast<u32>(x);
    const u32 ix = hx & 0x7FFFFFFFu;
    if (ix == 0x3F800000u) return (static_cast<std::int32_t>(hx) > 0) ? 0.0f : from_bits(0x40490FDAu);
    if (static_cast<std::int32_t>(ix) > 0x3F800000) return divide(c, x - x, x - x);
    if (static_cast<std::int32_t>(ix) <= 0x3EFFFFFF) {
        if (static_cast<std::int32_t>(ix) <= 0x23000000) return from_bits(0x3FC90FDAu);
        const float z = x * x;
        float p, q;
        acosf_poly_pq(z, p, q);
        const float r = divide(c, p, q);
        return from_bits(0x3FC90FDAu) - (x - (from_bits(0x33A22168u) - x * r));
    }
    if (static_cast<std::int32_t>(hx) < 0) {
        const float z = (x + 1.0f) * 0.5f;
        float p, q;
        acosf_poly_pq(z, p, q);
        const float s = hw_sqrtf(z);
        const float r = divide(c, p, q);
        const float w = r * s - from_bits(0x33A22168u);
        const float t = s + w;
        return from_bits(0x40490FDAu) - (t + t);
    }
    const float z = (1.0f - x) * 0.5f;
    const float s = hw_sqrtf(z);
    const float df = from_bits(std::bit_cast<u32>(s) & 0xFFFFF000u);
    const float cc = divide(c, z - df * df, s + df);
    float p, q;
    acosf_poly_pq(z, p, q);
    const float r = divide(c, p, q);
    const float t = df + (r * s + cc);
    return t + t;
}

// Exact PS2 SDK atanf (0x0052E408).
float ps2_atanf(Cpu &c, float x_in) {
    static constexpr u32 kAtanHi[4] = {0x3EED6338u, 0x3F490FDAu, 0x3F7B985Du, 0x3FC90FDAu};
    static constexpr u32 kAtanLo[4] = {0x31AC3769u, 0x33222167u, 0x33140FB4u, 0x33A22168u};
    const u32 hx = std::bit_cast<u32>(x_in);
    const u32 ix = hx & 0x7FFFFFFFu;
    if (static_cast<std::int32_t>(ix) > 0x507FFFFF) {
        return (static_cast<std::int32_t>(hx) > 0) ? from_bits(0x3FC90FDAu) : from_bits(0xBFC90FDAu);
    }
    float x = x_in;
    int id = -1;
    if (static_cast<std::int32_t>(ix) <= 0x3EDFFFFF) {
        if (static_cast<std::int32_t>(ix) <= 0x30FFFFFF) {
            if (fp::key(std::bit_cast<u32>(x + from_bits(0x7149F2C9u))) > fp::key(0x3F800000u)) return x;
        }
    } else {
        x = from_bits(ix);
        if (static_cast<std::int32_t>(ix) <= 0x3F97FFFF) {
            if (static_cast<std::int32_t>(ix) <= 0x3F2FFFFF) {
                id = 0;
                x = divide(c, (x + x) - 1.0f, x + 2.0f);
            } else {
                id = 1;
                x = divide(c, x - 1.0f, x + 1.0f);
            }
        } else if (static_cast<std::int32_t>(ix) <= 0x401BFFFF) {
            id = 2;
            x = divide(c, x - 1.5f, x * 1.5f + 1.0f);
        } else {
            id = 3;
            x = divide(c, -1.0f, x);
        }
    }
    const float z = x * x;
    const float w = z * z;
    float s1 = w * from_bits(0x3C8569D7u) + from_bits(0x3D4BDA59u);
    s1 = w * s1 + from_bits(0x3D886B34u);
    s1 = w * s1 + from_bits(0x3DBA2E6Eu);
    s1 = w * s1 + from_bits(0x3E124924u);
    s1 = w * s1 + from_bits(0x3EAAAAABu);
    s1 = z * s1;
    float s2 = w * from_bits(0xBD15A221u) + from_bits(0xBD6EF16Au);
    s2 = w * s2 + from_bits(0xBD9D8795u);
    s2 = w * s2 + from_bits(0xBDE38E38u);
    s2 = w * s2 + from_bits(0xBE4CCCCCu);
    s2 = w * s2;
    if (id < 0) return x - x * (s1 + s2);
    const float z_res = from_bits(kAtanHi[id]) - ((x * (s1 + s2) - from_bits(kAtanLo[id])) - x);
    return (static_cast<std::int32_t>(hx) < 0) ? from_bits(std::bit_cast<u32>(z_res) ^ fp::kSign) : z_res;
}

// Exact PS2 SDK atan2f (0x0052B380).
[[gnu::noinline]] float ps2_atan2f(Cpu &c, float y, float x) {
    const u32 hx = std::bit_cast<u32>(x);
    const u32 hy = std::bit_cast<u32>(y);
    if (hx == 0x3F800000u) return ps2_atanf(c, y);
    const u32 ix = hx & 0x7FFFFFFFu;
    const u32 iy = hy & 0x7FFFFFFFu;
    const u32 m = ((hx >> 30) & 2u) | (hy >> 31);
    if (static_cast<std::int32_t>(iy) <= 0x007FFFFF) {
        if (m == 2u) return from_bits(0x40490FDAu);
        if (m == 3u) return from_bits(0xC0490FDAu);
        if (m == 0u || m == 1u) return y;
    }
    if (static_cast<std::int32_t>(ix) <= 0x007FFFFF) {
        return (static_cast<std::int32_t>(hy) < 0) ? from_bits(0xBFC90FDAu) : from_bits(0x3FC90FDAu);
    }
    const std::int32_t k = static_cast<std::int32_t>(iy - ix) >> 23;
    float z;
    if (k >= 61) {
        z = from_bits(0x3FC90FDAu);
    } else if (static_cast<std::int32_t>(hx) < 0 && k < -60) {
        z = 0.0f;
    } else {
        const float ratio = divide(c, y, x);
        z = ps2_atanf(c, from_bits(std::bit_cast<u32>(ratio) & 0x7FFFFFFFu));
    }
    switch (m) {
    case 0u: return z;
    case 1u: return from_bits(std::bit_cast<u32>(z) ^ fp::kSign);
    case 2u: return from_bits(0x40490FDAu) - (z - from_bits(0x34222168u));
    default: return (z - from_bits(0x34222168u)) - from_bits(0x40490FDAu);
    }
}

// RwErrorSet({1, code}) matching 0x004C1C50 (only called on zero-length RwV3dNormalize).
[[gnu::noinline, gnu::cold]] void rw_error_set_zero_vec() {
    constexpr u32 gp = 0x007D2CF0u;
    const u32 off = word(g_ram + gp - 17440u);
    u8 *err = range(0x00960070u + off, 8u);
    if (!err) return;
    if (word(err) == 0u && word(err + 4u) == 0x80000000u) {
        put_w(err, 1u);
        put_w(err + 4u, 25u);
    }
}

// RwV3dNormalize (0x004C69F0): normalizes in -> out, returns length.
inline float rw_v3d_normalize_impl(Cpu &c, float *out, const float *in, bool set_err, std::int32_t &spent) {
    const float x = in[0], y = in[1], z = in[2];
    const float len2 = (y * y + x * x) + z * z;
    const float len = hw_sqrtf(len2);
    const bool zero = fp::key(std::bit_cast<u32>(len)) <= fp::key(0u);
    const float s = zero ? len : divide(c, 1.0f, len);
    out[0] = x * s;
    out[1] = y * s;
    out[2] = z * s;
    if (zero && set_err) {
        rw_error_set_zero_vec();
        spent += 58;
    } else {
        spent += zero ? 34 : 41;
    }
    return len;
}

// Default RwV3dTransformPoint (0x004C6700) and RwV3dTransformVector (0x004C6840).
inline void rw_v3d_transform_point_impl(float *out, const float *in, const float *m) {
    const float x = in[0], y = in[1], z = in[2];
    const float ox = ((x * m[0] + y * m[4]) + z * m[8]) + m[12];
    const float oy = ((x * m[1] + y * m[5]) + z * m[9]) + m[13];
    const float oz = ((x * m[2] + y * m[6]) + z * m[10]) + m[14];
    out[0] = ox; out[1] = oy; out[2] = oz;
}

inline void rw_v3d_transform_vector_impl(float *out, const float *in, const float *m) {
    const float x = in[0], y = in[1], z = in[2];
    const float ox = (x * m[0] + y * m[4]) + z * m[8];
    const float oy = (x * m[1] + y * m[5]) + z * m[9];
    const float oz = (x * m[2] + y * m[6]) + z * m[10];
    out[0] = ox; out[1] = oy; out[2] = oz;
}

// RwMatrixRotate (0x004C31B0) + RwMatrixRotateOneMinusCosineSine (0x004C2FC0).
[[gnu::noinline]] bool rw_matrix_rotate_impl(Cpu &c, u8 *mat_b, const float *axis, float angle_deg, u32 op,
                           u32 vf10_w, u32 vf11_w, u32 vf12_w, std::int32_t &spent) {
    if (op > 2u) return false;
    const float rad = from_bits(0x3C8EFA35u) * angle_deg;
    const float ax = axis[0], ay = axis[1], az = axis[2];
    float len = hw_sqrtf((ay * ay + ax * ax) + az * az);
    if (fp::key(std::bit_cast<u32>(len)) > fp::key(0u)) len = divide(c, 1.0f, len);
    const float ux = ax * len, uy = ay * len, uz = az * len;
    float s_val, c_val;
    if (!ps2_sinf(rad, s_val) || !ps2_cosf(rad, c_val)) return false;
    const float omc = 1.0f - c_val;

    alignas(16) u8 rot_b[64];
    auto *r = reinterpret_cast<Float *>(rot_b);
    const float ux_s = ux * s_val, uy_s = uy * s_val, uz_s = uz * s_val;
    const float yz_o = (uy * uz) * omc;
    const float zx_o = (uz * ux) * omc;
    const float xy_o = (ux * uy) * omc;
    r[0] = 1.0f - (1.0f - ux * ux) * omc;
    r[1] = xy_o + uz_s;
    r[2] = zx_o - uy_s;
    put_w(rot_b + 12u, 3u);
    r[4] = xy_o - uz_s;
    r[5] = 1.0f - (1.0f - uy * uy) * omc;
    r[6] = yz_o + ux_s;
    put_w(rot_b + 28u, 0u);
    r[8] = zx_o + uy_s;
    r[9] = yz_o - ux_s;
    r[10] = 1.0f - (1.0f - uz * uz) * omc;
    put_w(rot_b + 44u, 0u);
    r[12] = 0.0f; r[13] = 0.0f; r[14] = 0.0f;
    put_w(rot_b + 60u, 0u);

    if (op == 0u) {
        std::memcpy(mat_b, rot_b, 64u);
        spent += 250;
    } else {
        alignas(16) u8 tmp_b[64];
        const u8 *a_b = (op == 1u) ? rot_b : mat_b;
        const u8 *b_b = (op == 1u) ? mat_b : rot_b;
        const auto *a = reinterpret_cast<const Float *>(a_b);
        const auto *b = reinterpret_cast<const Float *>(b_b);
        auto *t = reinterpret_cast<Float *>(tmp_b);
        product_row<3, false>(t, a, b);
        product_row<3, false>(t + 4, a + 4, b);
        product_row<3, false>(t + 8, a + 8, b);
        product_row<3, true>(t + 12, a + 12, b);
        put_w(tmp_b + 12u, word(a_b + 12u) & word(b_b + 12u));
        put_w(tmp_b + 28u, vf10_w);
        put_w(tmp_b + 44u, vf11_w);
        put_w(tmp_b + 60u, vf12_w);
        std::memcpy(mat_b, tmp_b, 64u);
        spent += 285;
    }
    return true;
}

// RtQuatConvertFromMatrix (0x004BDCB0) + 0x004BDB30/0x004BDBB0/0x004BDC30.
[[gnu::noinline]] void rt_quat_from_matrix_impl(Cpu &c, float *q, const float *m, std::int32_t &spent) {
    const float m00 = m[0], m11 = m[5], m22 = m[10];
    const float tr = m22 + (m00 + m11);
    if (fp::key(std::bit_cast<u32>(tr)) > fp::key(0u)) {
        const float s = hw_sqrtf(1.0f + tr);
        const float k = divide(c, 0.5f, s);
        q[3] = 0.5f * s;
        q[0] = k * (m[6] - m[9]);
        q[1] = k * (m[8] - m[2]);
        q[2] = k * (m[1] - m[4]);
        spent += 48;
    } else if (fp::key(std::bit_cast<u32>(m00)) > fp::key(std::bit_cast<u32>(m11)) &&
               fp::key(std::bit_cast<u32>(m00)) > fp::key(std::bit_cast<u32>(m22))) {
        const float s = hw_sqrtf(1.0f + (m00 - (m11 + m22)));
        const float k = divide(c, 0.5f, s);
        q[0] = 0.5f * s;
        q[3] = k * (m[6] - m[9]);
        q[1] = k * (m[1] + m[4]);
        q[2] = k * (m[2] + m[8]);
        spent += 58;
    } else if (fp::key(std::bit_cast<u32>(m11)) > fp::key(std::bit_cast<u32>(m22))) {
        const float s = hw_sqrtf(1.0f + (m11 - (m22 + m00)));
        const float k = divide(c, 0.5f, s);
        q[1] = 0.5f * s;
        q[3] = k * (m[8] - m[2]);
        q[2] = k * (m[6] + m[9]);
        q[0] = k * (m[4] + m[1]);
        spent += 60;
    } else {
        const float s = hw_sqrtf(1.0f + (m22 - (m00 + m11)));
        const float k = divide(c, 0.5f, s);
        q[2] = 0.5f * s;
        q[3] = k * (m[1] - m[4]);
        q[0] = k * (m[8] + m[2]);
        q[1] = k * (m[9] + m[6]);
        spent += 60;
    }
}

// RtQuatSetupSlerpCache (0x004BE310): populates cache[0..3]=q0, cache[4..7]=q1, cache[8]=theta, use_lerp.
[[gnu::noinline]] void rt_quat_slerp_cache_impl(Cpu &c, const float *q0, const float *q1, float *cache, u32 &use_lerp, std::int32_t &spent) {
    cache[0] = q0[0]; cache[1] = q0[1]; cache[2] = q0[2]; cache[3] = q0[3];
    float dot = ((q0[1] * q1[1] + q0[0] * q1[0]) + q0[2] * q1[2]) + q0[3] * q1[3];
    if (fp::key(std::bit_cast<u32>(dot)) < fp::key(0u)) {
        if (fp::key(std::bit_cast<u32>(dot)) < fp::key(0xBF800000u)) dot = 1.0f;
        else dot = from_bits(std::bit_cast<u32>(dot) ^ fp::kSign);
        cache[4] = from_bits(std::bit_cast<u32>(q1[0]) ^ fp::kSign);
        cache[5] = from_bits(std::bit_cast<u32>(q1[1]) ^ fp::kSign);
        cache[6] = from_bits(std::bit_cast<u32>(q1[2]) ^ fp::kSign);
        cache[7] = from_bits(std::bit_cast<u32>(q1[3]) ^ fp::kSign);
    } else {
        if (fp::key(std::bit_cast<u32>(dot)) > fp::key(0x3F800000u)) dot = 1.0f;
        cache[4] = q1[0]; cache[5] = q1[1]; cache[6] = q1[2]; cache[7] = q1[3];
    }
    const u32 bits = std::bit_cast<u32>(dot);
    const u32 mag = bits & fp::kAbs;
    float theta;
    if (static_cast<std::int32_t>(mag) >= 0x3F800000) {
        theta = (static_cast<std::int32_t>(bits) > 0) ? 0.0f : from_bits(0x40490FDBu);
    } else if (static_cast<std::int32_t>(mag) >= 0x3F000000) {
        if (static_cast<std::int32_t>(bits) < 0) {
            const float a = 0.5f * (1.0f + dot);
            const float s = hw_sqrtf(a);
            const float r = divide(c, acos_top(a), acos_bottom(a));
            theta = from_bits(0x40490FDAu) - 2.0f * (s + (r * s - from_bits(0x33A22168u)));
        } else {
            const float a = 0.5f * (1.0f - dot);
            const float s = hw_sqrtf(a);
            const float r = divide(c, acos_top(a), acos_bottom(a));
            const float high = from_bits(std::bit_cast<u32>(s) & 0xFFFFF000u);
            const float low = divide(c, a - high * high, s + high);
            theta = 2.0f * (high + (low + r * s));
        }
    } else if (static_cast<std::int32_t>(mag) < 0x23000001) {
        theta = from_bits(0x3FC90FDBu);
    } else {
        const float a = dot * dot;
        const float r = divide(c, acos_top(a), acos_bottom(a));
        theta = from_bits(0x3FC90FDAu) - (dot - (from_bits(0x33A22168u) - dot * r));
    }
    cache[8] = theta;
    use_lerp = (fp::key(bits) < static_cast<std::int32_t>(0x3F7FFF58u)) ? 0u : 1u;
    if (use_lerp == 0u) {
        const float inv_s = divide(c, 1.0f, sine(theta, theta * theta));
        for (int i = 0; i < 8; ++i) cache[i] *= inv_s;
        spent += 165;
    } else {
        spent += 105;
    }
}

// Inline slerp evaluation matching L_00312338..L_0031249C and L_003125BC..L_00312760.
[[gnu::noinline]] void rt_quat_slerp_eval(float *out, const float *q0, const float *q1, const float *cache, u32 use_lerp, float t, std::int32_t &spent) {
    if (fp::key(std::bit_cast<u32>(t)) <= fp::key(0u)) {
        out[0] = q0[0]; out[1] = q0[1]; out[2] = q0[2]; out[3] = q0[3];
        spent += 10;
        return;
    }
    if (fp::key(0x3F800000u) <= fp::key(std::bit_cast<u32>(t))) {
        out[0] = q1[0]; out[1] = q1[1]; out[2] = q1[2]; out[3] = q1[3];
        spent += 16;
        return;
    }
    float w0 = 1.0f - t, w1 = t;
    if (use_lerp == 0u) {
        const float theta = cache[8];
        const float a0 = w0 * theta, a1 = t * theta;
        w0 = sine(a0, a0 * a0);
        w1 = sine(a1, a1 * a1);
        spent += 37;
    }
    out[0] = cache[0] * w0 + cache[4] * w1;
    out[1] = cache[1] * w0 + cache[5] * w1;
    out[2] = cache[2] * w0 + cache[6] * w1;
    out[3] = cache[7] * w1 + cache[3] * w0;
    spent += 42;
}

}   // namespace

bool native_matrix_scale(Cpu &c) {
    const std::uint64_t op = c.r[6].d[0];
    if (op > 2ull) return false;
    const u32 m_at = c.r[4].w[0], s_at = c.r[5].w[0];
    u8 *mp = range(m_at, 64u);
    const u8 *sp = range(s_at, 12u);
    if (!mp || !sp) return false;
    float s[3];
    std::memcpy(s, sp, 12u);
    rw_matrix_scale_impl(mp, s, static_cast<u32>(op));
    c.budget -= (op == 0ull) ? 45 : (op == 1ull) ? 52 : 61;
    set32(c.r[2], m_at);
    return true;
}

// RwMatrixRotate (0x004C31B0): builds axis-angle rotation matrix (op 0=replace, 1=preconcat, 2=postconcat).
bool native_rw_matrix_rotate(Cpu &c) {
    const std::uint64_t op = c.r[6].d[0];
    if (op > 2ull || c.vu0.vf[0].w[3] != fp::kOne) return false;
    const u32 m_at = c.r[4].w[0], ax_at = c.r[5].w[0];
    if ((m_at & 15u) || (ax_at & 3u)) return false;
    u8 *mp = range(m_at, 64u);
    const u8 *axp = range(ax_at, 12u);
    if (!mp || !axp) return false;
    float axis[3];
    std::memcpy(axis, axp, 12u);
    std::int32_t spent = 0;
    if (!rw_matrix_rotate_impl(c, mp, axis, std::bit_cast<float>(c.f[12]), static_cast<u32>(op),
                               c.vu0.vf[10].w[3], c.vu0.vf[11].w[3], c.vu0.vf[12].w[3], spent))
        return false;
    set32(c.r[2], m_at);
    c.budget -= spent;
    return true;
}

// Fast internal RwV3dNormalize (0x004C6970): normalizes a1 -> a0, returns 1/len (or 0 if len <= 0) in f0.
bool native_rw_v3d_normalize_fast(Cpu &c) {
    const u32 dst_at = c.r[4].w[0], src_at = c.r[5].w[0];
    if ((dst_at | src_at) & 3u) return false;
    u8 *dp = range(dst_at, 12u);
    const u8 *sp = range(src_at, 12u);
    if (!dp || !sp) return false;
    const auto *s = reinterpret_cast<const Float *>(sp);
    auto *d = reinterpret_cast<Float *>(dp);
    const float x = s[0], y = s[1], z = s[2];
    const float len = hw_sqrtf((y * y + x * x) + z * z);
    const bool zero = fp::key(std::bit_cast<u32>(len)) <= fp::key(0u);
    const float inv = zero ? len : divide(c, 1.0f, len);
    d[0] = x * inv;
    d[1] = y * inv;
    d[2] = z * inv;
    c.f[0] = std::bit_cast<u32>(inv);
    c.budget -= zero ? 22 : 29;
    return true;
}

// Public RwV3dNormalize (0x004C69F0): normalizes a1 -> a0, returns len in f0, sets RW error on zero vector.
bool native_rw_v3d_normalize(Cpu &c) {
    constexpr u32 gp = 0x007D2CF0u;
    if (c.r[28].w[0] != gp) return false;
    const u32 dst_at = c.r[4].w[0], src_at = c.r[5].w[0];
    if ((dst_at | src_at) & 3u) return false;
    u8 *dp = range(dst_at, 12u);
    const u8 *sp = range(src_at, 12u);
    if (!dp || !sp) return false;
    float in_v[3], out_v[3];
    std::memcpy(in_v, sp, 12u);
    std::int32_t spent = 0;
    const float len = rw_v3d_normalize_impl(c, out_v, in_v, true, spent);
    std::memcpy(dp, out_v, 12u);
    c.f[0] = std::bit_cast<u32>(len);
    c.budget -= spent;
    return true;
}

// Public RwV3dTransformPoint (0x004C6700) and RwV3dTransformVector (0x004C6840): transform a1 by matrix a2 -> a0.
bool native_rw_v3d_transform_point(Cpu &c) {
    const u32 dst_at = c.r[4].w[0], src_at = c.r[5].w[0], mat_at = c.r[6].w[0];
    if ((dst_at | src_at | mat_at) & 3u) return false;
    u8 *dp = range(dst_at, 12u);
    const u8 *sp = range(src_at, 12u);
    const u8 *mp = range(mat_at, 60u);
    if (!dp || !sp || !mp) return false;
    const auto *s = reinterpret_cast<const Float *>(sp);
    const auto *m = reinterpret_cast<const Float *>(mp);
    const float x = s[0], y = s[1], z = s[2];
    auto *d = reinterpret_cast<Float *>(dp);
    d[0] = ((x * m[0] + y * m[4]) + z * m[8]) + m[12];
    d[1] = ((x * m[1] + y * m[5]) + z * m[9]) + m[13];
    d[2] = ((x * m[2] + y * m[6]) + z * m[10]) + m[14];
    set32(c.r[2], dst_at);
    c.budget -= 35;
    return true;
}

bool native_rw_v3d_transform_vector(Cpu &c) {
    const u32 dst_at = c.r[4].w[0], src_at = c.r[5].w[0], mat_at = c.r[6].w[0];
    if ((dst_at | src_at | mat_at) & 3u) return false;
    u8 *dp = range(dst_at, 12u);
    const u8 *sp = range(src_at, 12u);
    const u8 *mp = range(mat_at, 44u);
    if (!dp || !sp || !mp) return false;
    const auto *s = reinterpret_cast<const Float *>(sp);
    const auto *m = reinterpret_cast<const Float *>(mp);
    const float x = s[0], y = s[1], z = s[2];
    auto *d = reinterpret_cast<Float *>(dp);
    d[0] = (x * m[0] + y * m[4]) + z * m[8];
    d[1] = (x * m[1] + y * m[5]) + z * m[9];
    d[2] = (x * m[2] + y * m[6]) + z * m[10];
    set32(c.r[2], dst_at);
    c.budget -= 28;
    return true;
}

// PS2 SDK cosf (0x0052E6D8) and sinf (0x0052E878) for |x| <= 128*pi/2 (falls back on huge angles / NaN).
bool native_cosf(Cpu &c) {
    float out;
    if (!ps2_cosf(std::bit_cast<float>(c.f[12]), out)) return false;
    c.f[0] = std::bit_cast<u32>(out);
    c.budget -= 90;
    return true;
}

bool native_sinf(Cpu &c) {
    float out;
    if (!ps2_sinf(std::bit_cast<float>(c.f[12]), out)) return false;
    c.f[0] = std::bit_cast<u32>(out);
    c.budget -= 90;
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

namespace {

[[gnu::noinline, gnu::cold]] void scale_mat_3x3(Float *lf, const float *scale) {
    for (int r = 0; r < 3; ++r)
        for (int i = 0; i < 3; ++i) lf[r * 4 + i] = lf[r * 4 + i] * scale[r];
}

}   // namespace

// The game's own keyframe callback (0x00312C70): the one it replaced, with one bone's position scaled before it (and the scaled
// position kept) and one bone's matrix scaled after it. The bones and the scale are in three words at 0x009571B4.
struct BoneCallback {
    u32 scaled_pos = 0u, scaled_mat = 0u;   // the keyframe addresses, 0 for none
    float scale[3];
    static bool plain(u32 cb) { return cb == 0x00464760u || cb == 0x004F7470u; }
    bool known(u32 cb) {
        if (plain(cb)) return true;
        if (cb != 0x00312C70u) return false;
        const u8 *g = g_ram + 0x009571B0u;
        if (!plain(word(g))) return false;
        scaled_mat = word(g + 4u);
        scaled_pos = word(g + 8u);
        if (scaled_mat | scaled_pos) {
            const u8 *v = range(word(g + 12u), 12u);
            if (!v || (word(g + 12u) & 3u)) return false;
            std::memcpy(scale, v, 12u);
        }
        return true;
    }
    // The local matrix of the keyframe at kf; rows' last words are left alone but for the flags.
    void matrix(float *lm, u8 *kf, u32 kf_at) const {
        auto *k = reinterpret_cast<Float *>(kf);
        if (kf_at == scaled_pos) [[unlikely]]
            for (int i = 0; i < 3; ++i) k[6 + i] = k[6 + i] * scale[i];
        const float x = k[2], y = k[3], z = k[4], w = k[5];
        const float xx = x * x, yy = y * y, zz = z * z;
        const float yz = y * z, zx = z * x, xy = x * y;
        const float wz = w * z, wx = w * x, wy = w * y;
        auto *lf = reinterpret_cast<Float *>(lm);
        lf[0] = 1.0f - 2.0f * (yy + zz);
        lf[1] = 2.0f * (xy + wz);
        lf[2] = 2.0f * (zx - wy);
        lf[4] = 2.0f * (xy - wz);
        lf[5] = 1.0f - 2.0f * (xx + zz);
        lf[6] = 2.0f * (yz + wx);
        lf[8] = 2.0f * (zx + wy);
        lf[9] = 2.0f * (yz - wx);
        lf[10] = 1.0f - 2.0f * (xx + yy);
        lf[12] = k[6];
        lf[13] = k[7];
        lf[14] = k[8];
        u32 flags = 3u;
        if (kf_at == scaled_mat) [[unlikely]] {
            scale_mat_3x3(lf, scale);
            flags = 0u;
        }
        std::memcpy(lf + 3, &flags, 4u);
    }
};

namespace {

[[gnu::always_inline]] inline void make_ident_mat(u8 *m) {
    auto *w = reinterpret_cast<u32 *>(m);
    w[0] = 0x3F800000u; w[1] = 0u; w[2] = 0u; w[3] = 0x00020003u;
    w[4] = 0u; w[5] = 0x3F800000u; w[6] = 0u; w[7] = 0u;
    w[8] = 0u; w[9] = 0u; w[10] = 0x3F800000u; w[11] = 0u;
    w[12] = 0u; w[13] = 0u; w[14] = 0u; w[15] = 0u;
}

[[gnu::always_inline]] inline void mat_mul_vu0(u8 *to_b, const u8 *a_b, const u8 *b_b, u32 w10, u32 w11, u32 w12) {
    const auto *a = reinterpret_cast<const Float *>(a_b);
    const auto *b = reinterpret_cast<const Float *>(b_b);
    auto *r = reinterpret_cast<Float *>(to_b);
    const u32 fl = word(a_b + 12u) & word(b_b + 12u);
    product_row<3, false>(r, a, b);
    product_row<3, false>(r + 4, a + 4, b);
    product_row<3, false>(r + 8, a + 8, b);
    product_row<3, true>(r + 12, a + 12, b);
    put_w(to_b + 12u, fl);
    put_w(to_b + 28u, w10);
    put_w(to_b + 44u, w11);
    put_w(to_b + 60u, w12);
}

[[gnu::noinline]] void mat_mul_vu0_cold(u8 *to_b, const u8 *a_b, const u8 *b_b, u32 w10, u32 w11, u32 w12) {
    mat_mul_vu0(to_b, a_b, b_b, w10, w11, w12);
}

[[gnu::noinline]] bool resolve_root_ltm(u32 ltm_frame, u8 *root_mat, u32 vf10_w, u32 vf11_w, u32 vf12_w, std::int32_t &spent) {
    u32 par = 0u;
    if (ltm_frame != 0u) {
        if ((ltm_frame & 3u) || !ram_ptr(ltm_frame, 8u)) return false;
        par = word(g_ram + ltm_frame + 4u);
    }
    if (par == 0u) {
        make_ident_mat(root_mat);
        spent += 25;
        return true;
    }
    if ((par & 3u) || !ram_ptr(par, 164u)) return false;
    const u32 par_root = word(g_ram + par + 160u);
    if ((par_root & 3u) || !ram_ptr(par_root, 4u)) return false;
    if ((g_ram[par_root + 3u] & 3u) == 0u) {
        std::memcpy(root_mat, g_ram + par + 80u, 64u);
        spent += 75;
        return true;
    }
    std::memcpy(root_mat, g_ram + par + 16u, 64u);
    spent += 70;
    u32 f = word(g_ram + par + 4u);
    int depth = 0;
    while (f != 0u) {
        if ((f & 3u) || !ram_ptr(f, 80u) || ++depth > 64) return false;
        alignas(16) u8 tmp[64];
        std::memcpy(tmp, root_mat, 64u);
        mat_mul_vu0(root_mat, tmp, g_ram + f + 16u, vf10_w, vf11_w, vf12_w);
        f = word(g_ram + f + 4u);
        spent += 95;
    }
    return true;
}

[[gnu::always_inline]] inline void quat_to_mat_look(Cpu &c, u8 *dst_b, const u8 *q_b) {
    const auto *qf = reinterpret_cast<const Float *>(q_b);
    const float qx = qf[0], qy = qf[1], qz = qf[2], qw = qf[3];
    const float norm2 = ((qy * qy + qx * qx) + qz * qz) + qw * qw;
    const float s = divide(c, 2.0f, norm2);
    const float xs = qx * s, ys = qy * s, zs = qz * s;
    const float wx = xs * qw, wy = ys * qw, wz = zs * qw;
    const float xx = qx * xs, yy = qy * ys, zz = qz * zs;
    const float yz = qy * zs, zx = qz * xs, xy = qx * ys;
    auto *df = reinterpret_cast<Float *>(dst_b);
    df[0] = 1.0f - (yy + zz);
    df[1] = xy + wz;
    df[2] = zx - wy;
    put_w(dst_b + 12u, 3u);
    df[4] = xy - wz;
    df[5] = 1.0f - (zz + xx);
    df[6] = yz + wx;
    df[8] = zx + wy;
    df[9] = yz - wx;
    df[10] = 1.0f - (xx + yy);
    df[12] = 0.0f; df[13] = 0.0f; df[14] = 0.0f;
}

// Cold-separated bone 5001 handler for 0x00311730 (runs at most once per hierarchy so the 30-bone hot loop stays tiny).
[[gnu::noinline]] void mdl_look_bone_5001(Cpu &c, u8 *out_mat, const u8 *local_mat, const u8 *parent_mat,
                                          const u8 *root_mat, u8 *look, const u8 *mdl_flags, const u8 *extra_mat,
                                          u32 inv_gp_flags, u32 pf5001, u32 vf10_w, u32 vf11_w, u32 vf12_w,
                                          std::int32_t &spent) {
    if (extra_mat == nullptr) {
        std::uint16_t lfl = half_u16(look);
        if (!(lfl & 1024u)) {
            lfl = static_cast<std::uint16_t>(lfl | 1536u);
            std::memcpy(look, &lfl, 2u);
        }
    }
    const auto *lf = reinterpret_cast<const Float *>(look);
    const bool mdl_scaled = (half_u16(mdl_flags) & 16u) != 0u;
    float inv_s[3] = {divide(c, 1.0f, lf[12]), divide(c, 1.0f, lf[13]), divide(c, 1.0f, lf[14])};
    if (mdl_scaled) {
        const float g0 = *reinterpret_cast<const Float *>(g_ram + 0x009571C0u);
        const float g1 = *reinterpret_cast<const Float *>(g_ram + 0x009571C4u);
        const float inv_g0 = divide(c, 1.0f, g0);
        const float inv_g01 = inv_g0 * g1;
        inv_s[0] *= inv_g0;
        inv_s[1] *= inv_g01;
        inv_s[2] *= inv_g01;
    }
    alignas(16) u8 tmp_parent[64];
    std::memcpy(tmp_parent, parent_mat, 64u);
    rw_matrix_scale_impl(tmp_parent, inv_s, 1u);
    alignas(16) u8 m400[64];
    mat_mul_vu0(m400, local_mat, tmp_parent, vf10_w, vf11_w, vf12_w);
    const float pos[3] = {
        *reinterpret_cast<const Float *>(m400 + 48u),
        *reinterpret_cast<const Float *>(m400 + 52u),
        *reinterpret_cast<const Float *>(m400 + 56u),
    };
    spent += 180;

    if (extra_mat == nullptr) {
        rw_matrix_scale_impl(m400, inv_s, 1u);
        alignas(16) u8 m528[64];
        static constexpr float kAxisY[3] = {0.0f, 1.0f, 0.0f};
        static constexpr float kAxisZ[3] = {0.0f, 0.0f, 1.0f};
        rw_matrix_rotate_impl(c, m528, kAxisY, 180.0f, 0u, vf10_w, vf11_w, vf12_w, spent);
        rw_matrix_rotate_impl(c, m528, kAxisZ, -90.0f, 2u, vf10_w, vf11_w, vf12_w, spent);
        alignas(16) u8 m592[64];
        mat_mul_vu0(m592, m528, root_mat, vf10_w, vf11_w, vf12_w);

        float q384[4];
        u32 sp256 = 0u;
        std::uint16_t lfl = half_u16(look);
        if (lfl & 256u) {
            rt_quat_from_matrix_impl(c, q384, reinterpret_cast<const Float *>(m400), spent);
        } else {
            bool clamped = false;
            if (lfl & 128u) {
                sp256 = 0u;
                static constexpr float kAxisX[3] = {1.0f, 0.0f, 0.0f};
                rw_matrix_rotate_impl(c, m528, kAxisX, lf[15], 0u, vf10_w, vf11_w, vf12_w, spent);
                rw_matrix_rotate_impl(c, m528, kAxisY, lf[16], 2u, vf10_w, vf11_w, vf12_w, spent);
            } else {
                sp256 = 1u;
                float v1136[3];
                if (lfl & 64u) {
                    v1136[0] = pos[0] - lf[15];
                    v1136[1] = pos[1] - lf[16];
                    v1136[2] = pos[2] - lf[17];
                } else {
                    v1136[0] = from_bits(std::bit_cast<u32>(lf[15]) ^ fp::kSign);
                    v1136[1] = from_bits(std::bit_cast<u32>(lf[16]) ^ fp::kSign);
                    v1136[2] = from_bits(std::bit_cast<u32>(lf[17]) ^ fp::kSign);
                }
                rw_v3d_normalize_impl(c, v1136, v1136, true, spent);

                alignas(16) u8 m464[64];
                const u32 rfl = word(root_mat + 12u);
                if ((inv_gp_flags & rfl & 0x00020000u) != 0u) {
                    std::memcpy(m464, root_mat, 64u);
                    spent += 80;
                } else {
                    const auto *s = reinterpret_cast<const Float *>(root_mat);
                    auto *d = reinterpret_cast<Float *>(m464);
                    const float r0 = s[0], r1 = s[1], r2 = s[2];
                    const float u0 = s[4], u1 = s[5], u2 = s[6];
                    const float a0 = s[8], a1 = s[9], a2 = s[10];
                    const float p0 = s[12], p1 = s[13], p2 = s[14];
                    d[0] = r0; d[1] = u0; d[2] = a0; put_w(m464 + 12u, 3u);
                    d[4] = r1; d[5] = u1; d[6] = a1;
                    d[8] = r2; d[9] = u2; d[10] = a2;
                    d[12] = -((p1 * r1 + p0 * r0) + p2 * r2);
                    d[13] = -((p1 * u1 + p0 * u0) + p2 * u2);
                    d[14] = -((p1 * a1 + p0 * a0) + p2 * a2);
                    spent += 59;
                }
                const auto *m464_f = reinterpret_cast<const Float *>(m464);
                rw_v3d_transform_vector_impl(v1136, v1136, m464_f);
                rw_v3d_normalize_impl(c, v1136, v1136, true, spent);

                float v1104[3] = {0.0f, 0.0f, -100.0f};
                rw_v3d_transform_point_impl(v1104, v1104, reinterpret_cast<const Float *>(tmp_parent));
                float v1120[3] = {pos[0] - v1104[0], pos[1] - v1104[1], pos[2] - v1104[2]};
                rw_v3d_transform_vector_impl(v1120, v1120, m464_f);
                rw_v3d_normalize_impl(c, v1120, v1120, true, spent);

                const float rad2deg = from_bits(0x42652EE1u);
                const float f20 = rad2deg * ps2_acosf(c, v1120[1]) - 90.0f;
                const float f21 = 180.0f + rad2deg * ps2_atan2f(c, v1120[0], v1120[2]);
                float f23 = rad2deg * ps2_acosf(c, v1136[1]) - 90.0f;
                float f22 = 180.0f + rad2deg * ps2_atan2f(c, v1136[0], v1136[2]);

                f23 -= f20;
                while (fp::key(std::bit_cast<u32>(f23)) < fp::key(0u)) f23 += 360.0f;
                while (!(fp::key(std::bit_cast<u32>(f23)) <= fp::key(0x43B40000u))) f23 -= 360.0f;
                const float lim_p = lf[2];
                if (!(fp::key(std::bit_cast<u32>(f23)) <= fp::key(std::bit_cast<u32>(lim_p)))) {
                    const float hi_p = 360.0f - lim_p;
                    if (fp::key(std::bit_cast<u32>(f23)) < fp::key(std::bit_cast<u32>(hi_p))) {
                        f23 = (fp::key(std::bit_cast<u32>(f23)) < fp::key(0x43340000u)) ? lim_p : hi_p;
                        clamped = true;
                    }
                }
                const float ang_p = from_bits(std::bit_cast<u32>(f23 + f20) ^ fp::kSign);
                rw_matrix_rotate_impl(c, m528, reinterpret_cast<const Float *>(m592 + 16u), ang_p, 0u, vf10_w, vf11_w, vf12_w, spent);

                f22 -= f21;
                while (fp::key(std::bit_cast<u32>(f22)) < fp::key(0u)) f22 += 360.0f;
                while (!(fp::key(std::bit_cast<u32>(f22)) <= fp::key(0x43B40000u))) f22 -= 360.0f;
                const float lim_y = lf[3];
                if (!(fp::key(std::bit_cast<u32>(f22)) <= fp::key(std::bit_cast<u32>(lim_y)))) {
                    const float hi_y = 360.0f - lim_y;
                    if (fp::key(std::bit_cast<u32>(f22)) < fp::key(std::bit_cast<u32>(hi_y))) {
                        f22 = (fp::key(std::bit_cast<u32>(f22)) < fp::key(0x43340000u)) ? lim_y : hi_y;
                        clamped = true;
                    }
                }
                rw_matrix_rotate_impl(c, m528, reinterpret_cast<const Float *>(m592), f22 + f21, 2u, vf10_w, vf11_w, vf12_w, spent);
                spent += 520;
            }
            mat_mul_vu0(out_mat, m592, m528, vf10_w, vf11_w, vf12_w);
            rt_quat_from_matrix_impl(c, q384, reinterpret_cast<const Float *>(out_mat), spent);
            lfl = half_u16(look);
            if ((lfl & 4096u) && clamped) {
                if (!(lfl & 2048u)) std::memcpy(look + 32u, q384, 16u);
                lfl = static_cast<std::uint16_t>(lfl | 2048u);
                std::memcpy(look, &lfl, 2u);
            } else {
                lfl = static_cast<std::uint16_t>(lfl & 63487u);
                std::memcpy(look, &lfl, 2u);
            }
            if (lfl & 2048u) std::memcpy(q384, look + 32u, 16u);
        }

        lfl = half_u16(look);
        if (lfl & 512u) {
            float q_init[4];
            rt_quat_from_matrix_impl(c, q_init, reinterpret_cast<const Float *>(m400), spent);
            std::memcpy(look + 16u, q_init, 16u);
            lfl = static_cast<std::uint16_t>(lfl & 65023u);
            if (lfl & 256u) lfl = static_cast<std::uint16_t>(lfl & 65055u & 64511u & 63487u);
            std::memcpy(look, &lfl, 2u);
        } else {
            if (lfl & 256u) {
                float dot = ((lf[5] * q384[1] + lf[4] * q384[0]) + lf[6] * q384[2]) + lf[7] * q384[3];
                if (fp::key(std::bit_cast<u32>(dot)) < fp::key(0u)) {
                    const float nx = from_bits(std::bit_cast<u32>(q384[0]) ^ fp::kSign);
                    const float ny = from_bits(std::bit_cast<u32>(q384[1]) ^ fp::kSign);
                    const float nz = from_bits(std::bit_cast<u32>(q384[2]) ^ fp::kSign);
                    const float nw = from_bits(std::bit_cast<u32>(q384[3]) ^ fp::kSign);
                    dot = ((lf[5] * ny + lf[4] * nx) + lf[6] * nz) + lf[7] * nw;
                }
                const float ac = ps2_acosf(c, dot);
                if (fp::key(std::bit_cast<u32>(2.0f * ac)) < fp::key(0x3DB2B8C2u)) {
                    lfl = static_cast<std::uint16_t>(lfl & 65055u & 64511u);
                }
                lfl = static_cast<std::uint16_t>(lfl & 63487u);
                std::memcpy(look, &lfl, 2u);
                spent += 120;
            }
            float q304[4], cache336[9], q320[4];
            std::memcpy(q304, look + 16u, 16u);
            u32 use_lerp = 0u;
            rt_quat_slerp_cache_impl(c, q304, q384, cache336, use_lerp, spent);
            rt_quat_slerp_eval(q320, q304, q384, cache336, use_lerp, lf[1], spent);
            if ((half_u16(look) & 8192u) && sp256 != 0u) {
                float q288[4];
                rt_quat_from_matrix_impl(c, q288, reinterpret_cast<const Float *>(m400), spent);
                float dot = ((q288[1] * q320[1] + q288[0] * q320[0]) + q288[2] * q320[2]) + q288[3] * q320[3];
                if (fp::key(std::bit_cast<u32>(dot)) < fp::key(0u)) {
                    const float nx = from_bits(std::bit_cast<u32>(q320[0]) ^ fp::kSign);
                    const float ny = from_bits(std::bit_cast<u32>(q320[1]) ^ fp::kSign);
                    const float nz = from_bits(std::bit_cast<u32>(q320[2]) ^ fp::kSign);
                    const float nw = from_bits(std::bit_cast<u32>(q320[3]) ^ fp::kSign);
                    dot = ((q288[1] * ny + q288[0] * nx) + q288[2] * nz) + q288[3] * nw;
                }
                const float ang = 2.0f * ps2_acosf(c, dot);
                const float max_ang = from_bits(0x3C8EFA35u) * lf[3];
                if (fp::key(std::bit_cast<u32>(ang)) <= fp::key(std::bit_cast<u32>(max_ang))) {
                    std::memcpy(look + 16u, q320, 16u);
                } else {
                    const float t2 = 1.0f - divide(c, from_bits(0x3FB2B8C2u), ang);
                    u32 use_lerp2 = 0u;
                    rt_quat_slerp_cache_impl(c, q320, q288, cache336, use_lerp2, spent);
                    float q_clamped[4];
                    rt_quat_slerp_eval(q_clamped, q320, q288, cache336, use_lerp2, t2, spent);
                    std::memcpy(look + 16u, q_clamped, 16u);
                }
                spent += 120;
            } else {
                std::memcpy(look + 16u, q320, 16u);
            }
        }

        quat_to_mat_look(c, out_mat, look + 16u);
        float fwd_s[3] = {lf[12], lf[13], lf[14]};
        if (mdl_scaled) {
            const float g0 = *reinterpret_cast<const Float *>(g_ram + 0x009571C0u);
            const float g1 = *reinterpret_cast<const Float *>(g_ram + 0x009571C4u);
            const float g01 = g0 * g1;
            fwd_s[0] *= g0;
            fwd_s[1] *= g01;
            fwd_s[2] *= g01;
        }
        rw_matrix_scale_impl(out_mat, fwd_s, 1u);
        std::memcpy(out_mat + 48u, pos, 12u);
        spent += 130;
    } else {
        quat_to_mat_look(c, m400, look + 16u);
        float fwd_s[3] = {lf[12], lf[13], lf[14]};
        if (mdl_scaled) {
            const float g0 = *reinterpret_cast<const Float *>(g_ram + 0x009571C0u);
            const float g1 = *reinterpret_cast<const Float *>(g_ram + 0x009571C4u);
            const float g01 = g0 * g1;
            fwd_s[0] *= g0;
            fwd_s[1] *= g01;
            fwd_s[2] *= g01;
        }
        rw_matrix_scale_impl(m400, fwd_s, 1u);
        std::memcpy(m400 + 48u, pos, 12u);
        mat_mul_vu0(out_mat, m400, extra_mat, vf10_w, vf11_w, vf12_w);
        spent += 160;
    }

    if (pf5001 != 0u) {
        const u32 par5001 = word(g_ram + pf5001 + 4u);
        if (par5001 != 0u) {
            const u32 par_root = word(g_ram + par5001 + 160u);
            if (g_ram[par_root + 3u] & 1u) {
                sync_hierarchy_root_impl(g_ram + par_root, spent);
            }
        }
    }
}

}   // namespace

// 0x00466EF0 (RpHAnimHierarchyUpdateMatrices): walks bone hierarchy, converts quaternion+pos keyframes to matrices,
// multiplies by parent matrix stack, and updates attached RwFrames without per-bone call/spill overhead.
bool native_hanim_update_matrices(Cpu &c) {
    const u32 h_at = c.r[4].w[0];
    if ((h_at & 3u) || c.vu0.vf[0].w[3] != fp::kOne) return false;
    const u8 *h_p = ram_ptr(h_at, 36u);
    if (!h_p) return false;
    const u32 flags = word(h_p);
    if (flags & 2u) return false;   // sub-hierarchy fallback path (0x00466730)

    const std::int32_t num_nodes = static_cast<std::int32_t>(word(h_p + 4u));
    const u32 out_mats_at = word(h_p + 8u);
    const u32 node_info_at = word(h_p + 16u);
    const u32 interp_at = word(h_p + 32u);
    if ((interp_at & 3u) || !ram_ptr(interp_at, 76u)) return false;
    const u32 cb = word(g_ram + interp_at + 60u);
    const u32 kf_step = word(g_ram + interp_at + 36u);
    BoneCallback bone;
    if (num_nodes > 0) {
        if (!bone.known(cb) || num_nodes > 256) return false;
        if ((out_mats_at & 15u) || (node_info_at & 3u) || ((interp_at + 76u) & 3u) || (kf_step & 3u)) return false;
        if (!ram_ptr(out_mats_at, static_cast<u32>(num_nodes) * 64u)) return false;
        if (!ram_ptr(node_info_at, static_cast<u32>(num_nodes) * 16u)) return false;
        if (!ram_ptr(interp_at + 76u, static_cast<u32>(num_nodes - 1) * kf_step + 36u)) return false;
    }

    const bool update_ltm = (flags & 8192u) != 0u;
    const bool update_mod = (flags & 4096u) != 0u;
    const bool local_space = (flags & 16384u) != 0u;

    // Pre-validate root frame if update_ltm will touch it at L_004670D0.
    u32 h_rf_at = 0u, h_rf_root = 0u;
    if (update_ltm) {
        h_rf_at = word(h_p + 20u);
        if ((h_rf_at & 3u) || !ram_ptr(h_rf_at, 164u)) return false;
        h_rf_root = word(g_ram + h_rf_at + 160u);
        if ((h_rf_root & 3u) || !ram_ptr(h_rf_root, 16u)) return false;
        if ((g_ram[h_rf_root + 3u] & 3u) == 0u) {
            const u32 first = word(g_ram + 0x0096012Cu);
            if ((first & 3u) || !ram_ptr(first, 8u)) return false;
        }
    }

    alignas(16) u8 root_mat[64];
    alignas(16) u8 ident_mat[64];
    const u32 vf10_w = c.vu0.vf[10].w[3], vf11_w = c.vu0.vf[11].w[3], vf12_w = c.vu0.vf[12].w[3];

    bool need_root_ltm = false;
    u32 ltm_frame = 0u;
    const u8 *parent_mat = nullptr;

    if ((flags & 1u) && word(h_p + 28u) != 0xFFFFFFFFu) {
        const u32 p_idx = word(h_p + 28u);
        const u32 p_h_at = word(h_p + 24u);
        if ((p_h_at & 3u) || !ram_ptr(p_h_at, 24u)) return false;
        const u32 pm_at = word(g_ram + p_h_at + 8u) + (p_idx << 6);
        if ((pm_at & 15u) || !ram_ptr(pm_at, 64u)) return false;
        parent_mat = g_ram + pm_at;
        if (update_ltm && local_space) {
            ltm_frame = word(g_ram + p_h_at + 20u);
            need_root_ltm = true;
        }
    } else if (local_space) {
        make_ident_mat(ident_mat);
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
    if (need_root_ltm && !resolve_root_ltm(ltm_frame, root_mat, vf10_w, vf11_w, vf12_w, spent)) return false;

    // Pre-validate attached node frames before any writes to guest memory.
    if (num_nodes > 0 && (update_mod || update_ltm)) {
        for (std::int32_t i = 0; i < num_nodes; ++i) {
            const u32 pf = word(g_ram + node_info_at + static_cast<u32>(i) * 16u + 12u);
            if (pf != 0u) {
                if ((pf & 3u) || !ram_ptr(pf, 164u)) return false;
                if (update_mod && !update_ltm) {
                    const u32 pr = word(g_ram + pf + 160u);
                    if ((pr & 3u) || !ram_ptr(pr, 16u)) return false;
                    if ((g_ram[pr + 3u] & 3u) == 0u) {
                        const u32 first = word(g_ram + 0x0096012Cu);
                        if ((first & 3u) || !ram_ptr(first, 8u)) return false;
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
            put_w(g_ram + node_at, first);
            put_w(g_ram + h_rf_root + 12u, head);
            put_w(g_ram + first + 4u, node_at);
            put_w(g_ram + head, node_at);
            g_ram[h_rf_root + 3u] = static_cast<u8>(rfl | 2u);
            spent += 25;
        }
    }

    const u8 *mat_stack[64];
    int sp_idx = 0;
    alignas(16) u8 local_mat[64];
    {
        auto *lw = reinterpret_cast<u32 *>(local_mat);
        lw[3] = 3u; lw[7] = 0u; lw[11] = 0u; lw[15] = 0u;
    }

    u8 *kf = g_ram + interp_at + 76u;
    u8 *out_mat = g_ram + out_mats_at;
    const u8 *node = g_ram + node_info_at;

    for (std::int32_t i = 0; i < num_nodes; ++i, kf += kf_step, out_mat += 64u, node += 16u) {
        bone.matrix(reinterpret_cast<float *>(local_mat), kf, static_cast<u32>(kf - g_ram));
        mat_mul_vu0(out_mat, local_mat, parent_mat, vf10_w, vf11_w, vf12_w);

        const u32 pf = word(node + 12u);
        if (pf != 0u) {
            if (update_mod) {
                std::memcpy(g_ram + pf + 16u, local_mat, 64u);
                if (!update_ltm) rw_frame_update_objects_impl(pf);
            }
            if (update_ltm) {
                if (!local_space) std::memcpy(g_ram + pf + 80u, out_mat, 64u);
                else mat_mul_vu0_cold(g_ram + pf + 80u, out_mat, root_mat, vf10_w, vf11_w, vf12_w);
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

// The bone loop of the game's own copy of that function (0x00311730, L_00311A54), which turns a character's head to look at
// something: it stops for that bone (5001), and the last bone is left to the translated code.
void native_loop_look_bones(Cpu &c) {
    if (c.vu0.vf[0].w[3] != fp::kOne) return;
    const u32 sp = c.r[29].w[0];
    if (sp & 15u) return;
    auto ram = [](u32 a, u32 n) { return a < kRamSize && n <= kRamSize - a; };
    const u32 h_at = c.r[20].w[0];
    if (!ram(sp, 1184u) || !ram(h_at, 36u) || (h_at & 3u)) return;
    u8 *frame = g_ram + sp + 176u;
    const u8 *h = g_ram + h_at;
    const u32 count = word(h + 4u);
    u32 i = c.r[30].w[0];
    if (count > 256u || i + 1u >= count) return;
    BoneCallback bone;
    if (!bone.known(word(frame + 48u))) return;
    const u32 step = word(frame + 32u);
    const bool local_space = word(frame) != 0u, update_mod = word(frame + 16u) != 0u, update_ltm = word(frame + 64u) != 0u;
    const u32 n = count - 1u - i;
    u32 kf_at = c.r[16].w[0], out_at = c.r[17].w[0], parent_at = c.r[18].w[0], stack_at = c.r[22].w[0], node_at = c.r[23].w[0];
    if ((step & 3u) || step > 256u || ((kf_at | node_at | stack_at) & 3u) || ((out_at | parent_at) & 15u)) return;
    if (!ram(kf_at, (n - 1u) * step + 36u) || !ram(out_at, n * 64u) || !ram(node_at, n * 16u) || !ram(parent_at, 64u) || word(h + 16u) + i * 16u != node_at) return;
    const u32 stack_base = sp + 852u, stack_end = sp + 976u;
    const u32 w10 = c.vu0.vf[10].w[3], w11 = c.vu0.vf[11].w[3], w12 = c.vu0.vf[12].w[3];
    const u8 *root = frame + 864u;   // sp + 1040
    alignas(16) u8 local_mat[64];
    std::memcpy(local_mat, frame + 480u, 64u);   // sp + 656
    const u8 *parent = g_ram + parent_at;
    std::int32_t spent = 0;
    u32 done = 0;
    for (; done < n; ++done) {
        const u8 *node = g_ram + node_at;
        if (word(node) == 5001u) break;
        const u32 pf = word(node + 12u), kind = word(node + 8u) & 3u;
        if (pf != 0u) {
            if ((pf & 3u) || !ram(pf, 164u)) break;
            if (update_mod && !update_ltm) {
                const u32 pr = word(g_ram + pf + 160u);
                if ((pr & 3u) || !ram(pr, 16u)) break;
                if ((g_ram[pr + 3u] & 3u) == 0u) {
                    const u32 first = word(g_ram + 0x0096012Cu);
                    if ((first & 3u) || !ram(first, 8u)) break;
                }
            }
        }
        u32 next_parent = parent_at;
        if (kind == 2u) {
            if (stack_at < stack_base || stack_at >= stack_end) break;
            next_parent = out_at;
        } else if (kind == 1u) {
            if (stack_at <= stack_base || stack_at > stack_end) break;
            next_parent = word(g_ram + stack_at - 4u);
            if ((next_parent & 15u) || !ram(next_parent, 64u)) break;
        } else if (kind == 0u) {
            next_parent = out_at;
        }
        u8 *out = g_ram + out_at;
        bone.matrix(reinterpret_cast<float *>(local_mat), g_ram + kf_at, kf_at);
        mat_mul_vu0(out, local_mat, parent, w10, w11, w12);
        spent += 135;
        if (pf != 0u) {
            if (update_mod) {
                std::memcpy(g_ram + pf + 16u, local_mat, 64u);
                if (!update_ltm) rw_frame_update_objects_impl(pf);
                spent += 75;
            }
            if (update_ltm) {
                if (!local_space) std::memcpy(g_ram + pf + 80u, out, 64u);
                else mat_mul_vu0_cold(g_ram + pf + 80u, out, root, w10, w11, w12);
                g_ram[pf + 3u] = static_cast<u8>((g_ram[pf + 3u] & ~4u) | 8u);
                spent += 75;
            }
        }
        if (kind == 2u) {
            put_w(g_ram + stack_at, parent_at);
            stack_at += 4u;
        } else if (kind == 1u) {
            stack_at -= 4u;
        }
        parent_at = next_parent;
        parent = g_ram + parent_at;
        kf_at += step;
        out_at += 64u;
        node_at += 16u;
    }
    if (!done) return;
    std::memcpy(frame + 480u, local_mat, 64u);
    set32(c.r[16], kf_at);
    set32(c.r[17], out_at);
    set32(c.r[18], parent_at);
    set32(c.r[22], stack_at);
    set32(c.r[23], node_at);
    set32(c.r[30], i + done);
    c.budget -= spent;
}

// Full mdlSlotUpdateHierarchyLookAt (0x00311730): walks the entire bone hierarchy including head look-at bone 5001
// (active target tracking, fixed yaw/pitch, idle return slerp, and extra_mat override) without stack/call overhead.
bool native_mdl_look_hierarchy(Cpu &c) {
    constexpr u32 gp = 0x007D2CF0u;
    if (c.r[28].w[0] != gp || c.vu0.vf[0].w[3] != fp::kOne) return false;
    const u32 h_at = c.r[4].w[0];
    const u32 mdl_flags_at = c.r[5].w[0];
    const u32 look_at = c.r[6].w[0];
    const u32 extra_mat_at = c.r[7].w[0];
    if (h_at & 3u) return false;
    const u8 *h_p = ram_ptr(h_at, 36u);
    if (!h_p) return false;

    const u32 flags = word(h_p);
    const std::int32_t num_nodes = static_cast<std::int32_t>(word(h_p + 4u));
    const u32 out_mats_at = word(h_p + 8u);
    const u32 node_info_at = word(h_p + 16u);
    const u32 interp_at = word(h_p + 32u);
    if ((interp_at & 3u) || !ram_ptr(interp_at, 76u)) return false;
    const u32 cb = word(g_ram + interp_at + 60u);
    const u32 kf_step = word(g_ram + interp_at + 36u);
    BoneCallback bone;
    if (num_nodes > 0) {
        if (!bone.known(cb) || num_nodes > 256) return false;
        if ((out_mats_at & 15u) || (node_info_at & 3u) || ((interp_at + 76u) & 3u) || (kf_step & 3u)) return false;
        if (!ram_ptr(out_mats_at, static_cast<u32>(num_nodes) * 64u)) return false;
        if (!ram_ptr(node_info_at, static_cast<u32>(num_nodes) * 16u)) return false;
        if (!ram_ptr(interp_at + 76u, static_cast<u32>(num_nodes - 1) * kf_step + 36u)) return false;
    }

    const bool update_ltm = (flags & 8192u) != 0u;
    const bool update_mod = (flags & 4096u) != 0u;
    const bool local_space = (flags & 16384u) != 0u;

    u32 h_rf_at = 0u, h_rf_root = 0u;
    if (update_ltm) {
        h_rf_at = word(h_p + 20u);
        if ((h_rf_at & 3u) || !ram_ptr(h_rf_at, 164u)) return false;
        h_rf_root = word(g_ram + h_rf_at + 160u);
        if ((h_rf_root & 3u) || !ram_ptr(h_rf_root, 16u)) return false;
        if ((g_ram[h_rf_root + 3u] & 3u) == 0u) {
            const u32 first = word(g_ram + 0x0096012Cu);
            if ((first & 3u) || !ram_ptr(first, 8u)) return false;
        }
    }

    alignas(16) u8 root_mat[64];
    alignas(16) u8 ident_mat[64];
    const u32 vf10_w = c.vu0.vf[10].w[3], vf11_w = c.vu0.vf[11].w[3], vf12_w = c.vu0.vf[12].w[3];

    bool need_root_ltm = false;
    u32 ltm_frame = 0u;
    const u8 *parent_mat = nullptr;
    if ((flags & 1u) && word(h_p + 28u) != 0xFFFFFFFFu) {
        const u32 p_idx = word(h_p + 28u);
        const u32 p_h_at = word(h_p + 24u);
        if ((p_h_at & 3u) || !ram_ptr(p_h_at, 24u)) return false;
        const u32 pm_at = word(g_ram + p_h_at + 8u) + (p_idx << 6);
        if ((pm_at & 15u) || !ram_ptr(pm_at, 64u)) return false;
        parent_mat = g_ram + pm_at;
        if (update_ltm && local_space) {
            ltm_frame = word(g_ram + p_h_at + 20u);
            need_root_ltm = true;
        }
    } else if (local_space) {
        make_ident_mat(ident_mat);
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
    if (need_root_ltm && !resolve_root_ltm(ltm_frame, root_mat, vf10_w, vf11_w, vf12_w, spent)) return false;

    bool has_5001 = false;
    u8 *look = nullptr;
    const u8 *mdl_flags = nullptr;
    const u8 *extra_mat = nullptr;
    if (num_nodes > 0) {
        for (std::int32_t i = 0; i < num_nodes; ++i) {
            const u8 *nd = g_ram + node_info_at + static_cast<u32>(i) * 16u;
            const u32 pf = word(nd + 12u);
            if (word(nd) == 5001u) {
                has_5001 = true;
                if (pf != 0u) {
                    if ((pf & 3u) || !ram_ptr(pf, 164u)) return false;
                    const u32 par = word(g_ram + pf + 4u);
                    if (par != 0u) {
                        if ((par & 3u) || !ram_ptr(par, 164u)) return false;
                        const u32 par_root = word(g_ram + par + 160u);
                        if ((par_root & 15u) || !ram_ptr(par_root, 164u)) return false;
                    }
                }
            }
            if (pf != 0u && (update_mod || update_ltm)) {
                if ((pf & 3u) || !ram_ptr(pf, 164u)) return false;
                if (update_mod && !update_ltm) {
                    const u32 pr = word(g_ram + pf + 160u);
                    if ((pr & 3u) || !ram_ptr(pr, 16u)) return false;
                    if ((g_ram[pr + 3u] & 3u) == 0u) {
                        const u32 first = word(g_ram + 0x0096012Cu);
                        if ((first & 3u) || !ram_ptr(first, 8u)) return false;
                    }
                }
            }
        }
    }

    u32 inv_gp_flags = 0u;
    if (has_5001) {
        if ((look_at & 3u) || (mdl_flags_at & 1u)) return false;
        look = ram_ptr(look_at, 72u);
        mdl_flags = ram_ptr(mdl_flags_at, 2u);
        if (!look || !mdl_flags) return false;
        if (extra_mat_at != 0u) {
            if (extra_mat_at & 15u) return false;
            extra_mat = ram_ptr(extra_mat_at, 64u);
            if (!extra_mat) return false;
        } else {
            if (!need_root_ltm) return false;
            const std::uint16_t lfl = half_u16(look);
            if (!(lfl & 256u)) {
                if (lfl & 128u) {
                    float rx, ry;
                    std::memcpy(&rx, look + 60u, 4u);
                    std::memcpy(&ry, look + 64u, 4u);
                    const u32 arx = std::bit_cast<u32>(rx) & 0x7FFFFFFFu;
                    const u32 ary = std::bit_cast<u32>(ry) & 0x7FFFFFFFu;
                    if (arx > 0x46200000u || ary > 0x46200000u) return false;
                } else {
                    const u32 v3d_off = word(g_ram + (gp - 17376u));
                    const u8 *v3d_tbl = ram_ptr(0x00960070u + v3d_off, 24u);
                    if (!v3d_tbl || word(v3d_tbl + 8u) != 0x004C6700u || word(v3d_tbl + 16u) != 0x004C6840u) return false;
                    const u32 mat_off = word(g_ram + (gp - 17432u));
                    const u8 *mat_fl = ram_ptr(0x00960070u + mat_off + 4u, 4u);
                    if (!mat_fl) return false;
                    inv_gp_flags = word(mat_fl);
                    const u32 rfl = word(root_mat + 12u);
                    if (!((inv_gp_flags & rfl & 0x00020000u) != 0u || (rfl & 3u) == 3u)) return false;
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
            put_w(g_ram + node_at, first);
            put_w(g_ram + h_rf_root + 12u, head);
            put_w(g_ram + first + 4u, node_at);
            put_w(g_ram + head, node_at);
            g_ram[h_rf_root + 3u] = static_cast<u8>(rfl | 2u);
            spent += 25;
        }
    }

    const u8 *mat_stack[64];
    int sp_idx = 0;
    alignas(16) u8 local_mat[64];
    {
        auto *lw = reinterpret_cast<u32 *>(local_mat);
        lw[3] = 3u; lw[7] = 0u; lw[11] = 0u; lw[15] = 0u;
    }

    u8 *kf = g_ram + interp_at + 76u;
    u8 *out_mat = g_ram + out_mats_at;
    const u8 *node = g_ram + node_info_at;

    for (std::int32_t i = 0; i < num_nodes; ++i, kf += kf_step, out_mat += 64u, node += 16u) {
        bone.matrix(reinterpret_cast<float *>(local_mat), kf, static_cast<u32>(kf - g_ram));

        if (word(node) != 5001u) [[likely]] {
            mat_mul_vu0(out_mat, local_mat, parent_mat, vf10_w, vf11_w, vf12_w);
            spent += 135;
        } else {
            mdl_look_bone_5001(c, out_mat, local_mat, parent_mat, root_mat, look, mdl_flags, extra_mat,
                               inv_gp_flags, word(node + 12u), vf10_w, vf11_w, vf12_w, spent);
        }

        const u32 pf = word(node + 12u);
        if (pf != 0u) {
            if (update_mod) {
                std::memcpy(g_ram + pf + 16u, local_mat, 64u);
                if (!update_ltm) rw_frame_update_objects_impl(pf);
                spent += 75;
            }
            if (update_ltm) {
                if (!local_space) std::memcpy(g_ram + pf + 80u, out_mat, 64u);
                else mat_mul_vu0_cold(g_ram + pf + 80u, out_mat, root_mat, vf10_w, vf11_w, vf12_w);
                g_ram[pf + 3u] = static_cast<u8>((g_ram[pf + 3u] & ~4u) | 8u);
                spent += 75;
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

    set32(c.r[2], 1u);
    c.budget -= spent + 25;
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
