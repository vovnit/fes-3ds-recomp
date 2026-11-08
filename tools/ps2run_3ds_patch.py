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
void iop_wait();
struct IopGuard {
    IopGuard() { iop_lock(); }
    ~IopGuard() { iop_unlock(); }
};
#else
inline void iop_wait() {}
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
    if (now() < g_iop_tight) iop_wait();
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


def fail(msg):
    print("PATCH NOT APPLIED:", msg)
    raise SystemExit(1)


# Check everything first, so a refused patch changes nothing.
originals = {}
texts = {}
pending = 0
for key, edits in (*EDITS.items(), *PERF_EDITS.items(), *SPLIT_GS_EDITS.items()):
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
        for old, new in reversed((*PERF_EDITS.get(rel, ()), *SPLIT_GS_EDITS.get(rel, ()))):
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
