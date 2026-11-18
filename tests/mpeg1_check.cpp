// Test aid for tests/test_3ds_movies.py: decodes a .p3v with port3ds/mpeg1.cpp and writes the pictures as raw yuv420p.
#include "../port3ds/mpeg1.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    std::FILE *in = std::fopen(argv[1], "rb"), *out = std::fopen(argv[2], "wb");
    if (!in || !out) return 2;
    unsigned char head[16];
    if (std::fread(head, 1, 16, in) != 16 || std::memcmp(head, "P3V1", 4)) return 3;
    unsigned count;
    std::memcpy(&count, head + 12, 4);
    std::vector<unsigned> offsets(count + 1);
    if (std::fread(offsets.data(), 4, count + 1, in) != count + 1) return 3;
    mpeg1::Decoder d;
    std::vector<unsigned char> unit;
    double seconds = 0;
    for (unsigned n = 0; n < count; ++n) {
        unit.resize(offsets[n + 1] - offsets[n]);
        std::fseek(in, static_cast<long>(offsets[n]), SEEK_SET);
        if (std::fread(unit.data(), 1, unit.size(), in) != unit.size()) return 3;
        const auto start = std::chrono::steady_clock::now();
        if (!d.decode(unit.data(), unit.size())) {
            std::fprintf(stderr, "picture %u does not decode\n", n);
            return 1;
        }
        seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const std::size_t luma = static_cast<std::size_t>(d.width) * d.height;
        std::fwrite(d.luma(), 1, luma, out);
        std::fwrite(d.blue(), 1, luma / 4, out);
        std::fwrite(d.red(), 1, luma / 4, out);
    }
    std::fprintf(stderr, "%u pictures of %dx%d, %.2f ms each\n", count, d.width, d.height, seconds * 1000 / (count ? count : 1));
    return 0;
}
