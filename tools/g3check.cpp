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
#include <chrono>
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
    float sx[512], sy[512];
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
        const float sz = (clip[2] * q + g.offset[2]) * (g.xyzf ? 1.0f : 16.0f);
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
        // XYZF2 keeps 16 bits of x and y: a vertex far off the picture (a clipper batch's, which the clipper cuts) wraps.
        auto wrapped = [](double d) { d = std::fmod(std::fabs(d), 65536.0); return std::min(d, 65536.0 - d); };
        const double dx = wrapped(sx[k] * 16.0 - X), dy = wrapped(sy[k] * 16.0 - Y);
        const double z = g.xyzf ? (Z >> 4) & 0xFFFFFFu : Z;
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

// Test aid (-clipstrip): the clipper's vertices that keep their triangle (the same three vertices as in the batch) against
// what the plain strip would send for them: tells whether the clipper culls and lights as the strip does.
struct ClipStrip { unsigned table[3][2][2][2] = {}; unsigned shown = 0, batches = 0, identical = 0, compared = 0, adc_differs = 0, other_differs = 0; };
void clip_vs_strip(const Record &r, ClipStrip &cs) {
    std::vector<std::uint8_t> mem = r.mem;
    mem[0x3FF0] &= ~2u;
    g3::Input in{};
    in.program = 42;
    in.mem = mem.data();
    in.top = r.top;
    in.itop = r.itop;
    in.vf = r.regs.data();
    std::vector<std::uint8_t> out;
    std::vector<std::uint32_t> ends;
    g3::End end;
    if (!g3::run(in, out, ends, end) || r.packets.empty()) return;
    out.resize(ends.back());
    ++cs.batches;
    if (out == r.packets) ++cs.identical;
    // model vertices: 3 qwords each after the 16-byte tag; XYZF2 is the third
    const std::size_t mn = (out.size() - 16) / 48;
    auto xyz = [](const std::vector<std::uint8_t> &v, std::size_t at) { std::uint64_t a; std::uint32_t b; std::memcpy(&a, v.data() + at + 32, 8); std::memcpy(&b, v.data() + at + 40, 4); return std::pair{a, b}; };
    std::size_t at = 0;
    while (at + 16 <= r.packets.size()) {
        const unsigned nl = word(r.packets, at) & 0x7FFFu;
        const std::size_t first = at + 16;
        for (unsigned j = 2; j < nl; ++j) {
            const std::size_t pv = first + 48u * j;
            for (std::size_t k = 2; k < mn; ++k) {
                const std::size_t mv = 16 + 48u * k;
                if (xyz(r.packets, pv) != xyz(out, mv) || xyz(r.packets, pv - 48) != xyz(out, mv - 48) || xyz(r.packets, pv - 96) != xyz(out, mv - 96)) continue;
                ++cs.compared;
                if (((word(r.packets, pv + 44) ^ word(out, mv + 44)) >> 15) & 1u) ++cs.adc_differs;
                const bool stq = std::memcmp(r.packets.data() + pv, out.data() + mv, 12) != 0, rgba = std::memcmp(r.packets.data() + pv + 16, out.data() + mv + 16, 16) != 0;
                if (stq || rgba) ++cs.other_differs;
                const bool adc = ((word(r.packets, pv + 44) ^ word(out, mv + 44)) >> 15) & 1u;
                {
                    auto sxy = [&](std::size_t v, float &x, float &y) { x = (word(out, v + 32) & 0xFFFFu) / 16.0f; y = (word(out, v + 36) & 0xFFFFu) / 16.0f; };
                    float x0, y0, x1, y1, x2, y2;
                    const std::size_t va = (k & 1u) ? mv - 48 : mv - 96, vb = (k & 1u) ? mv - 96 : mv - 48;
                    sxy(va, x0, y0), sxy(vb, x1, y1), sxy(mv, x2, y2);
                    const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
                    const int sign = std::fabs(area) < 0.5f ? 0 : area > 0 ? 1 : 2;
                    ++cs.table[sign][(word(r.packets, pv + 44) >> 15) & 1u][(word(out, mv + 44) >> 15) & 1u][k & 1u];
                }
                if ((adc || stq || rgba) && cs.shown < 12) {
                    ++cs.shown;
                    std::printf("  strip vertex %zu (out %u): adc program %u model %u; stq %s rgba %s; program", k, j, (word(r.packets, pv + 44) >> 15) & 1u, (word(out, mv + 44) >> 15) & 1u, stq ? "differs" : "same", rgba ? "differs" : "same");
                    for (int q = 0; q < 8; ++q) std::printf(" %08X", word(r.packets, pv + 4 * q));
                    std::printf(" model");
                    for (int q = 0; q < 8; ++q) std::printf(" %08X", word(out, mv + 4 * q));
                    std::printf("\n");
                }
                break;
            }
        }
        at = first + 48u * nl;
    }
}

