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
    Vec vf(int i) const { return Vec{f(in.vf[i][0]), f(in.vf[i][1]), f(in.vf[i][2]), f(in.vf[i][3])}; }
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

// A weight whose exponent is zero is a denormal, which the VU reads as zero: its low bits are only the bone's address.
inline float weight_of(float w) { return (bits(w) & 0x7F800000u) ? w : 0.0f; }

// The colour (0-255) and the normal (a signed byte) of a vertex, from their integer lanes. False when they do not fit.
inline bool colour_normal(const std::uint8_t *mem, u32 at, bool lights, GpuVertex &o) {
    std::int32_t c[4], n[4];
    std::memcpy(c, mem + (at & 1023u) * 16u, 16);
    std::memcpy(n, mem + ((at + 1u) & 1023u) * 16u, 16);
    for (int i = 0; i < 4; ++i) {
        if (c[i] < 0 || c[i] > 255) return false;
        o.colour[i] = static_cast<std::uint8_t>(c[i]);
        if (!lights) {
            o.normal[i] = 0;
        } else {
            if (i < 3 && (n[i] < -128 || n[i] > 127)) return false;
            o.normal[i] = i < 3 ? static_cast<std::int8_t>(n[i]) : 0;
        }
    }
    return true;
}

}

int gpu_batch(const Input &in, Gpu &g, End &end) {
    end = End{};
    const std::uint8_t *mem = in.mem;
    const u32 flags = mem[0x3FF * 16];
    const u32 top = in.top & 1023u, n = in.itop & 1023u;
    const bool skin = in.program == 64;
    if (n == 0 || n > 256u || top + (skin ? 5u : 4u) * n > 0x3D0u) return kGpuNotModelled;
    if (flags & 2u) {
        if (skin || !(flags & 8u) || !clipper_culls_all(in, top, n)) return kGpuNotModelled;
        end.swap = false;
        end.clipper = true;
        return -1;
    }
    if (flags & 4u) return kGpuNotModelled;
    static Lighting L;
    if (!prepare_lighting(mem, L)) return kGpuNotModelled;

    // PRIM from the GIF tag: a Gouraud triangle strip with ST coordinates is what the shader and the strip indices assume.
    const Vec tag = load(mem, 0x3FA);
    const u32 tag_hi = bits(tag.y);
    if (!(tag_hi & (1u << 14))) return kGpuPrim;   // no PRE: PRIM would stay as it was
    g.prim = (tag_hi >> 15) & 0x7FFu;
    if ((g.prim & 7u) != 4u || !(g.prim & 8u) || (g.prim & 0x100u)) return kGpuPrim;

    // Lights: ambient ones summed; directional ones each get N.L clamped at 0. Without lights, the colour times the material.
    for (auto &d : g.light_dir) set4(d, 0, 0, 0, 0);
    for (auto &c : g.light_colour) set4(c, 0, 0, 0, 0);
    set4(g.ambient, 0, 0, 0, 0);
    unsigned directional = 0;
    if (L.lights) {
        for (unsigned i = 0; i < L.count; ++i) {
            const Light &l = L.l[i];
            if (!l.directional) {
                g.ambient[0] += l.ax, g.ambient[1] += l.ay, g.ambient[2] += l.az;
                continue;
            }
            if (directional == 3) return kGpuLights;
            set4(g.light_dir[directional], l.dx, l.dy, l.dz, 0);
            set4(g.light_colour[directional], l.ax, l.ay, l.az, 0);
            ++directional;
        }
    }
    set4(g.material, L.mat.x, L.mat.y, L.mat.z, L.mat.w);

    const Batch b{in, top, n};
    const Vec v28 = b.vf(28), v29 = b.vf(29), v30 = b.vf(30), v31 = b.vf(31), v2 = b.vf(2);
    set4(g.proj[0], v28.x, v28.y, 0, v28.w);
    set4(g.proj[1], v29.x, v29.y, 0, v29.w);
    set4(g.proj[2], v30.x, v30.y, v30.z, v30.w);
    set4(g.proj[3], v31.x, v31.y, v31.z, v31.w);
    set4(g.offset, v2.x, v2.y, v2.z, 0);
    g.fog = flags & 1u;
    const Vec fog = load(mem, 0x3F6);
    g.fog_scale = fog.z;
    g.fog_near = fog.w;

    // Which facings keep their triangles: ADC set means the GS leaves the triangle out.
    const u32 cull = u16_at(mem, 0x3FF, 8);
    bool adc_negative, adc_positive;
    if (g.fog) {
        adc_negative = cull != 32u;
        adc_positive = cull != 0u;
    } else {
        const u32 base = ((cull + 0x7FE0u) | 0x7FE0u) & 0xFFFFu;
        adc_negative = ((base + 32u) >> 15) & 1u;
        adc_positive = (base >> 15) & 1u;
    }
    g.keep_negative = !adc_negative;
    g.keep_positive = !adc_positive;
    if (!g.keep_negative && !g.keep_positive) return -1;

    g.n = n;
    g.skin = skin;
    g.slots = 0;
    if (!skin) {
        for (u32 k = 0; k < n; ++k) {
            const u32 at = top + 4u * k;
            GpuVertex &o = g.v[k];
            const Vec p = load(mem, at), t = load(mem, at + 1u);
            o.x = p.x, o.y = p.y, o.z = p.z, o.u = t.x, o.v = t.y;
            if (!colour_normal(mem, at + 2u, L.lights, o)) return kGpuRange;
            o.weight[0] = 1.0f, o.weight[1] = o.weight[2] = o.weight[3] = 0.0f;
            o.bone[0] = o.bone[1] = o.bone[2] = o.bone[3] = 0;
        }
        return 0;
    }

    // Skin: each distinct bone matrix gets a palette slot. The game sends each batch's bones to whatever addresses it likes, so
    // a slot is found by the matrix itself: the same matrix as an earlier batch's keeps that slot, which the shader still has.
    // A new one takes the slot unused for longest. A weight of zero points at slot 0 (whatever it holds, times zero).
    static std::uint32_t address_seen[1024], slot_hash[kGpuBones], used_in[kGpuBones], batch = 0;
    static std::uint8_t address_slot[1024];
    static bool slot_full[kGpuBones];
    ++batch;
    for (u32 k = 0; k < n; ++k) {
        const u32 at = top + 5u * k;
        GpuVertex &o = g.v[k];
        const Vec p = load(mem, at), t = load(mem, at + 1u), w = load(mem, at + 4u);
        o.x = p.x, o.y = p.y, o.z = p.z, o.u = t.x, o.v = t.y;
        if (!colour_normal(mem, at + 2u, L.lights, o)) return kGpuRange;
        const float ws[4] = {weight_of(w.x), weight_of(w.y), weight_of(w.z), weight_of(w.w)};
        for (int i = 0; i < 4; ++i) {
            o.weight[i] = ws[i];
            if (ws[i] == 0.0f) {
                o.bone[i] = 0;   // slot 0 times zero: not counted in slots, whatever slot 0 holds when it is drawn
                continue;
            }
            const u32 address = u16_at(mem, at + 4u, 4u * i) & 1023u;
            if (address_seen[address] != batch) {
                // The program's rows 0-2 (rotation) and 3 (translation), turned: vector c is (row0.c, row1.c, row2.c, row3.c).
                Vec row[4];
                for (u32 r = 0; r < 4; ++r) row[r] = load(mem, address + 696u + r);
                float m[3][4];
                set4(m[0], row[0].x, row[1].x, row[2].x, row[3].x);
                set4(m[1], row[0].y, row[1].y, row[2].y, row[3].y);
                set4(m[2], row[0].z, row[1].z, row[2].z, row[3].z);
                std::uint32_t h = 0x811C9DC5u, word[12];
                std::memcpy(word, m, sizeof word);
                for (std::uint32_t x : word) h = (h ^ x) * 0x01000193u;
                int slot = -1;
                for (unsigned j = 0; j < kGpuBones && slot < 0; ++j)
                    if (slot_full[j] && slot_hash[j] == h && !std::memcmp(g.bone[3 * j], m, sizeof m)) slot = static_cast<int>(j);
                if (slot < 0) {
                    slot = 0;
                    for (unsigned j = 1; j < kGpuBones; ++j)
                        if (used_in[j] < used_in[slot]) slot = static_cast<int>(j);
                    if (used_in[slot] == batch) return kGpuBoneCount;
                    std::memcpy(g.bone[3 * slot], m, sizeof m);
                    slot_hash[slot] = h;
                    slot_full[slot] = true;
                }
                used_in[slot] = batch;
                address_seen[address] = batch;
                address_slot[address] = static_cast<std::uint8_t>(slot);
            }
            o.bone[i] = static_cast<std::uint8_t>(3 * address_slot[address]);
            g.slots |= 1u << address_slot[address];
        }
    }
    return 0;
}

#ifndef G3_HOST_CHECK
// The batch end (042 0x3940, 064 0x3C10): the two output buffers vi13 and vi14 change places, vi15 points back at the batch
// start (0x198 / 8) for the next MSCNT, and the E bit leaves TPC just past the end.
void leave_state(int program, const End &end, ps2::Vu0Regs &r) {
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
    r.vi[15] = 51;
    r.tpc = program == 64 ? 0x3C38u : 0x3968u;
}
#endif

}
