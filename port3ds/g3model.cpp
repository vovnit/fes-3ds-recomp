// See g3model.hpp. Written from the compiled programs in generated/p3fes-vu3ds (the labels below are VU1 micro addresses of
// program 042; program 064 runs the same output code 0x2D0 bytes later) and from the C-GBL port's notes on the same programs
// (docs/ANALYSIS/31_native_renderer.md there), checked batch by batch against the programs (tools/g3check, "g3 2").
//
// Every float operation is done in the program's own order with the same single roundings, so the packets come out byte for
// byte the same (the VU code on the 3DS is plain IEEE single precision too; build without contracting a*b+c into one rounding).
#include "g3model.hpp"

#ifndef G3_HOST_CHECK   // tools/g3check builds the model alone, without the runtime
#include "ps2/cpu.hpp"
#include "vu_micro.hpp"
#endif

#include <cstring>

#ifndef __3DS__
#include <cmath>
#endif

#ifdef G3_HOST_CHECK
#define G3_PROF_EXTRA(k)
#else
#include "prof.hpp"
#define G3_PROF_EXTRA(k) PS2_PROF_EXTRA(k)
#endif

namespace g3 {

namespace {

using u32 = std::uint32_t;

inline float f(u32 x) {
    float v;
    std::memcpy(&v, &x, 4);
    return v;
}
inline u32 bits(float v) {
    u32 x;
    std::memcpy(&x, &v, 4);
    return x;
}

// fp::min / fp::max of the runtime: the bit patterns compared in integer order.
inline std::int32_t order(u32 x) { return static_cast<std::int32_t>(x ^ (static_cast<u32>(static_cast<std::int32_t>(x) >> 31) >> 1)); }
inline float vmax(float a, float b) { return order(bits(a)) >= order(bits(b)) ? a : b; }
inline float vmin(float a, float b) { return order(bits(a)) <= order(bits(b)) ? a : b; }

// FTOI0 / FTOI4: truncation, saturating as the PS2 (and the 3DS FPU) does.
inline u32 ftoi(float v, int fraction) {
    const float x = v * static_cast<float>(1 << fraction);
#ifdef __3DS__
    return static_cast<u32>(static_cast<std::int32_t>(x));
#else
    if (!(x == x)) return 0;
    if (x >= 2147483647.0f) return 0x7FFFFFFFu;
    if (x <= -2147483648.0f) return 0x80000000u;
    return static_cast<u32>(static_cast<std::int32_t>(x));
#endif
}

// DIV as fp::div: a zero divisor gives the largest float with the quotient's sign.
inline float vdiv(float s, float t) {
    if (t == 0.0f) {
        const u32 sign = (bits(s) ^ bits(t)) & 0x80000000u;
        return f(sign | 0x7F7FFFFFu);
    }
    return s / t;
}

struct Vec { float x, y, z, w; };

inline Vec load(const std::uint8_t *mem, u32 qword) {
    Vec v;
    std::memcpy(&v, mem + (qword & 1023u) * 16u, 16);
    return v;
}

// The sign the MAC flag gives the z of a cross product a x b, computed as the programs do: a.x*b.y, then minus b.x*a.y.
inline bool cross_z_negative(float ax, float ay, float bx, float by) {
    const float p = ax * by;
    const float q = bx * ay;
    return bits(p - q) >> 31;
}

struct Batch {
    const Input &in;
    u32 top, n;
    Vec pos(u32 k) const { return load(in.mem, top + 4u * k); }
    Vec uv(u32 k) const { return load(in.mem, top + 4u * k + 1u); }
    Vec colour(u32 k) const { return load(in.mem, top + 4u * k + 2u); }
    Vec vf(int i) const { return load(in.vf, i); }
};

inline u32 u16_at(const std::uint8_t *mem, u32 qword, u32 byte) { return mem[(qword & 1023u) * 16u + byte] | (mem[(qword & 1023u) * 16u + byte + 1] << 8); }
inline Vec itof(const std::uint8_t *mem, u32 qword) {
    std::int32_t v[4];
    std::memcpy(v, mem + (qword & 1023u) * 16u, 16);
    return Vec{static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]), static_cast<float>(v[3])};
}
inline Vec plus0(Vec v) { return Vec{v.x + 0.0f, v.y + 0.0f, v.z + 0.0f, v.w + 0.0f}; }
inline Vec mul(Vec v, float s) { return Vec{v.x * s, v.y * s, v.z * s, v.w * s}; }
inline Vec madd(Vec acc, Vec v, float s) { return Vec{acc.x + v.x * s, acc.y + v.y * s, acc.z + v.z * s, acc.w + v.w * s}; }
constexpr float normal_unit = 0.0078125f;   // normals come as integers in 1/128 units (LOI 0x3C000000)

// The program works stage by stage over all vertices (convert, each light, clamp, then the strip). Each vertex's result
// depends only on its own inputs, so the model does every stage for one vertex before the next, in the same order of
// operations, and keeps nothing per vertex but the last two screen positions: that keeps it inside the ARM11's data cache.

// The light list at 0x3D4 worked out once per batch (042 0x410-0xD80, 064 0x570-...).
struct Light { bool directional; float ax, ay, az, dx, dy, dz; };
struct Lighting {
    bool lights;
    unsigned count;
    Light l[32];
    Vec mat;
};

// False for what is not modelled yet: point and spot lights, and morphing, which leaves the compiled program.
bool prepare_lighting(const std::uint8_t *mem, Lighting &L) {
    const Vec v3 = load(mem, 0x3FC);
    if (v3.w + 0.0f != 0.0f) return false;
    L.mat = load(mem, 0x3FB);
    L.lights = u16_at(mem, 0x3D4, 12) != 0;
    L.count = 0;
    if (!L.lights) return true;
    const Vec m8 = load(mem, 0x3D0), m9 = load(mem, 0x3D1), m10 = load(mem, 0x3D2), m11 = load(mem, 0x3D3);
    const float v10w = 0.0f - 1.0f * m11.w;
    u32 at = 0x3D4;
    for (;;) {
        if (L.count == 32) return false;
        const u32 type = u16_at(mem, at, 12);
        const Vec lc = load(mem, at), p = load(mem, at + 1u);
        Light &l = L.l[L.count];
        if (type == 2) {   // ambient
            l = Light{false, lc.x * v3.x, lc.y * v3.x, lc.z * v3.x, 0.0f, 0.0f, 0.0f};
            ++L.count;
            at += 1u;
            continue;
        }
        if (type == 1) {   // directional: the direction through the lighting matrix
            float dx = m8.x * p.x, dy = m8.y * p.x, dz = m8.z * p.x;
            dx = dx + m9.x * p.y; dy = dy + m9.y * p.y; dz = dz + m9.z * p.y;
            dx = dx + m10.x * p.z; dy = dy + m10.y * p.z; dz = dz + m10.z * p.z;
            l = Light{true, lc.x * v3.z, lc.y * v3.z, lc.z * v3.z, dx * v10w, dy * v10w, dz * v10w};
            ++L.count;
            at += 2u;
            continue;
        }
        if (type == 0x80 || type == 0x81 || type == 0x82) return false;   // point, spot, soft spot: not yet
        return true;   // anything else ends the list
    }
}

// One vertex's colour: the lights summed in list order, N.L clamped at 0 (its three products summed in the order the
// program's group of four gives vertex k), then the clamp to 255 and the material; without lights only the material.
inline Vec shade(const Lighting &L, u32 k, Vec c, const Vec &nn) {
    const Vec &m = L.mat;
    if (!L.lights) return Vec{c.x * m.x, c.y * m.y, c.z * m.z, c.w * m.w};
    for (unsigned i = 0; i < L.count; ++i) {
        const Light &l = L.l[i];
        if (!l.directional) {
            c.x = c.x + l.ax;
            c.y = c.y + l.ay;
            c.z = c.z + l.az;
            continue;
        }
        const float px = nn.x * l.dx, py = nn.y * l.dy, pz = nn.z * l.dz;
        float d;
        switch (k & 3u) {
        case 0: d = (px + pz) + py; break;
        case 1: d = (py + px) + pz; break;
        case 2: d = (pz + py) + px; break;
        default: d = px + (py + pz); break;
        }
        d = vmax(d, 0.0f);
        c.x = (c.x + 0.0f) + l.ax * d;
        c.y = (c.y + 0.0f) + l.ay * d;
        c.z = (c.z + 0.0f) + l.az * d;
    }
    return Vec{vmin(c.x, 255.0f) * m.x, vmin(c.y, 255.0f) * m.y, vmin(c.z, 255.0f) * m.z, vmin(c.w, 255.0f) * m.w};
}

struct Vertex { Vec pos, uv, colour; };

// g3_generic's vertices: 4 qwords (position, uv, prelit colour and normal as integers).
inline void fetch_generic(const std::uint8_t *mem, u32 top, u32 k, const Lighting &L, Vertex &v) {
    const u32 at = top + 4u * k;
    v.pos = load(mem, at);
    v.uv = load(mem, at + 1u);
    const Vec nn = L.lights ? mul(itof(mem, at + 3u), normal_unit) : Vec{};
    v.colour = shade(L, k, itof(mem, at + 2u), nn);
}

// g3_skin's vertices (0x198-0x3C0, or 0xD90 without lights): 5 qwords, the fifth the four bone weights, each with the bone's
// matrix address in its low 16 bits (rows at 696-699 past it, the fourth the translation). Position and normal go through
// the weighted sum of the four matrices.
inline void fetch_skin(const std::uint8_t *mem, u32 top, u32 k, const Lighting &L, Vertex &v) {
    const u32 at = top + 5u * k;
    Vec pos = load(mem, at), uv = load(mem, at + 1u);
    if (k) {   // the software pipeline copies all but the first vertex with an ADD of zero
        pos = plus0(pos);
        uv = plus0(uv);
    }
    const Vec w = load(mem, at + 4u);
    const u32 bone[4] = {u16_at(mem, at + 4u, 0), u16_at(mem, at + 4u, 4), u16_at(mem, at + 4u, 8), u16_at(mem, at + 4u, 12)};
    Vec row[4];   // 0-2 the rotation rows (696-698), 3 the translation (699)
    for (u32 r = 0; r < 4; ++r) {
        const u32 off = 696u + r;
        Vec a = mul(load(mem, bone[0] + off), w.x);
        a = madd(a, load(mem, bone[1] + off), w.y);
        a = madd(a, load(mem, bone[2] + off), w.z);
        row[r] = madd(a, load(mem, bone[3] + off), w.w);
    }
    Vec p = mul(row[3], 1.0f);
    p = madd(p, row[0], pos.x);
    p = madd(p, row[1], pos.y);
    v.pos = madd(p, row[2], pos.z);
    v.uv = uv;
    Vec nn{};
    if (L.lights) {
        const Vec n0 = mul(itof(mem, at + 3u), normal_unit);
        nn = mul(row[0], n0.x);
        nn = madd(nn, row[1], n0.y);
        nn = madd(nn, row[2], n0.z);
    }
    v.colour = shade(L, k, itof(mem, at + 2u), nn);
}

