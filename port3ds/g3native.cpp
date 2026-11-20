// Native stand-ins for the game's two RenderWare model programs on VU1, g3_generic (program 042 of the game's 114) and
// g3_skin (064): together over half of the drawing thread's time in 3D scenes (port3ds/README.md). Each MSCNT batch of
// those programs is computed by g3model.cpp from VU1 memory instead of running the compiled microprogram.
//
// Only batches are replaced, never the program's MSCAL 0 start (its init plus the object's first batch), so the VU
// registers the init sets are always the program's own, and any batch the model declines simply runs the program.
//
// "g3 N" in args.txt:
//   0  off (the programs run as before)
//   1  native: the model's GIF packets go to the GIF in place of the program's
//   2  compare: the program runs and draws; the model runs too and its packets are compared byte for byte (log.txt)
//   3  capture: as 2, and the first "g3cap N" batches (one in "g3every K") are saved to sdmc:/p3fes/g3cap.bin,
//      for tools/g3check (the model on a computer against the program's own output)
//   4  GPU: the batch's own vertices go to the PICA renderer, and its vertex shader does the program's transform, skinning
//      and lighting (g3::gpu_batch, pica::draw_g3, port3ds/gs.v.pica). Close to the program's pictures, not exact. Batches
//      it cannot take go to the model as in 1 (and from there to the program).
#include "g3model.hpp"

#include "gs/gif.hpp"
#include "gs/gs.hpp"
#include "gs/queue.hpp"
#include "hw.hpp"
#include "vu_micro.hpp"
#include "ps2/cpu.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gen {
void vu1_CBE8B2DF5F7B4F30(ps2::Vu0Regs &r, std::uint8_t *mem);   // g3_generic (042): the MSCNT resume at 0x3968
void vu1_82FF17C192C71968(ps2::Vu0Regs &r, std::uint8_t *mem);   // g3_skin (064): the MSCNT resume at 0x3C38
}