// Test aid (-liststrip): a triangle-list batch's vertices against the strip model's for the same vertices (flag 4 cleared).
struct ListStrip { unsigned stq_ulp = 0, batches = 0, vertices = 0, stq = 0, rgba = 0, xyz = 0, word3[4] = {}; };
void list_vs_strip(const Record &r, ListStrip &ls) {
    std::vector<std::uint8_t> mem = r.mem;
    mem[0x3FF0] &= ~4u;
    g3::Input in{};
    in.program = static_cast<int>(r.program);
    in.mem = mem.data();
    in.top = r.top;
    in.itop = r.itop;
    in.vf = r.regs.data();
    std::vector<std::uint8_t> out;
    std::vector<std::uint32_t> ends;
    g3::End end;
    if (!g3::run(in, out, ends, end) || r.packets.size() != ends.back()) return;
    ++ls.batches;
    const std::size_t n = (r.packets.size() - 16) / 48;
    for (std::size_t k = 0; k < n; ++k) {
        const std::size_t at = 16 + 48 * k;
        ++ls.vertices;
        if (std::memcmp(r.packets.data() + at, out.data() + at, 12)) ++ls.stq;
        for (int q = 0; q < 3; ++q) {
            const std::int64_t d = std::llabs(static_cast<std::int64_t>(static_cast<std::int32_t>(word(r.packets, at + 4 * q))) - static_cast<std::int32_t>(word(out, at + 4 * q)));
            if (d > 4 && ls.stq_ulp <= 4) std::printf("  STQ word %d: program %08X model %08X\n", q, word(r.packets, at + 4 * q), word(out, at + 4 * q));
            if (d > ls.stq_ulp) ls.stq_ulp = static_cast<unsigned>(std::min<std::int64_t>(d, 0x7FFFFFFF));
        }
        if (std::memcmp(r.packets.data() + at + 16, out.data() + at + 16, 16)) ++ls.rgba;
        if (std::memcmp(r.packets.data() + at + 32, out.data() + at + 32, 12)) ++ls.xyz;
        const std::uint32_t w = word(r.packets, at + 44);
        ++ls.word3[k % 3 == 2 ? (w & 0x8000u ? 3 : w ? 2 : 1) : (w ? 2 : 0)];
        if (ls.vertices <= 6) {
            std::printf("  list vertex %zu: program", k);
            for (int q = 0; q < 12; ++q) std::printf(" %08X", word(r.packets, at + 4 * q));
            std::printf("\n                 model  ");
            for (int q = 0; q < 12; ++q) std::printf(" %08X", word(out, at + 4 * q));
            std::printf("\n");
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: g3check g3cap.bin [-v N]\n");
        return 2;
    }
    int verbose = 0;
    bool gpu = false, clipstrip = false, liststrip = false;
    int bench = 0;
    ClipStrip cs;
    ListStrip ls;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-v") && i + 1 < argc) verbose = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "-gpu")) gpu = true;
        if (!std::strcmp(argv[i], "-clipstrip")) clipstrip = true;
        if (!std::strcmp(argv[i], "-liststrip")) liststrip = true;
        if (!std::strcmp(argv[i], "-bench") && i + 1 < argc) bench = std::atoi(argv[i + 1]);
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
        in.vf = r.regs.data();
        std::vector<std::uint8_t> out;
        std::vector<std::uint32_t> ends;
        g3::End end;
        const bool modelled = g3::run(in, out, ends, end);
        out.resize(ends.empty() ? 0 : ends.back());
        char key[64];
        std::snprintf(key, sizeof key, "%u flags %X cull %X", r.program, word(r.mem, 0x3FF0) & 0xFFu, word(r.mem, 0x3FF8));
        Count &c = counts[key];
        ++c.total;
        if (gpu && r.program == 42 && (r.mem[0x3FF0] & 2u)) {
            // A clipper batch: its kept vertices are the plain strip's (-clipstrip), so the GPU path is checked against that.
            std::vector<std::uint8_t> mem = r.mem, strip;
            mem[0x3FF0] &= ~2u;
            g3::Input sin = in;
            sin.mem = mem.data();
            std::vector<std::uint32_t> sends;
            g3::End send;
            const bool smodelled = g3::run(sin, strip, sends, send);
            strip.resize(sends.empty() ? 0 : sends.back());
            check_gpu(in, strip, smodelled, gc);
        } else if (gpu && (r.mem[0x3FF0] & 4u)) {
            // A list: the program's own vertices in the order gpu_batch turns them into a strip (g3::list_to_strip).
            std::vector<std::uint8_t> strip(r.packets.begin(), r.packets.begin() + std::min<std::size_t>(16, r.packets.size()));
            const std::size_t nv = r.packets.size() >= 16 ? (r.packets.size() - 16) / 48 : 0;
            auto put = [&](std::size_t k) { strip.insert(strip.end(), r.packets.begin() + 16 + 48 * k, r.packets.begin() + 64 + 48 * k); };
            for (std::size_t t = 0; t < nv / 3; ++t) {
                if (t) put(3 * t - 1), put(3 * t), put(3 * t);
                put(3 * t), put(3 * t + 1), put(3 * t + 2);
            }
            check_gpu(in, strip, !r.packets.empty(), gc);
        } else if (gpu) {
            check_gpu(in, out, modelled, gc);
        }
        if (liststrip && (r.mem[0x3FF0] & 4u)) list_vs_strip(r, ls);
        if (clipstrip && r.program == 42 && (r.mem[0x3FF0] & 2u)) clip_vs_strip(r, cs);
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
    if (bench) {
        // Test aid (-bench N): gpu_batch's time per batch and per vertex, by program, over every record N times.
        std::rewind(f);
        std::vector<Record> all;
        Record b;
        while (read_record(f, b)) all.push_back(b);
        static g3::Gpu g;
        double ns[2] = {}, verts[2] = {}, count[2] = {};
        for (int pass = 0; pass < bench; ++pass)
            for (const Record &rec : all) {
                g3::Input in{};
                in.program = static_cast<int>(rec.program);
                in.mem = rec.mem.data();
                in.top = rec.top;
                in.itop = rec.itop;
                in.vf = rec.regs.data();
                g3::End e;
                const auto t0 = std::chrono::steady_clock::now();
                const int got = g3::gpu_batch(in, g, e);
                const auto t1 = std::chrono::steady_clock::now();
                if (got != 0) continue;
                const int k = rec.program == 64;
                ns[k] += std::chrono::duration<double, std::nano>(t1 - t0).count();
                verts[k] += g.n;
                ++count[k];
            }
        for (int k = 0; k < 2; ++k)
            if (count[k]) std::printf("bench %s: %.0f ns per batch, %.1f ns per vertex (%.0f vertices a batch)\n", k ? "skin" : "generic", ns[k] / count[k], ns[k] / verts[k], verts[k] / count[k]);
    }
    std::printf("%u batches\n", n);
    for (const auto &[k, c] : counts)
        std::printf("  %-28s %6u batches, %6u modelled, %6u identical; qwords differing: tag %u, RGBA %u, STQ %u, XYZF2 %u, XYZ2 %u\n", k.c_str(), c.total,
                    c.modelled, c.same, c.differ[0], c.differ[1], c.differ[2], c.differ[4], c.differ[5]);
    if (liststrip)
        std::printf("list batches against the strip model: %u batches, %u vertices; differ: STQ %u (at most %u units in the last place), RGBA %u, XYZ %u; last word: 0 on first two %u, "
                    "set on first two %u, third: 0 %u, set without ADC %u, ADC %u\n", ls.batches, ls.vertices, ls.stq, ls.stq_ulp, ls.rgba, ls.xyz, ls.word3[0],
                    ls.word3[2], ls.word3[1], ls.word3[2], ls.word3[3]);
    if (clipstrip)
        for (int a = 0; a < 3; ++a)
            for (int p = 0; p < 2; ++p)
                for (int m = 0; m < 2; ++m)
                    for (int o = 0; o < 2; ++o)
                        if (cs.table[a][p][m][o]) std::printf("  area %s, ADC program %d model %d, %s vertex: %u\n", a == 0 ? "zero" : a == 1 ? "above 0" : "below 0", p, m, o ? "odd" : "even", cs.table[a][p][m][o]);
    if (clipstrip)
        std::printf("clipper batches against the strip model: %u batches, %u identical; vertices with the same triangle: %u, ADC differs %u, "
                    "colour/STQ differ %u\n", cs.batches, cs.identical, cs.compared, cs.adc_differs, cs.other_differs);
    if (gpu) {
        std::printf("GPU path: %u batches, %u drawn, %u draw nothing; declined: %u not modelled, %u bones, %u lights, %u range, %u prim, %u clipper depth, %u other\n",
                    gc.batches, gc.drawn, gc.nothing, gc.declined[g3::kGpuNotModelled], gc.declined[g3::kGpuBoneCount], gc.declined[g3::kGpuLights],
                    gc.declined[g3::kGpuRange], gc.declined[g3::kGpuPrim], gc.declined[g3::kGpuClipDepth], gc.declined[0]);
        std::printf("  %u vertices: worst x/y %.2f (1/16 px), %u over a pixel; worst z %.2g (relative); worst colour %.2f, %u over 2; worst fog %.2f, %u over 2\n",
                    gc.vertices, gc.worst_xy, gc.xy_over, gc.worst_z, gc.worst_colour, gc.colour_over, gc.worst_fog, gc.fog_over);
        std::printf("  %u triangles, %u drawn differently\n", gc.triangles, gc.facing_wrong);
    }
}