// The vertices' screen positions without the offset vf02, before FTOI4: the last three are kept for the culling test.
struct Screen { float x, y, z; };

// Whether the triangle ending at vertex k (k >= 2) faces away: the cross product of its two edges, with the order of the
// operands alternating as the strip's winding does (0x1298-0x13F0). s0, s1, s2 are vertices k-2, k-1, k.
inline bool facing_negative(const Screen &s0, const Screen &s1, const Screen &s2, u32 k) {
    if (!(k & 1u)) return cross_z_negative(s1.x - s0.x, s1.y - s0.y, s1.x - s2.x, s1.y - s2.y);
    return cross_z_negative(s2.x - s1.x, s2.y - s1.y, s0.x - s1.x, s0.y - s1.y);
}

inline void put(u32 *&o, u32 a, u32 b, u32 c, u32 d) {
    o[0] = a;
    o[1] = b;
    o[2] = c;
    o[3] = d;
    o += 4;
}

// The strip: with fog (042 0x1150-0x1408) XYZF2 with the fog in w and ADC by adding 2048.5 to the fog before FTOI4; without
// fog (042 0xD90-0xF90) XYZ2 with ADC in the w word as a 16-bit integer. One GIF packet, written to o.
template <bool Skin, bool Fog>
void strip(const Input &in, u32 top, u32 n, const Lighting &L, u32 *o) {
    const std::uint8_t *mem = in.mem;
    const Batch b{in, top, n};
    const Vec v28 = b.vf(28), v29 = b.vf(29), v30 = b.vf(30), v31 = b.vf(31), v2 = b.vf(2);
    const u32 cull = u16_at(mem, 0x3FF, 8);
    const Vec tag = load(mem, 0x3FA);
    put(o, 0x8000u | n, bits(tag.y), bits(tag.z), bits(tag.w));
    // Fog: the matrix with its w column scaled by the fog factor (0x1150 on).
    const Vec fog = load(mem, 0x3F6);
    const float fs = fog.z, near = fog.w;
    float r26x, r26y, r26z, r26w, r23x, r23y, r23w, r24x, r24y, r24w, r25x, r25y, r25z, r25w, fs_near, fog_top, nx, ny, nz, adc_w;
    if (Fog) {
        r26x = v31.x + 0.0f; r26y = v31.y + 0.0f; r26z = v31.z + 0.0f; r26w = v31.w * fs;
        r23x = v28.x + 0.0f; r23y = v28.y + 0.0f; r23w = v28.w * fs;
        r24x = v29.x + 0.0f; r24y = v29.y + 0.0f; r24w = v29.w * fs;
        r25x = v30.x + 0.0f; r25y = v30.y + 0.0f; r25z = v30.z + 0.0f; r25w = v30.w * fs;
        fs_near = near * fs;
        fog_top = 255.0f + near * fs;
        nx = 0.0f - v2.x; ny = 0.0f - v2.y; nz = 0.0f - v2.z;
        adc_w = fs_near - 2048.5f;   // LOI 2048.5 (0x45000800)
    }
    const u32 base = ((cull + 0x7FE0u) | 0x7FE0u) & 0xFFFFu;
    Screen s[3];
    for (u32 k = 0; k < n; ++k) {
        Vertex v;
        if (Skin) fetch_skin(mem, top, k, L, v);
        else fetch_generic(mem, top, k, L, v);
        const Vec &p = v.pos, &t = v.uv, &c = v.colour;
        Screen &sk = s[k % 3u];
        float q;
        u32 xyz[4];
        if (Fog) {
            const float cx = ((r26x + r23x * p.x) + r24x * p.y) + r25x * p.z;
            const float cy = ((r26y + r23y * p.x) + r24y * p.y) + r25y * p.z;
            const float cz = r26z + r25z * p.z;
            const float cw = ((r26w + r23w * p.x) + r24w * p.y) + r25w * p.z;
            q = vdiv(fs, cw);
            const float fogged = vmin(vmax(cw, fs_near), fog_top);
            sk = Screen{cx * q, cy * q, cz * q};
            const bool adc = k >= 2 && (facing_negative(s[(k - 2) % 3u], s[(k - 1) % 3u], sk, k) ? 32u : 0u) != cull;
            xyz[0] = ftoi(sk.x - nx, 4);
            xyz[1] = ftoi(sk.y - ny, 4);
            xyz[2] = ftoi(sk.z - nz, 4);
            xyz[3] = ftoi(fogged - (adc ? adc_w : fs_near), 4);
        } else {
            const float cx = ((v31.x * 1.0f + v28.x * p.x) + v29.x * p.y) + v30.x * p.z;
            const float cy = ((v31.y * 1.0f + v28.y * p.x) + v29.y * p.y) + v30.y * p.z;
            const float cz = v31.z * 1.0f + v30.z * p.z;
            const float cw = ((v31.w * 1.0f + v28.w * p.x) + v29.w * p.y) + v30.w * p.z;
            q = vdiv(1.0f, cw);
            sk = Screen{v2.x * 1.0f + cx * q, v2.y * 1.0f + cy * q, v2.z * 1.0f + cz * q};
            u32 w = 0;
            if (k >= 2) {
                const u32 sum = ((facing_negative(s[(k - 2) % 3u], s[(k - 1) % 3u], sk, k) ? 32u : 0u) + base) & 0xFFFFu;
                w = static_cast<u32>(static_cast<std::int32_t>(static_cast<std::int16_t>(sum)));
            }
            xyz[0] = ftoi(sk.x, 4);
            xyz[1] = ftoi(sk.y, 4);
            xyz[2] = ftoi(sk.z, 4);
            xyz[3] = w;
        }
        put(o, bits(t.x * q), bits(t.y * q), bits(1.0f * q), 0u);
        put(o, ftoi(c.x, 0), ftoi(c.y, 0), ftoi(c.z, 0), ftoi(c.w, 0));
        put(o, xyz[0], xyz[1], xyz[2], xyz[3]);
    }
}

// CLIP: the bits of x, y and z beyond +|w| and -|w|, compared as the runtime does (denormals as zero, sign and magnitude).
inline std::int32_t key(float v) {
    u32 x = bits(v);
    if (!(x & 0x7F800000u)) x &= 0x80000000u;
    const auto magnitude = static_cast<std::int32_t>(x & 0x7FFFFFFFu);
    return (x & 0x80000000u) ? -magnitude : magnitude;
}
inline u32 clip_codes(float x, float y, float z, float w) {
    const std::int32_t lim = key(f(bits(w) & 0x7FFFFFFFu));
    const float s[3] = {x, y, z};
    u32 codes = 0;
    for (int i = 0; i < 3; ++i) {
        const std::int32_t k = key(s[i]);
        if (k > lim) codes |= 1u << (i * 2);
        if (k < -lim) codes |= 2u << (i * 2);
    }
    return codes;
}

// The full clipper's first pass (042 0x1FF8-0x21F0): every vertex to clip space, its x and y scaled by the guard band
// factors (M[0x3FD].y, M[0x3FE].y) and its depth tested as M[0x3FD].z + M[0x3FE].z * w. When all vertices are beyond one
// same plane, the program draws nothing and ends the batch (0x3958) without changing output buffers. The pass goes over the
// vertices in pairs, so with an odd count it tests the qword after the last vertex too.
bool clipper_culls_all(const Input &in, u32 top, u32 n) {
    const Batch b{in, top, n};
    const Vec v28 = b.vf(28), v29 = b.vf(29), v30 = b.vf(30), v31 = b.vf(31);
    const Vec m26 = load(in.mem, 0x3FD), m27 = load(in.mem, 0x3FE);
    const float bx = v31.x + 0.0f, by = v31.y + 0.0f, bw = v31.w + 0.0f;
    const float sx = m26.y, sy = 0.0f + m27.y, sz = 0.0f + m27.z;
    const float az = 0.0f + m26.z;
    const u32 count = (n + 1u) & ~1u;
    u32 all = 63;
    for (u32 k = 0; k < count && all; ++k) {
        const Vec p = load(in.mem, top + 4u * k);
        const float cx = ((bx * 1.0f + v28.x * p.x) + v29.x * p.y) + v30.x * p.z;
        const float cy = ((by * 1.0f + v28.y * p.x) + v29.y * p.y) + v30.y * p.z;
        const float cw = ((bw * 1.0f + v28.w * p.x) + v29.w * p.y) + v30.w * p.z;
        const float w = cw * 1.0f;
        all &= clip_codes(cx * sx, cy * sy, az + sz * cw, w);
    }
    return all != 0;
}

}

bool run(const Input &in, std::vector<std::uint8_t> &out, std::vector<std::uint32_t> &ends, End &end) {
    end = End{};
    if (in.program == 41 || in.program == 43 || in.program == 44 || in.program == 45 || in.program == 46 ||
        in.program == 51 || in.program == 53 || in.program == 59 || in.program == 65 || in.program == 66 ||
        in.program == 73 || in.program == 75 || in.program == 76 || in.program == 95 || in.program == 102)
        return false;   // handled by gpu_batch only
    const u32 flags = in.mem[0x3FF * 16];
    const u32 top = in.top & 1023u, n = in.itop & 1023u;
    const bool skin = in.program == 64;
    if (n == 0 || top + (skin ? 5u : 4u) * n > 0x3D0u) return false;
    if (flags & 2u) {
        if (skin || !(flags & 8u)) return false;            // the guard band clipper, and 064's clippers: not yet
        if (!clipper_culls_all(in, top, n)) return false;   // the clipper's drawing: not yet
        end.swap = false;
        end.clipper = true;
        return true;
    }
    if (flags & 4u) return false;   // triangle lists: not yet
    static Lighting lighting;
    if (!prepare_lighting(in.mem, lighting)) return false;
    const std::size_t bytes = 16u + 48u * n;
    if (out.size() < bytes) out.resize(bytes < 16384u ? 16384u : bytes);
    u32 *o = reinterpret_cast<u32 *>(out.data());
    if (skin) {
        if (flags & 1u) strip<true, true>(in, top, n, lighting, o);
        else strip<true, false>(in, top, n, lighting, o);
    } else {
        if (flags & 1u) strip<false, true>(in, top, n, lighting, o);
        else strip<false, false>(in, top, n, lighting, o);
    }
    ends.push_back(static_cast<u32>(bytes));
    return true;
}

