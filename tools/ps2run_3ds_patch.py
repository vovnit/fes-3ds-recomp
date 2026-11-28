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
        ('    std::map<std::uint32_t, std::string> natives;   // ps2run_3ds_patch.py\n', '''    std::map<std::uint32_t, std::string> natives;   // ps2run_3ds_patch.py
    bool record_seen = false;   // ps2run_3ds_patch.py: eerecomp --record-seen and --seen
    std::uint32_t seen_base = 0;
    std::vector<std::uint8_t> seen;
'''),
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
        ('    bool stats = false;\n', '    bool stats = false, record_seen = false;   // ps2run_3ds_patch.py\n    std::vector<std::string> seen_paths;\n'),
        ('''        else if (arg == "--natives" && i + 1 < argc) natives_path = argv[++i];
''', '''        else if (arg == "--natives" && i + 1 < argc) natives_path = argv[++i];
        else if (arg == "--record-seen") record_seen = true;
        else if (arg == "--seen" && i + 1 < argc) seen_paths.push_back(argv[++i]);
'''),
        ('''            if (!natives_path.empty()) {   // ps2run_3ds_patch.py
''', '''            options.record_seen = record_seen;   // ps2run_3ds_patch.py
            for (const std::string &path : seen_paths) {   // each --seen file adds what one recording reached
                std::ifstream file(path, std::ios::binary);
                std::uint32_t head[3] = {};
                if (!file.read(reinterpret_cast<char *>(head), sizeof head) || head[0] != 0x31534545u) throw std::runtime_error("not a recording: " + path);
                if (!options.seen.empty() && head[1] != options.seen_base) throw std::runtime_error("recorded from another program: " + path);
                std::vector<std::uint8_t> bytes(head[2]);
                if (!file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) throw std::runtime_error("short recording: " + path);
                options.seen_base = head[1];
                options.seen.resize(std::max(options.seen.size(), bytes.size()));
                for (std::size_t k = 0; k < bytes.size(); ++k) options.seen[k] |= bytes[k];
            }
            if (!natives_path.empty()) {   // ps2run_3ds_patch.py
'''),
    ],
    # eerecomp --record-seen makes code that sets a byte as each label runs, one byte per 16 bytes of the program, which keeps the
    # table small enough to fit beside the game. The 3DS port writes those bytes to sdmc:/p3fes/eeseen.bin when a "frames" test
    # ends. eerecomp --seen FILE (any number of times) then marks every label that no recording reached as cold, and a unit with
    # none reached as a cold function. GCC moves cold code to the end of each function and makes it smaller, so the code that
    # runs sits closer together in the instruction cache.
    'recomp/codegen.cpp#seen': [
        ('''    std::string rebase() const { return relocatable_ ? "c.base = B; " : ""; }
''', r'''    std::string rebase() const { return relocatable_ ? "c.base = B; " : ""; }
    std::string heat(std::uint32_t a) const {   // ps2run_3ds_patch.py
        if (relocatable_) return "";
        if (options_.record_seen) return std::format(" g_ee_seen[{}u] = 1;", (a - layout_.origin()) >> 4);
        const std::uint32_t k = (a - options_.seen_base) >> 4;
        return k < options_.seen.size() && !options_.seen[k] ? " __attribute__((cold));" : "";
    }
    bool unit_ran(std::uint32_t u) const {
        if (relocatable_ || options_.seen.empty()) return true;
        for (std::uint32_t a = layout_.base_of(u); a < layout_.base_of(u) + layout_.size(); a += 16)
            if (const std::uint32_t k = (a - options_.seen_base) >> 4; k >= options_.seen.size() || options_.seen[k]) return true;
        return false;
    }
'''),
        ('''        out_ << "#include \\"units.hpp\\"\\n\\nnamespace gen {\\n\\nusing namespace ps2;\\n\\n";
''', r'''        out_ << "#include \"units.hpp\"\n\nnamespace gen {\n\nusing namespace ps2;\n\n";
        if (options_.record_seen && !relocatable_) out_ << "extern std::uint8_t g_ee_seen[];\n\n";
'''),
        ('''        out_ << "void " << unit_name(u) << "(Cpu &c, std::uint32_t entry, std::uint32_t depth) {\\n";
''', r'''        out_ << (unit_ran(u) ? "" : "__attribute__((cold)) ") << "void " << unit_name(u) << "(Cpu &c, std::uint32_t entry, std::uint32_t depth) {\n";
'''),
        ('''                out_ << label(a) << ":\\n";
''', r'''                out_ << label(a) << ":" << heat(a) << "\n";
'''),
        ('''    write_if_changed(fs::path(options.output) / "units.hpp", header.str());
''', r'''    {   // ps2run_3ds_patch.py: the bytes --record-seen sets, one per 16 bytes of the program, and their writer
        std::uint32_t base = 0, count = 0;
        for (const Translation &t : translations)
            if (!t.relocatable) {
                base = t.program->base & ~((1u << options.unit_bits) - 1u);
                count = (t.program->end - base + 15u) >> 4;
            }
        table << "\n#include <cstdio>\n\n";
        if (options.record_seen) table << std::format("namespace gen {{ std::uint8_t g_ee_seen[{}]; }}\n\n", count);
        table << "bool ps2_dump_ee_seen(const char *path) {\n";
        if (options.record_seen)
            table << std::format("    std::FILE *f = std::fopen(path, \"wb\");\n    if (!f) return false;\n"
                                 "    const std::uint32_t head[3] = {{0x31534545u, {:#x}u, {}u}};\n    std::fwrite(head, 4, 3, f);\n"
                                 "    std::fwrite(gen::g_ee_seen, 1, sizeof gen::g_ee_seen, f);\n    std::fclose(f);\n    return true;\n", base, count);
        else
            table << "    (void)path;\n    return false;\n";
        table << "}\n";
    }
    write_if_changed(fs::path(options.output) / "units.hpp", header.str());
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


# These edits build on the existing port and can also upgrade a previously patched tree.
PERF_EDITS = {
    'runtime/src/dmac.hpp': [
        ('bool ipu_pull(std::uint8_t *qword);\n', '''bool ipu_pull(std::uint8_t *qword);
#ifdef __3DS__
std::size_t ipu_pull_many(std::uint8_t *data, std::size_t qwords);
#endif
'''),
    ],
    'runtime/src/dmac.cpp': [
        ('''bool ipu_push(const std::uint8_t *qword) {
''', '''#ifdef __3DS__
std::size_t ipu_pull_many(std::uint8_t *data, std::size_t qwords) {
    if (!qwords) return 0;
    Channel &c = g_ch[4];
    // Let the single-word path read the next tag, end the transfer or handle unaligned input.
    if (!(c.chcr & 0x100u) || !c.qwc || (c.madr & 15u))
        return ipu_pull(data) ? 1 : 0;
    const std::uint32_t room = (c.madr & 0x80000000u)
        ? kScratchpadSize - (c.madr & (kScratchpadSize - 1))
        : kRamSize - (c.madr & (kRamSize - 16));
    const std::size_t n = std::min<std::size_t>({qwords, c.qwc, room / 16});
    std::memcpy(data, source(c.madr), n * 16);
    c.madr += static_cast<std::uint32_t>(n * 16);
    c.qwc -= static_cast<std::uint32_t>(n);
    if (!c.qwc && (((c.chcr >> 2) & 3u) != 1 || c.tag_end)) finish(4);
    return n;
}
#endif

bool ipu_push(const std::uint8_t *qword) {
'''),
    ],
    'runtime/src/ipu/ipu.cpp': [
        ('''void convert_fast(const std::uint8_t *raw) {   // 32-bit pixels, no alpha thresholds
''', '''bool have_csc_input() {
    const std::size_t end = g_bit + 384 * 8;
    while (g_in.size() * 8 < end) {
        std::uint8_t data[384];
        const std::size_t missing = end - g_in.size() * 8;
        const std::size_t qwords = std::min<std::size_t>((missing + 127) / 128, sizeof data / 16);
        const std::size_t n = dmac::ipu_pull_many(data, qwords);
        if (!n) return false;
        g_in.insert(g_in.end(), data, data + n * 16);
    }
    return true;
}

void convert_fast(const std::uint8_t *raw) {   // 32-bit pixels, no alpha thresholds
'''),
        ('''            if (!(g_bit & 7u) && !((cmd >> 27) & 1u) && !g_th0 && !g_th1 && have(384 * 8)) {
''', '''            if (!(g_bit & 7u) && !((cmd >> 27) & 1u) && !g_th0 && !g_th1 && have_csc_input()) {
'''),
        ('''        if (have(32)) {   // nearly always: one read of the stream for the end test, the code and its sign
            const std::uint32_t w = bits_at(g_bit, 32);
            if (index && (table_one ? w >> 28 == 6u : w >> 30 == 2u)) {
''', '''        if (have(32)) {   // keep the existing DMA refill threshold
            // A coefficient needs at most 16 code bits and one sign bit. Three bytes suffice at any bit offset.
            const std::uint8_t *p = g_in.data() + (g_bit >> 3);
            const std::uint32_t packed = (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8);
            const std::uint32_t w = packed << (g_bit & 7u);
            if (index && (table_one ? w >> 28 == 6u : w >> 30 == 2u)) {
'''),
        ('''inline void idct_column(const int *b, std::int16_t *o, int stride) {
    int x1 = b[32] * 256, x2 = b[48], x3 = b[16], x4 = b[8], x5 = b[56], x6 = b[40], x7 = b[24];
    if (!(x1 | x2 | x3 | x4 | x5 | x6 | x7)) {
        const std::int16_t v = static_cast<std::int16_t>(clip9((b[0] + 32) >> 6));
        for (int i = 0; i < 8; ++i) o[i * stride] = v;
        return;
    }
''', '''template <bool one_ac>
inline void idct_column(const int *b, std::int16_t *o, int stride) {
    int x1 = one_ac ? 0 : b[32] * 256, x2 = one_ac ? 0 : b[48], x3 = one_ac ? 0 : b[16], x4 = b[8];
    int x5 = one_ac ? 0 : b[56], x6 = one_ac ? 0 : b[40], x7 = one_ac ? 0 : b[24];
'''),
        ('''// Writes one block into a macroblock that is already zero, stride samples between its lines.
void decode_block(std::int16_t *out, int stride, int channel, bool intra, std::uint32_t qsc) {
''', '''// Writes all 64 samples of a block, with stride samples between output lines.
template <bool intra>
void decode_block(std::int16_t *out, int stride, int channel, std::uint32_t qsc) {
'''),
        ('''    for (int y = 0; y < 8; ++y)
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
''', '''    // With no vertical frequencies, the column pass gives the same output on every line.
    if (rows == 1u) {
        idct_row(f);
        std::int16_t line[8];
        for (int x = 0; x < 8; ++x) line[x] = static_cast<std::int16_t>(clip9((f[x] + 32) >> 6));
        for (int y = 0; y < 8; ++y, out += stride) std::memcpy(out, line, sizeof line);
        return;
    }
    for (int y = 0; y < 8; ++y)
        if (rows & (1u << y)) idct_row(f + y * 8);
    if (rows & ~3u) {
        for (int x = 0; x < 8; ++x) idct_column<false>(f + x, out + x, stride);
    } else {
        for (int x = 0; x < 8; ++x) idct_column<true>(f + x, out + x, stride);
    }
}

template <bool intra>
void decode_macroblock_type(std::int16_t *raw, bool field, bool dc_reset, std::uint32_t qsc) {
    const unsigned cbp = intra ? 0x3Fu : decode(tables().cbp) & 0xFFu;
    if (dc_reset) g_dc[0] = g_dc[1] = g_dc[2] = static_cast<std::int16_t>(128 << dc_precision());
    if (cbp != 0x3Fu) std::memset(raw, 0, 768);
    const int stride = field ? 32 : 16;
    std::int16_t *lower = raw + (field ? 16 : 128);
    if (cbp & 32u) decode_block<intra>(raw, stride, 0, qsc);
    if (cbp & 16u) decode_block<intra>(raw + 8, stride, 0, qsc);
    if (cbp & 8u) decode_block<intra>(lower, stride, 0, qsc);
    if (cbp & 4u) decode_block<intra>(lower + 8, stride, 0, qsc);
    if (cbp & 2u) decode_block<intra>(raw + 256, 8, 1, qsc);
    if (cbp & 1u) decode_block<intra>(raw + 320, 8, 2, qsc);
}

void decode_macroblock(std::int16_t *raw, bool intra, bool field, bool dc_reset, std::uint32_t qsc) {
    if (intra) decode_macroblock_type<true>(raw, field, dc_reset, qsc);
    else decode_macroblock_type<false>(raw, field, dc_reset, qsc);
}
'''),
        ('''    DctEntry head[1024], tail[1024];
''', '''    DctEntry head[1024], tail[1024];
    DctEntry signed_prefix[256];   // Up to seven code bits followed by the sign.
'''),
        ('''            (v >> 10 ? l.head[v >> 6] : l.tail[v]) = {e.length, e.run, e.level};
        }
    }
''', '''            (v >> 10 ? l.head[v >> 6] : l.tail[v]) = {e.length, e.run, e.level};
        }
        if (e.run != kEscape && e.length < 8) {
            const unsigned suffix = 7u - e.length;
            for (unsigned sign = 0; sign < 2; ++sign) {
                const auto level = static_cast<std::int16_t>(sign ? -e.level : e.level);
                const unsigned first = ((static_cast<unsigned>(e.code) << 1) | sign) << suffix;
                for (unsigned i = 0; i < (1u << suffix); ++i)
                    l.signed_prefix[first + i] = {static_cast<std::uint8_t>(e.length + 1), e.run, level};
            }
        }
    }
'''),
        ('''inline std::uint32_t sat8(int v) {   // v >> 7, to 0..255
    std::uint32_t r;
    __asm__("usat %0, #8, %1, asr #7" : "=r"(r) : "r"(v));
    return r;
}

''', '''inline int clip12(int v) {
    int r;
    __asm__("ssat %0, #12, %1" : "=r"(r) : "r"(v));
    return r;
}

inline std::uint32_t sat8(int v) {   // v >> 7, to 0..255
    std::uint32_t r;
    __asm__("usat %0, #8, %1, asr #7" : "=r"(r) : "r"(v));
    return r;
}

'''),
        ('''        f[r] = x < -2048 ? -2048 : x > 2047 ? 2047 : x;
''', '''        f[r] = clip12(x);
'''),
        ('''        f[0] = x < -2048 ? -2048 : x > 2047 ? 2047 : x;
''', '''        f[0] = clip12(x);
'''),
        ('''        if (have(32)) {   // keep the existing DMA refill threshold
            // A coefficient needs at most 16 code bits and one sign bit. Three bytes suffice at any bit offset.
            const std::uint8_t *p = g_in.data() + (g_bit >> 3);
            const std::uint32_t packed = (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8);
            const std::uint32_t w = packed << (g_bit & 7u);
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
''', '''        if (have(32)) {   // keep the existing DMA refill threshold
            // Two bytes cover the eight-bit lookup at any bit offset.
            const std::uint8_t *p = g_in.data() + (g_bit >> 3);
            const std::uint32_t prefix = (std::uint32_t{p[0]} << 8) | p[1];
            const unsigned shift = g_bit & 7u;
            const DctEntry &short_code = table.signed_prefix[((prefix << shift) >> 8) & 255u];
            if (short_code.length) {
                g_bit += short_code.length;
                run = short_code.run;
                level = short_code.level;
            } else {
                // Longer codes need at most 16 code bits and one sign bit.
                const std::uint32_t w = ((prefix << 16) | (std::uint32_t{p[2]} << 8)) << shift;
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
            }
'''),
        ('''inline int clip9(int v) {   // to -256..255
    int r;
    __asm__("ssat %0, #9, %1" : "=r"(r) : "r"(v));
    return r;
}

''', '''template <unsigned shift>
inline int clip9(int v) {   // shift, then saturate to -256..255
    int result;
    __asm__("ssat %0, #9, %1, asr %2" : "=r"(result) : "r"(v), "I"(shift));
    return result;
}

'''),
        ('''template <bool one_ac>
inline void idct_column(const int *b, std::int16_t *o, int stride) {
    int x1 = one_ac ? 0 : b[32] * 256, x2 = one_ac ? 0 : b[48], x3 = one_ac ? 0 : b[16], x4 = b[8];
    int x5 = one_ac ? 0 : b[56], x6 = one_ac ? 0 : b[40], x7 = one_ac ? 0 : b[24];
''', '''template <unsigned row_limit>
inline void idct_column(const int *b, std::int16_t *o, int stride) {
    int x1 = row_limit > 4 ? b[32] * 256 : 0, x2 = row_limit > 6 ? b[48] : 0, x3 = row_limit > 2 ? b[16] : 0, x4 = b[8];
    int x5 = row_limit > 7 ? b[56] : 0, x6 = row_limit > 5 ? b[40] : 0, x7 = row_limit > 3 ? b[24] : 0;
'''),
        ('''    o[0] = static_cast<std::int16_t>(clip9((x7 + x1) >> 14));
    o[stride] = static_cast<std::int16_t>(clip9((x3 + x2) >> 14));
    o[2 * stride] = static_cast<std::int16_t>(clip9((x0 + x4) >> 14));
    o[3 * stride] = static_cast<std::int16_t>(clip9((x8 + x6) >> 14));
    o[4 * stride] = static_cast<std::int16_t>(clip9((x8 - x6) >> 14));
    o[5 * stride] = static_cast<std::int16_t>(clip9((x0 - x4) >> 14));
    o[6 * stride] = static_cast<std::int16_t>(clip9((x3 - x2) >> 14));
    o[7 * stride] = static_cast<std::int16_t>(clip9((x7 - x1) >> 14));
''', '''    o[0] = static_cast<std::int16_t>(clip9<14>(x7 + x1));
    o[stride] = static_cast<std::int16_t>(clip9<14>(x3 + x2));
    o[2 * stride] = static_cast<std::int16_t>(clip9<14>(x0 + x4));
    o[3 * stride] = static_cast<std::int16_t>(clip9<14>(x8 + x6));
    o[4 * stride] = static_cast<std::int16_t>(clip9<14>(x8 - x6));
    o[5 * stride] = static_cast<std::int16_t>(clip9<14>(x0 - x4));
    o[6 * stride] = static_cast<std::int16_t>(clip9<14>(x3 - x2));
    o[7 * stride] = static_cast<std::int16_t>(clip9<14>(x7 - x1));
'''),
        ('''    if (!ac) {
        const std::int16_t v = static_cast<std::int16_t>(clip9((f[0] + 4) >> 3));
        for (int y = 0; y < 8; ++y, out += stride)
            for (int x = 0; x < 8; ++x) out[x] = v;
        return;
    }
    // With no vertical frequencies, the column pass gives the same output on every line.
    if (rows == 1u) {
        idct_row(f);
        std::int16_t line[8];
        for (int x = 0; x < 8; ++x) line[x] = static_cast<std::int16_t>(clip9((f[x] + 32) >> 6));
        for (int y = 0; y < 8; ++y, out += stride) std::memcpy(out, line, sizeof line);
        return;
    }
    for (int y = 0; y < 8; ++y)
        if (rows & (1u << y)) idct_row(f + y * 8);
    if (rows & ~3u) {
        for (int x = 0; x < 8; ++x) idct_column<false>(f + x, out + x, stride);
    } else {
        for (int x = 0; x < 8; ++x) idct_column<true>(f + x, out + x, stride);
    }''', '''    if (!ac) {
        const std::int16_t v = static_cast<std::int16_t>(clip9<3>(f[0] + 4));
        for (int y = 0; y < 8; ++y, out += stride)
            for (int x = 0; x < 8; ++x) out[x] = v;
        return;
    }
    // With no vertical frequencies, the column pass gives the same output on every line.
    if (rows == 1u) {
        idct_row(f);
        std::int16_t line[8];
        for (int x = 0; x < 8; ++x) line[x] = static_cast<std::int16_t>(clip9<6>(f[x] + 32));
        for (int y = 0; y < 8; ++y, out += stride) std::memcpy(out, line, sizeof line);
        return;
    }
    for (int y = 0; y < 8; ++y)
        if (rows & (1u << y)) idct_row(f + y * 8);
    // Select a column transform that omits the absent high vertical frequencies.
    if (!(rows & ~3u)) {
        for (int x = 0; x < 8; ++x) idct_column<2>(f + x, out + x, stride);
    } else if (!(rows & ~7u)) {
        for (int x = 0; x < 8; ++x) idct_column<3>(f + x, out + x, stride);
    } else if (!(rows & ~15u)) {
        for (int x = 0; x < 8; ++x) idct_column<4>(f + x, out + x, stride);
    } else if (!(rows & ~63u)) {
        for (int x = 0; x < 8; ++x) idct_column<6>(f + x, out + x, stride);
    } else {
        for (int x = 0; x < 8; ++x) idct_column<8>(f + x, out + x, stride);
    }'''),
        ('''void convert_fast(const std::uint8_t *raw) {   // 32-bit pixels, no alpha thresholds
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
}''', '''struct CscTables {
    std::uint16_t luma[256];
    std::uint32_t cb[256], cr[256];
};

constexpr std::uint32_t csc_pair(int low, int high) {
    return static_cast<std::uint16_t>(low) | (static_cast<std::uint32_t>(static_cast<std::uint16_t>(high)) << 16);
}

constexpr auto kCscTables = [] {
    CscTables t{};
    for (int i = 0; i < 256; ++i) {
        const int chroma = i - 128;
        t.luma[i] = i > 16 ? 149 * (i - 16) : 0;
        // The blue offset can exceed a signed halfword. Store half the offset.
        t.cb[i] = csc_pair(129 * chroma + 32, -50 * chroma);
        t.cr[i] = csc_pair(204 * chroma + 64, -104 * chroma + 64);
    }
    return t;
}();

void convert_fast(const std::uint8_t *raw) {   // 32-bit pixels, no alpha thresholds
    std::uint32_t pixels[256];
    const std::uint8_t *cb = raw + 256, *cr = raw + 320;
    const auto pixel = [](std::uint32_t y, int r, int g, int b) {
        const int lum = kCscTables.luma[y];
        return sat8(lum + r) | (sat8(lum + g) << 8) | (sat8(lum + b) << 16) | 0x80000000u;
    };
    for (int row = 0; row < 8; ++row) {
        std::uint32_t *top = pixels + row * 32, *bottom = top + 16;
        const std::uint8_t *y0 = raw + row * 32, *y1 = y0 + 16;
        for (int x = 0; x < 8; ++x, top += 2, bottom += 2, y0 += 2, y1 += 2) {
            const std::uint32_t u = kCscTables.cb[*cb++], v = kCscTables.cr[*cr++];
            const int r = static_cast<std::int16_t>(v), b = 2 * static_cast<std::int16_t>(u);
            const int g = (static_cast<std::int32_t>(u) >> 16) + (static_cast<std::int32_t>(v) >> 16);
            top[0] = pixel(y0[0], r, g, b);
            top[1] = pixel(y0[1], r, g, b);
            bottom[0] = pixel(y1[0], r, g, b);
            bottom[1] = pixel(y1[1], r, g, b);
        }
    }
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(pixels);
    g_out.insert(g_out.end(), bytes, bytes + sizeof pixels);
}'''),
        ('''#ifdef __3DS__
    if (byte + 8 <= g_in.size()) {
''', '''#ifdef __3DS__
    // A read of up to 32 bits spans at most five bytes at any bit offset.
    if (byte + 5 <= g_in.size() && n <= 32) {
        const auto *p = g_in.data() + byte;
        const unsigned offset = bit & 7u;
        std::uint32_t word;
        std::memcpy(&word, p, 4);
        const std::uint32_t shifted = (__builtin_bswap32(word) << offset) | (std::uint32_t{p[4]} >> (8 - offset));
        return shifted >> (32 - n);
    }
    if (byte + 8 <= g_in.size()) {
'''),
        ('''    g_in.erase(g_in.begin(), g_in.begin() + static_cast<std::ptrdiff_t>(drop));
''', '''    if (!drop) return;
    g_in.erase(g_in.begin(), g_in.begin() + static_cast<std::ptrdiff_t>(drop));
'''),
        ('''    case kVdec: {
        const bool ok = atomic([&] {
            skip(cmd & 0x3Fu);
            const Lookup *table = nullptr;
            switch ((cmd >> 26) & 3u) {
            case 0: table = &tables().mbai; break;
            case 1:
                switch (picture_type()) {
                case 1: table = &tables().mb_i; break;
                case 2: table = &tables().mb_p; break;
                case 3: table = &tables().mb_b; break;
                default: throw BadCode{};
                }
                break;
            case 2: table = &tables().motion; break;
            default: table = &tables().dmv; break;
            }
            g_result = decode(*table);
        });
        if (ok) commit();
        return ok;
    }
    case kFdec: {
        const bool ok = atomic([&] {
            skip(cmd & 0x3Fu);
            g_result = peek(32);
        });
        if (ok) commit();
        return ok;
    }
''', '''    case kVdec: {
        const auto select_table = [&]() -> const Lookup * {
            switch ((cmd >> 26) & 3u) {
            case 0: return &tables().mbai;
            case 1:
                switch (picture_type()) {
                case 1: return &tables().mb_i;
                case 2: return &tables().mb_p;
                case 3: return &tables().mb_b;
                default: return nullptr;
                }
            case 2: return &tables().motion;
            default: return &tables().dmv;
            }
        };
#ifdef __3DS__
        // Buffered commands need no DMA refill or rollback.
        const std::size_t at = g_bit + (cmd & 0x3Fu);
        const Lookup *cached = select_table();
        if (cached && cached->bits <= 25 && (at >> 3) + 4 <= g_in.size()) {
            std::uint32_t word;
            std::memcpy(&word, g_in.data() + (at >> 3), 4);
            const auto code = (__builtin_bswap32(word) << (at & 7u)) >> (32 - cached->bits);
            const Entry &entry = cached->entries[code];
            if (entry.length) {
                g_result = entry.value;
                g_bit = at + entry.length;
                commit();
                return true;
            }
        }
#endif
        const bool ok = atomic([&] {
            skip(cmd & 0x3Fu);
            const Lookup *table = select_table();
            if (!table) throw BadCode{};
            g_result = decode(*table);
        });
        if (ok) commit();
        return ok;
    }
    case kFdec: {
#ifdef __3DS__
        const std::size_t at = g_bit + (cmd & 0x3Fu);
        if (g_in.size() * 8 >= at + 32) {
            g_result = bits_at(at, 32);
            g_bit = at;
            commit();
            return true;
        }
#endif
        const bool ok = atomic([&] {
            skip(cmd & 0x3Fu);
            g_result = peek(32);
        });
        if (ok) commit();
        return ok;
    }
'''),
    ],
    # The I/O processor code tools/p3fes_iopgen.py translated. Build with PS2_IOP_BLOCKS if it is there, which the Makefile sees to;
    # blocks are checked against what is in memory whenever that changes, so anything else stays with the interpreter.
    'runtime/src/iop/iop.hpp': [
        ('void set_trace(bool modules, bool hle);\n', '''void set_trace(bool modules, bool hle);

#ifdef PS2_IOP_BLOCKS
inline constexpr std::int32_t kNoBlock = INT32_MIN;
void blocks_refresh();
std::int32_t blocks_run(Cpu &c, std::int32_t left);
#else
inline void blocks_refresh() {}
#endif
'''),
    ],
    'runtime/src/iop/cpu.cpp': [
        ('''    while (left-- > 0) {
        const std::uint32_t pc = c.pc;
''', '''    while (left-- > 0) {
#ifdef PS2_IOP_BLOCKS
        if (const std::int32_t rest = blocks_run(c, left + 1); rest != kNoBlock) {
            left = rest;
            if (kernel::switch_pending()) return;
            continue;
        }
#endif
        const std::uint32_t pc = c.pc;
#ifdef PS2_IOP_RECORD
        record(pc);
#endif
'''),
        ('void execute(Cpu &c, std::int64_t &budget) {\n', '''#ifdef PS2_IOP_RECORD
// One bit per word that ran, and the memory it ran from, which is what tools/p3fes_iopgen.py works from.
void record(std::uint32_t pc) {
    static std::uint8_t seen[kRamSize / 32];
    static std::uint32_t count;
    const std::uint32_t word = (pc & (kRamSize - 1)) >> 2;
    seen[word >> 3] |= static_cast<std::uint8_t>(1u << (word & 7u));
    if (++count >> 25) {
        count = 0;
        if (std::FILE *f = std::fopen("sdmc:/p3fes/iopseen.bin", "wb")) { std::fwrite(seen, 1, sizeof seen, f); std::fclose(f); }
        if (std::FILE *f = std::fopen("sdmc:/p3fes/iopram.bin", "wb")) { std::fwrite(g_ram, 1, kRamSize, f); std::fclose(f); }
    }
}
#endif

void execute(Cpu &c, std::int64_t &budget) {
'''),
    ],
    'runtime/src/iop/kernel.cpp': [
        ('    link_imports(base, total);\n\n    const int id = g_next_module++;\n', '    link_imports(base, total);\n    blocks_refresh();\n\n    const int id = g_next_module++;\n'),
        ('    hw::reset();\n    kernel::boot();\n', '    hw::reset();\n    kernel::boot();\n    blocks_refresh();\n'),
        ('    s.bytes(g_ram, kRamSize);\n    kernel::state(s);\n', '    s.bytes(g_ram, kRamSize);\n    blocks_refresh();\n    kernel::state(s);\n'),
    ],
    'runtime/include/ps2/system.hpp': [
        ('''void sync_iop();
''', '''void sync_iop();
void iop_vblank(bool start);
#ifdef __3DS__
// ps2run_3ds_patch.py: the I/O processor on its own core, in port3ds/iop3ds.cpp.
bool start_iop_thread(int priority, int core);
void stop_iop_thread();
bool iop_threaded();
void iop_lock();
void iop_unlock();
void iop_advance(std::uint64_t ee_cycles);
void iop_edge(bool start);
void iop_wait(bool (*enough)() = nullptr);   // until it is level with the game, or enough() says so
struct IopGuard {
    IopGuard() { iop_lock(); }
    ~IopGuard() { iop_unlock(); }
};
#else
inline void iop_wait(bool (*)() = nullptr) {}
struct IopGuard {};
#endif
'''),
    ],
    'runtime/src/system.cpp': [
        ('''void idle(Cpu &c) {
    std::uint64_t next = next_event_time();
''', '''#ifdef __3DS__
std::uint64_t g_iop_tight = 0;   // until this time, a game with nothing to do waits for the I/O processor: it may be expecting a reply
#endif

void idle(Cpu &c) {
#ifdef __3DS__
    if (now() < g_iop_tight) iop_wait(sif::reply_ready);
#endif
    std::uint64_t next = next_event_time();
'''),
        ('''    iop::sio2::flush_cards();
''', '''    {
        IopGuard guard;
        iop::sio2::flush_cards();
    }
'''),
        ('''    iop::vblank(true);
    schedule(hw::kEventVblankEnd + 100, now() + hblank_cycles() * 22u, [] { iop::vblank(false); });
''', '''    iop_vblank(true);
    schedule(hw::kEventVblankEnd + 100, now() + hblank_cycles() * 22u, [] { iop_vblank(false); });
'''),
        ('''    if (id == hw::kEventVblankEnd + 100) return [] { iop::vblank(false); };
''', '''    if (id == hw::kEventVblankEnd + 100) return [] { iop_vblank(false); };
'''),
        ('''    iop::state(s);
''', '''    iop_wait();
    iop::state(s);
'''),
        ('''void sync_iop() {
    const std::uint64_t t = now();
    if (t > g_iop_synced) {
''', '''void sync_iop() {
    const std::uint64_t t = now();
#ifdef __3DS__
    if (iop_threaded()) {
        if (t > g_iop_synced && t - g_iop_synced >= 100000) {
            iop_advance(t - g_iop_synced);
            g_iop_synced = t;
        }
        if (sif::pump_wanted()) {
            IopGuard guard;
            sif::pump();
        }
        return;
    }
#endif
    if (t > g_iop_synced) {
'''),
        ('''void request_iop_sync() {
    if (g_cpu.budget > 64) set_budget(g_cpu, 64);
}
''', '''void iop_vblank(bool start) {
#ifdef __3DS__
    if (iop_threaded()) {
        const std::uint64_t t = now();
        if (t > g_iop_synced) {
            iop_advance(t - g_iop_synced);
            g_iop_synced = t;
        }
        iop_edge(start);
        return;
    }
#endif
    iop::vblank(start);
}

void request_iop_sync() {
#ifdef __3DS__
    g_iop_tight = now() + 300000;
#endif
    if (g_cpu.budget > 64) set_budget(g_cpu, 64);
}
'''),
        ('''            case gs::queue::kVif1: vif::transfer(1, payload, h.arg0); break;
''', '''            case gs::queue::kVif1: vif::transfer(1, payload, h.arg0); break;
#ifdef PS2_PICA
            case gs::queue::kVif1Records: vif::transfer_records(payload, h.bytes); break;
            case gs::queue::kGifAddress: {
                const std::uint8_t *data;
                std::memcpy(&data, payload, sizeof data);
                gif::vu_path3(data, h.arg1);
                break;
            }
#endif
'''),
    ],
    'runtime/src/iop/sif.hpp': [
        ('''void pump();
void dump();
''', '''void pump();
bool pump_wanted();
bool reply_ready();
void dump();
'''),
    ],
    'runtime/src/iop/sif.cpp': [
        ('''std::uint64_t g_ee_dma_done = 0;
''', '''std::uint64_t g_ee_dma_done = 0;
// ps2run_3ds_patch.py: with the I/O processor on its own core only the game side raises interrupts, so it delivers the packets.
// These say that it is worth taking the lock to look.
std::atomic<bool> g_sif_wake{false}, g_sif_binds{false};
'''),
        ('''#include "ps2/state.hpp"
''', '''#include "ps2/state.hpp"

#include <atomic>
'''),
        ('''    g_outgoing.emplace_back(bytes, bytes + size);
    pump();
}
''', '''    g_outgoing.emplace_back(bytes, bytes + size);
#ifdef __3DS__
    if (iop_threaded()) {
        g_sif_wake.store(true);
        return;
    }
#endif
    pump();
}
'''),
        ('''    g_binds[server] = PendingBind{r, now() + kEeClock * 4};
''', '''    g_binds[server] = PendingBind{r, now() + kEeClock * 4};
    g_sif_binds.store(true);
'''),
        ('''std::uint32_t get_reg(std::uint32_t reg) {
    switch (reg) {
''', '''std::uint32_t get_reg(std::uint32_t reg) {
    IopGuard guard;
    switch (reg) {
'''),
        ('''void set_reg(std::uint32_t reg, std::uint32_t value) {
    if (reg == 1) g_mainaddr = value;
''', '''void set_reg(std::uint32_t reg, std::uint32_t value) {
    IopGuard guard;
    if (reg == 1) g_mainaddr = value;
'''),
        ('''std::uint32_t set_dma(std::uint32_t transfers, std::uint32_t count) {
    if (!count) count = 1;
''', '''std::uint32_t set_dma(std::uint32_t transfers, std::uint32_t count) {
    IopGuard guard;
    g_sif_wake.store(true);
    if (!count) count = 1;
'''),
        ('''void packet_processed() {
    g_processed = true;
    pump();
}

void pump() {
    const std::uint64_t t = now();
''', '''void packet_processed() {
    IopGuard guard;
    g_processed = true;
    pump();
}

bool pump_wanted() { return g_sif_wake.load() || g_sif_binds.load(); }
bool reply_ready() { return g_sif_wake.load(); }   // a packet for the game

void pump() {
    g_sif_wake.store(false);
    g_sif_binds.store(!g_binds.empty());
    const std::uint64_t t = now();
'''),
    ],
    'runtime/src/gs/gs.cpp': [
        ('''void packed_run(const std::uint8_t *kinds, std::uint32_t nreg, std::uint32_t loops, const std::uint8_t *data) {
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
''', '''using PackedWord = std::uint32_t __attribute__((may_alias, aligned(4)));

static inline void packed_stq(State &s, const PackedWord *q) {
    s.st = q[0] | (static_cast<std::uint64_t>(q[1]) << 32);
    std::memcpy(&s.q, q + 2, 4);
}

static inline void packed_rgbaq(State &s, const PackedWord *q, std::uint32_t qbits) {
    s.rgbaq = (q[0] & 0xFFu) | ((q[1] & 0xFFu) << 8) | ((q[2] & 0xFFu) << 16) | ((q[3] & 0xFFu) << 24) | (static_cast<std::uint64_t>(qbits) << 32);
}

static inline void packed_xyzf(const PackedWord *q) {
    vertex(q[0] & 0xFFFFu, q[1] & 0xFFFFu, (q[2] >> 4) & 0xFFFFFFu, static_cast<std::uint8_t>(q[3] >> 4), !((q[3] >> 15) & 1u));
}

void packed_run(const std::uint8_t *kinds, std::uint32_t nreg, std::uint32_t loops, const std::uint8_t *data) {
    State &s = g_state;
    const PackedWord *q = reinterpret_cast<const PackedWord *>(data);
    // The game's 3D draws send STQ, RGBA and XYZF for each vertex.
    if (nreg == 3 && kinds[0] == 2 && kinds[1] == 1 && kinds[2] == 4) {
        for (; loops; --loops, q += 12) {
            packed_stq(s, q);
            packed_rgbaq(s, q + 4, q[2]);
            packed_xyzf(q + 8);
        }
        return;
    }
    for (; loops; --loops) {
        for (std::uint32_t r = 0; r < nreg; ++r, q += 4) {
            switch (kinds[r]) {
            case 1: {
                std::uint32_t qbits;
                std::memcpy(&qbits, &s.q, 4);
                packed_rgbaq(s, q, qbits);
                break;
            }
            case 2: packed_stq(s, q); break;
            case 3: s.uv = (q[0] & 0x3FFFu) | ((q[1] & 0x3FFFu) << 16); break;
            case 4: packed_xyzf(q); break;
            default: vertex(q[0] & 0xFFFFu, q[1] & 0xFFFFu, q[2], static_cast<std::uint8_t>(s.fog >> 56), !((q[3] >> 15) & 1u)); break;
            }
        }
    }
}
'''),
        ('''std::uint32_t g_setup_generation = 0;
''', '''std::uint32_t g_setup_generation = 0;
#ifdef PS2_PICA
// Render-thread-only sequence, intentionally outside saved GS state.
std::uint32_t g_vertex_serial = 0;
#endif
'''),
        ('''    pica::draw(s, s.queue, needed);
    uc.clamp = saved_clamp;
''', '''    pica::draw(s, s.queue, needed, g_vertex_serial);
    uc.clamp = saved_clamp;
'''),
        ('''        if (pica::active()) pica::draw(s, s.queue, needed);
''', '''        if (pica::active()) pica::draw(s, s.queue, needed, g_vertex_serial);
'''),
        ('''void vertex(std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint8_t f, bool draw) {
    State &s = g_state;
''', '''void vertex(std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint8_t f, bool draw) {
#ifdef PS2_PICA
    ++g_vertex_serial;
#endif
    State &s = g_state;
'''),
        ('''void draw_gpu(State &s, int needed) {
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
    pica::draw(s, s.queue, needed, g_vertex_serial);
    uc.clamp = saved_clamp;
''', '''void draw_gpu(const State &s) {
    if (queue::g_skip_picture) return;
    static bool known = false;
    static std::uint32_t seen = 0;
    static std::uint64_t clamp = 0;
    if (!known || seen != g_setup_generation) {
        known = true;
        seen = g_setup_generation;
        const Context &uc = s.ctx[static_cast<unsigned>(s.prim >> 9) & 1u];
        g_upload_ext.erase((static_cast<std::uint32_t>(uc.frame) & 0x1FFu) * 32u);
        clamp = upload_clamped(s, uc);
    }
    pica::draw(s, g_vertex_serial, clamp);
'''),
        ('''        draw_gpu(s, needed);
''', '''        draw_gpu(s);
'''),
        ('''#ifdef PS2_PICA
        if (pica::active()) pica::draw(s, s.queue, needed, g_vertex_serial);
        else
#endif
        if (cpu_raster()) raster::draw(s, s.queue, needed);
''', '''        // PICA draws before this fallback, with its clamp passed separately.
        if (cpu_raster()) raster::draw(s, s.queue, needed);
'''),
    ],
    'runtime/include/ps2/vu_ops.hpp': [
        ('''PS2_VU_INLINE std::uint8_t *qword(std::uint8_t *mem, std::uint32_t index) { return mem + (index & 1023u) * 16u; }
''', '''PS2_VU_INLINE std::uint8_t *qword(std::uint8_t *mem, std::uint32_t index) {
#if defined(__3DS__) && defined(PS2_VU_FAST)
    // The compiled VU1 programs are called with the aligned VU1 data RAM.
    mem = static_cast<std::uint8_t *>(__builtin_assume_aligned(mem, 16));
#endif
    return mem + (index & 1023u) * 16u;
}
'''),
        ('''    std::uint32_t mac = 0;
    for (int i = 0; i < 4; ++i) {
        if (!(dest & (8u >> i))) continue;
        const std::uint32_t bit = 3u - static_cast<std::uint32_t>(i);
        const std::uint32_t x = v.w[i];
        if (!(x & 0x7F800000u)) mac |= 1u << bit;
        if (x & 0x80000000u) mac |= 1u << (bit + 4);
        if ((x & 0x7FFFFFFFu) == 0x7FFFFFFFu) mac |= 1u << (bit + 12);
    }
    r.mac = mac;
''', '''    std::uint32_t mac = 0;
    const auto field = [&](int i) __attribute__((always_inline)) {
        if (!(dest & (8u >> i))) return;
        const std::uint32_t bit = 3u - static_cast<std::uint32_t>(i);
        const std::uint32_t x = v.w[i];
        mac |= static_cast<std::uint32_t>(!(x & 0x7F800000u)) << bit;
        mac |= (x >> 31) << (bit + 4);
        mac |= static_cast<std::uint32_t>((x & 0x7FFFFFFFu) == 0x7FFFFFFFu) << (bit + 12);
    };
    field(0);
    field(1);
    field(2);
    field(3);
    r.mac = mac;
'''),
    ],
    'runtime/src/gs/queue.hpp': [
        ('''    std::uint64_t capacity_ = 0;
    alignas(64) std::atomic<std::uint64_t> head_{0};
    alignas(64) std::atomic<std::uint64_t> tail_{0};
''', '''    std::uint32_t capacity_ = 0;
    // Each cursor is an offset in [0, 2 * capacity). The extra lap distinguishes full from empty.
    alignas(64) std::atomic<std::uint32_t> head_{0};
    alignas(64) std::atomic<std::uint32_t> tail_{0};
'''),
        ('kPath3Flush, kPath3ChainEnd, kVuStop };', 'kPath3Flush, kPath3ChainEnd, kVuStop, kVif1Records, kGifAddress };'),
        ('''    void post(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes);
    bool peek(''', '''    void post(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes);
    // A message built a little at a time, in place, by the one thread that posts. It goes when full or when anything else is posted.
    static constexpr std::uint32_t kOpenBytes = 16u << 10;
    std::uint8_t *extend(Type type, std::uint32_t bytes);
    void close();
    bool peek('''),
        ('''private:
    std::unique_ptr<std::uint8_t[]> buffer_;
''', '''private:
    std::uint8_t *room(std::uint32_t need);
    void publish(std::uint32_t need);
    std::uint8_t *open_at_ = nullptr;
    std::uint32_t open_bytes_ = 0, next_ = 0;
    Type open_type_ = kWrap;
    std::unique_ptr<std::uint8_t[]> buffer_;
'''),
    ],
    'runtime/src/gs/queue.cpp': [
        ('''constexpr std::uint64_t align16(std::uint64_t n) { return (n + 15) & ~15ull; }
''', '''constexpr std::uint32_t align16(std::uint32_t n) { return (n + 15) & ~15u; }

std::uint32_t wrap_offset(std::uint32_t offset, std::uint32_t length) {
    return offset >= length ? offset - length : offset;
}
'''),
        ('''    buffer_ = std::make_unique<std::uint8_t[]>(capacity);
    capacity_ = capacity;
''', '''    if (capacity < sizeof(Header) || (capacity & 15) || capacity > UINT32_MAX / 4)
        throw std::invalid_argument("invalid GS queue capacity");
    buffer_ = std::make_unique<std::uint8_t[]>(capacity);
    capacity_ = static_cast<std::uint32_t>(capacity);
'''),
        ('''#include <cstring>
''', '''#include <cstring>
#include <stdexcept>
'''),
        ('''    const std::uint64_t need = sizeof(Header) + align16(bytes);
    std::uint64_t head = head_.load(std::memory_order_relaxed);
    for (;;) {
        const std::uint64_t pos = head % capacity_;
        const std::uint64_t pad = pos + need > capacity_ ? capacity_ - pos : 0;
''', '''    if (capacity_ == 0 || bytes > capacity_ - sizeof(Header))
        throw std::length_error("message too large for GS queue");

    const std::uint32_t need = sizeof(Header) + align16(bytes);
    const std::uint32_t span = capacity_ * 2;
    const std::uint32_t head = head_.load(std::memory_order_relaxed);
    const std::uint32_t pos = wrap_offset(head, capacity_);
    const std::uint32_t pad = pos + need > capacity_ ? capacity_ - pos : 0;
    for (;;) {
'''),
        ('''        if (head + pad + need - tail_.load(std::memory_order_acquire) <= capacity_) {
            if (pad) {
                const Header wrap{kWrap, 0, 0, 0};
                std::memcpy(buffer_.get() + pos, &wrap, sizeof wrap);
                head += pad;
            }
            break;
        }
''', '''        const std::uint32_t tail = tail_.load(std::memory_order_acquire);
        const std::uint32_t used = wrap_offset(head + span - tail, span);
        if (used + pad + need <= capacity_) break;

        // A message close to capacity may need more space than an entire empty lap
        // if its padding is included. Publish the wrap separately once it fits.
        if (pad + need > capacity_ && used + pad <= capacity_) {
            const Header wrap{kWrap, 0, 0, 0};
            std::memcpy(buffer_.get() + pos, &wrap, sizeof wrap);
            head_.store(wrap_offset(head + pad, span), std::memory_order_release);
#ifndef __3DS__
            posted_.fetch_add(1);
            if (consumer_waiting_.exchange(0)) notify(posted_);
#endif
            post(type, arg0, arg1, data, bytes);
            return;
        }
'''),
        ('''        if (head + pad + need - tail_.load() > capacity_) wait(freed_, seen);
''', '''        const std::uint32_t current_tail = tail_.load(std::memory_order_acquire);
        const std::uint32_t current_used = wrap_offset(head + span - current_tail, span);
        if (current_used + pad + (pad + need <= capacity_ ? need : 0) > capacity_) wait(freed_, seen);
'''),
        ('''    std::uint8_t *at = buffer_.get() + head % capacity_;
''', '''    if (pad) {
        const Header wrap{kWrap, 0, 0, 0};
        std::memcpy(buffer_.get() + pos, &wrap, sizeof wrap);
    }
    std::uint8_t *at = buffer_.get() + (pad ? 0 : pos);
'''),
        ('''    head_.store(head + need, std::memory_order_release);
''', '''    head_.store(wrap_offset(head + pad + need, span), std::memory_order_release);
'''),
        ('''    std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    while (tail != head) {
        const std::uint64_t pos = tail % capacity_;
''', '''    std::uint32_t tail = tail_.load(std::memory_order_relaxed);
    const std::uint32_t head = head_.load(std::memory_order_acquire);
    while (tail != head) {
        const std::uint32_t pos = wrap_offset(tail, capacity_);
'''),
        ('''        tail += capacity_ - pos;
        tail_.store(tail, std::memory_order_release);
''', '''        tail = wrap_offset(tail + capacity_ - pos, capacity_ * 2);
        tail_.store(tail, std::memory_order_release);
#ifndef __3DS__
        freed_.fetch_add(1);
        if (producer_waiting_.exchange(0)) notify(freed_);
#endif
'''),
        ('''    tail_.store(tail_.load(std::memory_order_relaxed) + sizeof(Header) + align16(header.bytes), std::memory_order_release);
''', '''    const std::uint32_t next = tail_.load(std::memory_order_relaxed) + sizeof(Header) + align16(header.bytes);
    tail_.store(wrap_offset(next, capacity_ * 2), std::memory_order_release);
'''),
        # room() is the first half of what post() was, and publish() the end of it.
        ('''void Ring::post(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes) {
    if (capacity_ == 0 || bytes > capacity_ - sizeof(Header))
        throw std::length_error("message too large for GS queue");

    const std::uint32_t need = sizeof(Header) + align16(bytes);
    const std::uint32_t span = capacity_ * 2;
''', '''// Waits until a message of this many bytes fits, and says where it goes. It starts at next_ in cursor terms.
std::uint8_t *Ring::room(std::uint32_t need) {
    const std::uint32_t span = capacity_ * 2;
'''),
        ('''            post(type, arg0, arg1, data, bytes);
            return;
''', '''            return room(need);
'''),
        ('''    std::uint8_t *at = buffer_.get() + (pad ? 0 : pos);
    const Header h{type, bytes, arg0, arg1};
    std::memcpy(at, &h, sizeof h);
    if (bytes) std::memcpy(at + sizeof h, data, bytes);
    head_.store(wrap_offset(head + pad + need, span), std::memory_order_release);
''', '''    next_ = wrap_offset(head + pad, span);
    return buffer_.get() + (pad ? 0 : pos);
}

void Ring::publish(std::uint32_t need) {
    head_.store(wrap_offset(next_ + need, capacity_ * 2), std::memory_order_release);
'''),
        ('''bool Ring::peek(Header &header, const std::uint8_t *&payload) {
''', '''void Ring::post(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes) {
    if (capacity_ == 0 || bytes > capacity_ - sizeof(Header))
        throw std::length_error("message too large for GS queue");
    close();
    const std::uint32_t need = sizeof(Header) + align16(bytes);
    std::uint8_t *at = room(need);
    const Header h{type, bytes, arg0, arg1};
    std::memcpy(at, &h, sizeof h);
    if (bytes) std::memcpy(at + sizeof h, data, bytes);
    publish(need);
}

std::uint8_t *Ring::extend(Type type, std::uint32_t bytes) {
    if (open_bytes_ + bytes > kOpenBytes) close();
    if (!open_bytes_) {
        open_at_ = room(sizeof(Header) + kOpenBytes);
        open_type_ = type;
    }
    std::uint8_t *at = open_at_ + sizeof(Header) + open_bytes_;
    open_bytes_ += bytes;
    return at;
}

void Ring::close() {
    if (!open_bytes_) return;
    const Header h{open_type_, open_bytes_, 0, 0};
    std::memcpy(open_at_, &h, sizeof h);
    publish(sizeof(Header) + align16(open_bytes_));
    open_bytes_ = 0;
}

bool Ring::peek(Header &header, const std::uint8_t *&payload) {
'''),
    ],
    'runtime/src/vif.cpp': [
        ('''template <std::uint32_t Bits, std::uint32_t Components, bool Usn>
void unpack_fast(std::uint8_t *mem, std::uint32_t mask, std::uint32_t addr, std::uint32_t wl, std::uint32_t skip, const std::uint8_t *bytes, std::uint32_t num) {
    std::uint32_t cycle = 0;
    const bool keep_zw = v2_keep(), zero_zw = v2_zero();
    for (std::uint32_t n = 0; n < num; ++n) {
        std::uint32_t value[4];
        for (std::uint32_t j = 0; j < Components; ++j) {
            const std::size_t at = static_cast<std::size_t>(n) * Components + j;
            if constexpr (Bits == 32) {
                std::memcpy(&value[j], bytes + at * 4, 4);
            } else if constexpr (Bits == 16) {
                std::uint16_t h;
                std::memcpy(&h, bytes + at * 2, 2);
                value[j] = Usn ? h : static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(h)));
            } else {
                value[j] = Usn ? bytes[at] : static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(bytes[at])));
            }
        }
        if constexpr (Components == 1) {
            value[1] = value[2] = value[3] = value[0];
        } else if constexpr (Components == 2) {
            if (keep_zw) {
                std::uint32_t old[4];
                std::memcpy(old, mem + (addr & mask) * 16, 16);
                value[2] = old[2];
                value[3] = old[3];
            } else if (zero_zw) {
                value[2] = value[3] = 0;
            } else {
                value[2] = value[0];
                value[3] = value[1];
            }
        } else if constexpr (Components == 3) {
            value[3] = 0;
        }
        std::memcpy(mem + (addr & mask) * 16, value, 16);
        ++addr;
        if (++cycle >= wl) {
            addr += skip;
            cycle = 0;
        }
    }
}

''', '''template <std::uint32_t Bits, std::uint32_t Components, bool Usn>
__attribute__((always_inline)) inline void unpack_vector(std::uint8_t *dst, const std::uint8_t *src, bool keep_zw, bool zero_zw) {
    std::uint32_t value[4];
    for (std::uint32_t j = 0; j < Components; ++j) {
        if constexpr (Bits == 32) {
            std::memcpy(&value[j], src + j * 4, 4);
        } else if constexpr (Bits == 16) {
            std::uint16_t h;
            std::memcpy(&h, src + j * 2, 2);
            value[j] = Usn ? h : static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(h)));
        } else {
            value[j] = Usn ? src[j] : static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(src[j])));
        }
    }
    if constexpr (Components == 1) {
        value[1] = value[2] = value[3] = value[0];
    } else if constexpr (Components == 2) {
        if (keep_zw) {
            std::uint32_t old[4];
            std::memcpy(old, dst, 16);
            value[2] = old[2];
            value[3] = old[3];
        } else if (zero_zw) {
            value[2] = value[3] = 0;
        } else {
            value[2] = value[0];
            value[3] = value[1];
        }
    } else if constexpr (Components == 3) {
        value[3] = 0;
    }
    std::memcpy(dst, value, 16);
}

template <std::uint32_t Bits, std::uint32_t Components, bool Usn>
void unpack_fast(std::uint8_t *mem, std::uint32_t mask, std::uint32_t addr, std::uint32_t wl, std::uint32_t skip, const std::uint8_t *bytes, std::uint32_t num) {
    std::uint32_t cycle = 0;
    const bool keep_zw = v2_keep(), zero_zw = v2_zero();
#ifdef PS2_PICA
    // A regular stride inside VU memory needs neither cycle tracking nor address wrapping.
    if (wl <= 1 || skip == 0) {
        const std::uint32_t step = 1 + skip;
        addr &= mask;
        if ((num - 1) * step <= mask - addr) {
            for (std::uint32_t n = 0; n < num; ++n, addr += step, bytes += Components * (Bits / 8))
                unpack_vector<Bits, Components, Usn>(mem + addr * 16, bytes, keep_zw, zero_zw);
            return;
        }
    }
#endif
    for (std::uint32_t n = 0; n < num; ++n) {
        unpack_vector<Bits, Components, Usn>(mem + (addr & mask) * 16, bytes + n * Components * (Bits / 8), keep_zw, zero_zw);
        ++addr;
        if (++cycle >= wl) {
            addr += skip;
            cycle = 0;
        }
    }
}

'''),
        ('''void run_vu(int unit, std::uint32_t address, bool resume = false) {
    Unit &u = state(unit);
''', '''void run_vu(int unit, Unit &u, bool is_shadow, std::uint32_t address, bool resume = false) {
'''),
        ('''        if (shadow(unit)) return;
        vu::set_tops(u.top, u.itop);
''', '''        if (is_shadow) return;
        vu::set_tops(u.top, u.itop);
'''),
        ('''void unpack(int unit, std::uint32_t cmd, const std::uint32_t *data) {
    Unit &u = state(unit);
    bool dry = shadow(unit);
''', '''void unpack(int unit, Unit &u, bool dry, std::uint32_t cmd, const std::uint32_t *data) {
'''),
        ('''        kFastUnpack[vl][vn][usn ? 1 : 0](mem, wrap - 1, addr, wl, cl - wl, reinterpret_cast<const std::uint8_t *>(data), num);
''', '''        if (vn == 3 && vl == 0 && cl == wl) {
            // V4-32 with no gaps writes the payload verbatim. At most one wrap is possible.
            addr &= wrap - 1;
            const std::uint32_t first = std::min(num, wrap - addr);
            std::memcpy(mem + addr * 16, data, first * 16);
            if (first < num) std::memcpy(mem, data + first * 4, (num - first) * 16);
        } else {
            kFastUnpack[vl][vn][usn ? 1 : 0](mem, wrap - 1, addr, wl, cl - wl, reinterpret_cast<const std::uint8_t *>(data), num);
        }
'''),
        ('''std::uint32_t data_words(int unit, std::uint32_t cmd) {
    const Unit &u = state(unit);
''', '''std::uint32_t data_words(const Unit &u, std::uint32_t cmd) {
'''),
        ('''void execute(int unit, std::uint32_t cmd, const std::uint32_t *data) {
    Unit &u = state(unit);
''', '''void execute(int unit, Unit &u, bool from_vu, bool is_shadow, std::uint32_t cmd, const std::uint32_t *data) {
'''),
        ('''    if (!on_vu_thread()) ++g_commands[unit][op];
''', '''    if (!from_vu) ++g_commands[unit][op];
'''),
        ('''    case 0x14: case 0x15: run_vu(unit, imm * 8u); break;
    case 0x17: run_vu(unit, shadow(unit) ? 0u : vu::tpc(unit), true); break;
''', '''    case 0x14: case 0x15: run_vu(unit, u, is_shadow, imm * 8u); break;
    case 0x17: run_vu(unit, u, is_shadow, is_shadow ? 0u : vu::tpc(unit), true); break;
'''),
        ('''        if (shadow(unit)) break;
        std::uint8_t *code''', '''        if (is_shadow) break;
        std::uint8_t *code'''),
        ('''        if (shadow(unit)) gif::scan_signals''', '''        if (is_shadow) gif::scan_signals'''),
        ('''        if ((op & 0x60u) == 0x60u) unpack(unit, cmd, data);
''', '''        if ((op & 0x60u) == 0x60u) {
            if (is_shadow && u.mode != 2) u.num = 0;
            else unpack(unit, u, is_shadow, cmd, data);
        }
'''),
        ('''        if (!on_vu_thread()) hw::intc_raise(4u + static_cast<unsigned>(unit));
''', '''        if (!from_vu) hw::intc_raise(4u + static_cast<unsigned>(unit));
'''),
        ('''std::size_t run(int unit, const std::uint32_t *words, std::size_t count) {
    Unit &u = state(unit);
''', '''std::size_t run(int unit, Unit &u, bool from_vu, bool is_shadow, const std::uint32_t *words, std::size_t count) {
'''),
        ('''        const std::uint32_t need = data_words(unit, cmd);
        if (start + need > count) break;
        execute(unit, cmd, words + start);
''', '''        const std::uint32_t need = data_words(u, cmd);
        if (start + need > count) break;
        execute(unit, u, from_vu, is_shadow, cmd, words + start);
'''),
        ('''std::size_t run_skipped(int unit, const std::uint32_t *words, std::size_t count) {
    Unit &u = state(unit);
''', '''std::size_t run_skipped(int unit, Unit &u, const std::uint32_t *words, std::size_t count) {
'''),
        ('''        const std::uint32_t need = data_words(unit, cmd);
        if (start + need > count) break;
        const bool buffered = (cmd & 0xE0008000u) == 0x60008000u && u.mode != 2;
        execute(unit, cmd, words + start);
        if (!buffered) {
            std::memcpy(g_kept + kept, words + pos, (1u + need) * 4u);
            kept += 1u + need;
        }
''', '''        const std::uint32_t need = data_words(u, cmd);
        if (start + need > count) break;
        const bool buffered = (cmd & 0xE0008000u) == 0x60008000u && u.mode != 2;
        execute(unit, u, false, true, cmd, words + start);
        if (!buffered) {
            g_kept[kept++] = cmd;
            if (need) {
                std::memcpy(g_kept + kept, words + start, need * 4u);
                kept += need;
            }
        }
'''),
        ('''    std::memcpy(g_kept + kept, words + pos, left * 4u);
''', '''    if (left) std::memcpy(g_kept + kept, words + pos, left * 4u);
'''),
        ('''void process(int unit) {
    Unit &u = state(unit);
    const std::size_t pos = run(unit, u.pending.data(), u.pending.size());
''', '''void process(int unit, Unit &u, bool from_vu, bool is_shadow) {
    const std::size_t pos = run(unit, u, from_vu, is_shadow, u.pending.data(), u.pending.size());
'''),
        ('''    PS2_PROF(ps2::prof::Vif);
#ifdef PS2_PICA
    if (shadow(unit) && gs::queue::g_ee_skip && qwc * 4u <= kKeptWords) {
        Unit &u = state(unit);
''', '''    PS2_PROF(ps2::prof::Vif);
    const bool from_vu = on_vu_thread();
    const bool is_shadow = unit == 1 && !from_vu && gs::queue::vu_active();
    Unit &u = unit == 1 && from_vu ? g_vu_side : g_units[unit];
#ifdef PS2_PICA
    if (is_shadow && gs::queue::g_ee_skip && qwc <= kKeptWords / 4u) {
'''),
        ('''            const std::size_t pos = run_skipped(unit, words, count);
''', '''            const std::size_t pos = run_skipped(unit, u, words, count);
'''),
        ('''    if (shadow(unit)) gs::queue::vu().post(gs::queue::kVif1, qwc, 0, data, qwc * 16);
    Unit &u = state(unit);
''', '''    if (is_shadow) gs::queue::vu().post(gs::queue::kVif1, qwc, 0, data, qwc * 16);
'''),
        ('''        const std::size_t pos = run(unit, words, count);
''', '''        const std::size_t pos = run(unit, u, from_vu, is_shadow, words, count);
'''),
        ('''    process(unit);
}
''', '''    process(unit, u, from_vu, is_shadow);
}
'''),
        ('''                process(unit);
''', '''                process(unit, u, on_vu_thread(), shadow(unit));
'''),
        # A skipped picture's data goes across in pieces, each copied once, straight into the queue, and most of it not at all.
        ('''#include "ps2/state.hpp"
#include "ps2/system.hpp"
''', '''#include "ps2/memory.hpp"
#include "ps2/state.hpp"
#include "ps2/system.hpp"
'''),
        ('''    bool dbf = false, stalled = false;
    std::vector<std::uint32_t> pending;
};
''', '''    bool dbf = false, stalled = false;
    std::vector<std::uint32_t> pending;
    std::uint32_t dropping = 0, passing = 0;   // words of a command split across transfers still to leave out, or to pass on
};
'''),
        ('''// A skipped picture needs its programs and registers but not the data for the double buffer, and copying that across is the game
// thread's largest single cost. So only the rest goes to the VU thread, which then sees the same commands apart from those.
constexpr std::size_t kKeptWords = 1u << 14;
std::uint32_t g_kept[kKeptWords];

std::size_t run_skipped(int unit, Unit &u, const std::uint32_t *words, std::size_t count) {
    std::size_t pos = 0, kept = 0;
    while (pos < count && !u.stalled) {
''', '''// A skipped picture needs its programs and registers but not the data for the double buffer, and copying that across is the game
// thread's largest single cost. So only the rest goes to the VU thread, which then sees the same commands apart from those.
// This game names most of its data with one DMA tag and sends it with the next, so a command split in two is the usual case here.
//
// What is kept goes across as records in one long message. Commands and short data are copied into it, while the data of a long
// unpack goes as an address, for the VU thread to read from PS2 memory. The game could change that memory first, but only a picture
// nobody will see is affected, and what it sets is normally set again by the next one. Commands are always copied, because the game
// builds them afresh each picture and the VU thread must see the same ones as this side. A record is a word count, with the top bit
// set if an offset into PS2 memory follows.
constexpr std::uint32_t kAddressedWords = 16, kCopiedWords = 1024;
const std::uint32_t *g_kept = nullptr;
std::uint32_t g_kept_words = 0;

void send_kept() {
    const std::uint32_t *words = g_kept;
    std::uint32_t count = g_kept_words;
    g_kept_words = 0;
    while (count) {
        const std::uint32_t n = std::min(count, kCopiedWords);
        auto *record = reinterpret_cast<std::uint32_t *>(gs::queue::vu().extend(gs::queue::kVif1Records, 4u + n * 4u));
        record[0] = n;
        std::memcpy(record + 1, words, n * 4u);
        words += n;
        count -= n;
    }
}

void keep(const std::uint32_t *words, std::size_t count) {
    if (!count) return;
    if (g_kept_words && g_kept + g_kept_words == words) {
        g_kept_words += static_cast<std::uint32_t>(count);
        return;
    }
    if (g_kept_words) send_kept();
    g_kept = words;
    g_kept_words = static_cast<std::uint32_t>(count);
}

void keep_data(const std::uint32_t *words, std::size_t count) {
    const auto *at = reinterpret_cast<const std::uint8_t *>(words);
    if (count < kAddressedWords || at < g_ram || at >= g_ram + kRamSize) {
        keep(words, count);
        return;
    }
    if (g_kept_words) send_kept();
    auto *record = reinterpret_cast<std::uint32_t *>(gs::queue::vu().extend(gs::queue::kVif1Records, 8));
    record[0] = static_cast<std::uint32_t>(count) | 0x80000000u;
    record[1] = static_cast<std::uint32_t>(at - g_ram);
}

std::size_t run_skipped(int unit, Unit &u, const std::uint32_t *words, std::size_t count) {
    std::size_t pos = 0;
    if (u.dropping) {
        pos = std::min<std::size_t>(u.dropping, count);
        u.dropping -= static_cast<std::uint32_t>(pos);
    } else if (u.passing) {
        pos = std::min<std::size_t>(u.passing, count);
        u.passing -= static_cast<std::uint32_t>(pos);
        keep_data(words, pos);
    }
    const bool drop = gs::queue::g_ee_skip;
    while (pos < count && !u.stalled) {
'''),
        ('''        const std::size_t start = pos + 1;
        const std::uint32_t need = data_words(u, cmd);
        if (start + need > count) break;
        const bool buffered = (cmd & 0xE0008000u) == 0x60008000u && u.mode != 2;
        execute(unit, u, false, true, cmd, words + start);
        if (!buffered) {
            g_kept[kept++] = cmd;
            if (need) {
                std::memcpy(g_kept + kept, words + start, need * 4u);
                kept += need;
            }
        }
        pos = start + need;
    }
    // A command cut off by the end of the transfer has to arrive whole, so the padding goes before it.
    const std::size_t left = count - pos;
    while ((kept + left) & 3u) g_kept[kept++] = 0;
    if (left) std::memcpy(g_kept + kept, words + pos, left * 4u);
    kept += left;
    if (kept) gs::queue::vu().post(gs::queue::kVif1, static_cast<std::uint32_t>(kept / 4u), 0, g_kept, static_cast<std::uint32_t>(kept * 4u));
    return pos;
}
''', '''        const std::size_t start = pos + 1;
        const std::uint32_t need = data_words(u, cmd);
        const bool whole = start + need <= count;
        const bool unpacks = (cmd & 0x60000000u) == 0x60000000u && u.mode != 2;
        // These two do nothing with their data on this side, so they can run before it has all arrived, unless they also stop the DMA.
        if (!whole && !((unpacks || ((cmd >> 24) & 0x7Fu) == 0x4A) && !(cmd >> 31))) break;
        const bool buffered = drop && unpacks && (cmd & 0x80008000u) == 0x8000u;
        execute(unit, u, false, true, cmd, words + start);
        if (whole) {
            if (!buffered && unpacks) {
                keep(words + pos, 1);
                keep_data(words + start, need);
            } else if (!buffered) {
                keep(words + pos, 1u + need);
            }
            pos = start + need;
        } else {
            const std::size_t have = count - pos;
            (buffered ? u.dropping : u.passing) = static_cast<std::uint32_t>(1u + need - have);
            if (!buffered) {
                keep(words + pos, 1);
                keep_data(words + start, have - 1);
            }
            pos = count;
        }
    }
    keep(words + pos, count - pos);
    if (g_kept_words) send_kept();
    return pos;
}
'''),
        ('''bool stalled(int unit) { return state(unit).stalled; }
''', '''#ifdef PS2_PICA
void transfer_words(const std::uint32_t *words, std::size_t count) {
    Unit &u = g_vu_side;
    if (u.pending.empty() && !u.stalled) {
        const std::size_t pos = run(1, u, true, false, words, count);
        if (pos < count) u.pending.assign(words + pos, words + count);
        return;
    }
    u.pending.insert(u.pending.end(), words, words + count);
    process(1, u, true, false);
}

void transfer_records(const std::uint8_t *payload, std::uint32_t bytes) {
    const auto *at = reinterpret_cast<const std::uint32_t *>(payload);
    const std::uint32_t *const end = at + bytes / 4u;
    while (at < end) {
        const std::uint32_t count = *at++;
        if (count >> 31) transfer_words(reinterpret_cast<const std::uint32_t *>(g_ram + *at++), count & 0x7FFFFFFFu);
        else {
            transfer_words(at, count);
            at += count;
        }
    }
}
#endif

bool stalled(int unit) { return state(unit).stalled; }
'''),
        ('''    if (is_shadow && gs::queue::g_ee_skip && qwc <= kKeptWords / 4u) {
''', '''    if (is_shadow && (gs::queue::g_ee_skip || u.dropping || u.passing)) {
'''),
        ('''std::size_t run(int unit, Unit &u, bool from_vu, bool is_shadow, const std::uint32_t *words, std::size_t count) {
    std::size_t pos = 0;
    while (pos < count && !u.stalled) {
        const std::uint32_t cmd = words[pos];
        if (!cmd) {   // padding: about 40000 of these per picture in this game
            u.code = 0;
            ++pos;
            continue;
        }
        const std::size_t start = pos + 1;
        const std::uint32_t need = data_words(u, cmd);
        if (start + need > count) break;
        execute(unit, u, from_vu, is_shadow, cmd, words + start);
        pos = start + need;
    }
    return pos;
}
''', '''std::size_t run(int unit, Unit &u, bool from_vu, bool is_shadow, const std::uint32_t *words, std::size_t count) {
    const std::uint32_t *at = words;
    const std::uint32_t *const end = words + count;
    while (at < end && !u.stalled) {
        const std::uint32_t cmd = *at++;
        if (!cmd) {
            u.code = 0;
            while (at < end && !*at) ++at;
            continue;
        }
        const std::uint32_t need = data_words(u, cmd);
        if (static_cast<std::size_t>(end - at) < need) {
            --at;
            break;
        }
        execute(unit, u, from_vu, is_shadow, cmd, at);
        at += need;
    }
    return static_cast<std::size_t>(at - words);
}
'''),
        ('''void process(int unit, Unit &u, bool from_vu, bool is_shadow) {''', '''// DMA chains often put an UNPACK command at the end of a tag and its data in
// the next transfer. If only the command is pending, read the complete payload
// from the new transfer without copying it through the pending vector.
[[gnu::always_inline]] inline bool run_transfer(int unit, Unit &u, bool from_vu, bool is_shadow, const std::uint32_t *words, std::size_t count) {
    if (u.stalled) return false;
    std::size_t pos = 0;
    if (!u.pending.empty()) {
        if (u.pending.size() != 1) return false;
        const std::uint32_t cmd = u.pending.front();
        const std::uint32_t need = data_words(u, cmd);
        if (!need || need > count) return false;
        execute(unit, u, from_vu, is_shadow, cmd, words);
        u.pending.clear();
        pos = need;
    }
    pos += run(unit, u, from_vu, is_shadow, words + pos, count - pos);
    if (pos < count) u.pending.assign(words + pos, words + count);
    return true;
}

void process(int unit, Unit &u, bool from_vu, bool is_shadow) {'''),
        ('''    if (is_shadow) gs::queue::vu().post(gs::queue::kVif1, qwc, 0, data, qwc * 16);
    if (u.pending.empty() && !u.stalled) {
        const auto *words = reinterpret_cast<const std::uint32_t *>(data);
        const std::size_t count = static_cast<std::size_t>(qwc) * 4;
        const std::size_t pos = run(unit, u, from_vu, is_shadow, words, count);
        if (pos < count) u.pending.assign(words + pos, words + count);
        return;
    }
''', '''    if (is_shadow) gs::queue::vu().post(gs::queue::kVif1, qwc, 0, data, qwc * 16);
    const auto *words = reinterpret_cast<const std::uint32_t *>(data);
    const std::size_t count = static_cast<std::size_t>(qwc) * 4;
    if (run_transfer(unit, u, from_vu, is_shadow, words, count)) return;
'''),
        ('''void transfer_words(const std::uint32_t *words, std::size_t count) {
    Unit &u = g_vu_side;
    if (u.pending.empty() && !u.stalled) {
        const std::size_t pos = run(1, u, true, false, words, count);
        if (pos < count) u.pending.assign(words + pos, words + count);
        return;
    }
''', '''void transfer_words(const std::uint32_t *words, std::size_t count) {
    Unit &u = g_vu_side;
    if (run_transfer(1, u, true, false, words, count)) return;
'''),
        ('''        if (at_first + count * 8u <= size && std::memcmp(code + at_first, data, count * 8u) == 0) break;   // the usual case
''', '''        if (at_first + count * 8u <= size) {
            if (std::memcmp(code + at_first, data, count * 8u) == 0) break;
            // This game swaps programs in and out many times a picture. A block that does not wrap is copied in one piece, and the
            // program cache is told which part changed.
            std::memcpy(code + at_first, data, count * 8u);
            vu::code_changed(unit, at_first, count * 8u);
            break;
        }
'''),
    ],
    # Faster vector programs: the compiler is told VU memory and the registers do not overlap, flags nobody can see are not worked
    # out, and conversions and comparisons use the FPU instead of taking floats apart in integer registers.
    'runtime/src/vif.hpp': [
        ('''void transfer(int unit, const std::uint8_t *data, std::uint32_t qwc);
''', '''void transfer(int unit, const std::uint8_t *data, std::uint32_t qwc);
void transfer_records(const std::uint8_t *payload, std::uint32_t bytes);   // ps2run_3ds_patch.py: on the VU thread
'''),
    ],
    'runtime/src/gs/gif.cpp': [
        ('''            if (g_fast[sink.path].on && p.reg == 0) {
''', '''            // A+D tags with one register contain only direct GS writes.
            if (!g_fast[sink.path].on && p.nreg == 1 && (p.regs & 15u) == 0xEu) {
                const std::uint32_t loops = std::min(qwc - i, p.nloop - p.loop);
                const auto *at = data + static_cast<std::size_t>(i) * 16;
                for (std::uint32_t r = 0; r < loops; ++r, at += 16) {
                    std::uint64_t value;
                    std::memcpy(&value, at, sizeof value);
                    sink.reglist(at[8], value);
                }
                i += loops - 1;
                p.loop += loops;
                if (p.loop == p.nloop) p.active = false;
                break;
            }
            if (g_fast[sink.path].on && p.reg == 0) {
'''),
        ('''#include "ps2/state.hpp"
''', '''#include "ps2/memory.hpp"
#include "ps2/state.hpp"
'''),
        ('''    gs::queue::post(gs::queue::kGif, static_cast<std::uint32_t>(path), qwc, data, qwc * 16);
}
''', '''#ifdef PS2_PICA
    // Texture data for a skipped picture is read from PS2 memory by the VU thread, not copied: see run_skipped in vif.cpp. Each
    // cache line of this copy costs about 230 cycles, nearly all of them waiting on memory.
    if (path == 3 && qwc >= 256 && gs::queue::g_ee_skip && data >= g_ram && data + qwc * 16u <= g_ram + kRamSize) {
        gs::queue::post(gs::queue::kGifAddress, 3, qwc, &data, sizeof data);
        return;
    }
#endif
    gs::queue::post(gs::queue::kGif, static_cast<std::uint32_t>(path), qwc, data, qwc * 16);
}
'''),
    ],
    'recomp/vurecomp.cpp': [
        ('''    std::set<int> shadowed;
    for (std::uint32_t pc : pcs)
''', '''    // A result's flags can be read from four instructions later until a newer result's can. Only follow a setter that far.
    std::set<std::uint32_t> flags_seen;
    if (g_scalar && any_flags && !any_status) {
        struct Step { std::int64_t pc; unsigned age, newer; };
        for (std::uint32_t from : pcs) {
            if (!upper_sets_flags(word(p.code, from + 4))) continue;
            std::vector<Step> todo;
            std::set<std::tuple<std::int64_t, unsigned, unsigned>> visited;
            for (std::int64_t s : successors(from)) todo.push_back({s, 1u, 0u});
            bool seen = false;
            while (!todo.empty() && !seen) {
                const Step at = todo.back();
                todo.pop_back();
                if (at.newer >= 4u) continue;
                if (at.pc < 0 || !in.count(static_cast<std::uint32_t>(at.pc))) {
                    seen = true;
                    break;
                }
                if (!visited.insert({at.pc, at.age, at.newer}).second) continue;
                const std::uint32_t pc = static_cast<std::uint32_t>(at.pc);
                if (lowers[pc].reads_flags && at.age >= 4u) {
                    seen = true;
                    break;
                }
                const unsigned newer = at.newer ? at.newer + 1u : (upper_sets_flags(word(p.code, pc + 4)) ? 1u : 0u);
                for (std::int64_t s : successors(pc)) todo.push_back({s, std::min(at.age + 1u, 4u), std::min(newer, 4u)});
            }
            if (seen) flags_seen.insert(from);
        }
    }
    std::set<int> shadowed;
    for (std::uint32_t pc : pcs)
'''),
        ('''    std::string o = std::format("void {}(Vu0Regs &r, std::uint8_t *mem) {{\\n''', '''    std::string o = std::format("void {}(Vu0Regs &__restrict r, std::uint8_t *__restrict mem) {{\\n'''),
        ('''        const Upper u = upper(hi, any_status ? true : (any_flags && out_live));
''', '''        const Upper u = upper(hi, any_status ? true : (any_flags && out_live && (!g_scalar || flags_seen.count(pc))));
'''),
        # The float registers and ACC live in locals, like the integer registers, so GCC can keep them in registers and drop
        # stores a later instruction overwrites. Every exit goes through one of two labels, where they are written back.
        ('''        else if (!l.supported) body += std::format("vu::interpret(1, {}u); return; ", pc);
''', '''        else if (!l.supported) body += std::format("jpc = {}u; goto dispatch; ", pc);
'''),
        ('''                o += std::format("    if (ending) {{ r.tpc = {}; return; }}\\n", tpc);
''', '''                o += std::format("    if (ending) {{ jpc = {}; goto leave; }}\\n", tpc);
'''),
        ('''        if (!in.count(next)) o += std::format("    vu::interpret(1, {}u);\\n    return;\\n", next);
    }
    o += "dispatch:\\n    vu::interpret(1, jpc);\\n    return;\\n}\\n\\n";
''', '''        if (!in.count(next)) o += std::format("    jpc = {}u;\\n    goto dispatch;\\n", next);
    }
    o += "dispatch:\\n    vu::interpret(1, jpc);\\n    return;\\nleave:\\n    r.tpc = jpc;\\n    return;\\n}\\n\\n";
'''),
        ('''    return local_vi(o);
''', '''    return local_vi(local_vf(o));
'''),
        ('''// VU integer ALU instructions whose result a directly following conditional branch does not see yet
''', '''// Whether the program can change this register: an assignment to it or a lane, or a copy or store into it.
bool writes_register(const std::string &o, const std::string &name) {
    for (std::size_t at = o.find(name); at != std::string::npos; at = o.find(name, at + 1)) {
        const std::string_view before(o.data(), at), after(o.data() + at + name.size());
        if (before.ends_with("memcpy(&") || before.ends_with("vstore(") || before.ends_with("put(")) return true;
        const std::size_t lane = after.starts_with(".w[") || after.starts_with(".sw[") ? after.find(']') + 1 : 0;
        if (after.substr(lane).starts_with(" = ")) return true;
    }
    return false;
}

// The float registers VF1 to VF31 and ACC as locals, and those the program changes written back where it leaves. efu reads its
// register from r.
std::string local_vf(std::string o) {
    std::string declare, sync;
    for (int n = 0; n < 33; ++n) {
        const std::string get = n == 32 ? "r.acc" : std::format("r.vf[{}]", n), local = n == 32 ? "acc" : std::format("vf{}", n);
        if (!n || o.find(get) == std::string::npos) continue;
        if (writes_register(o, get)) sync += std::format("{} = {}; ", get, local);
        replace_all(o, get, local);
        declare += std::format("    Reg128 {} = {};\\n", local, get);
    }
    if (declare.empty()) return o;
    o.insert(o.find("    goto L_"), declare);
    if (!sync.empty())
        for (const char *exit : {"vu::interpret(", "r.tpc = ", "efu(r, "}) replace_all(o, exit, sync + exit);
    return o;
}

// VU integer ALU instructions whose result a directly following conditional branch does not see yet
'''),
    ],
    # eerecomp --natives: a name that starts with loop_ goes at the top of a loop, returns nothing and leaves the game's code to carry on.
    'recomp/codegen.cpp': [
        ('''                line(std::format("{{ bool native_{0}(Cpu &); if (native_{0}(c)) {{ c.pc = c.r[31].w[0]; return; }} }}", native->second));
''', '''                if (native->second.starts_with("loop_")) line(std::format("{{ void native_{0}(Cpu &); native_{0}(c); }}", native->second));
                else line(std::format("{{ bool native_{0}(Cpu &); if (native_{0}(c)) {{ c.pc = c.r[31].w[0]; return; }} }}", native->second));
'''),
    ],
    'runtime/include/ps2/fpu.hpp': [
        ('''    if (fraction == 0) return static_cast<std::uint32_t>(static_cast<std::int32_t>(to_float(x)));   // the FPU saturates as the PS2 does
#endif
    const int e = static_cast<int>(exponent(x)) + fraction;
''', '''    return static_cast<std::uint32_t>(static_cast<std::int32_t>(to_float(x) * static_cast<float>(1 << fraction)));   // the FPU saturates as the PS2 does
#else
    const int e = static_cast<int>(exponent(x)) + fraction;
'''),
        ('''    return negative ? 0u - v : v;
}

inline std::uint32_t from_fixed''', '''    return negative ? 0u - v : v;
#endif
}

inline std::uint32_t from_fixed'''),
        ('''inline Result div(std::uint32_t a, std::uint32_t b) {
    a = flush(a);
''', '''inline Result div(std::uint32_t a, std::uint32_t b) {
#ifdef PS2_VU_FAST
    if (exponent(b)) return {from_float(to_float(a) / to_float(b)), false};   // denormals count as zero in the FPU
#endif
    a = flush(a);
'''),
        ('''inline std::uint32_t max(std::uint32_t a, std::uint32_t b) { return key(a) >= key(b) ? a : b; }
inline std::uint32_t min(std::uint32_t a, std::uint32_t b) { return key(a) <= key(b) ? a : b; }
''', '''#ifdef PS2_VU_FAST
// The bits put in integer order, so a comparison needs no float compare, which stalls the FPU. Only denormals and the two zeros can
// come out differently from comparing them as floats.
inline std::int32_t order(std::uint32_t x) {
    return static_cast<std::int32_t>(x ^ (static_cast<std::uint32_t>(static_cast<std::int32_t>(x) >> 31) >> 1));
}
inline std::uint32_t max(std::uint32_t a, std::uint32_t b) { return order(a) >= order(b) ? a : b; }
inline std::uint32_t min(std::uint32_t a, std::uint32_t b) { return order(a) <= order(b) ? a : b; }
#else
inline std::uint32_t max(std::uint32_t a, std::uint32_t b) { return key(a) >= key(b) ? a : b; }
inline std::uint32_t min(std::uint32_t a, std::uint32_t b) { return key(a) <= key(b) ? a : b; }
#endif
'''),
    ],
    # A program upload marks only the 256-byte pages it changed, so the programs looked up elsewhere in VU memory stay cached.
    'runtime/src/vu_micro.hpp': [
        ('''void code_changed(int unit);
''', '''void code_changed(int unit);
void code_changed(int unit, std::uint32_t first, std::uint32_t bytes);   // ps2run_3ds_patch.py: only these bytes of code changed
'''),
    ],
    'runtime/src/vu_micro.cpp': [
        ('''struct Candidate {
    std::vector<std::uint32_t> runs;
    std::vector<std::uint8_t> code;
''', '''struct Candidate {
    std::vector<std::uint32_t> runs;
    std::vector<std::uint8_t> code;
    std::uint64_t pages = 0;   // the 256-byte pages of VU1 code the runs cover
'''),
        ('''    std::vector<Candidate> candidates;
    std::uint32_t next = 0;
};
''', '''    std::vector<Candidate> candidates;
    std::uint32_t next = 0;
    std::uint64_t pages = 0;   // the pages the cached answer depends on
};
'''),
        ('''        c.code.insert(c.code.end(), g_vu1_code + pc, g_vu1_code + pc + 8);
''', '''        c.code.insert(c.code.end(), g_vu1_code + pc, g_vu1_code + pc + 8);
        c.pages |= std::uint64_t{1} << ((pc >> 8) & 63u);
'''),
        ('''void code_changed(int unit) {
    g_scan[unit & 1] = true;
    ++g_generation[unit & 1];
}
''', '''// The generation at which each 256-byte page of VU1 code last changed. A cached program none of whose pages changed since it was
// looked up is still the right one, whatever was uploaded elsewhere.
std::uint32_t g_page_changed[64] = {};

void code_changed(int unit) {
    g_scan[unit & 1] = true;
    ++g_generation[unit & 1];
    if (unit & 1)
        for (std::uint32_t &page : g_page_changed) page = g_generation[1];
}

void code_changed(int unit, std::uint32_t first, std::uint32_t bytes) {
    if (!(unit & 1) || !bytes || first + bytes > 16384u) {
        code_changed(unit);
        return;
    }
    g_scan[1] = true;
    ++g_generation[1];
    for (std::uint32_t page = first >> 8; page <= (first + bytes - 1) >> 8; ++page) g_page_changed[page] = g_generation[1];
}
'''),
        ('''    if (e.generation == g_generation[1]) return e.function;
    e.generation = g_generation[1];
    for (const Candidate &c : e.candidates)
        if (candidate_matches(c)) {
            e.function = c.function;
            e.jit = c.jit;
            return e.function;
        }
''', '''    if (e.generation == g_generation[1]) return e.function;
    if (e.pages) {
        bool stale = false;
        for (std::uint64_t m = e.pages; m && !stale; m &= m - 1) stale = g_page_changed[__builtin_ctzll(m)] > e.generation;
        if (!stale) {
            e.generation = g_generation[1];
            return e.function;
        }
    }
    e.generation = g_generation[1];
    for (const Candidate &c : e.candidates)
        if (candidate_matches(c)) {
            e.function = c.function;
            e.jit = c.jit;
            e.pages = c.pages;
            return e.function;
        }
'''),
        ('''    Candidate c = make_candidate(pcs);
    c.function = e.function;
    c.jit = e.jit;
''', '''    Candidate c = make_candidate(pcs);
    c.function = e.function;
    c.jit = e.jit;
    e.pages = c.pages;
'''),
    ],
}


# Keep the optional 3DS GS worker after the serial VU and graphics patches.
SPLIT_GS_EDITS = {
    'runtime/src/gs/queue.hpp': [
        ('''extern bool g_quiet;''', '''#ifdef PS2_PICA
extern bool g_split_pica;
#endif
extern bool g_quiet;'''),
    ],
    'runtime/src/gs/queue.cpp': [
        ('''bool g_quiet = false;''', '''bool g_quiet = false;
#ifdef PS2_PICA
bool g_split_pica = false;
#endif'''),
        ('''        PS2_PROF(ps2::prof::Full);
        svcSleepThread(100000);''', '''        // Only the game-to-VU queue feeds the EE frame-skipping heuristic.
        if (this == &g_vu) {
            PS2_PROF(ps2::prof::Full);
            svcSleepThread(100000);
        } else {
            svcSleepThread(100000);
        }'''),
    ],
    'runtime/src/gs/gs.cpp': [
        ('''void start_thread() { queue::render().start(4096); }   // never carries anything: serve() below takes the messages directly''', '''void start_thread() { queue::render().start(queue::g_split_pica ? 512u << 10 : 4096u); }'''),
    ],
    'runtime/src/gs/gif.cpp': [
        ('''    GsSink sink{path};
    walk(g_paths[path], data, qwc, sink);''', '''#ifdef PS2_PICA
    if (gs::queue::g_split_pica && gs::queue::role() == gs::queue::Role::Vu) {
        // The VU reuses its packet memory after this call. Copy complete qwords
        // into the ring; the GS retains the parser state across chunks.
        while (qwc) {
            const std::uint32_t part = std::min<std::uint32_t>(qwc, 4096u);
            gs::queue::render().post(gs::queue::kGif, path, part, data, part * 16);
            qwc -= part;
            data += part * 16;
        }
        return;
    }
#endif
    GsSink sink{path};
    walk(g_paths[path], data, qwc, sink);'''),
    ],
    'runtime/src/system.cpp': [
        ('''void run_vu_thread() {''', '''#ifdef PS2_PICA
void enable_split_pica(bool enabled) { gs::queue::g_split_pica = enabled; }

void run_pica_thread() {
    using namespace gs::queue;
    role() = Role::Render;
    Ring &in = render();
    for (;;) {
        Header h;
        const std::uint8_t *payload = nullptr;
        if (!in.peek(h, payload)) {
            in.wait_for_work(50);
            continue;
        }
        if (h.type == kVuStop) {
            in.pop(h);
            return;
        }
        if (h.type == kGif) gif::render(static_cast<int>(h.arg0), payload, h.arg1);
        else if (h.type == kVuFence) {
            g_vu_fence.fetch_add(1);
            notify(g_vu_fence);
        } else gs::serve(h.type, h.arg0, payload);
        in.pop(h);
    }
}
#endif

void run_vu_thread() {'''),
        ('''            case gs::queue::kVuFence:
                g_vu_fence.fetch_add(1);''', '''            case gs::queue::kVuFence:
#ifdef PS2_PICA
                if (gs::queue::g_split_pica) {
                    gs::queue::render().post(gs::queue::kVuFence, 0, 0, nullptr, 0);
                    break;
                }
#endif
                g_vu_fence.fetch_add(1);'''),
        ('''            case gs::queue::kVuStop:
                in.pop(h);
                return;
            default: gs::serve(h.type, h.arg0, payload); break;''', '''            case gs::queue::kVuStop:
                if (gs::queue::g_split_pica) gs::queue::render().post(gs::queue::kVuStop, 0, 0, nullptr, 0);
                in.pop(h);
                return;
            default:
                if (gs::queue::g_split_pica)
                    gs::queue::render().post(static_cast<gs::queue::Type>(h.type), h.arg0, h.arg1, payload, h.bytes);
                else gs::serve(h.type, h.arg0, payload);
                break;'''),
        ('''    gs::stats();
''', '''#ifdef PS2_PICA
    if (gs::queue::g_split_pica) sync_vu_thread();
#endif
    gs::stats();
'''),
    ],
    'runtime/include/ps2/system.hpp': [
        ('''void start_vu_thread();
''', '''void start_vu_thread();
#ifdef __3DS__
void enable_split_pica(bool enabled);
void run_pica_thread();
#endif
'''),
    ],
}


# Translated calls nest on the host stack. They used to pass a depth count to every call, which cost a register and three
# instructions per call site; now each call compares the stack pointer with a floor (a constant on the 3DS, where the game
# thread's stack sits at a fixed address), and a call that would go below it unwinds to the dispatcher as before.
STACK_EDITS = {
    'runtime/include/ps2/generated.hpp': [
        (r'''using UnitFn = void (*)(Cpu &, std::uint32_t entry, std::uint32_t depth);

inline constexpr std::uint32_t kMaxDepth = 192;
''', r'''using UnitFn = void (*)(Cpu &, std::uint32_t entry);

// A translated call goes ahead only while the host stack is above a floor; otherwise it unwinds to the dispatcher. The stack
// pointer is fixed for the length of a function, so the compiler tests it once per call of a unit.
#ifdef __3DS__
inline constexpr std::uint32_t kStackFloor3ds = 0x0BD00000u;   // the game thread's stack is mapped above it (main3ds.cpp); an ARM immediate
[[gnu::always_inline]] inline bool stack_ok() {
    std::uint32_t sp;
    asm("mov %0, sp" : "=r"(sp));
    return sp >= kStackFloor3ds;
}
#else
extern std::uintptr_t g_stack_floor;   // set by run(); 0 lets every call through
[[gnu::always_inline]] inline bool stack_ok() { return reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0)) >= g_stack_floor; }
#endif
'''),
        (r'''void dispatch(Cpu &c, std::uint32_t entry, std::uint32_t depth);
void call(Cpu &c, std::uint32_t target, std::uint32_t depth);
''', r'''void dispatch(Cpu &c, std::uint32_t entry);
void call(Cpu &c, std::uint32_t target);
'''),
    ],
    'runtime/src/dispatch.cpp': [
        (r'''void dispatch(Cpu &c, std::uint32_t, std::uint32_t depth) {
''', r'''#ifndef __3DS__
std::uintptr_t g_stack_floor = 0;
#endif

void dispatch(Cpu &c, std::uint32_t) {
'''),
        (r'''                mu->function(c, entry, depth);
''', r'''                mu->function(c, entry);
'''),
        (r'''    u->function(c, id, depth);
}

void call(Cpu &c, std::uint32_t target, std::uint32_t depth) {
    c.pc = target;
    if (depth >= kMaxDepth) return;
    dispatch(c, 0, depth);
}
''', r'''    u->function(c, id);
}

void call(Cpu &c, std::uint32_t target) {
    c.pc = target;
    if (!stack_ok()) return;
    dispatch(c, 0);
}
'''),
    ],
    'runtime/src/kernel.cpp': [
        (r'''        dispatch(c, 0, 0);
    }
    g_guest_calls.pop_back();
''', r'''        dispatch(c, 0);
    }
    g_guest_calls.pop_back();
'''),
    ],
    'runtime/src/system.cpp': [
        (r'''int run() {
    Cpu &c = g_cpu;
''', r'''int run() {
    Cpu &c = g_cpu;
#ifndef __3DS__
    if (!g_stack_floor) g_stack_floor = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0)) - (256u << 10);
#endif
'''),
        (r'''        if (c.budget <= 0) refill(c);
        dispatch(c, 0, 0);
''', r'''        if (c.budget <= 0) refill(c);
        dispatch(c, 0);
'''),
    ],
    'recomp/codegen.cpp': [
        (r'''                line("{ const std::uint32_t t = eret(c); c.pc = t; [[clang::musttail]] return dispatch(c, 0u, depth); }");
            } else if (fl == Flow::Stop) {
''', r'''                line("{ const std::uint32_t t = eret(c); c.pc = t; [[clang::musttail]] return dispatch(c, 0u); }");
            } else if (fl == Flow::Stop) {
'''),
        (r'''                line("{ const std::uint32_t t = eret(c); c.pc = t; [[clang::musttail]] return dispatch(c, 0u, depth); }");
                falls = false;
''', r'''                line("{ const std::uint32_t t = eret(c); c.pc = t; [[clang::musttail]] return dispatch(c, 0u); }");
                falls = false;
'''),
        (r'''"(Cpu &c, std::uint32_t entry, std::uint32_t depth) {\n";''', r'''"(Cpu &c, std::uint32_t entry) {\n";'''),
        (r'''line(std::format("{}[[clang::musttail]] return {}(c, {}u, depth);", rebase(), unit_name(layout_.unit_of(a)), id_of(a)));''',
         r'''line(std::format("{}[[clang::musttail]] return {}(c, {}u);", rebase(), unit_name(layout_.unit_of(a)), id_of(a)));'''),
        (r'''line(std::format("c.pc = {}; [[clang::musttail]] return dispatch(c, 0u, depth);", pc(a)));''',
         r'''line(std::format("c.pc = {}; [[clang::musttail]] return dispatch(c, 0u);", pc(a)));'''),
        (r'''return check + std::format("{}[[clang::musttail]] return {}(c, {}u, depth);", rebase(), unit_name(layout_.unit_of(t)), id_of(t));''',
         r'''return check + std::format("{}[[clang::musttail]] return {}(c, {}u);", rebase(), unit_name(layout_.unit_of(t)), id_of(t));'''),
        (r'''return std::format("c.pc = {}; [[clang::musttail]] return dispatch(c, 0u, depth);", pc(t));''',
         r'''return std::format("c.pc = {}; [[clang::musttail]] return dispatch(c, 0u);", pc(t));'''),
        (r'''            invoke = relocatable_ ? std::format("if (depth < kMaxDepth) {{ c.base = B; {}(c, {}u, depth + 1u); }} else {{ c.pc = {}; return; }}",
                                                unit_name(layout_.unit_of(t)), id_of(t), pc(t))
                                  : std::format("if (depth < kMaxDepth) {}(c, {}u, depth + 1u); else {{ c.pc = {}; return; }}",
                                                unit_name(layout_.unit_of(t)), id_of(t), pc(t));
        else
            invoke = std::format("call(c, {}, depth + 1u);", pc(t));
''', r'''            invoke = relocatable_ ? std::format("if (stack_ok()) {{ c.base = B; {}(c, {}u); }} else {{ c.pc = {}; return; }}",
                                                unit_name(layout_.unit_of(t)), id_of(t), pc(t))
                                  : std::format("if (stack_ok()) {}(c, {}u); else {{ c.pc = {}; return; }}",
                                                unit_name(layout_.unit_of(t)), id_of(t), pc(t));
        else
            invoke = std::format("call(c, {});", pc(t));
'''),
        (r'''{} {} call(c, t, depth + 1u); if (c.pc != {}) [[unlikely]] return; }}", target, link, slot''',
         r'''{} {} call(c, t); if (c.pc != {}) [[unlikely]] return; }}", target, link, slot'''),
        (r'''c.pc = t; [[clang::musttail]] return dispatch(c, 0u, depth); }}", target, slot));''',
         r'''c.pc = t; [[clang::musttail]] return dispatch(c, 0u); }}", target, slot));'''),
        (r'''if (entry) goto dispatch_entry; }} [[clang::musttail]] return dispatch(c, 0u, depth); }}"''',
         r'''if (entry) goto dispatch_entry; }} [[clang::musttail]] return dispatch(c, 0u); }}"'''),
        (r'''c.pc = t; call(c, t, depth + 1u); if (c.pc != {}) [[unlikely]] return; }}",''',
         r'''c.pc = t; call(c, t); if (c.pc != {}) [[unlikely]] return; }}",'''),
        (r'''"(ps2::Cpu &c, std::uint32_t entry, std::uint32_t depth);\n";''', r'''"(ps2::Cpu &c, std::uint32_t entry);\n";'''),
    ],
}


# Guest registers are 64 bits wide (128 with the multimedia half), but nearly all game code works on 32-bit values whose
# upper word only copies bit 31. Storing that copy after every 32-bit result costs an extra instruction and an extra store.
# The generator now works out, per unit, which registers' upper words are known to be such a copy (so 64-bit operations
# on them can be done in 32 bits) and which upper words nothing reads before they are rewritten (so they need not be
# stored). Every exit from the unit counts as reading every register, so guest state outside a unit is unchanged.
# EERECOMP_WIDE=1 in the environment turns it off.
NARROW_EDITS = {
    'recomp/codegen.cpp': [
        (r'''#include <bit>
''', r'''#include <bit>
#include <cstdlib>
'''),
        (r'''std::string d(std::uint32_t r) { return r ? std::format("c.r[{}].d[0]", r) : "0ull"; }
std::string sd(std::uint32_t r) { return r ? std::format("c.r[{}].sd[0]", r) : "0ll"; }
std::string reg(std::uint32_t r) { return std::format("c.r[{}]", r); }
''', r'''// ps2run_3ds_patch.py: while the generator works out which upper words an instruction reads, d, sd and reg note them here
std::uint32_t *g_hi_reads = nullptr;
std::string d(std::uint32_t r) { if (g_hi_reads) *g_hi_reads |= 1u << r; return r ? std::format("c.r[{}].d[0]", r) : "0ull"; }
std::string sd(std::uint32_t r) { if (g_hi_reads) *g_hi_reads |= 1u << r; return r ? std::format("c.r[{}].sd[0]", r) : "0ll"; }
std::string reg(std::uint32_t r) { if (g_hi_reads) *g_hi_reads |= 1u << r; return std::format("c.r[{}]", r); }
'''),
        (r'''    std::string uimm(const Insn &i) const {
        if (const auto e = field(i)) return "(static_cast<std::uint64_t>(" + *e + ") & 0xFFFFull)";
        return std::format("{}ull", i.uimm());
    }
''', r'''    std::string uimm(const Insn &i) const {
        if (const auto e = field(i)) return "(static_cast<std::uint64_t>(" + *e + ") & 0xFFFFull)";
        return std::format("{}ull", i.uimm());
    }

    std::string uimm32(const Insn &i) const {
        if (const auto e = field(i)) return "(static_cast<std::uint32_t>(" + *e + ") & 0xFFFFu)";
        return std::format("{}u", i.uimm());
    }

    // ps2run_3ds_patch.py: facts about the guest registers' upper words at one instruction of the unit being written.
    // canon: registers whose upper word is known to copy bit 31 of the lower one (r0 always).
    // live: registers whose upper word may be read after the instruction before it is rewritten; every exit reads them all.
    struct Ctx { std::uint32_t canon = 1u, live = ~1u; };
    struct Effect { std::uint32_t reads = 0, kills = 0, out = 1u; bool barrier = false; };

    bool cn(std::uint32_t r) const { return r == 0 || ((ctx_.canon >> r) & 1u); }
    std::string s32(std::uint32_t r) { return cn(r) ? std::format("static_cast<std::int32_t>({})", w(r)) : sd(r); }

    void use_node(std::uint32_t a, bool slot) {
        ctx_ = narrow_ ? nodes_[((a - base_) >> 2) * 2 + (slot ? 1u : 0u)] : Ctx{};
    }

    // what one instruction (or the delay slot of the branch at a) does to the facts, found by generating it and dropping the text
    Effect dry(std::uint32_t a, bool slot, std::uint32_t canon) {
        Effect e;
        const Ctx keep = ctx_;
        const std::uint32_t pending = pending_;
        std::string *const capture = capture_;
        std::string text;
        ctx_ = Ctx{canon | 1u, ~1u};
        kills_ = canon_set_ = 0;
        g_hi_reads = &e.reads;
        const Insn i = at(a);
        const Flow fl = flow(i.op);
        std::uint32_t clobber = 0;
        if (slot) {
            const Insn ds = p_.is_code(a + 4) ? at(a + 4) : Insn{a + 4, 0, Invalid};
            text = delay(ds);
            clobber = (1u << ds.rd()) | (1u << ds.rt());
        } else if (fl == Flow::None) {
            capture_ = &text;
            simple(i);
            clobber = (1u << i.rd()) | (1u << i.rt());
        } else if (fl != Flow::Stop) {
            if (fl == Flow::Branch || fl == Flow::BranchLikely || i.op == Bltzal || i.op == Bgezal || i.op == Bltzall || i.op == Bgezall)
                (void)condition(i);
            clobber = (1u << 31) | (fl == Flow::CallRegister ? 1u << i.rd() : 0u);
        }
        g_hi_reads = nullptr;
        capture_ = capture;
        pending_ = pending;
        e.reads &= ~1u;
        e.kills = kills_ & ~1u;
        e.barrier = (!slot && fl == Flow::Stop) || text.find("return") != std::string::npos || text.find("trap(c") != std::string::npos
                    || text.find("breakpoint(c") != std::string::npos;
        e.out = e.barrier ? 1u : ((canon & ~clobber) | canon_set_ | 1u);
        ctx_ = keep;
        return e;
    }

    void analyse_narrow() {
        const std::uint32_t words = layout_.size() / 4, end = base_ + layout_.size();
        narrow_ = std::getenv("EERECOMP_WIDE") == nullptr;
        for (std::uint32_t a = base_; narrow_ && a < end; a += 4)
            if (p_.is_code(a) && group_start(a)) narrow_ = false;
        ctx_ = Ctx{};
        if (!narrow_) return;
        constexpr std::uint32_t kExit = 0xFFFFFFFFu;
        struct Edge { std::uint32_t to; bool reset; };
        struct Node { bool code = false, pre = false, reached = false; std::vector<Edge> next; };
        std::vector<Node> g(2 * words);
        const auto index = [&](std::uint32_t a, bool slot) { return ((a - base_) >> 2) * 2 + (slot ? 1u : 0u); };
        const auto normal = [&](std::uint32_t a) { return in_unit(a) && p_.is_code(a) ? index(a, false) : kExit; };
        for (std::uint32_t a = base_; a < end; a += 4) {
            if (!p_.is_code(a)) continue;
            Node &n = g[index(a, false)];
            n.code = true;
            n.pre = options_.natives.count(a) || options_.probes.count(a);
            const Insn i = at(a);
            const Flow fl = flow(i.op);
            if (fl == Flow::None) {
                n.next.push_back({normal(a + 4), false});
                continue;
            }
            if (fl == Flow::Stop) continue;
            Node &s = g[index(a, true)];
            s.code = true;
            n.next.push_back({index(a, true), false});
            const std::uint32_t t = i.branch_target_of();
            const auto target = [&](Node &x) {
                if (dynamic_jump(a) || !in_unit(t) || !p_.is_code(t)) {
                    x.next.push_back({kExit, false});
                    return;
                }
                x.next.push_back({index(t, false), false});
                if (t <= a) x.next.push_back({kExit, false});   // the time-slice check before a backward jump may leave
            };
            if (fl == Flow::BranchLikely || i.op == Bltzall || i.op == Bgezall) n.next.push_back({normal(a + 8), false});
            switch (fl) {
            case Flow::Branch: target(s); s.next.push_back({normal(a + 8), false}); break;
            case Flow::BranchLikely: case Flow::Jump: target(s); break;
            case Flow::Call: case Flow::CallRegister: s.next.push_back({kExit, false}); s.next.push_back({normal(a + 8), true}); break;
            default: s.next.push_back({kExit, false}); break;
            }
        }
        std::vector<std::uint32_t> canon(2 * words, ~0u);
        for (const Node &n : g)
            for (const Edge &e : n.next)
                if (e.to != kExit) g[e.to].reached = true;
        for (std::uint32_t k = 0; k < g.size(); ++k)
            if (g[k].pre || !g[k].reached) canon[k] = 1u;
        for (const auto &[x, id] : ids_[layout_.unit_of(base_)]) canon[index(x, false)] = 1u;   // entered from outside
        std::vector<Effect> fx(2 * words);
        for (bool changed = true; changed;) {
            changed = false;
            for (std::uint32_t k = 0; k < g.size(); ++k) {
                if (!g[k].code) continue;
                fx[k] = dry(base_ + (k >> 1) * 4u, (k & 1u) != 0, canon[k]);
                for (const Edge &e : g[k].next) {
                    if (e.to == kExit) continue;
                    const std::uint32_t v = canon[e.to] & (e.reset ? 1u : fx[k].out);
                    if (v != canon[e.to]) {
                        canon[e.to] = v;
                        changed = true;
                    }
                }
            }
        }
        std::vector<std::uint32_t> in(2 * words, 0u);
        const auto out_of = [&](std::uint32_t k) {
            std::uint32_t v = 0;
            for (const Edge &e : g[k].next) v |= e.to == kExit ? ~1u : in[e.to];
            return v;
        };
        for (bool changed = true; changed;) {
            changed = false;
            for (std::uint32_t k = static_cast<std::uint32_t>(g.size()); k-- > 0;) {
                if (!g[k].code) continue;
                const std::uint32_t v = g[k].pre || fx[k].barrier ? ~1u : (fx[k].reads | (out_of(k) & ~fx[k].kills));
                if (v != in[k]) {
                    in[k] = v;
                    changed = true;
                }
            }
        }
        nodes_.assign(2 * words, Ctx{});
        for (std::uint32_t k = 0; k < g.size(); ++k)
            if (g[k].code) nodes_[k] = Ctx{canon[k] | 1u, out_of(k)};
    }
'''),
        (r'''        out_.str({});
''', r'''        analyse_narrow();
        out_.str({});
'''),
        (r'''            if (options_.probes.count(a)) line(std::format("probe(c, {});", pc(a)));
            const Insn i = at(a);
''', r'''            if (options_.probes.count(a)) line(std::format("probe(c, {});", pc(a)));
            const Insn i = at(a);
            use_node(a, false);
'''),
        (r'''        const std::string slot = delay(ds);
''', r'''        const Ctx keep = ctx_;   // ps2run_3ds_patch.py: the delay slot has facts of its own
        use_node(a, true);
        const std::string slot = delay(ds);
        ctx_ = keep;
'''),
        (r'''        case Beq: case Beql: return d(i.rs()) + " == " + d(i.rt());
        case Bne: case Bnel: return d(i.rs()) + " != " + d(i.rt());
        case Blez: case Blezl: return sd(i.rs()) + " <= 0";
        case Bgtz: case Bgtzl: return sd(i.rs()) + " > 0";
        case Bltz: case Bltzl: case Bltzal: case Bltzall: return sd(i.rs()) + " < 0";
        case Bgez: case Bgezl: case Bgezal: case Bgezall: return sd(i.rs()) + " >= 0";
''', r'''        case Beq: case Beql: return cn(i.rs()) && cn(i.rt()) ? w(i.rs()) + " == " + w(i.rt()) : d(i.rs()) + " == " + d(i.rt());
        case Bne: case Bnel: return cn(i.rs()) && cn(i.rt()) ? w(i.rs()) + " != " + w(i.rt()) : d(i.rs()) + " != " + d(i.rt());
        case Blez: case Blezl: return s32(i.rs()) + " <= 0";
        case Bgtz: case Bgtzl: return s32(i.rs()) + " > 0";
        case Bltz: case Bltzl: case Bltzal: case Bltzall: return s32(i.rs()) + " < 0";
        case Bgez: case Bgezl: case Bgezal: case Bgezall: return s32(i.rs()) + " >= 0";
'''),
        (r'''    void set_d(std::uint32_t r, const std::string &value) {
        if (r) line(std::format("c.r[{}].d[0] = {};", r, value));
    }

    void set_w(std::uint32_t r, const std::string &value) {
        if (r) line(std::format("set32(c.r[{}], {});", r, value));
    }
''', r'''    void set_d(std::uint32_t r, const std::string &value) {
        if (!r) return;
        kills_ |= 1u << r;
        line(std::format("c.r[{}].d[0] = {};", r, value));
    }

    // ps2run_3ds_patch.py: a 32-bit result; its upper word is stored only when something may read it before it is rewritten
    void set_w(std::uint32_t r, const std::string &value) {
        if (!r) return;
        kills_ |= 1u << r;
        canon_set_ |= 1u << r;
        if ((ctx_.live >> r) & 1u) line(std::format("set32(c.r[{}], {});", r, value));
        else line(std::format("c.r[{}].w[0] = {};", r, value));
    }
'''),
        (r'''        case Movz: if (rd) line(std::format("if ({} == 0) c.r[{}].d[0] = {};", d(rt), rd, d(rs))); break;
        case Movn: if (rd) line(std::format("if ({} != 0) c.r[{}].d[0] = {};", d(rt), rd, d(rs))); break;
''', r'''        case Movz: case Movn: {
            if (!rd) break;
            const std::string test = std::format("if ({} {} 0)", cn(rt) ? w(rt) : d(rt), i.op == Movz ? "==" : "!=");
            if (cn(rd) && cn(rs)) canon_set_ |= 1u << rd;
            if (!cn(rs)) line(std::format("{} c.r[{}].d[0] = {};", test, rd, d(rs)));
            else if ((ctx_.live >> rd) & 1u) line(std::format("{} set32(c.r[{}], {});", test, rd, w(rs)));
            else line(std::format("{} c.r[{}].w[0] = {};", test, rd, w(rs)));
            break;
        }
'''),
        (r'''        case Add: case Addu: set_w(rd, w(rs) + " + " + w(rt)); break;
        case Sub: case Subu: set_w(rd, w(rs) + " - " + w(rt)); break;
        case And: set_d(rd, d(rs) + " & " + d(rt)); break;
        case Or: set_d(rd, d(rs) + " | " + d(rt)); break;
        case Xor: set_d(rd, d(rs) + " ^ " + d(rt)); break;
        case Nor: set_d(rd, "~(" + d(rs) + " | " + d(rt) + ")"); break;
''', r'''        case Add: case Addu: set_w(rd, w(rs) + " + " + w(rt)); break;
        case Sub: case Subu: set_w(rd, w(rs) + " - " + w(rt)); break;
        case And: if (cn(rs) && cn(rt)) set_w(rd, w(rs) + " & " + w(rt)); else set_d(rd, d(rs) + " & " + d(rt)); break;
        case Or: if (cn(rs) && cn(rt)) set_w(rd, w(rs) + " | " + w(rt)); else set_d(rd, d(rs) + " | " + d(rt)); break;
        case Xor: if (cn(rs) && cn(rt)) set_w(rd, w(rs) + " ^ " + w(rt)); else set_d(rd, d(rs) + " ^ " + d(rt)); break;
        case Nor: if (cn(rs) && cn(rt)) set_w(rd, "~(" + w(rs) + " | " + w(rt) + ")"); else set_d(rd, "~(" + d(rs) + " | " + d(rt) + ")"); break;
'''),
        (r'''        case Slt: set_d(rd, std::format("{} < {} ? 1ull : 0ull", sd(rs), sd(rt))); break;
        case Sltu: set_d(rd, std::format("{} < {} ? 1ull : 0ull", d(rs), d(rt))); break;
        case Dadd: case Daddu: set_d(rd, d(rs) + " + " + d(rt)); break;
        case Dsub: case Dsubu: set_d(rd, d(rs) + " - " + d(rt)); break;
''', r'''        case Slt:
            if (cn(rs) && cn(rt)) set_w(rd, std::format("static_cast<std::int32_t>({}) < static_cast<std::int32_t>({}) ? 1u : 0u", w(rs), w(rt)));
            else set_w(rd, std::format("{} < {} ? 1u : 0u", sd(rs), sd(rt)));
            break;
        case Sltu:
            if (cn(rs) && cn(rt)) set_w(rd, std::format("{} < {} ? 1u : 0u", w(rs), w(rt)));
            else set_w(rd, std::format("{} < {} ? 1u : 0u", d(rs), d(rt)));
            break;
        case Dadd: case Daddu:
            if ((rt == 0 && cn(rs)) || (rs == 0 && cn(rt))) set_w(rd, w(rs | rt));
            else set_d(rd, d(rs) + " + " + d(rt));
            break;
        case Dsub: case Dsubu: if (rt == 0 && cn(rs)) set_w(rd, w(rs)); else set_d(rd, d(rs) + " - " + d(rt)); break;
'''),
        (r'''        case Dsrl32: set_d(rd, std::format("{} >> {}", d(rt), sa + 32)); break;
        case Dsra32: set_d(rd, std::format("static_cast<std::uint64_t>({} >> {})", sd(rt), sa + 32)); break;
''', r'''        case Dsrl32:
            if (sa) set_w(rd, std::format("static_cast<std::uint32_t>({} >> {})", d(rt), sa + 32));
            else set_d(rd, std::format("{} >> {}", d(rt), sa + 32));
            break;
        case Dsra32: set_w(rd, std::format("static_cast<std::uint32_t>({} >> {})", sd(rt), sa + 32)); break;
'''),
        (r'''        case Slti: set_d(rt, std::format("{} < {}ll ? 1ull : 0ull", sd(rs), i.simm())); break;
        case Sltiu: set_d(rt, std::format("{} < static_cast<std::uint64_t>({}ll) ? 1ull : 0ull", d(rs), i.simm())); break;
        case Andi: set_d(rt, std::format("{} & {}", d(rs), uimm(i))); break;
        case Ori: set_d(rt, std::format("{} | {}", d(rs), uimm(i))); break;
        case Xori: set_d(rt, std::format("{} ^ {}", d(rs), uimm(i))); break;
''', r'''        case Slti:
            if (cn(rs)) set_w(rt, std::format("static_cast<std::int32_t>({}) < {} ? 1u : 0u", w(rs), i.simm()));
            else set_w(rt, std::format("{} < {}ll ? 1u : 0u", sd(rs), i.simm()));
            break;
        case Sltiu:
            if (cn(rs)) set_w(rt, std::format("{} < static_cast<std::uint32_t>({}) ? 1u : 0u", w(rs), i.simm()));
            else set_w(rt, std::format("{} < static_cast<std::uint64_t>({}ll) ? 1u : 0u", d(rs), i.simm()));
            break;
        case Andi: set_w(rt, std::format("{} & {}", w(rs), uimm32(i))); break;
        case Ori: if (cn(rs)) set_w(rt, std::format("{} | {}", w(rs), uimm32(i))); else set_d(rt, std::format("{} | {}", d(rs), uimm(i))); break;
        case Xori: if (cn(rs)) set_w(rt, std::format("{} ^ {}", w(rs), uimm32(i))); else set_d(rt, std::format("{} ^ {}", d(rs), uimm(i))); break;
'''),
        (r'''            if (const auto e = field(i)) line(std::format("c.r[{}].sd[0] = static_cast<std::int32_t>({} << 16);", rt, *e));
            else line(std::format("c.r[{}].sd[0] = static_cast<std::int32_t>({});", rt, hex(i.uimm() << 16)));
''', r'''            if (const auto e = field(i)) set_w(rt, std::format("({} << 16)", *e));
            else set_w(rt, hex(i.uimm() << 16));
'''),
        (r'''line(std::format("c.r[{}].sd[0] = static_cast<std::int8_t>(ld8({}));", rt, address(i)));''',
         r'''set_w(rt, std::format("static_cast<std::uint32_t>(static_cast<std::int8_t>(ld8({})))", address(i)));'''),
        (r'''line(std::format("c.r[{}].d[0] = ld8({});", rt, address(i)));''', r'''set_w(rt, std::format("ld8({})", address(i)));'''),
        (r'''line(std::format("c.r[{}].sd[0] = static_cast<std::int16_t>(ld16({}));", rt, address(i)));''',
         r'''set_w(rt, std::format("static_cast<std::uint32_t>(static_cast<std::int16_t>(ld16({})))", address(i)));'''),
        (r'''line(std::format("c.r[{}].d[0] = ld16({});", rt, address(i)));''', r'''set_w(rt, std::format("ld16({})", address(i)));'''),
        (r'''line(std::format("c.r[{}].d[0] = ld32({});", rt, address(i)));''', r'''set_d(rt, std::format("ld32({})", address(i)));'''),
        (r'''line(std::format("c.r[{}].d[0] = ld64({});", rt, address(i)));''', r'''set_d(rt, std::format("ld64({})", address(i)));'''),
        (r'''        case Sd: line(std::format("st64({}, {});", address(i), d(rt))); break;
''', r'''        case Sd:
            line(std::format("st64({}, {});", address(i),
                             cn(rt) ? std::format("static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>({})))", w(rt)) : d(rt)));
            break;
'''),
        (r'''    std::string *capture_{};
''', r'''    std::string *capture_{};
    Ctx ctx_;   // ps2run_3ds_patch.py: upper-word facts at the instruction being written
    std::vector<Ctx> nodes_;
    bool narrow_ = false;
    std::uint32_t kills_ = 0, canon_set_ = 0;
'''),
    ],
}


# The last edits for their files: small loops with no calls keep the guest registers they use in locals, so the
# compiler holds them in ARM registers for the whole loop (promote_loops; EERECOMP_NOLOOPS=1 turns it off and
# EERECOMP_LOOPS=a:b limits it to some loops, for bisecting). The cycle budget can also live in a local
# (local_budget), but only with EERECOMP_LOCALBUD=1: it saves about 1% of console time and costs 2.8 MB of code.
LOOP_EDITS = {
    'recomp/codegen.cpp': [
        (r'''#include <filesystem>
#include <format>
''', r'''#include <filesystem>
#include <functional>
#include <cctype>
#include <cstring>
#include <format>
'''),
        (r'''struct UnitRow {''', r'''// ps2run_3ds_patch.py: small loops with no calls (marked by the generator with //@LOOP_BEGIN and //@LOOP_END) keep the
// guest registers they use in locals, so the compiler can hold them in ARM registers for the whole loop. The locals are
// loaded before the loop and written back at every exit and before a preempt check that gives up the thread. A loop
// whose text calls anything that may read the registers, or uses a register in a form this does not know, stays as it
// was (for that register, or entirely). EERECOMP_NOLOOPS=1 turns it off.
std::string promote_loops(std::string s) {
    const std::string begin_tag = "//@LOOP_BEGIN ", end_tag = "//@LOOP_END ";
    std::string decls;
    int k = 0;
    for (std::size_t b = s.find(begin_tag); b != std::string::npos; b = s.find(begin_tag, b)) {
        const std::size_t bl = s.find('\n', b);
        const std::string lab = s.substr(b + begin_tag.size(), bl - b - begin_tag.size());
        const std::size_t e = s.find(end_tag + lab + "\n", bl);
        if (e == std::string::npos) throw std::runtime_error("promote_loops: unmatched " + lab);
        const std::size_t el = e + end_tag.size() + lab.size() + 1;
        std::string body = s.substr(bl + 1, e - bl - 1);
        const auto keep = [&] { s.replace(b, el - b, body); b += body.size(); };
        // calls that may read guest registers, and anything else this does not handle
        bool ok = body.find("goto dispatch_entry") == std::string::npos && body.find('#') == std::string::npos &&
                  body.find("stop(") == std::string::npos && body.find("probe(") == std::string::npos;
        for (std::size_t q = body.find("(c"); ok && q != std::string::npos; q = body.find("(c", q + 1)) {
            if (q + 2 >= body.size() || (body[q + 2] != ',' && body[q + 2] != ')')) continue;
            std::size_t n = q;
            while (n > 0 && (std::isalnum(static_cast<unsigned char>(body[n - 1])) || body[n - 1] == '_' || body[n - 1] == ':')) --n;
            const std::string name = body.substr(n, q - n);
            const bool tail = n >= 7 && body.compare(n - 7, 7, "return ") == 0;
            static const std::set<std::string> plain = {"preempt", "fpu_compare", "fpu_div", "mfc0", "mtc0", "cfc2", "ctc2", "cpcond0"};   // never touch guest registers
            if (!tail && !plain.count(name)) ok = false;
        }
        for (std::size_t q = body.find("return;"); ok && q != std::string::npos; q = body.find("return;", q + 1))
            if (q < 9 || body.compare(q - 9, 9, "kUnwind; ") != 0) ok = false;
        // registers and the forms they are used in
        std::uint32_t used = 0, pinned = 0, written = 0;
        for (std::size_t q = body.find("c.r["); ok && q != std::string::npos; q = body.find("c.r[", q + 1)) {
            const std::size_t close = body.find(']', q);
            const std::uint32_t r = static_cast<std::uint32_t>(std::stoul(body.substr(q + 4, close - q - 4)));
            used |= 1u << r;
            const std::string rest = body.substr(close + 1, 8);
            const bool typed = rest.starts_with(".w[0]") || rest.starts_with(".d[0]") || rest.starts_with(".sd[0]");
            const bool set = q >= 6 && body.compare(q - 6, 6, "set32(") == 0;
            if (!typed && !set) pinned |= 1u << r;
            const std::size_t after = typed ? body.find(']', close + 1) + 1 : close + 1;
            if (set || (typed && body.compare(after, 3, " = ") == 0)) written |= 1u << r;
        }
        const std::uint32_t promo = used & ~pinned & ~1u;
        static long seen_loops = 0;   // EERECOMP_LOOPS=a:b promotes only loops a..b-1 in generation order (for bisecting)
        const long n = seen_loops++;
        if (const char *range = std::getenv("EERECOMP_LOOPS")) {
            const long lo = std::strtol(range, nullptr, 10), hi = std::strtol(std::strchr(range, ':') + 1, nullptr, 10);
            if (n < lo || n >= hi) ok = false;
        }
        if (!ok || !promo) { keep(); continue; }
        const auto local = [&](std::uint32_t r) { return std::format("l{}_{}", k, r); };
        // the end of an expression that starts at i: the next ';' or unmatched ')' outside parentheses
        const auto expr_end = [](const std::string &t, std::size_t i) {
            int open = 0;
            for (; i < t.size(); ++i) {
                const char ch = t[i];
                if (ch == '(') ++open;
                else if (ch == ')') { if (open == 0) return i; --open; }
                else if (ch == ';' && open == 0) return i;
            }
            return i;
        };
        std::function<std::string(const std::string &)> rewrite = [&](const std::string &t) {
            std::string out;
            std::size_t cur = 0;
            for (std::size_t q = t.find("c.r["); q != std::string::npos; q = t.find("c.r[", cur)) {
                const std::size_t close = t.find(']', q);
                const std::uint32_t r = static_cast<std::uint32_t>(std::stoul(t.substr(q + 4, close - q - 4)));
                if (!(promo >> r & 1)) { out.append(t, cur, close + 1 - cur); cur = close + 1; continue; }
                const bool set = q >= 6 && q - 6 >= cur && t.compare(q - 6, 6, "set32(") == 0;
                if (set) {   // set32(c.r[N], X)
                    const std::size_t x = close + 3, xe = expr_end(t, x);
                    out.append(t, cur, q - 6 - cur);
                    out += local(r) + " = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(" + rewrite(t.substr(x, xe - x)) + ")))";
                    cur = xe + 1;
                    continue;
                }
                const std::size_t dot = close + 1, fe = t.find(']', dot) + 1;
                const std::string field = t.substr(dot + 1, t.find('[', dot) - dot - 1);   // w, d or sd
                out.append(t, cur, q - cur);
                if (t.compare(fe, 3, " = ") == 0) {
                    const std::size_t x = fe + 3, xe = expr_end(t, x);
                    const std::string v = rewrite(t.substr(x, xe - x));
                    if (field == "d") out += local(r) + " = " + v;
                    else if (field == "sd") out += local(r) + " = static_cast<std::uint64_t>(" + v + ")";
                    else out += local(r) + " = (" + local(r) + " & 0xFFFFFFFF00000000ull) | static_cast<std::uint32_t>(" + v + ")";
                    cur = xe;
                } else {
                    if (field == "d") out += local(r);
                    else if (field == "sd") out += "static_cast<std::int64_t>(" + local(r) + ")";
                    else out += "static_cast<std::uint32_t>(" + local(r) + ")";
                    cur = fe;
                }
            }
            out.append(t, cur);
            return out;
        };
        body = rewrite(body);
        std::string wb, wbx, rl, load;
        for (std::uint32_t r = 1; r < 32; ++r) {
            if (!(promo >> r & 1)) continue;
            decls += "    std::uint64_t " + local(r) + ";\n";
            const std::string from = std::format("c.r[{}].d[0]", r);
            load += local(r) + " = " + from + "; ";
            rl += (rl.empty() ? "" : ", ") + local(r) + " = " + from;
            if (written >> r & 1) {
                wb += from + " = " + local(r) + "; ";
                wbx += (wbx.empty() ? "" : ", ") + from + " = " + local(r);
            }
        }
        // labels of the loop: branches to anything else leave it
        std::set<std::string> inside;
        for (std::size_t q = body.find("\nL_"); q != std::string::npos; q = body.find("\nL_", q + 1)) inside.insert(body.substr(q + 1, 10));
        std::string out;
        std::size_t cur = 0;
        for (std::size_t q = body.find("goto L_"); q != std::string::npos; q = body.find("goto L_", cur)) {
            const std::string target = body.substr(q + 5, 10);
            out.append(body, cur, q - cur);
            if (target == lab) out += "goto " + lab + "_in;";
            else if (inside.count(target)) out += "goto " + target + ";";
            else out += "{ " + wb + "goto " + target + "; }";
            cur = q + 16;
        }
        out.append(body, cur);
        body.swap(out);
        const auto replace_all = [&](const std::string &from, const std::string &to) {
            for (std::size_t q = body.find(from); q != std::string::npos; q = body.find(from, q + to.size())) body.replace(q, from.size(), to);
        };
        replace_all("[[clang::musttail]] return", wb + "[[clang::musttail]] return");
        replace_all("c.budget < 0 && preempt(c)", "c.budget < 0 && (" + (wbx.empty() ? "" : wbx + ", ") + "preempt(c) || (" + rl + ", false))");
        body = "    " + load + "\n" + lab + "_in:\n" + body + "    " + wb + "\n";
        std::size_t start = b;
        keep();
        // jumps from outside to a label inside the loop (other than its top, where the loads are) load the locals first
        for (const std::string &y : inside) {
            const std::string from = "goto " + y + ";", to = "{ " + load + "goto " + y + "; }";
            for (std::size_t q = s.find(from); q != std::string::npos; q = s.find(from, q + 1)) {
                if (q >= start && q < b) { q = b - 1; continue; }
                s.replace(q, from.size(), to);
                if (q < start) { start += to.size() - from.size(); b += to.size() - from.size(); }
                q += to.size() - 1;
            }
        }
        ++k;
    }
    if (!decls.empty()) {
        const std::string head = "(Cpu &c, std::uint32_t entry) {\n";
        const std::size_t at = s.find(head);
        if (at == std::string::npos) throw std::runtime_error("promote_loops: no unit function");
        s.insert(at + head.size(), decls);
    }
    return s;
}

// ps2run_3ds_patch.py: generated code keeps the cycle budget in a local, bud, so the compiler can keep it in a
// register instead of loading and storing c.budget in every block. It is written back before anything that may read
// or change it (calls, returns, preempt, syscalls, hardware access through the slow memory paths) and read again after.
// Off unless EERECOMP_LOCALBUD=1: it saves about 1% of console time but the write-backs on the slow paths add 2.8 MB
// of code, nearly all the memory the 3DS has left.
std::string local_budget(std::string s) {
    if (!std::getenv("EERECOMP_LOCALBUD")) return s;
    const auto replace_all = [&](const std::string &from, const std::string &to) {
        std::string r;
        r.reserve(s.size() + s.size() / 8);
        std::size_t p = 0;
        for (std::size_t q; (q = s.find(from, p)) != std::string::npos; p = q + from.size()) r.append(s, p, q - p).append(to);
        r.append(s, p);
        s.swap(r);
    };
    replace_all("c.budget", "bud");
    replace_all("[[clang::musttail]] return ", "c.budget = bud; [[clang::musttail]] return ");
    replace_all("return;", "{ c.budget = bud; return; }");
    static const std::set<std::string> plain = {"fpu_compare", "fpu_div", "ctc1", "cfc1"};   // never touch the budget
    static const std::set<std::string> memory = {"ld8", "ld16", "ld32", "ld64", "ld128", "st8", "st16", "st32", "st64", "st128",
                                                 "lwl", "lwr", "ldl", "ldr", "swl", "swr", "sdl", "sdr"};
    const auto ident = [](char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == ':'; };
    std::string r;
    r.reserve(s.size() + s.size() / 4);
    std::size_t p = 0;
    for (std::size_t q = s.find('('); q != std::string::npos; q = s.find('(', q + 1)) {
        std::size_t b = q;
        while (b > p && ident(s[b - 1])) --b;
        if (b == q) continue;
        const std::string name = s.substr(b, q - b);
        if (memory.count(name)) {
            r.append(s, p, b - p).append("bud_").append(name).append("(c, bud, ");
            p = q + 1;
        } else if ((s.compare(q, 3, "(c,") == 0 || s.compare(q, 3, "(c)") == 0) && !plain.count(name) && !name.starts_with("mmi::") &&
                   !(b >= 7 && s.compare(b - 7, 7, "return ") == 0)) {
            r.append(s, p, b - p).append("bud_x(c, bud, ").append(name);
            p = q + 2;
        }
    }
    r.append(s, p);
    const std::string head = "(Cpu &c, std::uint32_t entry) {\n";
    const std::size_t at = r.find(head);
    if (at == std::string::npos) throw std::runtime_error("local_budget: no unit function");
    r.insert(at + head.size(), "    std::int32_t bud = c.budget;\n");
    return r;
}

struct UnitRow {'''),
        (r'''        write_if_changed(fs::path(options_.output) / std::format("{}.cpp", unit_name(u)), out_.str());''', r'''        write_if_changed(fs::path(options_.output) / std::format("{}.cpp", unit_name(u)), local_budget(promote_loops(out_.str())));'''),
        (r'''                out_ << label(a) << ":" << heat(a) << "\n";
            }''', r'''                out_ << label(a) << ":" << heat(a) << "\n";
                if (loops_.count(a)) out_ << "//@LOOP_BEGIN " << label(a) << "\n";
            }'''),
        (r'''                control(i, ds);
                falls = fl != Flow::Jump && fl != Flow::JumpRegister;''', r'''                control(i, ds);
                if (const auto lp = loop_ends_.find(a); lp != loop_ends_.end()) out_ << "//@LOOP_END " << label(lp->second) << "\n";
                falls = fl != Flow::Jump && fl != Flow::JumpRegister;'''),
        (r'''        analyse_narrow();
        out_.str({});''', r'''        analyse_narrow();
        find_loops();
        out_.str({});'''),
        (r'''    bool in_unit(std::uint32_t a) const { return a >= base_ && a < base_ + layout_.size(); }''', r'''    bool in_unit(std::uint32_t a) const { return a >= base_ && a < base_ + layout_.size(); }

    // ps2run_3ds_patch.py: loops for promote_loops: a backward branch and its target, at most 160 instructions, only
    // plain instructions and static branches inside, not overlapping (smallest first)
    void find_loops() {
        loops_.clear();
        loop_ends_.clear();
        if (std::getenv("EERECOMP_NOLOOPS") || relocatable_ || !variants_.empty()) return;
        const std::uint32_t end = base_ + layout_.size();
        std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;   // from, to
        for (std::uint32_t a = base_; a < end; a += 4) {
            if (!p_.is_code(a)) continue;
            const Flow fl = flow(at(a).op);
            if ((fl == Flow::Branch || fl == Flow::BranchLikely || fl == Flow::Jump) && !dynamic_jump(a)) edges.push_back({a, at(a).branch_target_of()});
        }
        std::vector<std::pair<std::uint32_t, std::uint32_t>> cands;   // target, branch
        static const std::uint32_t most = std::getenv("EERECOMP_LOOPMAX") ? std::strtoul(std::getenv("EERECOMP_LOOPMAX"), nullptr, 10) : 160;
        static const bool outer = std::getenv("EERECOMP_LOOPOUTER") != nullptr;   // largest first, so outer loops win
        for (const auto &[x, t] : edges)
            if (t <= x && in_unit(t) && x + 8 <= end && x + 8 - t <= 4 * most) cands.push_back({t, x});
        std::sort(cands.begin(), cands.end(), [](const auto &l, const auto &r) { return outer ? r.second - r.first < l.second - l.first : l.second - l.first < r.second - r.first; });
        std::vector<std::pair<std::uint32_t, std::uint32_t>> taken;
        for (const auto &[t, x] : cands) {
            const std::uint32_t stop = x + 8;
            // entries other than the top are fine: promote_loops loads the locals at every jump in from outside
            bool ok = labels_.count(t) != 0;
            for (const auto &[lo, hi] : taken) if (t < hi + 8 && lo < stop) ok = false;
            for (std::uint32_t a = t; ok && a < stop; a += 4) {
                if (!p_.is_code(a) || options_.natives.count(a) || options_.probes.count(a)) { ok = false; break; }
                const Flow fl = flow(at(a).op);
                if (fl != Flow::None && fl != Flow::Branch && fl != Flow::BranchLikely && fl != Flow::Jump) ok = false;
                if (fl != Flow::None && dynamic_jump(a)) ok = false;
            }
            if (!ok) continue;
            taken.push_back({t, x});
            loops_[t] = x;
            loop_ends_[x] = t;
        }
    }'''),
        (r'''    std::set<std::uint32_t> labels_;
    std::string *capture_{};''', r'''    std::set<std::uint32_t> labels_;
    std::map<std::uint32_t, std::uint32_t> loops_, loop_ends_;   // ps2run_3ds_patch.py: loop start -> backward branch, and back
    std::string *capture_{};'''),
    ],
    'runtime/include/ps2/memory.hpp': [
        (r'''#include <cstring>
''', r'''#include <cstring>
#include <type_traits>
'''),
        (r'''inline void sdr(const Reg128 &rt, std::uint32_t a) {
    const std::uint32_t shift = (a & 7u) * 8u;
    const std::uint64_t mem = ld64(a & ~7u);
    st64(a & ~7u, (rt.d[0] << shift) | (mem & mask_below(shift)));
}
''', r'''inline void sdr(const Reg128 &rt, std::uint32_t a) {
    const std::uint32_t shift = (a & 7u) * 8u;
    const std::uint64_t mem = ld64(a & ~7u);
    st64(a & ~7u, (rt.d[0] << shift) | (mem & mask_below(shift)));
}

// ps2run_3ds_patch.py: generated code keeps the cycle budget in a local (see local_budget in recomp/codegen.cpp).
// bud_x writes it back around a call that may read or change it; the memory wrappers do so only on the slow path,
// which reaches hardware registers whose handlers read the time or shorten the slice.
template <class F, class... A>
[[gnu::always_inline]] inline decltype(auto) bud_x(Cpu &c, std::int32_t &b, F &&f, A &&...a) {
    c.budget = b;
    if constexpr (std::is_void_v<decltype(f(c, static_cast<A &&>(a)...))>) {
        f(c, static_cast<A &&>(a)...);
        b = c.budget;
    } else {
        auto v = f(c, static_cast<A &&>(a)...);
        b = c.budget;
        return v;
    }
}

#define PS2_BUD_LD(name, T) \
    [[gnu::always_inline]] inline T bud_##name(Cpu &c, std::int32_t &b, std::uint32_t a) { \
        if (a < kRamSize) [[likely]] return name(a); \
        c.budget = b; const T v = name(a); b = c.budget; return v; }
#define PS2_BUD_ST(name, T) \
    [[gnu::always_inline]] inline void bud_##name(Cpu &c, std::int32_t &b, std::uint32_t a, T v) { \
        if (a < kRamSize) [[likely]] { name(a, v); return; } \
        c.budget = b; name(a, v); b = c.budget; }
#define PS2_BUD_REG(name, R) \
    [[gnu::always_inline]] inline void bud_##name(Cpu &c, std::int32_t &b, R rt, std::uint32_t a) { \
        if (a < kRamSize) [[likely]] { name(rt, a); return; } \
        c.budget = b; name(rt, a); b = c.budget; }
PS2_BUD_LD(ld8, std::uint8_t)
PS2_BUD_LD(ld16, std::uint16_t)
PS2_BUD_LD(ld32, std::uint32_t)
PS2_BUD_LD(ld64, std::uint64_t)
PS2_BUD_ST(st8, std::uint8_t)
PS2_BUD_ST(st16, std::uint16_t)
PS2_BUD_ST(st32, std::uint32_t)
PS2_BUD_ST(st64, std::uint64_t)
PS2_BUD_ST(st128, const Reg128 &)
PS2_BUD_REG(lwl, Reg128 &)
PS2_BUD_REG(lwr, Reg128 &)
PS2_BUD_REG(ldl, Reg128 &)
PS2_BUD_REG(ldr, Reg128 &)
PS2_BUD_REG(swl, const Reg128 &)
PS2_BUD_REG(swr, const Reg128 &)
PS2_BUD_REG(sdl, const Reg128 &)
PS2_BUD_REG(sdr, const Reg128 &)
[[gnu::always_inline]] inline void bud_ld128(Cpu &c, std::int32_t &b, std::uint32_t a, Reg128 &out) {
    if (a < kRamSize) [[likely]] { ld128(a, out); return; }
    c.budget = b; ld128(a, out); b = c.budget; }
#undef PS2_BUD_LD
#undef PS2_BUD_ST
#undef PS2_BUD_REG
'''),
    ],
}


# Loads and stores through the guest stack pointer skip the range check: eerecomp rewrites ld32((sp + N)) into
# sp_ld32(sp, N), which masks the stack pointer into main RAM, so GCC folds the offset into the ARM load. The game's
# stacks stay in main RAM (checked over a cold boot, the lobby and battle). EERECOMP_SPCHECK=1 keeps the checks.
SP_EDITS = {
    'recomp/codegen.cpp': [
        (r'''struct UnitRow {''', r'''// ps2run_3ds_patch.py: loads and stores through the stack pointer skip the range check. The game's stacks are in main
// RAM (every one in a cold boot, the lobby and battle), so sp_ld32(sp, offset) and the others mask the stack pointer
// into RAM instead of testing the address, and GCC can fold the offset into the ARM load. EERECOMP_SPCHECK=1 keeps the
// checks.
std::string stack_access(std::string s) {
    if (std::getenv("EERECOMP_SPCHECK")) return s;
    static const char *const kNames[] = {"ld8", "ld16", "ld32", "ld64", "ld128", "st8", "st16", "st32", "st64", "st128"};
    std::string out;
    out.reserve(s.size() + s.size() / 16);
    std::size_t at = 0;
    for (std::size_t p = s.find("((", 0); p != std::string::npos; p = s.find("((", p + 1)) {
        if (p < at) continue;
        const std::size_t a = p + 1;   // the address: (c.r[29].w[0] + Nu), (c.r[29].w[0]) or (static_cast<std::uint32_t>(lK_29) + Nu)
        std::size_t base_end;
        if (!s.compare(a, 13, "(c.r[29].w[0]")) base_end = a + 13;
        else if (!s.compare(a, 29, "(static_cast<std::uint32_t>(l")) {
            std::size_t e = a + 29;
            while (e < s.size() && (std::isdigit(static_cast<unsigned char>(s[e])) || s[e] == '_')) ++e;
            if (s.compare(e - 3, 4, "_29)")) continue;
            base_end = e + 1;
        } else continue;
        std::size_t n = p;   // the function name in front
        while (n && (std::isalnum(static_cast<unsigned char>(s[n - 1])) || s[n - 1] == '_')) --n;
        const std::string name = s.substr(n, p - n);
        if (std::find(std::begin(kNames), std::end(kNames), name) == std::end(kNames)) continue;
        out.append(s, at, n - at);
        out += "sp_" + name + "(";
        if (name.ends_with("128")) {   // the address is masked to 16 bytes after the add: keep it whole
            at = a;
            continue;
        }
        std::string offset = "0u";
        std::size_t close = base_end;
        if (!s.compare(base_end, 3, " + ")) {
            std::size_t e = base_end + 3;
            while (e < s.size() && std::isdigit(static_cast<unsigned char>(s[e]))) ++e;
            if (e == base_end + 3 || s.compare(e, 2, "u)")) {   // not a plain offset: keep the checked access
                out.resize(out.size() - name.size() - 4);
                out += name + "(";
                at = a;
                continue;
            }
            offset = s.substr(base_end + 3, e + 1 - (base_end + 3));
            close = e + 1;
        }
        if (s[close] != ')') {
            out.resize(out.size() - name.size() - 4);
            out += name + "(";
            at = a;
            continue;
        }
        out += s.substr(a + 1, base_end - a - 1) + ", " + offset;
        at = close + 1;
    }
    out.append(s, at, std::string::npos);
    return out;
}

struct UnitRow {'''),
        (r'''local_budget(promote_loops(out_.str()))''', r'''stack_access(local_budget(promote_loops(out_.str())))'''),
    ],
    'runtime/include/ps2/memory.hpp': [
        (r'''#undef PS2_BUD_REG
''', r'''#undef PS2_BUD_REG

// ps2run_3ds_patch.py: loads and stores through the guest stack pointer (eerecomp's stack_access). The stack pointer is
// masked into main RAM instead of the address being tested, so the offset folds into the ARM load or store.
inline std::uint8_t *stack_at(std::uint32_t sp, std::uint32_t offset) { return g_ram + static_cast<std::uint32_t>((sp & (kRamSize - 1u)) + offset); }
#ifdef __3DS__
#define PS2_SP_WATCH(sp, offset, n, v)
#else
#define PS2_SP_WATCH(sp, offset, n, v) if ((sp) + (offset) + (n) - 4u - g_swlo < g_swlen + (n) - 4u) [[unlikely]] store_watch_hit((sp) + (offset), n, &(v))
#endif
inline std::uint8_t sp_ld8(std::uint32_t sp, std::uint32_t offset) { return *stack_at(sp, offset); }
inline std::uint16_t sp_ld16(std::uint32_t sp, std::uint32_t offset) { return load<std::uint16_t>(stack_at(sp, offset)); }
inline std::uint32_t sp_ld32(std::uint32_t sp, std::uint32_t offset) { return load<std::uint32_t>(stack_at(sp, offset)); }
inline std::uint64_t sp_ld64(std::uint32_t sp, std::uint32_t offset) { return load<std::uint64_t>(stack_at(sp, offset)); }
inline void sp_st8(std::uint32_t sp, std::uint32_t offset, std::uint8_t v) { *stack_at(sp, offset) = v; }
inline void sp_st16(std::uint32_t sp, std::uint32_t offset, std::uint16_t v) { store(stack_at(sp, offset), v); }
inline void sp_st32(std::uint32_t sp, std::uint32_t offset, std::uint32_t v) {
    PS2_SP_WATCH(sp, offset, 4u, v);
    store(stack_at(sp, offset), v);
}
inline void sp_st64(std::uint32_t sp, std::uint32_t offset, std::uint64_t v) {
    PS2_SP_WATCH(sp, offset, 8u, v);
    store(stack_at(sp, offset), v);
}
inline void sp_ld128(std::uint32_t a, Reg128 &out) { std::memcpy(&out, g_ram + (a & (kRamSize - 1u)), 16); }
inline void sp_st128(std::uint32_t a, const Reg128 &v) {
    PS2_SP_WATCH(a, 0u, 16u, v);
    std::memcpy(g_ram + (a & (kRamSize - 1u)), &v, 16);
}
#undef PS2_SP_WATCH
'''),
    ],
}


# Hot copies of busy game functions (tools/p3fes_eehot.py): a native whose name starts with hot_ is a second copy of the
# code that starts there, with the guest registers in locals. Every build calls it, so the native build can check that
# it does the same work to the cycle. -DPS2_NO_HOT leaves the calls out.
HOT_EDITS = {
    'recomp/codegen.cpp': [
        (r"""                if (native->second.starts_with("loop_")) line(std::format("{{ void native_{0}(Cpu &); native_{0}(c); }}", native->second));
                else line(std::format("{{ bool native_{0}(Cpu &); if (native_{0}(c)) {{ c.pc = c.r[31].w[0]; return; }} }}", native->second));
""", r"""                if (native->second.starts_with("hot_")) {
                    out_ << "#endif\n#ifndef PS2_NO_HOT\n";
                    line(std::format("{{ bool {0}(Cpu &); if ({0}(c)) return; }}", native->second));
                } else if (native->second.starts_with("loop_")) line(std::format("{{ void native_{0}(Cpu &); native_{0}(c); }}", native->second));
                else line(std::format("{{ bool native_{0}(Cpu &); if (native_{0}(c)) {{ c.pc = c.r[31].w[0]; return; }} }}", native->second));
"""),
    ],
    'recomp/main.cpp': [
        (r"""        std::printf("code %zu instructions (%zu KiB), functions %zu, entries %zu, rejected seeds %zu\n",
                    code, code * 4 / 1024, program.functions.size(), program.entries.size(), program.rejected.size());
""", r"""        std::printf("code %zu instructions (%zu KiB), functions %zu, entries %zu, rejected seeds %zu\n",
                    code, code * 4 / 1024, program.functions.size(), program.entries.size(), program.rejected.size());
        if (const char *path = getenv("EERECOMP_FUNCTIONS")) {   // ps2run_3ds_patch.py: where functions start, for tools/p3fes_eehot.py
            if (std::FILE *f = std::fopen(path, "w")) {
                for (const std::uint32_t a : program.functions) std::fprintf(f, "%08X\n", a);
                std::fclose(f);
            }
        }
"""),
    ],
}


# Unchanging VIF and texture data by address, with the game side's command pass replayed (2026-11-10).
VIFADDR_EDITS = {
    'runtime/src/gs/queue.hpp': [
        (r"""extern bool g_quiet;         // ps2run_3ds_patch.py: nothing is drawn while this is set
""", r"""bool unchanging(const std::uint8_t *data, std::uint32_t bytes);   // ps2run_3ds_patch.py: both take a place in main PS2 memory
void rewritten(const std::uint8_t *data, std::uint32_t bytes);
extern bool g_quiet;         // ps2run_3ds_patch.py: nothing is drawn while this is set
"""),
    ],
    'runtime/src/gs/queue.cpp': [
        (r"""#include "ps2/system.hpp"
""", r"""#include "ps2/memory.hpp"
#include "ps2/system.hpp"
"""),
        (r"""bool g_ee_skip = false;
""", r"""bool g_ee_skip = false;

// ps2run_3ds_patch.py: model and texture data that the game sends every picture from the same place, unchanged, need not be copied to
// the VU thread, which can read PS2 memory itself. Copying it was the largest single cost on the game thread: about 230 cycles a
// cache line. A block counts as unchanging once the same bytes have been seen four times running, and is then checked again every
// 24 to 39 sends. The game also builds packets afresh for each picture, in buffers it uses again two pictures later, and in a still
// scene those can look unchanging too. So a 4 KB page of PS2 memory that was ever seen to change, or that the game filled from the
// scratchpad, never counts. What is left: if a block changes for the first time between two checks while the VU thread is more
// than a picture behind, that one picture is drawn from the newer bytes. Build with -DPS2_VIF_COPY to copy everything as before.
namespace {
std::uint32_t g_rewritten[kRamSize / 4096 / 32];
struct Unchanging {
    const std::uint8_t *data;
    std::uint32_t bytes, hash;
    std::uint8_t same, rest;
};
Unchanging g_unchanging[16384];
}

void rewritten(const std::uint8_t *data, std::uint32_t bytes) {
    const std::uint32_t first = static_cast<std::uint32_t>(data - g_ram) >> 12, last = static_cast<std::uint32_t>(data - g_ram + bytes - 1) >> 12;
    for (std::uint32_t p = first; p <= last && p < kRamSize / 4096; ++p) g_rewritten[p >> 5] |= 1u << (p & 31u);
}

bool unchanging(const std::uint8_t *data, std::uint32_t bytes) {
#ifdef PS2_VIF_COPY
    return false;
#else
    Unchanging &e = g_unchanging[(static_cast<std::uint32_t>(data - g_ram) >> 4) * 2654435761u >> 18];
    const bool known = e.data == data && e.bytes == bytes;
    if (known && e.rest) {
        --e.rest;
        return true;
    }
    const std::uint32_t first = static_cast<std::uint32_t>(data - g_ram) >> 12, last = static_cast<std::uint32_t>(data - g_ram + bytes - 1) >> 12;
    for (std::uint32_t p = first; p <= last; ++p)
        if (g_rewritten[p >> 5] & (1u << (p & 31u))) return false;
    std::uint32_t h = 2166136261u;
    for (std::uint32_t i = 0; i + 4 <= bytes; i += 4) {
        std::uint32_t w;
        std::memcpy(&w, data + i, 4);
        h = (h ^ w) * 16777619u;
    }
    if (known && h == e.hash) {
        if (e.same < 3) ++e.same;
        if (e.same >= 3) e.rest = static_cast<std::uint8_t>(24u + (h >> 28));
    } else {
        if (known) rewritten(data, bytes);
        e = Unchanging{data, bytes, h, 0, 0};
    }
    return false;
#endif
}
"""),
    ],
    'runtime/src/dmac.cpp': [
        (r"""#include "gs/gs.hpp"
""", r"""#include "gs/gs.hpp"
#include "gs/queue.hpp"
"""),
        (r"""        std::memcpy(target(at), g_spr + from, static_cast<std::size_t>(n) * 16);
""", r"""        std::memcpy(target(at), g_spr + from, static_cast<std::size_t>(n) * 16);
#ifdef PS2_PICA
        if (!(at & 0x80000000u)) gs::queue::rewritten(target(at), n * 16);   // ps2run_3ds_patch.py: the game builds its packets this way
#endif
"""),
    ],
    'runtime/src/gs/gif.cpp': [
        (r"""    if (path == 3 && qwc >= 256 && gs::queue::g_ee_skip && data >= g_ram && data + qwc * 16u <= g_ram + kRamSize) {
""", r"""    // A shown picture's goes the same way if the game has sent the same bytes from the same place several times running.
    if (path == 3 && data >= g_ram && data + qwc * 16u <= g_ram + kRamSize &&
        ((qwc >= 256 && gs::queue::g_ee_skip) || (qwc >= 16 && gs::queue::vu_active() && gs::queue::unchanging(data, qwc * 16u)))) {
"""),
    ],
    'runtime/src/vif.cpp': [
        (r"""Unit g_units[2];
""", r"""// ps2run_3ds_patch.py: for run_unchanging, what the game side's commands touched. g_side: something outside the unit's registers (a
// stop, the PATH3 mask, data for the GIF, or an unpack in mode 2, which adds to the row registers). g_wrote: which registers, as kW bits.
bool g_side = false;
std::uint32_t g_wrote = 0;
Unit g_units[2];
"""),
        (r"""    const std::uint32_t op = (cmd >> 24) & 0x7Fu, imm = cmd & 0xFFFFu, num = (cmd >> 16) & 0xFFu;
    u.code = cmd;
""", r"""    const std::uint32_t op = (cmd >> 24) & 0x7Fu, imm = cmd & 0xFFFFu, num = (cmd >> 16) & 0xFFu;
    if (is_shadow) {
        if ((cmd & 0x80000000u) || op == 0x06 || op == 0x50 || op == 0x51) g_side = true;
        switch (op) {
        case 0x01: g_wrote |= 1u; break;
        case 0x02: g_wrote |= 2u | 4u | 8u; break;
        case 0x03: g_wrote |= 16u; break;
        case 0x04: g_wrote |= 32u; break;
        case 0x05: g_wrote |= 64u; break;
        case 0x07: g_wrote |= 128u; break;
        case 0x14: case 0x15: case 0x17: g_wrote |= unit ? 256u | 512u | 4u | 8u : 512u; break;
        case 0x20: g_wrote |= 1024u; break;
        case 0x30: g_wrote |= 2048u; break;
        case 0x31: g_wrote |= 4096u; break;
        default:
            if ((op & 0x60u) == 0x60u) {
                if (u.mode == 2) g_side = true;
                g_wrote |= 8192u;
            }
            break;
        }
        g_wrote |= 16384u;
    }
    u.code = cmd;
"""),
        (r"""void keep_data(const std::uint32_t *words, std::size_t count) {
""", r"""// ps2run_3ds_patch.py: unchanging data (see unchanging() in gs/queue.cpp) is the same commands every picture, so their effect on this
// side's registers is the same too, given the registers they start from. That effect is kept and replayed, and the VU thread, which
// still runs every command, gets the whole transfer as one address. Two thirds of this game's commands come this way, and parsing them
// here cost more than copying them. What a transfer can read on this side is the cycle, the mode and the double-buffer registers, so
// those and the data are the key. Only transfers that end on a whole command, with nothing changed outside the registers, are kept.
enum : std::uint32_t { kWCycle = 1, kWOfst = 2, kWDbf = 4, kWTops = 8, kWBase = 16, kWItops = 32, kWMode = 64, kWMark = 128, kWTop = 256,
                       kWItop = 512, kWMask = 1024, kWRow = 2048, kWCol = 4096, kWNum = 8192, kWCode = 16384 };
struct Key {
    std::uint32_t cycle, mode, base, ofst, tops, itops, dbf;
    bool operator==(const Key &o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
};
Key key_of(const Unit &u) { return Key{u.cycle, u.mode, u.base, u.ofst, u.tops, u.itops, u.dbf ? 1u : 0u}; }
struct Replay {
    const std::uint8_t *data;
    std::uint32_t qwc, wrote;
    Key before;
    std::uint32_t cycle, ofst, tops, base, itops, mode, mark, top, itop, mask, row[4], col[4], num, code;
    bool dbf;
};
void record(Replay &r, const Unit &u) {
    r.cycle = u.cycle; r.ofst = u.ofst; r.tops = u.tops; r.base = u.base; r.itops = u.itops; r.mode = u.mode; r.mark = u.mark;
    r.top = u.top; r.itop = u.itop; r.mask = u.mask; r.num = u.num; r.code = u.code; r.dbf = u.dbf;
    std::memcpy(r.row, u.row, sizeof r.row);
    std::memcpy(r.col, u.col, sizeof r.col);
}
void replay(const Replay &r, Unit &u) {
    const std::uint32_t w = r.wrote;
    if (w & kWCycle) u.cycle = r.cycle;
    if (w & kWOfst) u.ofst = r.ofst;
    if (w & kWDbf) u.dbf = r.dbf;
    if (w & kWTops) u.tops = r.tops;
    if (w & kWBase) u.base = r.base;
    if (w & kWItops) u.itops = r.itops;
    if (w & kWMode) u.mode = r.mode;
    if (w & kWMark) u.mark = r.mark;
    if (w & kWTop) u.top = r.top;
    if (w & kWItop) u.itop = r.itop;
    if (w & kWMask) u.mask = r.mask;
    if (w & kWRow) std::memcpy(u.row, r.row, sizeof r.row);
    if (w & kWCol) std::memcpy(u.col, r.col, sizeof r.col);
    if (w & kWNum) u.num = r.num;
    if (w & kWCode) u.code = r.code;
}
constexpr std::uint32_t kReplays = 4096;
Replay *g_replays = nullptr;   // 512 KB, from the heap on first use
std::uint32_t replay_slot(const std::uint8_t *data, std::uint32_t qwc, const Key &k) {
    std::uint32_t h = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(data)) * 2654435761u ^ qwc;
    const auto *w = reinterpret_cast<const std::uint32_t *>(&k);
    for (std::size_t i = 0; i < sizeof k / 4; ++i) h = (h ^ w[i]) * 16777619u;
    return (h ^ (h >> 15)) & (kReplays - 1);
}

void send_address(const std::uint32_t *words, std::size_t count) {
    if (g_kept_words) send_kept();
    auto *record = reinterpret_cast<std::uint32_t *>(gs::queue::vu().extend(gs::queue::kVif1Records, 8));
    record[0] = static_cast<std::uint32_t>(count) | 0x80000000u;
    record[1] = static_cast<std::uint32_t>(reinterpret_cast<const std::uint8_t *>(words) - g_ram);
}

// Returns false if the transfer does not qualify, and the caller goes on as before.
bool run_unchanging(int unit, Unit &u, const std::uint8_t *data, std::uint32_t qwc) {
    if (u.stalled || !u.pending.empty() || u.dropping || u.passing || qwc < 4 || data < g_ram || data + qwc * 16u > g_ram + kRamSize ||
        !gs::queue::unchanging(data, qwc * 16u))
        return false;
    if (!g_replays) g_replays = new Replay[kReplays]();
    const auto *words = reinterpret_cast<const std::uint32_t *>(data);
    const std::size_t count = static_cast<std::size_t>(qwc) * 4;
    const Key before = key_of(u);
    Replay &slot = g_replays[replay_slot(data, qwc, before)];
    send_address(words, count);
    if (slot.data == data && slot.qwc == qwc && slot.before == before) {
        replay(slot, u);
        return true;
    }
    g_side = false;
    g_wrote = kWCode;   // a transfer of only padding still clears the code register
    const std::size_t pos = run(unit, u, false, true, words, count);
    if (pos < count) {
        u.pending.assign(words + pos, words + count);
    } else if (!g_side && !u.stalled) {
        slot.data = data;
        slot.qwc = qwc;
        slot.wrote = g_wrote;
        slot.before = before;
        record(slot, u);
    }
    return true;
}

void keep_data(const std::uint32_t *words, std::size_t count) {
"""),
        (r"""    if (is_shadow && (gs::queue::g_ee_skip || u.dropping || u.passing)) {
""", r"""    if (is_shadow && run_unchanging(unit, u, data, qwc)) return;   // ps2run_3ds_patch.py
    bool by_address = is_shadow && (gs::queue::g_ee_skip || u.dropping || u.passing);
    if (is_shadow && !by_address && qwc >= 8 && !u.stalled && u.pending.size() == 1 && data >= g_ram &&
        data + qwc * 16u <= g_ram + kRamSize && gs::queue::unchanging(data, qwc * 16u)) {
        // An unpack command at the end of the last transfer, which was copied across, and this unchanging transfer its data. The
        // command runs here now, and the VU thread, which also holds it, gets the data as an address.
        const std::uint32_t cmd = u.pending.front(), need = data_words(u, cmd);
        if ((cmd & 0xE0000000u) == 0x60000000u && u.mode != 2 && need && need <= static_cast<std::size_t>(qwc) * 4) {
            execute(unit, u, false, true, cmd, reinterpret_cast<const std::uint32_t *>(data));
            u.pending.clear();
            u.passing = need;
            by_address = true;
        }
    }
    if (by_address) {
"""),
    ],
}


# Unchanging VIF transfers keep the records run_skipped sends (commands copied, long unpack data by address) and send them again,
# instead of the whole transfer as one address, which let the VU thread read a buffer the game had already rebuilt and lose its place
# in the commands (2026-11-10).
VIFREC_EDITS = {
    'runtime/src/vif.cpp': [
        (r"""
void send_kept() {
""", r"""
// ps2run_3ds_patch.py: while run_unchanging parses a transfer for the first time, the records it sends are also written here, so the
// next time the same records can go again without parsing. g_capture_left runs out (and the transfer is not kept) if they do not fit.
std::uint32_t *g_capture = nullptr;
std::uint32_t g_capture_words = 0, g_capture_left = 0;
bool g_capture_full = false;
void capture(const std::uint32_t *record, std::uint32_t words) {
    if (!g_capture) return;
    if (words > g_capture_left) {
        g_capture_full = true;
        return;
    }
    std::memcpy(g_capture + g_capture_words, record, words * 4u);
    g_capture_words += words;
    g_capture_left -= words;
}

void send_kept() {
"""),
        (r"""        std::memcpy(record + 1, words, n * 4u);
        words += n;
""", r"""        std::memcpy(record + 1, words, n * 4u);
        capture(record, 1u + n);
        words += n;
"""),
        (r"""    std::uint32_t cycle, ofst, tops, base, itops, mode, mark, top, itop, mask, row[4], col[4], num, code;
    bool dbf;
""", r"""    std::uint32_t cycle, ofst, tops, base, itops, mode, mark, top, itop, mask, row[4], col[4], num, code;
    std::uint32_t records, words, epoch;   // what to send the VU thread: g_records[records], words long, if epoch is still g_epoch
    bool dbf;
"""),
        (r"""
void send_address(const std::uint32_t *words, std::size_t count) {
    if (g_kept_words) send_kept();
    auto *record = reinterpret_cast<std::uint32_t *>(gs::queue::vu().extend(gs::queue::kVif1Records, 8));
    record[0] = static_cast<std::uint32_t>(count) | 0x80000000u;
    record[1] = static_cast<std::uint32_t>(reinterpret_cast<const std::uint8_t *>(words) - g_ram);
}

// Returns false if the transfer does not qualify, and the caller goes on as before.
bool run_unchanging(int unit, Unit &u, const std::uint8_t *data, std::uint32_t qwc) {
    if (u.stalled || !u.pending.empty() || u.dropping || u.passing || qwc < 4 || data < g_ram || data + qwc * 16u > g_ram + kRamSize ||
        !gs::queue::unchanging(data, qwc * 16u))
        return false;
    if (!g_replays) g_replays = new Replay[kReplays]();
    const auto *words = reinterpret_cast<const std::uint32_t *>(data);
    const std::size_t count = static_cast<std::size_t>(qwc) * 4;
    const Key before = key_of(u);
    Replay &slot = g_replays[replay_slot(data, qwc, before)];
    send_address(words, count);
    if (slot.data == data && slot.qwc == qwc && slot.before == before) {
        replay(slot, u);
        return true;
    }
    g_side = false;
    g_wrote = kWCode;   // a transfer of only padding still clears the code register
    const std::size_t pos = run(unit, u, false, true, words, count);
    if (pos < count) {
        u.pending.assign(words + pos, words + count);
    } else if (!g_side && !u.stalled) {
        slot.data = data;
        slot.qwc = qwc;
        slot.wrote = g_wrote;
        slot.before = before;
        record(slot, u);
""", r"""
// The records each kept transfer sends, one after another; when full it starts again from the front and every kept transfer is
// parsed once more. 256 KB, from the heap on first use.
constexpr std::uint32_t kRecordWords = 65536, kKeptRecordWords = 4096;
std::uint32_t *g_records = nullptr;
std::uint32_t g_records_used = 0, g_epoch = 1;

std::size_t run_skipped(int unit, Unit &u, const std::uint32_t *words, std::size_t count);

// Returns false if the transfer does not qualify, and the caller goes on as before.
//
// The VU thread must get the same commands as this side: it reads PS2 memory later, and the game may have built something else in the
// same place by then (it rewrites some buffers every picture). So, as in run_skipped, commands are copied and only long unpack data
// goes as an address. The first time, run_skipped parses the transfer and the records it sends are kept; later the same records go
// again. Sending the whole transfer as one address instead let the VU thread read a rebuilt buffer, lose its place in the commands,
// stop at data it took for an interrupt command and hold every later transfer until memory ran out.
bool run_unchanging(int unit, Unit &u, const std::uint8_t *data, std::uint32_t qwc) {
    if (u.stalled || !u.pending.empty() || u.dropping || u.passing || qwc < 4 || data < g_ram || data + qwc * 16u > g_ram + kRamSize ||
        !gs::queue::unchanging(data, qwc * 16u))
        return false;
    if (!g_replays) {
        g_replays = new Replay[kReplays]();
        g_records = new std::uint32_t[kRecordWords];
    }
    const auto *words = reinterpret_cast<const std::uint32_t *>(data);
    const std::size_t count = static_cast<std::size_t>(qwc) * 4;
    const Key before = key_of(u);
    Replay &slot = g_replays[replay_slot(data, qwc, before)];
    if (g_kept_words) send_kept();
    if (slot.data == data && slot.qwc == qwc && slot.before == before && slot.epoch == g_epoch) {
        std::uint32_t at = slot.records, left = slot.words;
        while (left) {   // in pieces no bigger than send_kept's
            std::uint32_t n = 0;
            while (n < left) {
                const std::uint32_t r = g_records[at + n], size = r >> 31 ? 2u : 1u + r;
                if (n && n + size > 1u + kCopiedWords) break;
                n += size;
            }
            std::memcpy(gs::queue::vu().extend(gs::queue::kVif1Records, n * 4u), g_records + at, n * 4u);
            at += n;
            left -= n;
        }
        replay(slot, u);
        return true;
    }
    // Records for a skipped picture leave out data that picture does not need, so they are not kept.
    const bool keep_records = !gs::queue::g_ee_skip;
    if (keep_records) {
        if (kRecordWords - g_records_used < kKeptRecordWords) {
            g_records_used = 0;
            ++g_epoch;
        }
        g_capture = g_records + g_records_used;
        g_capture_words = 0;
        g_capture_left = kKeptRecordWords;
        g_capture_full = false;
    }
    g_side = false;
    g_wrote = kWCode;   // a transfer of only padding still clears the code register
    const std::size_t pos = run_skipped(unit, u, words, count);
    g_capture = nullptr;
    if (pos < count) {
        u.pending.assign(words + pos, words + count);
    } else if (keep_records && !g_capture_full && !g_side && !u.stalled && !u.dropping && !u.passing) {
        slot.data = data;
        slot.qwc = qwc;
        slot.wrote = g_wrote;
        slot.before = before;
        slot.records = g_records_used;
        slot.words = g_capture_words;
        slot.epoch = g_epoch;
        g_records_used += g_capture_words;
        record(slot, u);
"""),
        (r"""    record[1] = static_cast<std::uint32_t>(at - g_ram);
}
""", r"""    record[1] = static_cast<std::uint32_t>(at - g_ram);
    capture(record, 2);
}
"""),
    ],
}


# The I/O processor thread may run ahead of the game's clock up to the next vblank (port3ds/iop3ds.cpp, iop_horizon).
IOPAHEAD_EDITS = {
    'runtime/include/ps2/system.hpp': [
        (r"""void iop_edge(bool start);
void iop_wait(bool (*enough)() = nullptr);   // until it is level with the game, or enough() says so""", r"""void iop_edge(bool start);
void iop_horizon(std::uint32_t ee_cycles);   // ps2run_3ds_patch.py: the I/O processor may run this far past the last edge
void iop_wait(bool (*enough)() = nullptr);   // until it is level with the game, or enough() says so"""),
    ],
    'runtime/src/system.cpp': [
        (r"""        iop_edge(start);
        return;
""", r"""        iop_edge(start);
        if (!start) iop_horizon(static_cast<std::uint32_t>(frame_cycles() - hblank_cycles() * 22u));   // up to the next vblank
        return;
"""),
    ],
}


# Unchanging data sent as an address is read by the VU thread when it gets to it. Its queue then held twenty pictures and more, and on
# entering an area the game had put the new area's models where the old ones were, so the VU thread drew garbage, asked for a 10 MB
# copy buffer and stopped. The game now waits at a vblank while the VU thread is more than ten behind (counted as a full queue, which
# the picture skipping goes by); between full checks a few words of each block are still compared; kept VIF records go with the
# bytes' hash; and a GS copy of more than a million pixels is left out.
STALE_EDITS = {
    'runtime/src/gs/queue.hpp': [
        (r"""void rewritten(const std::uint8_t *data, std::uint32_t bytes);
extern bool g_quiet;         // ps2run_3ds_patch.py: nothing is drawn while this is set
""", r"""void rewritten(const std::uint8_t *data, std::uint32_t bytes);
extern std::uint32_t g_unchanging_hash;   // ps2run_3ds_patch.py: the bytes' hash when unchanging() last said yes
extern bool g_quiet;         // ps2run_3ds_patch.py: nothing is drawn while this is set
"""),
    ],
    'runtime/src/gs/queue.cpp': [
        (r"""// than a picture behind, that one picture is drawn from the newer bytes. Build with -DPS2_VIF_COPY to copy everything as before.
namespace {
""", r"""// than a picture behind, that one picture is drawn from the newer bytes. Build with -DPS2_VIF_COPY to copy everything as before.
//
// Between checks a few words of the block are still compared every time (sample()): on entering an area the game puts the new area's
// models where the last one's were, and without that the VU thread read the new bytes as the old commands until the next check, drew
// garbage and once asked for a 10 MB copy buffer.
namespace {
"""),
        (r"""    std::uint32_t bytes, hash;
""", r"""    std::uint32_t bytes, hash, sample;
"""),
        (r"""Unchanging g_unchanging[16384];
}
""", r"""Unchanging g_unchanging[16384];
}
std::uint32_t g_unchanging_hash = 0;
namespace {

// Four words: the first, the last, and two between (blocks are whole quadwords, at least 16 bytes).
std::uint32_t sample(const std::uint8_t *data, std::uint32_t bytes) {
    const std::uint32_t q = (bytes >> 2) & ~3u;
    std::uint32_t a, b, c, d;
    std::memcpy(&a, data, 4);
    std::memcpy(&b, data + q, 4);
    std::memcpy(&c, data + q * 2 + 4, 4);
    std::memcpy(&d, data + bytes - 4, 4);
    return a ^ (b * 3u) ^ (c * 5u) ^ (d * 7u);
}
}
"""),
        (r"""    if (known && e.rest) {
""", r"""    if (known && e.rest && sample(data, bytes) == e.sample) {
"""),
        (r"""        --e.rest;
        return true;
""", r"""        --e.rest;
        g_unchanging_hash = e.hash;
        return true;
"""),
        (r"""        if (e.same >= 3) e.rest = static_cast<std::uint8_t>(24u + (h >> 28));
""", r"""        if (e.same >= 3) {
            e.rest = static_cast<std::uint8_t>(24u + (h >> 28));
            e.sample = sample(data, bytes);
        }
"""),
        (r"""        e = Unchanging{data, bytes, h, 0, 0};
""", r"""        e = Unchanging{data, bytes, h, 0, 0, 0};
"""),
    ],
    'runtime/src/vif.cpp': [
        (r"""    std::uint32_t records, words, epoch;   // what to send the VU thread: g_records[records], words long, if epoch is still g_epoch
    bool dbf;
""", r"""    std::uint32_t records, words, epoch;   // what to send the VU thread: g_records[records], words long, if epoch is still g_epoch
    std::uint32_t hash;   // of the bytes then: the same place can hold other unchanging data later (a new area's models)
    bool dbf;
"""),
        (r"""    if (slot.data == data && slot.qwc == qwc && slot.before == before && slot.epoch == g_epoch) {
""", r"""    if (slot.data == data && slot.qwc == qwc && slot.before == before && slot.epoch == g_epoch && slot.hash == gs::queue::g_unchanging_hash) {
"""),
        (r"""        slot.epoch = g_epoch;
        g_records_used += g_capture_words;
""", r"""        slot.epoch = g_epoch;
        slot.hash = gs::queue::g_unchanging_hash;
        g_records_used += g_capture_words;
"""),
    ],
    'runtime/src/gs/gs.hpp': [
        (r"""void vblank(bool start);
bool interlaced_odd_field();
""", r"""void vblank(bool start);
extern std::uint32_t g_max_vu_lag;   // ps2run_3ds_patch.py: vblanks the drawing side may be behind; 0 for no limit
bool interlaced_odd_field();
"""),
    ],
    'runtime/src/gs/gs.cpp': [
        (r"""    const std::uint32_t rrw = static_cast<std::uint32_t>(reg) & 0xFFFu, rrh = static_cast<std::uint32_t>(reg >> 32) & 0xFFFu;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(rrw) * rrh);
""", r"""    const std::uint32_t rrw = static_cast<std::uint32_t>(reg) & 0xFFFu, rrh = static_cast<std::uint32_t>(reg >> 32) & 0xFFFu;
    if (static_cast<std::size_t>(rrw) * rrh > (1u << 20)) {   // ps2run_3ds_patch.py: only garbage asks this much; it took 10 MB of a 3DS heap
        static bool warned = false;
        if (!warned) std::fprintf(stderr, "[gs] local copy of %ux%u left out\n", rrw, rrh);
        warned = true;
        return;
    }
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(rrw) * rrh);
"""),
        (r"""
void vblank(bool start) {
""", r"""
std::atomic<std::uint32_t> g_vblanks_served{0};
std::uint32_t g_vblanks_posted = 0;
// ps2run_3ds_patch.py: the drawing side reads unchanging data from PS2 memory when it gets to it (see unchanging() in queue.cpp), and
// with that data sent as addresses its queue held twenty pictures and more, by which time the game had put a new area's models
// there. So the game waits at a vblank while the drawing side is more than this many behind. In the lobby it is normally 6 to 10
// behind: a limit of 3 cost 7.6 ms a frame on the console, 6 cost 4, and 10 cost 0.8, and walking into the hallway in Azahar
// with draw 3 fails with no limit and passes with 10 or 14.
std::uint32_t g_max_vu_lag = 10;

void vblank(bool start) {
"""),
        (r"""            queue::post(queue::kVblank, static_cast<std::uint32_t>(ps2::frame_count()), 0, &message, sizeof message);
        }
""", r"""            queue::post(queue::kVblank, static_cast<std::uint32_t>(ps2::frame_count()), 0, &message, sizeof message);
            ++g_vblanks_posted;
            if (g_max_vu_lag && g_vblanks_posted - g_vblanks_served.load() > g_max_vu_lag) {
                PS2_PROF(ps2::prof::Full);   // counts as a full queue: the picture skipping goes by that
                for (std::uint32_t done = g_vblanks_served.load(); g_vblanks_posted - done > g_max_vu_lag; done = g_vblanks_served.load())
                    queue::wait(g_vblanks_served, done);
            }
        }
"""),
        (r"""        if (pica::active()) pica::vblank(arg0, message.regs, message.picture);
        break;
""", r"""        if (pica::active()) pica::vblank(arg0, message.regs, message.picture);
        g_vblanks_served.fetch_add(1);
        queue::notify(g_vblanks_served);
        break;
"""),
    ],
}


# Movies and saved states for the console itself (2026-11-13).
# - The IPU asks a watcher about each macroblock a CSC command converts. The 3DS build's movie player (port3ds/movie3ds.cpp)
#   reads its picture markers there and has the conversion left out while one of its own movies is on.
# - On the 3DS a state is written to the card packed, as it is made, and read back the same way: the whole of one does not
#   fit the heap. zlib reads a plain state from the computer too. The caller of request_state_save() names the file there.
MOVIE_EDITS = {
    'runtime/src/ipu/ipu.hpp': [
        (r"""void stats();

""", r"""void stats();
// ps2run_3ds_patch.py: asked for each macroblock a CSC command is about to convert, with its number in the command and how many
// the command converts. True leaves the conversion out, and the pixels come out black. The 3DS build reads its movie markers
// here (port3ds/movie3ds.cpp).
extern bool (*g_csc_watch)(const std::uint8_t *raw, unsigned index, unsigned count);

"""),
    ],
    'runtime/src/ipu/ipu.cpp': [
        (r"""                convert_fast(g_in.data() + (g_bit >> 3));
""", r"""                if (g_csc_watch && g_csc_watch(g_in.data() + (g_bit >> 3), (cmd & 0x7FFu) - g_count, cmd & 0x7FFu)) g_out.insert(g_out.end(), 1024, 0);
                else convert_fast(g_in.data() + (g_bit >> 3));
"""),
        (r"""                convert(raw, (cmd >> 27) & 1u);
""", r"""                if (g_csc_watch && g_csc_watch(raw, (cmd & 0x7FFu) - g_count, cmd & 0x7FFu)) g_out.insert(g_out.end(), (cmd >> 27) & 1u ? 512 : 1024, 0);
                else convert(raw, (cmd >> 27) & 1u);
"""),
        (r"""
}

void reset() {
""", r"""
}

bool (*g_csc_watch)(const std::uint8_t *raw, unsigned index, unsigned count) = nullptr;   // ps2run_3ds_patch.py

void reset() {
"""),
    ],
    'runtime/src/system.cpp': [
        (r"""#include <stdexcept>

""", r"""#include <stdexcept>
#ifdef __3DS__
#include <zlib.h>   // ps2run_3ds_patch.py: states are written and read packed, piece by piece
#endif

"""),
        (r"""    if (g_live_state_request.exchange(false)) {
        std::string name;
""", r"""    if (g_live_state_request.exchange(false)) {
#ifndef __3DS__   // ps2run_3ds_patch.py: there the caller names the file in g_options.save_state_path
        std::string name;
"""),
        (r"""        g_options.save_state_path = name;
        g_state_request = true;
""", r"""        g_options.save_state_path = name;
#endif
        g_state_request = true;
"""),
        (r"""
bool save_state(const std::string &path) {
    try {
""", r"""
#ifdef __3DS__
namespace {
// ps2run_3ds_patch.py: a state is about 44 MB and the console has 7 MB of heap to spare, so it goes to the card as it is made.
// zlib's fastest setting makes it about a quarter of that, which the card writes sooner than the whole.
class PackedWriter : public state::Stream {
public:
    explicit PackedWriter(gzFile file) : Stream(true), file_(file) {}
    void bytes(void *data, std::size_t size) override {
        if (size && gzwrite(file_, data, static_cast<unsigned>(size)) != static_cast<int>(size)) throw std::runtime_error("cannot write the state file");
    }

private:
    gzFile file_;
};
}
#endif

bool save_state(const std::string &path) {
#ifdef __3DS__
    {
        const std::string part = path + ".part";
        gzFile file = gzopen(part.c_str(), "wb1");
        if (!file) {
            std::fprintf(stderr, "[state] cannot write %s\n", part.c_str());
            return false;
        }
        gzbuffer(file, 128u << 10);
        bool saved = false;
        try {
            PackedWriter w(file);
            machine_state(w);
            saved = true;
        } catch (const std::exception &e) {
            std::fprintf(stderr, "[state] cannot save: %s\n", e.what());
        }
        if (gzclose(file) != Z_OK) saved = false;
        // The old file goes only once the new one is whole.
        if (saved) {
            std::remove(path.c_str());
            saved = std::rename(part.c_str(), path.c_str()) == 0;
        }
        if (saved) std::fprintf(stderr, "[state] saved frame %llu to %s\n", static_cast<unsigned long long>(g_frames), path.c_str());
        else std::remove(part.c_str());
        return saved;
    }
#endif
    try {
"""),
        (r"""class FileReader : public state::Stream {   // ps2run_3ds_patch.py
""", r"""class FileReader : public state::Stream {   // ps2run_3ds_patch.py: zlib reads a packed state and a plain one alike
"""),
        (r"""    explicit FileReader(std::FILE *file) : Stream(false), file_(file) {}
""", r"""    explicit FileReader(gzFile file) : Stream(false), file_(file) {}
"""),
        (r"""        if (std::fread(data, 1, size, file_) != size) throw std::runtime_error("state file is truncated");
    }

private:
    std::FILE *file_;
};
}
#endif

""", r"""        if (size && gzread(file_, data, static_cast<unsigned>(size)) != static_cast<int>(size)) throw std::runtime_error("state file is truncated");
    }

private:
    gzFile file_;
};
}
#endif

"""),
        (r"""    std::FILE *file = std::fopen(path.c_str(), "rb");
""", r"""    gzFile file = gzopen(path.c_str(), "rb");
"""),
        (r"""    }
    bool loaded = false;
""", r"""    }
    gzbuffer(file, 128u << 10);
    bool loaded = false;
"""),
        (r"""    }
    std::fclose(file);
    return loaded;
""", r"""    }
    gzclose(file);
    return loaded;
"""),
    ],
}


SPLIT_SKIP_EDITS = {
    'runtime/src/gs/gs.cpp': [
        ('''void start_thread() { queue::render().start(queue::g_split_pica ? 512u << 10 : 4096u); }''',
         '''void start_thread() {
    // Keep enough GIF data queued for the VU to continue while the GS worker waits for the GPU.
    queue::render().start(queue::g_split_pica ? 1536u << 10 : 4096u);
}'''),
        ('''            queue::post(queue::kVblank, static_cast<std::uint32_t>(ps2::frame_count()), 0, &message, sizeof message);''',
         '''            queue::post(queue::kVblank, static_cast<std::uint32_t>(ps2::frame_count()), message.picture, &message, sizeof message);'''),
    ],
    'runtime/src/gs/queue.hpp': [
        ('extern bool g_split_pica;', 'extern bool g_split_pica;\nextern bool g_vu_skip_picture;'),
    ],
    'runtime/src/gs/queue.cpp': [
        ('bool g_split_pica = false;', 'bool g_split_pica = false;\nbool g_vu_skip_picture = false;'),
    ],
    'runtime/src/vif.cpp': [
        ('''        if (gs::queue::g_skip_picture && (resume || vu::seen(address))) return;
        if (gs::queue::g_skip_picture) {''', '''        const bool skip = gs::queue::g_split_pica ? gs::queue::g_vu_skip_picture : gs::queue::g_skip_picture;
        if (skip && (resume || vu::seen(address))) return;
        if (skip) {'''),
        ('''    if (unit == 1 && gs::queue::g_skip_picture && ((cmd >> 15) & 1u)) dry = true;''',
         '''    if (unit == 1 && ((cmd >> 15) & 1u) && (gs::queue::g_split_pica ? gs::queue::g_vu_skip_picture : gs::queue::g_skip_picture)) dry = true;'''),
    ],
    'runtime/src/system.cpp': [
        ('''#include "gs/queue.hpp"
''', '''#include "gs/queue.hpp"
#ifdef PS2_PICA
#include "pica.hpp"
#endif
'''),
        ('''#ifdef PS2_PICA
            case gs::queue::kVuStop:''', '''#ifdef PS2_PICA
            case gs::queue::kVblank:
                if (gs::queue::g_split_pica) {
                    gs::pica::advance_vu_picture(h.arg1);
                    gs::queue::render().post(gs::queue::kVblank, h.arg0, h.arg1, payload, h.bytes);
                } else gs::serve(h.type, h.arg0, payload);
                break;
            case gs::queue::kVuStop:'''),
    ],
}

# SAMPLE=1 (port3ds/Makefile): the sampler also notes which compiled VU program the VU thread is running (2026-11-18).
SAMPLE_EDITS = {
    'runtime/include/ps2/system.hpp': [
        ('''#else
#define PS2_PROF(counter)
#endif
''', '''#else
#define PS2_PROF(counter)
#define PS2_PROF_EXTRA(k)
#define PS2_SAMPLE_VU(fn)
#endif
'''),
    ],
    'runtime/src/vu_micro.cpp': [
        ('''            else fn(g_vu1, g_vu1_mem);
''', '''            else {
                PS2_SAMPLE_VU(reinterpret_cast<std::uintptr_t>(fn));   // ps2run_3ds_patch.py
                fn(g_vu1, g_vu1_mem);
            }
'''),
        ('''    run_fast(unit, pc);
}
''', '''    PS2_SAMPLE_VU(std::uintptr_t{1});
    run_fast(unit, pc);
}
'''),
    ],
    # What the VU thread's queue loop does between programs, for the sampler only (PS2_PROF_EXTRA is empty otherwise, and with
    # PROF=1 it counts only on the game thread). x0 to x3 are taken by port3ds/pica.cpp.
    'runtime/src/system.cpp': [
        ('''            case gs::queue::kVif1Records: vif::transfer_records(payload, h.bytes); break;
            case gs::queue::kGifAddress: {
                const std::uint8_t *data;
                std::memcpy(&data, payload, sizeof data);
                gif::vu_path3(data, h.arg1);
                break;
            }
''', '''            case gs::queue::kVif1Records: {
                PS2_PROF_EXTRA(4);   // ps2run_3ds_patch.py
                vif::transfer_records(payload, h.bytes);
                break;
            }
            case gs::queue::kGifAddress: {
                PS2_PROF_EXTRA(5);
                const std::uint8_t *data;
                std::memcpy(&data, payload, sizeof data);
                gif::vu_path3(data, h.arg1);
                break;
            }
'''),
        ('''            case gs::queue::kGif:
                if (h.arg0 == 3) gif::vu_path3(payload, h.arg1);
#ifdef PS2_PICA
                else gif::render(static_cast<int>(h.arg0), payload, h.arg1);
''', '''            case gs::queue::kGif:
                if (h.arg0 == 3) {
                    PS2_PROF_EXTRA(5);
                    gif::vu_path3(payload, h.arg1);
                }
#ifdef PS2_PICA
                else {
                    PS2_PROF_EXTRA(6);
                    gif::render(static_cast<int>(h.arg0), payload, h.arg1);
                }
'''),
        ('''            default:
                if (gs::queue::g_split_pica)
                    gs::queue::render().post(static_cast<gs::queue::Type>(h.type), h.arg0, h.arg1, payload, h.bytes);
                else gs::serve(h.type, h.arg0, payload);
                break;
''', '''            default: {
                PS2_PROF_EXTRA(7);
                if (gs::queue::g_split_pica)
                    gs::queue::render().post(static_cast<gs::queue::Type>(h.type), h.arg0, h.arg1, payload, h.bytes);
                else gs::serve(h.type, h.arg0, payload);
                break;
            }
'''),
    ],
}

# A native stand-in for VU1 programs (port3ds/g3native.cpp): the VIF's program start asks a hook first, and XGKICK packets can be
# copied out, so a stand-in can be checked against the program itself.
G3_EDITS = {
    'runtime/src/vu_micro.hpp': [
        ('''#include <cstdint>
''', '''#include <cstdint>
#include <vector>
'''),
        ('''Vu0Regs &regs(int unit);
''', '''Vu0Regs &regs(int unit);
// ps2run_3ds_patch.py: a native stand-in for VU1 programs. The hook gets each VU1 start address first; true means it did the work.
using NativeHook = bool (*)(std::uint32_t address);
extern NativeHook g_native_hook;
std::uintptr_t program_at(std::uint32_t address);   // the compiled program that starts here, or 0
void run_program(std::uint32_t address);            // run the VU1 program itself (for a hook that declines or checks)
extern std::vector<std::uint8_t> *g_kick_capture;   // when set, each VU1 XGKICK packet is also appended here
std::uint32_t data_top();
std::uint32_t data_itop();
'''),
    ],
    'runtime/src/vu_micro.cpp': [
        ('''void kick(int unit, std::uint32_t address) {
    if (g_kick_log && unit == 1) g_kick_log->push_back(address);
    Unit u = unit_of(unit);
    xgkick(u, address);
}
''', '''NativeHook g_native_hook = nullptr;   // ps2run_3ds_patch.py
std::vector<std::uint8_t> *g_kick_capture = nullptr;

void capture_packet(Unit &u, std::uint32_t address) {
    std::uint32_t q = address & (u.data_qwords - 1), count = 0;
    bool done = false;
    while (!done && count < u.data_qwords) {
        std::uint64_t tag;
        std::memcpy(&tag, qword(u, q), 8);
        const std::uint32_t nloop = static_cast<std::uint32_t>(tag) & 0x7FFFu, flg = static_cast<std::uint32_t>(tag >> 58) & 3u;
        std::uint32_t nreg = static_cast<std::uint32_t>(tag >> 60) & 0xFu;
        if (!nreg) nreg = 16;
        const std::uint32_t body = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
        for (std::uint32_t i = 0; i <= body && count < u.data_qwords; ++i, ++count) {
            const std::uint8_t *p = qword(u, q);
            g_kick_capture->insert(g_kick_capture->end(), p, p + 16);
            q = (q + 1) & (u.data_qwords - 1);
        }
        done = (tag >> 15) & 1u;
    }
}

void kick(int unit, std::uint32_t address) {
    if (g_kick_log && unit == 1) g_kick_log->push_back(address);
    Unit u = unit_of(unit);
    if (g_kick_capture && unit == 1) capture_packet(u, address);
    xgkick(u, address);
}

std::uint32_t data_top() { return g_top; }
std::uint32_t data_itop() { return g_itop; }
'''),
        ('''    if (n == 1 && !exact) {
        interpret(n, address);
        return;
    }
''', '''    if (n == 1 && !exact) {
        if (g_native_hook && g_native_hook(address)) return;   // ps2run_3ds_patch.py
        interpret(n, address);
        return;
    }
'''),
        ('''ProgramFn lookup(std::uint32_t address) {
''', '''std::uintptr_t program_at(std::uint32_t address);
void run_program(std::uint32_t address) { interpret(1, address); }   // ps2run_3ds_patch.py

ProgramFn lookup(std::uint32_t address) {
'''),
        ('''void run(int n, std::uint32_t address) {
''', '''std::uintptr_t program_at(std::uint32_t address) { return reinterpret_cast<std::uintptr_t>(lookup(address)); }   // ps2run_3ds_patch.py

void run(int n, std::uint32_t address) {
'''),
    ],
    # The GPU path ("g3 4"): a model batch goes to the PICA renderer as the batch's own vertices (its GIF tag, which sets
    # PRIM, has gone through the GIF first).
    'runtime/src/gs/gs.hpp': [
        ('''namespace ps2::gs {
''', '''namespace g3 { struct Gpu; }   // ps2run_3ds_patch.py

namespace ps2::gs {
''', ),
        ('''void write(std::uint32_t reg, std::uint64_t value);
''', '''void write(std::uint32_t reg, std::uint64_t value);
bool draw_g3(const ::g3::Gpu &g);   // ps2run_3ds_patch.py: false when the PICA renderer cannot take it (PRIM already set)
'''),
    ],
    'runtime/src/gs/gs.cpp': [
        ('''void draw_gpu(const State &s) {
    if (queue::g_skip_picture) return;
    static bool known = false;
    static std::uint32_t seen = 0;
    static std::uint64_t clamp = 0;
    if (!known || seen != g_setup_generation) {
        known = true;
        seen = g_setup_generation;
        const Context &uc = s.ctx[static_cast<unsigned>(s.prim >> 9) & 1u];
        g_upload_ext.erase((static_cast<std::uint32_t>(uc.frame) & 0x1FFu) * 32u);
        clamp = upload_clamped(s, uc);
    }
    pica::draw(s, g_vertex_serial, clamp);
    ++g_primitives;
}
#endif
''', '''std::uint64_t gpu_clamp(const State &s) {
    static bool known = false;
    static std::uint32_t seen = 0;
    static std::uint64_t clamp = 0;
    if (!known || seen != g_setup_generation) {
        known = true;
        seen = g_setup_generation;
        const Context &uc = s.ctx[static_cast<unsigned>(s.prim >> 9) & 1u];
        g_upload_ext.erase((static_cast<std::uint32_t>(uc.frame) & 0x1FFu) * 32u);
        clamp = upload_clamped(s, uc);
    }
    return clamp;
}

void draw_gpu(const State &s) {
    if (queue::g_skip_picture) return;
    pica::draw(s, g_vertex_serial, gpu_clamp(s));
    ++g_primitives;
}
#endif

bool draw_g3(const ::g3::Gpu &g) {   // ps2run_3ds_patch.py
#ifdef PS2_PICA
    if (!pica::active() || !pica::g3_ready()) return false;
    if (!queue::g_skip_picture && !pica::draw_g3(g_state, gpu_clamp(g_state), g)) return false;
    ++g_primitives;
    return true;
#else
    (void)g;
    return false;
#endif
}
'''),
    ],
}



# Core 0 dispatch/fast-target, Core 2 deferred GS uploads & direct CLUTs, VIF/DMAC/queue optimizations.
RT_OPT_EDITS = {
    'runtime/include/ps2/generated.hpp': [
        ('''void register_units(const UnitInfo *units, std::uint32_t count);
void register_modules(const ModuleInfo *modules, std::uint32_t count);

void dispatch(Cpu &c, std::uint32_t entry);
''',
         '''void register_units(const UnitInfo *units, std::uint32_t count);
void register_modules(const ModuleInfo *modules, std::uint32_t count);
using FastFn = bool (*)(Cpu &);
struct FastTarget {
    std::uint32_t pc;
    FastFn fn;
    bool is_native;
};
void register_fast_targets(const FastTarget *targets, std::uint32_t count);

void dispatch(Cpu &c, std::uint32_t entry);
'''),
    ],
    'runtime/include/ps2/system.hpp': [
        ('''#define PS2_PROF(counter)
#define PS2_PROF_EXTRA(k)
#define PS2_SAMPLE_VU(fn)
#endif
''',
         '''#define PS2_PROF(counter)
#define PS2_PROF_EXTRA(k)
#define PS2_SAMPLE_VU(fn)
#endif
#ifdef PS2_PICA
#include "prof.hpp"
#else
#define PS2_PROF(counter)
#define PS2_SAMPLE_VU(fn)
#endif
'''),
        ('''#endif
void stop_vu_thread();
void run_vu_thread();
double vu_thread_idle_ms();
''',
         '''#endif
void stop_vu_thread();
void abort_vu_thread();
bool vu_aborting();
void run_vu_thread();
double vu_thread_idle_ms();
'''),
    ],
    'runtime/src/dispatch.cpp': [
        ('''std::uint32_t g_origin = 0;
std::uint32_t g_unit_size = 1;
std::set<std::uint32_t> g_misses;
std::set<std::string> g_module_misses;
''',
         '''std::uint32_t g_origin = 0;
std::uint32_t g_unit_size = 1;
std::uint32_t g_unit_shift = 0;
struct FastEntry {
    std::uint32_t pc_tag;
    FastFn fn;
};
constexpr std::uint32_t kFastMask = 1023u;
FastEntry g_fast[kFastMask + 1u]{};
bool g_has_fast = false;
std::set<std::uint32_t> g_misses;
std::set<std::string> g_module_misses;
'''),
        ('''}

const UnitInfo *unit_for(std::uint32_t pc) {
    const std::uint32_t slot = (pc - g_origin) / g_unit_size;
    if (pc < g_origin || slot >= g_index.size()) return nullptr;
    return g_index[slot];
}

''',
         '''}

inline const UnitInfo *unit_for(std::uint32_t pc) {
    if (pc < g_origin) return nullptr;
    const std::uint32_t slot = g_unit_shift ? ((pc - g_origin) >> g_unit_shift) : ((pc - g_origin) / g_unit_size);
    if (slot >= g_index.size()) return nullptr;
    return g_index[slot];
}

[[gnu::always_inline]] inline bool try_fast(Cpu &c, std::uint32_t pc) {
    if (!g_has_fast || (pc & 3u)) return false;
    std::uint32_t slot = ((pc >> 2) * 2654435761u) >> 22;
    for (;;) {
        const FastEntry &e = g_fast[slot];
        const std::uint32_t tag = e.pc_tag;
        if (!tag) return false;
        if ((tag & ~1u) == pc) {
            if (e.fn(c)) {
                if (tag & 1u) c.pc = c.r[31].w[0];
                return true;
            }
            return false;
        }
        slot = (slot + 1u) & kFastMask;
    }
}

'''),
        ('''    g_origin = g_units.front().base;
    g_unit_size = g_units.front().size;
    g_index.assign((g_units.back().base - g_origin) / g_unit_size + 1, nullptr);
    for (const UnitInfo &u : g_units) g_index[(u.base - g_origin) / g_unit_size] = &u;
}

''',
         '''    g_origin = g_units.front().base;
    g_unit_size = g_units.front().size;
    g_unit_shift = (g_unit_size && (g_unit_size & (g_unit_size - 1u)) == 0) ? static_cast<std::uint32_t>(__builtin_ctz(g_unit_size)) : 0u;
    g_index.assign((g_units.back().base - g_origin) / g_unit_size + 1, nullptr);
    for (const UnitInfo &u : g_units) g_index[(u.base - g_origin) / g_unit_size] = &u;
}

void register_fast_targets(const FastTarget *targets, std::uint32_t count) {
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t pc = targets[i].pc;
        if (!pc || (pc & 3u) || !targets[i].fn) continue;
        std::uint32_t slot = ((pc >> 2) * 2654435761u) >> 22;
        for (std::uint32_t p = 0; p <= kFastMask; ++p) {
            FastEntry &e = g_fast[slot];
            if (!e.pc_tag || (e.pc_tag & ~1u) == pc) {
                e = FastEntry{pc | (targets[i].is_native ? 1u : 0u), targets[i].fn};
                g_has_fast = true;
                break;
            }
            slot = (slot + 1u) & kFastMask;
        }
    }
}

'''),
        ('''    const std::uint32_t pc = c.pc;
    if (pc & kUnwind) return;
    if (g_comm.enabled != 0) comm_trace(c, pc);
    if (pc == kHandlerReturn || pc == kIdle) return;
    const UnitInfo *u = unit_for(pc);
    const std::uint16_t id = u ? u->entries[(pc - u->base) >> 2] : 0;
''',
         '''    const std::uint32_t pc = c.pc;
    if (pc & kUnwind) return;
#ifndef __3DS__
    if (g_comm.enabled != 0) comm_trace(c, pc);
#endif
    if (pc == kHandlerReturn || pc == kIdle) return;
#ifdef __3DS__
    if (try_fast(c, pc)) return;
#endif
    const UnitInfo *u = unit_for(pc);
    const std::uint16_t id = u ? u->entries[(pc - u->base) >> 2] : 0;
'''),
        ('''    c.pc = target;
    if (!stack_ok()) return;
    dispatch(c, 0);
}
''',
         '''    c.pc = target;
    if (!stack_ok()) return;
#ifdef __3DS__
    if (try_fast(c, target)) return;
#endif
    dispatch(c, 0);
}
'''),
    ],
    'runtime/src/dmac.cpp': [
        ('''    const std::size_t room = (address & 0x80000000u) ? kScratchpadSize - (address & (kScratchpadSize - 1)) : kRamSize - (address & (kRamSize - 16));
    const std::uint8_t *data = source(address);
    if (bytes > room || ch == 9) {
        buffer.resize(bytes);
''',
         '''    const std::size_t room = (address & 0x80000000u) ? kScratchpadSize - (address & (kScratchpadSize - 1)) : kRamSize - (address & (kRamSize - 16));
    const std::uint8_t *data = source(address);
    if (ch == 9 && bytes <= room) {
        std::uint32_t sadr = g_ch[9].sadr & (kScratchpadSize - 1);
        std::uint32_t left = qwc;
        const std::uint8_t *src = data;
        while (left) {
            const std::uint32_t n = std::min<std::uint32_t>(left, (kScratchpadSize - sadr) / 16u);
            std::memcpy(g_spr + sadr, src, static_cast<std::size_t>(n) * 16u);
            src += static_cast<std::size_t>(n) * 16u;
            sadr = (sadr + n * 16u) & (kScratchpadSize - 1);
            left -= n;
        }
        g_ch[9].sadr = sadr;
        return;
    }
    if (bytes > room || ch == 9) {
        buffer.resize(bytes);
'''),
        ('''        const std::uint32_t addr = static_cast<std::uint32_t>(tag >> 32) & 0x7FFFFFF0u;
        const std::uint32_t addr_full = addr | (static_cast<std::uint32_t>(tag >> 63) << 31);
        if (tte && (ch == 0 || ch == 1)) {
            std::uint8_t q[16];
''',
         '''        const std::uint32_t addr = static_cast<std::uint32_t>(tag >> 32) & 0x7FFFFFF0u;
        const std::uint32_t addr_full = addr | (static_cast<std::uint32_t>(tag >> 63) << 31);
#ifdef PS2_PICA
        std::uint32_t tte_words[2];
        bool tte_deferred = false;
        if (tte && (ch == 0 || ch == 1)) {
            std::memcpy(tte_words, tagp + 8, 8);
            if (ch == 1 && qwc) {
                tte_deferred = true;
            } else {
                vif::transfer_tte(ch, tte_words);
            }
        }
#else
        if (tte && (ch == 0 || ch == 1)) {
            std::uint8_t q[16];
'''),
        ('''            vif::transfer(ch, q, 1);
        }
        bool end = false;
        const std::uint32_t tag_at = c.tadr;
''',
         '''            vif::transfer(ch, q, 1);
        }
#endif
        bool end = false;
        const std::uint32_t tag_at = c.tadr;
'''),
        (r'''        if (trace_tags && ch == 1) std::fprintf(stderr, "[dma] ch1 tag at 0x%08X id %u qwc %u irq %d addr 0x%08X next 0x%08X\n", tag_at, id, qwc, irq ? 1 : 0, addr_full, c.tadr);
        if (mfifo && (id == 1 || id == 7)) {
            deliver_ring(ch, data, qwc);
            c.madr = ring(data + qwc * 16);
        } else {
            deliver(ch, data, qwc);
            c.madr = data + qwc * 16;
''',
         r'''        if (trace_tags && ch == 1) std::fprintf(stderr, "[dma] ch1 tag at 0x%08X id %u qwc %u irq %d addr 0x%08X next 0x%08X\n", tag_at, id, qwc, irq ? 1 : 0, addr_full, c.tadr);
        if (mfifo && (id == 1 || id == 7)) {
#ifdef PS2_PICA
            const std::uint32_t ring_room = ((g_rbsr - (data & g_rbsr)) >> 4) + 1u;
            const std::size_t bytes = static_cast<std::size_t>(qwc) * 16u;
            const std::size_t ram_room = kRamSize - (data & (kRamSize - 16u));
            if (ch == 1 && qwc && qwc <= ring_room && bytes <= ram_room) {
                if (tte_deferred) vif::transfer_tte_data(1, tte_words, source(data), qwc, false);
                else vif::transfer_dynamic(1, source(data), qwc);
            } else {
                if (tte_deferred) vif::transfer_tte(1, tte_words);
                deliver_ring(ch, data, qwc);
            }
#else
            deliver_ring(ch, data, qwc);
#endif
            c.madr = ring(data + qwc * 16);
        } else {
#ifdef PS2_PICA
            if (tte_deferred) {
                const std::size_t bytes = static_cast<std::size_t>(qwc) * 16u;
                const std::size_t room = (data & 0x80000000u) ? kScratchpadSize - (data & (kScratchpadSize - 1)) : kRamSize - (data & (kRamSize - 16));
                if (bytes <= room) {
                    vif::transfer_tte_data(1, tte_words, source(data), qwc, true);
                } else {
                    vif::transfer_tte(1, tte_words);
                    deliver(1, data, qwc);
                }
            } else
#endif
            deliver(ch, data, qwc);
            c.madr = data + qwc * 16;
'''),
    ],
    'runtime/src/gs/gif.cpp': [
        ('''        default: {
            const std::uint32_t run = std::min(qwc - i, p.nloop - p.loop);
            sink.image(data + static_cast<std::size_t>(i) * 16, run);
            p.loop += run;
            i += run - 1;
''',
         '''        default: {
            const std::uint32_t run = std::min(qwc - i, p.nloop - p.loop);
            sink.image(data + static_cast<std::size_t>(i) * 16, run, i);
            p.loop += run;
            i += run - 1;
'''),
        ('''struct GsSink {
    int path;
    void tag(const Path &p, const Reg128 &q) {
        ++g_tags[path][p.flg];
''',
         '''struct GsSink {
    int path;
#ifdef PS2_PICA
    std::uint32_t chunk_hash = 0;
    bool ram_backed = false;
#endif
    void tag(const Path &p, const Reg128 &q) {
        ++g_tags[path][p.flg];
'''),
        ('''    void packed(std::uint32_t reg, const Reg128 &q) { gs::write_packed(reg, q); }
    void reglist(std::uint32_t reg, std::uint64_t value) { gs::write(reg, value); }
    void image(const std::uint8_t *data, std::uint32_t qwords) { gs::write_image(data, qwords); }
};

''',
         '''    void packed(std::uint32_t reg, const Reg128 &q) { gs::write_packed(reg, q); }
    void reglist(std::uint32_t reg, std::uint64_t value) { gs::write(reg, value); }
    void image(const std::uint8_t *data, std::uint32_t qwords, std::uint32_t qoff) {
#ifdef PS2_PICA
        const std::uint32_t h = chunk_hash ? ((chunk_hash ^ (qoff * 0x9E3779B1u) ^ (qwords * 0x85EBCA6Bu)) | 1u) : 0u;
        gs::write_image(data, qwords, h, ram_backed);
#else
        (void)qoff;
        gs::write_image(data, qwords);
#endif
    }
};

'''),
        ('''}

void render(int path, const std::uint8_t *data, std::uint32_t qwc) {
#ifdef PS2_PICA
    if (gs::queue::g_split_pica && gs::queue::role() == gs::queue::Role::Vu) {
        // The VU reuses its packet memory after this call. Copy complete qwords
        // into the ring; the GS retains the parser state across chunks.
''',
         '''}

void render_chunk(int path, const std::uint8_t *data, std::uint32_t qwc, std::uint32_t known_hash, bool ram_backed) {
#ifdef PS2_PICA
    if (gs::queue::g_split_pica && gs::queue::is_vu_thread()) {
        // The VU reuses its packet memory after this call. Copy complete qwords
        // into the ring; the GS retains the parser state across chunks.
'''),
        ('''        return;
    }
#endif
    GsSink sink{path};
    walk(g_paths[path], data, qwc, sink);
}

void transfer_direct(int path, const std::uint8_t *data, std::uint32_t qwc);

namespace {
std::vector<std::uint8_t> g_pend3;
std::size_t g_pend3_head = 0;   // bytes already sent: moving what is left down on every packet took 200 MB of copying per picture
bool g_released = false;
bool g_vmask = false;  // the VU thread's own view of the mask (threaded build)
bool mask_up() { return gs::queue::vu_active() ? g_vmask : (g_stat & 2u) != 0; }

// Length in qwords of the GIF packet (tags up to and including the next one with EOP) at the front of the queue.
''',
         '''        return;
    }
    GsSink sink{path, known_hash, ram_backed};
#else
    (void)known_hash, (void)ram_backed;
    GsSink sink{path};
#endif
    walk(g_paths[path], data, qwc, sink);
}

void render(int path, const std::uint8_t *data, std::uint32_t qwc) { render_chunk(path, data, qwc, 0, false); }

void transfer_direct(int path, const std::uint8_t *data, std::uint32_t qwc, std::uint32_t known_hash = 0, bool ram_backed = false);

namespace {
bool g_released = false;
bool g_vmask = false;  // the VU thread's own view of the mask (threaded build)
bool mask_up() { return gs::queue::vu_active() ? g_vmask : (g_stat & 2u) != 0; }

#ifdef PS2_PICA
struct Pend3Chunk {
    const std::uint8_t *ram;   // non-null when zero-copy in g_ram
    std::uint32_t buf_off;     // byte offset in g_pend3_buf when ram == nullptr
    std::uint32_t qwc;
    std::uint32_t hash;        // non-zero when ram came from unchanging()
};
std::vector<Pend3Chunk> g_pend3_chunks;
std::size_t g_pend3_chunk_head = 0;
std::uint32_t g_pend3_chunk_off = 0;
std::vector<std::uint8_t> g_pend3_buf;
std::size_t g_pend3_buf_head = 0;

bool pend3_empty() { return g_pend3_chunk_head >= g_pend3_chunks.size(); }

std::size_t pend3_qwords() {
    std::size_t total = 0;
    for (std::size_t i = g_pend3_chunk_head; i < g_pend3_chunks.size(); ++i)
        total += g_pend3_chunks[i].qwc - (i == g_pend3_chunk_head ? g_pend3_chunk_off : 0u);
    return total;
}

void pend3_reset_if_empty() {
    if (g_pend3_chunk_head >= g_pend3_chunks.size()) {
        g_pend3_chunks.clear();
        g_pend3_chunk_head = 0;
        g_pend3_chunk_off = 0;
        g_pend3_buf.clear();
        g_pend3_buf_head = 0;
    }
}

void release(bool everything) {
    if (everything) {
        while (g_pend3_chunk_head < g_pend3_chunks.size()) {
            const Pend3Chunk c = g_pend3_chunks[g_pend3_chunk_head++];
            const std::uint32_t off = g_pend3_chunk_off;
            g_pend3_chunk_off = 0;
            const std::uint32_t n = c.qwc - off;
            const std::uint8_t *ptr = (c.ram ? c.ram : g_pend3_buf.data() + c.buf_off) + static_cast<std::size_t>(off) * 16;
            if (!c.ram) g_pend3_buf_head = c.buf_off + static_cast<std::size_t>(c.qwc) * 16;
            const std::uint32_t h = c.hash ? (off == 0 ? c.hash : ((c.hash ^ (off * 0x9E3779B1u)) | 1u)) : 0u;
            transfer_direct(3, ptr, n, h, c.ram != nullptr);
        }
        pend3_reset_if_empty();
        return;
    }
    std::uint32_t tag_left = 0;
    bool seen_eop = false;
    while (g_pend3_chunk_head < g_pend3_chunks.size()) {
        const Pend3Chunk c = g_pend3_chunks[g_pend3_chunk_head];
        const std::uint8_t *base = c.ram ? c.ram : g_pend3_buf.data() + c.buf_off;
        const std::uint32_t start = g_pend3_chunk_off;
        std::uint32_t off = start;
        while (off < c.qwc) {
            if (tag_left == 0) {
                std::uint64_t tag;
                std::memcpy(&tag, base + static_cast<std::size_t>(off) * 16, 8);
                const std::uint32_t nloop = static_cast<std::uint32_t>(tag) & 0x7FFFu;
                const std::uint32_t flg = static_cast<std::uint32_t>(tag >> 58) & 3u;
                std::uint32_t nreg = static_cast<std::uint32_t>(tag >> 60) & 0xFu;
                if (!nreg) nreg = 16;
                seen_eop = (tag >> 15) & 1u;
                const std::uint32_t body = flg == 0 ? nloop * nreg : flg == 1 ? (nloop * nreg + 1) / 2 : nloop;
                tag_left = 1u + body;
            }
            const std::uint32_t take = std::min(c.qwc - off, tag_left);
            off += take;
            tag_left -= take;
            if (tag_left == 0 && seen_eop) break;
        }
        const std::uint32_t n = off - start;
        if (off >= c.qwc) {
            ++g_pend3_chunk_head;
            g_pend3_chunk_off = 0;
            if (!c.ram) g_pend3_buf_head = c.buf_off + static_cast<std::size_t>(c.qwc) * 16;
        } else {
            g_pend3_chunk_off = off;
        }
        const std::uint32_t h = c.hash ? ((start == 0 && n == c.qwc) ? c.hash : ((c.hash ^ (start * 0x9E3779B1u) ^ (n * 0x85EBCA6Bu)) | 1u)) : 0u;
        transfer_direct(3, base + static_cast<std::size_t>(start) * 16, n, h, c.ram != nullptr);
        if (tag_left == 0 && seen_eop) break;
    }
    pend3_reset_if_empty();
}
#else
std::vector<std::uint8_t> g_pend3;
std::size_t g_pend3_head = 0;   // bytes already sent: moving what is left down on every packet took 200 MB of copying per picture
bool pend3_empty() { return g_pend3.empty(); }
std::size_t pend3_qwords() { return g_pend3.size() / 16; }

// Length in qwords of the GIF packet (tags up to and including the next one with EOP) at the front of the queue.
'''),
        (r'''    }
}
}

void flush_path3() {
    if (gs::queue::vu_active() && gs::queue::role() != gs::queue::Role::Vu) {
        gs::queue::post(gs::queue::kPath3Flush, 0, 0, nullptr, 0);
        return;
    }
    static int shown = 0;
    if (!g_pend3.empty() && shown++ < 20)
        std::fprintf(stderr, "[path3] %zu qwords still waiting at vblank (masked %u, released %u)\n", g_pend3.size() / 16, (mask_up() ? 1u : 0u), g_released ? 1u : 0u);
    if (g_released || !mask_up()) {
        release(true);
        g_released = false;
''',
         r'''    }
}
#endif
}

void flush_path3() {
    if (gs::queue::vu_active() && !gs::queue::is_vu_thread()) {
        gs::queue::post(gs::queue::kPath3Flush, 0, 0, nullptr, 0);
        return;
    }
    static int shown = 0;
    if (!pend3_empty() && shown++ < 20)
        std::fprintf(stderr, "[path3] %zu qwords still waiting at vblank (masked %u, released %u)\n", pend3_qwords(), (mask_up() ? 1u : 0u), g_released ? 1u : 0u);
    if (g_released || !mask_up()) {
        release(true);
        g_released = false;
    }
}

void release_if_pending() {
    if (g_released) {
        release(true);
        g_released = false;
'''),
        ('''// The VIF1 chain ended while PATH3 was left unmasked: the GIF had the rest of the frame to drain its queue.
void chain_end() {
    if (gs::queue::vu_active() && gs::queue::role() != gs::queue::Role::Vu) {
        gs::queue::post(gs::queue::kPath3ChainEnd, 0, 0, nullptr, 0);
        return;
''',
         '''// The VIF1 chain ended while PATH3 was left unmasked: the GIF had the rest of the frame to drain its queue.
void chain_end() {
    if (gs::queue::vu_active() && !gs::queue::is_vu_thread()) {
        gs::queue::post(gs::queue::kPath3ChainEnd, 0, 0, nullptr, 0);
        return;
'''),
        ('''}

void transfer(int path, const std::uint8_t *data, std::uint32_t qwc) {
    if (path == 3 && gs::queue::vu_active() && gs::queue::role() != gs::queue::Role::Vu) {
        transfer_direct(path, data, qwc);
        return;
    }
''',
         '''}

void transfer_impl(int path, const std::uint8_t *data, std::uint32_t qwc, std::uint32_t known_hash, bool ram_backed) {
    if (path == 3 && gs::queue::vu_active() && !gs::queue::is_vu_thread()) {
        transfer_direct(path, data, qwc, known_hash, ram_backed);
        return;
    }
'''),
        ('''        g_released = false;
    }
    if (path == 3 && (mask_up() || !g_pend3.empty())) {
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
''',
         '''        g_released = false;
    }
    if (path == 3 && (mask_up() || !pend3_empty())) {
#ifdef PS2_PICA
        if (g_pend3_chunk_head > 64 && g_pend3_chunk_head > g_pend3_chunks.size() / 2) {
            g_pend3_chunks.erase(g_pend3_chunks.begin(), g_pend3_chunks.begin() + static_cast<std::ptrdiff_t>(g_pend3_chunk_head));
            g_pend3_chunk_head = 0;
        }
        if (ram_backed) {
            g_pend3_chunks.push_back(Pend3Chunk{data, 0u, qwc, known_hash});
            return;
        }
        constexpr std::size_t kPend3Limit = 2u << 20;
        const std::size_t incoming = static_cast<std::size_t>(qwc) * 16;
        if (!g_pend3_buf.capacity()) g_pend3_buf.reserve(kPend3Limit);
        if (g_pend3_buf.size() - g_pend3_buf_head + incoming > kPend3Limit) release(true);
        if (incoming > kPend3Limit) {
            transfer_direct(path, data, qwc, known_hash, false);
            return;
        }
        if ((g_pend3_buf_head > g_pend3_buf.size() / 2 || g_pend3_buf.size() + incoming > kPend3Limit) && g_pend3_buf_head > 0) {
            g_pend3_buf.erase(g_pend3_buf.begin(), g_pend3_buf.begin() + static_cast<std::ptrdiff_t>(g_pend3_buf_head));
            for (std::size_t i = g_pend3_chunk_head; i < g_pend3_chunks.size(); ++i)
                if (!g_pend3_chunks[i].ram) g_pend3_chunks[i].buf_off -= static_cast<std::uint32_t>(g_pend3_buf_head);
            g_pend3_buf_head = 0;
        }
        const std::uint32_t off = static_cast<std::uint32_t>(g_pend3_buf.size());
        g_pend3_buf.insert(g_pend3_buf.end(), data, data + incoming);
        g_pend3_chunks.push_back(Pend3Chunk{nullptr, off, qwc, known_hash});
        return;
#else
        bool compact = g_pend3_head > g_pend3.size() / 2;   // only now and then, and only here, where the vector may move anyway
        if (compact) {
            g_pend3.erase(g_pend3.begin(), g_pend3.begin() + static_cast<std::ptrdiff_t>(g_pend3_head));
'''),
        (r'''        { static const bool t = std::getenv("PS2_TRACE_M3") != nullptr; if (t) std::fprintf(stderr, "[p3] queued %u qwords, %zu waiting\n", qwc, g_pend3.size() / 16); }
        return;
    }
    transfer_direct(path, data, qwc);
}

void vu_path3(const std::uint8_t *data, std::uint32_t qwc) { transfer(3, data, qwc); }

void transfer_direct(int path, const std::uint8_t *data, std::uint32_t qwc) {
    { static const bool t = std::getenv("PS2_TRACE_M3") != nullptr; if (t) std::fprintf(stderr, "[gifx] path=%d qwc=%u\n", path, qwc); }
    if (!gs::queue::active()) {
        render(path, data, qwc);
        return;
    }
    if (gs::queue::role() != gs::queue::Role::Vu) scan(g_scan[path], data, qwc);
#ifdef PS2_PICA
    if (gs::queue::role() == gs::queue::Role::Vu) {   // the 3DS has no third thread: the VU thread also draws
        render(path, data, qwc);
        return;
    }
#endif
#ifdef PS2_PICA
    // Texture data for a skipped picture is read from PS2 memory by the VU thread, not copied: see run_skipped in vif.cpp. Each
    // cache line of this copy costs about 230 cycles, nearly all of them waiting on memory.
    // A shown picture's goes the same way if the game has sent the same bytes from the same place several times running.
    if (path == 3 && data >= g_ram && data + qwc * 16u <= g_ram + kRamSize &&
        ((qwc >= 256 && gs::queue::g_ee_skip) || (qwc >= 16 && gs::queue::vu_active() && gs::queue::unchanging(data, qwc * 16u)))) {
        gs::queue::post(gs::queue::kGifAddress, 3, qwc, &data, sizeof data);
        return;
    }
#endif
''',
         r'''        { static const bool t = std::getenv("PS2_TRACE_M3") != nullptr; if (t) std::fprintf(stderr, "[p3] queued %u qwords, %zu waiting\n", qwc, g_pend3.size() / 16); }
        return;
#endif
    }
    transfer_direct(path, data, qwc, known_hash, ram_backed);
}

void transfer(int path, const std::uint8_t *data, std::uint32_t qwc) { transfer_impl(path, data, qwc, 0, false); }
#ifdef PS2_PICA
void transfer_ram(int path, const std::uint8_t *data, std::uint32_t qwc) { transfer_impl(path, data, qwc, 0x80000000u, true); }
#endif

void vu_path3(const std::uint8_t *data, std::uint32_t qwc, std::uint32_t known_hash, bool ram_backed) {
    transfer_impl(3, data, qwc, known_hash, ram_backed);
}

void transfer_direct(int path, const std::uint8_t *data, std::uint32_t qwc, std::uint32_t known_hash, bool ram_backed) {
    { static const bool t = std::getenv("PS2_TRACE_M3") != nullptr; if (t) std::fprintf(stderr, "[gifx] path=%d qwc=%u\n", path, qwc); }
    if (!gs::queue::active()) {
        render_chunk(path, data, qwc, known_hash, ram_backed);
        return;
    }
    if (gs::queue::is_vu_thread()) {
#ifdef PS2_PICA
        render_chunk(path, data, qwc, known_hash, ram_backed);
        return;
#endif
    } else {
        scan(g_scan[path], data, qwc);
    }
#ifdef PS2_PICA
    // Texture data for a skipped picture is read from PS2 memory by the VU thread, not copied: see run_skipped in vif.cpp. Each
    // cache line of this copy costs about 230 cycles, nearly all of them waiting on memory.
    // A shown picture's goes the same way if the game has sent the same bytes from the same place several times running.
    if (path == 3 && data >= g_ram && data + qwc * 16u <= g_ram + kRamSize) {
        const bool skip_ref = qwc >= 256 && gs::queue::g_ee_skip;
        if (skip_ref) {
            gs::queue::post(gs::queue::kGifAddress, 0u, qwc, &data, sizeof data);
            return;
        }
        if (qwc >= 4 && gs::queue::vu_active()) {
            const bool stable = gs::queue::unchanging(data, qwc * 16u);
            const std::uint32_t raw = gs::queue::g_unchanging_hash;
            const std::uint32_t hash = raw ? ((raw & 0x3FFFFFFFu) ? (raw & 0x3FFFFFFFu) : 1u) : 0u;
            if (stable) {
                gs::queue::post(gs::queue::kGifAddress, hash, qwc, &data, sizeof data);
                return;
            }
            if (hash) {
                gs::queue::post(gs::queue::kGif, 3u | (hash << 2), qwc, data, qwc * 16);
                return;
            }
        }
    }
#endif
'''),
        ('''void set_path3_masked(bool masked) {
    if (gs::queue::vu_active()) {
        if (gs::queue::role() != gs::queue::Role::Vu) {
            g_stat = (g_stat & ~2u) | (masked ? 2u : 0u);  // EE thread: only the status bit the CPU reads
            return;
''',
         '''void set_path3_masked(bool masked) {
    if (gs::queue::vu_active()) {
        if (!gs::queue::is_vu_thread()) {
            g_stat = (g_stat & ~2u) | (masked ? 2u : 0u);  // EE thread: only the status bit the CPU reads
            return;
'''),
    ],
    'runtime/src/gs/gif.hpp': [
        ('''void stats();
void transfer(int path, const std::uint8_t *data, std::uint32_t qwc);
void render(int path, const std::uint8_t *data, std::uint32_t qwc);
void render_reset();
''',
         '''void stats();
void transfer(int path, const std::uint8_t *data, std::uint32_t qwc);
#ifdef PS2_PICA
void transfer_ram(int path, const std::uint8_t *data, std::uint32_t qwc);
#endif
void render(int path, const std::uint8_t *data, std::uint32_t qwc);
void render_reset();
'''),
        ('''// Release everything still waiting behind the PATH3 mask when it is off (called at vertical blank)
void flush_path3();
// Called when a VIF1 DMA chain finishes
void chain_end();
// VU thread: PATH3 data posted by the EE thread, passed through the mask rule
void vu_path3(const std::uint8_t *data, std::uint32_t qwc);

}
''',
         '''// Release everything still waiting behind the PATH3 mask when it is off (called at vertical blank)
void flush_path3();
void release_if_pending();
// Called when a VIF1 DMA chain finishes
void chain_end();
// VU thread: PATH3 data posted by the EE thread, passed through the mask rule
void vu_path3(const std::uint8_t *data, std::uint32_t qwc, std::uint32_t known_hash = 0, bool ram_backed = false);

}
'''),
    ],
    'runtime/src/gs/gs.cpp': [
        ('''    }
}
#endif

unsigned long long g_up_bytes = 0, g_up_nz = 0;

void feed_image(const std::uint8_t *data, std::size_t bytes) {
    PS2_PROF(ps2::prof::Upload);
#ifdef PS2_PICA
    if (pica::active()) pica::note_data(data, bytes);
#endif
#ifndef PS2_PICA   // a count for tracing that cost more than the upload itself
''',
         '''    }
}

struct DeferredUpload {
    bool active = false;
    bool full_pages = false;
    std::uint16_t first_block = 0, end_block = 0;
    std::uint16_t dsax = 0, dsay = 0, rrw = 0, rrh = 0;
    std::uint32_t psm = 0;
    std::uint32_t first_page = 0, page_count = 0;
    std::uint32_t vblank_age = 0;
    std::uint64_t seq = 0;
    std::uint64_t bitbltbuf = 0, trxpos = 0, trxreg = 0;
    const std::uint8_t *data = nullptr;
    std::size_t bytes = 0;
};

constexpr int kMaxDeferred = 128;
DeferredUpload g_deferred[kMaxDeferred];
std::uint32_t g_def_range[kMaxDeferred]{};   // packed first_block | (end_block << 16), 0 if inactive (512 B SoA)
std::uint64_t g_page_max_seq[512]{};
std::int8_t g_clut_slot[256];
bool g_clut_slot_init = false;
std::uint32_t g_deferred_mask[16]{};
std::uint32_t g_deferred_count = 0;
int g_deferred_hi = 0;
std::uint32_t g_deferred_vblank = 0;
std::uint64_t g_deferred_seq = 0;

void psm_page_dims(std::uint32_t psm, std::uint32_t &pw, std::uint32_t &ph) {
    if (psm == PSMT8) { pw = 128; ph = 64; }
    else if (psm == PSMT4) { pw = 128; ph = 128; }
    else if (psm == PSMCT16 || psm == PSMCT16S || psm == PSMZ16 || psm == PSMZ16S) { pw = 64; ph = 64; }
    else { pw = 64; ph = 32; }
}

void upload_block_range(std::uint32_t psm, std::uint32_t dbp, std::uint32_t dbw, std::uint32_t dsax, std::uint32_t dsay,
                        std::uint32_t rrw, std::uint32_t rrh, std::uint32_t &first_block, std::uint32_t &end_block) {
    std::uint32_t pw, ph;
    psm_page_dims(psm, pw, ph);
    const std::uint32_t fb_byte = page_address(dbp, dbw, dsax, dsay, pw, ph);
    const std::uint32_t lb_byte = page_address(dbp, dbw, dsax + rrw - 1u, dsay + rrh - 1u, pw, ph);
    first_block = fb_byte >> 8;
    if (fb_byte == lb_byte && dsax == 0 && dsay == 0 && rrw <= pw && rrh <= ph) {
        int max_b = 31;
        if (psm == PSMCT32 || psm == PSMCT24) {
            const int bx = static_cast<int>((rrw - 1u) >> 3), by = static_cast<int>((rrh - 1u) >> 3);
            max_b = 0;
            for (int y = 0; y <= by; ++y)
                for (int x = 0; x <= bx; ++x) max_b = std::max(max_b, kBlock32[y][x]);
        } else if (psm == PSMCT16 || psm == PSMCT16S) {
            const auto &kb = psm == PSMCT16 ? kBlock16 : kBlock16S;
            const int bx = static_cast<int>((rrw - 1u) >> 4), by = static_cast<int>((rrh - 1u) >> 3);
            max_b = 0;
            for (int y = 0; y <= by; ++y)
                for (int x = 0; x <= bx; ++x) max_b = std::max(max_b, kb[y][x]);
        } else if (psm == PSMT8) {
            const int bx = static_cast<int>((rrw - 1u) >> 4), by = static_cast<int>((rrh - 1u) >> 4);
            max_b = 0;
            for (int y = 0; y <= by; ++y)
                for (int x = 0; x <= bx; ++x) max_b = std::max(max_b, kBlock32[y][x]);
        } else if (psm == PSMT4) {
            const int bx = static_cast<int>((rrw - 1u) >> 5), by = static_cast<int>((rrh - 1u) >> 4);
            max_b = 0;
            for (int y = 0; y <= by; ++y)
                for (int x = 0; x <= bx; ++x) max_b = std::max(max_b, kBlock16[y][x]);
        }
        end_block = first_block + static_cast<std::uint32_t>(max_b) + 1u;
    } else {
        end_block = (lb_byte >> 8) + 32u;
    }
}

void set_deferred_bits(std::uint32_t first, std::uint32_t count) {
    const std::uint32_t end = std::min<std::uint32_t>(first + count, 512u);
    for (std::uint32_t p = first; p < end; ++p) g_deferred_mask[p >> 5] |= 1u << (p & 31u);
}

void rebuild_deferred_mask() {
    std::memset(g_deferred_mask, 0, sizeof g_deferred_mask);
    int hi = 0;
    if (!g_deferred_count) {
        g_deferred_hi = 0;
        return;
    }
    for (int i = 0; i < g_deferred_hi; ++i) {
        if (g_deferred[i].active) {
            set_deferred_bits(g_deferred[i].first_page, g_deferred[i].page_count);
            hi = i + 1;
        }
    }
    g_deferred_hi = hi;
}

bool any_deferred_in(std::uint32_t first, std::uint32_t count) {
    if (!g_deferred_count || !count) return false;
    const std::uint32_t end = std::min<std::uint32_t>(first + count, 512u);
    for (std::uint32_t p = first; p < end; ++p)
        if (g_deferred_mask[p >> 5] & (1u << (p & 31u))) return true;
    return false;
}

void materialize_deferred(int idx) {
    DeferredUpload &u = g_deferred[idx];
    u.active = false;
    g_def_range[idx] = 0u;
    --g_deferred_count;
    const std::uint64_t saved_buf = g_state.bitbltbuf, saved_pos = g_state.trxpos, saved_reg = g_state.trxreg;
    const Transfer saved_tr = g_transfer;
    const std::size_t saved_carried = g_carried;
    g_state.bitbltbuf = u.bitbltbuf;
    g_state.trxpos = u.trxpos;
    g_state.trxreg = u.trxreg;
    g_transfer = Transfer{true, 0, 0};
    g_carried = 0;
    image_fast(u.psm, u.data, u.bytes);
    g_state.bitbltbuf = saved_buf;
    g_state.trxpos = saved_pos;
    g_state.trxreg = saved_reg;
    g_transfer = saved_tr;
    g_carried = saved_carried;
}

void flush_deferred_blocks(std::uint32_t first_block, std::uint32_t end_block) {
    if (!g_deferred_count || first_block >= end_block) return;
    const std::uint32_t clamped_end = std::min<std::uint32_t>(end_block, 16384u);
    if (first_block >= clamped_end) return;
    const std::uint32_t fp = first_block >> 5, lp = (clamped_end - 1u) >> 5;
    if (!any_deferred_in(fp, lp - fp + 1u)) return;
    int order[kMaxDeferred];
    int n = 0;
    for (int i = 0; i < g_deferred_hi; ++i) {
        const std::uint32_t r = g_def_range[i];
        if (r && (r & 0xFFFFu) < clamped_end && (r >> 16) > first_block) order[n++] = i;
    }
    if (!n) return;
    if (n > 1) std::sort(order, order + n, [](int a, int b) { return g_deferred[a].seq < g_deferred[b].seq; });
    for (int i = 0; i < n; ++i) materialize_deferred(order[i]);
    rebuild_deferred_mask();
}

void flush_deferred_range(std::uint32_t first, std::uint32_t count) {
    if (!count) return;
    const std::uint32_t end = std::min<std::uint32_t>(first + count, 512u);
    flush_deferred_blocks(first * 32u, end * 32u);
}

void clear_all_deferred() {
    for (int i = 0; i < g_deferred_hi; ++i) {
        g_deferred[i].active = false;
        g_def_range[i] = 0u;
    }
    std::memset(g_deferred_mask, 0, sizeof g_deferred_mask);
    std::memset(g_clut_slot, -1, sizeof g_clut_slot);
    g_clut_slot_init = true;
    g_deferred_count = 0;
    g_deferred_hi = 0;
}

void flush_all_deferred_impl() {
    if (!g_deferred_count) return;
    flush_deferred_blocks(0u, 16384u);
}

bool defer_or_prepare_image(std::uint32_t psm, std::uint32_t bits, const std::uint8_t *data, std::size_t bytes, std::uint32_t known_hash, bool ram_backed) {
    const std::uint64_t buf = g_state.bitbltbuf, pos = g_state.trxpos, reg = g_state.trxreg;
    const std::uint32_t dbp = static_cast<std::uint32_t>(buf >> 32) & 0x3FFFu;
    const std::uint32_t dbw = static_cast<std::uint32_t>(buf >> 48) & 0x3Fu;
    const std::uint32_t dsax = static_cast<std::uint32_t>(pos >> 32) & 0x7FFu;
    const std::uint32_t dsay = static_cast<std::uint32_t>(pos >> 48) & 0x7FFu;
    const std::uint32_t rrw = static_cast<std::uint32_t>(reg) & 0xFFFu;
    const std::uint32_t rrh = static_cast<std::uint32_t>(reg >> 32) & 0xFFFu;
    if (!rrw || !rrh) return false;
    std::uint32_t pw, ph;
    psm_page_dims(psm, pw, ph);
    const bool no_wrap = dsax + rrw <= 2048u && dsay + rrh <= 2048u;
    std::uint32_t first_block = 0, end_block = 16384u;
    if (no_wrap) upload_block_range(psm, dbp, dbw, dsax, dsay, rrw, rrh, first_block, end_block);
    const bool contig = no_wrap && end_block > first_block && end_block <= 16384u;
    const std::uint32_t first_page = contig ? (first_block >> 5) : 0u;
    const std::uint32_t last_page = contig ? ((end_block - 1u) >> 5) : 511u;
    const std::uint32_t page_count = contig ? (last_page - first_page + 1u) : 512u;

    // Defer static unchanging g_ram texture and CLUT uploads (>= 64 B) that complete in one chunk.
    const bool can_defer = ram_backed && known_hash != 0 && contig && pica::active() && g_transfer.x == 0 && g_transfer.y == 0 && g_carried == 0 &&
                           bytes >= 64u &&
                           (psm == PSMT8 || psm == PSMT4 || psm == PSMCT32 || psm == PSMCT24 || psm == PSMCT16 || psm == PSMCT16S) &&
                           bytes * 8u >= static_cast<std::size_t>(rrw) * rrh * bits;
    if (!can_defer) {
        if (g_deferred_count) {
            if (contig) flush_deferred_blocks(first_block, end_block);
            else flush_all_deferred_impl();
        }
        return false;
    }

    if (!g_clut_slot_init) {
        std::memset(g_clut_slot, -1, sizeof g_clut_slot);
        g_clut_slot_init = true;
    }
    const std::uint32_t stride_pages = std::max<std::uint32_t>(std::max<std::uint32_t>(dbw, 1u) * 64u / pw, 1u);
    const bool full_pages = (dbp % 32u == 0) && dsax == 0 && dsay == 0 && (rrw % pw == 0) && (rrh % ph == 0) &&
                            (rrw / pw == stride_pages) &&
                            (static_cast<std::size_t>(rrw) * rrh * bits == static_cast<std::size_t>(page_count) * 65536u);
    if (any_deferred_in(first_page, page_count)) {
        bool dropped = false;
        for (int i = 0; i < g_deferred_hi; ++i) {
            const std::uint32_t r = g_def_range[i];
            if (!r || (r & 0xFFFFu) >= end_block || (r >> 16) <= first_block) continue;
            DeferredUpload &u = g_deferred[i];
            const bool covered = (full_pages && first_block <= u.first_block && end_block >= u.end_block) ||
                                 (u.bitbltbuf == buf && u.trxpos == pos && u.trxreg == reg) ||
                                 (u.first_block == first_block && u.end_block == end_block && u.psm == psm && u.dsax == dsax && u.dsay == dsay && u.rrw == rrw && u.rrh == rrh);
            if (covered) {
                u.active = false;
                g_def_range[i] = 0u;
                --g_deferred_count;
                dropped = true;
            }
        }
        if (dropped) rebuild_deferred_mask();
    }
    if (g_deferred_count >= kMaxDeferred) {
        int oldest = -1;
        for (int i = 0; i < g_deferred_hi; ++i)
            if (g_def_range[i] && (oldest < 0 || g_deferred[i].seq < g_deferred[oldest].seq)) oldest = i;
        if (oldest >= 0) {
            materialize_deferred(oldest);
            rebuild_deferred_mask();
        }
    }
    for (int i = 0; i < kMaxDeferred; ++i) {
        if (!g_def_range[i]) {
            const std::uint64_t seq = ++g_deferred_seq;
            g_deferred[i] = DeferredUpload{true, full_pages, static_cast<std::uint16_t>(first_block), static_cast<std::uint16_t>(end_block),
                                           static_cast<std::uint16_t>(dsax), static_cast<std::uint16_t>(dsay),
                                           static_cast<std::uint16_t>(rrw), static_cast<std::uint16_t>(rrh),
                                           psm, first_page, page_count, g_deferred_vblank, seq, buf, pos, reg, data, bytes};
            g_def_range[i] = first_block | (end_block << 16);
            if (i + 1 > g_deferred_hi) g_deferred_hi = i + 1;
            ++g_deferred_count;
            set_deferred_bits(first_page, page_count);
            for (std::uint32_t p = first_page; p < first_page + page_count && p < 512u; ++p) g_page_max_seq[p] = seq;
            if (dsax == 0 && dsay == 0 && ((rrw == 16 && rrh == 16) || (rrw == 8 && rrh == 2)))
                g_clut_slot[first_block & 255u] = static_cast<std::int8_t>(i);
            g_transfer.active = false;
            return true;
        }
    }
    return false;
}
#endif

unsigned long long g_up_bytes = 0, g_up_nz = 0;

void feed_image(const std::uint8_t *data, std::size_t bytes, std::uint32_t known_hash = 0, bool ram_backed = false) {
    PS2_PROF(ps2::prof::Upload);
#ifdef PS2_PICA
    if (pica::active()) pica::note_data(data, bytes, known_hash == 0x80000000u ? 0u : known_hash);
#else
    (void)known_hash, (void)ram_backed;
#endif
#ifndef PS2_PICA   // a count for tracing that cost more than the upload itself
'''),
        ('''    const std::uint32_t bits = bits_per_pixel(psm);
#ifdef PS2_PICA
    if (psm == PSMT8 || psm == PSMT4 || psm == PSMCT32 || psm == PSMCT24 || psm == PSMCT16 || psm == PSMCT16S) {
        image_fast(psm, data, bytes);
''',
         '''    const std::uint32_t bits = bits_per_pixel(psm);
#ifdef PS2_PICA
    if (defer_or_prepare_image(psm, bits, data, bytes, known_hash, ram_backed)) return;
    if (psm == PSMT8 || psm == PSMT4 || psm == PSMCT32 || psm == PSMCT24 || psm == PSMCT16 || psm == PSMCT16S) {
        image_fast(psm, data, bytes);
'''),
        ('''
void local_copy() {
    const std::uint64_t buf = g_state.bitbltbuf, pos = g_state.trxpos, reg = g_state.trxreg;
    const std::uint32_t sbp = static_cast<std::uint32_t>(buf) & 0x3FFFu, sbw = static_cast<std::uint32_t>(buf >> 16) & 0x3Fu, spsm = static_cast<std::uint32_t>(buf >> 24) & 0x3Fu;
''',
         '''
void local_copy() {
#ifdef PS2_PICA
    flush_all_deferred_impl();
#endif
    const std::uint64_t buf = g_state.bitbltbuf, pos = g_state.trxpos, reg = g_state.trxreg;
    const std::uint32_t sbp = static_cast<std::uint32_t>(buf) & 0x3FFFu, sbw = static_cast<std::uint32_t>(buf >> 16) & 0x3Fu, spsm = static_cast<std::uint32_t>(buf >> 24) & 0x3Fu;
'''),
        ('''    if (!cpu_raster()) return;
    const bool eight = psm == PSMT8 || psm == PSMT8H;
    const std::uint32_t entries = eight ? 256u : 16u;
    const std::uint32_t texclut = static_cast<std::uint32_t>(g_state.texclut);
''',
         '''    if (!cpu_raster()) return;
    const bool eight = psm == PSMT8 || psm == PSMT8H;
    const std::uint32_t eff_cpsm = cpsm == 0 ? PSMCT32 : cpsm;
#ifdef PS2_PICA
    const std::uint32_t clut_blocks = csm ? 64u : ((eff_cpsm == PSMCT32 || eff_cpsm == PSMCT24) ? (eight ? 4u : 1u) : (eight ? 2u : 1u));
    const std::uint32_t q_end = std::min<std::uint32_t>(cbp + clut_blocks, 16384u);
    if (!csm && g_deferred_count && (eff_cpsm == PSMCT32 || eff_cpsm == PSMCT16 || eff_cpsm == PSMCT16S)) {
        const std::uint32_t fp = cbp >> 5, lp = (q_end - 1u) >> 5;
        if (any_deferred_in(fp, lp - fp + 1u)) {
            int best = -1;
            const int hint = g_clut_slot_init ? static_cast<int>(g_clut_slot[cbp & 255u]) : -1;
            if (hint >= 0 && g_def_range[hint] && g_deferred[hint].first_block == cbp &&
                g_deferred[hint].seq == g_page_max_seq[fp] && (fp == lp || g_deferred[hint].seq == g_page_max_seq[lp])) {
                best = hint;
            } else {
                std::uint64_t best_seq = 0;
                for (int i = 0; i < g_deferred_hi; ++i) {
                    const std::uint32_t r = g_def_range[i];
                    if (r && (r & 0xFFFFu) < q_end && (r >> 16) > cbp && g_deferred[i].seq > best_seq) {
                        best = i;
                        best_seq = g_deferred[i].seq;
                    }
                }
            }
            if (best >= 0) {
                const DeferredUpload &u = g_deferred[best];
                const std::uint32_t need_w = eight ? 16u : 8u, need_h = eight ? 16u : 2u;
                if (u.first_block == cbp && u.dsax == 0 && u.dsay == 0 && u.psm == eff_cpsm && u.rrw == need_w && u.rrh == need_h) {
                    if (eff_cpsm == PSMCT32) {
                        if (eight) {
                            for (std::uint32_t g = 0; g < 256u; g += 32u) {
                                std::memcpy(g_state.clut + g + 0u,  u.data + (g + 0u) * 4u, 32u);
                                std::memcpy(g_state.clut + g + 8u,  u.data + (g + 16u) * 4u, 32u);
                                std::memcpy(g_state.clut + g + 16u, u.data + (g + 8u) * 4u, 32u);
                                std::memcpy(g_state.clut + g + 24u, u.data + (g + 24u) * 4u, 32u);
                            }
                        } else {
                            std::memcpy(g_state.clut + (csa & 15u) * 16u, u.data, 64u);
                        }
                    } else {
                        if (eight) {
                            for (std::uint32_t i = 0; i < 256u; ++i) {
                                const std::uint32_t j = (i & ~0x18u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1);
                                std::uint16_t v;
                                std::memcpy(&v, u.data + j * 2u, 2u);
                                g_state.clut[i] = v;
                            }
                        } else {
                            std::uint32_t *dst = g_state.clut + (csa & 15u) * 16u;
                            for (std::uint32_t i = 0; i < 16u; ++i) {
                                std::uint16_t v;
                                std::memcpy(&v, u.data + i * 2u, 2u);
                                dst[i] = v;
                            }
                        }
                    }
                    return;
                }
            }
        }
    }
    flush_deferred_blocks(cbp, q_end);
#endif
    const std::uint32_t entries = eight ? 256u : 16u;
    const std::uint32_t texclut = static_cast<std::uint32_t>(g_state.texclut);
'''),
        ('''
void render_reset() {
#ifdef PS2_GPU
    g_seen_ranges.clear();
''',
         '''
void render_reset() {
#ifdef PS2_PICA
    clear_all_deferred();
#endif
#ifdef PS2_GPU
    g_seen_ranges.clear();
'''),
        ('''}

void kick(bool draw) {
    State &s = g_state;
''',
         '''}

int draw_g3_direct(const ::g3::Input &in, ::g3::Gpu &g, const ::g3::BatchPlan &plan) {   // ps2run_3ds_patch.py
#ifdef PS2_PICA
    if (!pica::active() || !pica::g3_ready()) return -1;
    if (!queue::g_skip_picture) {
        const int r = pica::draw_g3_direct(g_state, gpu_clamp(g_state), in, g, plan);
        if (r != 0) return r;
    }
    ++g_primitives;
    return 0;
#else
    (void)in; (void)g; (void)plan;
    return -1;
#endif
}

bool draw_im2d_tris(const std::uint8_t *vu_mem, std::uint32_t top, std::uint32_t count, bool xyzf) {   // ps2run_3ds_patch.py
#ifdef PS2_PICA
    if (!pica::active()) return false;
    if (!queue::g_skip_picture && !pica::draw_im2d_tris(g_state, gpu_clamp(g_state), vu_mem, top, count, xyzf)) return false;
    g_primitives += count;
    return true;
#else
    (void)vu_mem; (void)top; (void)count; (void)xyzf;
    return false;
#endif
}

void kick(bool draw) {
    State &s = g_state;
'''),
        ('''    ++g_writes[reg & 127u];
    constexpr std::uint64_t kVertexRegs = (1ull << 0x01) | (1ull << 0x02) | (1ull << 0x03) | (1ull << 0x04) | (1ull << 0x05) | (1ull << 0x0A) | (1ull << 0x0C) | (1ull << 0x0D);
    if (reg >= 64 || !((kVertexRegs >> reg) & 1u)) ++g_setup_generation;
    static const char *seq = std::getenv("PS2_TRACE_GSSEQ");
    if (seq) {
''',
         '''    ++g_writes[reg & 127u];
    constexpr std::uint64_t kVertexRegs = (1ull << 0x01) | (1ull << 0x02) | (1ull << 0x03) | (1ull << 0x04) | (1ull << 0x05) | (1ull << 0x0A) | (1ull << 0x0C) | (1ull << 0x0D);
    if (reg == 0x00) {
        if ((s.prim & ~7ull) != (value & ~7ull)) ++g_setup_generation;
    } else if (reg >= 64 || !((kVertexRegs >> reg) & 1u)) {
        ++g_setup_generation;
    }
    static const char *seq = std::getenv("PS2_TRACE_GSSEQ");
    if (seq) {
'''),
        ('''
#ifdef PS2_PICA
// PACKED loops that only set vertex registers make up nearly all of a 3D scene. They skip write_packed and write, and so the
// trip through two switches and the tracing checks for each register.
''',
         '''
#ifdef PS2_PICA
void write_image(const std::uint8_t *data, std::uint32_t qwords, std::uint32_t known_hash, bool ram_backed) {
    feed_image(data, static_cast<std::size_t>(qwords) * 16, known_hash, ram_backed);
}

void upload_blocks(std::uint32_t psm, std::uint32_t dbp, std::uint32_t dbw, std::uint32_t dsax, std::uint32_t dsay,
                   std::uint32_t rrw, std::uint32_t rrh, std::uint32_t &first_block, std::uint32_t &end_block) {
    if (!rrw || !rrh) {
        first_block = dbp;
        end_block = dbp;
        return;
    }
    upload_block_range(psm, dbp, dbw, dsax, dsay, rrw, rrh, first_block, end_block);
}

void flush_deferred_pages(std::uint32_t first_page, std::uint32_t page_count) {
    flush_deferred_range(first_page, page_count);
}

void flush_all_deferred() {
    flush_all_deferred_impl();
}

void age_deferred_uploads() {
    ++g_deferred_vblank;
}
#endif

#ifdef PS2_PICA
// PACKED loops that only set vertex registers make up nearly all of a 3D scene. They skip write_packed and write, and so the
// trip through two switches and the tracing checks for each register.
'''),
        ('''
void render_read_image(std::uint8_t *data, std::uint32_t qwords) {
    const std::size_t bytes = static_cast<std::size_t>(qwords) * 16;
    std::memset(data, 0, bytes);
''',
         '''
void render_read_image(std::uint8_t *data, std::uint32_t qwords) {
#ifdef PS2_PICA
    flush_all_deferred_impl();
#endif
    const std::size_t bytes = static_cast<std::size_t>(qwords) * 16;
    std::memset(data, 0, bytes);
'''),
        ('''std::uint32_t g_max_vu_lag = 10;

void vblank(bool start) {
    if (start) {
''',
         '''std::uint32_t g_max_vu_lag = 10;

void note_vblank_aborted() {
    g_vblanks_served.fetch_add(1);
    queue::notify(g_vblanks_served);
}

void vblank(bool start) {
    if (start) {
'''),
        ('''            queue::post(queue::kVblank, static_cast<std::uint32_t>(ps2::frame_count()), message.picture, &message, sizeof message);
            ++g_vblanks_posted;
            if (g_max_vu_lag && g_vblanks_posted - g_vblanks_served.load() > g_max_vu_lag) {
                PS2_PROF(ps2::prof::Full);   // counts as a full queue: the picture skipping goes by that
                for (std::uint32_t done = g_vblanks_served.load(); g_vblanks_posted - done > g_max_vu_lag; done = g_vblanks_served.load())
                    queue::wait(g_vblanks_served, done);
            }
''',
         '''            queue::post(queue::kVblank, static_cast<std::uint32_t>(ps2::frame_count()), message.picture, &message, sizeof message);
            ++g_vblanks_posted;
            if (g_max_vu_lag && !ps2::vu_aborting() && g_vblanks_posted - g_vblanks_served.load() > g_max_vu_lag) {
                PS2_PROF(ps2::prof::Full);   // counts as a full queue: the picture skipping goes by that
                for (std::uint32_t done = g_vblanks_served.load(); !ps2::vu_aborting() && g_vblanks_posted - done > g_max_vu_lag; done = g_vblanks_served.load())
                    queue::wait(g_vblanks_served, done);
            }
'''),
        ('''void render_state(ps2::state::Stream &s) {
    flush_and_wait();
    s.pod(g_state);
    ++g_setup_generation;
''',
         '''void render_state(ps2::state::Stream &s) {
    flush_and_wait();
#ifdef PS2_PICA
    if (s.saving()) flush_all_deferred_impl();
    else clear_all_deferred();
#endif
    s.pod(g_state);
    ++g_setup_generation;
'''),
    ],
    'runtime/src/gs/gs.hpp': [
        ('''}

namespace g3 { struct Gpu; }   // ps2run_3ds_patch.py

namespace ps2::gs {
''',
         '''}

namespace g3 { struct Gpu; struct Input; struct BatchPlan; }   // ps2run_3ds_patch.py

namespace ps2::gs {
'''),
        ('''void write(std::uint32_t reg, std::uint64_t value);
bool draw_g3(const ::g3::Gpu &g);   // ps2run_3ds_patch.py: false when the PICA renderer cannot take it (PRIM already set)
void signal(std::uint32_t reg, std::uint64_t value);
void note_transfer(std::uint32_t reg, std::uint64_t value);
void write_packed(std::uint32_t reg, const Reg128 &q);
void write_image(const std::uint8_t *data, std::uint32_t qwords);
bool packed_vertex_regs(std::uint64_t regs, std::uint32_t nreg, std::uint8_t *kinds);   // ps2run_3ds_patch.py
void packed_run(const std::uint8_t *kinds, std::uint32_t nreg, std::uint32_t loops, const std::uint8_t *data);
''',
         '''void write(std::uint32_t reg, std::uint64_t value);
bool draw_g3(const ::g3::Gpu &g);   // ps2run_3ds_patch.py: false when the PICA renderer cannot take it (PRIM already set)
int draw_g3_direct(const ::g3::Input &in, ::g3::Gpu &g, const ::g3::BatchPlan &plan);   // ps2run_3ds_patch.py: zero-copy into ring (0 ok, -1 cmd full, >0 GpuDecline)
bool draw_im2d_tris(const std::uint8_t *vu_mem, std::uint32_t top, std::uint32_t count, bool xyzf);   // ps2run_3ds_patch.py
void signal(std::uint32_t reg, std::uint64_t value);
void note_transfer(std::uint32_t reg, std::uint64_t value);
void write_packed(std::uint32_t reg, const Reg128 &q);
void write_image(const std::uint8_t *data, std::uint32_t qwords);
#ifdef PS2_PICA
void write_image(const std::uint8_t *data, std::uint32_t qwords, std::uint32_t known_hash, bool ram_backed);
void upload_blocks(std::uint32_t psm, std::uint32_t dbp, std::uint32_t dbw, std::uint32_t dsax, std::uint32_t dsay,
                   std::uint32_t rrw, std::uint32_t rrh, std::uint32_t &first_block, std::uint32_t &end_block);
void flush_deferred_pages(std::uint32_t first_page, std::uint32_t page_count);
void flush_all_deferred();
void age_deferred_uploads();
#endif
bool packed_vertex_regs(std::uint64_t regs, std::uint32_t nreg, std::uint8_t *kinds);   // ps2run_3ds_patch.py
void packed_run(const std::uint8_t *kinds, std::uint32_t nreg, std::uint32_t loops, const std::uint8_t *data);
'''),
        ('''void write_privileged(std::uint32_t address, std::uint64_t value);
void vblank(bool start);
extern std::uint32_t g_max_vu_lag;   // ps2run_3ds_patch.py: vblanks the drawing side may be behind; 0 for no limit
bool interlaced_odd_field();
''',
         '''void write_privileged(std::uint32_t address, std::uint64_t value);
void vblank(bool start);
void note_vblank_aborted();
extern std::uint32_t g_max_vu_lag;   // ps2run_3ds_patch.py: vblanks the drawing side may be behind; 0 for no limit
bool interlaced_odd_field();
'''),
    ],
    'runtime/src/gs/queue.cpp': [
        ('''}

Ring g_render, g_vu;
thread_local Role g_role = Role::Emulation;

''',
         '''}

Ring g_render;
thread_local Role g_role = Role::Emulation;

'''),
        ('''    const std::uint8_t *data;
    std::uint32_t bytes, hash, sample;
    std::uint8_t same, rest;
};
Unchanging g_unchanging[16384];
''',
         '''    const std::uint8_t *data;
    std::uint32_t bytes, hash, sample;
    std::uint16_t rest;
    std::uint8_t same;
};
Unchanging g_unchanging[16384];
'''),
        ('''namespace {

// Four words: the first, the last, and two between (blocks are whole quadwords, at least 16 bytes).
std::uint32_t sample(const std::uint8_t *data, std::uint32_t bytes) {
    const std::uint32_t q = (bytes >> 2) & ~3u;
    std::uint32_t a, b, c, d;
    std::memcpy(&a, data, 4);
    std::memcpy(&b, data + q, 4);
    std::memcpy(&c, data + q * 2 + 4, 4);
    std::memcpy(&d, data + bytes - 4, 4);
    return a ^ (b * 3u) ^ (c * 5u) ^ (d * 7u);
}
}
''',
         '''namespace {

// Two words: the first and the last (blocks are whole quadwords, at least 16 bytes) — halves L1D misses on Core 0.
std::uint32_t sample(const std::uint8_t *data, std::uint32_t bytes) {
    std::uint32_t a, d;
    std::memcpy(&a, data, 4);
    std::memcpy(&d, data + bytes - 4, 4);
    return a ^ (d * 7u);
}
}
'''),
        ('''    return false;
#else
    Unchanging &e = g_unchanging[(static_cast<std::uint32_t>(data - g_ram) >> 4) * 2654435761u >> 18];
    const bool known = e.data == data && e.bytes == bytes;
    if (known && e.rest && sample(data, bytes) == e.sample) {
''',
         '''    return false;
#else
    const std::uint32_t off = static_cast<std::uint32_t>(data - g_ram);
    const std::uint32_t first = off >> 12, last = (off + bytes - 1u) >> 12;
    if (first == last) {
        if (g_rewritten[first >> 5] & (1u << (first & 31u))) {
            g_unchanging_hash = 0;
            return false;
        }
    } else {
        for (std::uint32_t p = first; p <= last; ++p)
            if (g_rewritten[p >> 5] & (1u << (p & 31u))) {
                g_unchanging_hash = 0;
                return false;
            }
    }
    Unchanging &e = g_unchanging[(off >> 4) * 2654435761u >> 18];
    const bool known = e.data == data && e.bytes == bytes;
    if (known && e.rest && sample(data, bytes) == e.sample) {
'''),
        ('''        return true;
    }
    const std::uint32_t first = static_cast<std::uint32_t>(data - g_ram) >> 12, last = static_cast<std::uint32_t>(data - g_ram + bytes - 1) >> 12;
    for (std::uint32_t p = first; p <= last; ++p)
        if (g_rewritten[p >> 5] & (1u << (p & 31u))) return false;
    std::uint32_t h = 2166136261u;
    for (std::uint32_t i = 0; i + 4 <= bytes; i += 4) {
''',
         '''        return true;
    }
    std::uint32_t h = 2166136261u;
    for (std::uint32_t i = 0; i + 4 <= bytes; i += 4) {
'''),
        ('''        h = (h ^ w) * 16777619u;
    }
    if (known && h == e.hash) {
        if (e.same < 3) ++e.same;
        if (e.same >= 3) {
            e.rest = static_cast<std::uint8_t>(24u + (h >> 28));
            e.sample = sample(data, bytes);
        }
    } else {
        if (known) rewritten(data, bytes);
        e = Unchanging{data, bytes, h, 0, 0, 0};
    }
''',
         '''        h = (h ^ w) * 16777619u;
    }
    g_unchanging_hash = h ? h : 1u;
    if (known && h == e.hash) {
        if (e.same < 3) ++e.same;
        if (e.same >= 3) {
            e.rest = static_cast<std::uint16_t>(1000u + (h >> 24));
            e.sample = sample(data, bytes);
            return true;
        }
    } else {
        if (known) {
            rewritten(data, bytes);
            g_unchanging_hash = 0;
        }
        e = Unchanging{data, bytes, h, 0, 0, 0};
    }
'''),
        ('''}

Ring &render() { return g_render; }
Ring &vu() { return g_vu; }
Role &role() { return g_role; }

Ring &outbox() {
    if (g_role == Role::Emulation && g_vu.active()) return g_vu;
    return g_render;
}

bool active() { return g_render.active(); }
bool vu_active() { return g_vu.active(); }

void post(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes) { outbox().post(type, arg0, arg1, data, bytes); }
''',
         '''}

Ring g_vu;
bool g_vu_active = false;
#ifdef __3DS__
void *g_vu_tls = nullptr;
#endif

Ring &render() { return g_render; }
Role &role() { return g_role; }
void set_vu_thread() {
    g_role = Role::Vu;
#ifdef __3DS__
    void *tls;
    asm("mrc p15, 0, %0, c13, c0, 3" : "=r"(tls));
    g_vu_tls = tls;
#endif
}

Ring &outbox() {
    if (g_vu_active && !is_vu_thread() && g_role == Role::Emulation) return g_vu;
    return g_render;
}

bool active() { return g_render.active(); }

void post(Type type, std::uint32_t arg0, std::uint32_t arg1, const void *data, std::uint32_t bytes) { outbox().post(type, arg0, arg1, data, bytes); }
'''),
        ('''    buffer_ = std::make_unique<std::uint8_t[]>(capacity);
    capacity_ = static_cast<std::uint32_t>(capacity);
}

''',
         '''    buffer_ = std::make_unique<std::uint8_t[]>(capacity);
    capacity_ = static_cast<std::uint32_t>(capacity);
    if (this == &g_vu) g_vu_active = true;
}

'''),
        ('''}

std::uint8_t *Ring::extend(Type type, std::uint32_t bytes) {
    if (open_bytes_ + bytes > kOpenBytes) close();
    if (!open_bytes_) {
        open_at_ = room(sizeof(Header) + kOpenBytes);
''',
         '''}

std::uint8_t *Ring::extend_slow(Type type, std::uint32_t bytes) {
    if (open_bytes_ && (open_type_ != type || open_bytes_ + bytes > kOpenBytes)) close();
    if (!open_bytes_) {
        open_at_ = room(sizeof(Header) + kOpenBytes);
'''),
    ],
    'runtime/src/gs/queue.hpp': [
        ('''    // A message built a little at a time, in place, by the one thread that posts. It goes when full or when anything else is posted.
    static constexpr std::uint32_t kOpenBytes = 16u << 10;
    std::uint8_t *extend(Type type, std::uint32_t bytes);
    void close();
    bool peek(Header &header, const std::uint8_t *&payload);
''',
         '''    // A message built a little at a time, in place, by the one thread that posts. It goes when full or when anything else is posted.
    static constexpr std::uint32_t kOpenBytes = 16u << 10;
    inline std::uint8_t *extend(Type type, std::uint32_t bytes) {
        if (open_bytes_ && open_type_ == type && open_bytes_ + bytes <= kOpenBytes) {
            std::uint8_t *at = open_at_ + sizeof(Header) + open_bytes_;
            open_bytes_ += bytes;
            return at;
        }
        return extend_slow(type, bytes);
    }
    void close();
    bool peek(Header &header, const std::uint8_t *&payload);
'''),
        ('''
private:
    std::uint8_t *room(std::uint32_t need);
    void publish(std::uint32_t need);
''',
         '''
private:
    std::uint8_t *extend_slow(Type type, std::uint32_t bytes);
    std::uint8_t *room(std::uint32_t need);
    void publish(std::uint32_t need);
'''),
        ('''};

Ring &render();
Ring &vu();
Role &role();
Ring &outbox();

bool active();
bool vu_active();
extern bool g_ee_skip;        // ps2run_3ds_patch.py: the same for the game thread, which is ahead of the drawing side
#ifdef PS2_PICA
''',
         '''};

extern Ring g_vu;
extern bool g_vu_active;
#ifdef __3DS__
extern void *g_vu_tls;
#endif

Ring &render();
inline Ring &vu() { return g_vu; }
Role &role();
void set_vu_thread();
inline bool is_vu_thread() {
#ifdef __3DS__
    void *tls;
    asm("mrc p15, 0, %0, c13, c0, 3" : "=r"(tls));
    return tls == g_vu_tls;
#else
    return role() == Role::Vu;
#endif
}
Ring &outbox();

bool active();
inline bool vu_active() { return g_vu_active; }
extern bool g_ee_skip;        // ps2run_3ds_patch.py: the same for the game thread, which is ahead of the drawing side
#ifdef PS2_PICA
'''),
    ],
    'runtime/src/system.cpp': [
        ('''
void sync_vu_thread() {
    if (!gs::queue::vu_active()) return;
    const std::uint32_t seen = g_vu_fence.load();
    gs::queue::vu().post(gs::queue::kVuFence, 0, 0, nullptr, 0);
    while (g_vu_fence.load() == seen) gs::queue::wait(g_vu_fence, seen);
}

''',
         '''
void sync_vu_thread() {
    if (!gs::queue::vu_active() || vu_aborting()) return;
    const std::uint32_t seen = g_vu_fence.load();
    gs::queue::vu().post(gs::queue::kVuFence, 0, 0, nullptr, 0);
    while (!vu_aborting() && g_vu_fence.load() == seen) gs::queue::wait(g_vu_fence, seen);
}

'''),
        ('''void init_graphics() { gs::init(); }
void start_render_thread() { gs::start_thread(); }
#ifdef __3DS__
void start_vu_thread() { gs::queue::vu().start(7u << 20); }   // 7 MB: enough for a whole picture to wait while the last one is drawn
void stop_vu_thread() { gs::queue::vu().post(gs::queue::kVuStop, 0, 0, nullptr, 0); }
#else
void start_vu_thread() { gs::queue::vu().start(32u << 20); }
#endif

''',
         '''void init_graphics() { gs::init(); }
void start_render_thread() { gs::start_thread(); }
std::atomic<bool> g_vu_abort{false};
bool vu_aborting() { return g_vu_abort.load(std::memory_order_acquire); }
void abort_vu_thread() {
    g_vu_abort.store(true, std::memory_order_release);
    gs::note_vblank_aborted();
    g_vu_fence.fetch_add(1);
    gs::queue::notify(g_vu_fence);
}
#ifdef __3DS__
void start_vu_thread() { g_vu_abort.store(false, std::memory_order_release); gs::queue::vu().start(7u << 20); }   // 7 MB: enough for a whole picture to wait while the last one is drawn
void stop_vu_thread() { g_vu_abort.store(true, std::memory_order_release); gs::queue::vu().post(gs::queue::kVuStop, 0, 0, nullptr, 0); }
#else
void start_vu_thread() { g_vu_abort.store(false, std::memory_order_release); gs::queue::vu().start(32u << 20); }
#endif

'''),
        ('''
void run_vu_thread() {
    gs::queue::role() = gs::queue::Role::Vu;
    gs::queue::Ring &in = gs::queue::vu();
    for (;;) {
''',
         '''
void run_vu_thread() {
    gs::queue::set_vu_thread();
    gs::queue::Ring &in = gs::queue::vu();
    for (;;) {
'''),
        ('''        const std::uint8_t *payload = nullptr;
        while (in.peek(h, payload)) {
            switch (h.type) {
            case gs::queue::kVif1: vif::transfer(1, payload, h.arg0); break;
''',
         '''        const std::uint8_t *payload = nullptr;
        while (in.peek(h, payload)) {
            if (g_vu_abort.load(std::memory_order_relaxed)) {
                if (h.type == gs::queue::kVuStop) {
#ifdef PS2_PICA
                    if (gs::queue::g_split_pica) gs::queue::render().post(gs::queue::kVuStop, 0, 0, nullptr, 0);
#endif
                    in.pop(h);
                    return;
                }
                if (h.type == gs::queue::kVuFence) {
                    g_vu_fence.fetch_add(1);
                    gs::queue::notify(g_vu_fence);
                }
                if (h.type == gs::queue::kVblank) gs::note_vblank_aborted();
                in.pop(h);
                continue;
            }
            switch (h.type) {
            case gs::queue::kVif1: vif::transfer(1, payload, h.arg0); break;
'''),
        ('''                const std::uint8_t *data;
                std::memcpy(&data, payload, sizeof data);
                gif::vu_path3(data, h.arg1);
                break;
            }
''',
         '''                const std::uint8_t *data;
                std::memcpy(&data, payload, sizeof data);
                gif::vu_path3(data, h.arg1, h.arg0, true);
                break;
            }
'''),
        ('''                break;
            case gs::queue::kGif:
                if (h.arg0 == 3) {
                    PS2_PROF_EXTRA(5);
                    gif::vu_path3(payload, h.arg1);
                }
#ifdef PS2_PICA
                else {
                    PS2_PROF_EXTRA(6);
                    gif::render(static_cast<int>(h.arg0), payload, h.arg1);
                }
#else
                else gs::queue::render().post(static_cast<gs::queue::Type>(h.type), h.arg0, h.arg1, payload, h.bytes);
#endif
                break;
''',
         '''                break;
            case gs::queue::kGif:
#ifdef PS2_PICA
                if ((h.arg0 & 3u) == 3u) {
                    PS2_PROF_EXTRA(5);
                    gif::vu_path3(payload, h.arg1, h.arg0 >> 2, false);
                } else {
                    PS2_PROF_EXTRA(6);
                    gif::render(static_cast<int>(h.arg0 & 3u), payload, h.arg1);
                }
#else
                if (h.arg0 == 3) {
                    PS2_PROF_EXTRA(5);
                    gif::vu_path3(payload, h.arg1);
                } else gs::queue::render().post(static_cast<gs::queue::Type>(h.type), h.arg0, h.arg1, payload, h.bytes);
#endif
                break;
'''),
    ],
    'runtime/src/vif.cpp': [
        ('''// stop, the PATH3 mask, data for the GIF, or an unpack in mode 2, which adds to the row registers). g_wrote: which registers, as kW bits.
bool g_side = false;
std::uint32_t g_wrote = 0;
Unit g_units[2];
''',
         '''// stop, the PATH3 mask, data for the GIF, or an unpack in mode 2, which adds to the row registers). g_wrote: which registers, as kW bits.
bool g_side = false;
bool g_skip_unsafe = false;
std::uint32_t g_wrote = 0;
Unit g_units[2];
'''),
        ('''std::uint8_t g_dry_run[16384];

bool on_vu_thread() { return gs::queue::role() == gs::queue::Role::Vu; }
bool shadow(int unit) { return unit == 1 && gs::queue::vu_active() && !on_vu_thread(); }
Unit &state(int unit) { return unit == 1 && on_vu_thread() ? g_vu_side : g_units[unit]; }
''',
         '''std::uint8_t g_dry_run[16384];

inline bool on_vu_thread() { return gs::queue::is_vu_thread(); }
bool shadow(int unit) { return unit == 1 && gs::queue::vu_active() && !on_vu_thread(); }
Unit &state(int unit) { return unit == 1 && on_vu_thread() ? g_vu_side : g_units[unit]; }
'''),
        ('''}

void run_vu(int unit, Unit &u, bool is_shadow, std::uint32_t address, bool resume = false) {
    if (unit == 1) {
''',
         '''}

#ifdef PS2_PICA
struct DeferredUnpack {
    const std::uint8_t *src;
    std::uint16_t addr, num;
    std::uint8_t vn, vl, cl, rel;
    bool usn, in_ram;
};
DeferredUnpack g_def_unpack[8];
int g_def_count = 0;
std::uint16_t g_def_tops = 0;
std::uint8_t g_def_cl = 0, g_def_slot_mask = 0;
std::uint32_t g_def_min = 1024u, g_def_max = 0u;
bool g_def_any_non_ram = false;

struct DeferredMpg {
    const std::uint8_t *src;
    std::uint16_t first, bytes;
};
constexpr int kMaxDefMpg = 32;
DeferredMpg g_def_mpg[kMaxDefMpg];
int g_def_mpg_count = 0;
std::uint64_t g_def_mpg_pages = 0;

struct MpgCacheEntry {
    std::uint64_t sig = 0;
    std::uint64_t pages = 0;
    std::uint32_t address = 0;
    std::uint8_t count = 0;
    std::uintptr_t fn = 0;
};
constexpr int kMpgCacheSize = 64;
MpgCacheEntry g_mpg_cache[kMpgCacheSize];
struct LastMpg {
    std::uint32_t pc = ~0u;
    std::uintptr_t fn = 0;
};
LastMpg g_last_mpg[2];
#endif

void run_vu(int unit, Unit &u, bool is_shadow, std::uint32_t address, bool resume = false) {
    if (unit == 1) {
'''),
        ('''        vu::set_tops(u.top, u.itop);
#ifdef PS2_PICA
        // A picture that will not be shown needs no vertices. Uploads and registers all still happen, so the next picture finds
        // the same machine state, and a program not seen before runs in case it is one that sets things up once.
        const bool skip = gs::queue::g_split_pica ? gs::queue::g_vu_skip_picture : gs::queue::g_skip_picture;
        if (skip && (resume || vu::seen(address))) return;
        if (skip) {
            gs::queue::g_quiet = true;   // its input was left out, so what it draws would be wrong
            vu::run(unit, address);
''',
         '''        vu::set_tops(u.top, u.itop);
#ifdef PS2_PICA
        if (g_def_count && g_def_tops != u.top) flush_deferred_unpacks();
        // A picture that will not be shown needs no vertices. Uploads and registers all still happen, so the next picture finds
        // the same machine state, and a program not seen before runs in case it is one that sets things up once.
        const bool skip = gs::queue::g_split_pica ? gs::queue::g_vu_skip_picture : gs::queue::g_skip_picture;
        if (skip && (resume || vu::seen(address))) {
            discard_deferred_unpacks();
            return;
        }
        if (skip) {
            flush_deferred_unpacks();
            gs::queue::g_quiet = true;   // its input was left out, so what it draws would be wrong
            vu::run(unit, address);
'''),
        ('''    const std::uint32_t wrap = vu_qwords(unit);
    if (!masked && u.mode == 0 && !(wl > cl) && !(vn == 3 && vl == 3)) {
        if (vn == 3 && vl == 0 && cl == wl) {
            // V4-32 with no gaps writes the payload verbatim. At most one wrap is possible.
''',
         '''    const std::uint32_t wrap = vu_qwords(unit);
    if (!masked && u.mode == 0 && !(wl > cl) && !(vn == 3 && vl == 3)) {
#ifdef PS2_PICA
        if (unit == 1 && !dry) {
            if (((cmd >> 15) & 1u) && wl == 1u && (cl == 4u || cl == 5u)) {
                const std::uint32_t rel = cmd & 0x3FFu;
                const std::uint32_t full_addr = (rel + u.tops) & 1023u;
                if (rel < cl && full_addr + (num - 1u) * cl < 0x2B8u) {
                    if (g_def_count && (g_def_tops != u.tops || g_def_cl != cl || g_def_count >= 8 || (g_def_slot_mask & (1u << rel))))
                        flush_deferred_unpacks();
                    const auto *bytes_ptr = reinterpret_cast<const std::uint8_t *>(data);
                    const std::size_t byte_len = static_cast<std::size_t>(num) * components * (bits / 8u);
                    const bool in_ram = bytes_ptr >= g_ram && bytes_ptr + byte_len <= g_ram + kRamSize;
                    g_def_unpack[g_def_count++] = DeferredUnpack{bytes_ptr, static_cast<std::uint16_t>(full_addr), static_cast<std::uint16_t>(num),
                                                                 static_cast<std::uint8_t>(vn), static_cast<std::uint8_t>(vl), static_cast<std::uint8_t>(cl),
                                                                 static_cast<std::uint8_t>(rel), usn, in_ram};
                    g_def_tops = static_cast<std::uint16_t>(u.tops);
                    g_def_cl = static_cast<std::uint8_t>(cl);
                    g_def_slot_mask |= static_cast<std::uint8_t>(1u << rel);
                    g_def_min = std::min<std::uint32_t>(g_def_min, full_addr);
                    g_def_max = std::max<std::uint32_t>(g_def_max, full_addr + (num - 1u) * cl + 1u);
                    g_def_any_non_ram = g_def_any_non_ram || !in_ram;
                    u.num = 0;
                    return;
                }
            }
            if (g_def_count) {
                if (cl == wl && (addr & 1023u) + num <= 1024u) {
                    const std::uint32_t w0 = addr & 1023u, w1 = w0 + num;
                    if (w0 < g_def_max && w1 > g_def_min) flush_deferred_unpacks();
                } else {
                    flush_deferred_unpacks();
                }
            }
        }
#endif
        if (vn == 3 && vl == 0 && cl == wl) {
            // V4-32 with no gaps writes the payload verbatim. At most one wrap is possible.
'''),
        ('''        return;
    }
    for (std::uint32_t n = 0; n < num; ++n) {
        const bool fill = wl > cl && cycle >= cl;
''',
         '''        return;
    }
#ifdef PS2_PICA
    if (unit == 1 && !dry) {
        if (g_def_count) flush_deferred_unpacks();
        // Fast pointer-based path for mode == 0 masked or filling unpacks (eliminates per-element bit_pos div/mod & memcpy)
        if (u.mode == 0 && !(vn == 3 && vl == 3)) {
            const std::uint32_t wrap_mask = wrap - 1u;
            const std::uint8_t *src = reinterpret_cast<const std::uint8_t *>(data);
            if (masked && vn == 3 && vl == 0 && cl == 4 && wl == 4 && u.mask == 0xC000C000u && (addr & wrap_mask) + num <= wrap) {
                std::uint8_t *dst = mem + (addr & wrap_mask) * 16u;
                std::uint32_t n = 0;
                for (; n + 1u < num; n += 2u, dst += 32u, src += 32u) std::memcpy(dst, src, 28u);
                if (n < num) std::memcpy(dst, src, 16u);
                u.num = 0;
                return;
            }
            const std::uint32_t elem_bytes = components * (bits >> 3);
            const bool zero_zw = v2_zero(), keep_zw = v2_keep();
            const bool filling = wl > cl;
            for (std::uint32_t n = 0; n < num; ++n) {
                const bool fill = filling && cycle >= cl;
                auto *q = reinterpret_cast<std::uint32_t *>(mem + (addr & wrap_mask) * 16u);
                if (!masked) {
                    if (fill) {
                        q[0] = u.row[0]; q[1] = u.row[1]; q[2] = u.row[2]; q[3] = u.row[3];
                    } else if (vl == 0 && vn == 3) {
                        std::memcpy(q, src, 16u);
                        src += 16u;
                    } else {
                        std::uint32_t v[4];
                        if (vl == 0) {
                            for (std::uint32_t j = 0; j < components; ++j) std::memcpy(&v[j], src + j * 4u, 4u);
                        } else if (vl == 1) {
                            for (std::uint32_t j = 0; j < components; ++j) {
                                std::uint16_t h;
                                std::memcpy(&h, src + j * 2u, 2u);
                                v[j] = usn ? h : static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(h)));
                            }
                        } else {
                            for (std::uint32_t j = 0; j < components; ++j)
                                v[j] = usn ? src[j] : static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(src[j])));
                        }
                        src += elem_bytes;
                        if (components == 1) { v[1] = v[2] = v[3] = v[0]; }
                        else if (components == 2) {
                            if (keep_zw) { v[2] = q[2]; v[3] = q[3]; }
                            else if (zero_zw) { v[2] = v[3] = 0u; }
                            else { v[2] = v[0]; v[3] = v[1]; }
                        } else if (components == 3) { v[3] = 0u; }
                        q[0] = v[0]; q[1] = v[1]; q[2] = v[2]; q[3] = v[3];
                    }
                } else {
                    std::uint32_t v[4] = {0u, 0u, 0u, 0u};
                    if (!fill) {
                        if (vl == 0) {
                            for (std::uint32_t j = 0; j < components; ++j) std::memcpy(&v[j], src + j * 4u, 4u);
                        } else if (vl == 1) {
                            for (std::uint32_t j = 0; j < components; ++j) {
                                std::uint16_t h;
                                std::memcpy(&h, src + j * 2u, 2u);
                                v[j] = usn ? h : static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(h)));
                            }
                        } else {
                            for (std::uint32_t j = 0; j < components; ++j)
                                v[j] = usn ? src[j] : static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(src[j])));
                        }
                        src += elem_bytes;
                        if (components == 1) { v[1] = v[2] = v[3] = v[0]; }
                        else if (components == 2) {
                            if (zero_zw) { v[2] = v[3] = 0u; }
                            else { v[2] = v[0]; v[3] = v[1]; }
                        }
                    }
                    const std::uint32_t mrow = cycle < 3u ? cycle : 3u;
                    const std::uint32_t mbyte = (u.mask >> (mrow * 8u)) & 0xFFu;
                    for (std::uint32_t j = 0; j < 4u; ++j) {
                        std::uint32_t m = (mbyte >> (j * 2u)) & 3u;
                        if (fill && m == 0u) m = 1u;
                        if ((!fill && components == 2u && j >= 2u && keep_zw) || m == 3u) continue;
                        q[j] = m == 1u ? u.row[j] : (m == 2u ? u.col[mrow] : v[j]);
                    }
                }
                ++addr;
                ++cycle;
                if (!filling) {
                    if (cycle >= wl) { addr += cl - wl; cycle = 0u; }
                } else if (cycle >= wl) {
                    cycle = 0u;
                }
            }
            u.num = 0;
            return;
        }
    }
#endif
    for (std::uint32_t n = 0; n < num; ++n) {
        const bool fill = wl > cl && cycle >= cl;
'''),
        ('''            }
        }
        std::uint8_t *q = mem + (addr % wrap) * 16;
        const std::uint32_t mrow = std::min<std::uint32_t>(cycle, 3u);
        for (std::uint32_t j = 0; j < 4; ++j) {
''',
         '''            }
        }
        std::uint8_t *q = mem + (addr & (wrap - 1u)) * 16;
        const std::uint32_t mrow = std::min<std::uint32_t>(cycle, 3u);
        for (std::uint32_t j = 0; j < 4; ++j) {
'''),
        ('''}

void execute(int unit, Unit &u, bool from_vu, bool is_shadow, std::uint32_t cmd, const std::uint32_t *data) {
    const std::uint32_t op = (cmd >> 24) & 0x7Fu, imm = cmd & 0xFFFFu, num = (cmd >> 16) & 0xFFu;
    if (is_shadow) {
        if ((cmd & 0x80000000u) || op == 0x06 || op == 0x50 || op == 0x51) g_side = true;
        switch (op) {
        case 0x01: g_wrote |= 1u; break;
''',
         '''}

template <bool FromVu, bool IsShadow>
void execute(int unit, Unit &u, std::uint32_t cmd, const std::uint32_t *data) {
    const std::uint32_t op = (cmd >> 24) & 0x7Fu, imm = cmd & 0xFFFFu, num = (cmd >> 16) & 0xFFu;
    if constexpr (IsShadow) {
        if ((cmd & 0x80000000u) || op == 0x06 || op == 0x50 || op == 0x51) g_side = true;
        if (op == 0x4A || op == 0x14 || op == 0x15 || ((op & 0x60u) == 0x60u && !(cmd & 0x8000u))) g_skip_unsafe = true;
        switch (op) {
        case 0x01: g_wrote |= 1u; break;
'''),
        ('''    }
    u.code = cmd;
    if (!from_vu) ++g_commands[unit][op];
    switch (op) {
    case 0x00: break;
''',
         '''    }
    u.code = cmd;
    if constexpr (!FromVu) ++g_commands[unit][op];
    switch (op) {
    case 0x00: break;
'''),
        (r'''    case 0x07: u.mark = imm; break;
    case 0x10: case 0x11: case 0x13: break;
    case 0x14: case 0x15: run_vu(unit, u, is_shadow, imm * 8u); break;
    case 0x17: run_vu(unit, u, is_shadow, is_shadow ? 0u : vu::tpc(unit), true); break;
    case 0x20: u.mask = data[0]; break;
    case 0x30: std::memcpy(u.row, data, 16); break;
    case 0x31: std::memcpy(u.col, data, 16); break;
    case 0x4A: {
        if (is_shadow) break;
        std::uint8_t *code = unit ? g_vu1_code : g_vu0_code;
        const std::uint32_t size = unit ? 16384u : 4096u;
        const std::uint32_t count = num ? num : 256u;
        const std::uint32_t at_first = (imm * 8u) & (size - 1);
        if (at_first + count * 8u <= size) {
            if (std::memcmp(code + at_first, data, count * 8u) == 0) break;
            // This game swaps programs in and out many times a picture. A block that does not wrap is copied in one piece, and the
            // program cache is told which part changed.
            std::memcpy(code + at_first, data, count * 8u);
            vu::code_changed(unit, at_first, count * 8u);
            break;
        }
        bool changed = false;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint8_t *at = code + ((imm * 8u + i * 8u) & (size - 1));
            if (std::memcmp(at, data + i * 2, 8) == 0) continue;
            std::memcpy(at, data + i * 2, 8);
            changed = true;
        }
        if (changed) vu::code_changed(unit);
        break;
    }
    case 0x50: case 0x51:
        if (is_shadow) gif::scan_signals(2, reinterpret_cast<const std::uint8_t *>(data), imm ? imm : 0x10000u);
        else gif::transfer(2, reinterpret_cast<const std::uint8_t *>(data), imm ? imm : 0x10000u);
        break;
    default:
        if ((op & 0x60u) == 0x60u) {
            if (is_shadow && u.mode != 2) u.num = 0;
            else unpack(unit, u, is_shadow, cmd, data);
        }
        else std::fprintf(stderr, "[vif%d] unknown command 0x%08X\n", unit, cmd);
''',
         r'''    case 0x07: u.mark = imm; break;
    case 0x10: case 0x11: case 0x13: break;
    case 0x14: case 0x15: run_vu(unit, u, IsShadow, imm * 8u); break;
    case 0x17: run_vu(unit, u, IsShadow, IsShadow ? 0u : vu::tpc(unit), true); break;
    case 0x20: u.mask = data[0]; break;
    case 0x30: std::memcpy(u.row, data, 16); break;
    case 0x31: std::memcpy(u.col, data, 16); break;
    case 0x4A: {
        if constexpr (IsShadow) break;
        else {
            std::uint8_t *code = unit ? g_vu1_code : g_vu0_code;
            const std::uint32_t size = unit ? 16384u : 4096u;
            const std::uint32_t count = num ? num : 256u;
            const std::uint32_t at_first = (imm * 8u) & (size - 1);
            const std::uint32_t bytes = count * 8u;
#ifdef PS2_PICA
            if (unit == 1 && at_first + bytes <= size) {
                const auto *src = reinterpret_cast<const std::uint8_t *>(data);
                if (src >= g_ram && src + bytes <= g_ram + kRamSize) {
                    int w = 0;
                    for (int i = 0; i < g_def_mpg_count; ++i) {
                        const DeferredMpg &d = g_def_mpg[i];
                        if (at_first <= d.first && at_first + bytes >= static_cast<std::uint32_t>(d.first) + d.bytes) continue;
                        if (w != i) g_def_mpg[w] = d;
                        ++w;
                    }
                    g_def_mpg_count = w;
                    if (g_def_mpg_count >= kMaxDefMpg) flush_deferred_mpg();
                    g_def_mpg[g_def_mpg_count++] = DeferredMpg{src, static_cast<std::uint16_t>(at_first), static_cast<std::uint16_t>(bytes)};
                    for (std::uint32_t p = at_first >> 8; p <= (at_first + bytes - 1u) >> 8; ++p)
                        g_def_mpg_pages |= std::uint64_t{1} << p;
                    g_last_mpg[0].fn = g_last_mpg[1].fn = 0;
                    break;
                }
            }
            if (unit == 1 && g_def_mpg_count) flush_deferred_mpg();
#endif
            if (at_first + bytes <= size) {
                if (std::memcmp(code + at_first, data, bytes) == 0) break;
                // This game swaps programs in and out many times a picture. A block that does not wrap is copied in one piece, and the
                // program cache is told which part changed.
                std::memcpy(code + at_first, data, bytes);
                vu::code_changed(unit, at_first, bytes);
                break;
            }
            bool changed = false;
            for (std::uint32_t i = 0; i < count; ++i) {
                std::uint8_t *at = code + ((imm * 8u + i * 8u) & (size - 1));
                if (std::memcmp(at, data + i * 2, 8) == 0) continue;
                std::memcpy(at, data + i * 2, 8);
                changed = true;
            }
            if (changed) vu::code_changed(unit);
            break;
        }
    }
    case 0x50: case 0x51: {
        const std::uint32_t qwc = imm ? imm : 0x10000u;
        const auto *p = reinterpret_cast<const std::uint8_t *>(data);
        if constexpr (IsShadow) gif::scan_signals(2, p, qwc);
#ifdef PS2_PICA
        else if (unit == 1 && p >= g_ram && p + static_cast<std::size_t>(qwc) * 16u <= g_ram + kRamSize)
            gif::transfer_ram(2, p, qwc);
#endif
        else gif::transfer(2, p, qwc);
        break;
    }
    default:
        if ((op & 0x60u) == 0x60u) {
            if constexpr (IsShadow) {
                if (u.mode != 2) u.num = 0;
                else unpack(unit, u, true, cmd, data);
            } else {
                unpack(unit, u, false, cmd, data);
            }
        }
        else std::fprintf(stderr, "[vif%d] unknown command 0x%08X\n", unit, cmd);
'''),
        ('''        u.stat |= 0xC00u;
        u.stalled = true;
        if (!from_vu) hw::intc_raise(4u + static_cast<unsigned>(unit));
    }
}

std::size_t run(int unit, Unit &u, bool from_vu, bool is_shadow, const std::uint32_t *words, std::size_t count) {
    const std::uint32_t *at = words;
    const std::uint32_t *const end = words + count;
''',
         '''        u.stat |= 0xC00u;
        u.stalled = true;
        if constexpr (!FromVu) hw::intc_raise(4u + static_cast<unsigned>(unit));
    }
}

template <bool FromVu, bool IsShadow>
std::size_t run(int unit, Unit &u, const std::uint32_t *words, std::size_t count) {
    const std::uint32_t *at = words;
    const std::uint32_t *const end = words + count;
'''),
        ('''            continue;
        }
        const std::uint32_t need = data_words(u, cmd);
        if (static_cast<std::size_t>(end - at) < need) {
''',
         '''            continue;
        }
        if constexpr (FromVu || IsShadow) {
            if (!(cmd & 0x80000000u)) {
                const std::uint32_t op = cmd >> 24;
                if (op <= 0x13u) {
                    u.code = cmd;
                    const std::uint32_t imm = cmd & 0xFFFFu;
                    switch (op) {
                    case 0x01: u.cycle = imm; continue;
                    case 0x02: u.ofst = imm & 0x3FFu; u.dbf = false; u.tops = u.base; continue;
                    case 0x03: u.base = imm & 0x3FFu; continue;
                    case 0x04: u.itops = imm & 0x3FFu; continue;
                    case 0x05: u.mode = imm & 3u; continue;
                    case 0x07: u.mark = imm; continue;
                    case 0x10: case 0x11: case 0x13: continue;
                    default: break;
                    }
                }
            }
        }
        const std::uint32_t need = data_words(u, cmd);
        if (static_cast<std::size_t>(end - at) < need) {
'''),
        ('''            break;
        }
        execute(unit, u, from_vu, is_shadow, cmd, at);
        at += need;
    }
''',
         '''            break;
        }
        execute<FromVu, IsShadow>(unit, u, cmd, at);
        at += need;
    }
'''),
        ('''                       kWItop = 512, kWMask = 1024, kWRow = 2048, kWCol = 4096, kWNum = 8192, kWCode = 16384 };
struct Key {
    std::uint32_t cycle, mode, base, ofst, tops, itops, dbf;
    bool operator==(const Key &o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
};
Key key_of(const Unit &u) { return Key{u.cycle, u.mode, u.base, u.ofst, u.tops, u.itops, u.dbf ? 1u : 0u}; }
struct Replay {
    const std::uint8_t *data;
    std::uint32_t qwc, wrote;
    Key before;
    std::uint32_t cycle, ofst, tops, base, itops, mode, mark, top, itop, mask, row[4], col[4], num, code;
    std::uint32_t records, words, epoch;   // what to send the VU thread: g_records[records], words long, if epoch is still g_epoch
    std::uint32_t hash;   // of the bytes then: the same place can hold other unchanging data later (a new area's models)
    bool dbf;
};
void record(Replay &r, const Unit &u) {
    r.cycle = u.cycle; r.ofst = u.ofst; r.tops = u.tops; r.base = u.base; r.itops = u.itops; r.mode = u.mode; r.mark = u.mark;
    r.top = u.top; r.itop = u.itop; r.mask = u.mask; r.num = u.num; r.code = u.code; r.dbf = u.dbf;
    std::memcpy(r.row, u.row, sizeof r.row);
    std::memcpy(r.col, u.col, sizeof r.col);
}
void replay(const Replay &r, Unit &u) {
    const std::uint32_t w = r.wrote;
    if (w & kWCycle) u.cycle = r.cycle;
    if (w & kWOfst) u.ofst = r.ofst;
    if (w & kWDbf) u.dbf = r.dbf;
    if (w & kWTops) u.tops = r.tops;
    if (w & kWBase) u.base = r.base;
''',
         '''                       kWItop = 512, kWMask = 1024, kWRow = 2048, kWCol = 4096, kWNum = 8192, kWCode = 16384 };
struct Key {
    std::uint32_t a, b, c;
    bool operator==(const Key &o) const { return a == o.a && b == o.b && c == o.c; }
};
inline Key key_of(const Unit &u) {
    return Key{(u.cycle & 0xFFFFu) | ((u.mode & 3u) << 16) | (u.dbf ? 0x100000u : 0u),
               (u.base & 0x3FFu) | ((u.ofst & 0x3FFu) << 16),
               (u.tops & 0x3FFu) | ((u.itops & 0x3FFu) << 16)};
}
struct ReplayRowCol {
    std::uint32_t row[4], col[4];
};
struct Replay {
    const std::uint8_t *data;
    Key before;
    std::uint32_t qwc, epoch, hash;
    std::uint32_t records, mask, code;
    std::uint16_t words, wrote, cycle, mark, ofst, tops, base, itops, top, itop;
    std::uint8_t mode, num, flags;   // bit 0: dbf, bit 1: skip_safe
};
constexpr std::uint32_t kReplays = 4096;
Replay *g_replays = nullptr;   // ~272 KB, from the heap on first use
ReplayRowCol *g_replay_rc = nullptr;
void record(Replay &r, std::uint32_t idx, const Unit &u) {
    r.cycle = static_cast<std::uint16_t>(u.cycle);
    r.ofst = static_cast<std::uint16_t>(u.ofst);
    r.tops = static_cast<std::uint16_t>(u.tops);
    r.base = static_cast<std::uint16_t>(u.base);
    r.itops = static_cast<std::uint16_t>(u.itops);
    r.mode = static_cast<std::uint8_t>(u.mode);
    r.mark = static_cast<std::uint16_t>(u.mark);
    r.top = static_cast<std::uint16_t>(u.top);
    r.itop = static_cast<std::uint16_t>(u.itop);
    r.mask = u.mask;
    r.num = static_cast<std::uint8_t>(u.num);
    r.code = u.code;
    r.flags = (u.dbf ? 1u : 0u) | (r.flags & 2u);
    if (r.wrote & (kWRow | kWCol)) {
        if (!g_replay_rc) g_replay_rc = new ReplayRowCol[kReplays]();
        if (r.wrote & kWRow) std::memcpy(g_replay_rc[idx].row, u.row, sizeof u.row);
        if (r.wrote & kWCol) std::memcpy(g_replay_rc[idx].col, u.col, sizeof u.col);
    }
}
void replay(const Replay &r, std::uint32_t idx, Unit &u) {
    const std::uint32_t w = r.wrote;
    if (w & kWCycle) u.cycle = r.cycle;
    if (w & kWOfst) u.ofst = r.ofst;
    if (w & kWDbf) u.dbf = (r.flags & 1u) != 0;
    if (w & kWTops) u.tops = r.tops;
    if (w & kWBase) u.base = r.base;
'''),
        ('''    if (w & kWItop) u.itop = r.itop;
    if (w & kWMask) u.mask = r.mask;
    if (w & kWRow) std::memcpy(u.row, r.row, sizeof r.row);
    if (w & kWCol) std::memcpy(u.col, r.col, sizeof r.col);
    if (w & kWNum) u.num = r.num;
    if (w & kWCode) u.code = r.code;
}
constexpr std::uint32_t kReplays = 4096;
Replay *g_replays = nullptr;   // 512 KB, from the heap on first use
std::uint32_t replay_slot(const std::uint8_t *data, std::uint32_t qwc, const Key &k) {
    std::uint32_t h = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(data)) * 2654435761u ^ qwc;
    const auto *w = reinterpret_cast<const std::uint32_t *>(&k);
    for (std::size_t i = 0; i < sizeof k / 4; ++i) h = (h ^ w[i]) * 16777619u;
    return (h ^ (h >> 15)) & (kReplays - 1);
}
''',
         '''    if (w & kWItop) u.itop = r.itop;
    if (w & kWMask) u.mask = r.mask;
    if (w & (kWRow | kWCol)) {
        if (w & kWRow) std::memcpy(u.row, g_replay_rc[idx].row, sizeof u.row);
        if (w & kWCol) std::memcpy(u.col, g_replay_rc[idx].col, sizeof u.col);
    }
    if (w & kWNum) u.num = r.num;
    if (w & kWCode) u.code = r.code;
}
inline std::uint32_t replay_slot(const std::uint8_t *data, std::uint32_t qwc, const Key &k) {
    std::uint32_t h = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(data)) * 2654435761u ^ qwc;
    h = (h ^ k.a) * 16777619u;
    h = (h ^ k.b) * 16777619u;
    h = (h ^ k.c) * 16777619u;
    return (h ^ (h >> 15)) & (kReplays - 1);
}
'''),
        ('''    const std::size_t count = static_cast<std::size_t>(qwc) * 4;
    const Key before = key_of(u);
    Replay &slot = g_replays[replay_slot(data, qwc, before)];
    if (g_kept_words) send_kept();
    if (slot.data == data && slot.qwc == qwc && slot.before == before && slot.epoch == g_epoch && slot.hash == gs::queue::g_unchanging_hash) {
        std::uint32_t at = slot.records, left = slot.words;
        while (left) {   // in pieces no bigger than send_kept's
            std::uint32_t n = 0;
            while (n < left) {
                const std::uint32_t r = g_records[at + n], size = r >> 31 ? 2u : 1u + r;
                if (n && n + size > 1u + kCopiedWords) break;
                n += size;
            }
            std::memcpy(gs::queue::vu().extend(gs::queue::kVif1Records, n * 4u), g_records + at, n * 4u);
            at += n;
            left -= n;
        }
        replay(slot, u);
        return true;
    }
''',
         '''    const std::size_t count = static_cast<std::size_t>(qwc) * 4;
    const Key before = key_of(u);
    const std::uint32_t slot_idx = replay_slot(data, qwc, before);
    Replay &slot = g_replays[slot_idx];
    if (g_kept_words) send_kept();
    if (slot.data == data && slot.qwc == qwc && slot.before == before && slot.epoch == g_epoch && slot.hash == gs::queue::g_unchanging_hash) {
        if (gs::queue::g_ee_skip && (slot.flags & 2u)) {
            std::uint32_t buf[22];
            std::uint32_t n = 0;
            const std::uint32_t w = slot.wrote;
            buf[n++] = 0x40000000u | (w & 0x7FFFu) | ((slot.flags & 1u) ? 0x00010000u : 0u);
            if (w & kWCycle) buf[n++] = slot.cycle;
            if (w & kWOfst) buf[n++] = slot.ofst;
            if (w & kWTops) buf[n++] = slot.tops;
            if (w & kWBase) buf[n++] = slot.base;
            if (w & kWItops) buf[n++] = slot.itops;
            if (w & kWMode) buf[n++] = slot.mode;
            if (w & kWMark) buf[n++] = slot.mark;
            if (w & kWTop) buf[n++] = slot.top;
            if (w & kWItop) buf[n++] = slot.itop;
            if (w & kWMask) buf[n++] = slot.mask;
            if (w & kWRow) { std::memcpy(buf + n, g_replay_rc[slot_idx].row, 16); n += 4; }
            if (w & kWCol) { std::memcpy(buf + n, g_replay_rc[slot_idx].col, 16); n += 4; }
            if (w & kWNum) buf[n++] = slot.num;
            if (w & kWCode) buf[n++] = slot.code;
            std::memcpy(gs::queue::vu().extend(gs::queue::kVif1Records, n * 4u), buf, n * 4u);
            replay(slot, slot_idx, u);
            return true;
        }
        std::uint32_t at = slot.records, left = slot.words;
        if (left <= 1u + kCopiedWords) {
            if (left) std::memcpy(gs::queue::vu().extend(gs::queue::kVif1Records, left * 4u), g_records + at, left * 4u);
        } else {
            while (left) {   // in pieces no bigger than send_kept's
                std::uint32_t n = 0;
                while (n < left) {
                    const std::uint32_t r = g_records[at + n];
                    const std::uint32_t size = (r >> 31) ? ((r & 0x40000000u) ? 3u : 2u) : 1u + r;
                    if (n && n + size > 1u + kCopiedWords) break;
                    n += size;
                }
                std::memcpy(gs::queue::vu().extend(gs::queue::kVif1Records, n * 4u), g_records + at, n * 4u);
                at += n;
                left -= n;
            }
        }
        replay(slot, slot_idx, u);
        return true;
    }
'''),
        ('''    }
    g_side = false;
    g_wrote = kWCode;   // a transfer of only padding still clears the code register
    const std::size_t pos = run_skipped(unit, u, words, count);
''',
         '''    }
    g_side = false;
    g_skip_unsafe = false;
    g_wrote = kWCode;   // a transfer of only padding still clears the code register
    const std::size_t pos = run_skipped(unit, u, words, count);
'''),
        ('''        slot.data = data;
        slot.qwc = qwc;
        slot.wrote = g_wrote;
        slot.before = before;
        slot.records = g_records_used;
        slot.words = g_capture_words;
        slot.epoch = g_epoch;
        slot.hash = gs::queue::g_unchanging_hash;
        g_records_used += g_capture_words;
        record(slot, u);
    }
    return true;
''',
         '''        slot.data = data;
        slot.qwc = qwc;
        slot.wrote = static_cast<std::uint16_t>(g_wrote);
        slot.before = before;
        slot.records = g_records_used;
        slot.words = static_cast<std::uint16_t>(g_capture_words);
        slot.epoch = g_epoch;
        slot.hash = gs::queue::g_unchanging_hash;
        slot.flags = g_skip_unsafe ? 0u : 2u;
        g_records_used += g_capture_words;
        record(slot, slot_idx, u);
    }
    return true;
'''),
        ('''        const bool whole = start + need <= count;
        const bool unpacks = (cmd & 0x60000000u) == 0x60000000u && u.mode != 2;
        // These two do nothing with their data on this side, so they can run before it has all arrived, unless they also stop the DMA.
        if (!whole && !((unpacks || ((cmd >> 24) & 0x7Fu) == 0x4A) && !(cmd >> 31))) break;
        const bool buffered = drop && unpacks && (cmd & 0x80008000u) == 0x8000u;
        execute(unit, u, false, true, cmd, words + start);
        if (whole) {
            if (!buffered && unpacks) {
                keep(words + pos, 1);
                keep_data(words + start, need);
            } else if (!buffered) {
                keep(words + pos, 1u + need);
''',
         '''        const bool whole = start + need <= count;
        const bool unpacks = (cmd & 0x60000000u) == 0x60000000u && u.mode != 2;
        const bool is_mpg = ((cmd >> 24) & 0x7Fu) == 0x4Au;
        const bool is_direct = !drop && (((cmd >> 24) & 0x7Fu) == 0x50u || ((cmd >> 24) & 0x7Fu) == 0x51u);
        // These two do nothing with their data on this side, so they can run before it has all arrived, unless they also stop the DMA.
        if (!whole && !((unpacks || is_mpg) && !(cmd >> 31))) break;
        const bool buffered = drop && unpacks && (cmd & 0x80008000u) == 0x8000u;
        execute<false, true>(unit, u, cmd, words + start);
        if (whole) {
            if (!buffered && (unpacks || is_mpg || is_direct)) {
                const auto *at_bytes = reinterpret_cast<const std::uint8_t *>(words + start);
                if ((need >= kAddressedWords || (is_mpg && need >= 4u)) && at_bytes >= g_ram && at_bytes + need * 4u <= g_ram + kRamSize) {
                    if (g_kept_words) send_kept();
                    auto *rec = reinterpret_cast<std::uint32_t *>(gs::queue::vu().extend(gs::queue::kVif1Records, 12u));
                    rec[0] = static_cast<std::uint32_t>(need) | 0xC0000000u;
                    rec[1] = cmd;
                    rec[2] = static_cast<std::uint32_t>(at_bytes - g_ram);
                    capture(rec, 3u);
                } else {
                    keep(words + pos, 1u + need);
                }
            } else if (!buffered) {
                keep(words + pos, 1u + need);
'''),
        ('''// the next transfer. If only the command is pending, read the complete payload
// from the new transfer without copying it through the pending vector.
[[gnu::always_inline]] inline bool run_transfer(int unit, Unit &u, bool from_vu, bool is_shadow, const std::uint32_t *words, std::size_t count) {
    if (u.stalled) return false;
    std::size_t pos = 0;
''',
         '''// the next transfer. If only the command is pending, read the complete payload
// from the new transfer without copying it through the pending vector.
template <bool FromVu, bool IsShadow>
[[gnu::always_inline]] inline bool run_transfer(int unit, Unit &u, const std::uint32_t *words, std::size_t count) {
    if (u.stalled) return false;
    std::size_t pos = 0;
'''),
        ('''        const std::uint32_t need = data_words(u, cmd);
        if (!need || need > count) return false;
        execute(unit, u, from_vu, is_shadow, cmd, words);
        u.pending.clear();
        pos = need;
    }
    pos += run(unit, u, from_vu, is_shadow, words + pos, count - pos);
    if (pos < count) u.pending.assign(words + pos, words + count);
    return true;
''',
         '''        const std::uint32_t need = data_words(u, cmd);
        if (!need || need > count) return false;
        execute<FromVu, IsShadow>(unit, u, cmd, words);
        u.pending.clear();
        pos = need;
    }
    pos += run<FromVu, IsShadow>(unit, u, words + pos, count - pos);
    if (pos < count) u.pending.assign(words + pos, words + count);
    return true;
'''),
        ('''
void process(int unit, Unit &u, bool from_vu, bool is_shadow) {
    const std::size_t pos = run(unit, u, from_vu, is_shadow, u.pending.data(), u.pending.size());
    u.pending.erase(u.pending.begin(), u.pending.begin() + static_cast<std::ptrdiff_t>(pos));
}
''',
         '''
void process(int unit, Unit &u, bool from_vu, bool is_shadow) {
    const std::size_t pos = from_vu ? run<true, false>(unit, u, u.pending.data(), u.pending.size())
                          : is_shadow ? run<false, true>(unit, u, u.pending.data(), u.pending.size())
                                      : run<false, false>(unit, u, u.pending.data(), u.pending.size());
#ifdef PS2_PICA
    if (from_vu && g_def_any_non_ram) flush_deferred_unpacks();
#endif
    u.pending.erase(u.pending.begin(), u.pending.begin() + static_cast<std::ptrdiff_t>(pos));
}
'''),
        ('''}

void reset_vu_side() { g_vu_side = Unit{}; }

void stats() {
''',
         '''}

void reset_vu_side() {
#ifdef PS2_PICA
    discard_deferred_unpacks();
    discard_deferred_mpg();
#endif
    g_vu_side = Unit{};
}

void stats() {
'''),
        ('''        // command runs here now, and the VU thread, which also holds it, gets the data as an address.
        const std::uint32_t cmd = u.pending.front(), need = data_words(u, cmd);
        if ((cmd & 0xE0000000u) == 0x60000000u && u.mode != 2 && need && need <= static_cast<std::size_t>(qwc) * 4) {
            execute(unit, u, false, true, cmd, reinterpret_cast<const std::uint32_t *>(data));
            u.pending.clear();
            u.passing = need;
''',
         '''        // command runs here now, and the VU thread, which also holds it, gets the data as an address.
        const std::uint32_t cmd = u.pending.front(), need = data_words(u, cmd);
        const bool is_unpack = (cmd & 0xE0000000u) == 0x60000000u && u.mode != 2;
        const bool is_mpg = ((cmd >> 24) & 0x7Fu) == 0x4Au && !(cmd >> 31);
        const bool is_direct = (((cmd >> 24) & 0x7Fu) == 0x50u || ((cmd >> 24) & 0x7Fu) == 0x51u) && !(cmd >> 31);
        if ((is_unpack || is_mpg || is_direct) && need && need <= static_cast<std::size_t>(qwc) * 4) {
            execute<false, true>(unit, u, cmd, reinterpret_cast<const std::uint32_t *>(data));
            u.pending.clear();
            u.passing = need;
'''),
        ('''    }
#endif
    if (is_shadow) gs::queue::vu().post(gs::queue::kVif1, qwc, 0, data, qwc * 16);
    const auto *words = reinterpret_cast<const std::uint32_t *>(data);
    const std::size_t count = static_cast<std::size_t>(qwc) * 4;
    if (run_transfer(unit, u, from_vu, is_shadow, words, count)) return;
    const std::size_t old = u.pending.size();
    u.pending.resize(old + static_cast<std::size_t>(qwc) * 4);
    std::memcpy(u.pending.data() + old, data, static_cast<std::size_t>(qwc) * 16);
    process(unit, u, from_vu, is_shadow);
}

#ifdef PS2_PICA
void transfer_words(const std::uint32_t *words, std::size_t count) {
    Unit &u = g_vu_side;
    if (run_transfer(1, u, true, false, words, count)) return;
    u.pending.insert(u.pending.end(), words, words + count);
    process(1, u, true, false);
''',
         '''    }
#endif
    if (is_shadow) {
#ifdef PS2_PICA
        if (qwc <= 512u) {
            const std::uint32_t w = qwc * 4u;
            auto *rec = reinterpret_cast<std::uint32_t *>(gs::queue::vu().extend(gs::queue::kVif1Records, 4u + w * 4u));
            rec[0] = w;
            std::memcpy(rec + 1, data, w * 4u);
        } else
#endif
        gs::queue::vu().post(gs::queue::kVif1, qwc, 0, data, qwc * 16);
    }
    const auto *words = reinterpret_cast<const std::uint32_t *>(data);
    const std::size_t count = static_cast<std::size_t>(qwc) * 4;
    const bool done = from_vu ? run_transfer<true, false>(unit, u, words, count)
                    : is_shadow ? run_transfer<false, true>(unit, u, words, count)
                                : run_transfer<false, false>(unit, u, words, count);
    if (done) {
#ifdef PS2_PICA
        if (from_vu && g_def_any_non_ram) flush_deferred_unpacks();
#endif
        return;
    }
    const std::size_t old = u.pending.size();
    u.pending.resize(old + static_cast<std::size_t>(qwc) * 4);
    std::memcpy(u.pending.data() + old, data, static_cast<std::size_t>(qwc) * 16);
    process(unit, u, from_vu, is_shadow);
#ifdef PS2_PICA
    if (from_vu && g_def_any_non_ram) flush_deferred_unpacks();
#endif
}

#ifdef PS2_PICA
void flush_deferred_unpacks() {
    if (!g_def_count) return;
    std::uint8_t *const mem = g_vu1_mem;
    for (int i = 0; i < g_def_count; ++i) {
        const DeferredUnpack &d = g_def_unpack[i];
        kFastUnpack[d.vl][d.vn][d.usn ? 1 : 0](mem, 1023u, d.addr, 1u, d.cl - 1u, d.src, d.num);
    }
    g_def_count = 0;
    g_def_slot_mask = 0;
    g_def_min = 1024u;
    g_def_max = 0u;
    g_def_any_non_ram = false;
}

void discard_deferred_unpacks() {
    g_def_count = 0;
    g_def_slot_mask = 0;
    g_def_min = 1024u;
    g_def_max = 0u;
    g_def_any_non_ram = false;
}

bool has_deferred_mpg() { return g_def_mpg_count != 0; }

std::uintptr_t lookup_deferred_mpg(std::uint32_t address, std::uint64_t &sig_out, std::uint64_t &pages_out, std::uint8_t &count_out) {
    const std::uint32_t pc = address & 16383u;
    if (g_last_mpg[0].fn && g_last_mpg[0].pc == pc) return g_last_mpg[0].fn;
    if (g_last_mpg[1].fn && g_last_mpg[1].pc == pc) return g_last_mpg[1].fn;
    std::uint64_t h = 0xCBF29CE484222325ull ^ (static_cast<std::uint64_t>(pc) * 0x9E3779B97F4A7C15ull);
    for (int i = 0; i < g_def_mpg_count; ++i) {
        const DeferredMpg &d = g_def_mpg[i];
        std::uint32_t w0, wm;
        std::memcpy(&w0, d.src, 4);
        std::memcpy(&wm, d.src + (d.bytes >> 1), 4);
        h = (h ^ static_cast<std::uint64_t>(d.src - g_ram)) * 0x100000001B3ull;
        h = (h ^ (static_cast<std::uint64_t>(d.first) | (static_cast<std::uint64_t>(d.bytes) << 16))) * 0x100000001B3ull;
        h = (h ^ (static_cast<std::uint64_t>(w0) | (static_cast<std::uint64_t>(wm) << 32))) * 0x100000001B3ull;
    }
    sig_out = h;
    pages_out = g_def_mpg_pages;
    count_out = static_cast<std::uint8_t>(g_def_mpg_count);
    const MpgCacheEntry &e = g_mpg_cache[(h ^ (h >> 16) ^ (h >> 32)) & (kMpgCacheSize - 1)];
    if (e.fn && e.sig == h && e.pages == g_def_mpg_pages && e.address == pc && e.count == count_out) {
        g_last_mpg[1] = g_last_mpg[0];
        g_last_mpg[0] = LastMpg{pc, e.fn};
        return e.fn;
    }
    return 0;
}

void cache_deferred_mpg(std::uint32_t address, std::uint64_t sig, std::uint64_t pages, std::uint8_t count, std::uintptr_t fn) {
    MpgCacheEntry &e = g_mpg_cache[(sig ^ (sig >> 16) ^ (sig >> 32)) & (kMpgCacheSize - 1)];
    e = MpgCacheEntry{sig, pages, address & 16383u, count, fn};
}

void flush_deferred_mpg() {
    if (!g_def_mpg_count) return;
    g_last_mpg[0].fn = g_last_mpg[1].fn = 0;
    for (int i = 0; i < g_def_mpg_count; ++i) {
        const DeferredMpg &d = g_def_mpg[i];
        if (std::memcmp(g_vu1_code + d.first, d.src, d.bytes) != 0) {
            std::memcpy(g_vu1_code + d.first, d.src, d.bytes);
            vu::code_changed(1, d.first, d.bytes);
        }
    }
    g_def_mpg_count = 0;
    g_def_mpg_pages = 0;
}

void discard_deferred_mpg() {
    g_last_mpg[0].fn = g_last_mpg[1].fn = 0;
    g_def_mpg_count = 0;
    g_def_mpg_pages = 0;
}

bool get_g3_streams(std::uint32_t top, std::uint32_t stride, DirectStreams &out) {
    if (!g_def_count || g_def_tops != top || g_def_cl != stride) return false;
    out.top = g_def_tops;
    out.stride = g_def_cl;
    out.mask = g_def_slot_mask;
    for (int i = 0; i < g_def_count; ++i) {
        const DeferredUnpack &d = g_def_unpack[i];
        if (d.rel < 5) out.s[d.rel] = DirectStream{d.src, d.num, d.vn, d.vl, d.usn};
    }
    return true;
}

void transfer_tte(int unit, const std::uint32_t words[2]) {
    if (unit == 1 && gs::queue::vu_active() && !on_vu_thread()) {
        Unit &u = g_units[1];
        if (!words[0] && !words[1] && !u.stalled && u.pending.empty() && !u.dropping && !u.passing) {
            u.code = 0;
            return;
        }
        if (!gs::queue::g_ee_skip && !u.stalled && u.pending.empty() && !u.dropping && !u.passing) {
            PS2_PROF(ps2::prof::Vif);
            auto *rec = reinterpret_cast<std::uint32_t *>(gs::queue::vu().extend(gs::queue::kVif1Records, 12u));
            rec[0] = 2u;
            rec[1] = words[0];
            rec[2] = words[1];
            const std::size_t pos = run<false, true>(1, u, words, 2);
            if (pos < 2) u.pending.assign(words + pos, words + 2);
            return;
        }
    }
    alignas(16) std::uint32_t q[4] = {0, 0, words[0], words[1]};
    transfer(unit, reinterpret_cast<const std::uint8_t *>(q), 1);
}

void transfer_dynamic(int unit, const std::uint8_t *data, std::uint32_t qwc) {
    if (unit == 1 && gs::queue::vu_active() && !on_vu_thread() && !gs::queue::g_ee_skip && qwc > 0) {
        Unit &u = g_units[1];
        if (!u.stalled && !u.dropping && !u.passing) {
            PS2_PROF(ps2::prof::Vif);
            if (qwc <= 512u) {
                const std::uint32_t w = qwc * 4u;
                auto *rec = reinterpret_cast<std::uint32_t *>(gs::queue::vu().extend(gs::queue::kVif1Records, 4u + w * 4u));
                rec[0] = w;
                std::memcpy(rec + 1, data, w * 4u);
            } else {
                gs::queue::vu().post(gs::queue::kVif1, qwc, 0, data, qwc * 16u);
            }
            const auto *words = reinterpret_cast<const std::uint32_t *>(data);
            const std::size_t count = static_cast<std::size_t>(qwc) * 4u;
            if (!run_transfer<false, true>(1, u, words, count)) {
                const std::size_t old = u.pending.size();
                u.pending.resize(old + count);
                std::memcpy(u.pending.data() + old, data, count * 4u);
                process(1, u, false, true);
            }
            return;
        }
    }
    transfer(unit, data, qwc);
}

void transfer_tte_data(int unit, const std::uint32_t words[2], const std::uint8_t *data, std::uint32_t qwc, bool may_unchanging) {
    if (unit == 1 && gs::queue::vu_active() && !on_vu_thread() && !gs::queue::g_ee_skip && qwc > 0) {
        Unit &u = g_units[1];
        if (!u.stalled && u.pending.empty() && !u.dropping && !u.passing) {
            const std::uint32_t w0 = words[0], w1 = words[1];
            if (!w0 && !w1 && !may_unchanging) {
                u.code = 0;
                transfer_dynamic(unit, data, qwc);
                return;
            }
            const std::uint32_t op0 = w0 >> 24;
            const bool w0_ok = !w0 || (!(w0 & 0x80000000u) && (op0 <= 0x05u || op0 == 0x07u || op0 == 0x10u || op0 == 0x11u || op0 == 0x13u));
            const bool is_unpack = (w1 & 0xE0000000u) == 0x60000000u;
            const bool is_mpg = ((w1 >> 24) & 0x7Fu) == 0x4Au && !(w1 >> 31);
            if (w0_ok && (is_unpack || is_mpg)) {
                const std::uint32_t eff_mode = (w0 && op0 == 0x05u) ? (w0 & 3u) : u.mode;
                if (!is_unpack || eff_mode != 2u) {
                    std::uint32_t need;
                    if (is_mpg) {
                        const std::uint32_t num = (w1 >> 16) & 0xFFu;
                        need = (num ? num : 256u) * 2u;
                    } else {
                        Unit tmp = u;
                        if (w0 && op0 == 0x01u) tmp.cycle = w0 & 0xFFFFu;
                        need = data_words(tmp, w1);
                    }
                    if (need == qwc * 4u) {
                        PS2_PROF(ps2::prof::Vif);
                        if (w0) {
                            const std::uint32_t imm0 = w0 & 0xFFFFu;
                            switch (op0) {
                            case 0x01: u.cycle = imm0; break;
                            case 0x02: u.ofst = imm0 & 0x3FFu; u.dbf = false; u.tops = u.base; break;
                            case 0x03: u.base = imm0 & 0x3FFu; break;
                            case 0x04: u.itops = imm0 & 0x3FFu; break;
                            case 0x05: u.mode = imm0 & 3u; break;
                            case 0x07: u.mark = imm0; break;
                            default: break;
                            }
                        }
                        u.code = w1;
                        if (is_unpack) u.num = 0;
                        if (may_unchanging && qwc >= 4u && data >= g_ram && data + static_cast<std::size_t>(qwc) * 16u <= g_ram + kRamSize &&
                            gs::queue::unchanging(data, qwc * 16u)) {
                            const std::uint32_t off = static_cast<std::uint32_t>(data - g_ram);
                            if (w0) {
                                auto *rec = reinterpret_cast<std::uint32_t *>(gs::queue::vu().extend(gs::queue::kVif1Records, 20u));
                                rec[0] = 1u;
                                rec[1] = w0;
                                rec[2] = need | 0xC0000000u;
                                rec[3] = w1;
                                rec[4] = off;
                            } else {
                                auto *rec = reinterpret_cast<std::uint32_t *>(gs::queue::vu().extend(gs::queue::kVif1Records, 12u));
                                rec[0] = need | 0xC0000000u;
                                rec[1] = w1;
                                rec[2] = off;
                            }
                            return;
                        }
                        if (qwc <= 512u) {
                            if (w0) {
                                auto *rec = reinterpret_cast<std::uint32_t *>(gs::queue::vu().extend(gs::queue::kVif1Records, (3u + need) * 4u));
                                rec[0] = 2u + need;
                                rec[1] = w0;
                                rec[2] = w1;
                                std::memcpy(rec + 3, data, need * 4u);
                            } else {
                                auto *rec = reinterpret_cast<std::uint32_t *>(gs::queue::vu().extend(gs::queue::kVif1Records, (2u + need) * 4u));
                                rec[0] = 1u + need;
                                rec[1] = w1;
                                std::memcpy(rec + 2, data, need * 4u);
                            }
                            return;
                        }
                        if (w0) {
                            auto *rec = reinterpret_cast<std::uint32_t *>(gs::queue::vu().extend(gs::queue::kVif1Records, 12u));
                            rec[0] = 2u;
                            rec[1] = w0;
                            rec[2] = w1;
                        } else {
                            auto *rec = reinterpret_cast<std::uint32_t *>(gs::queue::vu().extend(gs::queue::kVif1Records, 8u));
                            rec[0] = 1u;
                            rec[1] = w1;
                        }
                        gs::queue::vu().post(gs::queue::kVif1, qwc, 0, data, qwc * 16u);
                        return;
                    }
                }
            }
        }
    }
    transfer_tte(unit, words);
    if (may_unchanging) transfer(unit, data, qwc);
    else transfer_dynamic(unit, data, qwc);
}

void transfer_words(const std::uint32_t *words, std::size_t count) {
    Unit &u = g_vu_side;
    if (run_transfer<true, false>(1, u, words, count)) return;
    u.pending.insert(u.pending.end(), words, words + count);
    process(1, u, true, false);
'''),
        ('''    while (at < end) {
        const std::uint32_t count = *at++;
        if (count >> 31) transfer_words(reinterpret_cast<const std::uint32_t *>(g_ram + *at++), count & 0x7FFFFFFFu);
        else {
            transfer_words(at, count);
            at += count;
        }
    }
}
#endif
''',
         '''    while (at < end) {
        const std::uint32_t count = *at++;
        if ((count & 0xC0000000u) == 0xC0000000u) {
            const std::uint32_t cmd = *at++;
            const std::uint32_t off = *at++;
            if (g_vu_side.pending.empty() && !g_vu_side.stalled) {
                execute<true, false>(1, g_vu_side, cmd, reinterpret_cast<const std::uint32_t *>(g_ram + off));
            } else {
                transfer_words(&cmd, 1);
                transfer_words(reinterpret_cast<const std::uint32_t *>(g_ram + off), count & 0x3FFFFFFFu);
            }
        } else if (count >> 31) {
            transfer_words(reinterpret_cast<const std::uint32_t *>(g_ram + *at++), count & 0x7FFFFFFFu);
        } else if (count & 0x40000000u) {
            discard_deferred_unpacks();
            Unit &u = g_vu_side;
            const std::uint32_t w = count & 0x7FFFu;
            if (w & kWCycle) u.cycle = *at++;
            if (w & kWOfst) u.ofst = *at++;
            if (w & kWDbf) u.dbf = ((count >> 16) & 1u) != 0;
            if (w & kWTops) u.tops = *at++;
            if (w & kWBase) u.base = *at++;
            if (w & kWItops) u.itops = *at++;
            if (w & kWMode) u.mode = *at++;
            if (w & kWMark) u.mark = *at++;
            if (w & kWTop) u.top = *at++;
            if (w & kWItop) u.itop = *at++;
            if (w & kWMask) u.mask = *at++;
            if (w & kWRow) { std::memcpy(u.row, at, 16); at += 4; }
            if (w & kWCol) { std::memcpy(u.col, at, 16); at += 4; }
            if (w & kWNum) u.num = *at++;
            if (w & kWCode) u.code = *at++;
            if (w & (kWTop | kWItop)) vu::set_tops(u.top, u.itop);
        } else {
            transfer_words(at, count);
            at += count;
        }
    }
    if (g_def_any_non_ram) flush_deferred_unpacks();
}
#endif
'''),
        ('''
void state(ps2::state::Stream &s) {
    s.tag("vif");
    for (Unit *u : {&g_units[0], &g_units[1], &g_vu_side}) {
''',
         '''
void state(ps2::state::Stream &s) {
#ifdef PS2_PICA
    flush_deferred_unpacks();
    flush_deferred_mpg();
#endif
    s.tag("vif");
    for (Unit *u : {&g_units[0], &g_units[1], &g_vu_side}) {
'''),
    ],
    'runtime/src/vif.hpp': [
        ('''void reset_vu_side();
void stats();
void transfer(int unit, const std::uint8_t *data, std::uint32_t qwc);
void transfer_records(const std::uint8_t *payload, std::uint32_t bytes);   // ps2run_3ds_patch.py: on the VU thread
bool stalled(int unit);
std::uint32_t read(int unit, std::uint32_t reg);
''',
         '''void reset_vu_side();
void stats();

void transfer(int unit, const std::uint8_t *data, std::uint32_t qwc);
void transfer_records(const std::uint8_t *payload, std::uint32_t bytes);   // ps2run_3ds_patch.py: on the VU thread
#ifdef PS2_PICA
struct DirectStream {
    const std::uint8_t *src;
    std::uint16_t num;
    std::uint8_t vn, vl;
    bool usn;
};
struct DirectStreams {
    std::uint16_t top;
    std::uint8_t stride;
    std::uint8_t mask;
    DirectStream s[5];
};
bool get_g3_streams(std::uint32_t top, std::uint32_t stride, DirectStreams &out);
void flush_deferred_unpacks();
void discard_deferred_unpacks();
bool has_deferred_mpg();
std::uintptr_t lookup_deferred_mpg(std::uint32_t address, std::uint64_t &sig_out, std::uint64_t &pages_out, std::uint8_t &count_out);
void cache_deferred_mpg(std::uint32_t address, std::uint64_t sig, std::uint64_t pages, std::uint8_t count, std::uintptr_t fn);
void flush_deferred_mpg();
void discard_deferred_mpg();
void transfer_tte(int unit, const std::uint32_t words[2]);
void transfer_tte_data(int unit, const std::uint32_t words[2], const std::uint8_t *data, std::uint32_t qwc, bool may_unchanging = true);
void transfer_dynamic(int unit, const std::uint8_t *data, std::uint32_t qwc);
#endif
bool stalled(int unit);
std::uint32_t read(int unit, std::uint32_t reg);
'''),
    ],
    'runtime/src/vu_micro.cpp': [
        ('''#include "gs/gif.hpp"
#include "gs/queue.hpp"
#include "vu_jit_web.hpp"
#include "hw.hpp"
''',
         '''#include "gs/gif.hpp"
#include "gs/queue.hpp"
#include "vif.hpp"
#include "vu_jit_web.hpp"
#include "hw.hpp"
'''),
        ('''
void reset() {
    if (gs::queue::vu_active() && gs::queue::role() != gs::queue::Role::Vu) {
        gs::queue::vu().post(gs::queue::kVuReset, 0, 0, nullptr, 0);
        return;
''',
         '''
void reset() {
    if (gs::queue::vu_active() && !gs::queue::is_vu_thread()) {
        gs::queue::vu().post(gs::queue::kVuReset, 0, 0, nullptr, 0);
        return;
'''),
        ('''
std::uintptr_t program_at(std::uint32_t address);
void run_program(std::uint32_t address) { interpret(1, address); }   // ps2run_3ds_patch.py

ProgramFn lookup(std::uint32_t address) {
    EntryCache &e = g_entries[(address & 16383u) >> 3];
    if (e.generation == g_generation[1]) return e.function;
''',
         '''
std::uintptr_t program_at(std::uint32_t address);
void run_program(std::uint32_t address) {
#ifdef PS2_PICA
    vif::flush_deferred_mpg();
#endif
    interpret(1, address);
}   // ps2run_3ds_patch.py

ProgramFn lookup_flushed(std::uint32_t address) {
    EntryCache &e = g_entries[(address & 16383u) >> 3];
    if (e.generation == g_generation[1]) return e.function;
'''),
        ('''    }
    return e.function;
}

''',
         '''    }
    return e.function;
}

ProgramFn lookup(std::uint32_t address) {
#ifdef PS2_PICA
    if (vif::has_deferred_mpg()) {
        std::uint64_t sig = 0, pages = 0;
        std::uint8_t cnt = 0;
        if (const std::uintptr_t cached = vif::lookup_deferred_mpg(address, sig, pages, cnt))
            return reinterpret_cast<ProgramFn>(cached);
        vif::flush_deferred_mpg();
        const ProgramFn fn = lookup_flushed(address);
        if (fn) {
            const EntryCache &e = g_entries[(address & 16383u) >> 3];
            if ((pages & e.pages) == e.pages)
                vif::cache_deferred_mpg(address, sig, pages, cnt, reinterpret_cast<std::uintptr_t>(fn));
        }
        return fn;
    }
#endif
    return lookup_flushed(address);
}

'''),
        ('''    if (n == 1 && !exact) {
        if (g_native_hook && g_native_hook(address)) return;   // ps2run_3ds_patch.py
        interpret(n, address);
        return;
    }
    Unit u = unit_of(n);
    Vu0Regs &r = *u.regs;
''',
         '''    if (n == 1 && !exact) {
        if (g_native_hook && g_native_hook(address)) return;   // ps2run_3ds_patch.py
#ifdef PS2_PICA
        vif::flush_deferred_unpacks();
        vif::flush_deferred_mpg();
#endif
        interpret(n, address);
        return;
    }
#ifdef PS2_PICA
    if (n == 1) {
        vif::flush_deferred_unpacks();
        vif::flush_deferred_mpg();
    }
#endif
    Unit u = unit_of(n);
    Vu0Regs &r = *u.regs;
'''),
        ('''
void state(ps2::state::Stream &s) {
    s.tag("vu");
    s.pod(g_vu1);
''',
         '''
void state(ps2::state::Stream &s) {
#ifdef PS2_PICA
    vif::flush_deferred_mpg();
#endif
    s.tag("vu");
    s.pod(g_vu1);
'''),
    ],
}


# Core 2: a deferred texture upload that newer uploads wrote over whole is dropped instead of copied into GS memory.
UPLOAD_EDITS = {
    'runtime/src/gs/gs.cpp': [
        ('''void materialize_deferred(int idx) {
    DeferredUpload &u = g_deferred[idx];
''',
         '''// The newest upload (its g_deferred_seq, truncated) that wrote every byte of each block. A deferred upload whose blocks have all
// been written whole since is dropped instead of copied into GS memory: nothing can read what it left there.
std::uint32_t g_block_full[16384];

// Calls fn(block, whole) for each block the rectangle touches, whole when it writes all 256 bytes of it, until fn returns false.
// False when the format is not one this knows.
template <class F>
bool rect_blocks(std::uint32_t psm, std::uint32_t dbp, std::uint32_t dbw, std::uint32_t dsax, std::uint32_t dsay, std::uint32_t rrw,
                 std::uint32_t rrh, F &&fn) {
    std::uint32_t bw, bh, cols;
    const int *table;
    switch (psm) {
    case PSMCT32: case PSMCT24: bw = 8; bh = 8; cols = 8; table = &kBlock32[0][0]; break;
    case PSMCT16: bw = 16; bh = 8; cols = 4; table = &kBlock16[0][0]; break;
    case PSMCT16S: bw = 16; bh = 8; cols = 4; table = &kBlock16S[0][0]; break;
    case PSMT8: bw = 16; bh = 16; cols = 8; table = &kBlock32[0][0]; break;
    case PSMT4: bw = 32; bh = 16; cols = 4; table = &kBlock16[0][0]; break;
    default: return false;
    }
    if (!rrw || !rrh || dsax + rrw > 2048u || dsay + rrh > 2048u) return false;
    std::uint32_t pw, ph;
    psm_page_dims(psm, pw, ph);
    const std::uint32_t per_row = std::max<std::uint32_t>(1, dbw * 64 / pw);
    const bool can_be_whole = psm != PSMCT24;   // 24-bit pixels leave the top byte of each word as it was
    const std::uint32_t x0 = dsax / bw, x1 = (dsax + rrw - 1u) / bw, y0 = dsay / bh, y1 = (dsay + rrh - 1u) / bh;
    for (std::uint32_t by = y0; by <= y1; ++by) {
        const std::uint32_t py = by * bh;
        const bool whole_y = can_be_whole && py >= dsay && py + bh <= dsay + rrh;
        for (std::uint32_t bx = x0; bx <= x1; ++bx) {
            const std::uint32_t px = bx * bw;
            const std::uint32_t block = dbp + ((px / pw) + (py / ph) * per_row) * 32u +
                                        static_cast<std::uint32_t>(table[((py % ph) / bh) * cols + (px % pw) / bw]);
            if (!fn(block & 16383u, whole_y && px >= dsax && px + bw <= dsax + rrw)) return true;
        }
    }
    return true;
}

void mark_whole_blocks(std::uint32_t psm, std::uint32_t dbp, std::uint32_t dbw, std::uint32_t dsax, std::uint32_t dsay, std::uint32_t rrw,
                       std::uint32_t rrh, std::uint64_t seq) {
    rect_blocks(psm, dbp, dbw, dsax, dsay, rrw, rrh, [seq](std::uint32_t b, bool whole) {
        if (whole) g_block_full[b] = static_cast<std::uint32_t>(seq);
        return true;
    });
}

bool superseded(const DeferredUpload &u) {
    const std::uint32_t seq = static_cast<std::uint32_t>(u.seq);
    bool all = true;
    const bool known = rect_blocks(u.psm, static_cast<std::uint32_t>(u.bitbltbuf >> 32) & 0x3FFFu, static_cast<std::uint32_t>(u.bitbltbuf >> 48) & 0x3Fu,
                                   u.dsax, u.dsay, u.rrw, u.rrh,
                                   [&](std::uint32_t b, bool) { return all = static_cast<std::int32_t>(g_block_full[b] - seq) > 0; });
    return known && all;
}

void drop_deferred(int idx) {
    g_deferred[idx].active = false;
    g_def_range[idx] = 0u;
    --g_deferred_count;
}

void materialize_deferred(int idx) {
    DeferredUpload &u = g_deferred[idx];
    if (superseded(u)) {
        drop_deferred(idx);
        return;
    }
'''),
        ('''    if (!can_defer) {
        if (g_deferred_count) {
            if (contig) flush_deferred_blocks(first_block, end_block);
            else flush_all_deferred_impl();
        }
        const std::uint64_t seq = ++g_deferred_seq;
        for (std::uint32_t p = first_page; p <= last_page; ++p) g_page_max_seq[p] = seq;
        return false;
    }
''',
         '''    if (!can_defer) {
        if (g_deferred_count) {
            if (contig) flush_deferred_blocks(first_block, end_block);
            else flush_all_deferred_impl();
        }
        const std::uint64_t seq = ++g_deferred_seq;
        for (std::uint32_t p = first_page; p <= last_page; ++p) g_page_max_seq[p] = seq;
        // Written now and in one piece: the blocks it fills whole make older deferred uploads there unneeded.
        if (no_wrap && g_transfer.x == 0 && g_transfer.y == 0 && g_carried == 0 && bytes * 8u >= static_cast<std::size_t>(rrw) * rrh * bits)
            mark_whole_blocks(psm, dbp, dbw, dsax, dsay, rrw, rrh, seq);
        return false;
    }
'''),
        ('''    if (any_deferred_in(first_page, page_count)) {
        bool dropped = false;''',
         '''    const std::uint64_t new_seq = ++g_deferred_seq;
    mark_whole_blocks(psm, dbp, dbw, dsax, dsay, rrw, rrh, new_seq);
    if (any_deferred_in(first_page, page_count)) {
        bool dropped = false;'''),
        ('''            if (covered) {
                u.active = false;
                g_def_range[i] = 0u;
                --g_deferred_count;
                dropped = true;
            }''',
         '''            if (covered || superseded(u)) {
                drop_deferred(i);
                dropped = true;
            }'''),
        ('''    if (g_deferred_count >= kMaxDeferred) {
        int oldest = -1;''',
         '''    if (g_deferred_count >= kMaxDeferred) {   // a full table first loses what newer uploads wrote over
        bool dropped = false;
        for (int i = 0; i < g_deferred_hi; ++i)
            if (g_def_range[i] && superseded(g_deferred[i])) {
                drop_deferred(i);
                dropped = true;
            }
        if (dropped) rebuild_deferred_mask();
    }
    if (g_deferred_count >= kMaxDeferred) {
        int oldest = -1;'''),
        ('''        if (!g_def_range[i]) {
            const std::uint64_t seq = ++g_deferred_seq;
            g_deferred[i] = DeferredUpload{''',
         '''        if (!g_def_range[i]) {
            const std::uint64_t seq = new_seq;
            g_deferred[i] = DeferredUpload{'''),
    ],
}


# The unchanging() table with two ways per set (2026-11-23): pairs of blocks no longer push each other out.
UNCHANGING_WAYS_EDITS = {
    'runtime/src/gs/queue.cpp': [
        ('''Unchanging g_unchanging[16384];
Unchanging *g_last_unchanging = nullptr;''',
         '''// Two ways per set, the one used less recently replaced. With one way, in the lobby a 17.5 KB texture and a 672-byte block took
// the same entry in turn, so neither ever counted as unchanging and the texture was hashed whole and copied every picture.
constexpr std::uint32_t kUnchangingSets = 8192;
Unchanging g_unchanging[kUnchangingSets * 2];
std::uint8_t g_unchanging_mru[kUnchangingSets / 8];   // per set, the way used last
Unchanging *g_last_unchanging = nullptr;'''),
        ('''    Unchanging &e = g_unchanging[(off >> 4) * 2654435761u >> 18];
    const bool known = e.data == data && e.bytes == bytes;
''',
         '''    const std::uint32_t set = (off >> 4) * 2654435761u >> 19;
    Unchanging *const ways = &g_unchanging[set * 2];
    std::uint32_t way = ways[1].data == data && ways[1].bytes == bytes ? 1u : 0u;
    const bool known = ways[way].data == data && ways[way].bytes == bytes;
    std::uint8_t &mru = g_unchanging_mru[set >> 3];
    const std::uint8_t bit = static_cast<std::uint8_t>(1u << (set & 7u));
    if (!known) way = (mru & bit) ? 0u : 1u;
    mru = way ? static_cast<std::uint8_t>(mru | bit) : static_cast<std::uint8_t>(mru & ~bit);
    Unchanging &e = ways[way];
'''),
    ],
}

# Finite 3DS FPU results for div/sqrt/rsqrt and non-finite EE float scrubbing on state load:
# ARM11 VFP treats 0x7FFFFFFF as Quiet NaN and runs in round-to-nearest with default NaN, whereas PS2 EE has no NaN/Inf.
FPU_FINITE_EDITS = {
    'runtime/include/ps2/fpu.hpp': [
        ('''inline Result div(std::uint32_t a, std::uint32_t b) {
#ifdef PS2_VU_FAST
    if (exponent(b)) return {from_float(to_float(a) / to_float(b)), false};   // denormals count as zero in the FPU
#endif
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

inline Result rsqrt(std::uint32_t a, std::uint32_t b) {
    a = flush(a);
    b = flush(b) & kAbs;
    if (!b) return {(a & kSign) | kAbs, true};
    if (!(a & kAbs)) return {a & kSign, false};
#ifdef __3DS__
    return {from_float(to_float(a) / std::sqrt(to_float(b))), false};
#else
    return {from_double(to_double(a) / std::sqrt(to_double(b))), false};
#endif
}''',
         '''#if defined(__3DS__) || defined(PS2_VU_FAST)
inline std::uint32_t clamp_nonfinite(std::uint32_t r) {
    return (r & kAbs) >= 0x7F800000u ? ((r & kSign) | 0x7F7FFFFFu) : r;
}
#endif

inline Result div(std::uint32_t a, std::uint32_t b) {
#ifdef PS2_VU_FAST
    if (exponent(b)) return {clamp_nonfinite(from_float(to_float(a) / to_float(b))), false};   // denormals count as zero in the FPU
#endif
    a = flush(a);
    b = flush(b);
    const std::uint32_t sign = (a ^ b) & kSign;
#ifdef __3DS__
    if (!(b & kAbs)) return {sign | 0x7F7FFFFFu, true};
    if (!(a & kAbs)) return {sign, false};
    return {clamp_nonfinite(from_float(to_float(a) / to_float(b))), false};
#else
    if (!(b & kAbs)) return {sign | kAbs, true};
    if (!(a & kAbs)) return {sign, false};
    return {from_double(to_double(a) / to_double(b)), false};
#endif
}

inline std::uint32_t sqrt(std::uint32_t a) {
    a = flush(a) & kAbs;
    if (!a) return 0;
#ifdef __3DS__
    if (a >= 0x7F800000u) a = 0x7F7FFFFFu;
    return from_float(std::sqrt(to_float(a)));
#else
    return from_double(std::sqrt(to_double(a)));
#endif
}

inline Result rsqrt(std::uint32_t a, std::uint32_t b) {
    a = flush(a);
    b = flush(b) & kAbs;
#ifdef __3DS__
    if (!b) return {(a & kSign) | 0x7F7FFFFFu, true};
    if (!(a & kAbs)) return {a & kSign, false};
    if (b >= 0x7F800000u) b = 0x7F7FFFFFu;
    return {clamp_nonfinite(from_float(to_float(a) / std::sqrt(to_float(b)))), false};
#else
    if (!b) return {(a & kSign) | kAbs, true};
    if (!(a & kAbs)) return {a & kSign, false};
    return {from_double(to_double(a) / std::sqrt(to_double(b))), false};
#endif
}'''),
    ],
    'runtime/src/kernel.cpp': [
        ('''    s.pod(g_boot_argc);
    s.pod(g_boot_argv);
}''',
         '''    s.pod(g_boot_argc);
    s.pod(g_boot_argv);
#ifdef __3DS__
    if (s.loading()) {
        std::uint32_t cleared = 0;
        auto scrub = [&cleared](auto &cpu) {
            for (std::uint32_t &v : cpu.f)
                if ((v & 0x7FFFFFFFu) >= 0x7F800000u) {
                    v = 0u;
                    ++cleared;
                }
            if ((cpu.facc & 0x7FFFFFFFu) >= 0x7F800000u) {
                cpu.facc = 0u;
                ++cleared;
            }
        };
        scrub(g_cpu);
        for (auto &[id, t] : g_threads) scrub(t.context);
        if (cleared) std::fprintf(stderr, "[state] cleared %u non-finite EE float(s)\\n", cleared);
    }
#endif
}'''),
    ],
}

# Reused font glyph and small texture buffers (32x32 PSMT4 = 512 B) have transparent zeros in their top/bottom rows, so a
# 2-word corner-only sample() collided across different letters and returned a stale hash for 1000+ sends. Always full-hash
# small PATH3 uploads (<= 2 KB) and zero samples, restore 4-word sample(), and re-verify every 128..383 sends.
UNCHANGING_SAMPLE_EDITS = {
    'runtime/src/gs/queue.hpp': [
        ('bool unchanging(const std::uint8_t *data, std::uint32_t bytes);   // ps2run_3ds_patch.py: both take a place in main PS2 memory',
         'bool unchanging(const std::uint8_t *data, std::uint32_t bytes, bool full_check = false);   // ps2run_3ds_patch.py: both take a place in main PS2 memory'),
    ],
    'runtime/src/gs/queue.cpp': [
        ('''// Two words: the first and the last (blocks are whole quadwords, at least 16 bytes) — halves L1D misses on Core 0.
std::uint32_t sample(const std::uint8_t *data, std::uint32_t bytes) {
    std::uint32_t a, d;
    std::memcpy(&a, data, 4);
    std::memcpy(&d, data + bytes - 4, 4);
    return a ^ (d * 7u);
}''',
         '''// Four words: the first, the last, and two between (blocks are whole quadwords, at least 16 bytes).
// Reading only corners misses reused texture/glyph buffers whose top and bottom edges are transparent zeros.
std::uint32_t sample(const std::uint8_t *data, std::uint32_t bytes) {
    const std::uint32_t q = (bytes >> 2) & ~3u;
    std::uint32_t a, b, c, d;
    std::memcpy(&a, data, 4);
    std::memcpy(&b, data + q, 4);
    std::memcpy(&c, data + q * 2 + 4, 4);
    std::memcpy(&d, data + bytes - 4, 4);
    return a ^ (b * 3u) ^ (c * 5u) ^ (d * 7u);
}'''),
        ('bool unchanging(const std::uint8_t *data, std::uint32_t bytes) {',
         'bool unchanging(const std::uint8_t *data, std::uint32_t bytes, bool full_check) {'),
        ('    if (known && e.rest && sample(data, bytes) == e.sample) {',
         '    if (!full_check && known && e.rest && e.sample && sample(data, bytes) == e.sample) {'),
        ('            e.rest = static_cast<std::uint16_t>(1000u + (h >> 24));',
         '            e.rest = static_cast<std::uint16_t>(128u + (h >> 24));'),
    ],
    'runtime/src/gs/gif.cpp': [
        ('            const bool stable = gs::queue::unchanging(data, qwc * 16u);',
         '            const bool stable = gs::queue::unchanging(data, qwc * 16u, qwc <= 128u);'),
    ],
}

# The game draws a model by calling a chain of DMA tags that does not change. A chain seen twice with the same bytes is kept as the
# records it sent to the VU thread, and the next call sends those as one record (memo_play in vif.cpp). Measured on the New 3DS
# (2026-11-24): with every picture drawn the lobby went from 934 to 834 ms per 30 vblanks together with the native bone loops, of which
# this is about 80. The record store is twice the size, as the lobby's records overflowed it about once a second.
CHAIN_MEMO_EDITS = {
    'runtime/src/gs/queue.hpp': [
        ('''    void close();
    bool peek(Header &header, const std::uint8_t *&payload);''',
         '''    void close();
    // ps2run_3ds_patch.py: the message being built with extend(), if it is of this type.
    const std::uint8_t *open_message(Type type, std::uint32_t &bytes) const {
        bytes = open_type_ == type ? open_bytes_ : 0;
        return bytes ? open_at_ + sizeof(Header) : nullptr;
    }
    std::uint32_t closes() const { return closes_; }
    bool peek(Header &header, const std::uint8_t *&payload);'''),
        ('''    std::uint8_t *open_at_ = nullptr;
    std::uint32_t open_bytes_ = 0, next_ = 0;
    Type open_type_ = kWrap;''',
         '''    std::uint8_t *open_at_ = nullptr;
    std::uint32_t open_bytes_ = 0, next_ = 0, closes_ = 0;
    Type open_type_ = kWrap;'''),
        ('''void rewritten(const std::uint8_t *data, std::uint32_t bytes);
extern std::uint32_t g_unchanging_hash;   // ps2run_3ds_patch.py: the bytes' hash when unchanging() last said yes''',
         '''void rewritten(const std::uint8_t *data, std::uint32_t bytes);
extern std::uint32_t g_rewritten_gen;     // goes up when a page is first marked rewritten
bool any_rewritten(std::uint32_t off, std::uint32_t bytes);
extern std::uint32_t g_unchanging_hash;   // ps2run_3ds_patch.py: the bytes' hash when unchanging() last said yes'''),
    ],
    'runtime/src/gs/queue.cpp': [
        ('''}
std::uint32_t g_unchanging_hash = 0;''',
         '''}
std::uint32_t g_rewritten_gen = 0;
std::uint32_t g_unchanging_hash = 0;'''),
        ('''    const std::uint32_t first = static_cast<std::uint32_t>(data - g_ram) >> 12, last = static_cast<std::uint32_t>(data - g_ram + bytes - 1) >> 12;
    for (std::uint32_t p = first; p <= last && p < kRamSize / 4096; ++p) g_rewritten[p >> 5] |= 1u << (p & 31u);
}''',
         '''    const std::uint32_t first = static_cast<std::uint32_t>(data - g_ram) >> 12, last = static_cast<std::uint32_t>(data - g_ram + bytes - 1) >> 12;
    for (std::uint32_t p = first; p <= last && p < kRamSize / 4096; ++p) {
        const std::uint32_t bit = 1u << (p & 31u);
        if (g_rewritten[p >> 5] & bit) continue;
        g_rewritten[p >> 5] |= bit;
        ++g_rewritten_gen;
    }
}

bool any_rewritten(std::uint32_t off, std::uint32_t bytes) {
    for (std::uint32_t p = off >> 12, last = (off + bytes - 1u) >> 12; p <= last; ++p)
        if (g_rewritten[p >> 5] & (1u << (p & 31u))) return true;
    return false;
}'''),
        ('''    open_bytes_ = 0;
}''',
         '''    open_bytes_ = 0;
    ++closes_;
}'''),
    ],
    'runtime/src/vif.hpp': [
        ('''bool has_pending1();
#endif''',
         '''bool has_pending1();
// ps2run_3ds_patch.py: a called chain of unchanging data (a model) sent whole, see memo_play in vif.cpp.
struct MemoEnd {
    std::uint32_t chcr_tag, madr;
};
bool memo_play(std::uint32_t target, MemoEnd &end);   // true: sent; the chain returns
bool memo_begin(std::uint32_t target);                // true: walk it with memo_block for each tag and data, then memo_end or memo_abort
void memo_block(std::uint32_t off, std::uint32_t bytes);
void memo_end(const MemoEnd &end);
void memo_abort();
#endif'''),
    ],
    'runtime/src/vif.cpp': [
        ('''bool g_skip_unsafe = false;
std::uint32_t g_wrote = 0;''',
         '''bool g_skip_unsafe = false;
bool g_memo_bad = false;   // ps2run_3ds_patch.py: the chain being walked for memo_begin copied data, or did something outside the registers
bool g_memo_mask_wrote = false;   // true if shadow VIF1 wrote u.mask during memo_begin (avoids mutating u.mask)
std::uint32_t g_wrote = 0;'''),
        ('''    const std::uint32_t op = (cmd >> 24) & 0x7Fu, imm = cmd & 0xFFFFu, num = (cmd >> 16) & 0xFFu;
    if constexpr (IsShadow && Record) {''',
         '''    const std::uint32_t op = (cmd >> 24) & 0x7Fu, imm = cmd & 0xFFFFu, num = (cmd >> 16) & 0xFFu;
    if constexpr (IsShadow)
        if ((cmd & 0x80000000u) || op == 0x06 || (op & 0x7Eu) == 0x30u || ((op & 0x60u) == 0x60u && u.mode == 2)) g_memo_bad = true;
    if constexpr (IsShadow && Record) {'''),
        ('''    case 0x20: u.mask = data[0]; break;''',
         '''    case 0x20: u.mask = data[0]; if constexpr (IsShadow) g_memo_mask_wrote = true; break;'''),
        ('''            if (gif::scan_signals(2, p, qwc)) {
                if constexpr (Record) g_side = true;''',
         '''            if (gif::scan_signals(2, p, qwc)) {
                g_memo_bad = true;
                if constexpr (Record) g_side = true;'''),
        ('''                        u.code = cmd;
                        gif::scan_signals(2, reinterpret_cast<const std::uint8_t *>(at), qwc);
                        at += need;''',
         '''                        u.code = cmd;
                        if (gif::scan_signals(2, reinterpret_cast<const std::uint8_t *>(at), qwc)) g_memo_bad = true;
                        at += need;'''),
        ('''    if (w & kWMask) u.mask = r.mask;
    if (w & (kWRow | kWCol)) {
        if (w & kWRow) std::memcpy(u.row, g_replay_rc[idx].row, sizeof u.row);''',
         '''    if (w & kWMask) { u.mask = r.mask; g_memo_mask_wrote = true; }
    if (w & (kWRow | kWCol)) {
        g_memo_bad = true;
        if (w & kWRow) std::memcpy(u.row, g_replay_rc[idx].row, sizeof u.row);'''),
        ('''// kept transfer is parsed once more. 256 KB, from the heap on first use.
constexpr std::uint32_t kRecordWords = 65536, kKeptRecordWords = 4096;
std::uint32_t *g_records = nullptr;''',
         '''// kept transfer is parsed once more. 512 KB, from the heap on first use.
constexpr std::uint32_t kRecordWords = 131072, kKeptRecordWords = 4096;
std::uint32_t *g_records = nullptr;'''),
        ('''#ifdef PS2_PICA
        if (qwc <= 512u) {''',
         '''#ifdef PS2_PICA
        g_memo_bad = true;
        if (qwc <= 512u) {'''),
        ('''            PS2_PROF(ps2::prof::Vif);
            if (qwc <= 512u) {''',
         '''            PS2_PROF(ps2::prof::Vif);
            g_memo_bad = true;
            if (qwc <= 512u) {'''),
        ('''                                const bool has_sig = gif::scan_signals(2, data, qwc);
                                if (is_unchanging && !has_sig) gs::queue::mark_unchanging_no_signals();''',
         '''                                const bool has_sig = gif::scan_signals(2, data, qwc);
                                if (has_sig) g_memo_bad = true;
                                if (is_unchanging && !has_sig) gs::queue::mark_unchanging_no_signals();'''),
        ('''                        }
                        if (qwc <= 512u) {''',
         '''                        }
                        g_memo_bad = true;
                        if (qwc <= 512u) {'''),
        ('''    else transfer_dynamic(unit, data, qwc);
}''',
         '''    else transfer_dynamic(unit, data, qwc);
}


// ps2run_3ds_patch.py: the game draws a model by calling a chain of DMA tags that never changes: in the lobby 228 calls a picture to
// the same places, 2,200 tags and 2.5 MB. Walking those tags, checking each one's data and parsing the commands split across them was
// half of the DMA and VIF time on this thread. A chain whose tags and data were the same bytes on kMemoSame walks, all of it sent to
// the VU thread by address, is kept as the records it sent and what it left in the registers, and the next call with the same
// registers sends those as one record. Skipped pictures keep their own, as their records leave data out. The same rules as
// unchanging() in gs/queue.cpp hold: no page the game is seen to rewrite, a few words of the first block and of one or two others in
// turn compared on every use, and all of the bytes every 96 to 223 uses. The data itself is read where it is, as before.
namespace {
struct MemoBlock {
    std::uint32_t off, bytes, sample;
};
struct Memo {
    std::uint32_t target = 0, hash = 0, epoch = 0, gen = 0;
    Key before{};
    std::uint32_t records = 0, first_block = 0, mask = 0, code = 0, chcr_tag = 0, madr = 0;
    std::uint16_t words = 0, blocks = 0, rest = 0, wait = 0, rot = 0, idle = 0;
    std::uint16_t cycle = 0, ofst = 0, tops = 0, base = 0, itops = 0, top = 0, itop = 0, mark = 0;
    std::uint8_t mode = 0, num = 0, dbf = 0, same = 0, changed = 0, ready = 0, skip = 0;
};
constexpr std::uint32_t kMemos = 2048, kMemoBlocks = 16384, kMemoWalk = 512, kMemoSame = 2, kMemoWords = 2048;
Memo *g_memos = nullptr;
MemoBlock *g_memo_blocks = nullptr, *g_memo_walk = nullptr;
std::uint32_t g_memo_blocks_used = 0, g_memo_walk_n = 0, g_memo_gen = 1;
Memo *g_memo_now = nullptr;
Unit g_memo_unit;
std::uint32_t g_memo_closes = 0, g_memo_epoch = 0;

std::uint32_t memo_sample(const std::uint8_t *data, std::uint32_t bytes) {
    const std::uint32_t q = (bytes >> 2) & ~3u;
    std::uint32_t a, b, c, d;
    std::memcpy(&a, data, 4);
    std::memcpy(&b, data + q, 4);
    std::memcpy(&c, data + q * 2 + (bytes > 16u ? 4u : 0u), 4);   // every word of a bare tag
    std::memcpy(&d, data + bytes - 4, 4);
    return a ^ (b * 3u) ^ (c * 5u) ^ (d * 7u);
}
std::uint32_t memo_hash(const MemoBlock *b, std::uint32_t n) {
    std::uint32_t h = 2166136261u;
    for (std::uint32_t i = 0; i < n; ++i) {
        h = (h ^ b[i].off) * 16777619u;
        const auto *w = reinterpret_cast<const std::uint32_t *>(g_ram + b[i].off);
        for (std::uint32_t left = b[i].bytes >> 4; left; --left, w += 4) {
            h = (h ^ w[0]) * 16777619u;
            h = (h ^ w[1]) * 16777619u;
            h = (h ^ w[2]) * 16777619u;
            h = (h ^ w[3]) * 16777619u;
        }
    }
    return h;
}
bool memo_rewritten(const MemoBlock *b, std::uint32_t n) {
    for (std::uint32_t i = 0; i < n; ++i)
        if (gs::queue::any_rewritten(b[i].off, b[i].bytes)) return true;
    return false;
}
void memo_forget() {
    for (std::uint32_t i = 0; i < kMemos; ++i) g_memos[i] = Memo{};
    g_memo_blocks_used = 0;
}
inline Memo *memo_set(std::uint32_t target, const Key &k) {   // four places; a skipped picture's records leave data out, so they are kept apart
    std::uint32_t h = ((target >> 4) ^ (gs::queue::g_ee_skip ? 0x8000000u : 0u)) * 2654435761u;
    h = (h ^ k.a) * 16777619u;
    h = (h ^ k.b) * 16777619u;
    h = (h ^ k.c) * 16777619u;
    return g_memos + ((h ^ (h >> 15)) & (kMemos - 4u));
}
inline Memo *memo_find(Memo *set, std::uint32_t target, const Key &k) {
    for (Memo *m = set; m < set + 4; ++m)
        if (m->target == target && m->skip == gs::queue::g_ee_skip && m->before == k) return m;
    return nullptr;
}
inline bool memo_clean(const Unit &u) {
    return gs::queue::vu_active() && !on_vu_thread() && !u.stalled && u.pending.empty() && !u.dropping && !u.passing && !gif::scan_active(2);
}
// Registers nothing in a chain reads: marked before the walk, so that one it writes with the value it already had is seen.
// u.mask IS read by mode-2 unpacks, so mask writes are tracked via g_memo_mask_wrote without mutating u.mask.
constexpr std::uint32_t kMemoMark = 0x5EA5A000u;
void memo_mark(Unit &u) {
    u.top ^= kMemoMark;
    u.itop ^= kMemoMark;
    u.mark ^= kMemoMark;
}
std::uint32_t memo_unmark(Unit &u, const Unit &before) {   // which were written
    std::uint32_t changed = 0;
    if (u.top == (before.top ^ kMemoMark)) u.top = before.top; else changed |= 1u;
    if (u.itop == (before.itop ^ kMemoMark)) u.itop = before.itop; else changed |= 2u;
    if (u.mark == (before.mark ^ kMemoMark)) u.mark = before.mark; else changed |= 4u;
    if (g_memo_mask_wrote) changed |= 8u;
    return changed;
}
}

bool memo_play(std::uint32_t target, MemoEnd &end) {
    Unit &u = g_units[1];
    if (!g_memos || (target & 0x80000000u) || !memo_clean(u)) return false;
    target &= kRamSize - 16u;
    const Key before = key_of(u);
    Memo *const at = memo_find(memo_set(target, before), target, before);
    if (!at || !at->ready || at->epoch != g_epoch) return false;
    Memo &m = *at;
    const MemoBlock *b = g_memo_blocks + m.first_block;
    const std::uint32_t n = m.blocks;
    if (m.gen != gs::queue::g_rewritten_gen) {
        if (memo_rewritten(b, n)) {
            m.ready = 0;
            m.same = 0;
            m.wait = 255;
            return false;
        }
        m.gen = gs::queue::g_rewritten_gen;
    }
    bool ok = memo_sample(g_ram + b[0].off, b[0].bytes) == b[0].sample;
    if (ok && n > 1u) {
        if (n <= 4u) {
            for (std::uint32_t r = 1u; r < n && ok; ++r)
                ok = memo_sample(g_ram + b[r].off, b[r].bytes) == b[r].sample;
        } else {
            ok = memo_sample(g_ram + b[n - 1u].off, b[n - 1u].bytes) == b[n - 1u].sample;
            std::uint32_t r = m.rot;
            for (std::uint32_t k = 2u; k && ok; --k) {
                if (++r >= n - 1u) r = 1u;
                ok = memo_sample(g_ram + b[r].off, b[r].bytes) == b[r].sample;
            }
            m.rot = static_cast<std::uint16_t>(r);
        }
    }
    if (ok && !m.rest--) {
        ok = memo_hash(b, n) == m.hash;
        m.rest = static_cast<std::uint16_t>(32u + ((m.hash >> 26) & 31u));
    }
    if (!ok) {
        m.ready = 0;
        m.same = 0;
        m.wait = 8;
        return false;
    }
    PS2_PROF(ps2::prof::Vif);
    if (g_kept_words) send_kept();
    if (m.words > 2u) {
        auto *rec = reinterpret_cast<std::uint32_t *>(gs::queue::vu().extend(gs::queue::kVif1Records, 8u));
        rec[0] = 0x20000000u | m.words;
        rec[1] = m.records;
    } else if (m.words) {
        std::memcpy(gs::queue::vu().extend(gs::queue::kVif1Records, m.words * 4u), g_records + m.records, m.words * 4u);
    }
    u.cycle = m.cycle;
    u.ofst = m.ofst;
    u.tops = m.tops;
    u.base = m.base;
    u.itops = m.itops;
    u.mode = m.mode;
    u.dbf = m.dbf;
    u.num = m.num;
    u.code = m.code;
    if (m.changed & 1u) u.top = m.top;
    if (m.changed & 2u) u.itop = m.itop;
    if (m.changed & 4u) u.mark = m.mark;
    if (m.changed & 8u) u.mask = m.mask;
    m.idle = 0;
    end = MemoEnd{m.chcr_tag, m.madr};
    return true;
}

bool memo_begin(std::uint32_t target) {
    Unit &u = g_units[1];
    if ((target & 0x80000000u) || !memo_clean(u)) return false;
    if (!g_records) return false;   // run_unchanging makes it
    if (!g_memos) {
        g_memos = new Memo[kMemos]();
        g_memo_blocks = new MemoBlock[kMemoBlocks];
        g_memo_walk = new MemoBlock[kMemoWalk];
    }
    target &= kRamSize - 16u;
    const Key before = key_of(u);
    Memo *const set = memo_set(target, before);
    Memo *at = memo_find(set, target, before);
    if (!at) {
        // A place that is free or out of date, or else the first of the four that has not been played for 64 tries.
        for (Memo *m = set; m < set + 4 && !at; ++m)
            if (!m->target || (m->ready && m->epoch != g_epoch)) at = m;
        for (Memo *m = set; m < set + 4 && !at; ++m)
            if (++m->idle >= 64u) at = m;
        if (!at) return false;
        *at = Memo{};
        at->target = target;
        at->skip = gs::queue::g_ee_skip;
        at->before = before;
    }
    Memo &m = *at;
    if (m.wait) {
        --m.wait;
        return false;
    }
    if (g_kept_words) send_kept();
    gs::queue::vu().close();
    g_memo_closes = gs::queue::vu().closes();
    g_memo_epoch = g_epoch;
    g_memo_now = &m;
    g_memo_walk_n = 0;
    g_memo_bad = false;
    g_memo_mask_wrote = false;
    g_memo_unit = u;
    memo_mark(u);
    return true;
}

void memo_block(std::uint32_t off, std::uint32_t bytes) {
    if (!g_memo_now) return;
    if (g_memo_walk_n >= kMemoWalk || off + bytes > kRamSize) {
        g_memo_bad = true;
        return;
    }
    g_memo_walk[g_memo_walk_n++] = MemoBlock{off, bytes, 0u};
}

void memo_abort() {
    if (!g_memo_now) return;
    memo_unmark(g_units[1], g_memo_unit);
    g_memo_now->wait = 64;
    g_memo_now = nullptr;
}

void memo_end(const MemoEnd &end) {
    Memo *const now = g_memo_now;
    if (!now) return;
    g_memo_now = nullptr;
    Memo &m = *now;
    Unit &u = g_units[1];
    const std::uint32_t changed = memo_unmark(u, g_memo_unit);
    const std::uint32_t n = g_memo_walk_n;
    if (g_kept_words) send_kept();
    std::uint32_t bytes = 0;
    const std::uint8_t *sent = gs::queue::vu().open_message(gs::queue::kVif1Records, bytes);
    if (g_memo_bad || !n || m.skip != gs::queue::g_ee_skip || !memo_clean(u) || gs::queue::vu().closes() != g_memo_closes || g_epoch != g_memo_epoch || !sent || bytes > kMemoWords * 4u ||
        std::memcmp(u.row, g_memo_unit.row, sizeof u.row) || std::memcmp(u.col, g_memo_unit.col, sizeof u.col) || u.stat != g_memo_unit.stat || u.err != g_memo_unit.err) {
        m.same = 0;
        m.wait = 32;
        return;
    }
    if (memo_rewritten(g_memo_walk, n)) {
        m.same = 0;
        m.wait = 255;
        return;
    }
    const std::uint32_t h = memo_hash(g_memo_walk, n);
    if (h != m.hash || !m.same) {
        m.hash = h;
        m.same = 1;
        return;
    }
    if (++m.same < kMemoSame) return;
    const std::uint32_t words = bytes / 4u;
    if (kRecordWords - g_records_used < words + kKeptRecordWords) return;   // run_unchanging starts the records again first
    if (!(m.blocks >= n)) {
        if (g_memo_blocks_used + n > kMemoBlocks) {
            const Memo keep = m;
            memo_forget();
            m = keep;
            m.ready = 0;
        }
        m.first_block = g_memo_blocks_used;
        g_memo_blocks_used += n;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        MemoBlock &b = g_memo_blocks[m.first_block + i];
        b = g_memo_walk[i];
        b.sample = memo_sample(g_ram + b.off, b.bytes);
    }
    m.blocks = static_cast<std::uint16_t>(n);
    m.records = g_records_used;
    m.words = static_cast<std::uint16_t>(words);
    std::memcpy(g_records + g_records_used, sent, bytes);
    g_records_used += words;
    m.epoch = g_epoch;
    m.gen = gs::queue::g_rewritten_gen;
    m.rest = static_cast<std::uint16_t>(32u + ((h >> 26) & 31u));
    m.rot = 0;
    m.wait = 0;
    m.cycle = static_cast<std::uint16_t>(u.cycle);
    m.ofst = static_cast<std::uint16_t>(u.ofst);
    m.tops = static_cast<std::uint16_t>(u.tops);
    m.base = static_cast<std::uint16_t>(u.base);
    m.itops = static_cast<std::uint16_t>(u.itops);
    m.top = static_cast<std::uint16_t>(u.top);
    m.itop = static_cast<std::uint16_t>(u.itop);
    m.mark = static_cast<std::uint16_t>(u.mark);
    m.mask = u.mask;
    m.mode = static_cast<std::uint8_t>(u.mode);
    m.num = static_cast<std::uint8_t>(u.num);
    m.code = u.code;
    m.dbf = u.dbf;
    m.changed = static_cast<std::uint8_t>(changed);
    m.chcr_tag = end.chcr_tag;
    m.madr = end.madr;
    m.ready = 1;
}'''),
    ],
    'runtime/src/dmac.cpp': [
        ('''#endif
    for (int guard = 0; guard < 1'000'000; ++guard) {''',
         '''#endif
#ifdef PS2_PICA
    const bool memo_calls = ch == 1 && tte && !mfifo;
    bool memo_at = false;
#endif
    for (int guard = 0; guard < 1'000'000; ++guard) {'''),
        ('''        bool end = false;
        const std::uint32_t tag_at = c.tadr;''',
         '''        bool end = false;
#ifdef PS2_PICA
        const bool memo_call = memo_calls && id == 5 && !qwc && !(irq && tie) && !(c.chcr & 0x30u);
        if (memo_at) {   // inside a called chain that is being looked at: only tags that go on or come back
            const bool ok = !(irq && tie) && (id == 1 || id == 2 || id == 3 || id == 4 || id == 6) && !((c.tadr | (id == 3 || id == 4 ? addr_full : 0u)) & 0x80000000u);
            if (!ok) {
                vif::memo_abort();
                memo_at = false;
            } else {
                const bool here = id != 3 && id != 4;
                vif::memo_block(c.tadr & (kRamSize - 16u), here ? 16u + qwc * 16u : 16u);
                if (!here && qwc) vif::memo_block(addr_full & (kRamSize - 16u), qwc * 16u);
            }
        }
#endif
        const std::uint32_t tag_at = c.tadr;'''),
        (r'''        c.madr = data;
        if (ch == 2 && g_starts[2] <= 2) std::fprintf(stderr, "[dma] gif tag id %u qwc %u addr 0x%08X data 0x%08X\n", id, qwc, addr_full, data);''',
         r'''        c.madr = data;
#ifdef PS2_PICA
        if (memo_call) {
            vif::MemoEnd left;
            if (vif::memo_play(c.tadr, left)) {
                c.tadr = c.asr[0];
                c.chcr = (c.chcr & 0xFFCFu) | left.chcr_tag;
                c.madr = left.madr;
                if (end || (tie && irq)) break;
                continue;
            }
            memo_at = vif::memo_begin(c.tadr);
        }
#endif
        if (ch == 2 && g_starts[2] <= 2) std::fprintf(stderr, "[dma] gif tag id %u qwc %u addr 0x%08X data 0x%08X\n", id, qwc, addr_full, data);'''),
        ('''        }
        if (vif_stalled(ch)) {
            pause(ch, end || (tie && irq));''',
         '''        }
#ifdef PS2_PICA
        if (memo_at && id == 6) {
            vif::memo_end(vif::MemoEnd{c.chcr & 0xFFFF0000u, c.madr});
            memo_at = false;
        }
#endif
        if (vif_stalled(ch)) {
#ifdef PS2_PICA
            if (memo_at) vif::memo_abort();
#endif
            pause(ch, end || (tie && irq));'''),
        ('''    }
    c.qwc = 0;''',
         '''    }
#ifdef PS2_PICA
    if (memo_at) vif::memo_abort();
#endif
    c.qwc = 0;'''),
    ],
}

# Exact shift/mask + whole-page fast path for rect_blocks() (eliminates ARM11 software divides on Core 2 uploads,
# ~22% of Core 2 busy time in live samples.txt) and preserve 0x80000000u sentinel in gif.cpp GsSink::image()
# so RAM-backed VIF1 DIRECT uploads still compute a real content hash in pica::note_data().
UPLOAD_FAST_EDITS = {
    'runtime/src/gs/gs.cpp': [
        ('''template <class F>
bool rect_blocks(std::uint32_t psm, std::uint32_t dbp, std::uint32_t dbw, std::uint32_t dsax, std::uint32_t dsay, std::uint32_t rrw,
                 std::uint32_t rrh, F &&fn) {
    std::uint32_t bw, bh, cols;
    const int *table;
    switch (psm) {
    case PSMCT32: case PSMCT24: bw = 8; bh = 8; cols = 8; table = &kBlock32[0][0]; break;
    case PSMCT16: bw = 16; bh = 8; cols = 4; table = &kBlock16[0][0]; break;
    case PSMCT16S: bw = 16; bh = 8; cols = 4; table = &kBlock16S[0][0]; break;
    case PSMT8: bw = 16; bh = 16; cols = 8; table = &kBlock32[0][0]; break;
    case PSMT4: bw = 32; bh = 16; cols = 4; table = &kBlock16[0][0]; break;
    default: return false;
    }
    if (!rrw || !rrh || dsax + rrw > 2048u || dsay + rrh > 2048u) return false;
    std::uint32_t pw, ph;
    psm_page_dims(psm, pw, ph);
    const std::uint32_t per_row = std::max<std::uint32_t>(1, dbw * 64 / pw);
    const bool can_be_whole = psm != PSMCT24;   // 24-bit pixels leave the top byte of each word as it was
    const std::uint32_t x0 = dsax / bw, x1 = (dsax + rrw - 1u) / bw, y0 = dsay / bh, y1 = (dsay + rrh - 1u) / bh;
    for (std::uint32_t by = y0; by <= y1; ++by) {
        const std::uint32_t py = by * bh;
        const bool whole_y = can_be_whole && py >= dsay && py + bh <= dsay + rrh;
        for (std::uint32_t bx = x0; bx <= x1; ++bx) {
            const std::uint32_t px = bx * bw;
            const std::uint32_t block = dbp + ((px / pw) + (py / ph) * per_row) * 32u +
                                        static_cast<std::uint32_t>(table[((py % ph) / bh) * cols + (px % pw) / bw]);
            if (!fn(block & 16383u, whole_y && px >= dsax && px + bw <= dsax + rrw)) return true;
        }
    }
    return true;
}''',
         '''template <class F>
bool rect_blocks(std::uint32_t psm, std::uint32_t dbp, std::uint32_t dbw, std::uint32_t dsax, std::uint32_t dsay, std::uint32_t rrw,
                 std::uint32_t rrh, F &&fn) {
    std::uint32_t bw_s, bh_s, pw_s, ph_s, cols_s;
    const int *table;
    switch (psm) {
    case PSMCT32: case PSMCT24: bw_s = 3; bh_s = 3; pw_s = 6; ph_s = 5; cols_s = 3; table = &kBlock32[0][0]; break;
    case PSMCT16:               bw_s = 4; bh_s = 3; pw_s = 6; ph_s = 6; cols_s = 2; table = &kBlock16[0][0]; break;
    case PSMCT16S:              bw_s = 4; bh_s = 3; pw_s = 6; ph_s = 6; cols_s = 2; table = &kBlock16S[0][0]; break;
    case PSMT8:                 bw_s = 4; bh_s = 4; pw_s = 7; ph_s = 6; cols_s = 3; table = &kBlock32[0][0]; break;
    case PSMT4:                 bw_s = 5; bh_s = 4; pw_s = 7; ph_s = 7; cols_s = 2; table = &kBlock16[0][0]; break;
    default: return false;
    }
    if (!rrw || !rrh || dsax + rrw > 2048u || dsay + rrh > 2048u) return false;
    const std::uint32_t per_row = std::max<std::uint32_t>(1u, (dbw << 6) >> pw_s);
    const bool can_be_whole = psm != PSMCT24;   // 24-bit pixels leave the top byte of each word as it was
    const std::uint32_t pw_mask = (1u << pw_s) - 1u, ph_mask = (1u << ph_s) - 1u;
    if (can_be_whole && (dsax & pw_mask) == 0u && (dsay & ph_mask) == 0u && (rrw & pw_mask) == 0u && (rrh & ph_mask) == 0u) {
        const std::uint32_t px0 = dsax >> pw_s, py0 = dsay >> ph_s;
        const std::uint32_t px_n = rrw >> pw_s, py_n = rrh >> ph_s;
        if (px_n == per_row && px0 == 0u) {
            const std::uint32_t total = (px_n * py_n) << 5;
            const std::uint32_t start = dbp + ((py0 * per_row) << 5);
            if (start + total <= 16384u) {
                for (std::uint32_t b = start, end = start + total; b < end; ++b)
                    if (!fn(b, true)) return true;
            } else {
                for (std::uint32_t i = 0; i < total; ++i)
                    if (!fn((start + i) & 16383u, true)) return true;
            }
            return true;
        }
        const std::uint32_t row_blocks = px_n << 5;
        for (std::uint32_t py = py0; py < py0 + py_n; ++py) {
            const std::uint32_t start = dbp + ((py * per_row + px0) << 5);
            if (start + row_blocks <= 16384u) {
                for (std::uint32_t b = start, end = start + row_blocks; b < end; ++b)
                    if (!fn(b, true)) return true;
            } else {
                for (std::uint32_t i = 0; i < row_blocks; ++i)
                    if (!fn((start + i) & 16383u, true)) return true;
            }
        }
        return true;
    }
    const std::uint32_t bw = 1u << bw_s, bh = 1u << bh_s;
    const std::uint32_t cols_mask = (1u << cols_s) - 1u;
    const std::uint32_t rows_s = ph_s - bh_s, rows_mask = (1u << rows_s) - 1u;
    const std::uint32_t x0 = dsax >> bw_s, x1 = (dsax + rrw - 1u) >> bw_s;
    const std::uint32_t y0 = dsay >> bh_s, y1 = (dsay + rrh - 1u) >> bh_s;
    const std::uint32_t whole_x0 = (dsax + bw - 1u) >> bw_s, whole_x1 = (dsax + rrw) >> bw_s;
    const std::uint32_t whole_y0 = (dsay + bh - 1u) >> bh_s, whole_y1 = (dsay + rrh) >> bh_s;
    for (std::uint32_t by = y0; by <= y1; ++by) {
        const bool whole_y = can_be_whole && by >= whole_y0 && by < whole_y1;
        const std::uint32_t row_base = dbp + (((by >> rows_s) * per_row) << 5);
        const int *const trow = table + ((by & rows_mask) << cols_s);
        for (std::uint32_t bx = x0; bx <= x1; ++bx) {
            const std::uint32_t block = (row_base + ((bx >> cols_s) << 5) + static_cast<std::uint32_t>(trow[bx & cols_mask])) & 16383u;
            if (!fn(block, whole_y && bx >= whole_x0 && bx < whole_x1)) return true;
        }
    }
    return true;
}'''),
    ],
    'runtime/src/gs/gif.cpp': [
        ('        const std::uint32_t h = chunk_hash ? ((chunk_hash ^ (qoff * 0x9E3779B1u) ^ (qwords * 0x85EBCA6Bu)) | 1u) : 0u;',
         '        const std::uint32_t h = (chunk_hash == 0u || chunk_hash == 0x80000000u) ? chunk_hash : ((chunk_hash ^ (qoff * 0x9E3779B1u) ^ (qwords * 0x85EBCA6Bu)) | 1u);'),
    ],
}

# Core 0 VIF/DMAC/GIF transfer fast paths (shown + skipped pictures):
# - Non-MFIFO ch1 TTE chain fast loop + 1 KB g_spr_pages bitmask so dynamic RW half-buffer tags skip unchanging()
# - transfer_tte / transfer_dynamic / transfer_tte_data skipped-frame fast paths + dynamic TTE+data coalescing
# - run_skipped<Capture> inline fast paths for op<=0x17 and UNPACK + single-NOP keep-span bridging
# - gif::scan single-packet IMAGE and PACKED A+D fast path avoiding Path writes
VIF_CORE0_EDITS = {
    'runtime/src/dmac.cpp': [
        ('''void spr_to_ram(Channel &c, std::uint32_t qwc) {''',
         '''#ifdef PS2_PICA
std::uint32_t g_spr_pages[kRamSize / 4096u / 32u];
inline void mark_spr_pages(std::uint32_t off, std::uint32_t bytes) {
    if (!bytes) return;
    const std::uint32_t first = off >> 12;
    const std::uint32_t last = (off + bytes - 1u) >> 12;
    for (std::uint32_t p = first; p <= last; ++p)
        g_spr_pages[p >> 5] |= 1u << (p & 31u);
}
inline bool spr_page_hit(std::uint32_t off, std::uint32_t bytes) {
    const std::uint32_t first = off >> 12;
    const std::uint32_t last = (off + bytes - 1u) >> 12;
    return ((g_spr_pages[first >> 5] >> (first & 31u)) & 1u) != 0u ||
           ((g_spr_pages[last >> 5] >> (last & 31u)) & 1u) != 0u;
}
#endif

void spr_to_ram(Channel &c, std::uint32_t qwc) {'''),
        ('''#ifdef PS2_PICA
        if (!wrap && !(at & 0x80000000u)) gs::queue::rewritten(target(at), n * 16);   // ps2run_3ds_patch.py: the game builds its packets this way
#endif''',
         '''#ifdef PS2_PICA
        if (!wrap && !(at & 0x80000000u)) {
            gs::queue::rewritten(target(at), n * 16);   // ps2run_3ds_patch.py: the game builds its packets this way
            mark_spr_pages(at & (kRamSize - 16u), n * 16u);
        }
#endif'''),
        ('''#ifdef PS2_PICA
    const bool memo_calls = ch == 1 && tte && !mfifo;
    bool memo_at = false;
#endif''',
         '''#ifdef PS2_PICA
    static const bool trace_tags1 = std::getenv("PS2_TRACE_DMA1") != nullptr;
    if (ch == 1 && tte && !mfifo && !trace_tags1) {
        bool memo_at = false;
        for (int guard = 0; guard < 1'000'000; ++guard) {
            const std::uint8_t *tagp = source(c.tadr);
            std::uint64_t tag;
            std::memcpy(&tag, tagp, 8);
            c.chcr = (c.chcr & 0xFFFFu) | (static_cast<std::uint32_t>(tag >> 16) << 16);
            const std::uint32_t qwc = static_cast<std::uint32_t>(tag) & 0xFFFFu;
            const std::uint32_t id = static_cast<std::uint32_t>(tag >> 28) & 7u;
            const bool irq = (tag >> 31) & 1u;
            const std::uint32_t addr = static_cast<std::uint32_t>(tag >> 32) & 0x7FFFFFF0u;
            const std::uint32_t addr_full = addr | (static_cast<std::uint32_t>(tag >> 63) << 31);
            std::uint32_t tte_words[2];
            std::memcpy(tte_words, tagp + 8, 8);
            if (!qwc) vif::transfer_tte(1, tte_words);
            bool end = false;
            const bool memo_call = id == 5 && !qwc && !(irq && tie) && !(c.chcr & 0x30u);
            if (memo_at) {
                const bool ok = !(irq && tie) && (id == 1 || id == 2 || id == 3 || id == 4 || id == 6) && !((c.tadr | (id == 3 || id == 4 ? addr_full : 0u)) & 0x80000000u);
                if (!ok) {
                    vif::memo_abort();
                    memo_at = false;
                } else {
                    const bool here = id != 3 && id != 4;
                    vif::memo_block(c.tadr & (kRamSize - 16u), here ? 16u + qwc * 16u : 16u);
                    if (!here && qwc) vif::memo_block(addr_full & (kRamSize - 16u), qwc * 16u);
                }
            }
            std::uint32_t data = c.tadr + 16;
            switch (id) {
            case 0: data = addr_full; c.tadr += 16; end = true; break;
            case 1: c.tadr = data + qwc * 16; break;
            case 2: c.tadr = addr_full; break;
            case 3: case 4: data = addr_full; c.tadr += 16; break;
            case 5: {
                const std::uint32_t asp = (c.chcr >> 4) & 3u;
                if (asp < 2) c.asr[asp] = data + qwc * 16;
                c.chcr = (c.chcr & ~0x30u) | (std::min<std::uint32_t>(asp + 1, 2u) << 4);
                c.tadr = addr_full;
                break;
            }
            case 6: {
                const std::uint32_t asp = (c.chcr >> 4) & 3u;
                if (asp > 0) {
                    c.tadr = c.asr[asp - 1];
                    c.chcr = (c.chcr & ~0x30u) | ((asp - 1) << 4);
                } else {
                    end = true;
                }
                break;
            }
            default: end = true; break;
            }
            c.madr = data;
            if (memo_call) {
                vif::MemoEnd left;
                if (vif::memo_play(c.tadr, left)) {
                    c.tadr = c.asr[0];
                    c.chcr = (c.chcr & 0xFFCFu) | left.chcr_tag;
                    c.madr = left.madr;
                    if (end || (tie && irq)) break;
                    continue;
                }
                memo_at = vif::memo_begin(c.tadr);
            }
            if (qwc) {
                const bool in_spr = (data & 0x80000000u) != 0u;
                const std::uint32_t off = in_spr ? (data & (kScratchpadSize - 1)) : (data & (kRamSize - 16u));
                const std::size_t bytes = static_cast<std::size_t>(qwc) * 16u;
                const std::size_t room = in_spr ? (kScratchpadSize - off) : (kRamSize - off);
                if (bytes <= room) {
                    const std::uint8_t *src = in_spr ? g_spr + off : g_ram + off;
                    const bool may_unchanging = !in_spr && !spr_page_hit(off, static_cast<std::uint32_t>(bytes));
                    vif::transfer_tte_data(1, tte_words, src, qwc, may_unchanging);
                } else {
                    vif::transfer_tte(1, tte_words);
                    deliver(1, data, qwc);
                }
            }
            c.madr = data + qwc * 16;
            if (memo_at && id == 6) {
                vif::memo_end(vif::MemoEnd{c.chcr & 0xFFFF0000u, c.madr});
                memo_at = false;
            }
            if (vif::stalled(1)) {
                if (memo_at) vif::memo_abort();
                pause(1, end || (tie && irq));
                return;
            }
            if (end || (tie && irq)) break;
        }
        if (memo_at) vif::memo_abort();
        c.qwc = 0;
        finish(1);
        return;
    }
    const bool memo_calls = ch == 1 && tte && !mfifo;
    bool memo_at = false;
#endif'''),
        ('''    g_mfifo_waiting = false;
}''',
         '''    g_mfifo_waiting = false;
#ifdef PS2_PICA
    std::memset(g_spr_pages, 0, sizeof g_spr_pages);
#endif
}'''),
        ('''        const Channel &v = g_ch[1];
        g_mfifo_waiting = mfifo_vif1()''',
         '''#ifdef PS2_PICA
        std::memset(g_spr_pages, 0, sizeof g_spr_pages);
#endif
        const Channel &v = g_ch[1];
        g_mfifo_waiting = mfifo_vif1()'''),
    ],
    'runtime/src/vif.cpp': [
        ('''std::size_t run_skipped(int unit, Unit &u, const std::uint32_t *words, std::size_t count);''',
         '''template <bool Capture>
std::size_t run_skipped(int unit, Unit &u, const std::uint32_t *words, std::size_t count);'''),
        ('''    g_wrote = kWCode;   // a transfer of only padding still clears the code register
    const std::size_t pos = run_skipped(unit, u, words, count);
    g_capture = nullptr;''',
         '''    g_wrote = kWCode;   // a transfer of only padding still clears the code register
    const std::size_t pos = keep_records ? run_skipped<true>(unit, u, words, count)
                                         : run_skipped<false>(unit, u, words, count);
    g_capture = nullptr;'''),
        ('''std::size_t run_skipped(int unit, Unit &u, const std::uint32_t *words, std::size_t count) {
    std::size_t pos = 0;
    if (u.dropping) {
        pos = std::min<std::size_t>(u.dropping, count);
        u.dropping -= static_cast<std::uint32_t>(pos);
    } else if (u.passing) {
        pos = std::min<std::size_t>(u.passing, count);
        u.passing -= static_cast<std::uint32_t>(pos);
        keep_data(words, pos);
    }
    const bool drop = gs::queue::g_ee_skip;
    while (pos < count && !u.stalled) {
        const std::uint32_t cmd = words[pos];
        if (!cmd) {
            u.code = 0;
            ++pos;
            continue;
        }
        const std::size_t start = pos + 1;
        const std::uint32_t need = data_words(u, cmd);
        const bool whole = start + need <= count;
        const bool unpacks = (cmd & 0x60000000u) == 0x60000000u && u.mode != 2;
        const bool is_mpg = ((cmd >> 24) & 0x7Fu) == 0x4Au;
        const bool is_direct = !drop && (((cmd >> 24) & 0x7Fu) == 0x50u || ((cmd >> 24) & 0x7Fu) == 0x51u);
        // These two do nothing with their data on this side, so they can run before it has all arrived, unless they also stop the DMA.
        if (!whole && !((unpacks || is_mpg) && !(cmd >> 31))) break;
        const bool buffered = drop && unpacks && (cmd & 0x80008000u) == 0x8000u;
        execute<false, true>(unit, u, cmd, words + start);
        if (whole) {
            if (!buffered && (unpacks || is_mpg || is_direct)) {
                const auto *at_bytes = reinterpret_cast<const std::uint8_t *>(words + start);
                if ((need >= kAddressedWords || (is_mpg && need >= 4u)) && at_bytes >= g_ram && at_bytes + need * 4u <= g_ram + kRamSize) {
                    if (g_kept_words) send_kept();
                    auto *rec = reinterpret_cast<std::uint32_t *>(gs::queue::vu().extend(gs::queue::kVif1Records, 12u));
                    rec[0] = static_cast<std::uint32_t>(need) | 0xC0000000u;
                    rec[1] = cmd;
                    rec[2] = static_cast<std::uint32_t>(at_bytes - g_ram);
                    capture(rec, 3u);
                } else {
                    keep(words + pos, 1u + need);
                }
            } else if (!buffered) {
                keep(words + pos, 1u + need);
            }
            pos = start + need;
        } else {
            const std::size_t have = count - pos;
            (buffered ? u.dropping : u.passing) = static_cast<std::uint32_t>(1u + need - have);
            if (!buffered) {
                keep(words + pos, 1);
                keep_data(words + start, have - 1);
            }
            pos = count;
        }
    }
    keep(words + pos, count - pos);
    if (g_kept_words) send_kept();
    return pos;
}''',
         '''template <bool Capture>
std::size_t run_skipped(int unit, Unit &u, const std::uint32_t *words, std::size_t count) {
    std::size_t pos = 0;
    if (u.dropping) {
        pos = std::min<std::size_t>(u.dropping, count);
        u.dropping -= static_cast<std::uint32_t>(pos);
    } else if (u.passing) {
        pos = std::min<std::size_t>(u.passing, count);
        u.passing -= static_cast<std::uint32_t>(pos);
        keep_data(words, pos);
    }
    const bool drop = Capture ? false : gs::queue::g_ee_skip;
    while (pos < count && !u.stalled) {
        const std::uint32_t cmd = words[pos];
        if (!cmd) {
            u.code = 0;
            if (g_kept_words && g_kept + g_kept_words == words + pos && pos + 1 < count && words[pos + 1]) ++g_kept_words;
            ++pos;
            continue;
        }
        if (!(cmd & 0x80000000u)) {
            const std::uint32_t op = cmd >> 24;
            if (op <= 0x17u && op != 0x06u) {
                const std::uint32_t imm = cmd & 0xFFFFu;
                bool ok = true;
                switch (op) {
                case 0x01: u.cycle = imm; if constexpr (Capture) g_wrote |= 1u; break;
                case 0x02: u.ofst = imm & 0x3FFu; u.dbf = false; u.tops = u.base; if constexpr (Capture) g_wrote |= 2u | 4u | 8u; break;
                case 0x03: u.base = imm & 0x3FFu; if constexpr (Capture) g_wrote |= 16u; break;
                case 0x04: u.itops = imm & 0x3FFu; if constexpr (Capture) g_wrote |= 32u; break;
                case 0x05: u.mode = imm & 3u; if constexpr (Capture) g_wrote |= 64u; break;
                case 0x07: u.mark = imm; if constexpr (Capture) g_wrote |= 128u; break;
                case 0x10: case 0x11: case 0x13: break;
                case 0x14: case 0x15:
                    u.top = u.tops; u.itop = u.itops; u.dbf = !u.dbf; u.tops = u.base + (u.dbf ? u.ofst : 0);
                    if constexpr (Capture) { g_skip_unsafe = true; g_wrote |= 256u | 512u | 4u | 8u; }
                    break;
                case 0x17:
                    u.top = u.tops; u.itop = u.itops; u.dbf = !u.dbf; u.tops = u.base + (u.dbf ? u.ofst : 0);
                    if constexpr (Capture) g_wrote |= 256u | 512u | 4u | 8u;
                    break;
                default: ok = false; break;
                }
                if (ok) {
                    u.code = cmd;
                    if constexpr (Capture) g_wrote |= 16384u;
                    keep(words + pos, 1u);
                    ++pos;
                    continue;
                }
            } else if ((op & 0x60u) == 0x60u && u.mode != 2u) {
                const std::uint32_t num = (cmd >> 16) & 0xFFu;
                const std::uint32_t need = unpack_words(cmd, u, num ? num : 256u);
                const std::size_t start = pos + 1;
                u.code = cmd;
                u.num = 0;
                if constexpr (Capture) {
                    if (!(cmd & 0x8000u)) g_skip_unsafe = true;
                    g_wrote |= 8192u | 16384u;
                }
                const bool buffered = drop && (cmd & 0x8000u);
                if (start + need <= count) {
                    if (!buffered) {
                        const auto *at_bytes = reinterpret_cast<const std::uint8_t *>(words + start);
                        if (need >= kAddressedWords && at_bytes >= g_ram && at_bytes + need * 4u <= g_ram + kRamSize) {
                            if (g_kept_words) send_kept();
                            auto *rec = reinterpret_cast<std::uint32_t *>(gs::queue::vu().extend(gs::queue::kVif1Records, 12u));
                            rec[0] = need | 0xC0000000u;
                            rec[1] = cmd;
                            rec[2] = static_cast<std::uint32_t>(at_bytes - g_ram);
                            if constexpr (Capture) capture(rec, 3u);
                        } else {
                            keep(words + pos, 1u + need);
                        }
                    }
                    pos = start + need;
                } else {
                    const std::size_t have = count - pos;
                    (buffered ? u.dropping : u.passing) = static_cast<std::uint32_t>(1u + need - have);
                    if (!buffered) {
                        keep(words + pos, 1);
                        keep_data(words + start, have - 1);
                    }
                    pos = count;
                }
                continue;
            }
        }
        const std::size_t start = pos + 1;
        const std::uint32_t need = data_words(u, cmd);
        const bool whole = start + need <= count;
        const bool unpacks = (cmd & 0x60000000u) == 0x60000000u && u.mode != 2;
        const bool is_mpg = ((cmd >> 24) & 0x7Fu) == 0x4Au;
        const bool is_direct = !drop && (((cmd >> 24) & 0x7Fu) == 0x50u || ((cmd >> 24) & 0x7Fu) == 0x51u);
        // These two do nothing with their data on this side, so they can run before it has all arrived, unless they also stop the DMA.
        if (!whole && !((unpacks || is_mpg) && !(cmd >> 31))) break;
        const bool buffered = drop && unpacks && (cmd & 0x80008000u) == 0x8000u;
        execute<false, true, Capture>(unit, u, cmd, words + start);
        if (whole) {
            if (!buffered && (unpacks || is_mpg || is_direct)) {
                const auto *at_bytes = reinterpret_cast<const std::uint8_t *>(words + start);
                if ((need >= kAddressedWords || (is_mpg && need >= 4u)) && at_bytes >= g_ram && at_bytes + need * 4u <= g_ram + kRamSize) {
                    if (g_kept_words) send_kept();
                    auto *rec = reinterpret_cast<std::uint32_t *>(gs::queue::vu().extend(gs::queue::kVif1Records, 12u));
                    rec[0] = static_cast<std::uint32_t>(need) | 0xC0000000u;
                    rec[1] = cmd;
                    rec[2] = static_cast<std::uint32_t>(at_bytes - g_ram);
                    if constexpr (Capture) capture(rec, 3u);
                } else {
                    keep(words + pos, 1u + need);
                }
            } else if (!buffered) {
                keep(words + pos, 1u + need);
            }
            pos = start + need;
        } else {
            const std::size_t have = count - pos;
            (buffered ? u.dropping : u.passing) = static_cast<std::uint32_t>(1u + need - have);
            if (!buffered) {
                keep(words + pos, 1);
                keep_data(words + start, have - 1);
            }
            pos = count;
        }
    }
    keep(words + pos, count - pos);
    if (g_kept_words) send_kept();
    return pos;
}'''),
        ('''            const std::size_t pos = run_skipped(unit, u, words, count);''',
         '''            const std::size_t pos = run_skipped<false>(unit, u, words, count);'''),
        ('''void transfer_tte(int unit, const std::uint32_t words[2]) {
    if (unit == 1 && gs::queue::vu_active() && !on_vu_thread()) {
        Unit &u = g_units[1];
        if (!words[0] && !words[1] && !u.stalled && u.pending.empty() && !u.dropping && !u.passing) {
            u.code = 0;
            return;
        }
        if (!gs::queue::g_ee_skip && !u.stalled && u.pending.empty() && !u.dropping && !u.passing) {
            PS2_PROF(ps2::prof::Vif);
            auto *rec = reinterpret_cast<std::uint32_t *>(gs::queue::vu().extend(gs::queue::kVif1Records, 12u));
            rec[0] = 2u;
            rec[1] = words[0];
            rec[2] = words[1];
            const std::size_t pos = run<false, true>(1, u, words, 2);
            if (pos < 2) u.pending.assign(words + pos, words + 2);
            return;
        }
    }
    alignas(16) std::uint32_t q[4] = {0, 0, words[0], words[1]};
    transfer(unit, reinterpret_cast<const std::uint8_t *>(q), 1);
}

void transfer_dynamic(int unit, const std::uint8_t *data, std::uint32_t qwc) {
    if (unit == 1 && gs::queue::vu_active() && !on_vu_thread() && !gs::queue::g_ee_skip && qwc > 0) {
        Unit &u = g_units[1];
        if (!u.stalled && !u.dropping && !u.passing) {
            PS2_PROF(ps2::prof::Vif);
            g_memo_bad = true;
            if (qwc <= 512u) {
                const std::uint32_t w = qwc * 4u;
                auto *rec = reinterpret_cast<std::uint32_t *>(gs::queue::vu().extend(gs::queue::kVif1Records, 4u + w * 4u));
                rec[0] = w;
                std::memcpy(rec + 1, data, w * 4u);
            } else {
                gs::queue::vu().post(gs::queue::kVif1, qwc, 0, data, qwc * 16u);
            }
            const auto *words = reinterpret_cast<const std::uint32_t *>(data);
            const std::size_t count = static_cast<std::size_t>(qwc) * 4u;
            if (!run_transfer<false, true>(1, u, words, count)) {
                const std::size_t old = u.pending.size();
                u.pending.resize(old + count);
                std::memcpy(u.pending.data() + old, data, count * 4u);
                process(1, u, false, true);
            }
            return;
        }
    }
    transfer(unit, data, qwc);
}''',
         '''void transfer_tte(int unit, const std::uint32_t words[2]) {
    if (unit == 1 && gs::queue::vu_active() && !on_vu_thread()) {
        Unit &u = g_units[1];
        if (!u.stalled && u.pending.empty() && !u.dropping && !u.passing) {
            const std::uint32_t w0 = words[0], w1 = words[1];
            if (!w0 && !w1) {
                u.code = 0;
                return;
            }
            auto simple_cmd = [](std::uint32_t w) {
                if (!w) return true;
                if (w & 0x80000000u) return false;
                const std::uint32_t op = w >> 24;
                return op <= 0x05u || op == 0x07u || op == 0x10u || op == 0x11u || op == 0x13u ||
                       op == 0x14u || op == 0x15u || op == 0x17u;
            };
            if (!gs::queue::g_ee_skip || (simple_cmd(w0) && simple_cmd(w1))) {
                PS2_PROF(ps2::prof::Vif);
                if (!w0) {
                    auto *rec = reinterpret_cast<std::uint32_t *>(gs::queue::vu().extend(gs::queue::kVif1Records, 8u));
                    rec[0] = 1u;
                    rec[1] = w1;
                    const std::size_t pos = run<false, true>(1, u, words + 1, 1);
                    if (pos < 1) u.pending.assign(words + 1, words + 2);
                    return;
                }
                if (!w1 && simple_cmd(w0)) {
                    auto *rec = reinterpret_cast<std::uint32_t *>(gs::queue::vu().extend(gs::queue::kVif1Records, 8u));
                    rec[0] = 1u;
                    rec[1] = w0;
                    run<false, true>(1, u, words, 2);
                    return;
                }
                auto *rec = reinterpret_cast<std::uint32_t *>(gs::queue::vu().extend(gs::queue::kVif1Records, 12u));
                rec[0] = 2u;
                rec[1] = w0;
                rec[2] = w1;
                const std::size_t pos = run<false, true>(1, u, words, 2);
                if (pos < 2) u.pending.assign(words + pos, words + 2);
                return;
            }
        }
    }
    alignas(16) std::uint32_t q[4] = {0, 0, words[0], words[1]};
    transfer(unit, reinterpret_cast<const std::uint8_t *>(q), 1);
}

void transfer_dynamic(int unit, const std::uint8_t *data, std::uint32_t qwc) {
    if (unit == 1 && gs::queue::vu_active() && !on_vu_thread() && qwc > 0) {
        Unit &u = g_units[1];
        if (gs::queue::g_ee_skip) {
            if (u.pending.empty() && !u.stalled) {
                PS2_PROF(ps2::prof::Vif);
                const auto *words = reinterpret_cast<const std::uint32_t *>(data);
                const std::size_t count = static_cast<std::size_t>(qwc) * 4u;
                const std::size_t pos = run_skipped<false>(1, u, words, count);
                if (pos < count) u.pending.assign(words + pos, words + count);
                return;
            }
        } else if (!u.stalled && !u.dropping && !u.passing) {
            PS2_PROF(ps2::prof::Vif);
            g_memo_bad = true;
            if (qwc <= 512u) {
                const std::uint32_t w = qwc * 4u;
                auto *rec = reinterpret_cast<std::uint32_t *>(gs::queue::vu().extend(gs::queue::kVif1Records, 4u + w * 4u));
                rec[0] = w;
                std::memcpy(rec + 1, data, w * 4u);
            } else {
                gs::queue::vu().post(gs::queue::kVif1, qwc, 0, data, qwc * 16u);
            }
            const auto *words = reinterpret_cast<const std::uint32_t *>(data);
            const std::size_t count = static_cast<std::size_t>(qwc) * 4u;
            if (!run_transfer<false, true>(1, u, words, count)) {
                const std::size_t old = u.pending.size();
                u.pending.resize(old + count);
                std::memcpy(u.pending.data() + old, data, count * 4u);
                process(1, u, false, true);
            }
            return;
        }
    }
    transfer(unit, data, qwc);
}'''),
        ('''void transfer_tte_data(int unit, const std::uint32_t words[2], const std::uint8_t *data, std::uint32_t qwc, bool may_unchanging) {
    if (unit == 1 && gs::queue::vu_active() && !on_vu_thread() && !gs::queue::g_ee_skip && qwc > 0) {
        Unit &u = g_units[1];
        if (!u.stalled && u.pending.empty() && !u.dropping && !u.passing) {
            const std::uint32_t w0 = words[0], w1 = words[1];
            if (!w0 && !w1 && !may_unchanging) {
                u.code = 0;
                transfer_dynamic(unit, data, qwc);
                return;
            }''',
         '''void transfer_tte_data(int unit, const std::uint32_t words[2], const std::uint8_t *data, std::uint32_t qwc, bool may_unchanging) {
    if (unit == 1 && gs::queue::vu_active() && !on_vu_thread() && qwc > 0) {
        Unit &u = g_units[1];
        if (!u.stalled && u.pending.empty() && !u.dropping && !u.passing) {
            const bool skip = gs::queue::g_ee_skip;
            const std::uint32_t w0 = words[0], w1 = words[1];'''),
        ('''                        u.code = w1;
                        if (is_unpack) u.num = 0;''',
         '''                        u.code = w1;
                        if (is_unpack) u.num = 0;
                        if (skip && is_unpack && (w1 & 0x80008000u) == 0x8000u) {
                            if (w0) {
                                auto *rec = reinterpret_cast<std::uint32_t *>(gs::queue::vu().extend(gs::queue::kVif1Records, 8u));
                                rec[0] = 1u;
                                rec[1] = w0;
                            }
                            return;
                        }'''),
        ('''                        gs::queue::vu().post(gs::queue::kVif1, qwc, 0, data, qwc * 16u);
                        return;
                    }
                }
            }
        }
    }
    transfer_tte(unit, words);''',
         '''                        gs::queue::vu().post(gs::queue::kVif1, qwc, 0, data, qwc * 16u);
                        return;
                    }
                }
            }
            if (!may_unchanging) {
                if (!skip && qwc <= 512u) {
                    PS2_PROF(ps2::prof::Vif);
                    g_memo_bad = true;
                    const std::uint32_t w = qwc * 4u;
                    const std::uint32_t tte_n = w0 ? 2u : (w1 ? 1u : 0u);
                    auto *rec = reinterpret_cast<std::uint32_t *>(gs::queue::vu().extend(gs::queue::kVif1Records, (1u + tte_n + w) * 4u));
                    rec[0] = tte_n + w;
                    std::uint32_t *dst = rec + 1;
                    if (tte_n == 2u) {
                        dst[0] = w0;
                        dst[1] = w1;
                        dst += 2;
                    } else if (tte_n == 1u) {
                        dst[0] = w1;
                        dst += 1;
                    }
                    std::memcpy(dst, data, w * 4u);
                    if (tte_n) {
                        const std::uint32_t *tw = tte_n == 2u ? words : words + 1;
                        const std::size_t tpos = run<false, true>(1, u, tw, tte_n);
                        if (tpos < tte_n) u.pending.assign(tw + tpos, tw + tte_n);
                    } else {
                        u.code = 0;
                    }
                    const auto *dwords = reinterpret_cast<const std::uint32_t *>(data);
                    const std::size_t count = static_cast<std::size_t>(w);
                    if (!run_transfer<false, true>(1, u, dwords, count)) {
                        const std::size_t old = u.pending.size();
                        u.pending.resize(old + count);
                        std::memcpy(u.pending.data() + old, data, count * 4u);
                        process(1, u, false, true);
                    }
                    return;
                }
                if (!w0 && !w1) {
                    u.code = 0;
                    transfer_dynamic(unit, data, qwc);
                    return;
                }
            }
        }
    }
    transfer_tte(unit, words);'''),
    ],
    'runtime/src/gs/gif.cpp': [
        ('''bool scan(Path &p, const std::uint8_t *data, std::uint32_t qwc) {
    bool hit = false;''',
         '''bool scan(Path &p, const std::uint8_t *data, std::uint32_t qwc) {
    if (!p.active && qwc) {
        std::uint64_t d0, d1;
        std::memcpy(&d0, data, 8);
        std::memcpy(&d1, data + 8, 8);
        const std::uint32_t nloop = static_cast<std::uint32_t>(d0) & 0x7FFFu;
        const std::uint32_t flg = static_cast<std::uint32_t>(d0 >> 58) & 3u;
        if (flg >= 2u && nloop + 1u == qwc) return false;
        if (flg == 0u && nloop + 1u == qwc) {
            const std::uint32_t nreg = static_cast<std::uint32_t>(d0 >> 60) & 0xFu;
            if (nreg == 1u) {
                if ((d1 & 0xFu) != 0xEu) return false;
                bool hit = false;
                const std::uint8_t *at = data + 16;
                for (std::uint32_t k = 0; k < nloop; ++k, at += 16) {
                    const std::uint8_t reg = at[8];
                    if (reg >= 0x50 && reg <= 0x62) [[unlikely]] {
                        hit = true;
                        std::uint64_t value;
                        std::memcpy(&value, at, 8);
                        if (reg >= 0x60) gs::signal(reg, value);
                        else if (reg <= 0x53) gs::note_transfer(reg, value);
                    }
                }
                return hit;
            }
        }
    }
    bool hit = false;'''),
    ],
}

# Wave 5 Core 2 VIF record decode + deferred MPG MRU + Core 0 memo_play + Core 2 GS deferred active bitmask:
# - vif.cpp: hoist clean state in transfer_record_slice + inline 1/2-word TTE & STMASK dispatch;
#   2-entry MRU in lookup_deferred_mpg & populate g_last_mpg in cache_deferred_mpg;
#   hoist g_ee_skip in memo_find & defer out-of-line gif::scan_active(2) in memo_play.
# - gs/gs.cpp: 32-byte g_def_active[] bitmask for O(active) scans & O(1) free-slot lookup +
#   word-level set_deferred_bits / any_deferred_in.
VIF_CORE2_GS_EDITS = {
    'runtime/src/vif.cpp': [
        ('''std::uintptr_t lookup_deferred_mpg(std::uint32_t address, std::uint64_t &sig_out, std::uint64_t &pages_out, std::uint8_t &count_out) {
    const std::uint32_t pc = address & 16383u;
    if (g_last_mpg[0].fn && g_last_mpg[0].pc == pc) return g_last_mpg[0].fn;
    if (g_last_mpg[1].fn && g_last_mpg[1].pc == pc) return g_last_mpg[1].fn;
    std::uint64_t h = 0xCBF29CE484222325ull ^ (static_cast<std::uint64_t>(pc) * 0x9E3779B97F4A7C15ull);
    for (int i = 0; i < g_def_mpg_count; ++i) {
        const DeferredMpg &d = g_def_mpg[i];
        std::uint32_t w0, wm;
        std::memcpy(&w0, d.src, 4);
        std::memcpy(&wm, d.src + (d.bytes >> 1), 4);
        h = (h ^ static_cast<std::uint64_t>(d.src - g_ram)) * 0x100000001B3ull;
        h = (h ^ (static_cast<std::uint64_t>(d.first) | (static_cast<std::uint64_t>(d.bytes) << 16))) * 0x100000001B3ull;
        h = (h ^ (static_cast<std::uint64_t>(w0) | (static_cast<std::uint64_t>(wm) << 32))) * 0x100000001B3ull;
    }
    sig_out = h;
    pages_out = g_def_mpg_pages;
    count_out = static_cast<std::uint8_t>(g_def_mpg_count);
    const MpgCacheEntry &e = g_mpg_cache[(h ^ (h >> 16) ^ (h >> 32)) & (kMpgCacheSize - 1)];
    if (e.fn && e.sig == h && e.pages == g_def_mpg_pages && e.address == pc && e.count == count_out) {
        g_last_mpg[1] = g_last_mpg[0];
        g_last_mpg[0] = LastMpg{pc, e.fn};
        return e.fn;
    }
    return 0;
}

void cache_deferred_mpg(std::uint32_t address, std::uint64_t sig, std::uint64_t pages, std::uint8_t count, std::uintptr_t fn) {
    MpgCacheEntry &e = g_mpg_cache[(sig ^ (sig >> 16) ^ (sig >> 32)) & (kMpgCacheSize - 1)];
    e = MpgCacheEntry{sig, pages, address & 16383u, count, fn};
}''',
         '''std::uintptr_t lookup_deferred_mpg(std::uint32_t address, std::uint64_t &sig_out, std::uint64_t &pages_out, std::uint8_t &count_out) {
    const std::uint32_t pc = address & 16383u;
    if (g_last_mpg[0].fn && g_last_mpg[0].pc == pc) return g_last_mpg[0].fn;
    if (g_last_mpg[1].fn && g_last_mpg[1].pc == pc) {
        const LastMpg hit = g_last_mpg[1];
        g_last_mpg[1] = g_last_mpg[0];
        g_last_mpg[0] = hit;
        return hit.fn;
    }
    std::uint64_t h = 0xCBF29CE484222325ull ^ (static_cast<std::uint64_t>(pc) * 0x9E3779B97F4A7C15ull);
    for (int i = 0; i < g_def_mpg_count; ++i) {
        const DeferredMpg &d = g_def_mpg[i];
        std::uint32_t w0, wm;
        std::memcpy(&w0, d.src, 4);
        std::memcpy(&wm, d.src + (d.bytes >> 1), 4);
        h = (h ^ static_cast<std::uint64_t>(d.src - g_ram)) * 0x100000001B3ull;
        h = (h ^ (static_cast<std::uint64_t>(d.first) | (static_cast<std::uint64_t>(d.bytes) << 16))) * 0x100000001B3ull;
        h = (h ^ (static_cast<std::uint64_t>(w0) | (static_cast<std::uint64_t>(wm) << 32))) * 0x100000001B3ull;
    }
    sig_out = h;
    pages_out = g_def_mpg_pages;
    count_out = static_cast<std::uint8_t>(g_def_mpg_count);
    const MpgCacheEntry &e = g_mpg_cache[(h ^ (h >> 16) ^ (h >> 32)) & (kMpgCacheSize - 1)];
    if (e.fn && e.sig == h && e.pages == g_def_mpg_pages && e.address == pc && e.count == count_out) {
        g_last_mpg[1] = g_last_mpg[0];
        g_last_mpg[0] = LastMpg{pc, e.fn};
        return e.fn;
    }
    return 0;
}

void cache_deferred_mpg(std::uint32_t address, std::uint64_t sig, std::uint64_t pages, std::uint8_t count, std::uintptr_t fn) {
    const std::uint32_t pc = address & 16383u;
    MpgCacheEntry &e = g_mpg_cache[(sig ^ (sig >> 16) ^ (sig >> 32)) & (kMpgCacheSize - 1)];
    e = MpgCacheEntry{sig, pages, pc, count, fn};
    g_last_mpg[1] = g_last_mpg[0];
    g_last_mpg[0] = LastMpg{pc, fn};
}'''),
        ('''inline Memo *memo_find(Memo *set, std::uint32_t target, const Key &k) {
    for (Memo *m = set; m < set + 4; ++m)
        if (m->target == target && m->skip == gs::queue::g_ee_skip && m->before == k) return m;
    return nullptr;
}''',
         '''inline Memo *memo_find(Memo *set, std::uint32_t target, const Key &k) {
    const std::uint8_t skip = gs::queue::g_ee_skip;
    for (Memo *m = set; m < set + 4; ++m)
        if (m->target == target && m->skip == skip && m->before == k) return m;
    return nullptr;
}'''),
        ('''bool memo_play(std::uint32_t target, MemoEnd &end) {
    Unit &u = g_units[1];
    if (!g_memos || (target & 0x80000000u) || !memo_clean(u)) return false;
    target &= kRamSize - 16u;
    const Key before = key_of(u);
    Memo *const at = memo_find(memo_set(target, before), target, before);
    if (!at || !at->ready || at->epoch != g_epoch) return false;
    Memo &m = *at;
    const MemoBlock *b = g_memo_blocks + m.first_block;
    const std::uint32_t n = m.blocks;
    if (m.gen != gs::queue::g_rewritten_gen) {
        if (memo_rewritten(b, n)) {
            m.ready = 0;
            m.same = 0;
            m.wait = 255;
            return false;
        }
        m.gen = gs::queue::g_rewritten_gen;
    }
    bool ok = memo_sample(g_ram + b[0].off, b[0].bytes) == b[0].sample;
    if (ok && n > 1u) {
        if (n <= 4u) {
            for (std::uint32_t r = 1u; r < n && ok; ++r)
                ok = memo_sample(g_ram + b[r].off, b[r].bytes) == b[r].sample;
        } else {
            ok = memo_sample(g_ram + b[n - 1u].off, b[n - 1u].bytes) == b[n - 1u].sample;
            std::uint32_t r = m.rot;
            for (std::uint32_t k = 2u; k && ok; --k) {
                if (++r >= n - 1u) r = 1u;
                ok = memo_sample(g_ram + b[r].off, b[r].bytes) == b[r].sample;
            }
            m.rot = static_cast<std::uint16_t>(r);
        }
    }
    if (ok && !m.rest--) {
        ok = memo_hash(b, n) == m.hash;
        m.rest = static_cast<std::uint16_t>(32u + ((m.hash >> 26) & 31u));
    }
    if (!ok) {
        m.ready = 0;
        m.same = 0;
        m.wait = 8;
        return false;
    }''',
         '''bool memo_play(std::uint32_t target, MemoEnd &end) {
    Unit &u = g_units[1];
    if (!g_memos || (target & 0x80000000u) || u.stalled || !u.pending.empty() || u.dropping || u.passing ||
        !gs::queue::vu_active() || on_vu_thread())
        return false;
    target &= kRamSize - 16u;
    const Key before = key_of(u);
    Memo *const at = memo_find(memo_set(target, before), target, before);
    if (!at || !at->ready || at->epoch != g_epoch || gif::scan_active(2)) return false;
    Memo &m = *at;
    const MemoBlock *b = g_memo_blocks + m.first_block;
    const std::uint32_t n = m.blocks;
    if (m.gen != gs::queue::g_rewritten_gen) [[unlikely]] {
        if (memo_rewritten(b, n)) {
            m.ready = 0;
            m.same = 0;
            m.wait = 255;
            return false;
        }
        m.gen = gs::queue::g_rewritten_gen;
    }
    bool ok = memo_sample(g_ram + b[0].off, b[0].bytes) == b[0].sample;
    if (ok && n > 1u) {
        if (n <= 4u) {
            for (std::uint32_t r = 1u; r < n && ok; ++r)
                ok = memo_sample(g_ram + b[r].off, b[r].bytes) == b[r].sample;
        } else {
            ok = memo_sample(g_ram + b[n - 1u].off, b[n - 1u].bytes) == b[n - 1u].sample;
            std::uint32_t r = m.rot;
            for (std::uint32_t k = 2u; k && ok; --k) {
                if (++r >= n - 1u) r = 1u;
                ok = memo_sample(g_ram + b[r].off, b[r].bytes) == b[r].sample;
            }
            m.rot = static_cast<std::uint16_t>(r);
        }
    }
    if (ok && !m.rest--) [[unlikely]] {
        ok = memo_hash(b, n) == m.hash;
        m.rest = static_cast<std::uint16_t>(32u + ((m.hash >> 26) & 31u));
    }
    if (!ok) [[unlikely]] {
        m.ready = 0;
        m.same = 0;
        m.wait = 8;
        return false;
    }'''),
        ('''void transfer_records(const std::uint8_t *payload, std::uint32_t bytes) {
    const auto *at = reinterpret_cast<const std::uint32_t *>(payload);
    const std::uint32_t *const end = at + bytes / 4u;
    while (at < end) {
        const std::uint32_t count = *at++;
        if ((count & 0xC0000000u) == 0xC0000000u) {
            const std::uint32_t cmd = *at++;
            const std::uint32_t off = *at++;
            if (g_vu_side.pending.empty() && !g_vu_side.stalled) {
                execute<true, false>(1, g_vu_side, cmd, reinterpret_cast<const std::uint32_t *>(g_ram + off));
            } else {
                transfer_words(&cmd, 1);
                transfer_words(reinterpret_cast<const std::uint32_t *>(g_ram + off), count & 0x3FFFFFFFu);
            }
        } else if (count >> 31) {
            transfer_words(reinterpret_cast<const std::uint32_t *>(g_ram + *at++), count & 0x7FFFFFFFu);
        } else if (count & 0x40000000u) {
            discard_deferred_unpacks();
            Unit &u = g_vu_side;
            const std::uint32_t w = count & 0x7FFFu;
            if (w & kWCycle) u.cycle = *at++;
            if (w & kWOfst) u.ofst = *at++;
            if (w & kWDbf) u.dbf = ((count >> 16) & 1u) != 0;
            if (w & kWTops) u.tops = *at++;
            if (w & kWBase) u.base = *at++;
            if (w & kWItops) u.itops = *at++;
            if (w & kWMode) u.mode = *at++;
            if (w & kWMark) u.mark = *at++;
            if (w & kWTop) u.top = *at++;
            if (w & kWItop) u.itop = *at++;
            if (w & kWMask) u.mask = *at++;
            if (w & kWRow) { std::memcpy(u.row, at, 16); at += 4; }
            if (w & kWCol) { std::memcpy(u.col, at, 16); at += 4; }
            if (w & kWNum) u.num = *at++;
            if (w & kWCode) u.code = *at++;
            if (w & (kWTop | kWItop)) vu::set_tops(u.top, u.itop);
        } else {
            transfer_words(at, count);
            at += count;
        }
    }
    if (g_def_any_non_ram) flush_deferred_unpacks();
}''',
         '''static void transfer_record_slice(const std::uint32_t *at, const std::uint32_t *const end) {
    Unit &u = g_vu_side;
    bool clean = u.pending.empty() && !u.stalled;
    auto exec_simple = [&u](std::uint32_t cmd) -> bool {
        if (!cmd) { u.code = 0; return true; }
        const std::uint32_t op = cmd >> 24;
        if (op <= 0x17u) {
            u.code = cmd;
            const std::uint32_t imm = cmd & 0xFFFFu;
            switch (op) {
            case 0x01: u.cycle = imm; return true;
            case 0x02: u.ofst = imm & 0x3FFu; u.dbf = false; u.tops = u.base; return true;
            case 0x03: u.base = imm & 0x3FFu; return true;
            case 0x04: u.itops = imm & 0x3FFu; return true;
            case 0x05: u.mode = imm & 3u; return true;
            case 0x06: gif::set_path3_masked((imm >> 15) & 1u); return true;
            case 0x07: u.mark = imm; return true;
            case 0x10: case 0x11: case 0x13: return true;
            case 0x14: case 0x15: run_vu(1, u, false, imm * 8u); return true;
            case 0x17: run_vu(1, u, false, vu::tpc(1), true); return true;
            default: return false;
            }
        }
        return false;
    };
    while (at < end) {
        const std::uint32_t count = *at++;
        if ((count & 0xC0000000u) == 0xC0000000u) {
            const std::uint32_t cmd = *at++;
            const std::uint32_t off = *at++;
            if (clean) [[likely]] {
                const auto *ptr = reinterpret_cast<const std::uint32_t *>(g_ram + off);
                if ((cmd & 0xE0000000u) == 0x60000000u) {
                    u.code = cmd;
                    unpack<true>(1, u, false, cmd, ptr);
                } else if (((cmd >> 24) == 0x50u || (cmd >> 24) == 0x51u)) {
                    u.code = cmd;
                    const std::uint32_t imm = cmd & 0xFFFFu;
                    gif::transfer_ram(2, g_ram + off, imm ? imm : 0x10000u);
                } else {
                    execute<true, false>(1, u, cmd, ptr);
                    clean = !u.stalled;
                }
            } else {
                transfer_words(&cmd, 1);
                transfer_words(reinterpret_cast<const std::uint32_t *>(g_ram + off), count & 0x3FFFFFFFu);
                clean = u.pending.empty() && !u.stalled;
            }
        } else if (count >> 31) {
            transfer_words(reinterpret_cast<const std::uint32_t *>(g_ram + *at++), count & 0x7FFFFFFFu);
            clean = u.pending.empty() && !u.stalled;
        } else if (count & 0x40000000u) {
            discard_deferred_unpacks();
            const std::uint32_t w = count & 0x7FFFu;
            if (w & kWCycle) u.cycle = *at++;
            if (w & kWOfst) u.ofst = *at++;
            if (w & kWDbf) u.dbf = ((count >> 16) & 1u) != 0;
            if (w & kWTops) u.tops = *at++;
            if (w & kWBase) u.base = *at++;
            if (w & kWItops) u.itops = *at++;
            if (w & kWMode) u.mode = *at++;
            if (w & kWMark) u.mark = *at++;
            if (w & kWTop) u.top = *at++;
            if (w & kWItop) u.itop = *at++;
            if (w & kWMask) u.mask = *at++;
            if (w & kWRow) { std::memcpy(u.row, at, 16); at += 4; }
            if (w & kWCol) { std::memcpy(u.col, at, 16); at += 4; }
            if (w & kWNum) u.num = *at++;
            if (w & kWCode) u.code = *at++;
            if (w & (kWTop | kWItop)) vu::set_tops(u.top, u.itop);
        } else if (count & 0x20000000u) {
            const std::uint32_t n = count & 0x1FFFFFFFu;
            const std::uint32_t off = *at++;
            transfer_record_slice(g_records + off, g_records + off + n);
            clean = u.pending.empty() && !u.stalled;
        } else {
            if (clean) [[likely]] {
                if (count == 1u) {
                    const std::uint32_t cmd = *at++;
                    if (!exec_simple(cmd)) {
                        transfer_words(at - 1, 1);
                        clean = u.pending.empty() && !u.stalled;
                    }
                    continue;
                }
                if (count == 2u) {
                    const std::uint32_t w0 = at[0], w1 = at[1];
                    if (w0 == 0x20000000u) {
                        u.code = w0;
                        u.mask = w1;
                        at += 2;
                        continue;
                    }
                    if (exec_simple(w0)) {
                        if (!exec_simple(w1)) {
                            transfer_words(at + 1, 1);
                            clean = u.pending.empty() && !u.stalled;
                        }
                        at += 2;
                        continue;
                    }
                }
                const std::size_t pos = run<true, false>(1, u, at, count);
                if (pos < count) {
                    u.pending.assign(at + pos, at + count);
                    clean = false;
                } else if (u.stalled) {
                    clean = false;
                }
            } else {
                transfer_words(at, count);
                clean = u.pending.empty() && !u.stalled;
            }
            at += count;
        }
    }
}

void transfer_records(const std::uint8_t *payload, std::uint32_t bytes) {
    const auto *at = reinterpret_cast<const std::uint32_t *>(payload);
    transfer_record_slice(at, at + bytes / 4u);
    if (g_def_any_non_ram) flush_deferred_unpacks();
}'''),
    ],
    'runtime/src/gs/gs.cpp': [
        ('''constexpr int kMaxDeferred = 256;
DeferredUpload g_deferred[kMaxDeferred];
std::uint32_t g_def_range[kMaxDeferred]{};   // packed first_block | (end_block << 16), 0 if inactive (1 KB SoA)''',
         '''constexpr int kMaxDeferred = 256;
constexpr int kDefMaskWords = kMaxDeferred / 32;
DeferredUpload g_deferred[kMaxDeferred];
std::uint32_t g_def_range[kMaxDeferred]{};   // packed first_block | (end_block << 16), 0 if inactive (1 KB SoA)
std::uint32_t g_def_active[kDefMaskWords]{}; // 32-byte active-slot bitmask for O(active) scans & O(1) free-slot lookup'''),
        ('''void set_deferred_bits(std::uint32_t first, std::uint32_t count) {
    const std::uint32_t end = std::min<std::uint32_t>(first + count, 512u);
    for (std::uint32_t p = first; p < end; ++p) g_deferred_mask[p >> 5] |= 1u << (p & 31u);
}

void rebuild_deferred_mask() {
    std::memset(g_deferred_mask, 0, sizeof g_deferred_mask);
    int hi = 0;
    if (!g_deferred_count) {
        g_deferred_hi = 0;
        return;
    }
    for (int i = 0; i < g_deferred_hi; ++i) {
        if (g_deferred[i].active) {
            set_deferred_bits(g_deferred[i].first_page, g_deferred[i].page_count);
            hi = i + 1;
        }
    }
    g_deferred_hi = hi;
}

bool any_deferred_in(std::uint32_t first, std::uint32_t count) {
    if (!g_deferred_count || !count) return false;
    const std::uint32_t end = std::min<std::uint32_t>(first + count, 512u);
    for (std::uint32_t p = first; p < end; ++p)
        if (g_deferred_mask[p >> 5] & (1u << (p & 31u))) return true;
    return false;
}''',
         '''void set_deferred_bits(std::uint32_t first, std::uint32_t count) {
    if (!count || first >= 512u) return;
    const std::uint32_t last = std::min<std::uint32_t>(first + count, 512u) - 1u;
    std::uint32_t w = first >> 5;
    const std::uint32_t end_w = last >> 5;
    const std::uint32_t lo = ~0u << (first & 31u);
    const std::uint32_t hi = ~0u >> (31u - (last & 31u));
    if (w == end_w) {
        g_deferred_mask[w] |= lo & hi;
        return;
    }
    g_deferred_mask[w] |= lo;
    while (++w < end_w) g_deferred_mask[w] = ~0u;
    g_deferred_mask[end_w] |= hi;
}

void rebuild_deferred_mask() {
    std::memset(g_deferred_mask, 0, sizeof g_deferred_mask);
    if (!g_deferred_count) {
        g_deferred_hi = 0;
        return;
    }
    int hi = 0;
    const int w_end = (g_deferred_hi + 31) >> 5;
    for (int w = 0; w < w_end; ++w) {
        std::uint32_t m = g_def_active[w];
        if (!m) continue;
        hi = (w << 5) + (32 - __builtin_clz(m));
        for (; m; m &= m - 1u) {
            const int i = (w << 5) | __builtin_ctz(m);
            set_deferred_bits(g_deferred[i].first_page, g_deferred[i].page_count);
        }
    }
    g_deferred_hi = hi;
}

bool any_deferred_in(std::uint32_t first, std::uint32_t count) {
    if (!g_deferred_count || !count || first >= 512u) return false;
    const std::uint32_t last = std::min<std::uint32_t>(first + count, 512u) - 1u;
    std::uint32_t w = first >> 5;
    const std::uint32_t end_w = last >> 5;
    const std::uint32_t lo = ~0u << (first & 31u);
    const std::uint32_t hi = ~0u >> (31u - (last & 31u));
    if (w == end_w) return (g_deferred_mask[w] & lo & hi) != 0u;
    if (g_deferred_mask[w] & lo) return true;
    while (++w < end_w)
        if (g_deferred_mask[w]) return true;
    return (g_deferred_mask[end_w] & hi) != 0u;
}'''),
        ('''void drop_deferred(int idx) {
    g_deferred[idx].active = false;
    g_def_range[idx] = 0u;
    --g_deferred_count;
}

void materialize_deferred(int idx) {
    DeferredUpload &u = g_deferred[idx];
    if (superseded(u)) {
        drop_deferred(idx);
        return;
    }
    u.active = false;
    g_def_range[idx] = 0u;
    --g_deferred_count;''',
         '''void drop_deferred(int idx) {
    g_deferred[idx].active = false;
    g_def_range[idx] = 0u;
    g_def_active[idx >> 5] &= ~(1u << (idx & 31u));
    --g_deferred_count;
}

void materialize_deferred(int idx) {
    DeferredUpload &u = g_deferred[idx];
    if (superseded(u)) {
        drop_deferred(idx);
        return;
    }
    u.active = false;
    g_def_range[idx] = 0u;
    g_def_active[idx >> 5] &= ~(1u << (idx & 31u));
    --g_deferred_count;'''),
        ('''    int order[kMaxDeferred];
    int n = 0;
    for (int i = 0; i < g_deferred_hi; ++i) {
        const std::uint32_t r = g_def_range[i];
        if (r && (r & 0xFFFFu) < clamped_end && (r >> 16) > first_block) order[n++] = i;
    }
    if (!n) return;
    if (n > 1) std::sort(order, order + n, [](int a, int b) { return g_deferred[a].seq < g_deferred[b].seq; });
    for (int i = 0; i < n; ++i) materialize_deferred(order[i]);
    rebuild_deferred_mask();
}

void flush_deferred_range(std::uint32_t first, std::uint32_t count) {
    if (!count) return;
    const std::uint32_t end = std::min<std::uint32_t>(first + count, 512u);
    flush_deferred_blocks(first * 32u, end * 32u);
}

void clear_all_deferred() {
    for (int i = 0; i < g_deferred_hi; ++i) {
        g_deferred[i].active = false;
        g_def_range[i] = 0u;
    }
    std::memset(g_deferred_mask, 0, sizeof g_deferred_mask);''',
         '''    int order[kMaxDeferred];
    int n = 0;
    const int w_end = (g_deferred_hi + 31) >> 5;
    for (int w = 0; w < w_end; ++w) {
        for (std::uint32_t m = g_def_active[w]; m; m &= m - 1u) {
            const int i = (w << 5) | __builtin_ctz(m);
            const std::uint32_t r = g_def_range[i];
            if ((r & 0xFFFFu) < clamped_end && (r >> 16) > first_block) order[n++] = i;
        }
    }
    if (!n) return;
    if (n > 1) std::sort(order, order + n, [](int a, int b) { return g_deferred[a].seq < g_deferred[b].seq; });
    for (int i = 0; i < n; ++i) materialize_deferred(order[i]);
    rebuild_deferred_mask();
}

void flush_deferred_range(std::uint32_t first, std::uint32_t count) {
    if (!count) return;
    const std::uint32_t end = std::min<std::uint32_t>(first + count, 512u);
    flush_deferred_blocks(first * 32u, end * 32u);
}

void clear_all_deferred() {
    const int w_end = (g_deferred_hi + 31) >> 5;
    for (int w = 0; w < w_end; ++w) {
        for (std::uint32_t m = g_def_active[w]; m; m &= m - 1u) {
            const int i = (w << 5) | __builtin_ctz(m);
            g_deferred[i].active = false;
            g_def_range[i] = 0u;
        }
        g_def_active[w] = 0u;
    }
    std::memset(g_deferred_mask, 0, sizeof g_deferred_mask);'''),
        ('''    if (any_deferred_in(first_page, page_count)) {
        bool dropped = false;
        for (int i = 0; i < g_deferred_hi; ++i) {
            const std::uint32_t r = g_def_range[i];
            if (!r || (r & 0xFFFFu) >= end_block || (r >> 16) <= first_block) continue;
            DeferredUpload &u = g_deferred[i];
            const bool covered = (full_pages && first_block <= u.first_block && end_block >= u.end_block) ||
                                 (u.bitbltbuf == buf && u.trxpos == pos && u.trxreg == reg) ||
                                 (u.first_block == first_block && u.end_block == end_block && u.psm == psm && u.dsax == dsax && u.dsay == dsay && u.rrw == rrw && u.rrh == rrh);
            if (covered || superseded(u)) {
                drop_deferred(i);
                dropped = true;
            }
        }
        if (dropped) rebuild_deferred_mask();
    }
    if (g_deferred_count >= kMaxDeferred) {   // a full table first loses what newer uploads wrote over
        bool dropped = false;
        for (int i = 0; i < g_deferred_hi; ++i)
            if (g_def_range[i] && superseded(g_deferred[i])) {
                drop_deferred(i);
                dropped = true;
            }
        if (dropped) rebuild_deferred_mask();
    }
    if (g_deferred_count >= kMaxDeferred) {
        int oldest = -1;
        for (int i = 0; i < g_deferred_hi; ++i)
            if (g_def_range[i] && (oldest < 0 || g_deferred[i].seq < g_deferred[oldest].seq)) oldest = i;
        if (oldest >= 0) {
            materialize_deferred(oldest);
            rebuild_deferred_mask();
        }
    }''',
         '''    if (any_deferred_in(first_page, page_count)) {
        bool dropped = false;
        const int w_end = (g_deferred_hi + 31) >> 5;
        for (int w = 0; w < w_end; ++w) {
            for (std::uint32_t m = g_def_active[w]; m; m &= m - 1u) {
                const int i = (w << 5) | __builtin_ctz(m);
                const std::uint32_t r = g_def_range[i];
                if ((r & 0xFFFFu) >= end_block || (r >> 16) <= first_block) continue;
                DeferredUpload &u = g_deferred[i];
                const bool covered = (full_pages && first_block <= u.first_block && end_block >= u.end_block) ||
                                     (u.bitbltbuf == buf && u.trxpos == pos && u.trxreg == reg) ||
                                     (u.first_block == first_block && u.end_block == end_block && u.psm == psm && u.dsax == dsax && u.dsay == dsay && u.rrw == rrw && u.rrh == rrh);
                if (covered || superseded(u)) {
                    drop_deferred(i);
                    dropped = true;
                }
            }
        }
        if (dropped) rebuild_deferred_mask();
    }
    if (g_deferred_count >= kMaxDeferred) {   // a full table first loses what newer uploads wrote over
        bool dropped = false;
        const int w_end = (g_deferred_hi + 31) >> 5;
        for (int w = 0; w < w_end; ++w) {
            for (std::uint32_t m = g_def_active[w]; m; m &= m - 1u) {
                const int i = (w << 5) | __builtin_ctz(m);
                if (superseded(g_deferred[i])) {
                    drop_deferred(i);
                    dropped = true;
                }
            }
        }
        if (dropped) rebuild_deferred_mask();
    }
    if (g_deferred_count >= kMaxDeferred) {
        int oldest = -1;
        const int w_end = (g_deferred_hi + 31) >> 5;
        for (int w = 0; w < w_end; ++w) {
            for (std::uint32_t m = g_def_active[w]; m; m &= m - 1u) {
                const int i = (w << 5) | __builtin_ctz(m);
                if (oldest < 0 || g_deferred[i].seq < g_deferred[oldest].seq) oldest = i;
            }
        }
        if (oldest >= 0) {
            materialize_deferred(oldest);
            rebuild_deferred_mask();
        }
    }'''),
        ('''    for (int i = 0; i < kMaxDeferred; ++i) {
        if (!g_def_range[i]) {
            const std::uint64_t seq = new_seq;
            g_deferred[i] = DeferredUpload{true, full_pages, static_cast<std::uint16_t>(first_block), static_cast<std::uint16_t>(end_block),
                                           static_cast<std::uint16_t>(dsax), static_cast<std::uint16_t>(dsay),
                                           static_cast<std::uint16_t>(rrw), static_cast<std::uint16_t>(rrh),
                                           psm, first_page, page_count, g_deferred_vblank, seq, buf, pos, reg, data, bytes};
            g_def_range[i] = first_block | (end_block << 16);
            if (i + 1 > g_deferred_hi) g_deferred_hi = i + 1;''',
         '''    for (int w = 0; w < kDefMaskWords; ++w) {
        const std::uint32_t free_m = ~g_def_active[w];
        if (!free_m) continue;
        const int i = (w << 5) | __builtin_ctz(free_m);
        const std::uint64_t seq = new_seq;
        g_deferred[i] = DeferredUpload{true, full_pages, static_cast<std::uint16_t>(first_block), static_cast<std::uint16_t>(end_block),
                                       static_cast<std::uint16_t>(dsax), static_cast<std::uint16_t>(dsay),
                                       static_cast<std::uint16_t>(rrw), static_cast<std::uint16_t>(rrh),
                                       psm, first_page, page_count, g_deferred_vblank, seq, buf, pos, reg, data, bytes};
        g_def_range[i] = first_block | (end_block << 16);
        g_def_active[w] |= 1u << (i & 31);
        if (i + 1 > g_deferred_hi) g_deferred_hi = i + 1;'''),
        ('''            g_transfer.active = false;
            return true;
        }
    }
    return false;''',
         '''        g_transfer.active = false;
        return true;
    }
    return false;'''),
        ('''                std::uint64_t best_seq = 0;
                for (int i = 0; i < g_deferred_hi; ++i) {
                    const std::uint32_t r = g_def_range[i];
                    if (r && (r & 0xFFFFu) < q_end && (r >> 16) > cbp && g_deferred[i].seq > best_seq) {
                        best = i;
                        best_seq = g_deferred[i].seq;
                    }
                }''',
         '''                std::uint64_t best_seq = 0;
                const int w_end = (g_deferred_hi + 31) >> 5;
                for (int w = 0; w < w_end; ++w) {
                    for (std::uint32_t m = g_def_active[w]; m; m &= m - 1u) {
                        const int i = (w << 5) | __builtin_ctz(m);
                        const std::uint32_t r = g_def_range[i];
                        if ((r & 0xFFFFu) < q_end && (r >> 16) > cbp && g_deferred[i].seq > best_seq) {
                            best = i;
                            best_seq = g_deferred[i].seq;
                        }
                    }
                }'''),
    ],
}

# Wave 6B Core 0/2 DMA+VIF L1 I-cache compaction & DMAC hot-path speedups:
# - dmac.cpp: O(1) LUT channel decode in read()/write() + noinline start() (write: 2,288 -> 464 B),
#   noinline run_chain_ch1() (6,212 -> 1,696 B), non-MFIFO spr_to_ram() fast path skipping redundant
#   gs::queue::rewritten() on already-marked g_spr_pages, single-memcpy ch9 toSPR fast path.
# - vif.cpp: split cold unpack_slow() out of unpack() + noinline execute/run/run_unchanging/run_skipped/
#   memo_hash/memo_rewritten (transfer_record_slice: 9,524 -> 3,496 B, transfer: 8,964 -> 2,536 B,
#   unpack: 8,412 -> 1,876 B; vif.o -6.6 KB .text).
VIF_DMAC_ICACHE_EDITS = {
    'runtime/src/dmac.cpp': [
        ('''    if (ch == 9 && bytes <= room) {
        std::uint32_t sadr = g_ch[9].sadr & (kScratchpadSize - 1);
        std::uint32_t left = qwc;''',
         '''    if (ch == 9 && bytes <= room) {
        std::uint32_t sadr = g_ch[9].sadr & (kScratchpadSize - 1);
        if (sadr + bytes <= kScratchpadSize) {
            std::memcpy(g_spr + sadr, data, bytes);
            g_ch[9].sadr = (sadr + static_cast<std::uint32_t>(bytes)) & (kScratchpadSize - 1);
            return;
        }
        std::uint32_t left = qwc;'''),
        ('''inline bool spr_page_hit(std::uint32_t off, std::uint32_t bytes) {
    const std::uint32_t first = off >> 12;
    const std::uint32_t last = (off + bytes - 1u) >> 12;
    return ((g_spr_pages[first >> 5] >> (first & 31u)) & 1u) != 0u ||
           ((g_spr_pages[last >> 5] >> (last & 31u)) & 1u) != 0u;
}''',
         '''inline bool spr_page_hit(std::uint32_t off, std::uint32_t bytes) {
    const std::uint32_t first = off >> 12;
    const std::uint32_t last = (off + bytes - 1u) >> 12;
    return ((g_spr_pages[first >> 5] >> (first & 31u)) & 1u) != 0u ||
           ((g_spr_pages[last >> 5] >> (last & 31u)) & 1u) != 0u;
}
inline bool spr_pages_all_set(std::uint32_t off, std::uint32_t bytes) {
    const std::uint32_t first = off >> 12;
    const std::uint32_t last = (off + bytes - 1u) >> 12;
    if (!((g_spr_pages[first >> 5] >> (first & 31u)) & 1u)) return false;
    if (first == last) return true;
    for (std::uint32_t p = first + 1u; p <= last; ++p)
        if (!((g_spr_pages[p >> 5] >> (p & 31u)) & 1u)) return false;
    return true;
}'''),
        ('''    const bool wrap = mfifo_vif1();
    std::uint32_t madr = c.madr, sadr = c.sadr;''',
         '''    const bool wrap = mfifo_vif1();
    std::uint32_t madr = c.madr, sadr = c.sadr;
#ifdef PS2_PICA
    if (!wrap && qwc && !(madr & 0x80000000u)) {
        const std::uint32_t from = sadr & (kScratchpadSize - 1);
        const std::uint32_t off = madr & (kRamSize - 16u);
        const std::uint32_t bytes = qwc * 16u;
        if (from + bytes <= kScratchpadSize && off + bytes <= kRamSize) {
            std::memcpy(g_ram + off, g_spr + from, bytes);
            if (!spr_pages_all_set(off, bytes)) [[unlikely]] {
                gs::queue::rewritten(g_ram + off, bytes);
                mark_spr_pages(off, bytes);
            }
            c.sadr = (c.sadr + bytes) & (kScratchpadSize - 1);
            return;
        }
    }
#endif'''),
        ('''#ifdef PS2_PICA
        if (!wrap && !(at & 0x80000000u)) {
            gs::queue::rewritten(target(at), n * 16);   // ps2run_3ds_patch.py: the game builds its packets this way
            mark_spr_pages(at & (kRamSize - 16u), n * 16u);
        }
#endif''',
         '''#ifdef PS2_PICA
        if (!wrap && !(at & 0x80000000u)) {
            const std::uint32_t off = at & (kRamSize - 16u);
            const std::uint32_t bytes = n * 16u;
            if (!spr_pages_all_set(off, bytes)) [[unlikely]] {
                gs::queue::rewritten(g_ram + off, bytes);   // ps2run_3ds_patch.py: the game builds its packets this way
                mark_spr_pages(off, bytes);
            }
        }
#endif'''),
        ('''void run_chain(int ch) {''',
         '''#ifdef PS2_PICA
[[gnu::noinline]] void run_chain_ch1(Channel &c, bool tie) {
    bool memo_at = false;
    for (int guard = 0; guard < 1'000'000; ++guard) {
        const std::uint8_t *tagp = source(c.tadr);
        std::uint64_t tag;
        std::memcpy(&tag, tagp, 8);
        c.chcr = (c.chcr & 0xFFFFu) | (static_cast<std::uint32_t>(tag >> 16) << 16);
        const std::uint32_t qwc = static_cast<std::uint32_t>(tag) & 0xFFFFu;
        const std::uint32_t id = static_cast<std::uint32_t>(tag >> 28) & 7u;
        const bool irq = (tag >> 31) & 1u;
        const std::uint32_t addr = static_cast<std::uint32_t>(tag >> 32) & 0x7FFFFFF0u;
        const std::uint32_t addr_full = addr | (static_cast<std::uint32_t>(tag >> 63) << 31);
        std::uint32_t tte_words[2];
        std::memcpy(tte_words, tagp + 8, 8);
        if (!qwc) vif::transfer_tte(1, tte_words);
        bool end = false;
        const bool memo_call = id == 5 && !qwc && !(irq && tie) && !(c.chcr & 0x30u);
        if (memo_at) {
            const bool ok = !(irq && tie) && (id == 1 || id == 2 || id == 3 || id == 4 || id == 6) && !((c.tadr | (id == 3 || id == 4 ? addr_full : 0u)) & 0x80000000u);
            if (!ok) {
                vif::memo_abort();
                memo_at = false;
            } else {
                const bool here = id != 3 && id != 4;
                vif::memo_block(c.tadr & (kRamSize - 16u), here ? 16u + qwc * 16u : 16u);
                if (!here && qwc) vif::memo_block(addr_full & (kRamSize - 16u), qwc * 16u);
            }
        }
        std::uint32_t data = c.tadr + 16;
        switch (id) {
        case 0: data = addr_full; c.tadr += 16; end = true; break;
        case 1: c.tadr = data + qwc * 16; break;
        case 2: c.tadr = addr_full; break;
        case 3: case 4: data = addr_full; c.tadr += 16; break;
        case 5: {
            const std::uint32_t asp = (c.chcr >> 4) & 3u;
            if (asp < 2) c.asr[asp] = data + qwc * 16;
            c.chcr = (c.chcr & ~0x30u) | (std::min<std::uint32_t>(asp + 1, 2u) << 4);
            c.tadr = addr_full;
            break;
        }
        case 6: {
            const std::uint32_t asp = (c.chcr >> 4) & 3u;
            if (asp > 0) {
                c.tadr = c.asr[asp - 1];
                c.chcr = (c.chcr & ~0x30u) | ((asp - 1) << 4);
            } else {
                end = true;
            }
            break;
        }
        default: end = true; break;
        }
        c.madr = data;
        if (memo_call) {
            vif::MemoEnd left;
            if (vif::memo_play(c.tadr, left)) {
                c.tadr = c.asr[0];
                c.chcr = (c.chcr & 0xFFCFu) | left.chcr_tag;
                c.madr = left.madr;
                if (end || (tie && irq)) break;
                continue;
            }
            memo_at = vif::memo_begin(c.tadr);
        }
        if (qwc) {
            const bool in_spr = (data & 0x80000000u) != 0u;
            const std::uint32_t off = in_spr ? (data & (kScratchpadSize - 1)) : (data & (kRamSize - 16u));
            const std::size_t bytes = static_cast<std::size_t>(qwc) * 16u;
            const std::size_t room = in_spr ? (kScratchpadSize - off) : (kRamSize - off);
            if (bytes <= room) {
                const std::uint8_t *src = in_spr ? g_spr + off : g_ram + off;
                const bool may_unchanging = !in_spr && !spr_page_hit(off, static_cast<std::uint32_t>(bytes));
                vif::transfer_tte_data(1, tte_words, src, qwc, may_unchanging);
            } else {
                vif::transfer_tte(1, tte_words);
                deliver(1, data, qwc);
            }
        }
        c.madr = data + qwc * 16;
        if (memo_at && id == 6) {
            vif::memo_end(vif::MemoEnd{c.chcr & 0xFFFF0000u, c.madr});
            memo_at = false;
        }
        if (vif::stalled(1)) [[unlikely]] {
            if (memo_at) vif::memo_abort();
            pause(1, end || (tie && irq));
            return;
        }
        if (end || (tie && irq)) break;
    }
    if (memo_at) vif::memo_abort();
    c.qwc = 0;
    finish(1);
}
#endif

void run_chain(int ch) {'''),
        ('''#ifdef PS2_PICA
    static const bool trace_tags1 = std::getenv("PS2_TRACE_DMA1") != nullptr;
    if (ch == 1 && tte && !mfifo && !trace_tags1) {
        bool memo_at = false;
        for (int guard = 0; guard < 1'000'000; ++guard) {
            const std::uint8_t *tagp = source(c.tadr);
            std::uint64_t tag;
            std::memcpy(&tag, tagp, 8);
            c.chcr = (c.chcr & 0xFFFFu) | (static_cast<std::uint32_t>(tag >> 16) << 16);
            const std::uint32_t qwc = static_cast<std::uint32_t>(tag) & 0xFFFFu;
            const std::uint32_t id = static_cast<std::uint32_t>(tag >> 28) & 7u;
            const bool irq = (tag >> 31) & 1u;
            const std::uint32_t addr = static_cast<std::uint32_t>(tag >> 32) & 0x7FFFFFF0u;
            const std::uint32_t addr_full = addr | (static_cast<std::uint32_t>(tag >> 63) << 31);
            std::uint32_t tte_words[2];
            std::memcpy(tte_words, tagp + 8, 8);
            if (!qwc) vif::transfer_tte(1, tte_words);
            bool end = false;
            const bool memo_call = id == 5 && !qwc && !(irq && tie) && !(c.chcr & 0x30u);
            if (memo_at) {
                const bool ok = !(irq && tie) && (id == 1 || id == 2 || id == 3 || id == 4 || id == 6) && !((c.tadr | (id == 3 || id == 4 ? addr_full : 0u)) & 0x80000000u);
                if (!ok) {
                    vif::memo_abort();
                    memo_at = false;
                } else {
                    const bool here = id != 3 && id != 4;
                    vif::memo_block(c.tadr & (kRamSize - 16u), here ? 16u + qwc * 16u : 16u);
                    if (!here && qwc) vif::memo_block(addr_full & (kRamSize - 16u), qwc * 16u);
                }
            }
            std::uint32_t data = c.tadr + 16;
            switch (id) {
            case 0: data = addr_full; c.tadr += 16; end = true; break;
            case 1: c.tadr = data + qwc * 16; break;
            case 2: c.tadr = addr_full; break;
            case 3: case 4: data = addr_full; c.tadr += 16; break;
            case 5: {
                const std::uint32_t asp = (c.chcr >> 4) & 3u;
                if (asp < 2) c.asr[asp] = data + qwc * 16;
                c.chcr = (c.chcr & ~0x30u) | (std::min<std::uint32_t>(asp + 1, 2u) << 4);
                c.tadr = addr_full;
                break;
            }
            case 6: {
                const std::uint32_t asp = (c.chcr >> 4) & 3u;
                if (asp > 0) {
                    c.tadr = c.asr[asp - 1];
                    c.chcr = (c.chcr & ~0x30u) | ((asp - 1) << 4);
                } else {
                    end = true;
                }
                break;
            }
            default: end = true; break;
            }
            c.madr = data;
            if (memo_call) {
                vif::MemoEnd left;
                if (vif::memo_play(c.tadr, left)) {
                    c.tadr = c.asr[0];
                    c.chcr = (c.chcr & 0xFFCFu) | left.chcr_tag;
                    c.madr = left.madr;
                    if (end || (tie && irq)) break;
                    continue;
                }
                memo_at = vif::memo_begin(c.tadr);
            }
            if (qwc) {
                const bool in_spr = (data & 0x80000000u) != 0u;
                const std::uint32_t off = in_spr ? (data & (kScratchpadSize - 1)) : (data & (kRamSize - 16u));
                const std::size_t bytes = static_cast<std::size_t>(qwc) * 16u;
                const std::size_t room = in_spr ? (kScratchpadSize - off) : (kRamSize - off);
                if (bytes <= room) {
                    const std::uint8_t *src = in_spr ? g_spr + off : g_ram + off;
                    const bool may_unchanging = !in_spr && !spr_page_hit(off, static_cast<std::uint32_t>(bytes));
                    vif::transfer_tte_data(1, tte_words, src, qwc, may_unchanging);
                } else {
                    vif::transfer_tte(1, tte_words);
                    deliver(1, data, qwc);
                }
            }
            c.madr = data + qwc * 16;
            if (memo_at && id == 6) {
                vif::memo_end(vif::MemoEnd{c.chcr & 0xFFFF0000u, c.madr});
                memo_at = false;
            }
            if (vif::stalled(1)) {
                if (memo_at) vif::memo_abort();
                pause(1, end || (tie && irq));
                return;
            }
            if (end || (tie && irq)) break;
        }
        if (memo_at) vif::memo_abort();
        c.qwc = 0;
        finish(1);
        return;
    }''',
         '''#ifdef PS2_PICA
    static const bool trace_tags1 = std::getenv("PS2_TRACE_DMA1") != nullptr;
    if (ch == 1 && tte && !mfifo && !trace_tags1) {
        run_chain_ch1(c, tie);
        return;
    }'''),
        ('''void start(int ch) {''',
         '''[[gnu::noinline]] void start(int ch) {'''),
        ('''std::uint32_t read(std::uint32_t a) {
    switch (a) {
    case 0x1000E000u: return g_ctrl;
    case 0x1000E020u: return g_pcr;
    case 0x1000E030u: return g_sqwc;
    case 0x1000E040u: return g_rbsr;
    case 0x1000E050u: return g_rbor;
    case 0x1000E060u: return g_stadr;
    default: break;
    }
    for (int ch = 0; ch < 10; ++ch) {
        if (a < kBase[ch] || a >= kBase[ch] + 0x100u) continue;
        const Channel &c = g_ch[ch];
        static const bool trace = std::getenv("PS2_TRACE_DMA") != nullptr;
        if (trace && ch == 4)
            std::fprintf(stderr, "[dma] ch4 read 0x%02X chcr 0x%08X madr 0x%08X qwc %u tadr 0x%08X\\n", a - kBase[ch], c.chcr, c.madr, c.qwc, c.tadr);
        switch (a - kBase[ch]) {
        case 0x00: return c.chcr;
        case 0x10: return c.madr;
        case 0x20: return c.qwc;
        case 0x30: return c.tadr;
        case 0x40: return c.asr[0];
        case 0x50: return c.asr[1];
        case 0x80: return c.sadr;
        default: return 0;
        }
    }
    return 0;
}''',
         '''inline int addr_to_ch(std::uint32_t a) {
    if (a < 0x10008000u || a >= 0x1000D800u || (a & 0x300u)) return -1;
    static constexpr std::int8_t kSlotToCh[22] = {0, -1, -1, -1, 1, -1, -1, -1, 2, -1, -1, -1, 3, 4, -1, -1, 5, 6, 7, -1, 8, 9};
    return kSlotToCh[(a - 0x10008000u) >> 10];
}

std::uint32_t read(std::uint32_t a) {
    switch (a) {
    case 0x1000E000u: return g_ctrl;
    case 0x1000E020u: return g_pcr;
    case 0x1000E030u: return g_sqwc;
    case 0x1000E040u: return g_rbsr;
    case 0x1000E050u: return g_rbor;
    case 0x1000E060u: return g_stadr;
    default: break;
    }
    const int ch = addr_to_ch(a);
    if (ch < 0) return 0;
    const Channel &c = g_ch[ch];
    static const bool trace = std::getenv("PS2_TRACE_DMA") != nullptr;
    if (trace && ch == 4)
        std::fprintf(stderr, "[dma] ch4 read 0x%02X chcr 0x%08X madr 0x%08X qwc %u tadr 0x%08X\\n", a & 0xFFu, c.chcr, c.madr, c.qwc, c.tadr);
    switch (a & 0xFFu) {
    case 0x00: return c.chcr;
    case 0x10: return c.madr;
    case 0x20: return c.qwc;
    case 0x30: return c.tadr;
    case 0x40: return c.asr[0];
    case 0x50: return c.asr[1];
    case 0x80: return c.sadr;
    default: return 0;
    }
}'''),
        ('''    for (int ch = 0; ch < 10; ++ch) {
        if (a < kBase[ch] || a >= kBase[ch] + 0x100u) continue;
        Channel &c = g_ch[ch];
        switch (a - kBase[ch]) {
        case 0x00: {
            const bool was = c.chcr & 0x100u;
            c.chcr = v;
            if (!(v & 0x100u)) {
                c.paused = false;
                if (ch == 1) g_mfifo_waiting = false;
            }
            if (!was && (v & 0x100u)) start(ch);
            break;
        }
        case 0x10: c.madr = v; break;
        case 0x20: c.qwc = v & 0xFFFFu; break;
        case 0x30: c.tadr = v; break;
        case 0x40: c.asr[0] = v; break;
        case 0x50: c.asr[1] = v; break;
        case 0x80: c.sadr = v & (kScratchpadSize - 16); break;
        default: break;
        }
        return;
    }''',
         '''    const int ch = addr_to_ch(a);
    if (ch < 0) return;
    Channel &c = g_ch[ch];
    switch (a & 0xFFu) {
    case 0x00: {
        const bool was = c.chcr & 0x100u;
        c.chcr = v;
        if (!(v & 0x100u)) {
            c.paused = false;
            if (ch == 1) g_mfifo_waiting = false;
        }
        if (!was && (v & 0x100u)) start(ch);
        break;
    }
    case 0x10: c.madr = v; break;
    case 0x20: c.qwc = v & 0xFFFFu; break;
    case 0x30: c.tadr = v; break;
    case 0x40: c.asr[0] = v; break;
    case 0x50: c.asr[1] = v; break;
    case 0x80: c.sadr = v & (kScratchpadSize - 16); break;
    default: break;
    }'''),
    ],
    'runtime/src/vif.cpp': [
        ('''    {{fast_pair<16>(1, false), fast_pair<16>(1, true)}, {fast_pair<16>(2, false), fast_pair<16>(2, true)}, {fast_pair<16>(3, false), fast_pair<16>(3, true)}, {fast_pair<16>(4, false), fast_pair<16>(4, true)}},
};''',
         '''    {{fast_pair<16>(1, false), fast_pair<16>(1, true)}, {fast_pair<16>(2, false), fast_pair<16>(2, true)}, {fast_pair<16>(3, false), fast_pair<16>(3, true)}, {fast_pair<16>(4, false), fast_pair<16>(4, true)}},
};

[[gnu::noinline]] void unpack_slow(int unit, Unit &u, bool dry, std::uint32_t cmd, const std::uint32_t *data,
                                   std::uint32_t vn, std::uint32_t vl, bool masked, bool usn, std::uint32_t num,
                                   std::uint32_t cl, std::uint32_t wl);'''),
        ('''        u.num = 0;
        return;
    }
    std::uint32_t addr = cmd & 0x3FFu;
    if (unit == 1 && ((cmd >> 15) & 1u)) addr += u.tops;
    const std::uint32_t components = vn + 1;
    const std::uint32_t bits = element_bits(vl);''',
         '''        u.num = 0;
        return;
    }
    unpack_slow(unit, u, dry, cmd, data, vn, vl, masked, usn, num, cl, wl);
}

[[gnu::noinline]] void unpack_slow(int unit, Unit &u, bool dry, std::uint32_t cmd, const std::uint32_t *data,
                                   std::uint32_t vn, std::uint32_t vl, bool masked, bool usn, std::uint32_t num,
                                   std::uint32_t cl, std::uint32_t wl) {
    std::uint32_t addr = cmd & 0x3FFu;
    if (unit == 1 && ((cmd >> 15) & 1u)) addr += u.tops;
    const std::uint32_t components = vn + 1;
    const std::uint32_t bits = element_bits(vl);'''),
        ('''template <bool FromVu, bool IsShadow, bool Record = true>
void execute(int unit, Unit &u, std::uint32_t cmd, const std::uint32_t *data) {''',
         '''template <bool FromVu, bool IsShadow, bool Record = true>
[[gnu::noinline]] void execute(int unit, Unit &u, std::uint32_t cmd, const std::uint32_t *data) {'''),
        ('''template <bool FromVu, bool IsShadow>
std::size_t run(int unit, Unit &u, const std::uint32_t *words, std::size_t count) {''',
         '''template <bool FromVu, bool IsShadow>
[[gnu::noinline]] std::size_t run(int unit, Unit &u, const std::uint32_t *words, std::size_t count) {'''),
        ('''bool run_unchanging(int unit, Unit &u, const std::uint8_t *data, std::uint32_t qwc) {''',
         '''[[gnu::noinline]] bool run_unchanging(int unit, Unit &u, const std::uint8_t *data, std::uint32_t qwc) {'''),
        ('''template <bool Capture>
std::size_t run_skipped(int unit, Unit &u, const std::uint32_t *words, std::size_t count) {''',
         '''template <bool Capture>
[[gnu::noinline]] std::size_t run_skipped(int unit, Unit &u, const std::uint32_t *words, std::size_t count) {'''),
        ('''std::uint32_t memo_hash(const MemoBlock *b, std::uint32_t n) {''',
         '''[[gnu::noinline]] std::uint32_t memo_hash(const MemoBlock *b, std::uint32_t n) {'''),
        ('''bool memo_rewritten(const MemoBlock *b, std::uint32_t n) {''',
         '''[[gnu::noinline]] bool memo_rewritten(const MemoBlock *b, std::uint32_t n) {'''),
    ],
}

def fail(msg):
    print("PATCH NOT APPLIED:", msg)
    raise SystemExit(1)


# Check everything first, so a refused patch changes nothing.
originals = {}
texts = {}
pending = 0

for key, edits in (*EDITS.items(), *PERF_EDITS.items(), *SPLIT_GS_EDITS.items(), *STACK_EDITS.items(), *NARROW_EDITS.items(), *LOOP_EDITS.items(), *SP_EDITS.items(), *HOT_EDITS.items(), *VIFADDR_EDITS.items(), *VIFREC_EDITS.items(), *IOPAHEAD_EDITS.items(), *STALE_EDITS.items(), *MOVIE_EDITS.items(), *SPLIT_SKIP_EDITS.items(), *SAMPLE_EDITS.items(), *G3_EDITS.items(), *RT_OPT_EDITS.items(), *UPLOAD_EDITS.items(), *UNCHANGING_WAYS_EDITS.items(), *FPU_FINITE_EDITS.items(), *UNCHANGING_SAMPLE_EDITS.items(), *CHAIN_MEMO_EDITS.items(), *UPLOAD_FAST_EDITS.items(), *VIF_CORE0_EDITS.items(), *VIF_CORE2_GS_EDITS.items(), *VIF_DMAC_ICACHE_EDITS.items()):
    rel = key.split('#')[0]   # the part after # only keeps two lists for one file apart
    if rel in texts:
        t = texts[rel]
    else:
        try:
            with open(root + '/' + rel, newline='') as f:
                t = originals[rel] = f.read()
        except OSError:
            fail("cannot read " + rel)
        # Unwind the later stages in memory so their inputs can be verified.
        for old, new in reversed((*PERF_EDITS.get(rel, ()), *SPLIT_GS_EDITS.get(rel, ()), *STACK_EDITS.get(rel, ()), *NARROW_EDITS.get(rel, ()), *LOOP_EDITS.get(rel, ()), *SP_EDITS.get(rel, ()), *HOT_EDITS.get(rel, ()), *VIFADDR_EDITS.get(rel, ()), *VIFREC_EDITS.get(rel, ()), *IOPAHEAD_EDITS.get(rel, ()), *STALE_EDITS.get(rel, ()), *MOVIE_EDITS.get(rel, ()), *SPLIT_SKIP_EDITS.get(rel, ()), *SAMPLE_EDITS.get(rel, ()), *G3_EDITS.get(rel, ()), *RT_OPT_EDITS.get(rel, ()), *UPLOAD_EDITS.get(rel, ()), *UNCHANGING_WAYS_EDITS.get(rel, ()), *FPU_FINITE_EDITS.get(rel, ()), *UNCHANGING_SAMPLE_EDITS.get(rel, ()), *CHAIN_MEMO_EDITS.get(rel, ()), *UPLOAD_FAST_EDITS.get(rel, ()), *VIF_CORE0_EDITS.get(rel, ()), *VIF_CORE2_GS_EDITS.get(rel, ()), *VIF_DMAC_ICACHE_EDITS.get(rel, ()))):
            if t.count(new) == 1:
                t = t.replace(new, old)
                pending -= 1
    for old, new in edits:
        if new in t:
            continue
        if t.count(old) != 1:
            fail(rel + " does not look like the expected source (cannot find: " + old.strip().splitlines()[0][:70] + ")")
        t = t.replace(old, new)
        pending += 1
    texts[rel] = t

changed = {rel: t for rel, t in texts.items() if t != originals[rel]}
if not changed:
    print("already patched")
    raise SystemExit

for rel, t in changed.items():
    with open(root + '/' + rel, 'w', newline='') as f:
        f.write(t)
print("patched (%d edits)" % pending)
