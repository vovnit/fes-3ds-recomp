// Runs port3ds/g3model.cpp on a computer against batches captured from the game (sdmc:/p3fes/g3cap.bin, written by a 3DS build
// with "g3 3" in args.txt) and compares the model's GIF packets with the ones the VU1 program kicked.
//
//   c++ -std=c++20 -O2 -ffp-contract=off -DG3_HOST_CHECK -Iport3ds tools/g3check.cpp port3ds/g3model.cpp -o build/g3check
//   build/g3check g3cap.bin [-v N] [-gpu]   (-v: describe the first N differing batches vertex by vertex; -gpu: check the GPU
//                                            path's batches and shader arithmetic against the exact model)
//
// The capture holds VU1 memory, which comes from your own disc: keep it on your computer.
#include "g3model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

struct Record {
    std::uint32_t program, address, top, itop;
    std::vector<std::uint8_t> regs, mem, packets;
};

bool read_record(std::FILE *f, Record &r) {
    std::uint32_t head[7];
    if (std::fread(head, sizeof head, 1, f) != 1) return false;
    if (head[0] != 0x50433347u) {
        std::fprintf(stderr, "bad record\n");
        return false;
    }
    r.program = head[1];
    r.address = head[2];
    r.top = head[3];
    r.itop = head[4];
    r.regs.resize(head[5]);
    r.mem.resize(16384);
    r.packets.resize(head[6]);
    return std::fread(r.regs.data(), 1, r.regs.size(), f) == r.regs.size() && std::fread(r.mem.data(), 1, 16384, f) == 16384 &&
           std::fread(r.packets.data(), 1, r.packets.size(), f) == r.packets.size();
}

std::uint32_t word(const std::vector<std::uint8_t> &v, std::size_t at) {
    std::uint32_t w = 0;
    if (at + 4 <= v.size()) std::memcpy(&w, v.data() + at, 4);
    return w;
}

float as_float(std::uint32_t w) {
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}

// Walks a GIF packet stream and prints each PACKED register qword of model and program side by side where they differ.
void describe(const Record &r, const std::vector<std::uint8_t> &model, int max_lines) {
    std::size_t at = 0;
    int lines = 0;
    while (at + 16 <= r.packets.size() && lines < max_lines) {
        std::uint64_t tag;
        std::memcpy(&tag, r.packets.data() + at, 8);
        const std::uint64_t regs = word(r.packets, at + 8) | static_cast<std::uint64_t>(word(r.packets, at + 12)) << 32;
        const unsigned nloop = tag & 0x7FFFu, nreg = ((tag >> 60) & 15u) ? (tag >> 60) & 15u : 16u;
        if (std::memcmp(r.packets.data() + at, model.size() >= at + 16 ? model.data() + at : r.packets.data(), 16) != 0 || model.size() < at + 16) {
            std::printf("    tag @%zu: program %016llX %016llX, model %08X %08X %08X %08X\n", at / 16, static_cast<unsigned long long>(tag),
                        static_cast<unsigned long long>(regs), word(model, at), word(model, at + 4), word(model, at + 8), word(model, at + 12));
            ++lines;
        }
        at += 16;
        for (unsigned l = 0; l < nloop; ++l)
            for (unsigned k = 0; k < nreg && at + 16 <= r.packets.size(); ++k, at += 16) {
                const unsigned reg = (regs >> (4 * k)) & 15u;
                if (model.size() >= at + 16 && std::memcmp(r.packets.data() + at, model.data() + at, 16) == 0) continue;
                if (lines++ >= max_lines) return;
                const char *name = reg == 2 ? "STQ" : reg == 1 ? "RGBAQ" : reg == 4 ? "XYZF2" : reg == 5 ? "XYZ2" : "reg";
                std::printf("    v%u %-5s program", l, name);
                for (int i = 0; i < 4; ++i) {
                    const std::uint32_t w = word(r.packets, at + 4 * i);
                    if (reg == 2) std::printf(" %.7g", as_float(w)); else std::printf(" %08X", w);
                }
                std::printf("  model");
                for (int i = 0; i < 4; ++i) {
                    const std::uint32_t w = word(model, at + 4 * i);
                    if (reg == 2) std::printf(" %.7g", as_float(w)); else std::printf(" %08X", w);
                }
                std::printf("\n");
            }
        if ((tag >> 15) & 1u) {
            // The next packet of the batch (each XGKICK is one packet with EOP).
        }
    }
}