namespace {

inline void set4(float *d, float x, float y, float z, float w) { d[0] = x, d[1] = y, d[2] = z, d[3] = w; }

// The vertex loops below work on 32-bit words only: on the 3DS's ARM11, moving values between the VFP and the integer
// registers (a float compare's flags, a float taken apart) stalls, and those loops were the bulk of gpu_batch's time there.

// The four words of a qword of VU memory.
inline void words(const std::uint8_t *mem, u32 qword, u32 w[4]) { std::memcpy(w, mem + (qword & 1023u) * 16u, 16); }

// The colour (0-255 each) and the normal (signed bytes, the fourth zero) of a vertex as GpuVertex holds them, as words, from
// their integer lanes. False when they do not fit: a lane out of range (the normal's moved up by 128) has bits above the
// low 8, negative ones too, so the lanes ORed together show it. Without lights the normal is zero.
inline bool colour_normal(const std::uint8_t *mem, u32 at, bool lights, u32 &colour, u32 &normal) {
    u32 c[4], n[4];
    words(mem, at, c);
    words(mem, at + 1u, n);
    if ((c[0] | c[1] | c[2] | c[3]) > 255u) return false;
    colour = c[0] | c[1] << 8 | c[2] << 16 | c[3] << 24;
    normal = 0;
    if (lights) {
        if (((n[0] + 128u) | (n[1] + 128u) | (n[2] + 128u)) > 255u) return false;
        normal = (n[0] & 255u) | (n[1] & 255u) << 8 | (n[2] & 255u) << 16;
    }
    return true;
}

// GpuVertex as words: x y z u v, colour, normal, the four weights, the four bones (bytes).
static_assert(sizeof(GpuVertex) == 12 * sizeof(u32));
constexpr u32 kOne = 0x3F800000u;   // 1.0f

}

bool g_behind_on_gpu = false;

// The clipper's all-outside test and, in stereo, a look for vertices behind the camera: 1 when all vertices are beyond one same
// clip plane (nothing drawn), 2 when one has w <= 0 or not a number (the program's own clipper then runs), 0 for the GPU.
// Vertices beyond the near or far plane are clipped by the GPU (gs.v.pica gives it clip space).
template <bool Behind, class Pos>
static inline int clipper_test(const Input &in, u32 n, u32 count, Pos pos) {
    const Batch b{in, 0, n};
    const Vec v28 = b.vf(28), v29 = b.vf(29), v30 = b.vf(30), v31 = b.vf(31);
    const Vec m26 = load(in.mem, 0x3FD), m27 = load(in.mem, 0x3FE);
    const float bx = v31.x + 0.0f, by = v31.y + 0.0f, bw = v31.w + 0.0f;
    const float sx = m26.y, sy = 0.0f + m27.y, sz = 0.0f + m27.z;
    const float az = 0.0f + m26.z;
    u32 all = 63;
    bool w_bad = false;
    for (u32 k = 0; k < count && (all || (Behind && !w_bad)); ++k) {
        float p[3];
        pos(k, p);
        const float cw = ((bw * 1.0f + v28.w * p[0]) + v29.w * p[1]) + v30.w * p[2];
        const u32 w_bits = bits(cw * 1.0f);
        if constexpr (Behind) {
            if (k < n && (w_bits - 0x00800000u) >= 0x7F000000u) w_bad = true;
            if (!all) continue;
        }
        const std::int32_t lim = key(f(w_bits & 0x7FFFFFFFu));
        u32 codes = 0;
        if (all & 3u) {
            const float cx = ((bx * 1.0f + v28.x * p[0]) + v29.x * p[1]) + v30.x * p[2];
            const std::int32_t kx = key(cx * sx);
            if (kx > lim) codes |= 1u;
            if (kx < -lim) codes |= 2u;
        }
        if (all & 12u) {
            const float cy = ((by * 1.0f + v28.y * p[0]) + v29.y * p[1]) + v30.y * p[2];
            const std::int32_t ky = key(cy * sy);
            if (ky > lim) codes |= 4u;
            if (ky < -lim) codes |= 8u;
        }
        if (all & 48u) {
            const std::int32_t kz = key(az + sz * cw);
            if (kz > lim) codes |= 16u;
            if (kz < -lim) codes |= 32u;
        }
        all &= codes;
    }
    if (all != 0) return 1;
    return Behind && w_bad ? 2 : 0;
}

static int clipper_test_direct(const Input &in, const std::uint8_t *src, u32 step, u32 n) {
    const auto pos = [src, step](u32 k, float *p) { std::memcpy(p, src + k * step, 12); };
    return g_behind_on_gpu ? clipper_test<false>(in, n, n, pos) : clipper_test<true>(in, n, n, pos);
}

static int clipper_test_indirect(const Input &in, u32 top, u32 n) {
    const auto pos = [&in, top](u32 k, float *p) {
        const Vec v = load(in.mem, top + 4u * k);
        p[0] = v.x, p[1] = v.y, p[2] = v.z;
    };
    const u32 count = (n + 1u) & ~1u;
    return g_behind_on_gpu ? clipper_test<false>(in, n, count, pos) : clipper_test<true>(in, n, count, pos);
}

// A list of n / 3 triangles as one strip: each triangle after the first is joined by its predecessor's last vertex and its
// own first three times, so it starts at an even place (where the GPU does not turn the triangle round) and every triangle
// between has no area. a0 b0 c0, then c0 a1 a1 a1 b1 c1, and so on: 2n - 3 vertices.
static void list_to_strip(Gpu &g) {
    const unsigned triangles = g.n / 3u;
    for (unsigned t = triangles - 1u; t >= 1u; --t) {
        const GpuVertex join = g.v[3u * t - 1u], a = g.v[3u * t], b = g.v[3u * t + 1u], c = g.v[3u * t + 2u];
        GpuVertex *o = g.v + 6u * t - 3u;
        o[0] = join, o[1] = a, o[2] = a, o[3] = a, o[4] = b, o[5] = c;
    }
    g.n = triangles ? 6u * triangles - 3u : 0u;
}

struct BoneCache {
    std::uint16_t address_seen[1024]{};
    std::uint8_t address_slot[1024]{};
    std::uint32_t slot_hash[kGpuBones]{};
    std::uint32_t used_in[kGpuBones]{};
    std::uint32_t slot_rows[kGpuBones][12]{};
    std::uint32_t batch = 0;
    std::uint32_t gen = 0;
    std::uint16_t seen_gen = 0;
    std::uint8_t by_hash[64]{};   // slot + 1 for a hash's bucket, 0 for none
    bool hash_dirty = false;      // set by restore_bones; rebuilt lazily in resolve_bone_slot
    BoneUse uses[kMaxBoneUses];   // the current batch's bone addresses in the order first met, with their slots
    int use_count = 0;            // -1: more than kMaxBoneUses
};

static BoneCache g_bones;

// The program's rows 0-2 (rotation) and 3 (translation), x, y and z of each, for the bone at address.
static inline void bone_rows(const std::uint8_t *mem, u32 address, std::uint32_t rows[12]) {
    const u32 base_q = (address + 696u) & 1023u;
    if (base_q <= 1020u) {
        const auto *q = reinterpret_cast<const u32 *>(mem + base_q * 16u);
        rows[0] = q[0]; rows[1] = q[1]; rows[2] = q[2];
        rows[3] = q[4]; rows[4] = q[5]; rows[5] = q[6];
        rows[6] = q[8]; rows[7] = q[9]; rows[8] = q[10];
        rows[9] = q[12]; rows[10] = q[13]; rows[11] = q[14];
    } else {
        for (u32 r = 0; r < 4; ++r) std::memcpy(rows + 3 * r, mem + ((address + 696u + r) & 1023u) * 16u, 12);
    }
}

static inline std::uint32_t rows_hash(const std::uint32_t rows[12]) {
    std::uint32_t h0 = 0x811C9DC5u, h1 = 0x01000193u;
    for (int j = 0; j < 12; j += 2) {
        h0 = (h0 ^ rows[j]) * 0x01000193u;
        h1 = (h1 ^ rows[j + 1]) * 0x9E3779B1u;
    }
    return h0 ^ (h1 >> 1);
}

static inline unsigned hash_bucket(std::uint32_t h) { return (h ^ (h >> 11) ^ (h >> 22)) & 63u; }

static inline bool rows_nonfinite(const std::uint32_t rows[12]) {
    constexpr std::uint32_t kExp = 0x7F800000u;
    for (int j = 0; j < 12; ++j)
        if ((rows[j] & kExp) == kExp) return true;
    return false;
}

// Puts the bone into the slot, for the shader too (a new generation, so the uniforms are sent).
static void set_bone_slot(int slot, const std::uint32_t rows[12], std::uint32_t h, Gpu &g) {
    const unsigned old = hash_bucket(g_bones.slot_hash[slot]);
    if (g_bones.by_hash[old] == slot + 1) g_bones.by_hash[old] = 0;
    g_bones.by_hash[hash_bucket(h)] = static_cast<std::uint8_t>(slot + 1);
    g_bones.slot_hash[slot] = h;
    std::memcpy(g_bones.slot_rows[slot], rows, 12 * sizeof(std::uint32_t));
    // turned for the shader: vector c is (row0.c, row1.c, row2.c, row3.c)
    float m[12];
    std::memcpy(m, rows, sizeof m);
    for (int c = 0; c < 3; ++c) set4(g.bone[3 * slot + c], m[c], m[3 + c], m[6 + c], m[9 + c]);
    if (rows_nonfinite(rows)) g.bone_nonfinite |= (1u << slot);
    else g.bone_nonfinite &= ~(1u << slot);
    g.bone_gen[slot] = ++g_bones.gen;
}

