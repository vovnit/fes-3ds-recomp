// MPEG-1 video, I and P pictures (ISO 11172-2). See mpeg1.hpp.
#include "mpeg1.hpp"

#include "ipu/tables.hpp"   // the code tables the runtime's IPU already carries

#include <cstdlib>
#include <cstring>

namespace mpeg1 {

namespace {

using namespace ps2::ipu::tables;

struct Code {
    std::uint8_t length;
    std::uint8_t run;
    std::int16_t value;
};

struct Tables {
    Code increment[2048];    // 11 bits
    Code motion[2048];
    Code pattern[512];       // 9 bits
    Code type_p[64];         // 6 bits
    Code dc_luma[512], dc_chroma[1024];
    Code short_codes[256];   // coefficient codes of up to 8 bits
    Code long_codes[1024];   // longer ones: they all begin with six zero bits, and this is the ten bits after those
};

template <std::size_t N, std::size_t M>
void fill(Code (&out)[M], const Vlc (&in)[N], unsigned bits) {
    std::memset(out, 0, sizeof out);
    for (const Vlc &v : in) {
        const unsigned first = static_cast<unsigned>(v.code) << (bits - v.length);
        for (unsigned i = 0; i < (1u << (bits - v.length)); ++i)
            out[first + i] = Code{v.length, 0, static_cast<std::int16_t>(v.value & 0xFFFFu)};
    }
}

const Tables &tables() {
    static Tables *t = nullptr;
    if (t) return *t;
    t = static_cast<Tables *>(std::malloc(sizeof(Tables)));
    fill(t->increment, kMbAddressIncrement, 11);
    fill(t->motion, kMotionCode, 11);
    fill(t->pattern, kCodedBlockPattern, 9);
    fill(t->type_p, kMbTypeP, 6);
    fill(t->dc_luma, kDcSizeLuma, 9);
    fill(t->dc_chroma, kDcSizeChroma, 10);
    std::memset(t->short_codes, 0, sizeof t->short_codes);
    std::memset(t->long_codes, 0, sizeof t->long_codes);
    for (const Dct &d : kDctTable0) {
        const Code code{d.length, d.run, d.level};
        if (d.length <= 8) {
            const unsigned first = static_cast<unsigned>(d.code) << (8 - d.length);
            for (unsigned i = 0; i < (1u << (8 - d.length)); ++i) t->short_codes[first + i] = code;
        } else {
            const unsigned first = static_cast<unsigned>(d.code) << (16 - d.length);
            for (unsigned i = 0; i < (1u << (16 - d.length)); ++i) t->long_codes[(first + i) & 1023u] = code;
        }
    }
    return *t;
}

constexpr std::uint8_t kDefaultIntra[64] = {
    8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37, 19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58, 26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83,
};

constexpr int kW1 = 2841, kW2 = 2676, kW3 = 2408, kW5 = 1609, kW6 = 1108, kW7 = 565;   // 2048 * sqrt(2) * cos(n * pi / 16)

inline void idct_row(int *b) {
    int x1 = b[4] << 11, x2 = b[6], x3 = b[2], x4 = b[1], x5 = b[7], x6 = b[5], x7 = b[3];
    if (!(x1 | x2 | x3 | x4 | x5 | x6 | x7)) {
        b[0] = b[1] = b[2] = b[3] = b[4] = b[5] = b[6] = b[7] = b[0] << 3;
        return;
    }
    int x0 = (b[0] << 11) + 128;
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

inline void idct_column(int *b) {
    int x1 = b[32] << 8, x2 = b[48], x3 = b[16], x4 = b[8], x5 = b[56], x6 = b[40], x7 = b[24];
    if (!(x1 | x2 | x3 | x4 | x5 | x6 | x7)) {
        b[0] = b[8] = b[16] = b[24] = b[32] = b[40] = b[48] = b[56] = (b[0] + 32) >> 6;
        return;
    }
    int x0 = (b[0] << 8) + 8192;
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
    b[0] = (x7 + x1) >> 14;
    b[8] = (x3 + x2) >> 14;
    b[16] = (x0 + x4) >> 14;
    b[24] = (x8 + x6) >> 14;
    b[32] = (x8 - x6) >> 14;
    b[40] = (x0 - x4) >> 14;
    b[48] = (x3 - x2) >> 14;
    b[56] = (x7 - x1) >> 14;
}

// rows: how many rows from the top hold anything
inline void idct(int *b, int rows) {
    for (int i = 0; i < rows; ++i) idct_row(b + i * 8);
    for (int i = 0; i < 8; ++i) idct_column(b + i);
}

inline std::uint8_t clip(int v) { return static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

inline void put(std::uint8_t *to, int stride, const int *b) {
    for (int y = 0; y < 8; ++y, to += stride, b += 8)
        for (int x = 0; x < 8; ++x) to[x] = clip(b[x]);
}

inline void add(std::uint8_t *to, int stride, const int *b) {
    for (int y = 0; y < 8; ++y, to += stride, b += 8)
        for (int x = 0; x < 8; ++x) to[x] = clip(to[x] + b[x]);
}

// A square of size n, moved by half samples when hx or hy is set.
void copy_block(std::uint8_t *to, const std::uint8_t *from, int stride, int n, int hx, int hy) {
    if (!hx && !hy) {
        for (int y = 0; y < n; ++y, to += stride, from += stride) std::memcpy(to, from, static_cast<std::size_t>(n));
    } else if (hx && !hy) {
        for (int y = 0; y < n; ++y, to += stride, from += stride)
            for (int x = 0; x < n; ++x) to[x] = static_cast<std::uint8_t>((from[x] + from[x + 1] + 1) >> 1);
    } else if (!hx) {
        for (int y = 0; y < n; ++y, to += stride, from += stride)
            for (int x = 0; x < n; ++x) to[x] = static_cast<std::uint8_t>((from[x] + from[x + stride] + 1) >> 1);
    } else {
        for (int y = 0; y < n; ++y, to += stride, from += stride)
            for (int x = 0; x < n; ++x)
                to[x] = static_cast<std::uint8_t>((from[x] + from[x + 1] + from[x + stride] + from[x + stride + 1] + 2) >> 2);
    }
}

}

struct Decoder::Bits {
    const std::uint8_t *at, *end;
    std::uint32_t cache = 0;   // the next bits, from the top
    int count = 0;

    inline void fill() {
        while (count <= 24) {
            cache |= static_cast<std::uint32_t>(at < end ? *at : 0) << (24 - count);
            ++at;
            count += 8;
        }
    }
    inline std::uint32_t peek(int n) {
        fill();
        return cache >> (32 - n);
    }
    inline void skip(int n) {
        cache <<= n;
        count -= n;
    }
    inline std::uint32_t get(int n) {
        const std::uint32_t v = peek(n);
        skip(n);
        return v;
    }
    const std::uint8_t *byte() const { return at - (count + 7) / 8; }   // where the next whole byte not yet read is
    bool past() const { return at - count / 8 > end + 4; }
};

Decoder::~Decoder() {
    std::free(y_[0]);
    std::free(y_[1]);
}

bool Decoder::sequence(const std::uint8_t *p, const std::uint8_t *end) {
    if (end - p < 8) return false;
    const int w = (p[0] << 4) | (p[1] >> 4), h = ((p[1] & 15) << 8) | p[2];
    if (!w || !h || w > 1024 || h > 1024) return false;
    if (w != width || h != height) {
        std::free(y_[0]);
        std::free(y_[1]);
        width = w;
        height = h;
        mb_width = (w + 15) / 16;
        mb_height = (h + 15) / 16;
        const std::size_t luma = static_cast<std::size_t>(mb_width * 16) * (mb_height * 16), chroma = luma / 4;
        for (int i = 0; i < 2; ++i) {
            y_[i] = static_cast<std::uint8_t *>(std::malloc(luma + 2 * chroma + 64));   // 64: a half-sample read past the last line
            if (!y_[i]) return false;
            std::memset(y_[i], 0, luma + 2 * chroma + 64);
            cb_[i] = y_[i] + luma;
            cr_[i] = cb_[i] + chroma;
        }
        have_reference_ = false;
    }
    // After the size and the shape and rate codes: bit rate (18), a marker, buffer size (10), a flag, then the matrices.
    Bits m{p + 4, end};
    m.get(15);
    m.get(15);
    if (m.get(1)) {
        for (int i = 0; i < 64; ++i) intra_matrix_[kZigzag[i]] = static_cast<std::uint8_t>(m.get(8));
    } else {
        std::memcpy(intra_matrix_, kDefaultIntra, 64);
    }
    if (m.get(1)) {
        for (int i = 0; i < 64; ++i) inter_matrix_[kZigzag[i]] = static_cast<std::uint8_t>(m.get(8));
    } else {
        std::memset(inter_matrix_, 16, 64);
    }
    return true;
}

template <bool intra>
bool Decoder::block(Bits &b, int *c, int plane) {
    const Tables &t = tables();
    std::memset(c, 0, 64 * sizeof(int));
    int i = 0, last = 0;
    bool first = false;   // the next code is a block's first one, where "10" is not an end
    const std::uint8_t *matrix = intra ? intra_matrix_ : inter_matrix_;
    const int q = quantiser_;
    if (intra) {
        const Code &size = plane ? t.dc_chroma[b.peek(10)] : t.dc_luma[b.peek(9)];
        if (!size.length) return false;
        b.skip(size.length);
        int diff = 0;
        if (size.value) {
            const int v = static_cast<int>(b.get(size.value));
            diff = v < (1 << (size.value - 1)) ? v + 1 - (1 << size.value) : v;
        }
        dc_[plane] += diff * 8;
        c[0] = dc_[plane];
    } else {
        // The first coefficient has a code of its own for +1 and -1.
        if (b.peek(1)) {
            const int sign = static_cast<int>(b.get(2)) & 1;
            int v = (3 * q * matrix[0]) >> 4;
            if (v) v = (v - 1) | 1;
            c[0] = sign ? -v : v;
        } else {
            i = -1;
            first = true;
        }
    }
    for (;;) {
        const std::uint32_t bits = b.peek(16);
        if (!first && (bits >> 14) == 2u) {
            b.skip(2);
            break;
        }
        first = false;
        const Code &code = (bits >> 10) ? t.short_codes[bits >> 8] : t.long_codes[bits & 1023u];
        if (!code.length) return false;
        b.skip(code.length);
        int run, level;
        if (code.run == kEscape) {
            run = static_cast<int>(b.get(6));
            level = static_cast<int>(b.get(8));
            if (level == 0) level = static_cast<int>(b.get(8));
            else if (level == 128) level = static_cast<int>(b.get(8)) - 256;
            else if (level > 128) level -= 256;
        } else {
            run = code.run;
            level = b.get(1) ? -code.value : code.value;
        }
        i += run + 1;
        if (i > 63) return false;
        const int j = kZigzag[i];
        int v = level < 0 ? -level : level;
        v = intra ? (2 * v * q * matrix[j]) >> 4 : ((2 * v + 1) * q * matrix[j]) >> 4;
        if (v) v = (v - 1) | 1;
        if (v > 2047) v = 2047;
        c[j] = level < 0 ? -v : v;
        if ((j >> 3) > last) last = j >> 3;
    }
    idct(c, last + 1);
    return true;
}

void Decoder::predict(int mb_x, int mb_y, int mv_x, int mv_y) {
    const int w = mb_width * 16, h = mb_height * 16, ref = now_ ^ 1;
    // Vectors are in half samples and must stay inside the picture; a broken one is pulled back in.
    int x = mb_x * 32 + mv_x, y = mb_y * 32 + mv_y;
    x = x < 0 ? 0 : (x > (w - 16) * 2 ? (w - 16) * 2 : x);
    y = y < 0 ? 0 : (y > (h - 16) * 2 ? (h - 16) * 2 : y);
    copy_block(y_[now_] + mb_y * 16 * w + mb_x * 16, y_[ref] + (y >> 1) * w + (x >> 1), w, 16, x & 1, y & 1);
    const int cw = w / 2, ch = h / 2;
    int cx = mb_x * 16 + mv_x / 2, cy = mb_y * 16 + mv_y / 2;
    cx = cx < 0 ? 0 : (cx > (cw - 8) * 2 ? (cw - 8) * 2 : cx);
    cy = cy < 0 ? 0 : (cy > (ch - 8) * 2 ? (ch - 8) * 2 : cy);
    const std::size_t to = static_cast<std::size_t>(mb_y * 8 * cw + mb_x * 8), from = static_cast<std::size_t>((cy >> 1) * cw + (cx >> 1));
    copy_block(cb_[now_] + to, cb_[ref] + from, cw, 8, cx & 1, cy & 1);
    copy_block(cr_[now_] + to, cr_[ref] + from, cw, 8, cx & 1, cy & 1);
}

bool Decoder::slice(Bits &b, int row) {
    const Tables &t = tables();
    const int w = mb_width * 16, cw = w / 2, total = mb_width * mb_height;
    quantiser_ = static_cast<int>(b.get(5));
    while (b.get(1)) b.skip(8);
    int address = row * mb_width - 1;
    int pmv_x = 0, pmv_y = 0;
    bool first = true, last_intra = false;
    dc_[0] = dc_[1] = dc_[2] = 1024;
    int c[64];
    for (;;) {
        int increment = 0;
        for (;;) {
            const std::uint32_t bits = b.peek(11);
            if (bits == 15u) {            // stuffing
                b.skip(11);
            } else if (bits == 8u) {      // escape: 33 more
                b.skip(11);
                increment += 33;
            } else {
                const Code &code = t.increment[bits];
                if (!code.length) return false;
                b.skip(code.length);
                increment += code.value;
                break;
            }
        }
        if (first) {
            address += increment;
        } else {
            if (increment > 1) {
                if (type_ != 2) return false;
                for (int k = 1; k < increment; ++k) {
                    const int a = address + k;
                    if (a >= total) return false;
                    predict(a % mb_width, a / mb_width, 0, 0);
                }
                pmv_x = pmv_y = 0;
                last_intra = false;
            }
            address += increment;
        }
        first = false;
        if (address >= total) return false;
        const int mb_x = address % mb_width, mb_y = address / mb_width;

        int type;
        if (type_ == 1) {
            const std::uint32_t bits = b.peek(2);
            if (bits & 2u) type = 0x01, b.skip(1);
            else if (bits == 1u) type = 0x11, b.skip(2);
            else return false;
        } else {
            const Code &code = t.type_p[b.peek(6)];
            if (!code.length) return false;
            b.skip(code.length);
            type = code.value;
        }
        if (type & 0x10) quantiser_ = static_cast<int>(b.get(5));
        if (!quantiser_) return false;

        if (type & 0x01) {
            if (!last_intra) dc_[0] = dc_[1] = dc_[2] = 1024;
            last_intra = true;
            pmv_x = pmv_y = 0;
            std::uint8_t *luma = y_[now_] + mb_y * 16 * w + mb_x * 16;
            for (int i = 0; i < 4; ++i) {
                if (!block<true>(b, c, 0)) return false;
                put(luma + (i >> 1) * 8 * w + (i & 1) * 8, w, c);
            }
            if (!block<true>(b, c, 1)) return false;
            put(cb_[now_] + mb_y * 8 * cw + mb_x * 8, cw, c);
            if (!block<true>(b, c, 2)) return false;
            put(cr_[now_] + mb_y * 8 * cw + mb_x * 8, cw, c);
        } else {
            last_intra = false;
            if (type & 0x08) {
                for (int axis = 0; axis < 2; ++axis) {
                    const Code &code = t.motion[b.peek(11)];
                    if (!code.length) return false;
                    b.skip(code.length);
                    int delta = static_cast<std::int16_t>(code.value);
                    if (delta && f_shift_) {
                        const int r = static_cast<int>(b.get(f_shift_));
                        const int magnitude = (((delta < 0 ? -delta : delta) - 1) << f_shift_) + r + 1;
                        delta = delta < 0 ? -magnitude : magnitude;
                    }
                    int &p = axis ? pmv_y : pmv_x;
                    const int range = 16 << f_shift_;
                    int v = p + delta;
                    if (v >= range) v -= 2 * range;
                    else if (v < -range) v += 2 * range;
                    p = v;
                }
            } else {
                pmv_x = pmv_y = 0;
            }
            predict(mb_x, mb_y, pmv_x << full_pel_, pmv_y << full_pel_);
            if (type & 0x02) {
                const Code &code = t.pattern[b.peek(9)];
                if (!code.length) return false;
                b.skip(code.length);
                const int cbp = code.value;
                std::uint8_t *luma = y_[now_] + mb_y * 16 * w + mb_x * 16;
                for (int i = 0; i < 4; ++i)
                    if (cbp & (32 >> i)) {
                        if (!block<false>(b, c, 0)) return false;
                        add(luma + (i >> 1) * 8 * w + (i & 1) * 8, w, c);
                    }
                if (cbp & 2) {
                    if (!block<false>(b, c, 1)) return false;
                    add(cb_[now_] + mb_y * 8 * cw + mb_x * 8, cw, c);
                }
                if (cbp & 1) {
                    if (!block<false>(b, c, 2)) return false;
                    add(cr_[now_] + mb_y * 8 * cw + mb_x * 8, cw, c);
                }
            }
        }
        if (b.past()) return false;
        if (!b.peek(23)) return true;   // 23 zero bits: the next start code
    }
}

bool Decoder::decode(const std::uint8_t *data, std::size_t size) {
    const std::uint8_t *p = data, *end = data + size;
    bool picture = false, any_slice = false;
    while (end - p >= 4) {
        const auto *found = static_cast<const std::uint8_t *>(std::memchr(p, 0, static_cast<std::size_t>(end - p - 3)));
        if (!found) break;
        if (found[1] != 0 || found[2] != 1) {
            p = found + 1;
            continue;
        }
        const unsigned code = found[3];
        p = found + 4;
        if (code == 0xB3) {
            if (!sequence(p, end)) return false;
        } else if (code == 0x00) {
            if (!width || end - p < 4) return false;
            type_ = (p[1] >> 3) & 7;
            if (type_ == 2) {
                if (!have_reference_) return false;
                // after temporal reference (10), type (3) and delay (16): full_pel (1), f_code (3)
                const unsigned v = (static_cast<unsigned>(p[3]) << 8) | (end - p > 4 ? p[4] : 0);
                full_pel_ = (v >> 10) & 1;
                const int f_code = (v >> 7) & 7;
                if (!f_code) return false;
                f_shift_ = f_code - 1;
            } else if (type_ != 1) {
                return false;
            }
            now_ ^= 1;
            if (type_ == 2) {
                // Rows that never arrive keep the picture before.
                const std::size_t luma = static_cast<std::size_t>(mb_width * 16) * (mb_height * 16);
                std::memcpy(y_[now_], y_[now_ ^ 1], luma + luma / 2);
            }
            picture = true;
        } else if (code >= 0x01 && code <= 0xAF) {
            if (!picture || static_cast<int>(code) > mb_height) return false;
            Bits b{p, end};
            if (!slice(b, static_cast<int>(code) - 1)) return false;
            any_slice = true;
            p = b.byte();
            if (p > end) p = end;
        }
    }
    if (picture && any_slice) have_reference_ = true;
    return picture && any_slice;
}

}