namespace g3native {

int g_mode = 0;
std::uint32_t g_gpu_mask = 7;   // test aid "g3gpu N": what the GPU path may take: 1 generic, 2 skin, 4 lit batches; 8 draws both facings,
                                // 16 no directional light colours, 32 no normals, 64 no light directions, 128 leave out
std::uint32_t g_capture_left = 0, g_capture_every = 7;

namespace {

struct Stats {
    std::uint64_t batches[2], native[2], declined[2], compared[2], mismatched[2], state_differs[2], gpu[2];
    std::uint64_t gpu_declined[g3::kGpuDeclines];
    std::uint64_t not_modelled[2][4];   // by flags: strip, clipper, guard band clipper, list
};
Stats g_stats{};
std::vector<std::uint8_t> g_model_out, g_real_out;
std::vector<std::uint32_t> g_model_ends;
g3::Gpu g_gpu;
std::FILE *g_capture = nullptr;
std::uint64_t g_seen = 0;
alignas(16) std::uint8_t g_mem_before[16384];

// The packets as the GS reads them: equal except for the w word of STQ, which the GS does not use (the programs leave
// whatever a register held there).
bool same_for_gs(const std::uint8_t *model, std::size_t model_bytes, const std::vector<std::uint8_t> &b) {
    struct View {
        const std::uint8_t *p;
        std::size_t n;
        std::size_t size() const { return n; }
        const std::uint8_t *data() const { return p; }
    } a{model, model_bytes};
    if (a.size() != b.size()) return false;
    std::size_t at = 0;
    while (at + 16 <= a.size()) {
        if (std::memcmp(a.data() + at, b.data() + at, 16) != 0) return false;
        std::uint64_t tag, regs;
        std::memcpy(&tag, a.data() + at, 8);
        std::memcpy(&regs, a.data() + at + 8, 8);
        at += 16;
        const unsigned nloop = tag & 0x7FFFu, nreg = ((tag >> 60) & 15u) ? (tag >> 60) & 15u : 16u;
        if (((tag >> 58) & 3u) != 0) return std::memcmp(a.data() + at, b.data() + at, a.size() - at) == 0;   // not PACKED
        for (unsigned l = 0; l < nloop; ++l)
            for (unsigned k = 0; k < nreg && at + 16 <= a.size(); ++k, at += 16)
                if (std::memcmp(a.data() + at, b.data() + at, ((regs >> (4 * k)) & 15u) == 2 ? 12 : 16) != 0) return false;
    }
    return at == a.size();
}

int program_of(std::uint32_t address) {
    const std::uintptr_t fn = ps2::vu::program_at(address);
    if (fn == reinterpret_cast<std::uintptr_t>(&gen::vu1_CBE8B2DF5F7B4F30)) return 42;
    if (fn == reinterpret_cast<std::uintptr_t>(&gen::vu1_82FF17C192C71968)) return 64;
    return 0;
}

g3::Input input_of(int program, const ps2::Vu0Regs &r, const std::uint8_t *mem) {
    g3::Input in;
    in.program = program;
    in.mem = mem;
    in.top = ps2::vu::data_top();
    in.itop = ps2::vu::data_itop();
    for (int i = 0; i < 32; ++i) std::memcpy(in.vf[i], &r.vf[i], 16);
    std::memcpy(in.vi, r.vi, sizeof in.vi);
    return in;
}

void write_capture(int program, std::uint32_t address, const ps2::Vu0Regs &before) {
    if (!g_capture) {
        g_capture = std::fopen("sdmc:/p3fes/g3cap.bin", "wb");
        if (!g_capture) {
            g_capture_left = 0;
            return;
        }
    }
    const std::uint32_t head[7] = {0x50433347u, static_cast<std::uint32_t>(program), address, ps2::vu::data_top(), ps2::vu::data_itop(),
                                   static_cast<std::uint32_t>(sizeof before), static_cast<std::uint32_t>(g_real_out.size())};
    std::fwrite(head, sizeof head, 1, g_capture);
    std::fwrite(&before, sizeof before, 1, g_capture);
    std::fwrite(g_mem_before, sizeof g_mem_before, 1, g_capture);
    std::fwrite(g_real_out.data(), 1, g_real_out.size(), g_capture);
    if (--g_capture_left == 0) {
        std::fclose(g_capture);
        g_capture = nullptr;
        std::fprintf(stderr, "[g3] capture written\n");
    }
}

void report() {
    std::fprintf(stderr, "[g3] generic %llu batches: %llu native, %llu declined, %llu compared, %llu differ (%llu state); skin %llu: %llu, %llu, %llu, %llu (%llu)\n",
                 static_cast<unsigned long long>(g_stats.batches[0]), static_cast<unsigned long long>(g_stats.native[0]),
                 static_cast<unsigned long long>(g_stats.declined[0]), static_cast<unsigned long long>(g_stats.compared[0]),
                 static_cast<unsigned long long>(g_stats.mismatched[0]), static_cast<unsigned long long>(g_stats.state_differs[0]),
                 static_cast<unsigned long long>(g_stats.batches[1]),
                 static_cast<unsigned long long>(g_stats.native[1]), static_cast<unsigned long long>(g_stats.declined[1]),
                 static_cast<unsigned long long>(g_stats.compared[1]), static_cast<unsigned long long>(g_stats.mismatched[1]),
                 static_cast<unsigned long long>(g_stats.state_differs[1]));
    if (g_mode == 4)
        std::fprintf(stderr, "[g3] GPU: generic %llu, skin %llu; declined: %llu not modelled, %llu bones, %llu lights, %llu range, %llu prim, %llu clipper depth\n",
                     static_cast<unsigned long long>(g_stats.gpu[0]), static_cast<unsigned long long>(g_stats.gpu[1]),
                     static_cast<unsigned long long>(g_stats.gpu_declined[g3::kGpuNotModelled]),
                     static_cast<unsigned long long>(g_stats.gpu_declined[g3::kGpuBoneCount]),
                     static_cast<unsigned long long>(g_stats.gpu_declined[g3::kGpuLights]),
                     static_cast<unsigned long long>(g_stats.gpu_declined[g3::kGpuRange]),
                     static_cast<unsigned long long>(g_stats.gpu_declined[g3::kGpuPrim]),
                     static_cast<unsigned long long>(g_stats.gpu_declined[g3::kGpuClipDepth]));
    if (g_mode == 4)
        for (int k = 0; k < 2; ++k)
            std::fprintf(stderr, "[g3] %s not modelled: strip %llu, clipper %llu, guard band clipper %llu, list %llu\n", k ? "skin" : "generic",
                         static_cast<unsigned long long>(g_stats.not_modelled[k][0]), static_cast<unsigned long long>(g_stats.not_modelled[k][1]),
                         static_cast<unsigned long long>(g_stats.not_modelled[k][2]), static_cast<unsigned long long>(g_stats.not_modelled[k][3]));
}

// Test aid (compare mode): what the batches the model declines are, what the program leaves in the registers and what it
// sends to the GS. Class: 0 strip, 1 clipper, 2 guard band clipper, 3 list, by flags.
std::uint64_t g_probe_class[2][4], g_probe_logged = 0;
void probe_declined(int program, const ps2::Vu0Regs &before, const ps2::Vu0Regs &after) {
    std::uint32_t flags[4];
    std::memcpy(flags, g_mem_before + 0x3FF * 16, 16);
    const int k = program == 64;
    const int c = (flags[0] & 2u) ? ((flags[0] & 8u) ? 1 : 2) : (flags[0] & 4u) ? 3 : 0;
    ++g_probe_class[k][c];
    if (c == 3) {
        // lists: each triangle's facing (area of the GS x, y) against the last word of its third vertex
        static std::uint64_t facing[3][3];
        std::uint64_t tag;
        std::memcpy(&tag, g_real_out.data(), g_real_out.size() >= 8 ? 8 : 0);
        const std::size_t nv = g_real_out.size() >= 16 ? (g_real_out.size() - 16) / 48 : 0;
        for (std::size_t t = 0; t + 2 < nv; t += 3) {
            std::uint32_t v[3][4];
            for (int j = 0; j < 3; ++j) std::memcpy(v[j], g_real_out.data() + 16 + 48 * (t + j) + 32, 16);
            const double area = (double(v[1][0]) - v[0][0]) * (double(v[2][1]) - v[0][1]) - (double(v[2][0]) - v[0][0]) * (double(v[1][1]) - v[0][1]);
            ++facing[area > 0 ? 0 : area < 0 ? 1 : 2][(v[2][3] & 0x8000u) ? 2 : v[2][3] ? 1 : 0];
        }
        static std::uint64_t lists = 0;
        if (++lists % 200 == 0)
            std::fprintf(stderr, "[g3probe] lists %llu: area>0 word 0/set/ADC %llu %llu %llu, area<0 %llu %llu %llu, zero %llu %llu %llu; prim %llX\n",
                         (unsigned long long)lists, (unsigned long long)facing[0][0], (unsigned long long)facing[0][1], (unsigned long long)facing[0][2],
                         (unsigned long long)facing[1][0], (unsigned long long)facing[1][1], (unsigned long long)facing[1][2],
                         (unsigned long long)facing[2][0], (unsigned long long)facing[2][1], (unsigned long long)facing[2][2],
                         (unsigned long long)((tag >> 47) & 0x7FFu));
    }
    if ((g_probe_class[k][0] + g_probe_class[k][1] + g_probe_class[k][2] + g_probe_class[k][3]) % 500 == 0)
        std::fprintf(stderr, "[g3probe] %d declined: strip %llu clipper %llu guard %llu list %llu\n", program,
                     (unsigned long long)g_probe_class[k][0], (unsigned long long)g_probe_class[k][1],
                     (unsigned long long)g_probe_class[k][2], (unsigned long long)g_probe_class[k][3]);
    if (++g_probe_logged > 40) return;
    char line[1024];
    int at = std::snprintf(line, sizeof line, "[g3probe] %d class %d flags %lX itop %lu tpc %lX->%lX:", program, c, (unsigned long)flags[0],
                           (unsigned long)ps2::vu::data_itop(), (unsigned long)before.tpc, (unsigned long)after.tpc);
    for (int i = 1; i < 16 && at < 900; ++i)
        if (before.vi[i] != after.vi[i]) at += std::snprintf(line + at, sizeof line - at, " vi%d %u->%u", i, before.vi[i], after.vi[i]);
    for (int i = 1; i < 32 && at < 900; ++i)
        for (int j = 0; j < 4; ++j)
            if (before.vf[i].w[j] != after.vf[i].w[j]) {
                at += std::snprintf(line + at, sizeof line - at, " vf%d.%c", i, "xyzw"[j]);
                if (i >= 28 && i <= 31) at += std::snprintf(line + at, sizeof line - at, "=%08lX(%08lX)", (unsigned long)after.vf[i].w[j], (unsigned long)before.vf[i].w[j]);
            }
    if (before.i != after.i) at += std::snprintf(line + at, sizeof line - at, " I");
    if (before.q != after.q) at += std::snprintf(line + at, sizeof line - at, " Q");
    std::fprintf(stderr, "%s\n", line);
    // The packets: each GIF tag's NLOOP, PRIM and register count.
    at = std::snprintf(line, sizeof line, "[g3probe]   %zu bytes:", g_real_out.size());
    std::size_t o = 0;
    for (int t = 0; o + 16 <= g_real_out.size() && t < 24 && at < 900; ++t) {
        std::uint64_t tag, regs;
        std::memcpy(&tag, g_real_out.data() + o, 8);
        std::memcpy(&regs, g_real_out.data() + o + 8, 8);
        const unsigned nloop = tag & 0x7FFFu, nreg = ((tag >> 60) & 15u) ? (tag >> 60) & 15u : 16u;
        at += std::snprintf(line + at, sizeof line - at, " [n%u pre%u prim%llX r%u %llX]", nloop, (unsigned)((tag >> 46) & 1u),
                            (unsigned long long)((tag >> 47) & 0x7FFu), nreg, (unsigned long long)regs);
        o += 16 + 16u * nloop * nreg;
    }
    std::fprintf(stderr, "%s\n", line);
}

bool hook(std::uint32_t address) {
    const int program = program_of(address);
    if (!program) return false;
    const int k = program == 64;
    ps2::Vu0Regs &r = ps2::vu::regs(1);
    if (++g_stats.batches[k] % 20000 == 0) report();
    g_model_ends.clear();
    // The GPU path draws on this thread, so not while the renderer runs on its own (split GS).
    if (g_mode == 4 && !ps2::gs::queue::g_split_pica) {
        g3::End end;
        int got = g3::gpu_batch(input_of(program, r, ps2::g_vu1_mem), g_gpu, end);
        if (got == 0) {
            const bool lit = g_gpu.ambient[0] != 0.0f || g_gpu.ambient[1] != 0.0f || g_gpu.ambient[2] != 0.0f || g_gpu.light_colour[0][0] != 0.0f ||
                             g_gpu.light_colour[0][1] != 0.0f || g_gpu.light_colour[0][2] != 0.0f;
            if (!(g_gpu_mask & (k ? 2u : 1u)) || (lit && !(g_gpu_mask & 4u))) got = g3::kGpuNotModelled;
            if (g_gpu_mask & 8u) g_gpu.keep_negative = g_gpu.keep_positive = true;
            if (g_gpu_mask & 16u) std::memset(g_gpu.light_colour, 0, sizeof g_gpu.light_colour);
            if (g_gpu_mask & 32u)
                for (unsigned i = 0; i < g_gpu.n; ++i) g_gpu.v[i].normal[0] = g_gpu.v[i].normal[1] = g_gpu.v[i].normal[2] = 0;
            if (g_gpu_mask & 64u) std::memset(g_gpu.light_dir, 0, sizeof g_gpu.light_dir);
        }
        if (got == 0 && (g_gpu_mask & 128u)) got = -1;   // test aid: leave the batch out
        if (got == 0) {
            // The batch's GIF tag alone (NLOOP 0) goes through the GIF as the program's packet would start: it sets PRIM,
            // and the GIF also does what it does at every tag (Q back to 1, PATH3 data held back while masked let through).
            alignas(16) std::uint8_t tag[16];
            std::memcpy(tag, ps2::g_vu1_mem + 0x3FA * 16, 16);
            const std::uint32_t eop = 0x8000u;
            std::memcpy(tag, &eop, 4);
            ps2::gif::transfer(1, tag, 1);
        }
        if (got <= 0 && (got < 0 || ps2::gs::draw_g3(g_gpu))) {
            g3::leave_state(program, end, r);
            ++g_stats.gpu[k];
            return true;
        }
        ++g_stats.gpu_declined[got > 0 ? got : 0];
        if (got == g3::kGpuNotModelled) {
            const std::uint32_t flags = ps2::g_vu1_mem[0x3FF * 16];
            ++g_stats.not_modelled[k][(flags & 2u) ? ((flags & 8u) ? 1 : 2) : (flags & 4u) ? 3 : 0];
        }
    }
    if (g_mode == 1 || g_mode == 4) {
        g3::End end;
        if (!g3::run(input_of(program, r, ps2::g_vu1_mem), g_model_out, g_model_ends, end)) {
            ++g_stats.declined[k];
            return false;
        }
        std::uint32_t from = 0;
        for (std::uint32_t end : g_model_ends) {
            ps2::gif::transfer(1, g_model_out.data() + from, (end - from) / 16u);
            from = end;
        }
        g3::leave_state(program, end, r);
        ++g_stats.native[k];
        return true;
    }
    // Compare (and capture): the model on the memory as the program finds it, then the program itself.
    const ps2::Vu0Regs before = r;
    std::memcpy(g_mem_before, ps2::g_vu1_mem, sizeof g_mem_before);
    g3::End end;
    const bool modelled = g3::run(input_of(program, before, g_mem_before), g_model_out, g_model_ends, end);
    g_real_out.clear();
    ps2::vu::g_kick_capture = &g_real_out;
    ps2::vu::run_program(address);
    ps2::vu::g_kick_capture = nullptr;
    if (!modelled) {
        ++g_stats.declined[k];
        probe_declined(program, before, r);
    } else {
        ++g_stats.compared[k];
        ps2::Vu0Regs expect = before;
        g3::leave_state(program, end, expect);
        bool same_z = true;
        for (int i = 28; i <= 30; ++i) same_z = same_z && expect.vf[i].w[2] == r.vf[i].w[2];
        if (expect.vi[13] != r.vi[13] || expect.vi[14] != r.vi[14] || expect.vi[15] != r.vi[15] || expect.tpc != r.tpc || !same_z) {
            if (++g_stats.state_differs[k] <= 10)
                std::fprintf(stderr, "[g3] %d leaves vi13 %u vi14 %u vi15 %u tpc %lX z %lX, model %u %u %u %lX %lX\n", program, r.vi[13], r.vi[14],
                             r.vi[15], (unsigned long)r.tpc, (unsigned long)r.vf[30].w[2], expect.vi[13], expect.vi[14], expect.vi[15],
                             (unsigned long)expect.tpc, (unsigned long)expect.vf[30].w[2]);
        }
        const std::size_t model_bytes = g_model_ends.empty() ? 0 : g_model_ends.back();
        if (!same_for_gs(g_model_out.data(), model_bytes, g_real_out)) {
            if (++g_stats.mismatched[k] <= 40) {
                std::size_t at = 0;
                while (at < model_bytes && at < g_real_out.size() && g_model_out[at] == g_real_out[at]) ++at;
                at &= ~std::size_t{15};
                std::uint32_t m[4] = {}, p[4] = {};
                if (at + 16 <= model_bytes) std::memcpy(m, g_model_out.data() + at, 16);
                if (at + 16 <= g_real_out.size()) std::memcpy(p, g_real_out.data() + at, 16);
                std::uint32_t flags[4];
                std::memcpy(flags, g_mem_before + 0x3FF * 16, 16);
                std::fprintf(stderr, "[g3] %d differs: %zu/%zu bytes, qword %zu: model %08lX %08lX %08lX %08lX, program %08lX %08lX %08lX %08lX; flags %lX %lX %lX, itop %lu\n",
                             program, model_bytes, g_real_out.size(), at / 16, (unsigned long)m[0], (unsigned long)m[1],
                             (unsigned long)m[2], (unsigned long)m[3], (unsigned long)p[0], (unsigned long)p[1], (unsigned long)p[2],
                             (unsigned long)p[3], (unsigned long)flags[0], (unsigned long)flags[1], (unsigned long)flags[2],
                             (unsigned long)ps2::vu::data_itop());
            }
        }
    }
    if (g_mode == 3 && g_capture_left && ++g_seen % g_capture_every == 0) write_capture(program, address, before);
    return true;
}

}

void start() {
    if (g_mode) ps2::vu::g_native_hook = hook;
}

}