// Same without updating by_hash/slot_hash: restore_bones already knows the slot and marks the hash table dirty.
static void set_bone_slot_direct(int slot, const std::uint32_t rows[12], Gpu &g) {
    g_bones.hash_dirty = true;
    std::memcpy(g_bones.slot_rows[slot], rows, 12 * sizeof(std::uint32_t));
    float m[12];
    std::memcpy(m, rows, sizeof m);
    for (int c = 0; c < 3; ++c) set4(g.bone[3 * slot + c], m[c], m[3 + c], m[6 + c], m[9 + c]);
    if (rows_nonfinite(rows)) g.bone_nonfinite |= (1u << slot);
    else g.bone_nonfinite &= ~(1u << slot);
    g.bone_gen[slot] = ++g_bones.gen;
}

static void rebuild_bone_hashes() {
    std::memset(g_bones.by_hash, 0, sizeof g_bones.by_hash);
    for (unsigned s = 0; s < kGpuBones; ++s) {
        if (!g_bones.used_in[s]) continue;
        const std::uint32_t h = rows_hash(g_bones.slot_rows[s]);
        g_bones.slot_hash[s] = h;
        g_bones.by_hash[hash_bucket(h)] = static_cast<std::uint8_t>(s + 1);
    }
    g_bones.hash_dirty = false;
}

[[gnu::noinline]] static int resolve_bone_slot(const std::uint8_t *mem, u32 address, u32 batch, std::uint16_t seen_gen, Gpu &g) {
    G3_PROF_EXTRA(9);   // SAMPLE=1: the bone lookup, apart from the rest of gpu_batch (x8)
    if (g_bones.hash_dirty) rebuild_bone_hashes();
    std::uint32_t rows[12];
    bone_rows(mem, address, rows);
    const std::uint32_t h = rows_hash(rows);
    const unsigned bucket = hash_bucket(h);
    int slot = g_bones.by_hash[bucket] - 1;
    bool hit = slot >= 0 && g_bones.slot_hash[slot] == h;
    if (hit) {
        const u32 *sr = g_bones.slot_rows[slot];
        for (int j = 0; j < 12; ++j)
            if (sr[j] != rows[j]) { hit = false; break; }
    }
    if (!hit) {
        slot = 0;
        for (unsigned j = 1; j < kGpuBones; ++j)
            if (g_bones.used_in[j] < g_bones.used_in[slot]) slot = static_cast<int>(j);
        if (g_bones.used_in[slot] == batch) return -1;
        set_bone_slot(slot, rows, h, g);
    }
    g_bones.used_in[slot] = batch;
    g_bones.address_seen[address] = seen_gen;
    g_bones.address_slot[address] = static_cast<std::uint8_t>(slot);
    if (g_bones.use_count >= 0) {
        if (g_bones.use_count < static_cast<int>(kMaxBoneUses))
            g_bones.uses[g_bones.use_count++] = BoneUse{static_cast<std::uint16_t>(address), static_cast<std::uint8_t>(slot)};
        else
            g_bones.use_count = -1;
    }
    return slot;
}

int last_bones(const BoneUse *&uses) {
    uses = g_bones.uses;
    return g_bones.use_count;
}

bool restore_bones(const std::uint8_t *mem, const BoneUse *uses, int count, Gpu &g) {
    if (count < 0 || count > static_cast<int>(kMaxBoneUses)) return false;
    // Fast path when all bones map to distinct slots (almost every batch): compare VU1 memory directly against slot_rows.
    std::uint32_t mask = 0;
    bool dup = false;
    for (int i = 0; i < count; ++i) {
        const u32 bit = 1u << uses[i].slot;
        if (mask & bit) { dup = true; break; }
        mask |= bit;
    }
    if (!dup) {
        const u32 batch = ++g_bones.batch;
        for (int i = 0; i < count; ++i) {
            const int slot = uses[i].slot;
            const u32 base_q = (uses[i].address + 696u) & 1023u;
            const u32 *sr = g_bones.slot_rows[slot];
            if (base_q <= 1020u) {
                const auto *q = reinterpret_cast<const u32 *>(mem + base_q * 16u);
                const u32 diff = (q[0] ^ sr[0]) | (q[1] ^ sr[1]) | (q[2] ^ sr[2]) |
                                 (q[4] ^ sr[3]) | (q[5] ^ sr[4]) | (q[6] ^ sr[5]) |
                                 (q[8] ^ sr[6]) | (q[9] ^ sr[7]) | (q[10] ^ sr[8]) |
                                 (q[12] ^ sr[9]) | (q[13] ^ sr[10]) | (q[14] ^ sr[11]);
                if (diff) {
                    const u32 rows[12] = {q[0], q[1], q[2], q[4], q[5], q[6], q[8], q[9], q[10], q[12], q[13], q[14]};
                    set_bone_slot_direct(slot, rows, g);
                }
            } else {
                u32 rows[12];
                bone_rows(mem, uses[i].address, rows);
                if (std::memcmp(sr, rows, sizeof rows) != 0) set_bone_slot_direct(slot, rows, g);
            }
            g_bones.used_in[slot] = batch;
        }
        g.slots = mask;
        return true;
    }
    std::uint32_t rows[kMaxBoneUses][12];
    mask = 0;
    for (int i = 0; i < count; ++i) {
        bone_rows(mem, uses[i].address, rows[i]);
        // Two of its bones were one matrix when its vertices were made, and they share a slot: they must still be one.
        for (int j = 0; j < i; ++j)
            if (uses[j].slot == uses[i].slot && std::memcmp(rows[j], rows[i], sizeof rows[i]) != 0) return false;
        mask |= 1u << uses[i].slot;
    }
    const u32 batch = ++g_bones.batch;
    for (int i = 0; i < count; ++i) {
        const int slot = uses[i].slot;
        if (g_bones.used_in[slot] != batch && std::memcmp(g_bones.slot_rows[slot], rows[i], sizeof rows[i]) != 0)
            set_bone_slot_direct(slot, rows[i], g);
        g_bones.used_in[slot] = batch;
    }
    g.slots = mask;
    return true;
}

template <bool Lights, bool UvXform>
[[gnu::noinline]] static void fill_generic_direct_impl(const std::uint8_t *mem, Gpu &g, GpuVertex *dst, const DirectStreams *ds, u32 n, u32 pos_w, u32 uv_w, u32 nrm_step) {
    const auto *p = reinterpret_cast<const u32 *>(ds->s[0].src);
    const auto *t = reinterpret_cast<const u32 *>(ds->s[1].src);
    const auto *c = reinterpret_cast<const u32 *>(ds->s[2].src);
    const std::uint8_t *nm = Lights ? ds->s[3].src : nullptr;
    auto *o = reinterpret_cast<u32 *>(dst);
    const Vec m956 = UvXform ? load(mem, 956u) : Vec{};
    const Vec m957 = UvXform ? load(mem, 957u) : Vec{};
    const Vec m958 = UvXform ? load(mem, 958u) : Vec{};
    u32 amin = 255u, amax = 0u;
    for (u32 k = 0; k < n; ++k, p += pos_w, t += uv_w, ++c, o += 12) {
        o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
        if constexpr (UvXform) {
            float u1, v1;
            std::memcpy(&u1, &t[0], 4);
            std::memcpy(&v1, &t[1], 4);
            const float uo = m958.x + m956.x * u1 + m957.x * v1;
            const float vo = m958.y + m956.y * u1 + m957.y * v1;
            std::memcpy(&o[3], &uo, 4);
            std::memcpy(&o[4], &vo, 4);
        } else {
            o[3] = t[0]; o[4] = t[1];
        }
        const u32 col = c[0];
        const u32 a = col >> 24;
        if (a < amin) amin = a;
        if (a > amax) amax = a;
        o[5] = col;
        if constexpr (Lights) {
            o[6] = static_cast<u32>(nm[0]) | (static_cast<u32>(nm[1]) << 8) | (static_cast<u32>(nm[2]) << 16);
            nm += nrm_step;
        } else {
            o[6] = 0;
        }
        o[7] = kOne; o[8] = 0; o[9] = 0; o[10] = 0; o[11] = 0;
    }
    g.alpha_min = static_cast<std::uint8_t>(amin);
    g.alpha_max = static_cast<std::uint8_t>(amax);
}

static inline void fill_generic_direct(const std::uint8_t *mem, Gpu &g, GpuVertex *dst, const DirectStreams *ds, u32 n, u32 pos_w, u32 uv_w, u32 nrm_step, bool lights, bool uv_xform) {
    if (uv_xform) {
        if (lights) fill_generic_direct_impl<true, true>(mem, g, dst, ds, n, pos_w, uv_w, nrm_step);
        else fill_generic_direct_impl<false, true>(mem, g, dst, ds, n, pos_w, uv_w, nrm_step);
    } else if (lights) {
        fill_generic_direct_impl<true, false>(mem, g, dst, ds, n, pos_w, uv_w, nrm_step);
    } else {
        fill_generic_direct_impl<false, false>(mem, g, dst, ds, n, pos_w, uv_w, nrm_step);
    }
}

