// A small MPEG-1 video decoder for the movies tools/p3fes_3ds_movies.py makes: I and P pictures only, one picture per call.
// It has no 3DS code in it, so tests/test_3ds_movies.py can build it on the computer and compare it with ffmpeg.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mpeg1 {

struct Decoder {
    int width = 0, height = 0;          // set by the first sequence header
    int mb_width = 0, mb_height = 0;
    // The newest picture. Luma is width x height, the two chroma planes half that each way; lines follow each other with no gap.
    const std::uint8_t *luma() const { return y_[now_]; }
    const std::uint8_t *blue() const { return cb_[now_]; }
    const std::uint8_t *red() const { return cr_[now_]; }

    // One picture with the headers that come before it. False if the data is broken or a P picture has nothing to build on.
    bool decode(const std::uint8_t *data, std::size_t size);
    void forget() { have_reference_ = false; }   // before starting again at a key picture
    ~Decoder();

private:
    struct Bits;
    bool sequence(const std::uint8_t *p, const std::uint8_t *end);
    bool slice(Bits &b, int row);
    void predict(int mb_x, int mb_y, int mv_x, int mv_y);
    template <bool intra> bool block(Bits &b, int *coefficients, int plane);

    std::uint8_t *y_[2]{}, *cb_[2]{}, *cr_[2]{};
    int now_ = 0;
    bool have_reference_ = false, picture_open_ = false;
    int type_ = 0, full_pel_ = 0, f_shift_ = 0;
    int quantiser_ = 1;
    int dc_[3]{};
    std::uint8_t intra_matrix_[64], inter_matrix_[64];
};

}
