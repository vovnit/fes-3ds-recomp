"""New 3DS support for the PS2 Web Recomp runtime (see port3ds/README.md).

Lets the runtime build with devkitARM's GCC for the 3DS's 32-bit ARM11:
- vu_ops.hpp: two Clang-only vector built-ins get GCC-friendly versions (the code generated for Clang is unchanged).
- spu2.cpp, dmac.cpp: std::clamp and std::min get an explicit type, because int32_t is long on this target.
- memory: on the 3DS the 32 MB of PS2 RAM sits at a fixed address in the heap area instead of in the program's data,
  because the 3DS code region is limited to about 63 MB and the translated game already takes about 40 MB of it.
  port3ds/main3ds.cpp maps that memory at start-up. A constant address also saves a load on every guest memory access.

Run in the ps2-web-recomp folder:  python3 ps2run_3ds_patch.py [folder]
(idempotent; refuses unknown source)
"""
import sys

root = sys.argv[1] if len(sys.argv) > 1 else '.'

EDITS = {
    'runtime/include/ps2/vu_ops.hpp': [
        ('''PS2_VU_INLINE v4u vfix(v4u x) {
    const v4i clamped = __builtin_elementwise_min((v4i)(x & 0x7FFFFFFFu), (v4i){0x7F7FFFFF, 0x7F7FFFFF, 0x7F7FFFFF, 0x7F7FFFFF});
''', '''PS2_VU_INLINE v4i vmin_signed(v4i a, v4i b) {   // ps2run_3ds_patch.py
#if defined(__clang__)
    return __builtin_elementwise_min(a, b);
#else
    return a < b ? a : b;
#endif
}
PS2_VU_INLINE v4u vfix(v4u x) {
    const v4i clamped = vmin_signed((v4i)(x & 0x7FFFFFFFu), (v4i){0x7F7FFFFF, 0x7F7FFFFF, 0x7F7FFFFF, 0x7F7FFFFF});
'''),
        ('''    if (__builtin_reduce_or((v4i)(x + 0x01000000u > 0x02000000u))) {
''', '''#if defined(__clang__)
    const bool any_large = __builtin_reduce_or((v4i)(x + 0x01000000u > 0x02000000u));
#else
    const v4i large = (v4i)(x + 0x01000000u > 0x02000000u);   // ps2run_3ds_patch.py
    const bool any_large = (large[0] | large[1] | large[2] | large[3]) != 0;
#endif
    if (any_large) {
'''),
    ],
    'runtime/src/iop/spu2.cpp': [
        ('level = std::clamp(level + delta, 0, 0x7FFF);', 'level = std::clamp<std::int32_t>(level + delta, 0, 0x7FFF);'),
        ('sample = std::clamp(sample, -32768, 32767);', 'sample = std::clamp<std::int32_t>(sample, -32768, 32767);'),
        ('return static_cast<std::int16_t>(std::clamp(v, -32768, 32767));', 'return static_cast<std::int16_t>(std::clamp<std::int32_t>(v, -32768, 32767));'),
    ],
    'runtime/src/system.cpp': [
        # One sized read. Reading byte by byte into a growing vector needs 24 MB at once for this 9 MB file, too much for the 3DS heap.
        ('''    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    const std::vector<std::uint8_t> data{std::istreambuf_iterator<char>(file), {}};
''', '''    std::FILE *file = std::fopen(path.c_str(), "rb");   // ps2run_3ds_patch.py
    if (!file) return false;
    std::fseek(file, 0, SEEK_END);
    const long file_size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    std::vector<std::uint8_t> data(file_size > 0 ? static_cast<std::size_t>(file_size) : 0);
    const std::size_t file_read = data.empty() ? 0 : std::fread(data.data(), 1, data.size(), file);
    std::fclose(file);
    if (file_read != data.size()) return false;
'''),
    ],
    # The profiler in port3ds/prof.hpp: nothing changes unless PS2_PICA is defined.
    'runtime/include/ps2/system.hpp': [
        ('#include "cpu.hpp"\n\n#include <cstdint>\n#include <functional>\n', '''#include "cpu.hpp"

#include <cstdint>
#include <functional>
#ifdef PS2_PICA
#include "prof.hpp"
#else
#define PS2_PROF(counter)
#endif
'''),
    ],
    'runtime/src/vu_micro.cpp': [
        ('void run_entry(int unit, std::uint32_t pc) {\n', 'void run_entry(int unit, std::uint32_t pc) {\n    PS2_PROF(ps2::prof::Vu);\n'),
        ('void xgkick(Unit &u, std::uint32_t address) {\n', 'void xgkick(Unit &u, std::uint32_t address) {\n    PS2_PROF(ps2::prof::Gif);\n'),
    ],
    'runtime/src/system.cpp#prof': [
        ('        iop::run(t - g_iop_synced);\n', '        PS2_PROF(ps2::prof::Iop);\n        iop::run(t - g_iop_synced);\n'),
    ],
    'runtime/src/iop/spu2.cpp#prof': [
        ('void tick() {\n', 'void tick() {\n    PS2_PROF(ps2::prof::Spu);\n'),
        ('#include "ps2/state.hpp"\n', '#include "ps2/state.hpp"\n#include "ps2/system.hpp"\n'),
    ],
    'runtime/src/ipu/ipu.cpp': [
        ('#include "ps2/state.hpp"\n', '#include "ps2/state.hpp"\n#include "ps2/system.hpp"\n'),
    ],
    # Video on the 3DS. The upstream decoder is written to be exact: a double-precision IDCT with 1024 multiplications per block,
    # 64-entry loops over every block and a table of 64 KB entries for the coefficients. Here the same stream goes through an
    # integer IDCT, which differs from the exact one by at most 1 or 2 in a sample (the mismatch toggle of the last coefficient
    # is left out too, for the same reason), two small tables, and bulk copies.
    'runtime/src/ipu/ipu.cpp#speed': [
        ('''struct DctLookup {
    unsigned bits = 0;
    std::vector<DctEntry> entries;
};
''', '''struct DctLookup {
    unsigned bits = 0;
#ifdef __3DS__
    // Every code over 10 bits starts with six zeros, so two tables of 1024 do the work of one of 65536 (512 KB saved in all).
    DctEntry head[1024], tail[1024];
    const DctEntry &at(std::uint32_t v) const { return v >> 10 ? head[v >> 6] : tail[v]; }
#else
    std::vector<DctEntry> entries;
    const DctEntry &at(std::uint32_t v) const { return entries[v]; }
#endif
};
'''),
        ('''    for (const Dct &e : table) l.bits = std::max<unsigned>(l.bits, e.length);
    l.entries.resize(std::size_t{1} << l.bits);
    for (const Dct &e : table) {
        const unsigned shift = l.bits - e.length;
        for (std::uint32_t i = 0; i < (1u << shift); ++i) l.entries[(static_cast<std::uint32_t>(e.code) << shift) | i] = {e.length, e.run, e.level};
    }
''', '''    for (const Dct &e : table) l.bits = std::max<unsigned>(l.bits, e.length);
#ifdef __3DS__
    for (const Dct &e : table) {
        const unsigned shift = l.bits - e.length;
        for (std::uint32_t i = 0; i < (1u << shift); ++i) {
            const std::uint32_t v = (static_cast<std::uint32_t>(e.code) << shift) | i;
            (v >> 10 ? l.head[v >> 6] : l.tail[v]) = {e.length, e.run, e.level};
        }
    }
#else
    l.entries.resize(std::size_t{1} << l.bits);
    for (const Dct &e : table) {
        const unsigned shift = l.bits - e.length;
        for (std::uint32_t i = 0; i < (1u << shift); ++i) l.entries[(static_cast<std::uint32_t>(e.code) << shift) | i] = {e.length, e.run, e.level};
    }
#endif
'''),
        ('''    std::uint64_t v = 0;
    const std::size_t byte = bit >> 3;
    for (std::size_t i = 0; i < 8; ++i) v = (v << 8) | (byte + i < g_in.size() ? g_in[byte + i] : 0u);
''', '''    std::uint64_t v = 0;
    const std::size_t byte = bit >> 3;
#ifdef __3DS__
    if (byte + 8 <= g_in.size()) {
        std::memcpy(&v, g_in.data() + byte, 8);
        return static_cast<std::uint32_t>((__builtin_bswap64(v) << (bit & 7u)) >> (64 - n));
    }
#endif
    for (std::size_t i = 0; i < 8; ++i) v = (v << 8) | (byte + i < g_in.size() ? g_in[byte + i] : 0u);
'''),
        ('''    const DctEntry &e = l.entries[peek_soft(l.bits, avail)];
''', '''    const DctEntry &e = l.at(peek_soft(l.bits, avail));
'''),
        ('''void decode_block(std::int16_t *out, int channel, bool intra, std::uint32_t qsc) {
    int coef[64] = {};
''', '''#ifdef __3DS__
constexpr int kW1 = 2841, kW2 = 2676, kW3 = 2408, kW5 = 1609, kW6 = 1108, kW7 = 565;   // 2048 * sqrt(2) * cos(n * pi / 16)

inline int clip9(int v) {   // to -256..255
    int r;
    __asm__("ssat %0, #9, %1" : "=r"(r) : "r"(v));
    return r;
}

inline std::uint32_t sat8(int v) {   // v >> 7, to 0..255
    std::uint32_t r;
    __asm__("usat %0, #8, %1, asr #7" : "=r"(r) : "r"(v));
    return r;
}

inline void idct_row(int *b) {
    int x1 = b[4] * 2048, x2 = b[6], x3 = b[2], x4 = b[1], x5 = b[7], x6 = b[5], x7 = b[3];
    if (!(x1 | x2 | x3 | x4 | x5 | x6 | x7)) {
        const int v = b[0] * 8;
        for (int i = 0; i < 8; ++i) b[i] = v;
        return;
    }
    int x0 = b[0] * 2048 + 128;
    int x8 = kW7 * (x4 + x5);
    x4 = x8 + (kW1 - kW7) * x4;
    x5 = x8 - (kW1 + kW7) * x5;
    x8 = kW3 * (x6 + x7);
    x6 = x8 - (kW3 - kW5) * x6;
    x7 = x8 - (kW3 + kW5) * x7;
    x8 = x0 + x1;
    x0 -= x1;
    x1 = kW6 * (x3 + x2);
    x2 = x1 - (kW2 + kW6) * x2;
    x3 = x1 + (kW2 - kW6) * x3;
    x1 = x4 + x6;
    x4 -= x6;
    x6 = x5 + x7;
    x5 -= x7;
    x7 = x8 + x3;
    x8 -= x3;
    x3 = x0 + x2;
    x0 -= x2;
    x2 = (181 * (x4 + x5) + 128) >> 8;
    x4 = (181 * (x4 - x5) + 128) >> 8;
    b[0] = (x7 + x1) >> 8;
    b[1] = (x3 + x2) >> 8;
    b[2] = (x0 + x4) >> 8;
    b[3] = (x8 + x6) >> 8;
    b[4] = (x8 - x6) >> 8;
    b[5] = (x0 - x4) >> 8;
    b[6] = (x3 - x2) >> 8;
    b[7] = (x7 - x1) >> 8;
}

inline void idct_column(const int *b, std::int16_t *o, int stride) {
    int x1 = b[32] * 256, x2 = b[48], x3 = b[16], x4 = b[8], x5 = b[56], x6 = b[40], x7 = b[24];
    if (!(x1 | x2 | x3 | x4 | x5 | x6 | x7)) {
        const std::int16_t v = static_cast<std::int16_t>(clip9((b[0] + 32) >> 6));
        for (int i = 0; i < 8; ++i) o[i * stride] = v;
        return;
    }
    int x0 = b[0] * 256 + 8192;
    int x8 = kW7 * (x4 + x5) + 4;
    x4 = (x8 + (kW1 - kW7) * x4) >> 3;
    x5 = (x8 - (kW1 + kW7) * x5) >> 3;
    x8 = kW3 * (x6 + x7) + 4;
    x6 = (x8 - (kW3 - kW5) * x6) >> 3;
    x7 = (x8 - (kW3 + kW5) * x7) >> 3;
    x8 = x0 + x1;
    x0 -= x1;
    x1 = kW6 * (x3 + x2) + 4;
    x2 = (x1 - (kW2 + kW6) * x2) >> 3;
    x3 = (x1 + (kW2 - kW6) * x3) >> 3;
    x1 = x4 + x6;
    x4 -= x6;
    x6 = x5 + x7;
    x5 -= x7;
    x7 = x8 + x3;
    x8 -= x3;
    x3 = x0 + x2;
    x0 -= x2;
    x2 = (181 * (x4 + x5) + 128) >> 8;
    x4 = (181 * (x4 - x5) + 128) >> 8;
    o[0] = static_cast<std::int16_t>(clip9((x7 + x1) >> 14));
    o[stride] = static_cast<std::int16_t>(clip9((x3 + x2) >> 14));
    o[2 * stride] = static_cast<std::int16_t>(clip9((x0 + x4) >> 14));
    o[3 * stride] = static_cast<std::int16_t>(clip9((x8 + x6) >> 14));
    o[4 * stride] = static_cast<std::int16_t>(clip9((x8 - x6) >> 14));
    o[5 * stride] = static_cast<std::int16_t>(clip9((x0 - x4) >> 14));
    o[6 * stride] = static_cast<std::int16_t>(clip9((x3 - x2) >> 14));
    o[7 * stride] = static_cast<std::int16_t>(clip9((x7 - x1) >> 14));
}

// Writes one block into a macroblock that is already zero, stride samples between its lines.
void decode_block(std::int16_t *out, int stride, int channel, bool intra, std::uint32_t qsc) {
    const Tables &t = tables();
    const bool table_one = intra && intra_vlc_one(), one = mpeg1();
    const DctLookup &table = table_one ? t.dct1 : t.dct0;
    const std::uint8_t *scan = alternate_scan() ? kAlternate : kZigzag;
    const std::uint8_t *weights = intra ? g_intra_iq : g_nonintra_iq;
    const int scale = one || !nonlinear_q() ? static_cast<int>(qsc) * 2 : kNonLinearScale[qsc & 31u];
    int f[64] = {};
    unsigned rows = 0, ac = 0;
    const auto put = [&](int k, int v) {
        if (!v) return;
        const int r = scan[k];
        const int sign = v > 0 ? 1 : -1;
        int x = intra ? (v * weights[r] * scale * 2) / 32 : ((2 * v + sign) * weights[r] * scale) / 32;
        if (one && x && !(x & 1)) x -= sign;
        f[r] = x < -2048 ? -2048 : x > 2047 ? 2047 : x;
        rows |= 1u << (r >> 3);
        ac |= static_cast<unsigned>(r);
    };
    int index = 0;
    if (intra) {
        g_dc[channel] = static_cast<std::int16_t>(g_dc[channel] + read_dc_diff(channel));
        const int x = g_dc[channel] * (8 >> dc_precision());
        f[0] = x < -2048 ? -2048 : x > 2047 ? 2047 : x;
        rows = 1;
        index = 1;
    } else if (peek(1)) {
        put(0, get(2) & 1u ? -1 : 1);
        index = 1;
    }
    for (;;) {
        int run = 0, level = 0;
        if (have(32)) {   // nearly always: one read of the stream for the end test, the code and its sign
            const std::uint32_t w = bits_at(g_bit, 32);
            if (index && (table_one ? w >> 28 == 6u : w >> 30 == 2u)) {
                g_bit += table_one ? 4 : 2;
                break;
            }
            const DctEntry &e = table.at(w >> 16);
            if (!e.length) throw BadCode{};
            if (e.run == kEscape) {
                read_coefficient(table, run, level);
            } else {
                run = e.run;
                level = (w >> (31 - e.length)) & 1u ? -e.level : e.level;
                g_bit += e.length + 1u;
            }
        } else {
            if (index && (table_one ? peek(4) == 6u : peek(2) == 2u)) {
                skip(table_one ? 4 : 2);
                break;
            }
            read_coefficient(table, run, level);
        }
        index += run;
        if (index >= 64) throw BadCode{};
        put(index++, level);
    }
    if (!ac) {
        const std::int16_t v = static_cast<std::int16_t>(clip9((f[0] + 4) >> 3));
        for (int y = 0; y < 8; ++y, out += stride)
            for (int x = 0; x < 8; ++x) out[x] = v;
        return;
    }
    for (int y = 0; y < 8; ++y)
        if (rows & (1u << y)) idct_row(f + y * 8);
    for (int x = 0; x < 8; ++x) idct_column(f + x, out + x, stride);
}

void decode_macroblock(std::int16_t *raw, bool intra, bool field, bool dc_reset, std::uint32_t qsc) {
    const unsigned cbp = intra ? 0x3Fu : decode(tables().cbp) & 0xFFu;
    if (dc_reset) g_dc[0] = g_dc[1] = g_dc[2] = static_cast<std::int16_t>(128 << dc_precision());
    std::memset(raw, 0, 768);
    const int stride = field ? 32 : 16;
    std::int16_t *lower = raw + (field ? 16 : 128);
    if (cbp & 32u) decode_block(raw, stride, 0, intra, qsc);
    if (cbp & 16u) decode_block(raw + 8, stride, 0, intra, qsc);
    if (cbp & 8u) decode_block(lower, stride, 0, intra, qsc);
    if (cbp & 4u) decode_block(lower + 8, stride, 0, intra, qsc);
    if (cbp & 2u) decode_block(raw + 256, 8, 1, intra, qsc);
    if (cbp & 1u) decode_block(raw + 320, 8, 2, intra, qsc);
}

void convert_fast(const std::uint8_t *raw) {   // 32-bit pixels, no alpha thresholds
    std::uint32_t pixels[256];
    const std::uint8_t *cb = raw + 256, *cr = raw + 320;
    const auto pixel = [](std::uint32_t y, int r, int g, int b) {
        const int lum = y > 16 ? 149 * static_cast<int>(y - 16) : 0;
        return sat8(lum + r) | (sat8(lum + g) << 8) | (sat8(lum + b) << 16) | 0x80000000u;
    };
    for (int row = 0; row < 8; ++row) {
        std::uint32_t *top = pixels + row * 32, *bottom = top + 16;
        const std::uint8_t *y0 = raw + row * 32, *y1 = y0 + 16;
        for (int x = 0; x < 8; ++x, top += 2, bottom += 2, y0 += 2, y1 += 2) {
            const int u = *cb++ - 128, v = *cr++ - 128;
            const int r = 204 * v + 64, g = -50 * u - 104 * v + 64, b = 258 * u + 64;
            top[0] = pixel(y0[0], r, g, b);
            top[1] = pixel(y0[1], r, g, b);
            bottom[0] = pixel(y1[0], r, g, b);
            bottom[1] = pixel(y1[1], r, g, b);
        }
    }
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(pixels);
    g_out.insert(g_out.end(), bytes, bytes + sizeof pixels);
}
#else
void decode_block(std::int16_t *out, int channel, bool intra, std::uint32_t qsc) {
    int coef[64] = {};
'''),
        ('''    std::memcpy(raw + 256, blocks[4], 128);
    std::memcpy(raw + 320, blocks[5], 128);
}
''', '''    std::memcpy(raw + 256, blocks[4], 128);
    std::memcpy(raw + 320, blocks[5], 128);
}
#endif

void drain();
'''),
        ('''    case kBdec: {
        const bool ok = atomic([&] {
''', '''    case kBdec: {
        PS2_PROF(ps2::prof::Ipu);
        const bool ok = atomic([&] {
'''),
        ('''    case kCsc:
        while (g_count) {
            std::uint8_t raw[384];
''', '''    case kCsc: {
        PS2_PROF(ps2::prof::Ipu);
        while (g_count) {
#ifdef __3DS__
            // The picture goes out as it is made: one held back whole needed 1.2 MB of a heap that has little to spare.
            if (!(g_bit & 7u) && !((cmd >> 27) & 1u) && !g_th0 && !g_th1 && have(384 * 8)) {
                convert_fast(g_in.data() + (g_bit >> 3));
                g_bit += 384 * 8;
                commit();
                --g_count;
                drain();
                continue;
            }
#endif
            std::uint8_t raw[384];
'''),
        ('''            commit();
            --g_count;
        }
        return true;
    case kSetth:
''', '''            commit();
            --g_count;
        }
        return true;
    }
    case kSetth:
'''),
        ('''void drain() {
    while (g_out_pos + 16 <= g_out.size() && dmac::ipu_push(g_out.data() + g_out_pos)) {
        g_out_pos += 16;
        ++g_out_qwords;
    }
''', '''void drain() {
#ifdef __3DS__
    while (g_out_pos + 16 <= g_out.size()) {
        const std::size_t n = dmac::ipu_push_many(g_out.data() + g_out_pos, (g_out.size() - g_out_pos) / 16);
        if (!n) break;
        g_out_pos += n * 16;
        g_out_qwords += n;
    }
#else
    while (g_out_pos + 16 <= g_out.size() && dmac::ipu_push(g_out.data() + g_out_pos)) {
        g_out_pos += 16;
        ++g_out_qwords;
    }
#endif
'''),
    ],
    # Video: see the note in the group.
    'runtime/include/ps2/mmi.hpp': [
        ('''PS2_LANES(paddh, h, 8, static_cast<std::uint16_t>(s.h[i] + t.h[i]))
''',
         '''#ifndef __3DS__
PS2_LANES(paddh, h, 8, static_cast<std::uint16_t>(s.h[i] + t.h[i]))
#endif
'''),
        ('''PS2_LANES(psubh, h, 8, static_cast<std::uint16_t>(s.h[i] - t.h[i]))
''',
         '''#ifndef __3DS__
PS2_LANES(psubh, h, 8, static_cast<std::uint16_t>(s.h[i] - t.h[i]))
#endif
'''),
        ('''PS2_LANES(paddb, b, 16, static_cast<std::uint8_t>(s.b[i] + t.b[i]))
''',
         '''#ifndef __3DS__
PS2_LANES(paddb, b, 16, static_cast<std::uint8_t>(s.b[i] + t.b[i]))
#endif
'''),
        ('''PS2_LANES(psubb, b, 16, static_cast<std::uint8_t>(s.b[i] - t.b[i]))
''',
         '''#ifndef __3DS__
PS2_LANES(psubb, b, 16, static_cast<std::uint8_t>(s.b[i] - t.b[i]))
#endif
'''),
        ('''PS2_LANES(paddsh, sh, 8, sat_add(s.sh[i], t.sh[i]))
''',
         '''#ifndef __3DS__
PS2_LANES(paddsh, sh, 8, sat_add(s.sh[i], t.sh[i]))
#endif
'''),
        ('''PS2_LANES(psubsh, sh, 8, sat_sub(s.sh[i], t.sh[i]))
''',
         '''#ifndef __3DS__
PS2_LANES(psubsh, sh, 8, sat_sub(s.sh[i], t.sh[i]))
#endif
'''),
        ('''PS2_LANES(paddsb, sb, 16, sat_add(s.sb[i], t.sb[i]))
''',
         '''#ifndef __3DS__
PS2_LANES(paddsb, sb, 16, sat_add(s.sb[i], t.sb[i]))
#endif
'''),
        ('''PS2_LANES(psubsb, sb, 16, sat_sub(s.sb[i], t.sb[i]))
''',
         '''#ifndef __3DS__
PS2_LANES(psubsb, sb, 16, sat_sub(s.sb[i], t.sb[i]))
#endif
'''),
        ('''PS2_LANES(padduh, h, 8, sat_add(s.h[i], t.h[i]))
''',
         '''#ifndef __3DS__
PS2_LANES(padduh, h, 8, sat_add(s.h[i], t.h[i]))
#endif
'''),
        ('''PS2_LANES(psubuh, h, 8, sat_sub(s.h[i], t.h[i]))
''',
         '''#ifndef __3DS__
PS2_LANES(psubuh, h, 8, sat_sub(s.h[i], t.h[i]))
#endif
'''),
        ('''PS2_LANES(paddub, b, 16, sat_add(s.b[i], t.b[i]))
''',
         '''#ifndef __3DS__
PS2_LANES(paddub, b, 16, sat_add(s.b[i], t.b[i]))
#endif
'''),
        ('''PS2_LANES(psubub, b, 16, sat_sub(s.b[i], t.b[i]))
''',
         '''#ifndef __3DS__
PS2_LANES(psubub, b, 16, sat_sub(s.b[i], t.b[i]))
#endif
'''),
        ('''PS2_LANES(pcgth, h, 8, static_cast<std::uint16_t>(s.sh[i] > t.sh[i] ? 0xFFFFu : 0u))
''',
         '''#ifndef __3DS__
PS2_LANES(pcgth, h, 8, static_cast<std::uint16_t>(s.sh[i] > t.sh[i] ? 0xFFFFu : 0u))
#endif
'''),
        ('''PS2_LANES(pcgtb, b, 16, static_cast<std::uint8_t>(s.sb[i] > t.sb[i] ? 0xFFu : 0u))
''',
         '''#ifndef __3DS__
PS2_LANES(pcgtb, b, 16, static_cast<std::uint8_t>(s.sb[i] > t.sb[i] ? 0xFFu : 0u))
#endif
'''),
        ('''PS2_LANES(pminh, sh, 8, s.sh[i] < t.sh[i] ? s.sh[i] : t.sh[i])
''',
         '''#ifndef __3DS__
PS2_LANES(pminh, sh, 8, s.sh[i] < t.sh[i] ? s.sh[i] : t.sh[i])
#endif
'''),
        ('''PS2_LANES(pmaxh, sh, 8, s.sh[i] > t.sh[i] ? s.sh[i] : t.sh[i])
''',
         '''#ifndef __3DS__
PS2_LANES(pmaxh, sh, 8, s.sh[i] > t.sh[i] ? s.sh[i] : t.sh[i])
#endif
'''),
        ('''PS2_LANES(pextlb, b, 16, (i & 1) ? s.b[i >> 1] : t.b[i >> 1])
''',
         '''#ifndef __3DS__
PS2_LANES(pextlb, b, 16, (i & 1) ? s.b[i >> 1] : t.b[i >> 1])
#endif
'''),
        ('''PS2_LANES(pextub, b, 16, (i & 1) ? s.b[8 + (i >> 1)] : t.b[8 + (i >> 1)])
''',
         '''#ifndef __3DS__
PS2_LANES(pextub, b, 16, (i & 1) ? s.b[8 + (i >> 1)] : t.b[8 + (i >> 1)])
#endif
'''),
        ('''PS2_LANES(ppacb, b, 16, i < 8 ? t.b[i * 2] : s.b[(i - 8) * 2])
''',
         '''#ifndef __3DS__
PS2_LANES(ppacb, b, 16, i < 8 ? t.b[i * 2] : s.b[(i - 8) * 2])
#endif
'''),
        ('''PS2_LANES(pcpyld, d, 2, i ? s.d[0] : t.d[0])
''',
         '''#ifndef __3DS__
PS2_LANES(pcpyld, d, 2, i ? s.d[0] : t.d[0])
#endif
'''),
        ('''PS2_LANES(pcpyud, d, 2, i ? t.d[1] : s.d[1])
''',
         '''#ifndef __3DS__
PS2_LANES(pcpyud, d, 2, i ? t.d[1] : s.d[1])
#endif
'''),
        ('''inline void psllh(Reg128 &d, const Reg128 &t, unsigned sa) { for (int i = 0; i < 8; ++i) d.h[i] = static_cast<std::uint16_t>(t.h[i] << (sa & 15)); }
''',
         '''#ifndef __3DS__
inline void psllh(Reg128 &d, const Reg128 &t, unsigned sa) { for (int i = 0; i < 8; ++i) d.h[i] = static_cast<std::uint16_t>(t.h[i] << (sa & 15)); }
#endif
'''),
        ('''inline void psrlh(Reg128 &d, const Reg128 &t, unsigned sa) { for (int i = 0; i < 8; ++i) d.h[i] = static_cast<std::uint16_t>(t.h[i] >> (sa & 15)); }
''',
         '''#ifndef __3DS__
inline void psrlh(Reg128 &d, const Reg128 &t, unsigned sa) { for (int i = 0; i < 8; ++i) d.h[i] = static_cast<std::uint16_t>(t.h[i] >> (sa & 15)); }
#endif
'''),
        ('''inline void psrah(Reg128 &d, const Reg128 &t, unsigned sa) { for (int i = 0; i < 8; ++i) d.sh[i] = static_cast<std::int16_t>(t.sh[i] >> (sa & 15)); }
''',
         '''#ifndef __3DS__
inline void psrah(Reg128 &d, const Reg128 &t, unsigned sa) { for (int i = 0; i < 8; ++i) d.sh[i] = static_cast<std::int16_t>(t.sh[i] >> (sa & 15)); }
#endif
'''),
        ('''inline void qfsrv(Reg128 &d, const Reg128 &s, const Reg128 &t, std::uint32_t sa) {
    std::uint8_t bytes[32];
    __builtin_memcpy(bytes, &t, 16);
    __builtin_memcpy(bytes + 16, &s, 16);
    Reg128 r;
    __builtin_memcpy(&r, bytes + (sa & 15), 16);
    d = r;
}
''',
         '''#ifdef __3DS__
// ps2run_3ds_patch.py: the ARM11 has parallel instructions for most of these. One word at a time, a lane loop becomes a third
// of the work or less, and video spends about 430,000 of these per picture.
#define PS2_ARM_LANES(name, insn) \\
    inline void name(Reg128 &d, const Reg128 &s, const Reg128 &t) { \\
        std::uint32_t r0, r1, r2, r3; \\
        __asm__(insn " %0, %1, %2" : "=r"(r0) : "r"(s.w[0]), "r"(t.w[0]) : "cc"); \\
        __asm__(insn " %0, %1, %2" : "=r"(r1) : "r"(s.w[1]), "r"(t.w[1]) : "cc"); \\
        __asm__(insn " %0, %1, %2" : "=r"(r2) : "r"(s.w[2]), "r"(t.w[2]) : "cc"); \\
        __asm__(insn " %0, %1, %2" : "=r"(r3) : "r"(s.w[3]), "r"(t.w[3]) : "cc"); \\
        d.w[0] = r0; d.w[1] = r1; d.w[2] = r2; d.w[3] = r3; \\
    }
PS2_ARM_LANES(paddh, "uadd16")
PS2_ARM_LANES(psubh, "usub16")
PS2_ARM_LANES(paddb, "uadd8")
PS2_ARM_LANES(psubb, "usub8")
PS2_ARM_LANES(paddsh, "qadd16")
PS2_ARM_LANES(psubsh, "qsub16")
PS2_ARM_LANES(paddsb, "qadd8")
PS2_ARM_LANES(psubsb, "qsub8")
PS2_ARM_LANES(padduh, "uqadd16")
PS2_ARM_LANES(psubuh, "uqsub16")
PS2_ARM_LANES(paddub, "uqadd8")
PS2_ARM_LANES(psubub, "uqsub8")
#undef PS2_ARM_LANES

inline std::uint32_t arm_minh(std::uint32_t a, std::uint32_t b) {
    std::uint32_t r;
    __asm__("ssub16 %0, %1, %2\\n\\tsel %0, %2, %1" : "=&r"(r) : "r"(a), "r"(b) : "cc");
    return r;
}

inline std::uint32_t arm_maxh(std::uint32_t a, std::uint32_t b) {
    std::uint32_t r;
    __asm__("ssub16 %0, %1, %2\\n\\tsel %0, %1, %2" : "=&r"(r) : "r"(a), "r"(b) : "cc");
    return r;
}

inline std::uint32_t arm_gth(std::uint32_t a, std::uint32_t b) {   // a > b in each signed halfword
    std::uint32_t r;
    __asm__("ssub16 %0, %2, %1\\n\\tsel %0, %3, %4" : "=&r"(r) : "r"(a), "r"(b), "r"(0u), "r"(0xFFFFFFFFu) : "cc");
    return r;
}

inline std::uint32_t arm_gtb(std::uint32_t a, std::uint32_t b) {
    std::uint32_t r;
    __asm__("ssub8 %0, %2, %1\\n\\tsel %0, %3, %4" : "=&r"(r) : "r"(a), "r"(b), "r"(0u), "r"(0xFFFFFFFFu) : "cc");
    return r;
}

#define PS2_ARM_WORDS(name, fn) \\
    inline void name(Reg128 &d, const Reg128 &s, const Reg128 &t) { \\
        const std::uint32_t r0 = fn(s.w[0], t.w[0]), r1 = fn(s.w[1], t.w[1]), r2 = fn(s.w[2], t.w[2]), r3 = fn(s.w[3], t.w[3]); \\
        d.w[0] = r0; d.w[1] = r1; d.w[2] = r2; d.w[3] = r3; \\
    }
PS2_ARM_WORDS(pminh, arm_minh)
PS2_ARM_WORDS(pmaxh, arm_maxh)
PS2_ARM_WORDS(pcgth, arm_gth)
PS2_ARM_WORDS(pcgtb, arm_gtb)
#undef PS2_ARM_WORDS

inline std::uint32_t arm_zip(std::uint32_t low, std::uint32_t high) {   // two 16-bit values: low0 high0 low1 high1, from the lowest byte
    low = (low | (low << 8)) & 0x00FF00FFu;
    high = (high | (high << 8)) & 0x00FF00FFu;
    return low | (high << 8);
}

inline std::uint32_t arm_even(std::uint32_t a, std::uint32_t b) {   // bytes 0 and 2 of a, then of b
    a &= 0x00FF00FFu;
    b &= 0x00FF00FFu;
    return ((a | (a >> 8)) & 0xFFFFu) | ((b | (b >> 8)) << 16);
}

inline void pextlb(Reg128 &d, const Reg128 &s, const Reg128 &t) {
    const std::uint32_t a = t.w[0], b = s.w[0], e = t.w[1], f = s.w[1];
    d.w[0] = arm_zip(a & 0xFFFFu, b & 0xFFFFu);
    d.w[1] = arm_zip(a >> 16, b >> 16);
    d.w[2] = arm_zip(e & 0xFFFFu, f & 0xFFFFu);
    d.w[3] = arm_zip(e >> 16, f >> 16);
}

inline void pextub(Reg128 &d, const Reg128 &s, const Reg128 &t) {
    const std::uint32_t a = t.w[2], b = s.w[2], e = t.w[3], f = s.w[3];
    d.w[0] = arm_zip(a & 0xFFFFu, b & 0xFFFFu);
    d.w[1] = arm_zip(a >> 16, b >> 16);
    d.w[2] = arm_zip(e & 0xFFFFu, f & 0xFFFFu);
    d.w[3] = arm_zip(e >> 16, f >> 16);
}

inline void ppacb(Reg128 &d, const Reg128 &s, const Reg128 &t) {
    const std::uint32_t r0 = arm_even(t.w[0], t.w[1]), r1 = arm_even(t.w[2], t.w[3]), r2 = arm_even(s.w[0], s.w[1]), r3 = arm_even(s.w[2], s.w[3]);
    d.w[0] = r0; d.w[1] = r1; d.w[2] = r2; d.w[3] = r3;
}

inline void pcpyld(Reg128 &d, const Reg128 &s, const Reg128 &t) {
    d.d[1] = s.d[0];
    d.d[0] = t.d[0];
}

inline void pcpyud(Reg128 &d, const Reg128 &s, const Reg128 &t) {
    d.d[0] = s.d[1];
    d.d[1] = t.d[1];
}

inline void psllh(Reg128 &d, const Reg128 &t, unsigned sa) {
    sa &= 15u;
    const std::uint32_t m = ((0xFFFFu << sa) & 0xFFFFu) * 0x10001u;
    d.w[0] = (t.w[0] << sa) & m; d.w[1] = (t.w[1] << sa) & m; d.w[2] = (t.w[2] << sa) & m; d.w[3] = (t.w[3] << sa) & m;
}

inline void psrlh(Reg128 &d, const Reg128 &t, unsigned sa) {
    sa &= 15u;
    const std::uint32_t m = (0xFFFFu >> sa) * 0x10001u;
    d.w[0] = (t.w[0] >> sa) & m; d.w[1] = (t.w[1] >> sa) & m; d.w[2] = (t.w[2] >> sa) & m; d.w[3] = (t.w[3] >> sa) & m;
}

inline std::uint32_t arm_srah(std::uint32_t x, unsigned sa) {
    return (static_cast<std::uint32_t>(static_cast<std::int32_t>(x) >> sa) & 0xFFFF0000u) | (static_cast<std::uint32_t>(static_cast<std::int32_t>(x << 16) >> sa) >> 16);
}

inline void psrah(Reg128 &d, const Reg128 &t, unsigned sa) {
    sa &= 15u;
    d.w[0] = arm_srah(t.w[0], sa); d.w[1] = arm_srah(t.w[1], sa); d.w[2] = arm_srah(t.w[2], sa); d.w[3] = arm_srah(t.w[3], sa);
}

inline void qfsrv(Reg128 &d, const Reg128 &s, const Reg128 &t, std::uint32_t sa) {
    const std::uint32_t v[8] = {t.w[0], t.w[1], t.w[2], t.w[3], s.w[0], s.w[1], s.w[2], s.w[3]};
    sa &= 15u;
    const std::uint32_t *p = v + (sa >> 2);
    const unsigned b = (sa & 3u) * 8u;
    if (!b) {
        d.w[0] = p[0]; d.w[1] = p[1]; d.w[2] = p[2]; d.w[3] = p[3];
    } else {
        d.w[0] = (p[0] >> b) | (p[1] << (32 - b));
        d.w[1] = (p[1] >> b) | (p[2] << (32 - b));
        d.w[2] = (p[2] >> b) | (p[3] << (32 - b));
        d.w[3] = (p[3] >> b) | (p[4] << (32 - b));
    }
}
#else
inline void qfsrv(Reg128 &d, const Reg128 &s, const Reg128 &t, std::uint32_t sa) {
    std::uint8_t bytes[32];
    __builtin_memcpy(bytes, &t, 16);
    __builtin_memcpy(bytes + 16, &s, 16);
    Reg128 r;
    __builtin_memcpy(&r, bytes + (sa & 15), 16);
    d = r;
}
#endif
'''),
    ],
    # eerecomp --natives: an address and a name per line. The game function at that address first tries native_<name>() from
    # port3ds/native3ds.cpp, on the 3DS only; if that returns false, as it does for anything it does not cover, the game's code runs.
    'recomp/codegen.hpp': [
        ('    std::set<std::uint32_t> probes;\n', '    std::set<std::uint32_t> probes;\n    std::map<std::uint32_t, std::string> natives;   // ps2run_3ds_patch.py\n'),
    ],
    'recomp/codegen.cpp': [
        ('''            if (group_start(a)) {
                emit_group(a);
                falls = skip = false;
                continue;
            }
''', '''            if (const auto native = options_.natives.find(a); native != options_.natives.end()) {   // ps2run_3ds_patch.py
                out_ << "#ifdef __3DS__\\n";
                line(std::format("{{ bool native_{0}(Cpu &); if (native_{0}(c)) {{ c.pc = c.r[31].w[0]; return; }} }}", native->second));
                out_ << "#endif\\n";
            }
            if (group_start(a)) {
                emit_group(a);
                falls = skip = false;
                continue;
            }
'''),
    ],
    'recomp/main.cpp': [
        ('''    std::string elf_path = argv[1], entries_path, out, probes_path, modules_dir, variants_path, vanilla_path;
''', '''    std::string elf_path = argv[1], entries_path, out, probes_path, modules_dir, variants_path, vanilla_path, natives_path;
'''),
        ('''        else if (arg == "--probes" && i + 1 < argc) probes_path = argv[++i];
''', '''        else if (arg == "--probes" && i + 1 < argc) probes_path = argv[++i];
        else if (arg == "--natives" && i + 1 < argc) natives_path = argv[++i];
'''),
        ('''            generate(translations, options);
''', '''            if (!natives_path.empty()) {   // ps2run_3ds_patch.py
                std::ifstream file(natives_path);
                if (!file) throw std::runtime_error("cannot read " + natives_path);
                std::string address, name;
                while (file >> address >> name) options.natives[static_cast<std::uint32_t>(std::stoul(address, nullptr, 16))] = name;
            }
            generate(translations, options);
'''),
    ],
    # Video reads and writes the IPU's registers about 30,000 times per picture.
    'runtime/src/memory.cpp#ipu': [
        ('#include "hw.hpp"\n#include "vu_micro.hpp"\n', '#include "hw.hpp"\n#include "ipu/ipu.hpp"\n#include "vu_micro.hpp"\n'),
        ('''std::uint32_t slow_ld32(std::uint32_t a) { return read_slow<std::uint32_t>(a); }
std::uint64_t slow_ld64(std::uint32_t a) { return read_slow<std::uint64_t>(a); }
''', '''#ifdef __3DS__
inline bool ipu_register(std::uint32_t a) { return (a >> 6) == (0x10002000u >> 6); }
std::uint32_t slow_ld32(std::uint32_t a) { return ipu_register(a) ? ipu::read32(a) : read_slow<std::uint32_t>(a); }
std::uint64_t slow_ld64(std::uint32_t a) {
    if (ipu_register(a)) return ipu::read32(a) | (static_cast<std::uint64_t>(ipu::read32(a + 4)) << 32);
    return read_slow<std::uint64_t>(a);
}
#else
std::uint32_t slow_ld32(std::uint32_t a) { return read_slow<std::uint32_t>(a); }
std::uint64_t slow_ld64(std::uint32_t a) { return read_slow<std::uint64_t>(a); }
#endif
'''),
        ('''void slow_st32(std::uint32_t a, std::uint32_t v) { write_slow(a, v); }
''', '''#ifdef __3DS__
void slow_st32(std::uint32_t a, std::uint32_t v) {
    if (ipu_register(a)) ipu::write32(a, v);
    else write_slow(a, v);
}
#else
void slow_st32(std::uint32_t a, std::uint32_t v) { write_slow(a, v); }
#endif
'''),
    ],
    # The I/O processor: most of its instructions touch neither the hardware nor the scheduler, so main RAM is reached inline and
    # the check for a thread switch runs only after an instruction that could have caused one.
    'runtime/src/iop/cpu.cpp#speed': [
        ('''void execute(Cpu &c, std::int64_t &budget) {
''', '''#ifdef __3DS__
template <typename T>
inline T fast_read(std::uint32_t a, bool &slow) {
    const std::uint32_t p = phys(a);
    if (p < 0x00800000u) {
        T v;
        std::memcpy(&v, g_ram + (p & (kRamSize - 1)), sizeof v);
        return v;
    }
    slow = true;
    if constexpr (sizeof(T) == 1) return read8(a);
    else if constexpr (sizeof(T) == 2) return read16(a);
    else return read32(a);
}

template <typename T>
inline void fast_write(std::uint32_t a, T v, bool &slow) {
    const std::uint32_t p = phys(a);
    if (p < 0x00800000u) {
        std::memcpy(g_ram + (p & (kRamSize - 1)), &v, sizeof v);
        return;
    }
    slow = true;
    if constexpr (sizeof(T) == 1) write8(a, v);
    else if constexpr (sizeof(T) == 2) write16(a, v);
    else write32(a, v);
}
#define read8(a) fast_read<std::uint8_t>(a, slow)
#define read16(a) fast_read<std::uint16_t>(a, slow)
#define read32(a) fast_read<std::uint32_t>(a, slow)
#define write8(a, v) fast_write<std::uint8_t>(a, v, slow)
#define write16(a, v) fast_write<std::uint16_t>(a, v, slow)
#define write32(a, v) fast_write<std::uint32_t>(a, v, slow)
#endif

void execute(Cpu &c, std::int64_t &budget) {
#ifdef __3DS__
    bool slow = false;
#endif
'''),
        ('''        const std::uint32_t pc = c.pc;
        if (probing) {
            const std::uint32_t a = pc & (kRamSize - 1);
            if (g_probes[a >> 5] & (1u << ((a >> 2) & 7u))) probe(c, pc);
        }
''', '''        const std::uint32_t pc = c.pc;
#ifndef __3DS__
        if (probing) {
            const std::uint32_t a = pc & (kRamSize - 1);
            if (g_probes[a >> 5] & (1u << ((a >> 2) & 7u))) probe(c, pc);
        }
#endif
'''),
        ('''                kernel::syscall(c, pc);
                break;
''', '''                kernel::syscall(c, pc);
#ifdef __3DS__
                slow = true;
#endif
                break;
'''),
        ('''        case 0x10:
            if (rs == 0) r[rt] = rd == 12 ? c.status''', '''        case 0x10:
#ifdef __3DS__
            slow = true;
#endif
            if (rs == 0) r[rt] = rd == 12 ? c.status'''),
        ('''        r[0] = 0;
        if (kernel::switch_pending()) return;
    }
}

}
''', '''        r[0] = 0;
#ifdef __3DS__
        if (slow) {
            slow = false;
            if (kernel::switch_pending()) return;
        }
#else
        if (kernel::switch_pending()) return;
#endif
    }
}

#ifdef __3DS__
#undef read8
#undef read16
#undef read32
#undef write8
#undef write16
#undef write32
#endif

}
'''),
    ],
    'runtime/src/dmac.hpp': [
        ('#include <cstdint>\n', '#include <cstddef>\n#include <cstdint>\n'),
        ('bool ipu_push(const std::uint8_t *qword);\n', 'bool ipu_push(const std::uint8_t *qword);\nstd::size_t ipu_push_many(const std::uint8_t *data, std::size_t qwords);\n'),
    ],
    'runtime/src/dmac.cpp#ipu': [
        ('void start(int ch) {\n    Channel &c = g_ch[ch];\n    c.paused = false;\n', 'void start(int ch) {\n    PS2_PROF(ps2::prof::Dma);\n    Channel &c = g_ch[ch];\n    c.paused = false;\n'),
        ('''void reset() {
    for (Channel &c : g_ch) c = Channel{};
''', '''std::size_t ipu_push_many(const std::uint8_t *data, std::size_t qwords) {   // ps2run_3ds_patch.py: returns how many were taken
    Channel &c = g_ch[3];
    if (!(c.chcr & 0x100u) || !c.qwc) return 0;
    const std::uint32_t room = (c.madr & 0x80000000u) ? kScratchpadSize - (c.madr & (kScratchpadSize - 1)) : kRamSize - (c.madr & (kRamSize - 16));
    const std::size_t n = std::min<std::size_t>(qwords, std::min<std::size_t>(c.qwc, room / 16));
    std::memcpy(target(c.madr), data, n * 16);
    c.madr += static_cast<std::uint32_t>(n * 16);
    c.qwc -= static_cast<std::uint32_t>(n);
    if (!c.qwc) finish(3);
    return n;
}

void reset() {
    for (Channel &c : g_ch) c = Channel{};
'''),
    ],
    # vurecomp --scalar: for a CPU without SIMD. Each lane is a plain float operation, with no second copy of the registers, and
    # with PS2_VU_FAST the fix-ups for infinities and denormals go away. Flags are still worked out where the program reads them.
    'recomp/vurecomp.cpp': [
        ('constexpr int kAcc = 32;\n', 'constexpr int kAcc = 32;\nbool g_scalar = false;   // ps2run_3ds_patch.py\n'),
        ('    const bool vector = kind == Add || kind == Sub || kind == Mul || kind == Madd || kind == Msub || kind == Max || kind == Min || kind == Abs || kind == Ftoi || kind == Itof;\n', '    const bool vector = !g_scalar && (kind == Add || kind == Sub || kind == Mul || kind == Madd || kind == Msub || kind == Max || kind == Min || kind == Abs || kind == Ftoi || kind == Itof);\n'),
        ('    const bool arithmetic = kind == Add || kind == Sub || kind == Mul || kind == Madd || kind == Msub;\n', '    const bool arithmetic = !g_scalar && (kind == Add || kind == Sub || kind == Mul || kind == Madd || kind == Msub);\n'),
        ('''case Itof: e = std::format("fp::from_fixed(r.vf[{}].sw[{}], {})", fs, i, frac); break;''',
         '''case Itof: e = g_scalar ? std::format("fixed_to_float(r.vf[{}].sw[{}], {})", fs, i, frac) : std::format("fp::from_fixed(r.vf[{}].sw[{}], {})", fs, i, frac); break;'''),
        ('    const fs::path in_dir = argv[1], out_dir = argv[2];\n', '    const fs::path in_dir = argv[1], out_dir = argv[2];\n    g_scalar = argc > 3 && std::strcmp(argv[3], "--scalar") == 0;\n'),
    ],
    'runtime/include/ps2/vu_ops.hpp#fast': [
        ('PS2_VU_INLINE float fin(std::uint32_t x) {\n', '''PS2_VU_INLINE float fin(std::uint32_t x) {
#ifdef PS2_VU_FAST
    {
        float plain;
        std::memcpy(&plain, &x, 4);
        return plain;
    }
#endif
'''),
        ('PS2_VU_INLINE std::uint32_t fout(float f) {\n', '''PS2_VU_INLINE std::uint32_t fout(float f) {
#ifdef PS2_VU_FAST
    {
        std::uint32_t plain;
        std::memcpy(&plain, &f, 4);
        return plain;
    }
#endif
'''),
        ('PS2_VU_INLINE float norm(float f) { return fin(fout(f)); }\n', '''PS2_VU_INLINE float norm(float f) { return fin(fout(f)); }
PS2_VU_INLINE std::uint32_t fixed_to_float(std::int32_t v, int fraction) {
    return fout(static_cast<float>(v) / static_cast<float>(1 << fraction));
}
'''),
    ],
    # GCC for bare-metal ARM shrinks a plain enum to one byte, so saved states from a desktop build would not line up.
    'runtime/src/iop/kernel.hpp': [
        ('enum WaitKind { kWaitNone,', 'enum WaitKind : int { kWaitNone,'),
    ],
    'runtime/src/iop/spu2.cpp#enum': [
        ('enum Phase { kOff,', 'enum Phase : int { kOff,'),
    ],
    'runtime/src/system.cpp#state': [
        # A saved state is bigger than the 3DS heap, so it is read straight from the file instead of through a buffer.
        ('''bool load_state(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
''', '''#ifdef __3DS__
namespace {
class FileReader : public state::Stream {   // ps2run_3ds_patch.py
public:
    explicit FileReader(std::FILE *file) : Stream(false), file_(file) {}
    void bytes(void *data, std::size_t size) override {
        if (std::fread(data, 1, size, file_) != size) throw std::runtime_error("state file is truncated");
    }

private:
    std::FILE *file_;
};
}
#endif

bool load_state(const std::string &path) {
#ifdef __3DS__
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) {
        std::fprintf(stderr, "[state] cannot open %s\\n", path.c_str());
        return false;
    }
    bool loaded = false;
    try {
        FileReader r(file);
        machine_state(r);
        g_resumed = true;
        loaded = true;
        std::fprintf(stderr, "[state] loaded frame %llu\\n", static_cast<unsigned long long>(g_frames));
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[state] cannot load: %s\\n", e.what());
    }
    std::fclose(file);
    return loaded;
#endif
    std::ifstream in(path, std::ios::binary);
'''),
    ],
    'runtime/src/gs/gs.cpp': [
        # The New 3DS GPU path (port3ds/pica.cpp). GS memory is still kept up to date for uploads; only drawing moves.
        ('#include "raster.hpp"\n', '''#include "raster.hpp"
#ifdef PS2_PICA
#include "pica.hpp"
#endif
'''),
        ('        if (cpu_raster()) raster::draw(s, s.queue, needed);\n', '''#ifdef PS2_PICA
        if (pica::active()) pica::draw(s, s.queue, needed);
        else
#endif
        if (cpu_raster()) raster::draw(s, s.queue, needed);
'''),
        ('        note_upload(s, value);\n        s.trxdir = value;\n', '''        note_upload(s, value);
#ifdef PS2_PICA
        if (pica::active()) pica::note_transfer(s, value);
#endif
        s.trxdir = value;
'''),
        ('''    const std::uint32_t psm = static_cast<std::uint32_t>(tex0 >> 20) & 0x3Fu;
    const std::uint32_t cpsm = static_cast<std::uint32_t>(tex0 >> 51) & 0xFu;
    const std::uint32_t csm = ''', '''#ifdef PS2_PICA
    if (pica::active()) pica::note_clut();
#endif
    const std::uint32_t psm = static_cast<std::uint32_t>(tex0 >> 20) & 0x3Fu;
    const std::uint32_t cpsm = static_cast<std::uint32_t>(tex0 >> 51) & 0xFu;
    const std::uint32_t csm = '''),
        ('        g_odd_field = !g_odd_field;\n        raise(8);\n', '''        g_odd_field = !g_odd_field;
        raise(8);
#ifdef PS2_PICA
        if (pica::active() && !queue::active()) pica::vblank(static_cast<std::uint32_t>(ps2::frame_count()), display_regs(), pica::next_picture());
#endif
'''),
    ],
    # After the list above: these edits build on the lines it adds.
    'runtime/src/gs/gs.cpp#prof': [
        ('void feed_image(const std::uint8_t *data, std::size_t bytes) {\n', '''void feed_image(const std::uint8_t *data, std::size_t bytes) {
    PS2_PROF(ps2::prof::Upload);
#ifdef PS2_PICA
    if (pica::active()) pica::note_data(data, bytes);
#endif
'''),
    ],
    'runtime/src/dmac.cpp': [
        ('(std::min(asp + 1, 2u) << 4)', '(std::min<std::uint32_t>(asp + 1, 2u) << 4)'),
    ],
    'runtime/include/ps2/memory.hpp': [
        ('extern std::uint8_t g_ram[];\n', '''#ifdef __3DS__
// ps2run_3ds_patch.py: fixed address, mapped by port3ds/main3ds.cpp
inline constexpr std::uint32_t kRamAddress3ds = 0x09C00000u;   // right after the 28 MB heap; fits an ARM immediate, so an address needs one add and no load
static std::uint8_t *const g_ram = reinterpret_cast<std::uint8_t *>(kRamAddress3ds);
#else
extern std::uint8_t g_ram[];
#endif
'''),
    ],
    'runtime/src/memory.cpp': [
        ('alignas(64) std::uint8_t g_ram[kRamSize + 64];\n', '''#ifndef __3DS__
alignas(64) std::uint8_t g_ram[kRamSize + 64];
#endif
'''),
    ],
    'runtime/src/gs/gs.cpp#speed': [
        ('''    if ((g_state.trxdir & 3u) == 0) {
        g_up_bytes += bytes;
        for (std::size_t i = 0; i < bytes; ++i) g_up_nz += data[i] != 0;
    }
''', '''#ifndef PS2_PICA   // a count for tracing that cost more than the upload itself
    if ((g_state.trxdir & 3u) == 0) {
        g_up_bytes += bytes;
        for (std::size_t i = 0; i < bytes; ++i) g_up_nz += data[i] != 0;
    }
#endif
'''),
        ('''std::uint8_t g_carry[4];
std::size_t g_carried = 0;
''', '''std::uint8_t g_carry[4];
std::size_t g_carried = 0;

#ifdef PS2_PICA
// Uploads a page at a time: the page address and the row of the swizzle table stay the same across a page width, so they are
// worked out once per run of pixels and not once per pixel. write_u32 and friends are ps2run_3ds_patch.py.
template <std::uint32_t PageWidth, std::uint32_t PageHeight, typename Store>
void image_runs(std::size_t pixels, Store store) {
    const std::uint64_t buf = g_state.bitbltbuf, pos = g_state.trxpos, reg = g_state.trxreg;
    const std::uint32_t dbp = static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu, dbw = static_cast<std::uint32_t>(buf >> 48) & 0x3Fu;
    const std::uint32_t dsax = static_cast<std::uint32_t>(pos >> 32) & 0x7FFu, dsay = static_cast<std::uint32_t>(pos >> 48) & 0x7FFu;
    const std::uint32_t rrw = static_cast<std::uint32_t>(reg) & 0xFFFu, rrh = static_cast<std::uint32_t>(reg >> 32) & 0xFFFu;
    if (!rrw) return;
    std::size_t done = 0;
    while (done < pixels && g_transfer.active) {
        const std::uint32_t x = (dsax + g_transfer.x) & 0x7FFu, y = (dsay + g_transfer.y) & 0x7FFu;
        const std::uint32_t in_page = x & (PageWidth - 1u);
        const std::uint32_t run = static_cast<std::uint32_t>(std::min<std::size_t>({pixels - done, rrw - g_transfer.x, PageWidth - in_page}));
        store(page_address(dbp, dbw, x, y, PageWidth, PageHeight), y & (PageHeight - 1u), in_page, run, done);
        done += run;
        g_transfer.x += run;
        if (g_transfer.x >= rrw) {
            g_transfer.x = 0;
            if (++g_transfer.y >= rrh) g_transfer.active = false;
        }
    }
}

void image_fast(std::uint32_t psm, const std::uint8_t *data, std::size_t bytes) {
    std::uint8_t *const mem = g_memory;
    constexpr std::uint32_t mask = kMemorySize - 1u;
    if (psm == PSMT8) {
        image_runs<128, 64>(bytes, [&](std::uint32_t page, std::uint32_t row, std::uint32_t at, std::uint32_t run, std::size_t done) {
            const std::uint32_t *table = g_off8[row] + at;
            const std::uint8_t *src = data + done;
            for (std::uint32_t i = 0; i < run; ++i) mem[(page + table[i]) & mask] = src[i];
        });
    } else if (psm == PSMT4) {
        image_runs<128, 128>(bytes * 2, [&](std::uint32_t page, std::uint32_t row, std::uint32_t at, std::uint32_t run, std::size_t done) {
            const std::uint32_t *table = g_off4[row] + at;
            for (std::uint32_t i = 0; i < run; ++i) {
                const std::size_t pixel = done + i;
                const std::uint32_t value = (data[pixel / 2] >> ((pixel & 1u) * 4u)) & 15u;
                const std::uint32_t n = table[i];
                std::uint8_t &b = mem[(page + n / 2u) & mask];
                b = (n & 1u) ? static_cast<std::uint8_t>((b & 0x0Fu) | (value << 4)) : static_cast<std::uint8_t>((b & 0xF0u) | value);
            }
        });
    } else if (psm == PSMCT16 || psm == PSMCT16S) {
        const auto &tables = psm == PSMCT16 ? g_off16 : g_off16s;
        image_runs<64, 64>(bytes / 2, [&](std::uint32_t page, std::uint32_t row, std::uint32_t at, std::uint32_t run, std::size_t done) {
            const std::uint32_t *table = tables[row] + at;
            const std::uint8_t *src = data + done * 2;
            for (std::uint32_t i = 0; i < run; ++i) std::memcpy(mem + ((page + table[i]) & mask), src + i * 2, 2);
        });
    } else if (psm == PSMCT24) {
        for (; bytes && g_carried; ++data, --bytes) {   // a pixel split between two packets
            g_carry[g_carried++] = *data;
            if (g_carried == 3) {
                transfer_pixel(g_carry[0] | (g_carry[1] << 8) | (g_carry[2] << 16));
                g_carried = 0;
            }
        }
        image_runs<64, 32>(bytes / 3, [&](std::uint32_t page, std::uint32_t row, std::uint32_t at, std::uint32_t run, std::size_t done) {
            const std::uint32_t *table = g_off32[row] + at;
            const std::uint8_t *src = data + done * 3;
            for (std::uint32_t i = 0; i < run; ++i) std::memcpy(mem + ((page + table[i]) & mask), src + i * 3, 3);   // the top byte stays
        });
        if (g_transfer.active)
            for (std::size_t i = bytes - bytes % 3; i < bytes; ++i) g_carry[g_carried++] = data[i];
    } else {
        image_runs<64, 32>(bytes / 4, [&](std::uint32_t page, std::uint32_t row, std::uint32_t at, std::uint32_t run, std::size_t done) {
            const std::uint32_t *table = g_off32[row] + at;
            const std::uint8_t *src = data + done * 4;
            for (std::uint32_t i = 0; i < run; ++i) std::memcpy(mem + ((page + table[i]) & mask), src + i * 4, 4);
        });
    }
}
#endif
'''),
        ('''    const std::uint32_t bits = bits_per_pixel(psm);
    if (bits == 24) {
        for (std::size_t i = 0; i < bytes && g_transfer.active; ++i) {
''', '''    const std::uint32_t bits = bits_per_pixel(psm);
#ifdef PS2_PICA
    if (psm == PSMT8 || psm == PSMT4 || psm == PSMCT32 || psm == PSMCT24 || psm == PSMCT16 || psm == PSMCT16S) {
        image_fast(psm, data, bytes);
        return;
    }
#endif
    if (bits == 24) {
        for (std::size_t i = 0; i < bytes && g_transfer.active; ++i) {
'''),
        ('''void kick(bool draw) {
    State &s = g_state;
    const std::uint32_t prim = static_cast<std::uint32_t>(s.prim) & 7u;
    const int needed = prim == 0 ? 1 : (prim == 1 || prim == 2 || prim == 6) ? 2 : prim == 7 ? 1 : 3;
    if (s.count < needed) return;
    if (draw) {
''', '''#ifdef PS2_PICA
// upload_clamped looks in a hash table, which is too dear to do for every triangle, and its answer can only change after a
// setup register was written.
void draw_gpu(State &s, int needed) {
    if (queue::g_skip_picture) return;
    static bool known = false;
    static std::uint32_t seen = 0;
    static std::uint64_t clamp = 0;
    Context &uc = s.ctx[static_cast<unsigned>(s.prim >> 9) & 1u];
    if (!known || seen != g_setup_generation) {
        known = true;
        seen = g_setup_generation;
        g_upload_ext.erase((static_cast<std::uint32_t>(uc.frame) & 0x1FFu) * 32u);
        clamp = upload_clamped(s, uc);
    }
    const std::uint64_t saved_clamp = uc.clamp;
    uc.clamp = clamp;
    pica::draw(s, s.queue, needed);
    uc.clamp = saved_clamp;
    ++g_primitives;
}
#endif

void kick(bool draw) {
    State &s = g_state;
    const std::uint32_t prim = static_cast<std::uint32_t>(s.prim) & 7u;
    const int needed = prim == 0 ? 1 : (prim == 1 || prim == 2 || prim == 6) ? 2 : prim == 7 ? 1 : 3;
    if (s.count < needed) return;
#ifdef PS2_PICA
    if (draw && pica::active()) {
        draw_gpu(s, needed);
        draw = false;
    }
#endif
    if (draw) {
'''),
        ('''void write_image(const std::uint8_t *data, std::uint32_t qwords) { feed_image(data, static_cast<std::size_t>(qwords) * 16); }
''', '''void write_image(const std::uint8_t *data, std::uint32_t qwords) { feed_image(data, static_cast<std::size_t>(qwords) * 16); }

#ifdef PS2_PICA
// PACKED loops that only set vertex registers make up nearly all of a 3D scene. They skip write_packed and write, and so the
// trip through two switches and the tracing checks for each register.
bool packed_vertex_regs(std::uint64_t regs, std::uint32_t nreg, std::uint8_t *kinds) {
    for (std::uint32_t r = 0; r < nreg; ++r) {
        const std::uint32_t kind = static_cast<std::uint32_t>(regs >> (r * 4)) & 15u;
        if (kind < 1 || kind > 5) return false;
        kinds[r] = static_cast<std::uint8_t>(kind);
    }
    return true;
}

void packed_run(const std::uint8_t *kinds, std::uint32_t nreg, std::uint32_t loops, const std::uint8_t *data) {
    using word = std::uint32_t __attribute__((may_alias, aligned(4)));
    State &s = g_state;
    const word *q = reinterpret_cast<const word *>(data);
    for (; loops; --loops) {
        for (std::uint32_t r = 0; r < nreg; ++r, q += 4) {
            switch (kinds[r]) {
            case 1: {
                std::uint32_t qbits;
                std::memcpy(&qbits, &s.q, 4);
                s.rgbaq = (q[0] & 0xFFu) | ((q[1] & 0xFFu) << 8) | ((q[2] & 0xFFu) << 16) | ((q[3] & 0xFFu) << 24) | (static_cast<std::uint64_t>(qbits) << 32);
                break;
            }
            case 2:
                s.st = q[0] | (static_cast<std::uint64_t>(q[1]) << 32);
                std::memcpy(&s.q, const_cast<const word *>(q + 2), 4);
                break;
            case 3: s.uv = (q[0] & 0x3FFFu) | ((q[1] & 0x3FFFu) << 16); break;
            case 4: vertex(q[0] & 0xFFFFu, q[1] & 0xFFFFu, (q[2] >> 4) & 0xFFFFFFu, static_cast<std::uint8_t>(q[3] >> 4), !((q[3] >> 15) & 1u)); break;
            default: vertex(q[0] & 0xFFFFu, q[1] & 0xFFFFu, q[2], static_cast<std::uint8_t>(s.fog >> 56), !((q[3] >> 15) & 1u)); break;
            }
        }
    }
}
#endif
'''),
    ],
    'runtime/src/gs/gs.hpp': [
        ('''void write_image(const std::uint8_t *data, std::uint32_t qwords);
''', '''void write_image(const std::uint8_t *data, std::uint32_t qwords);
bool packed_vertex_regs(std::uint64_t regs, std::uint32_t nreg, std::uint8_t *kinds);   // ps2run_3ds_patch.py
void packed_run(const std::uint8_t *kinds, std::uint32_t nreg, std::uint32_t loops, const std::uint8_t *data);
'''),
    ],
    'runtime/src/gs/gif.cpp': [
        ('''template <typename Sink>
void walk(Path &p, const std::uint8_t *data, std::uint32_t qwc, Sink &sink) {
''', '''#ifdef PS2_PICA
struct FastLoop {   // not in Path, which is saved in state files
    bool on = false;
    std::uint8_t kinds[16];
};
FastLoop g_fast[4];
#endif

template <typename Sink>
void walk(Path &p, const std::uint8_t *data, std::uint32_t qwc, Sink &sink) {
'''),
        ('''        switch (p.flg) {
        case 0:
            sink.packed(static_cast<std::uint32_t>(p.regs >> (p.reg * 4)) & 0xFu, q);
''', '''        switch (p.flg) {
        case 0:
#ifdef PS2_PICA
            if (g_fast[sink.path].on && p.reg == 0) {
                const std::uint32_t loops = std::min((qwc - i) / p.nreg, p.nloop - p.loop);
                if (loops) {
                    gs::packed_run(g_fast[sink.path].kinds, p.nreg, loops, data + static_cast<std::size_t>(i) * 16);
                    i += loops * p.nreg - 1;
                    p.loop += loops;
                    if (p.loop >= p.nloop) p.active = false;
                    break;
                }
            }
#endif
            sink.packed(static_cast<std::uint32_t>(p.regs >> (p.reg * 4)) & 0xFu, q);
'''),
        ('''        ++g_tags[path][p.flg];
        gs::g_state.q = 1.0f;
''', '''        ++g_tags[path][p.flg];
#ifdef PS2_PICA
        g_fast[path].on = p.flg == 0 && gs::packed_vertex_regs(p.regs, p.nreg, g_fast[path].kinds);
#endif
        gs::g_state.q = 1.0f;
'''),
    ],
    'runtime/src/vu_micro.cpp#speed': [
        ('''    PS2_PROF(ps2::prof::Gif);
    std::uint8_t buffer[16 * 1024 + 16];
''', '''    PS2_PROF(ps2::prof::Gif);
#ifdef PS2_PICA
    {   // A packet that does not wrap around the end of VU memory can be read where it is.
        const std::uint32_t first = address & (u.data_qwords - 1);
        std::uint32_t at = first;
        bool end = false;
        while (!end && at < u.data_qwords) {
            std::uint64_t tag;
            std::memcpy(&tag, qword(u, at), 8);
            const std::uint32_t nloop = static_cast<std::uint32_t>(tag) & 0x7FFFu, flg = static_cast<std::uint32_t>(tag >> 58) & 3u;
            std::uint32_t nreg = static_cast<std::uint32_t>(tag >> 60) & 0xFu;
            if (!nreg) nreg = 16;
            at += 1 + (flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop);
            end = (tag >> 15) & 1u;
        }
        if (end && at <= u.data_qwords) {
            gif::transfer(1, qword(u, first), at - first);
            return;
        }
    }
#endif
    std::uint8_t buffer[16 * 1024 + 16];
'''),
    ],
    'runtime/src/system.cpp#speed': [
        ('''    next = std::min(next, now() + kEeClock / 2000);
    g_base = std::max(now(), next);
    g_given = 0;
''', '''    next = std::min(next, now() + kEeClock / 2000);
    g_idle_cycles += std::max(now(), next) - now();   // ps2run_3ds_patch.py
    g_base = std::max(now(), next);
    g_given = 0;
'''),
        ('''std::uint64_t now() { return g_base + static_cast<std::uint64_t>(g_given - g_cpu.budget); }
''', '''std::uint64_t now() { return g_base + static_cast<std::uint64_t>(g_given - g_cpu.budget); }
std::uint64_t idle_cycles() { return g_idle_cycles; }
'''),
        ('''void idle(Cpu &c) {
    std::uint64_t next = next_event_time();
''', '''std::uint64_t g_idle_cycles = 0;

void idle(Cpu &c) {
    std::uint64_t next = next_event_time();
'''),
    ],
    'runtime/src/vif.cpp': [
        ('''#include "ps2/state.hpp"
''', '''#include "ps2/state.hpp"
#include "ps2/system.hpp"
'''),
        ('''void transfer(int unit, const std::uint8_t *data, std::uint32_t qwc) {
''', '''void transfer(int unit, const std::uint8_t *data, std::uint32_t qwc) {
    PS2_PROF(ps2::prof::Vif);
'''),
    ],
    'runtime/src/kernel.cpp': [
        ('''void service(Cpu &c) {
    if (g_in_handler) return;
''', '''void service(Cpu &c) {
    PS2_PROF(ps2::prof::Kernel);
    if (g_in_handler) return;
'''),
    ],
    'runtime/src/system.cpp#events': [
        ('''void run_due_events() {
    sync_iop();
''', '''void run_due_events() {
    PS2_PROF(ps2::prof::Events);
    sync_iop();
'''),
    ],
    'runtime/src/gs/gif.cpp#queue': [
        ('''std::vector<std::uint8_t> g_pend3;
bool g_released = false;
''', '''std::vector<std::uint8_t> g_pend3;
std::size_t g_pend3_head = 0;   // bytes already sent: moving what is left down on every packet took 200 MB of copying per picture
bool g_released = false;
'''),
        ('''std::uint32_t packet_qwords(const std::vector<std::uint8_t> &q) {
    const std::uint32_t total = static_cast<std::uint32_t>(q.size() / 16);
    std::uint32_t at = 0;
    while (at < total) {
        std::uint64_t tag;
        std::memcpy(&tag, q.data() + static_cast<std::size_t>(at) * 16, 8);
''', '''std::uint32_t packet_qwords(const std::uint8_t *q, std::size_t bytes) {
    const std::uint32_t total = static_cast<std::uint32_t>(bytes / 16);
    std::uint32_t at = 0;
    while (at < total) {
        std::uint64_t tag;
        std::memcpy(&tag, q + static_cast<std::size_t>(at) * 16, 8);
'''),
        ('''    while (!g_pend3.empty()) {
        const std::uint32_t n = everything ? static_cast<std::uint32_t>(g_pend3.size() / 16) : packet_qwords(g_pend3);
        if (!n) break;
        std::vector<std::uint8_t> chunk(g_pend3.begin(), g_pend3.begin() + static_cast<std::size_t>(n) * 16);
        g_pend3.erase(g_pend3.begin(), g_pend3.begin() + static_cast<std::ptrdiff_t>(n) * 16);
''', '''    while (g_pend3_head < g_pend3.size()) {
        const std::size_t waiting = g_pend3.size() - g_pend3_head;
        const std::uint32_t n = everything ? static_cast<std::uint32_t>(waiting / 16) : packet_qwords(g_pend3.data() + g_pend3_head, waiting);
        if (!n) break;
        const std::uint8_t *packet = g_pend3.data() + g_pend3_head;
        g_pend3_head += static_cast<std::size_t>(n) * 16;
'''),
        ('''        transfer_direct(3, chunk.data(), n);
        if (!everything) break;
    }
}
''', '''        transfer_direct(3, packet, n);
        if (!everything) break;
    }
    if (g_pend3_head >= g_pend3.size()) {
        g_pend3.clear();
        g_pend3_head = 0;
    }
}
'''),
        ('''    if (path == 3 && (mask_up() || !g_pend3.empty())) {
        g_pend3.insert(g_pend3.end(), data, data + static_cast<std::size_t>(qwc) * 16);
''', '''    if (path == 3 && (mask_up() || !g_pend3.empty())) {
        bool compact = g_pend3_head > g_pend3.size() / 2;   // only now and then, and only here, where the vector may move anyway
#ifdef __3DS__
        // Memory here has no room for this queue to double in size, so it gets a fixed block. A burst bigger than that goes out
        // early, which risks a texture arriving ahead of its turn but not running out of memory.
        constexpr std::size_t kPend3Limit = 2u << 20;
        const std::size_t incoming = static_cast<std::size_t>(qwc) * 16;
        if (!g_pend3.capacity()) g_pend3.reserve(kPend3Limit);
        if (g_pend3.size() - g_pend3_head + incoming > kPend3Limit) release(true);
        if (incoming > kPend3Limit) {
            transfer_direct(path, data, qwc);
            return;
        }
        compact = compact || g_pend3.size() + incoming > kPend3Limit;
#endif
        if (compact) {
            g_pend3.erase(g_pend3.begin(), g_pend3.begin() + static_cast<std::ptrdiff_t>(g_pend3_head));
            g_pend3_head = 0;
        }
        g_pend3.insert(g_pend3.end(), data, data + static_cast<std::size_t>(qwc) * 16);
'''),
    ],
    'runtime/src/dmac.cpp#speed': [
        ('''    const bool wrap = mfifo_vif1();
    for (std::uint32_t i = 0; i < qwc; ++i) {
        const std::uint32_t at = wrap ? ring(c.madr + i * 16) : c.madr + i * 16;
        std::memcpy(target(at), g_spr + ((c.sadr + i * 16) & (kScratchpadSize - 1)), 16);
    }
    c.sadr = (c.sadr + qwc * 16) & (kScratchpadSize - 1);
''', '''    const bool wrap = mfifo_vif1();
    std::uint32_t madr = c.madr, sadr = c.sadr;
    for (std::uint32_t left = qwc; left;) {   // ps2run_3ds_patch.py: in runs, not 16 bytes at a time
        const std::uint32_t at = wrap ? ring(madr) : madr;
        const std::uint32_t from = sadr & (kScratchpadSize - 1);
        const std::uint32_t to = (at & 0x80000000u) ? at & (kScratchpadSize - 1) : at & (kRamSize - 16);
        const std::uint32_t end = (at & 0x80000000u) ? kScratchpadSize : kRamSize;
        std::uint32_t n = std::min(left, std::min(kScratchpadSize - from, end - to) / 16);
        if (wrap) n = std::min(n, ((g_rbsr - (at & g_rbsr)) >> 4) + 1);
        if (!n) n = 1;
        std::memcpy(target(at), g_spr + from, static_cast<std::size_t>(n) * 16);
        madr += n * 16;
        sadr += n * 16;
        left -= n;
    }
    c.sadr = (c.sadr + qwc * 16) & (kScratchpadSize - 1);
'''),
    ],
    'runtime/src/vif.cpp#speed': [
        ('''        const std::uint32_t cmd = words[pos];
        const std::size_t start = pos + 1;
''', '''        const std::uint32_t cmd = words[pos];
        if (!cmd) {   // padding: about 40000 of these per picture in this game
            u.code = 0;
            ++pos;
            continue;
        }
        const std::size_t start = pos + 1;
'''),
        ('''        const std::uint32_t count = num ? num : 256u;
        bool changed = false;
''', '''        const std::uint32_t count = num ? num : 256u;
        const std::uint32_t at_first = (imm * 8u) & (size - 1);
        if (at_first + count * 8u <= size && std::memcmp(code + at_first, data, count * 8u) == 0) break;   // the usual case
        bool changed = false;
'''),
        ('''    std::uint32_t cycle = 0;
    for (std::uint32_t n = 0; n < num; ++n) {
        std::uint32_t value[4];
''', '''    std::uint32_t cycle = 0;
    const bool keep_zw = v2_keep(), zero_zw = v2_zero();
    for (std::uint32_t n = 0; n < num; ++n) {
        std::uint32_t value[4];
'''),
        ('''            if (v2_keep()) {
                std::uint32_t old[4];
''', '''            if (keep_zw) {
                std::uint32_t old[4];
'''),
        ('''            } else if (v2_zero()) {
                value[2] = value[3] = 0;
            } else {
                value[2] = value[0];
                value[3] = value[1];
            }
        } else if constexpr (Components == 3) {
''', '''            } else if (zero_zw) {
                value[2] = value[3] = 0;
            } else {
                value[2] = value[0];
                value[3] = value[1];
            }
        } else if constexpr (Components == 3) {
'''),
    ],
    'runtime/src/gs/queue.hpp': [
        ('kPath3Flush, kPath3ChainEnd };', 'kPath3Flush, kPath3ChainEnd, kVuStop };'),
    ],
    'runtime/src/gs/queue.cpp': [
        ('''#include "queue.hpp"

#include <algorithm>
''', '''#include "queue.hpp"

#include "ps2/system.hpp"

#include <algorithm>
'''),
        ('''namespace ps2::gs::queue {

namespace {

constexpr std::uint64_t align16''', '''#ifdef __3DS__
// ps2run_3ds_patch.py: the two threads run on different cores, so a short sleep costs little and needs no kernel event.
extern "C" void svcSleepThread(std::int64_t nanoseconds);
#endif

namespace ps2::gs::queue {

namespace {

constexpr std::uint64_t align16'''),
        ('''#elif defined(__linux__)
    futex_wait(value, seen, -1);
#else
    value.wait(seen);
#endif
''', '''#elif defined(__linux__)
    futex_wait(value, seen, -1);
#elif defined(__3DS__)
    while (value.load() == seen) svcSleepThread(100000);
#else
    value.wait(seen);
#endif
'''),
        ('''#elif defined(__linux__)
    futex_wake(value);
#else
    value.notify_all();
#endif
''', '''#elif defined(__linux__)
    futex_wake(value);
#elif defined(__3DS__)
    (void)value;
#else
    value.notify_all();
#endif
'''),
        ('''        producer_waiting_.store(1);
        if (head + pad + need - tail_.load() > capacity_) wait(freed_, seen);
''', '''#ifdef __3DS__
        PS2_PROF(ps2::prof::Full);
        svcSleepThread(100000);
#else
        producer_waiting_.store(1);
        PS2_PROF(ps2::prof::Full);
        if (head + pad + need - tail_.load() > capacity_) wait(freed_, seen);
#endif
'''),
        ('''#elif defined(__linux__)
    futex_wait(posted_, seen, timeout_ms);
#else
    (void)seen;
    std::this_thread::sleep_for''', '''#elif defined(__linux__)
    futex_wait(posted_, seen, timeout_ms);
#elif defined(__3DS__)
    (void)seen, (void)timeout_ms;
    svcSleepThread(200000);
#else
    (void)seen;
    std::this_thread::sleep_for'''),
    ],
    'runtime/src/gs/gif.cpp#thread': [
        ('''    if (gs::queue::role() != gs::queue::Role::Vu) scan(g_scan[path], data, qwc);
    gs::queue::post(gs::queue::kGif, static_cast<std::uint32_t>(path), qwc, data, qwc * 16);
''', '''    if (gs::queue::role() != gs::queue::Role::Vu) scan(g_scan[path], data, qwc);
#ifdef PS2_PICA
    if (gs::queue::role() == gs::queue::Role::Vu) {   // the 3DS has no third thread: the VU thread also draws
        render(path, data, qwc);
        return;
    }
#endif
    gs::queue::post(gs::queue::kGif, static_cast<std::uint32_t>(path), qwc, data, qwc * 16);
'''),
    ],
    'runtime/src/gs/gs.cpp#thread': [
        ('''        if (queue::active()) queue::post(queue::kVblank, 0, 0, nullptr, 0);
#ifdef PS2_GPU
''', '''#ifdef PS2_PICA
        // The EE runs ahead of the drawing, so the registers saying which picture to show travel with the message. Read later,
        // they would point at the buffer the EE has moved on to, which is still half drawn.
        if (queue::active()) {
            const DisplayRegs regs = display_regs();
            const VblankMessage message{regs, pica::active() ? pica::next_picture() : 0u};
            queue::post(queue::kVblank, static_cast<std::uint32_t>(ps2::frame_count()), 0, &message, sizeof message);
        }
#else
        if (queue::active()) queue::post(queue::kVblank, 0, 0, nullptr, 0);
#endif
#ifdef PS2_GPU
'''),
        ('''void start_thread() { queue::render().start(32u << 20); }
''', '''#ifdef PS2_PICA
void start_thread() { queue::render().start(4096); }   // never carries anything: serve() below takes the messages directly
#else
void start_thread() { queue::render().start(32u << 20); }
#endif
'''),
        ('''std::uint64_t primitives_drawn() { return g_primitives; }
''', '''#ifdef PS2_PICA
// What render_pending does for the other builds, for the thread that runs the VU and draws.
void serve(std::uint32_t type, std::uint32_t arg0, const std::uint8_t *payload) {
    switch (type) {
    case queue::kGifReset: gif::render_reset(); break;
    case queue::kReset: render_reset(); break;
    case queue::kVertexReset: g_state.count = 0; break;
    case queue::kVblank: {
        VblankMessage message;
        std::memcpy(&message, payload, sizeof message);
        if (pica::active()) pica::vblank(arg0, message.regs, message.picture);
        break;
    }
    case queue::kState: serve_state(payload); break;
    case queue::kDownload: {
        std::uintptr_t target;
        std::memcpy(&target, payload, sizeof target);
        exact_download();
        render_read_image(reinterpret_cast<std::uint8_t *>(target), arg0);
        g_downloads.fetch_add(1);
        queue::notify(g_downloads);
        break;
    }
    case queue::kDownloadAsync: {
        DownloadKey key;
        std::memcpy(&key, payload, sizeof key);
        std::vector<std::uint8_t> fresh(static_cast<std::size_t>(arg0) * 16);
        render_read_image(fresh.data(), arg0);
        if (key.buf == g_state.bitbltbuf && key.pos == g_state.trxpos && key.reg == g_state.trxreg) remember(key, fresh.data(), fresh.size());
        else forget(key);
        break;
    }
    default: break;
    }
}
#endif

std::uint64_t primitives_drawn() { return g_primitives; }
'''),
    ],
    'runtime/src/gs/gs.hpp#thread': [
        ('''void render_reset();

std::uint64_t read_privileged''', '''void render_reset();
void serve(std::uint32_t type, std::uint32_t arg0, const std::uint8_t *payload);   // ps2run_3ds_patch.py

std::uint64_t read_privileged'''),
    ],
    'runtime/src/system.cpp#thread': [
        ('''void start_vu_thread() { gs::queue::vu().start(32u << 20); }
''', '''#ifdef __3DS__
void start_vu_thread() { gs::queue::vu().start(7u << 20); }   // 7 MB: enough for a whole picture to wait while the last one is drawn
void stop_vu_thread() { gs::queue::vu().post(gs::queue::kVuStop, 0, 0, nullptr, 0); }
#else
void start_vu_thread() { gs::queue::vu().start(32u << 20); }
#endif
'''),
        ('''                if (h.arg0 == 3) gif::vu_path3(payload, h.arg1);
                else gs::queue::render().post(static_cast<gs::queue::Type>(h.type), h.arg0, h.arg1, payload, h.bytes);
                break;
''', '''                if (h.arg0 == 3) gif::vu_path3(payload, h.arg1);
#ifdef PS2_PICA
                else gif::render(static_cast<int>(h.arg0), payload, h.arg1);
#else
                else gs::queue::render().post(static_cast<gs::queue::Type>(h.type), h.arg0, h.arg1, payload, h.bytes);
#endif
                break;
'''),
        ('''            default: gs::queue::render().post(static_cast<gs::queue::Type>(h.type), h.arg0, h.arg1, payload, h.bytes); break;
            }
            in.pop(h);
        }
        const auto start = std::chrono::steady_clock::now();
        in.wait_for_work(50);
''', '''#ifdef PS2_PICA
            case gs::queue::kVuStop:
                in.pop(h);
                return;
            default: gs::serve(h.type, h.arg0, payload); break;
#else
            default: gs::queue::render().post(static_cast<gs::queue::Type>(h.type), h.arg0, h.arg1, payload, h.bytes); break;
#endif
            }
            in.pop(h);
        }
        const auto start = std::chrono::steady_clock::now();
        {
            PS2_PROF(ps2::prof::Wait);
            in.wait_for_work(50);
        }
'''),
    ],
    'runtime/include/ps2/system.hpp#thread': [
        ('''void start_vu_thread();
''', '''void start_vu_thread();
void stop_vu_thread();
'''),
    ],
    'runtime/include/ps2/memory.hpp#speed': [
        ('''inline void st32(std::uint32_t a, std::uint32_t v) {
    if (a - g_swlo < g_swlen) [[unlikely]] store_watch_hit(a, 4, &v);
''', '''inline void st32(std::uint32_t a, std::uint32_t v) {
#ifndef __3DS__
    if (a - g_swlo < g_swlen) [[unlikely]] store_watch_hit(a, 4, &v);
#endif
'''),
        ('''inline void st64(std::uint32_t a, std::uint64_t v) {
    if (a + 4u - g_swlo < g_swlen + 4u) [[unlikely]] store_watch_hit(a, 8, &v);
''', '''inline void st64(std::uint32_t a, std::uint64_t v) {
#ifndef __3DS__
    if (a + 4u - g_swlo < g_swlen + 4u) [[unlikely]] store_watch_hit(a, 8, &v);
#endif
'''),
        ('''inline void st128(std::uint32_t a, const Reg128 &v) {
    if (a + 12u - g_swlo < g_swlen + 12u) [[unlikely]] store_watch_hit(a, 16, &v);
''', '''inline void st128(std::uint32_t a, const Reg128 &v) {
#ifndef __3DS__
    if (a + 12u - g_swlo < g_swlen + 12u) [[unlikely]] store_watch_hit(a, 16, &v);
#endif
'''),
    ],
    'runtime/include/ps2/fpu.hpp': [
        ('''inline std::uint32_t add(std::uint32_t a, std::uint32_t b) {
    a = flush(a);
''', '''#ifdef __3DS__
// ps2run_3ds_patch.py. The functions below this block work on the bits, which is exact and portable and costs about 80 instructions
// for an add. The ARM11 has a hardware FPU, and its threads start with denormals flushed and rounding toward zero, as on the
// PS2. What is left to do is the PS2's lack of infinities: an exponent of 255 is an ordinary huge number there.
inline float to_float(std::uint32_t x) { return std::bit_cast<float>(x); }
inline std::uint32_t from_float(float f) { return std::bit_cast<std::uint32_t>(f); }
inline std::uint32_t add(std::uint32_t a, std::uint32_t b) { return from_float(to_float(a) + to_float(b)); }
inline std::uint32_t sub(std::uint32_t a, std::uint32_t b) { return from_float(to_float(a) - to_float(b)); }
inline std::uint32_t mul(std::uint32_t a, std::uint32_t b) { return from_float(to_float(a) * to_float(b)); }
#else
inline std::uint32_t add(std::uint32_t a, std::uint32_t b) {
    a = flush(a);
'''),
        ('''    return pack(sign, e, static_cast<std::uint32_t>(m));
}

inline double to_double(std::uint32_t x) {
''', '''    return pack(sign, e, static_cast<std::uint32_t>(m));
}
#endif

inline double to_double(std::uint32_t x) {
'''),
        ('''inline Result div(std::uint32_t a, std::uint32_t b) {
    a = flush(a);
    b = flush(b);
    const std::uint32_t sign = (a ^ b) & kSign;
    if (!(b & kAbs)) return {sign | kAbs, true};
    if (!(a & kAbs)) return {sign, false};
    return {from_double(to_double(a) / to_double(b)), false};
}

inline std::uint32_t sqrt(std::uint32_t a) {
    a = flush(a) & kAbs;
    if (!a) return 0;
    return from_double(std::sqrt(to_double(a)));
}
''', '''inline Result div(std::uint32_t a, std::uint32_t b) {
    a = flush(a);
    b = flush(b);
    const std::uint32_t sign = (a ^ b) & kSign;
    if (!(b & kAbs)) return {sign | kAbs, true};
    if (!(a & kAbs)) return {sign, false};
#ifdef __3DS__
    return {from_float(to_float(a) / to_float(b)), false};
#else
    return {from_double(to_double(a) / to_double(b)), false};
#endif
}

inline std::uint32_t sqrt(std::uint32_t a) {
    a = flush(a) & kAbs;
    if (!a) return 0;
#ifdef __3DS__
    return from_float(std::sqrt(to_float(a)));
#else
    return from_double(std::sqrt(to_double(a)));
#endif
}
'''),
        ('''    if (!(a & kAbs)) return {a & kSign, false};
    return {from_double(to_double(a) / std::sqrt(to_double(b))), false};
''', '''    if (!(a & kAbs)) return {a & kSign, false};
#ifdef __3DS__
    return {from_float(to_float(a) / std::sqrt(to_float(b))), false};
#else
    return {from_double(to_double(a) / std::sqrt(to_double(b))), false};
#endif
'''),
        ('''inline std::uint32_t to_fixed(std::uint32_t x, int fraction) {
    const int e = static_cast<int>(exponent(x)) + fraction;
''', '''inline std::uint32_t to_fixed(std::uint32_t x, int fraction) {
#ifdef __3DS__
    if (fraction == 0) return static_cast<std::uint32_t>(static_cast<std::int32_t>(to_float(x)));   // the FPU saturates as the PS2 does
#endif
    const int e = static_cast<int>(exponent(x)) + fraction;
'''),
        ('''inline std::uint32_t from_fixed(std::int32_t v, int fraction) {
    return from_double(static_cast<double>(v) / static_cast<double>(1u << fraction));
''', '''inline std::uint32_t from_fixed(std::int32_t v, int fraction) {
#ifdef __3DS__
    if (fraction == 0) return std::bit_cast<std::uint32_t>(static_cast<float>(v));
#endif
    return from_double(static_cast<double>(v) / static_cast<double>(1u << fraction));
'''),
    ],
    'runtime/src/gs/queue.hpp#skip': [
        ('''bool active();
bool vu_active();
''', '''bool active();
bool vu_active();
extern bool g_ee_skip;        // ps2run_3ds_patch.py: the same for the game thread, which is ahead of the drawing side
extern bool g_quiet;         // ps2run_3ds_patch.py: nothing is drawn while this is set
extern bool g_skip_picture;   // ps2run_3ds_patch.py: set by the drawing side when the picture being made will not be shown
'''),
    ],
    'runtime/src/gs/queue.cpp#skip': [
        ('''Ring &render() { return g_render; }
''', '''bool g_skip_picture = false;
bool g_quiet = false;
bool g_ee_skip = false;

Ring &render() { return g_render; }
'''),
    ],
    'runtime/src/vif.cpp#skip': [
        ('''void run_vu(int unit, std::uint32_t address) {
    Unit &u = state(unit);
''', '''void run_vu(int unit, std::uint32_t address, bool resume = false) {
    Unit &u = state(unit);
'''),
        ('''        if (shadow(unit)) return;
        vu::set_tops(u.top, u.itop);
''', '''        if (shadow(unit)) return;
        vu::set_tops(u.top, u.itop);
#ifdef PS2_PICA
        // A picture that will not be shown needs no vertices. Uploads and registers all still happen, so the next picture finds
        // the same machine state, and a program not seen before runs in case it is one that sets things up once.
        if (gs::queue::g_skip_picture && (resume || vu::seen(address))) return;
        if (gs::queue::g_skip_picture) {
            gs::queue::g_quiet = true;   // its input was left out, so what it draws would be wrong
            vu::run(unit, address);
            gs::queue::g_quiet = false;
            return;
        }
#endif
'''),
        ('''    case 0x17: run_vu(unit, shadow(unit) ? 0u : vu::tpc(unit)); break;
''', '''    case 0x17: run_vu(unit, shadow(unit) ? 0u : vu::tpc(unit), true); break;
'''),
    ],
    'runtime/src/vu_micro.hpp': [
        ('''void reset();
''', '''void reset();
bool seen(std::uint32_t pc);   // ps2run_3ds_patch.py: the compiled program at this address has run before
'''),
    ],
    'runtime/src/vu_micro.cpp#skip': [
        ('''void run_entry(int unit, std::uint32_t pc) {
    PS2_PROF(ps2::prof::Vu);
''', '''#ifdef PS2_PICA
ProgramFn g_ran[1024];

bool ran(ProgramFn fn, bool mark) {
    for (std::uint32_t i = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(fn) >> 3);; ++i) {
        ProgramFn &slot = g_ran[i & 1023u];
        if (slot == fn) return true;
        if (!slot) {
            if (mark) slot = fn;
            return false;
        }
    }
}

bool seen(std::uint32_t pc) {
    const ProgramFn fn = lookup(pc);
    return fn && ran(fn, false);
}
#endif

void run_entry(int unit, std::uint32_t pc) {
    PS2_PROF(ps2::prof::Vu);
'''),
        ('''            if (jit && jit != fn && ++sampled % 32 == 0) check_jit(fn, jit);
            else fn(g_vu1, g_vu1_mem);
            return;
''', '''#ifdef PS2_PICA
            ran(fn, true);
#endif
            if (jit && jit != fn && ++sampled % 32 == 0) check_jit(fn, jit);
            else fn(g_vu1, g_vu1_mem);
            return;
'''),
    ],
    'runtime/include/ps2/vu.hpp': [
        ('''#include <cstdint>

namespace ps2::vu {
''', '''#include <bit>
#include <cstdint>

namespace ps2::vu {
'''),
        ('''inline void add(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {
''', '''#ifdef __3DS__
// Bone and object matrices spend most of the game's own time in these. Going through fp:: moves every number to an ARM register
// and back to look for the PS2's overflow, as often as eight stalls per lane, so here they stay in the FPU. As in the vector
// programs, a result that overflows becomes an infinity instead of the PS2's largest number.
using Lane = float __attribute__((may_alias));
#define PS2_LANE [[gnu::always_inline]] inline
PS2_LANE const Lane *lanes(const Reg128 &r) { return reinterpret_cast<const Lane *>(&r); }

template <typename F>
PS2_LANE void put(Reg128 &to, unsigned dest, F f) {
    const float x = f(0), y = f(1), z = f(2), w = f(3);
    Lane *o = reinterpret_cast<Lane *>(&to);
    if (dest & 8u) o[0] = x;
    if (dest & 4u) o[1] = y;
    if (dest & 2u) o[2] = z;
    if (dest & 1u) o[3] = w;
}

#define PS2_LANE_OP(name, body)                                                                        \\
    PS2_LANE void name(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {             \\
        if (!fd) return;                                                                               \\
        const Lane *s = lanes(v.vf[fs]), *t = lanes(v.vf[ft]), *a = lanes(v.acc);                      \\
        (void)a;                                                                                       \\
        put(v.vf[fd], dest, [&](int i) { return body; });                                              \\
    }                                                                                                  \\
    PS2_LANE void name##bc(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, std::uint32_t bc) {    \\
        if (!fd) return;                                                                               \\
        const Lane *s = lanes(v.vf[fs]), *a = lanes(v.acc);                                            \\
        (void)a;                                                                                       \\
        const float one = std::bit_cast<float>(bc);                                                    \\
        const float t[4] = {one, one, one, one};                                                       \\
        put(v.vf[fd], dest, [&](int i) { return body; });                                              \\
    }                                                                                                  \\
    PS2_LANE void name##a(Vu0Regs &v, unsigned dest, unsigned fs, unsigned ft) {                       \\
        const Lane *s = lanes(v.vf[fs]), *t = lanes(v.vf[ft]), *a = lanes(v.acc);                      \\
        (void)a;                                                                                       \\
        put(v.acc, dest, [&](int i) { return body; });                                                 \\
    }                                                                                                  \\
    PS2_LANE void name##abc(Vu0Regs &v, unsigned dest, unsigned fs, std::uint32_t bc) {                \\
        const Lane *s = lanes(v.vf[fs]), *a = lanes(v.acc);                                            \\
        (void)a;                                                                                       \\
        const float one = std::bit_cast<float>(bc);                                                    \\
        const float t[4] = {one, one, one, one};                                                       \\
        put(v.acc, dest, [&](int i) { return body; });                                                 \\
    }

PS2_LANE_OP(add, s[i] + t[i])
PS2_LANE_OP(sub, s[i] - t[i])
PS2_LANE_OP(mul, s[i] * t[i])
PS2_LANE_OP(madd, a[i] + s[i] * t[i])
PS2_LANE_OP(msub, a[i] - s[i] * t[i])
#else
inline void add(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {
'''),
        ('''inline void max(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {
''', '''#endif
inline void max(Vu0Regs &v, unsigned dest, unsigned fd, unsigned fs, unsigned ft) {
'''),
        ('''inline void adda(Vu0Regs &v, unsigned dest, unsigned fs, unsigned ft) {
''', '''#ifndef __3DS__
inline void adda(Vu0Regs &v, unsigned dest, unsigned fs, unsigned ft) {
'''),
        ('''inline void opmula(Vu0Regs &v, unsigned fs, unsigned ft) {
''', '''#endif

inline void opmula(Vu0Regs &v, unsigned fs, unsigned ft) {
'''),
    ],
    'runtime/include/ps2/memory.hpp#near': [
        ('''    if ((a >> 14) == (kScratchpadBase >> 14)) return reinterpret_cast<T *>(g_spr + (a & (kScratchpadSize - 1)));
    return nullptr;
''', '''#ifndef __3DS__   // ps2run_3ds_patch.py: the slow calls reach the scratchpad, and each memory access in the game gets smaller
    if ((a >> 14) == (kScratchpadBase >> 14)) return reinterpret_cast<T *>(g_spr + (a & (kScratchpadSize - 1)));
#endif
    return nullptr;
'''),
        ('std::uint8_t slow_ld8(std::uint32_t a);\n', '[[gnu::cold]] std::uint8_t slow_ld8(std::uint32_t a);\n'),
        ('std::uint16_t slow_ld16(std::uint32_t a);\n', '[[gnu::cold]] std::uint16_t slow_ld16(std::uint32_t a);\n'),
        ('std::uint32_t slow_ld32(std::uint32_t a);\n', '[[gnu::cold]] std::uint32_t slow_ld32(std::uint32_t a);\n'),
        ('std::uint64_t slow_ld64(std::uint32_t a);\n', '[[gnu::cold]] std::uint64_t slow_ld64(std::uint32_t a);\n'),
        ('void slow_ld128(std::uint32_t a, Reg128 &out);\n', '[[gnu::cold]] void slow_ld128(std::uint32_t a, Reg128 &out);\n'),
        ('void slow_st8(std::uint32_t a, std::uint8_t v);\n', '[[gnu::cold]] void slow_st8(std::uint32_t a, std::uint8_t v);\n'),
        ('void slow_st16(std::uint32_t a, std::uint16_t v);\n', '[[gnu::cold]] void slow_st16(std::uint32_t a, std::uint16_t v);\n'),
        ('void slow_st32(std::uint32_t a, std::uint32_t v);\n', '[[gnu::cold]] void slow_st32(std::uint32_t a, std::uint32_t v);\n'),
        ('void slow_st64(std::uint32_t a, std::uint64_t v);\n', '[[gnu::cold]] void slow_st64(std::uint32_t a, std::uint64_t v);\n'),
        ('void slow_st128(std::uint32_t a, const Reg128 &v);\n', '[[gnu::cold]] void slow_st128(std::uint32_t a, const Reg128 &v);\n'),
    ],
    'runtime/src/gs/gs.cpp#picture': [
        ('''void serve_state(const void *payload);
''', '''void serve_state(const void *payload);
#ifdef PS2_PICA
struct VblankMessage {
    DisplayRegs regs;
    std::uint32_t picture;
};
#endif
'''),
    ],
    'runtime/src/gs/queue.cpp#poll': [
        ('''        const std::uint32_t seen = freed_.load(std::memory_order_acquire);
''', '''#ifndef __3DS__
        const std::uint32_t seen = freed_.load(std::memory_order_acquire);
#endif
'''),
        ('''    head_.store(head + need, std::memory_order_release);
    posted_.fetch_add(1);
    if (consumer_waiting_.exchange(0)) notify(posted_);
}
''', '''    head_.store(head + need, std::memory_order_release);
#ifndef __3DS__   // ps2run_3ds_patch.py: both sides poll here, and these two cost more than the copy for a small message
    posted_.fetch_add(1);
    if (consumer_waiting_.exchange(0)) notify(posted_);
#endif
}
'''),
        ('''    tail_.store(tail_.load(std::memory_order_relaxed) + sizeof(Header) + align16(header.bytes), std::memory_order_release);
    freed_.fetch_add(1);
    if (producer_waiting_.exchange(0)) notify(freed_);
}
''', '''    tail_.store(tail_.load(std::memory_order_relaxed) + sizeof(Header) + align16(header.bytes), std::memory_order_release);
#ifndef __3DS__
    freed_.fetch_add(1);
    if (producer_waiting_.exchange(0)) notify(freed_);
#endif
}
'''),
    ],
    'runtime/src/vif.cpp#picture': [
        ('''    const bool dry = shadow(unit);
    if (dry && u.mode != 2) {
''', '''    bool dry = shadow(unit);
#ifdef PS2_PICA
    // Data for the double buffer is what a skipped picture's programs would have read, and they do not run.
    if (unit == 1 && gs::queue::g_skip_picture && ((cmd >> 15) & 1u)) dry = true;
#endif
    if (dry && u.mode != 2) {
'''),
        ('''void process(int unit) {
    Unit &u = state(unit);
''', '''#ifdef PS2_PICA
// A skipped picture needs its programs and registers but not the data for the double buffer, and copying that across is the game
// thread's largest single cost. So only the rest goes to the VU thread, which then sees the same commands apart from those.
constexpr std::size_t kKeptWords = 1u << 14;
std::uint32_t g_kept[kKeptWords];

std::size_t run_skipped(int unit, const std::uint32_t *words, std::size_t count) {
    Unit &u = state(unit);
    std::size_t pos = 0, kept = 0;
    while (pos < count && !u.stalled) {
        const std::uint32_t cmd = words[pos];
        if (!cmd) {
            u.code = 0;
            ++pos;
            continue;
        }
        const std::size_t start = pos + 1;
        const std::uint32_t need = data_words(unit, cmd);
        if (start + need > count) break;
        const bool buffered = (cmd & 0xE0008000u) == 0x60008000u && u.mode != 2;
        execute(unit, cmd, words + start);
        if (!buffered) {
            std::memcpy(g_kept + kept, words + pos, (1u + need) * 4u);
            kept += 1u + need;
        }
        pos = start + need;
    }
    // A command cut off by the end of the transfer has to arrive whole, so the padding goes before it.
    const std::size_t left = count - pos;
    while ((kept + left) & 3u) g_kept[kept++] = 0;
    std::memcpy(g_kept + kept, words + pos, left * 4u);
    kept += left;
    if (kept) gs::queue::vu().post(gs::queue::kVif1, static_cast<std::uint32_t>(kept / 4u), 0, g_kept, static_cast<std::uint32_t>(kept * 4u));
    return pos;
}
#endif

void process(int unit) {
    Unit &u = state(unit);
'''),
        ('''    if (shadow(unit)) gs::queue::vu().post(gs::queue::kVif1, qwc, 0, data, qwc * 16);
''', '''#ifdef PS2_PICA
    if (shadow(unit) && gs::queue::g_ee_skip && qwc * 4u <= kKeptWords) {
        Unit &u = state(unit);
        if (u.pending.empty() && !u.stalled) {
            const auto *words = reinterpret_cast<const std::uint32_t *>(data);
            const std::size_t count = static_cast<std::size_t>(qwc) * 4;
            const std::size_t pos = run_skipped(unit, words, count);
            if (pos < count) u.pending.assign(words + pos, words + count);
            return;
        }
    }
#endif
    if (shadow(unit)) gs::queue::vu().post(gs::queue::kVif1, qwc, 0, data, qwc * 16);
'''),
    ],
    'runtime/src/iop/cpu.cpp': [
        ('''    static const bool probing = load_probes();
    while (budget-- > 0) {
''', '''    static const bool probing = load_probes();
#ifdef __3DS__
    std::int32_t left = static_cast<std::int32_t>(std::min<std::int64_t>(budget, 0x7FFFFFFF));   // a 64-bit count costs six instructions here
    const auto leave = [&] { budget = left; };
    struct Leave {
        decltype(leave) &f;
        ~Leave() { f(); }
    } at_exit{leave};
#else
    std::int64_t &left = budget;
#endif
    while (left-- > 0) {
'''),
    ],
}


def fail(msg):
    print("PATCH NOT APPLIED:", msg)
    raise SystemExit(1)


# Check everything first, so a refused patch changes nothing.
texts = {}
pending = 0
for key, edits in EDITS.items():
    rel = key.split('#')[0]   # the part after # only keeps two lists for one file apart
    try:
        with open(root + '/' + rel, newline='') as f:
            t = texts[rel] if rel in texts else f.read()
    except OSError:
        fail("cannot read " + rel)
    for old, new in edits:
        if new in t:
            continue
        if t.count(old) != 1:
            fail(rel + " does not look like the expected source (cannot find: " + old.strip().splitlines()[0][:70] + ")")
        t = t.replace(old, new)
        pending += 1
    texts[rel] = t

if not pending:
    print("already patched")
    raise SystemExit

for rel, t in texts.items():
    with open(root + '/' + rel, 'w', newline='') as f:
        f.write(t)
print("patched (%d edits)" % pending)