template <bool Lights, bool Uv1Xform, bool Uv2Xform, bool HasUv2>
[[gnu::noinline]] static void fill_dual_direct_impl(const std::uint8_t *mem, Gpu &g, GpuVertex *dst, const DirectStreams *ds, u32 n, u32 pos_w, u32 uv_w, u32 nrm_step) {
    const auto *p = reinterpret_cast<const u32 *>(ds->s[0].src);
    const auto *t = reinterpret_cast<const u32 *>(ds->s[1].src);
    const auto *c = reinterpret_cast<const u32 *>(ds->s[2].src);
    const std::uint8_t *nm = Lights ? ds->s[3].src : nullptr;
    auto *o = reinterpret_cast<u32 *>(dst);
    const Vec m963 = (Uv1Xform || Uv2Xform) ? load(mem, 963u) : Vec{};
    const Vec m964 = (Uv1Xform || Uv2Xform) ? load(mem, 964u) : Vec{};
    const Vec m965 = (Uv1Xform || Uv2Xform) ? load(mem, 965u) : Vec{};
    u32 amin = 255u, amax = 0u;
    for (u32 k = 0; k < n; ++k, p += pos_w, t += uv_w, ++c, o += 12) {
        o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
        if constexpr (Uv1Xform) {
            float u1, v1;
            std::memcpy(&u1, &t[0], 4);
            std::memcpy(&v1, &t[1], 4);
            const float u1o = m965.x + m963.x * u1 + m964.x * v1;
            const float v1o = m965.y + m963.y * u1 + m964.y * v1;
            std::memcpy(&o[3], &u1o, 4);
            std::memcpy(&o[4], &v1o, 4);
        } else {
            o[3] = t[0]; o[4] = t[1];
        }
        const u32 col = c[0];
        const u32 a = col >> 24;
        if (a < amin) amin = a;
        if (a > amax) amax = a;
        o[5] = col;
        if constexpr (Lights) {
            o[6] = static_cast<u32>(nm[0]) | (static_cast<u32>(nm[1]) << 8) | (static_cast<u32>(nm[2]) << 16);
            nm += nrm_step;
        } else {
            o[6] = 0;
        }
        o[7] = kOne; o[8] = 0; o[9] = 0; o[10] = 0; o[11] = 0;
        if constexpr (HasUv2) {
            float u2, v2;
            std::memcpy(&u2, &t[2], 4);
            std::memcpy(&v2, &t[3], 4);
            if constexpr (Uv2Xform) {
                g.uv2[k][0] = m965.z + m963.z * u2 + m964.z * v2;
                g.uv2[k][1] = m965.w + m963.w * u2 + m964.w * v2;
            } else {
                g.uv2[k][0] = u2;
                g.uv2[k][1] = v2;
            }
        } else {
            g.uv2[k][0] = Uv2Xform ? m965.z : 0.0f;
            g.uv2[k][1] = Uv2Xform ? m965.w : 0.0f;
        }
    }
    g.alpha_min = static_cast<std::uint8_t>(amin);
    g.alpha_max = static_cast<std::uint8_t>(amax);
}

static inline void fill_dual_direct(const std::uint8_t *mem, Gpu &g, GpuVertex *dst, const DirectStreams *ds, u32 n, u32 pos_w, u32 uv_w, u32 nrm_step, bool lights, bool uv1_xform, bool uv2_xform) {
    const bool has_uv2 = uv_w >= 4u;
    if (lights) {
        if (uv1_xform) {
            if (has_uv2) fill_dual_direct_impl<true, true, true, true>(mem, g, dst, ds, n, pos_w, uv_w, nrm_step);
            else fill_dual_direct_impl<true, true, true, false>(mem, g, dst, ds, n, pos_w, uv_w, nrm_step);
        } else if (uv2_xform) {
            if (has_uv2) fill_dual_direct_impl<true, false, true, true>(mem, g, dst, ds, n, pos_w, uv_w, nrm_step);
            else fill_dual_direct_impl<true, false, true, false>(mem, g, dst, ds, n, pos_w, uv_w, nrm_step);
        } else {
            if (has_uv2) fill_dual_direct_impl<true, false, false, true>(mem, g, dst, ds, n, pos_w, uv_w, nrm_step);
            else fill_dual_direct_impl<true, false, false, false>(mem, g, dst, ds, n, pos_w, uv_w, nrm_step);
        }
    } else {
        if (uv1_xform) {
            if (has_uv2) fill_dual_direct_impl<false, true, true, true>(mem, g, dst, ds, n, pos_w, uv_w, nrm_step);
            else fill_dual_direct_impl<false, true, true, false>(mem, g, dst, ds, n, pos_w, uv_w, nrm_step);
        } else if (uv2_xform) {
            if (has_uv2) fill_dual_direct_impl<false, false, true, true>(mem, g, dst, ds, n, pos_w, uv_w, nrm_step);
            else fill_dual_direct_impl<false, false, true, false>(mem, g, dst, ds, n, pos_w, uv_w, nrm_step);
        } else {
            if (has_uv2) fill_dual_direct_impl<false, false, false, true>(mem, g, dst, ds, n, pos_w, uv_w, nrm_step);
            else fill_dual_direct_impl<false, false, false, false>(mem, g, dst, ds, n, pos_w, uv_w, nrm_step);
        }
    }
}

template <bool Lights, bool DirectW, bool UvXform>
[[gnu::noinline]] static int fill_skin_direct_impl(const std::uint8_t *mem, u32 top, Gpu &g, GpuVertex *dst, const DirectStreams *ds, u32 n, u32 pos_w, u32 uv_w, u32 nrm_step, u32 batch, std::uint16_t seen_gen) {
    const auto *p = reinterpret_cast<const u32 *>(ds->s[0].src);
    const auto *t = reinterpret_cast<const u32 *>(ds->s[1].src);
    const auto *c = reinterpret_cast<const u32 *>(ds->s[2].src);
    const std::uint8_t *nm = Lights ? ds->s[3].src : nullptr;
    const auto *wp = DirectW ? reinterpret_cast<const u32 *>(ds->s[4].src) : nullptr;
    auto *o = reinterpret_cast<u32 *>(dst);
    const Vec m956 = UvXform ? load(mem, 956u) : Vec{};
    const Vec m957 = UvXform ? load(mem, 957u) : Vec{};
    const Vec m958 = UvXform ? load(mem, 958u) : Vec{};
    u32 slots = 0;
    u32 amin = 255u, amax = 0u;
    std::uint16_t *const seen = g_bones.address_seen;
    std::uint8_t *const slot_tbl = g_bones.address_slot;
    u32 last_addr = ~0u;
    u32 last_slot3 = 0u;
    auto resolve_addr = [&](u32 address, u32 &slot3_out) -> bool {
        if (address == last_addr) {
            slot3_out = last_slot3;
            return true;
        }
        u32 slot;
        if (seen[address] == seen_gen) {
            slot = slot_tbl[address];
        } else {
            const int s = resolve_bone_slot(mem, address, batch, seen_gen, g);
            if (s < 0) return false;
            slot = static_cast<u32>(s);
        }
        last_addr = address;
        last_slot3 = slot + (slot << 1);
        slots |= 1u << slot;
        slot3_out = last_slot3;
        return true;
    };
    for (u32 k = 0; k < n; ++k, p += pos_w, t += uv_w, ++c, o += 12) {
        o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
        if constexpr (UvXform) {
            float u1, v1;
            std::memcpy(&u1, &t[0], 4);
            std::memcpy(&v1, &t[1], 4);
            const float uo = m958.x + m956.x * u1 + m957.x * v1;
            const float vo = m958.y + m956.y * u1 + m957.y * v1;
            std::memcpy(&o[3], &uo, 4);
            std::memcpy(&o[4], &vo, 4);
        } else {
            o[3] = t[0]; o[4] = t[1];
        }
        const u32 col = c[0];
        const u32 a = col >> 24;
        if (a < amin) amin = a;
        if (a > amax) amax = a;
        o[5] = col;
        if constexpr (Lights) {
            o[6] = static_cast<u32>(nm[0]) | (static_cast<u32>(nm[1]) << 8) | (static_cast<u32>(nm[2]) << 16);
            nm += nrm_step;
        } else {
            o[6] = 0;
        }
        const u32 *w;
        if constexpr (DirectW) {
            w = wp;
            wp += 4;
        } else {
            w = reinterpret_cast<const u32 *>(mem + ((top + 5u * k + 4u) & 1023u) * 16u);
        }
        const u32 w0 = w[0], w1 = w[1], w2 = w[2], w3 = w[3];
        if (!((w1 | w2 | w3) & 0x7F800000u)) {
            if (w0 & 0x7F800000u) {
                u32 s0;
                if (!resolve_addr(w0 & 1023u, s0)) return kGpuBoneCount;
                o[7] = w0; o[8] = 0; o[9] = 0; o[10] = 0; o[11] = s0;
            } else {
                o[7] = 0; o[8] = 0; o[9] = 0; o[10] = 0; o[11] = 0;
            }
            continue;
        }
        u32 bones = 0;
        if (w0 & 0x7F800000u) {
            u32 s0;
            if (!resolve_addr(w0 & 1023u, s0)) return kGpuBoneCount;
            o[7] = w0;
            bones = s0;
        } else {
            o[7] = 0;
        }
        if (w1 & 0x7F800000u) {
            u32 s1;
            if (!resolve_addr(w1 & 1023u, s1)) return kGpuBoneCount;
            o[8] = w1;
            bones |= s1 << 8;
        } else {
            o[8] = 0;
        }
        if (!((w2 | w3) & 0x7F800000u)) {
            o[9] = 0; o[10] = 0; o[11] = bones;
            continue;
        }
        if (w2 & 0x7F800000u) {
            u32 s2;
            if (!resolve_addr(w2 & 1023u, s2)) return kGpuBoneCount;
            o[9] = w2;
            bones |= s2 << 16;
        } else {
            o[9] = 0;
        }
        if (w3 & 0x7F800000u) {
            u32 s3;
            if (!resolve_addr(w3 & 1023u, s3)) return kGpuBoneCount;
            o[10] = w3;
            bones |= s3 << 24;
        } else {
            o[10] = 0;
        }
        o[11] = bones;
    }
    g.slots = slots;
    g.alpha_min = static_cast<std::uint8_t>(amin);
    g.alpha_max = static_cast<std::uint8_t>(amax);
    return 0;
}

static inline int fill_skin_direct(const std::uint8_t *mem, u32 top, Gpu &g, GpuVertex *dst, const DirectStreams *ds, u32 n, u32 pos_w, u32 uv_w, u32 nrm_step, bool lights, bool uv_xform, u32 batch, std::uint16_t seen_gen) {
    const bool direct_w = (ds->mask & 16u) != 0;
    if (uv_xform) {
        if (lights) {
            if (direct_w) return fill_skin_direct_impl<true, true, true>(mem, top, g, dst, ds, n, pos_w, uv_w, nrm_step, batch, seen_gen);
            return fill_skin_direct_impl<true, false, true>(mem, top, g, dst, ds, n, pos_w, uv_w, nrm_step, batch, seen_gen);
        }
        if (direct_w) return fill_skin_direct_impl<false, true, true>(mem, top, g, dst, ds, n, pos_w, uv_w, nrm_step, batch, seen_gen);
        return fill_skin_direct_impl<false, false, true>(mem, top, g, dst, ds, n, pos_w, uv_w, nrm_step, batch, seen_gen);
    }
    if (lights) {
        if (direct_w) return fill_skin_direct_impl<true, true, false>(mem, top, g, dst, ds, n, pos_w, uv_w, nrm_step, batch, seen_gen);
        return fill_skin_direct_impl<true, false, false>(mem, top, g, dst, ds, n, pos_w, uv_w, nrm_step, batch, seen_gen);
    }
    if (direct_w) return fill_skin_direct_impl<false, true, false>(mem, top, g, dst, ds, n, pos_w, uv_w, nrm_step, batch, seen_gen);
    return fill_skin_direct_impl<false, false, false>(mem, top, g, dst, ds, n, pos_w, uv_w, nrm_step, batch, seen_gen);
}