// Compares two packet streams the way the GS reads them: the w word of STQ is not used, so it is not compared. Counts the
// qwords that differ by register (index: 0 tag, 1 RGBAQ, 2 STQ, 4 XYZF2, 5 XYZ2).
bool same_for_gs(const std::vector<std::uint8_t> &program, const std::vector<std::uint8_t> &model, unsigned *differ) {
    if (program.size() != model.size()) {
        ++differ[0];
        return false;
    }
    bool same = true;
    std::size_t at = 0;
    while (at + 16 <= program.size()) {
        std::uint64_t tag;
        std::memcpy(&tag, program.data() + at, 8);
        const std::uint64_t regs = word(program, at + 8) | static_cast<std::uint64_t>(word(program, at + 12)) << 32;
        const unsigned nloop = tag & 0x7FFFu, nreg = ((tag >> 60) & 15u) ? (tag >> 60) & 15u : 16u;
        if (std::memcmp(program.data() + at, model.data() + at, 16) != 0) {
            ++differ[0];
            same = false;
        }
        at += 16;
        for (unsigned l = 0; l < nloop; ++l)
            for (unsigned k = 0; k < nreg && at + 16 <= program.size(); ++k, at += 16) {
                const unsigned reg = (regs >> (4 * k)) & 15u;
                const std::size_t bytes = reg == 2 ? 12 : 16;
                if (std::memcmp(program.data() + at, model.data() + at, bytes) != 0) {
                    ++differ[reg & 15u];
                    same = false;
                }
            }
    }
    return same;
}

// The GPU path (g3::gpu_batch and the g3 half of port3ds/gs.v.pica) worked out here in plain floats, compared with the exact
// model's packets: screen x and y (in GS pixels), z, colour, fog, and which triangles are drawn.
struct GpuCheck {
    unsigned batches = 0, drawn = 0, nothing = 0, declined[g3::kGpuDeclines] = {}, vertices = 0, triangles = 0, facing_wrong = 0;
    double worst_xy = 0, worst_z = 0, worst_colour = 0, worst_fog = 0;
    unsigned xy_over = 0, colour_over = 0, fog_over = 0;
};