[[gnu::noinline]] static int fill_skin_indirect(const std::uint8_t *mem, u32 top, Gpu &g, GpuVertex *dst, u32 n, bool lights, bool uv_xform, u32 batch, std::uint16_t seen_gen) {
    auto *o = reinterpret_cast<u32 *>(dst);
    const Vec m956 = uv_xform ? load(mem, 956u) : Vec{};
    const Vec m957 = uv_xform ? load(mem, 957u) : Vec{};
    const Vec m958 = uv_xform ? load(mem, 958u) : Vec{};
    u32 slots = 0;
    u32 amin = 255u, amax = 0u;
    for (u32 k = 0; k < n; ++k, o += 12) {
        const u32 at = top + 5u * k;
        u32 p[4], t[4], w[4];
        words(mem, at, p);
        words(mem, at + 1u, t);
        words(mem, at + 4u, w);
        u32 col, nrm;
        if (!colour_normal(mem, at + 2u, lights, col, nrm)) return kGpuRange;
        const u32 a = col >> 24;
        if (a < amin) amin = a;
        if (a > amax) amax = a;
        o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
        if (uv_xform) {
            float u1, v1;
            std::memcpy(&u1, &t[0], 4);
            std::memcpy(&v1, &t[1], 4);
            const float uo = m958.x + m956.x * u1 + m957.x * v1;
            const float vo = m958.y + m956.y * u1 + m957.y * v1;
            std::memcpy(&o[3], &uo, 4);
            std::memcpy(&o[4], &vo, 4);
        } else {
            o[3] = t[0]; o[4] = t[1];
        }
        o[5] = col; o[6] = nrm;
        u32 bones = 0;
        for (int i = 0; i < 4; ++i) {
            const u32 wi = w[i];
            if (!(wi & 0x7F800000u)) {
                o[7 + i] = 0;
                continue;
            }
            o[7 + i] = wi;
            const u32 address = wi & 1023u;
            u32 slot;
            if (g_bones.address_seen[address] == seen_gen) {
                slot = g_bones.address_slot[address];
            } else {
                const int s = resolve_bone_slot(mem, address, batch, seen_gen, g);
                if (s < 0) return kGpuBoneCount;
                slot = static_cast<u32>(s);
            }
            bones |= 3u * slot << (8 * i);
            slots |= 1u << slot;
        }
        o[11] = bones;
    }
    g.slots = slots;
    g.alpha_min = static_cast<std::uint8_t>(amin);
    g.alpha_max = static_cast<std::uint8_t>(amax);
    return 0;
}

int gpu_batch_setup(const Input &in, Gpu &g, End &end, BatchPlan &plan) {
    G3_PROF_EXTRA(10);   // SAMPLE=1: gpu_batch's setup; x11 the clipper tests, x12 and x13 the vertex loops, x9 the bone lookup
    end = End{};
    const std::uint8_t *mem = in.mem;
    const u32 flags = mem[0x3FF * 16];
    const u32 top = in.top & 1023u, n = in.itop & 1023u;
    const bool ortho = in.program == 41 || in.program == 65 || in.program == 76 || in.program == 102;
    const bool dual = in.program == 43 || in.program == 44 || in.program == 59 || in.program == 95;
    const bool dual_uv1 = in.program == 44 || in.program == 95;
    const bool dual_uv2 = in.program == 44 || in.program == 95;
    const bool env = in.program == 51 || in.program == 53 || in.program == 73 || in.program == 75 || in.program == 76;
    const bool no_cull = in.program == 46 || in.program == 51 || in.program == 59 || in.program == 73 || in.program == 95;
    const bool uv_xform = in.program == 45 || in.program == 66 || in.program == 102;
    const bool skin = in.program == 64 || in.program == 65 || in.program == 66 || in.program == 73 || in.program == 75 || in.program == 76 || in.program == 102;
    if (n == 0 || n > 256u || top + (skin ? 5u : 4u) * n > ((uv_xform || dual || env) ? 956u : 0x3D0u)) return kGpuNotModelled;
    if (ortho && (flags & 6u)) return kGpuNotModelled;   // only non-clipper strip for ortho programs
    // Triangle lists: only g3_generic's plain ones (flags 0xC) are known. Their vertices are the strip's, each triangle with
    // the strip's facing test as if it began at an even place (042 0x1520 on; tools/g3check -liststrip).
    const bool list = flags & 4u;
    if (list && (skin || dual || env || uv_xform || ortho || ((flags & 2u) ? !(flags & 8u) : (flags & 1u)) || n % 3u)) return kGpuNotModelled;
    const Vec v3 = load(mem, 0x3FC);
    if (v3.w + 0.0f != 0.0f) return kGpuNotModelled;
    const Vec mat = load(mem, 0x3FB);
    const bool has_lights = u16_at(mem, 0x3D4, 12) != 0;
    const bool need_normals = has_lights || env;
    const DirectStreams *ds = in.streams;
    bool direct = false;
    u32 pos_w = 0, uv_w = 0, nrm_step = 0;
    if (ds) {
        const bool ok_pos = (ds->mask & 1u) && ds->s[0].num >= n && ds->s[0].vl == 0 && (ds->s[0].vn == 2 || ds->s[0].vn == 3);
        const bool ok_uv = (ds->mask & 2u) && ds->s[1].num >= n && ds->s[1].vl == 0 && (ds->s[1].vn >= 1 && ds->s[1].vn <= 3);
        const bool ok_col = (ds->mask & 4u) && ds->s[2].num >= n && ds->s[2].vn == 3 && ds->s[2].vl == 2 && ds->s[2].usn;
        const bool ok_nrm = !need_normals || ((ds->mask & 8u) && ds->s[3].num >= n && ds->s[3].vl == 2 && !ds->s[3].usn && (ds->s[3].vn == 2 || ds->s[3].vn == 3));
        const bool ok_wgt = !skin || !(ds->mask & 16u) || (ds->s[4].num >= n && ds->s[4].vn == 3 && ds->s[4].vl == 0);
        const std::uintptr_t align_check =
            reinterpret_cast<std::uintptr_t>(ds->s[0].src) | reinterpret_cast<std::uintptr_t>(ds->s[1].src) |
            reinterpret_cast<std::uintptr_t>(ds->s[2].src) | ((skin && (ds->mask & 16u)) ? reinterpret_cast<std::uintptr_t>(ds->s[4].src) : 0u);
        if (ds->top == top && ds->stride == (skin ? 5u : 4u) && ok_pos && ok_uv && ok_col && ok_nrm && ok_wgt && !(align_check & 3u)) {
            direct = true;
            pos_w = static_cast<u32>(ds->s[0].vn + 1u);
            uv_w = static_cast<u32>(ds->s[1].vn + 1u);
            nrm_step = need_normals ? static_cast<u32>(ds->s[3].vn + 1u) : 0u;
        } else {
            return -2;
        }
    }
    if (flags & 2u) {
        if (skin || !(flags & 8u)) return kGpuNotModelled;
        end.clipper = true;
        G3_PROF_EXTRA(11);
        const int clip = direct ? clipper_test_direct(in, ds->s[0].src, pos_w * 4u, n) : clipper_test_indirect(in, top, n);
        if (clip == 1) {
            end.swap = false;
            return -1;
        }
        if (clip == 2) return kGpuClipDepth;
    }

    // PRIM from the GIF tag: a Gouraud triangle strip with ST coordinates is what the shader and the strip indices assume.
    const Vec tag = load(mem, 0x3FA);
    const u32 tag_hi = bits(tag.y);
    if (!(tag_hi & (1u << 14))) return kGpuPrim;   // no PRE: PRIM would stay as it was
    g.prim = (tag_hi >> 15) & 0x7FFu;
    if ((g.prim & 7u) != (list ? 3u : 4u) || !(g.prim & 8u) || (g.prim & 0x100u)) return kGpuPrim;
    if (dual || env) {
        const Vec tag2 = load(mem, env ? 962u : 956u);
        const u32 tag2_hi = bits(tag2.y);
        if (!(tag2_hi & (1u << 14))) return kGpuPrim;
        g.prim2 = (tag2_hi >> 15) & 0x7FFu;
        if ((g.prim2 & 7u) != 4u || !(g.prim2 & 8u) || (g.prim2 & 0x100u)) return kGpuPrim;
    }
    g.env = false;
    if (env) {
        const Vec m0 = load(mem, 963u), m1 = load(mem, 964u), m2 = load(mem, 965u), ev = load(mem, 966u);
        const float sx = 0.0f - ev.z, sy = 0.0f - ev.w;
        set4(g.env_u, m0.x * sx, m1.x * sx, m2.x * sx, (0.0f - ev.x) * sx);
        set4(g.env_v, m0.y * sy, m1.y * sy, m2.y * sy, (0.0f - ev.y) * sy);
        g.env_coeff = m2.w;
    }

    // Lights: ambient ones summed; directional ones each get N.L clamped at 0. Without lights, the colour times the material.
    // The batches of one model have the same lights, so what the last ones gave is kept with the memory they came from.
    const u32 light_x = bits(v3.x), light_z = bits(v3.z);
    bool same_lights = false;
    if (has_lights && g.light_words && g.light_x == light_x && g.light_z == light_z) {
        const u32 *now = reinterpret_cast<const u32 *>(mem + 0x3D0u * 16u), *was = g.light_mem;
        u32 diff = 0;
        for (unsigned left = g.light_words; left && !diff; left -= 4u, now += 4, was += 4)
            diff = (now[0] ^ was[0]) | (now[1] ^ was[1]) | (now[2] ^ was[2]) | (now[3] ^ was[3]);
        same_lights = diff == 0u;
    }
    if (!same_lights) {
        g.light_words = 0;
        float amb_x = 0.0f, amb_y = 0.0f, amb_z = 0.0f;
        unsigned directional = 0;
        if (has_lights) {
            const Vec m8 = load(mem, 0x3D0), m9 = load(mem, 0x3D1), m10 = load(mem, 0x3D2), m11 = load(mem, 0x3D3);
            const float v10w = 0.0f - 1.0f * m11.w;
            u32 at = 0x3D4;
            unsigned count = 0;
            for (;;) {
                if (count == 32u) return kGpuNotModelled;
                const u32 type = u16_at(mem, at, 12);
                if (type == 2u) {   // ambient
                    const Vec lc = load(mem, at);
                    amb_x += lc.x * v3.x;
                    amb_y += lc.y * v3.x;
                    amb_z += lc.z * v3.x;
                    ++count;
                    at += 1u;
                    continue;
                }
                if (type == 1u) {   // directional
                    if (directional == 3u) return kGpuLights;
                    const Vec lc = load(mem, at), p = load(mem, at + 1u);
                    float dx = m8.x * p.x, dy = m8.y * p.x, dz = m8.z * p.x;
                    dx = dx + m9.x * p.y; dy = dy + m9.y * p.y; dz = dz + m9.z * p.y;
                    dx = dx + m10.x * p.z; dy = dy + m10.y * p.z; dz = dz + m10.z * p.z;
                    set4(g.light_dir[directional], dx * v10w, dy * v10w, dz * v10w, 0.0f);
                    set4(g.light_colour[directional], lc.x * v3.z, lc.y * v3.z, lc.z * v3.z, 0.0f);
                    ++directional;
                    ++count;
                    at += 2u;
                    continue;
                }
                if (type == 0x80u || type == 0x81u || type == 0x82u) return kGpuNotModelled;
                break;
            }
            const u32 words = (at + 1u - 0x3D0u) * 4u;   // up to the entry that ended the list
            if (words <= 64u) {
                std::memcpy(g.light_mem, mem + 0x3D0u * 16u, words * 4u);
                g.light_words = words;
                g.light_x = light_x;
                g.light_z = light_z;
            }
        }
        set4(g.ambient, amb_x, amb_y, amb_z, 0.0f);
        for (unsigned d = directional; d < 3u; ++d) {
            set4(g.light_dir[d], 0, 0, 0, 0);
            set4(g.light_colour[d], 0, 0, 0, 0);
        }
        g.has_dir_lights = directional > 0u;
    }
    set4(g.material, mat.x, mat.y, mat.z, mat.w);

    const Batch b{in, top, n};
    const Vec v28 = b.vf(28), v29 = b.vf(29), v30 = b.vf(30), v31 = b.vf(31), v2 = b.vf(2);
    const Vec fog = load(mem, 0x3F6);
    if (ortho) {
        set4(g.proj[0], v28.x, v28.y, v28.z, v28.w);
        set4(g.proj[1], v29.x, v29.y, v29.z, v29.w);
        set4(g.proj[2], v30.x, v30.y, v30.z, v30.w);
        set4(g.proj[3], v31.x, v31.y, v31.z, v31.w);
    } else {
        set4(g.proj[0], v28.x, v28.y, 0, v28.w);
        set4(g.proj[1], v29.x, v29.y, 0, v29.w);
        set4(g.proj[2], v30.x, v30.y, v30.z, v30.w);
        set4(g.proj[3], v31.x, v31.y, v31.z, v31.w);
    }
    set4(g.offset, v2.x, v2.y, v2.z, 0);
    g.ortho = ortho;
    g.fog = ortho ? ((g.prim & 0x20u) != 0) : (((flags & 1u) != 0) || (end.clipper && (((g.prim | ((dual || env) ? g.prim2 : 0ull)) & 0x20u) != 0)));
    g.xyzf = ((bits(tag.z) >> 8) & 0xFu) == 4u;
    g.clipper = end.clipper;
    g.fog_scale = fog.z;
    g.fog_near = fog.w;

    // Which facings keep their triangles: ADC set means the GS leaves the triangle out.
    if (no_cull) {
        g.keep_negative = true;
        g.keep_positive = true;
    } else {
        const u32 cull = u16_at(mem, 0x3FF, 8);
        bool adc_negative, adc_positive;
        if (g.fog || ortho) {
            adc_negative = cull != 32u;
            adc_positive = cull != 0u;
        } else {
            const u32 base = ((cull + 0x7FE0u) | 0x7FE0u) & 0xFFFFu;
            adc_negative = ((base + 32u) >> 15) & 1u;
            adc_positive = (base >> 15) & 1u;
        }
        g.keep_negative = !adc_negative;
        g.keep_positive = !adc_positive;
        if (!g.keep_negative && !g.keep_positive) return (cull != 0u && cull != 32u) ? kGpuNotModelled : -1;
    }

    g.n = n;
    g.skin = skin;
    g.slots = 0;
    plan = BatchPlan{skin, dual, dual_uv1, dual_uv2, env, uv_xform, list, direct, has_lights, need_normals, top, n, pos_w, uv_w, nrm_step};
    return 0;
}

int gpu_batch_fill(const Input &in, Gpu &g, const BatchPlan &plan, GpuVertex *dst) {
    const std::uint8_t *mem = in.mem;
    if (!plan.skin) {
        G3_PROF_EXTRA(12);
        if (plan.direct) {
            if (plan.dual) fill_dual_direct(mem, g, dst, in.streams, plan.n, plan.pos_w, plan.uv_w, plan.nrm_step, plan.normals, plan.dual_uv1, plan.dual_uv2);
            else fill_generic_direct(mem, g, dst, in.streams, plan.n, plan.pos_w, plan.uv_w, plan.nrm_step, plan.normals, plan.uv_xform);
        } else {
            auto *o = reinterpret_cast<u32 *>(dst);
            const u32 uv_base = plan.dual ? 963u : 956u;
            const bool do_uv1 = plan.dual_uv1 || plan.uv_xform;
            const bool need_mat = do_uv1 || plan.dual_uv2;
            const Vec m0 = need_mat ? load(mem, uv_base) : Vec{};
            const Vec m1 = need_mat ? load(mem, uv_base + 1u) : Vec{};
            const Vec m2 = need_mat ? load(mem, uv_base + 2u) : Vec{};
            u32 amin = 255u, amax = 0u;
            for (u32 k = 0; k < plan.n; ++k, o += 12) {
                const u32 at = plan.top + 4u * k;
                u32 p[4], t[4];
                words(mem, at, p);
                words(mem, at + 1u, t);
                u32 col, nrm;
                if (!colour_normal(mem, at + 2u, plan.normals, col, nrm)) return kGpuRange;
                const u32 a = col >> 24;
                if (a < amin) amin = a;
                if (a > amax) amax = a;
                o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
                if (do_uv1) {
                    float u1, v1;
                    std::memcpy(&u1, &t[0], 4);
                    std::memcpy(&v1, &t[1], 4);
                    const float u1o = m2.x + m0.x * u1 + m1.x * v1;
                    const float v1o = m2.y + m0.y * u1 + m1.y * v1;
                    std::memcpy(&o[3], &u1o, 4);
                    std::memcpy(&o[4], &v1o, 4);
                } else {
                    o[3] = t[0]; o[4] = t[1];
                }
                o[5] = col; o[6] = nrm;
                o[7] = kOne; o[8] = 0; o[9] = 0; o[10] = 0; o[11] = 0;
                if (plan.dual) {
                    float u2, v2;
                    std::memcpy(&u2, &t[2], 4);
                    std::memcpy(&v2, &t[3], 4);
                    if (plan.dual_uv2) {
                        g.uv2[k][0] = m2.z + m0.z * u2 + m1.z * v2;
                        g.uv2[k][1] = m2.w + m0.w * u2 + m1.w * v2;
                    } else {
                        g.uv2[k][0] = u2;
                        g.uv2[k][1] = v2;
                    }
                }
            }
            g.alpha_min = static_cast<std::uint8_t>(amin);
            g.alpha_max = static_cast<std::uint8_t>(amax);
        }
        if (plan.list) list_to_strip(g);
        return 0;
    }

    const u32 batch = ++g_bones.batch;
    if (++g_bones.seen_gen == 0u) {
        std::memset(g_bones.address_seen, 0, sizeof g_bones.address_seen);
        g_bones.seen_gen = 1u;
    }
    const std::uint16_t seen_gen = g_bones.seen_gen;
    g_bones.use_count = 0;
    G3_PROF_EXTRA(13);
    if (plan.direct) return fill_skin_direct(mem, plan.top, g, dst, in.streams, plan.n, plan.pos_w, plan.uv_w, plan.nrm_step, plan.normals, plan.uv_xform, batch, seen_gen);
    return fill_skin_indirect(mem, plan.top, g, dst, plan.n, plan.normals, plan.uv_xform, batch, seen_gen);
}

int gpu_batch(const Input &in, Gpu &g, End &end) {
    BatchPlan plan;
    const int got = gpu_batch_setup(in, g, end, plan);
    if (got != 0) return got;
    return gpu_batch_fill(in, g, plan, g.v);
}