void check_gpu(const g3::Input &in, const std::vector<std::uint8_t> &model, bool modelled, GpuCheck &c) {
    static g3::Gpu g;
    g3::End end;
    ++c.batches;
    const int got = g3::gpu_batch(in, g, end);
    if (got > 0) {
        ++c.declined[got];
        return;
    }
    if (got < 0) {
        ++c.nothing;
        return;
    }
    if (!modelled || model.size() != 16u + 48u * g.n) {
        ++c.declined[0];
        return;
    }
    ++c.drawn;
    float sx[256], sy[256];
    for (unsigned k = 0; k < g.n; ++k) {
        const g3::GpuVertex &v = g.v[k];
        float p[4] = {v.x, v.y, v.z, 1.0f}, clip[4], nrm[4] = {v.normal[0] / 128.0f, v.normal[1] / 128.0f, v.normal[2] / 128.0f, 0.0f};
        if (g.skin) {
            float m[3][4] = {};
            for (int i = 0; i < 4; ++i)
                for (int c = 0; c < 3; ++c)
                    for (int j = 0; j < 4; ++j) m[c][j] += v.weight[i] * g.bone[v.bone[i] + c][j];
            const float n0[3] = {nrm[0], nrm[1], nrm[2]};
            for (int c = 0; c < 3; ++c) {
                p[c] = m[c][0] * v.x + m[c][1] * v.y + m[c][2] * v.z + m[c][3];
                nrm[c] = m[c][0] * n0[0] + m[c][1] * n0[1] + m[c][2] * n0[2];
            }
        }
        for (int j = 0; j < 4; ++j) clip[j] = g.proj[3][j] + g.proj[0][j] * p[0] + g.proj[1][j] * p[1] + g.proj[2][j] * p[2];
        const float q = 1.0f / clip[3];
        sx[k] = clip[0] * q + g.offset[0];
        sy[k] = clip[1] * q + g.offset[1];
        const float sz = (clip[2] * q + g.offset[2]) * (g.fog ? 1.0f : 16.0f);
        float colour[4];
        float d[3];
        for (int l = 0; l < 3; ++l) d[l] = std::max(0.0f, nrm[0] * g.light_dir[l][0] + nrm[1] * g.light_dir[l][1] + nrm[2] * g.light_dir[l][2]);
        for (int j = 0; j < 4; ++j) {
            float x = v.colour[j] + g.ambient[j];
            for (int l = 0; l < 3; ++l) x += g.light_colour[l][j] * d[l];
            colour[j] = std::min(x, 255.0f) * g.material[j];
        }
        const std::size_t at = 16u + 48u * k;
        const std::uint32_t X = word(model, at + 32) & 0xFFFFu, Y = word(model, at + 36) & 0xFFFFu, Z = word(model, at + 40);
        const double dx = std::fabs(sx[k] * 16.0 - X), dy = std::fabs(sy[k] * 16.0 - Y);
        const double z = g.fog ? (Z >> 4) & 0xFFFFFFu : Z;
        const double dz = std::fabs(sz - z) / std::max(1.0, z);
        c.worst_xy = std::max({c.worst_xy, dx, dy});
        c.worst_z = std::max(c.worst_z, dz);
        if (dx > 16 || dy > 16) ++c.xy_over;
        for (int j = 0; j < 4; ++j) {
            const double dc = std::fabs(colour[j] - static_cast<double>(word(model, at + 16 + 4 * j) & 0xFFu));
            c.worst_colour = std::max(c.worst_colour, dc);
            if (dc > 2) ++c.colour_over;
        }
        if (g.fog) {
            const float f = std::min(255.0f, std::max(0.0f, g.fog_scale * (clip[3] - g.fog_near)));
            const double df = std::fabs(f - static_cast<double>((word(model, at + 44) >> 4) & 0xFFu));
            c.worst_fog = std::max(c.worst_fog, df);
            if (df > 2) ++c.fog_over;
        }
        ++c.vertices;
        if (k < 2) continue;
        // Odd triangles as (k-1, k-2, k): the program's facing_negative is then this area above zero, in GS's y-down pixels.
        const unsigned a = k & 1u ? k - 1 : k - 2, b = k & 1u ? k - 2 : k - 1;
        const float area = (sx[b] - sx[a]) * (sy[k] - sy[a]) - (sx[k] - sx[a]) * (sy[b] - sy[a]);
        const bool gpu_draws = area > 0 ? g.keep_negative : g.keep_positive;
        const bool program_draws = !((word(model, at + 44) >> 15) & 1u);
        ++c.triangles;
        if (gpu_draws != program_draws && std::fabs(area) > 0.5f) ++c.facing_wrong;
    }
}

}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: g3check g3cap.bin [-v N]\n");
        return 2;
    }
    int verbose = 0;
    bool gpu = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-v") && i + 1 < argc) verbose = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "-gpu")) gpu = true;
    }
    GpuCheck gc;
    std::FILE *f = std::fopen(argv[1], "rb");
    if (!f) {
        std::perror(argv[1]);
        return 1;
    }
    struct Count { unsigned total = 0, modelled = 0, same = 0, differ[16] = {}; };
    std::map<std::string, Count> counts;
    Record r;
    unsigned described = 0, n = 0;
    while (read_record(f, r)) {
        ++n;
        g3::Input in{};
        in.program = static_cast<int>(r.program);
        in.mem = r.mem.data();
        in.top = r.top;
        in.itop = r.itop;
        std::memcpy(in.vf, r.regs.data(), 512);
        std::memcpy(in.vi, r.regs.data() + 528, 32);
        std::vector<std::uint8_t> out;
        std::vector<std::uint32_t> ends;
        g3::End end;
        const bool modelled = g3::run(in, out, ends, end);
        out.resize(ends.empty() ? 0 : ends.back());
        char key[64];
        std::snprintf(key, sizeof key, "%u flags %X cull %X", r.program, word(r.mem, 0x3FF0) & 0xFFu, word(r.mem, 0x3FF8));
        Count &c = counts[key];
        ++c.total;
        if (gpu) check_gpu(in, out, modelled, gc);
        if (!modelled) continue;
        ++c.modelled;
        if (same_for_gs(r.packets, out, c.differ)) {
            ++c.same;
            continue;
        }
        if (described < static_cast<unsigned>(verbose)) {
            ++described;
            std::printf("record %u (%s, itop %u, top %u): %zu bytes from the program, %zu from the model\n", n, key, r.itop, r.top,
                        r.packets.size(), out.size());
            describe(r, out, 24);
        }
    }
    std::printf("%u batches\n", n);
    for (const auto &[k, c] : counts)
        std::printf("  %-28s %6u batches, %6u modelled, %6u identical; qwords differing: tag %u, RGBA %u, STQ %u, XYZF2 %u, XYZ2 %u\n", k.c_str(), c.total,
                    c.modelled, c.same, c.differ[0], c.differ[1], c.differ[2], c.differ[4], c.differ[5]);
    if (gpu) {
        std::printf("GPU path: %u batches, %u drawn, %u draw nothing; declined: %u not modelled, %u bones, %u lights, %u range, %u prim, %u other\n",
                    gc.batches, gc.drawn, gc.nothing, gc.declined[g3::kGpuNotModelled], gc.declined[g3::kGpuBoneCount], gc.declined[g3::kGpuLights],
                    gc.declined[g3::kGpuRange], gc.declined[g3::kGpuPrim], gc.declined[0]);
        std::printf("  %u vertices: worst x/y %.2f (1/16 px), %u over a pixel; worst z %.2g (relative); worst colour %.2f, %u over 2; worst fog %.2f, %u over 2\n",
                    gc.vertices, gc.worst_xy, gc.xy_over, gc.worst_z, gc.worst_colour, gc.colour_over, gc.worst_fog, gc.fog_over);
        std::printf("  %u triangles, %u drawn differently\n", gc.triangles, gc.facing_wrong);
    }
}