#ifndef G3_HOST_CHECK
// The MSCAL 0 init (042/064 L_0000-L_0190): transforms the projection matrix at M[0x3F0..0x3F3] by the viewport scale M[0x3F7],
// sets the screen offset vf2 from M[0x3F8]+M[0x3F9], sets vi13/vi14, and stores the branch target in M[0x3FF].y.
void init_state(int program, std::uint8_t *mem, ps2::Vu0Regs &r) {
    const Vec m5 = load(mem, 0x3F7), m2 = load(mem, 0x3F8), m3 = load(mem, 0x3F9);
    if (program == 41 || program == 65 || program == 76 || program == 102) {
        for (int k = 0; k < 4; ++k) {
            const Vec q = load(mem, 0x3F0u + static_cast<u32>(k));
            const float x = (k == 3 ? q.x - 0.5f : q.x) * m5.x;
            const float y = (k == 3 ? q.y - 0.5f : q.y) * m5.y;
            const float z = q.z * m5.z;
            r.vf[28 + k].w[0] = bits(x);
            r.vf[28 + k].w[1] = bits(y);
            r.vf[28 + k].w[2] = bits(z);
            r.vf[28 + k].w[3] = bits(q.w);
        }
        r.vf[2].w[0] = bits(m2.x + m3.x);
        r.vf[2].w[1] = bits(m2.y + m3.y);
        r.vf[2].w[2] = bits(m2.z);
        r.vf[2].w[3] = 0u;
        r.i = 0x3F000000u;
        const u32 flags = u16_at(mem, 0x3FF, 0);
        if (program == 76) {
            const bool full_clip = (flags & 2u) && (flags & 8u);
            r.vi[12] = 360u;
            r.vi[13] = full_clip ? 436u : 473u;
            r.vi[14] = full_clip ? 512u : 586u;
            u32 i15;
            if (flags & 2u) {
                if (flags & 8u) i15 = (flags & 4u) ? 1174u : 816u;
                else i15 = (flags & 4u) ? 744u : 654u;
            } else {
                i15 = (flags & 4u) ? 596u : 520u;
            }
            std::memcpy(mem + 0x3FF * 16 + 4, &i15, 4);
            return;
        }
        r.vi[13] = program == 41 ? 552u : 430u;
        r.vi[14] = program == 41 ? 764u : 565u;
        u32 i12;
        if (program == 41) {
            if (flags & 2u) {
                if (flags & 8u) i12 = (flags & 4u) ? 1105u : 703u;
                else i12 = (flags & 4u) ? 633u : 545u;
            } else {
                i12 = (flags & 4u) ? 492u : 420u;
            }
        } else {
            if (flags & 2u) {
                if (flags & 8u) i12 = (flags & 4u) ? 1195u : 793u;
                else i12 = (flags & 4u) ? 723u : 635u;
            } else {
                i12 = (flags & 4u) ? 582u : 510u;
            }
            if (program == 102) i12 += 21u;
        }
        std::memcpy(mem + 0x3FF * 16 + 4, &i12, 4);
        return;
    }
    for (int k = 0; k < 4; ++k) {
        const Vec q = load(mem, 0x3F0u + static_cast<u32>(k));
        const float half_w = q.w * 0.5f;
        const float x = (q.x - half_w) * m5.x;
        const float y = (q.y - half_w) * m5.y;
        const float z = k == 3 ? 1.0f * m5.z : 0.0f;
        r.vf[28 + k].w[0] = bits(x);
        r.vf[28 + k].w[1] = bits(y);
        r.vf[28 + k].w[2] = bits(z);
        r.vf[28 + k].w[3] = bits(q.w);
    }
    r.vf[2].w[0] = bits(m2.x + m3.x);
    r.vf[2].w[1] = bits(m2.y + m3.y);
    r.vf[2].w[2] = bits(m2.z);
    r.vf[2].w[3] = 0u;
    r.i = 0x3F000000u;
    const u32 flags = u16_at(mem, 0x3FF, 0);
    if (program == 43 || program == 44 || program == 51 || program == 53 || program == 59 || program == 73 || program == 75 || program == 95) {
        const bool full_clip = (flags & 2u) && (flags & 8u);
        const bool skin_3buf = program == 73 || program == 75;
        r.vi[12] = skin_3buf ? 360u : 448u;
        r.vi[13] = skin_3buf ? (full_clip ? 436u : 473u) : (full_clip ? 563u : 617u);
        r.vi[14] = skin_3buf ? (full_clip ? 512u : 586u) : (full_clip ? 678u : 786u);
        u32 i15;
        if (program == 43) {   // ELF 061 dual[1]
            if (flags & 2u) {
                if (flags & 8u) i15 = (flags & 4u) ? 1416u : 1058u;
                else if (flags & 4u) i15 = (flags & 1u) ? 980u : 814u;
                else i15 = (flags & 1u) ? 874u : 724u;
            } else if (flags & 4u) {
                i15 = (flags & 1u) ? 655u : 508u;
            } else {
                i15 = (flags & 1u) ? 565u : 437u;
            }
        } else if (program == 44) {   // ELF 097 dualuv[1]
            if (flags & 2u) {
                if (flags & 8u) i15 = (flags & 4u) ? 1437u : 1079u;
                else if (flags & 4u) i15 = (flags & 1u) ? 1001u : 835u;
                else i15 = (flags & 1u) ? 895u : 745u;
            } else if (flags & 4u) {
                i15 = (flags & 1u) ? 676u : 529u;
            } else {
                i15 = (flags & 1u) ? 586u : 458u;
            }
        } else if (program == 59) {   // ELF 059 dual[0] no-cull
            if (flags & 2u) {
                if (flags & 8u) i15 = (flags & 4u) ? 943u : 626u;
                else i15 = (flags & 1u) ? 562u : 515u;
            } else {
                i15 = (flags & 1u) ? 465u : 423u;
            }
        } else if (program == 95) {   // ELF 095 dualuv[0] no-cull
            if (flags & 2u) {
                if (flags & 8u) i15 = (flags & 4u) ? 964u : 647u;
                else i15 = (flags & 1u) ? 583u : 536u;
            } else {
                i15 = (flags & 1u) ? 486u : 444u;
            }
        } else if (program == 51) {   // ELF 051 gen_env[0]
            if (flags & 2u) {
                if (flags & 8u) i15 = (flags & 4u) ? 970u : 646u;
                else i15 = (flags & 1u) ? 581u : 534u;
            } else {
                i15 = (flags & 1u) ? 484u : 444u;
            }
        } else if (program == 53) {   // ELF 053 gen_env[1]
            if (flags & 2u) {
                if (flags & 8u) i15 = (flags & 4u) ? 1438u : 1073u;
                else if (flags & 4u) i15 = (flags & 1u) ? 995u : 829u;
                else i15 = (flags & 1u) ? 889u : 739u;
            } else if (flags & 4u) {
                i15 = (flags & 1u) ? 670u : 526u;
            } else {
                i15 = (flags & 1u) ? 581u : 458u;
            }
        } else if (program == 73) {   // ELF 073 skin_env[0]
            if (flags & 2u) {
                if (flags & 8u) i15 = (flags & 4u) ? 1046u : 722u;
                else i15 = (flags & 1u) ? 657u : 610u;
            } else {
                i15 = (flags & 1u) ? 560u : 520u;
            }
        } else {   // ELF 075 skin_env[1]
            if (flags & 2u) {
                if (flags & 8u) i15 = (flags & 4u) ? 1514u : 1149u;
                else if (flags & 4u) i15 = (flags & 1u) ? 1071u : 905u;
                else i15 = (flags & 1u) ? 965u : 815u;
            } else if (flags & 4u) {
                i15 = (flags & 1u) ? 746u : 602u;
            } else {
                i15 = (flags & 1u) ? 657u : 534u;
            }
        }
        std::memcpy(mem + 0x3FF * 16 + 4, &i15, 4);
        return;
    }
    if (program == 46) {   // ELF 046 g3_generic[0] no-cull
        r.vi[13] = 552u;
        r.vi[14] = 764u;
        u32 i12;
        if (flags & 2u) {
            if (flags & 8u) i12 = (flags & 4u) ? 987u : 608u;
            else i12 = (flags & 1u) ? 547u : 506u;
        } else {
            i12 = (flags & 1u) ? 458u : 422u;
        }
        std::memcpy(mem + 0x3FF * 16 + 4, &i12, 4);
        return;
    }
    const bool skin = program == 64 || program == 66;
    const bool uv_xform = program == 45 || program == 66;
    r.vi[13] = skin ? 430u : (uv_xform ? 544u : 552u);
    r.vi[14] = skin ? 565u : (uv_xform ? 750u : 764u);
    u32 i12;
    if (flags & 2u) {
        if (flags & 8u) i12 = (flags & 4u) ? (skin ? 1528u : 1438u) : (skin ? 1113u : 1023u);
        else if (flags & 4u) i12 = (flags & 1u) ? (skin ? 1037u : 947u) : (skin ? 875u : 785u);
        else i12 = (flags & 1u) ? (skin ? 933u : 843u) : (skin ? 800u : 710u);
    } else if (flags & 4u) {
        i12 = (flags & 1u) ? (skin ? 732u : 642u) : (skin ? 589u : 499u);
    } else {
        i12 = (flags & 1u) ? (skin ? 644u : 554u) : (skin ? 524u : 434u);
    }
    if (uv_xform) i12 += 21u;
    std::memcpy(mem + 0x3FF * 16 + 4, &i12, 4);
}

// The batch end (042 0x3940, 064 0x3C10): the two output buffers vi13 and vi14 change places, vi15 points back at the batch
// start (0x198 / 8) for the next MSCNT, and the E bit leaves TPC just past the end.
void leave_state(int program, const End &end, ps2::Vu0Regs &r) {
    if (program == 43 || program == 44 || program == 51 || program == 53 || program == 59 || program == 73 || program == 75 || program == 76 || program == 95) {
        if (end.swap) {
            const std::uint16_t a = r.vi[12], b = r.vi[13], c = r.vi[14];
            r.vi[12] = c;
            r.vi[13] = a;
            r.vi[14] = b;
        }
        if (end.clipper)
            for (int i = 28; i <= 30; ++i) {
                const float w = f(r.vf[i].w[3]) + 0.0f;
                r.vf[i].w[2] = bits(w - w);
            }
        r.vi[15] = (program == 51 || program == 59 || program == 73 || program == 76 || program == 95) ? 40 : 54;
        r.tpc = program == 43 ? 0x36F0u : program == 44 ? 0x3798u : program == 51 ? 0x28A8u :
                program == 53 ? 0x3808u : program == 59 ? 0x2768u : program == 73 ? 0x2B08u :
                program == 75 ? 0x3A68u : program == 95 ? 0x2810u : 0x2F88u;
        return;
    }
    if (end.swap) {
        const std::uint16_t t = r.vi[13];
        r.vi[13] = r.vi[14];
        r.vi[14] = t;
    }
    if (end.clipper)
        for (int i = 28; i <= 30; ++i) {
            const float w = f(r.vf[i].w[3]) + 0.0f;   // the clipper's first pass sets z to 0 + w, its end subtracts it from itself
            r.vf[i].w[2] = bits(w - w);
        }
    r.vi[15] = program == 46 ? 39 : (program == 41 || program == 65 || program == 102) ? 37 : 51;
    r.tpc = program == 41 ? 0x2E68u : program == 46 ? 0x2AA8u : program == 102 ? 0x31E0u : program == 65 ? 0x3138u :
            program == 66 ? 0x3CE0u : program == 64 ? 0x3C38u : program == 45 ? 0x3A10u : 0x3968u;
}
#endif

}
