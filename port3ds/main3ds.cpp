// New 3DS host for the recompiled game: boots the runtime, feeds it the disc and the controls, shows the picture.
//
// Files on the SD card (everything is made from your own disc and stays on your SD card):
//   sdmc:/p3fes/SLUS_216.21     the game's boot program
//   sdmc:/p3fes/disc.iso        the disc image, or disc.00, disc.01, ... parts for FAT32 cards (see port3ds/README.md)
//   sdmc:/p3fes/args.txt        optional test settings, one per line: "frames N", "press 300:start", "every N", "shots N",
//                               "soft 1" (software renderer), "stats 1" (GPU counters in the log),
//                               "load name.state" (start from a saved state in this folder, or from the beginning if it is missing),
//                               "interp 1" (vector unit interpreter), "dump N" (log every GPU batch of game frame N),
//                               "record sdmc:/p3fes/vu1" (save vector programs that are not compiled yet),
//                               "vuthread 0" (run the vector programs and drawing on the same core as the game),
//                               "splitgs 1" or "splitgs 2" (run drawing on core 0 or core 1),
//                               "draw N" (draw one picture in N; the default 0 draws what keeps up with the game),
//                               "uncapped 1" (let a light scene run faster than the PS2, to measure how much room is left),
//                               "hide A-B" (leave out batches A up to B of each picture, to find which one draws something),
//                               "pmu 0 8" (hardware counters of the game's core in perf.txt, by event number, see below),
//                               "link 0" (no wireless test link), "stereo N" (depth of the 3D picture, 0 to 10),
//                               "movies 0" (the game's own movies, not the 400x240 copies in sdmc:/p3fes/movies),
//                               "saveat N" (test aid: save a state to slot 1 at game frame N, as the bottom screen's Save does),
//                               "g3 N" (the model programs g3_generic and g3_skin: 1 native, 2 compared with the programs, 3 also
//                               captured, "g3cap N" batches, one in "g3every K"; 4 on the GPU's vertex shader; see g3native.cpp)
//                               "g3cache N" (converted g3 vertices kept across pictures: 0 off, 1 on, the default, 2 on with counts in the log; g3cache.cpp)
//                               "g3skin 0" (the vertex cache leaves skinned batches out; they are in by default; g3cache.hpp)
//                               "g3index N" (test: 1 joins unskinned batches from the cache by index, 2 skinned too; both hang the console; g3cache.hpp)
//                               "debug 256" (checks every g3 strip before drawing: buffer, indices, values, bone slots, uniforms)
//                               "debug 512" (test: flush the whole vertex cache arena before every submit)
//                               "debug 1024" (logs each decision of the automatic picture skipping)
//   sdmc:/p3fes/slot1.state     states saved from the bottom screen (slot1 to slot3), packed
//   sdmc:/p3fes/next.txt        settings for the next start only: the bottom screen's Load writes "load slotN.state" here
//   sdmc:/p3fes/log.txt         everything the runtime prints
//   sdmc:/p3fes/perf.txt        one line of timing every 30 game frames
#include "audio3ds.hpp"
#include "g3cache.hpp"
#include "movie3ds.hpp"
#include "pica.hpp"
#include "remote3ds.hpp"
#include "ipu/ipu.hpp"
#include "ps2/disc.hpp"
#include "ps2/generated.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <3ds.h>

#include <cstdint>
#include <malloc.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace ps2::gs {
extern std::uint32_t g_max_vu_lag;   // runtime/src/gs/gs.cpp
}

namespace gen { extern bool g_lossy; }   // native3ds.cpp
namespace g3native {   // g3native.cpp
extern int g_mode;
extern std::uint32_t g_capture_left, g_capture_every, g_gpu_mask;
void start(bool gpu_available);
}

extern "C" {
// Program + these + the PS2 RAM must fit the 124 MB a New 3DS application gets.
u32 __ctru_heap_size = 28u << 20;
u32 __ctru_linear_heap_size = 15u << 20;   // 2 MB of it is the g3 vertex arena
}

static_assert(ps2::kRamAddress3ds == OS_HEAP_AREA_BEGIN + (28u << 20), "the PS2 RAM follows the heap with no gap");

void register_generated_code();
void register_generated_vu();
bool ps2_dump_ee_seen(const char *path);   // from eerecomp; false unless the game code was made with --record-seen

namespace {

void log_memory() {
    s64 used = 0, limit = 0, commit = 0;
    svcGetSystemInfo(&used, 0, 1);
    Handle handle = 0;
    ResourceLimitType type = RESLIMIT_COMMIT;
    if (R_SUCCEEDED(svcGetResourceLimit(&handle, CUR_PROCESS_HANDLE))) {
        svcGetResourceLimitLimitValues(&limit, handle, &type, 1);
        svcGetResourceLimitCurrentValues(&commit, handle, &type, 1);
        svcCloseHandle(handle);
    }
    std::fprintf(stderr, "[3ds] memory: region %lu KB, free %lu KB, used %lld KB, commit %lld of %lld KB\n",
                 static_cast<unsigned long>(osGetMemRegionSize(MEMREGION_APPLICATION) >> 10),
                 static_cast<unsigned long>(osGetMemRegionFree(MEMREGION_APPLICATION) >> 10), used >> 10, commit >> 10,
                 limit >> 10);
}

void log_block(u32 address) {
    MemInfo info{};
    PageInfo page{};
    const Result rc = svcQueryMemory(&info, &page, address);
    std::fprintf(stderr, "[3ds] block at 0x%08lX: result 0x%08lX, base 0x%08lX, size 0x%08lX, state %lu, perm %lu\n",
                 static_cast<unsigned long>(address), static_cast<unsigned long>(rc), static_cast<unsigned long>(info.base_addr),
                 static_cast<unsigned long>(info.size), static_cast<unsigned long>(info.state),
                 static_cast<unsigned long>(info.perm));
}

// After a refused request: what the console would have given, at the place wanted and further up.
void log_what_fits() {
    log_block(OS_HEAP_AREA_BEGIN);
    log_block(ps2::kRamAddress3ds);
    log_block(0x0B000000u);
    for (const u32 address : {static_cast<u32>(ps2::kRamAddress3ds), static_cast<u32>(0x0B000000u)}) {
        for (const u32 mb : {1u, 4u, 8u, 16u, 24u, 28u, 30u, 31u, 32u}) {
            u32 mapped = 0;
            const Result rc = svcControlMemory(&mapped, address, 0, mb << 20, MEMOP_ALLOC,
                                               static_cast<MemPerm>(MEMPERM_READ | MEMPERM_WRITE));
            std::fprintf(stderr, "[3ds] %lu MB at 0x%08lX: result 0x%08lX\n", static_cast<unsigned long>(mb),
                         static_cast<unsigned long>(address), static_cast<unsigned long>(rc));
            if (R_SUCCEEDED(rc)) svcControlMemory(&mapped, address, 0, mb << 20, MEMOP_FREE, MEMPERM_DONTCARE);
        }
    }
}

constexpr const char *kFolder = "/p3fes";
constexpr std::size_t kEmulationStack = 64u << 10;   // only until emulate() moves to kGameStackTop
// The game thread's real stack is mapped together with the PS2 RAM, after it, so translated calls can test their depth
// against a constant (ps2::stack_ok). Calls stop at the floor; the 1 MB under it holds the frames of the last calls.
constexpr u32 kGameStackTop = 0x0BD80000u;
static_assert(ps2::kStackFloor3ds >= ps2::kRamAddress3ds + ps2::kRamSize + (1u << 20) - 0x1000u, "room under the floor");
static_assert(kGameStackTop - ps2::kStackFloor3ds == 512u << 10, "512 KB of nested calls");
constexpr std::size_t kVuStack = 512u << 10;

volatile bool g_quit = false;
volatile bool g_finished = false;
std::uint32_t g_show_every = 2;
std::uint32_t g_shots_every = 0;
bool g_software = false;
std::string g_state_path;
std::uint64_t g_dump_frame = 0;
std::uint32_t g_skip_attr = 0, g_debug = 0, g_hide_from = 0, g_hide_to = 0;
bool g_stats = false;
bool g_vu_thread = true;
bool g_split_gs = false;
int g_gs_core = 0;
std::uint32_t g_draw_every = 0;
bool g_link = true;
s32 g_priority = 0x30;
bool g_iop_thread = true;
bool g_hold_speed = true;
enum class WorkerState { Waiting, Ready, Failed };
std::atomic<WorkerState> g_worker_start{WorkerState::Waiting};
std::atomic<WorkerState> g_vu_ready{WorkerState::Waiting}, g_gs_ready{WorkerState::Waiting};
Thread g_vu_handle = nullptr;
Thread g_gs_handle = nullptr;
std::atomic<bool> g_vu_stopped{false};
extern "C" volatile int g_fes_trace;
extern "C" void FesOverlapDescribe(char *out, std::size_t size);
std::FILE *g_perf = nullptr;
u64 g_start_ticks = 0;
u64 g_last_ticks = 0;
std::uint64_t g_last_prims = 0;
u64 g_last_frame_ticks = 0, g_peak_frame_ticks = 0;
unsigned g_frame_samples = 0, g_frames_over_budget = 0;

struct DiscPart {
    Handle file = 0;
    std::uint64_t start = 0, size = 0;
};
std::vector<DiscPart> g_parts;

std::size_t read_disc(std::uint64_t offset, void *out, std::size_t size) {
    auto *to = static_cast<std::uint8_t *>(out);
    std::size_t done = 0;
    for (const DiscPart &p : g_parts) {
        if (done == size) break;
        const std::uint64_t at = offset + done;
        if (at < p.start || at >= p.start + p.size) continue;
        const std::uint64_t room = p.start + p.size - at;
        const u32 want = static_cast<u32>(size - done < room ? size - done : room);
        u32 got = 0;
        if (R_FAILED(FSFILE_Read(p.file, &got, at - p.start, to + done, want)) || got == 0) break;
        done += got;
    }
    movie::disc_read(offset, to, done);
    return done;
}

bool open_part(FS_Archive sd, const std::string &path, std::uint64_t &total) {
    Handle file = 0;
    if (R_FAILED(FSUSER_OpenFile(&file, sd, fsMakePath(PATH_ASCII, path.c_str()), FS_OPEN_READ, 0))) return false;
    u64 size = 0;
    FSFILE_GetSize(file, &size);
    g_parts.push_back({file, total, size});
    total += size;
    return true;
}

bool open_disc() {
    FS_Archive sd = 0;
    if (R_FAILED(FSUSER_OpenArchive(&sd, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) return false;
    std::uint64_t total = 0;
    const std::string folder = kFolder;
    if (!open_part(sd, folder + "/disc.iso", total)) {
        for (int i = 0; i < 100; ++i) {
            char name[32];
            std::snprintf(name, sizeof name, "/disc.%02d", i);
            if (!open_part(sd, folder + name, total)) break;
        }
    }
    if (g_parts.empty()) return false;
    return ps2::disc::open("sdmc disc", total, read_disc);
}

void show_frame() {
    static std::vector<std::uint32_t> rgba;
    std::uint32_t width = 0, height = 0;
    if (!ps2::display_frame(rgba, width, height) || !width || !height) return;
    // The top screen's memory runs in columns, bottom to top: 240 bytes-times-3 per column of the 400 wide picture.
    u8 *screen = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
    const std::uint32_t *src = rgba.data();
    for (std::uint32_t x = 0; x < 400; ++x) {
        const std::uint32_t sx = x * width / 400;
        u8 *column = screen + x * 240 * 3;
        for (std::uint32_t y = 0; y < 240; ++y) {
            const std::uint32_t c = src[(239 - y) * height / 240 * width + sx];
            column[y * 3 + 0] = static_cast<u8>(c >> 16);
            column[y * 3 + 1] = static_cast<u8>(c >> 8);
            column[y * 3 + 2] = static_cast<u8>(c);
        }
    }
    gfxFlushBuffers();
    gfxSwapBuffers();
}

std::uint16_t read_buttons(u32 held) {
    // Same bit order as the PS2 pad in host/window.cpp. A confirms and B cancels, as the game's own Cross and Circle do.
    static const struct { u32 key; std::uint16_t button; } kMap[] = {
        {KEY_SELECT, 1u << 0}, {KEY_START, 1u << 3}, {KEY_DUP, 1u << 4}, {KEY_DRIGHT, 1u << 5}, {KEY_DDOWN, 1u << 6}, {KEY_DLEFT, 1u << 7},
        {KEY_ZL, 1u << 8},     {KEY_ZR, 1u << 9},    {KEY_L, 1u << 10}, {KEY_R, 1u << 11},      {KEY_X, 1u << 12},    {KEY_B, 1u << 13},
        {KEY_A, 1u << 14},     {KEY_Y, 1u << 15},
    };
    std::uint16_t pressed = 0;
    for (const auto &m : kMap)
        if (held & m.key) pressed |= m.button;
    return pressed;
}

std::uint8_t stick_axis(int value) {
    const int v = 128 + value * 127 / 156;
    return static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// "pmu A B": ARM11 events, for example 0 instruction cache misses, 1 cycles stalled for instructions, 2 cycles stalled on data,
// 7 branches mispredicted, 8 instructions, 11 and 13 data cache read and write misses, 16 main TLB misses, 18 cycles the load
// store unit was full. perf.txt then gives the game core's busy cycles per 30 frames, and how long the I/O processor kept it waiting.
}

namespace ps2 {
void iop_timing(bool on);
u64 iop_waited_ticks();
u64 iop_worked_ticks();
}

namespace {

int g_pmu_a = -1, g_pmu_b = -1;
u64 g_pmu_last[5];
#ifdef PS2_PROF_ALL
u64 g_prof_enters[40];   // how often each activity was switched to, so the cost of the switches themselves can be taken off
#endif

u64 pmu_read(PerfCounterRegister r) {
    u64 v = 0;
    svcControlPerformanceCounter(&v, PERFCOUNTEROP_GET_VALUE, r, 0);
    return v;
}

void pmu_start() {
    if (g_pmu_a < 0) return;
    u64 v = 0;
    Result rc = svcControlPerformanceCounter(&v, PERFCOUNTEROP_ENABLE, 0, 0);
    std::fprintf(stderr, "[pmu] enable %08lX\n", static_cast<unsigned long>(rc));
    rc = svcControlPerformanceCounter(&v, PERFCOUNTEROP_SET_VIRTUAL_COUNTER_ENABLED, 1, 0);
    rc |= svcControlPerformanceCounter(&v, PERFCOUNTEROP_SET_EVENT, PERFCOUNTERREG_CORE_COUNT_REG_0, g_pmu_a);
    rc |= svcControlPerformanceCounter(&v, PERFCOUNTEROP_SET_EVENT, PERFCOUNTERREG_CORE_COUNT_REG_1, g_pmu_b);
    rc |= svcControlPerformanceCounter(&v, PERFCOUNTEROP_RESET, 0xFFFFFFFFu, 0xFFFFFFFFu);
    std::fprintf(stderr, "[pmu] events %d %d, setup %08lX\n", g_pmu_a, g_pmu_b, static_cast<unsigned long>(rc));
#ifdef PS2_PROF_ALL
    {
        const u64 c0 = pmu_read(PERFCOUNTERREG_CORE_CYCLE_COUNTER), a0 = pmu_read(PERFCOUNTERREG_CORE_COUNT_REG_0), b0 = pmu_read(PERFCOUNTERREG_CORE_COUNT_REG_1);
        for (int i = 0; i < 4096; ++i) ps2::prof::switch_events(ps2::prof::g_events_at);
        const u64 c1 = pmu_read(PERFCOUNTERREG_CORE_CYCLE_COUNTER), a1 = pmu_read(PERFCOUNTERREG_CORE_COUNT_REG_0), b1 = pmu_read(PERFCOUNTERREG_CORE_COUNT_REG_1);
        std::fprintf(stderr, "[pmu] 4096 switches cost cycles %llu a %llu b %llu\n", static_cast<unsigned long long>(c1 - c0),
                     static_cast<unsigned long long>(a1 - a0), static_cast<unsigned long long>(b1 - b0));
    }
#endif
    ps2::iop_timing(true);
}

void pmu_line() {
    const u64 now[5] = {pmu_read(PERFCOUNTERREG_CORE_CYCLE_COUNTER), pmu_read(PERFCOUNTERREG_CORE_COUNT_REG_0),
                        pmu_read(PERFCOUNTERREG_CORE_COUNT_REG_1), ps2::iop_waited_ticks(), ps2::iop_worked_ticks()};
    std::fprintf(g_perf, " cycles %llu pmu%d %llu pmu%d %llu iop_wait_ms %llu iop_work_ms %llu", static_cast<unsigned long long>(now[0] - g_pmu_last[0]),
                 g_pmu_a, static_cast<unsigned long long>(now[1] - g_pmu_last[1]), g_pmu_b, static_cast<unsigned long long>(now[2] - g_pmu_last[2]),
                 static_cast<unsigned long long>((now[3] - g_pmu_last[3]) / static_cast<u64>(CPU_TICKS_PER_MSEC)),
                 static_cast<unsigned long long>((now[4] - g_pmu_last[4]) / static_cast<u64>(CPU_TICKS_PER_MSEC)));
    std::memcpy(g_pmu_last, now, sizeof now);
#ifdef PS2_PROF_ALL
    static constexpr const char *kNames[] = {"vu", "gif", "upload", "draw", "iop", "spu", "ipu", "vif", "dma", "events", "kernel",
                                             "present", "wait", "full", "game", "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13"};
    constexpr int kBuckets = ps2::prof::Count + 1 + ps2::prof::kExtra;
    static_assert(std::size(kNames) == kBuckets);
    static u64 printed[kBuckets][2];
    if (ps2::prof::on_game_thread()) ps2::prof::switch_events(ps2::prof::g_events_at);
    for (int k = 0; k < kBuckets; ++k) {
        const u64 a = ps2::prof::g_events[k][0] - printed[k][0], b = ps2::prof::g_events[k][1] - printed[k][1];
        if (a || b) std::fprintf(g_perf, " ev_%s %llu %llu", kNames[k], static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
        if (g_prof_enters[k]) std::fprintf(g_perf, " sw_%s %llu", kNames[k], static_cast<unsigned long long>(g_prof_enters[k]));
        g_prof_enters[k] = 0;
        printed[k][0] = ps2::prof::g_events[k][0];
        printed[k][1] = ps2::prof::g_events[k][1];
    }
#endif
}

}

#ifdef PS2_PROF_ALL
namespace ps2::prof {
std::uint64_t g_events[Count + 1 + kExtra][2];
int g_events_at = Count;
void *g_game_thread = nullptr;

void switch_events(int to) {
    static u64 last[2];
    if (g_pmu_a >= 0) {
        const u64 a = pmu_read(PERFCOUNTERREG_CORE_COUNT_REG_0), b = pmu_read(PERFCOUNTERREG_CORE_COUNT_REG_1);
        g_events[g_events_at][0] += a - last[0];
        g_events[g_events_at][1] += b - last[1];
        last[0] = a;
        last[1] = b;
    }
    g_events_at = to;
    ++g_prof_enters[to];
}
}
#endif

#ifdef PS2_HOT_SAMPLE
// SAMPLE=1: every 250 microseconds a thread on another core notes which copy from tools/p3fes_eehot.py the game thread is in (the
// innermost one; code without a copy counts for the copy that called it) and which activity, and which compiled VU program the VU
// thread is running and its activity. Every 10 seconds it appends what it counted to sdmc:/p3fes/samples.txt and starts again:
// "period N", then "copy activity count" lines for the game thread (copy 0 is outside every copy) and "vu program activity count"
// lines for the VU thread (program is a function address in build/3ds/p3fes.map, 1 the interpreter, 0 none). Activities are
// prof.hpp's counters: 14 is outside every activity, 15 to 28 are PS2_PROF_EXTRA 0 to 13, 31 not known (PROF=1, VU thread).
// tools/p3fes_samples.py reads it.
namespace gen {
volatile std::uint16_t g_hot_now;
}
namespace ps2::prof {
#ifndef PS2_PROF_ALL
volatile std::uint8_t g_where = Count, g_where_vu = Count;
void *g_game_thread = nullptr, *g_vu_thread = nullptr;
#endif
volatile std::uintptr_t g_vu_program = 0;
}
namespace {
constexpr int kSampleCopies = 1024, kSampleActs = 32, kSamplePrograms = 512;
std::uint32_t g_samples[kSampleCopies][kSampleActs];
struct ProgramSamples {
    std::uintptr_t fn;
    std::uint32_t n[kSampleActs];
};
ProgramSamples g_program_samples[kSamplePrograms];

std::uint32_t g_no_program[kSampleActs];

void count_program(std::uintptr_t fn, int act) {
    if (!fn) {
        ++g_no_program[act];
        return;
    }
    for (std::uint32_t i = static_cast<std::uint32_t>(fn >> 2) * 2654435761u >> 23, tries = 0; tries < kSamplePrograms;
         i = (i + 1) & (kSamplePrograms - 1), ++tries) {
        ProgramSamples &p = g_program_samples[i];
        if (p.fn == fn || !p.fn) {
            p.fn = fn;
            ++p.n[act];
            return;
        }
    }
}

void sampler(void *) {
    std::FILE *f = std::fopen("sdmc:/p3fes/samples.txt", "w");
    if (!f) return;
    const u64 period = SYSCLOCK_ARM11 * 10ull;
    u64 next = svcGetSystemTick() + period;
    for (int n = 0; !g_finished && !g_quit; ) {
        svcSleepThread(250000);
        const int k = gen::g_hot_now & (kSampleCopies - 1);
#ifdef PS2_PROF_ALL
        const int act = ps2::prof::g_events_at & (kSampleActs - 1), vu_act = kSampleActs - 1;
#else
        const int act = ps2::prof::g_where & (kSampleActs - 1), vu_act = ps2::prof::g_where_vu & (kSampleActs - 1);
#endif
        ++g_samples[k][act];
        count_program(ps2::prof::g_vu_program, vu_act);
        if (svcGetSystemTick() < next) continue;
        next += period;
        std::fprintf(f, "period %d\n", n++);
        for (int i = 0; i < kSampleCopies; ++i)
            for (int j = 0; j < kSampleActs; ++j)
                if (g_samples[i][j]) std::fprintf(f, "%d %d %lu\n", i, j, static_cast<unsigned long>(g_samples[i][j]));
        for (int j = 0; j < kSampleActs; ++j)
            if (g_no_program[j]) std::fprintf(f, "vu 00000000 %d %lu\n", j, static_cast<unsigned long>(g_no_program[j]));
        for (const ProgramSamples &p : g_program_samples)
            for (int j = 0; j < kSampleActs; ++j)
                if (p.n[j]) std::fprintf(f, "vu %08lX %d %lu\n", static_cast<unsigned long>(p.fn), j, static_cast<unsigned long>(p.n[j]));
        std::fflush(f);
        std::memset(g_samples, 0, sizeof g_samples);
        std::memset(g_program_samples, 0, sizeof g_program_samples);
        std::memset(g_no_program, 0, sizeof g_no_program);
    }
    std::fclose(f);
}

void start_sampler(s32 priority) {
    Thread t = nullptr;
    if (R_SUCCEEDED(APT_SetAppCpuTimeLimit(80))) t = threadCreate(sampler, nullptr, 16 * 1024, priority, 1, true);
    if (!t) t = threadCreate(sampler, nullptr, 16 * 1024, priority, 2, true);
    std::fprintf(stderr, "[3ds] sampler %s\n", t ? "started" : "could not start");
}
}
#endif

namespace {

// The game counts vblanks, so without this a scene lighter than one vblank of work plays fast. Four frames of credit even out slow
// ones: with two, the lobby at about 19 ms a frame still slept 45 ms in every 30 frames, after the faster stretches between slow
// ones, because a slow stretch had used up more credit than two frames.
void hold_speed() {
    constexpr u64 kClock = SYSCLOCK_ARM11, kFrame = kClock * 1001 / 60000, kCredit = 4;
    static u64 due = 0;
    const u64 now = svcGetSystemTick();
    due = std::max(due, now - std::min(now, kCredit * kFrame)) + kFrame;
    if (due > now) svcSleepThread(static_cast<s64>((due - now) * 1000000 / kClock * 1000));
}

// The bottom screen. The game thread reads the touch screen with the buttons and acts on what the main thread decided.
std::atomic<std::uint32_t> g_touch{0};          // 0x80000000 | x << 16 | y while the screen is touched
std::atomic<std::uint32_t> g_touch_buttons{0};  // PS2 pad bits held on the bottom screen
std::atomic<int> g_save_slot{0};                // 1 to 3: save a state there at the next vblank
std::atomic<std::uint32_t> g_saves_done{0};
std::uint64_t g_save_at = 0;

void on_vblank() {
    if (g_hold_speed) hold_speed();
    if (g_quit) {
        ps2::halt("closed from the 3DS");
        return;
    }
    hidScanInput();
    static bool saving = false;
    if (saving) {   // the save asked for at the last vblank is done by now
        saving = false;
        g_saves_done.fetch_add(1);
    }
    if (hidKeysHeld() & KEY_TOUCH) {
        touchPosition touch{};
        hidTouchRead(&touch);
        g_touch.store(0x80000000u | (static_cast<std::uint32_t>(touch.px) << 16) | touch.py);
    } else {
        g_touch.store(0);
    }
    if (g_save_at && ps2::frame_count() == g_save_at) g_save_slot.store(1);
    if (const int slot = g_save_slot.exchange(0)) {
        ps2::g_options.save_state_path = "sdmc:/p3fes/slot" + std::to_string(slot) + ".state";
        saving = true;
        ps2::request_state_save();
    }
    circlePosition pad{}, cstick{};
    hidCircleRead(&pad);
    hidCstickRead(&cstick);
    const std::uint64_t frame = ps2::frame_count();
    const bool scripted = std::any_of(ps2::g_options.presses.begin(), ps2::g_options.presses.end(), [frame](const ps2::Press &press) {
        return frame >= press.frame && frame - press.frame < press.frames;
    });
    if (!scripted) {
        ps2::set_pad_buttons(read_buttons(hidKeysHeld()) | remote::buttons(frame) | static_cast<std::uint16_t>(g_touch_buttons.load()));
        ps2::set_pad_axis(0, stick_axis(cstick.dx));
        ps2::set_pad_axis(1, stick_axis(-cstick.dy));
        ps2::set_pad_axis(2, stick_axis(pad.dx));
        ps2::set_pad_axis(3, stick_axis(-pad.dy));
    }
    if (g_software && g_show_every && frame % g_show_every == 0) show_frame();
    if (!g_perf) return;
    u64 now = 0;
    if (!g_hold_speed) {
        now = svcGetSystemTick();
        if (g_last_frame_ticks) {
            constexpr u64 kFrameBudget = static_cast<u64>(SYSCLOCK_ARM11) * 16 / 1000;
            const u64 elapsed = now - g_last_frame_ticks;
            g_peak_frame_ticks = std::max(g_peak_frame_ticks, elapsed);
            ++g_frame_samples;
            g_frames_over_budget += elapsed > kFrameBudget;
        }
        g_last_frame_ticks = now;
    }
    if (frame % 30 == 0) {
        if (g_hold_speed) now = svcGetSystemTick();
        const std::uint64_t prims = ps2::primitives_drawn();
        std::fprintf(g_perf, "frame %llu ms_total %llu ms_30frames %llu prims_30frames %llu", static_cast<unsigned long long>(frame),
                     static_cast<unsigned long long>((now - g_start_ticks) / static_cast<u64>(CPU_TICKS_PER_MSEC)),
                     static_cast<unsigned long long>((now - g_last_ticks) / static_cast<u64>(CPU_TICKS_PER_MSEC)),
                     static_cast<unsigned long long>(prims - g_last_prims));
        if (g_pmu_a >= 0) pmu_line();
        if (!g_hold_speed) {
            constexpr u64 kClock = SYSCLOCK_ARM11;
            const u64 peak_us = (g_peak_frame_ticks * 1000000 + kClock - 1) / kClock;
            std::fprintf(g_perf, " sampled_frames %u peak_frame_us %llu frames_over_16ms %u",
                         g_frame_samples, static_cast<unsigned long long>(peak_us), g_frames_over_budget);
            g_peak_frame_ticks = 0;
            g_frame_samples = g_frames_over_budget = 0;
        }
        std::fputc('\n', g_perf);
        std::fflush(g_perf);
        g_last_ticks = now;
        g_last_prims = prims;
    }
}

// mvdprobe 1: can the New 3DS video decoder (MVD) start while the game is resident? It logs the work buffer
// MVD asks for at a few movie sizes and whether it starts with the smallest one, then shuts it down again.
bool g_mvd_probe = false;

void mvd_probe() {
    std::fprintf(stderr, "[mvd] linear free %lu KB before\n", static_cast<unsigned long>(linearSpaceFree() >> 10));
    Result r = mvdstdInit(MVDMODE_COLORFORMATCONV, MVD_INPUT_YUYV422, MVD_OUTPUT_RGB565, 0, nullptr);
    std::fprintf(stderr, "[mvd] init for conversion 0x%08lX\n", static_cast<unsigned long>(r));
    if (R_FAILED(r)) return;
    struct Size { u32 w, h, refs; };
    const Size sizes[] = {{640, 448, 1}, {640, 448, 2}, {640, 448, 4}, {512, 448, 2}, {400, 240, 2}};
    u32 smallest = 0;
    for (const Size &z : sizes) {
        MVDSTD_CalculateWorkBufSizeConfig cfg{};
        cfg.ref_frames_a.enable = 1;
        cfg.ref_frames_a.ref_frames = static_cast<u8>(z.refs);
        cfg.width = z.w;
        cfg.height = z.h;
        u32 size = 0;
        r = mvdstdCalculateBufferSize(&cfg, &size);
        std::fprintf(stderr, "[mvd] %lux%lu, %lu reference frames: result 0x%08lX, work buffer %lu KB\n", static_cast<unsigned long>(z.w),
                     static_cast<unsigned long>(z.h), static_cast<unsigned long>(z.refs), static_cast<unsigned long>(r), static_cast<unsigned long>(size >> 10));
        if (R_SUCCEEDED(r) && z.w == 640 && z.refs == 2) smallest = size;
    }
    mvdstdExit();
    if (!smallest) smallest = MVD_DEFAULT_WORKBUF_SIZE;
    r = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_RGB565, smallest, nullptr);
    std::fprintf(stderr, "[mvd] init for H.264 with %lu KB: 0x%08lX, linear free %lu KB\n", static_cast<unsigned long>(smallest >> 10),
                 static_cast<unsigned long>(r), static_cast<unsigned long>(linearSpaceFree() >> 10));
    if (R_SUCCEEDED(r)) mvdstdExit();
    r = mvdstdInit(MVDMODE_VIDEOPROCESSING, MVD_INPUT_H264, MVD_OUTPUT_RGB565, MVD_DEFAULT_WORKBUF_SIZE, nullptr);
    std::fprintf(stderr, "[mvd] init for H.264 with the default %lu KB: 0x%08lX\n", static_cast<unsigned long>(MVD_DEFAULT_WORKBUF_SIZE >> 10),
                 static_cast<unsigned long>(r));
    if (R_SUCCEEDED(r)) mvdstdExit();
    std::fprintf(stderr, "[mvd] linear free %lu KB after\n", static_cast<unsigned long>(linearSpaceFree() >> 10));
}

bool g_movies = true;
bool g_sound = true;

void read_settings(const char *path) {
    std::FILE *f = std::fopen(path, "r");
    if (!f) return;
    char line[128];
    while (std::fgets(line, sizeof line, f)) {
        char key[32], value[64];
        if (std::sscanf(line, "%31s %63s", key, value) != 2) continue;
        if (!std::strcmp(key, "frames")) ps2::g_options.max_frames = std::strtoull(value, nullptr, 10);
        else if (!std::strcmp(key, "every")) g_show_every = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
        else if (!std::strcmp(key, "load")) g_state_path = std::string("sdmc:/p3fes/") + value;
        else if (!std::strcmp(key, "interp")) setenv("PS2_VU_INTERPRET", value, 1);
        else if (!std::strcmp(key, "record")) setenv("PS2_VU_RECORD", value, 1);
        else if (!std::strcmp(key, "dump")) g_dump_frame = std::strtoull(value, nullptr, 10);
        else if (!std::strcmp(key, "skip")) g_skip_attr = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 16));
        else if (!std::strcmp(key, "link")) g_link = std::atoi(value) != 0;
        else if (!std::strcmp(key, "debug")) g_debug = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
        else if (!std::strcmp(key, "hide")) {
            char *end = nullptr;
            g_hide_from = static_cast<std::uint32_t>(std::strtoul(value, &end, 10));
            g_hide_to = *end ? static_cast<std::uint32_t>(std::strtoul(end + 1, nullptr, 10)) : g_hide_from + 1;
        }
        else if (!std::strcmp(key, "pmu")) {
            char *end = nullptr;
            g_pmu_a = static_cast<int>(std::strtol(value, &end, 0));
            g_pmu_b = g_pmu_a + 1;
            char second[32];
            if (std::sscanf(line, "%*s %*s %31s", second) == 1) g_pmu_b = static_cast<int>(std::strtol(second, nullptr, 0));
        }
        else if (!std::strcmp(key, "soft")) g_software = std::strtoul(value, nullptr, 10) != 0;
        else if (!std::strcmp(key, "stats")) g_stats = std::strtoul(value, nullptr, 10) != 0;
        else if (!std::strcmp(key, "vuthread")) g_vu_thread = std::strtoul(value, nullptr, 10) != 0;
        else if (!std::strcmp(key, "splitgs")) {
            const auto mode = std::strtoul(value, nullptr, 10);
            g_split_gs = mode != 0;
            g_gs_core = mode == 2 ? 1 : 0;
        }
        else if (!std::strcmp(key, "iopthread")) g_iop_thread = std::strtoul(value, nullptr, 10) != 0;
        else if (!std::strcmp(key, "uncapped")) g_hold_speed = std::strtoul(value, nullptr, 10) == 0;
        else if (!std::strcmp(key, "mvdprobe")) g_mvd_probe = std::strtoul(value, nullptr, 10) != 0;
        else if (!std::strcmp(key, "g3")) g3native::g_mode = std::atoi(value);
        else if (!std::strcmp(key, "g3cache")) g3cache::g_mode = std::atoi(value);
        else if (!std::strcmp(key, "g3skin")) g3cache::g_skin = std::atoi(value);
        else if (!std::strcmp(key, "g3index")) g3cache::g_index = std::atoi(value);
        else if (!std::strcmp(key, "g3gpu")) g3native::g_gpu_mask = static_cast<std::uint32_t>(std::atoi(value));
        else if (!std::strcmp(key, "g3cap")) g3native::g_capture_left = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
        else if (!std::strcmp(key, "g3every")) g3native::g_capture_every = std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10)));
        else if (!std::strcmp(key, "lossy")) gen::g_lossy = std::strtoul(value, nullptr, 10) != 0;
        else if (!std::strcmp(key, "vulag")) ps2::gs::g_max_vu_lag = std::strtoul(value, nullptr, 10);
        else if (!std::strcmp(key, "stereo")) ps2::gs::pica::set_stereo(std::strtoul(value, nullptr, 10));
        else if (!std::strcmp(key, "movies")) g_movies = std::strtoul(value, nullptr, 10) != 0;
        else if (!std::strcmp(key, "sound")) g_sound = std::strtoul(value, nullptr, 10) != 0;
        else if (!std::strcmp(key, "saveat")) g_save_at = std::strtoull(value, nullptr, 10);
        else if (!std::strcmp(key, "draw")) g_draw_every = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
        else if (!std::strcmp(key, "shots")) {
            mkdir("sdmc:/p3fes/shots", 0777);
            g_shots_every = static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
        } else if (!std::strcmp(key, "press")) {
            ps2::Press press;
            if (ps2::parse_press(value, press)) ps2::g_options.presses.push_back(press);
        }
    }
    std::fclose(f);
}

// citro3d takes over the top screen's buffers, so it must start on the thread that draws.
bool start_gpu() {
    using namespace ps2::gs;
    if (!pica::init()) return false;
    if (g_mvd_probe) mvd_probe();
    pica::set_present_every(g_show_every);
    pica::set_shot_every(g_shots_every);
    pica::set_draw_every(g_draw_every);
    pica::set_stats(g_stats);
    pica::set_dump_frame(g_dump_frame);
    pica::set_skip_attr(g_skip_attr);
    pica::set_debug(g_debug);
    pica::set_hide(g_hide_from, g_hide_to);
    return true;
}

WorkerState wait_worker(const std::atomic<WorkerState> &state) {
    WorkerState value;
    while ((value = state.load()) == WorkerState::Waiting) svcSleepThread(1000000);
    return value;
}

void gs_main(void *) {
    const bool ready = start_gpu();
    g_gs_ready.store(ready ? WorkerState::Ready : WorkerState::Failed);
    if (!ready) return;
    if (wait_worker(g_worker_start) == WorkerState::Ready) ps2::run_pica_thread();
    ps2::gs::pica::shutdown();
}

// The VU worker owns the vector programs, and also draws when there is no separate GS worker.
void vu_main(void *) {
#if defined(PS2_HOT_SAMPLE) && !defined(PS2_PROF_ALL)
    ps2::prof::g_vu_thread = getThreadLocalStorage();
#endif
    if (wait_worker(g_worker_start) != WorkerState::Ready) return;
    const bool ready = g_split_gs || start_gpu();
    g_vu_ready.store(ready ? WorkerState::Ready : WorkerState::Failed);
    if (!ready) return;
    ps2::run_vu_thread();
    if (!g_split_gs) ps2::gs::pica::shutdown();
}

int g_vu_core = 0;

Thread start_vu_thread() {
    // Core 2 exists only on the New 3DS and is free for applications. Core 1 is shared with the system, which needs a limit set.
    Thread thread = threadCreate(vu_main, nullptr, kVuStack, g_priority + 1, 2, false);
    int core = 2;
    if (!thread) {
        APT_SetAppCpuTimeLimit(80);
        thread = threadCreate(vu_main, nullptr, kVuStack, g_priority + 1, 1, false);
        core = 1;
    }
    if (!thread) {
        std::fprintf(stderr, "[3ds] no core for the VU thread\n");
        return nullptr;
    }
    g_vu_handle = thread;
    g_vu_core = core;
    if (core != 2) g_split_gs = false;
    if (g_split_gs) {
        if (g_gs_core == 1 && R_FAILED(APT_SetAppCpuTimeLimit(80))) g_gs_core = 0;
        g_gs_handle = threadCreate(gs_main, nullptr, kVuStack, g_priority + 2, g_gs_core, false);
        if (!g_gs_handle && g_gs_core == 1) {
            std::fprintf(stderr, "[3ds] core 1 unavailable for drawing, trying core 0\n");
            g_gs_core = 0;
            g_gs_handle = threadCreate(gs_main, nullptr, kVuStack, g_priority + 2, 0, false);
        }
        if (!g_gs_handle) {
            std::fprintf(stderr, "[3ds] no GS thread, drawing on the VU thread\n");
            g_split_gs = false;
        } else if (wait_worker(g_gs_ready) == WorkerState::Failed) {
            g_worker_start.store(WorkerState::Failed);
            threadJoin(g_gs_handle, U64_MAX);
            threadFree(g_gs_handle);
            g_gs_handle = nullptr;
            threadJoin(thread, U64_MAX);
            threadFree(thread);
            g_finished = true;
            return nullptr;
        } else {
            std::fprintf(stderr, "[3ds] GS thread on core %d\n", g_gs_core);
        }
    }
    ps2::enable_split_pica(g_split_gs);
    ps2::start_render_thread();
    ps2::start_vu_thread();
    g_worker_start.store(WorkerState::Ready);
    if (wait_worker(g_vu_ready) == WorkerState::Failed) {
        threadJoin(thread, U64_MAX);
        threadFree(thread);
        g_finished = true;
        return nullptr;
    }
    std::fprintf(stderr, "[3ds] VU thread on core %d\n", core);
    return thread;
}

void emulate_on_game_stack() {
    Thread vu_thread = nullptr;
#if defined(PS2_PROF_ALL) || defined(PS2_HOT_SAMPLE)
    ps2::prof::g_game_thread = getThreadLocalStorage();
#endif
    g_split_gs = g_split_gs && g_vu_thread && !g_software;
    if (g_software) {
        ps2::g_options.frames_dir = g_shots_every ? "sdmc:/p3fes/shots" : "";
        ps2::g_options.frame_every = g_shots_every;
    } else {
        if (g_vu_thread) vu_thread = start_vu_thread();
        if (g_finished) return;
        if (!vu_thread && !start_gpu()) {
            g_finished = true;
            return;
        }
    }
    g3native::start(!g_software && !g_split_gs && ps2::gs::pica::g3_ready());
    if (!g_state_path.empty()) {   // a setting left over from a test must not end the game, which would take the wireless link with it
        if (std::FILE *state = std::fopen(g_state_path.c_str(), "rb")) {
            std::fclose(state);
        } else {
            std::fprintf(stderr, "[state] no file %s, starting from the beginning\n", g_state_path.c_str());
            g_state_path.clear();
        }
    }
    std::uint32_t entry = 0;
    if (!open_disc()) {
        std::fprintf(stderr, "cannot open the disc image in sdmc:/p3fes\n");
    } else if (!ps2::load_elf("sdmc:/p3fes/SLUS_216.21", entry)) {
        std::fprintf(stderr, "cannot load sdmc:/p3fes/SLUS_216.21\n");
    } else {
        // Core 1 is the system's too, so with the vector programs there as well, the I/O processor stays on the game's core.
        if (g_iop_thread && g_vu_core != 1) {
            const bool started = ps2::start_iop_thread(g_priority + 1, 1);
            std::fprintf(stderr, "[3ds] I/O processor %s\n", started ? "on core 1" : "stays on the game's core");
        }
        std::fprintf(stderr, "[3ds] booting, entry 0x%08lX\n", static_cast<unsigned long>(entry));
        pmu_start();
        g_start_ticks = g_last_ticks = svcGetSystemTick();
        if (!g_quit) {
            ps2::boot(entry, {"SLUS_216.21"});
            if (!g_quit && (g_state_path.empty() || ps2::load_state(g_state_path))) ps2::run();
        }
        ps2::stop_iop_thread();
        if (ps2_dump_ee_seen("sdmc:/p3fes/eeseen.bin")) std::fprintf(stderr, "[3ds] wrote sdmc:/p3fes/eeseen.bin\n");
    }
    {
        const struct mallinfo m = mallinfo();
        std::fprintf(stderr, "[3ds] heap in use %lu KB of %lu KB\n", static_cast<unsigned long>(m.uordblks >> 10), static_cast<unsigned long>(envGetHeapSize() >> 10));
    }
    if (vu_thread) {
        ps2::stop_vu_thread();
        threadJoin(vu_thread, U64_MAX);
        threadFree(vu_thread);
        g_vu_handle = nullptr;
        if (g_gs_handle) {
            threadJoin(g_gs_handle, U64_MAX);
            threadFree(g_gs_handle);
            g_gs_handle = nullptr;
        }
        g_vu_stopped.store(true);
    } else {
        ps2::gs::pica::shutdown();
    }
    g_finished = true;
}

// Calls fn on the stack that ends at top, then comes back to the caller's stack.
[[gnu::naked]] void call_on_stack(void (*fn)(), u32 top) {
    asm volatile("push {r4, lr}\n\t"
                 "mov r4, sp\n\t"
                 "mov sp, r1\n\t"
                 "blx r0\n\t"
                 "mov sp, r4\n\t"
                 "pop {r4, pc}\n\t");
}

void emulate(void *) { call_on_stack(emulate_on_game_stack, kGameStackTop); }

// ---- The bottom screen: saved states and a few controls the console has no button for.
// It is the text console, 40 columns by 30 rows of 8x8 cells, so a button is a block of coloured cells.
struct Button {
    int col, row, cols, rows;
    const char *label;
    int id;
};
enum { kSave = 1, kLoad, kSlot1, kSlot2, kSlot3, kL3, kR3, kFast, kFlatter, kDeeper };
constexpr Button kButtons[] = {
    {2, 4, 17, 5, "SAVE STATE", kSave},  {21, 4, 17, 5, "LOAD STATE", kLoad},
    {10, 11, 8, 3, "1", kSlot1},         {20, 11, 8, 3, "2", kSlot2},         {30, 11, 8, 3, "3", kSlot3},
    {2, 17, 11, 3, "L3", kL3},           {14, 17, 11, 3, "R3", kR3},          {27, 17, 11, 3, "FAST", kFast},
    {20, 22, 8, 3, "-", kFlatter},       {30, 22, 8, 3, "+", kDeeper},
};
int g_slot = 1;
char g_panel_status[40] = "";
bool g_panel_was_saving = false;
std::uint32_t g_panel_saves = 0;
std::uint64_t g_panel_frames = 0;
u64 g_panel_ticks = 0;

void panel_button(const Button &b, bool lit) {
    const int text_row = b.row + b.rows / 2, length = static_cast<int>(std::strlen(b.label));
    for (int r = 0; r < b.rows; ++r) {
        std::printf("\x1b[%d;%dH\x1b[%sm", b.row + r + 1, b.col + 1, lit ? "47;30" : "44;37");
        for (int c = 0; c < b.cols; ++c) {
            const int at = c - (b.cols - length) / 2;
            std::putchar(r + b.row == text_row && at >= 0 && at < length ? b.label[at] : ' ');
        }
    }
    std::printf("\x1b[0m");
}

bool slot_exists(int slot) {
    struct stat info;
    return stat(("sdmc:/p3fes/slot" + std::to_string(slot) + ".state").c_str(), &info) == 0;
}

void panel_draw() {
    std::printf("\x1b[2J\x1b[1;1H\x1b[0m Persona 3 FES");
    for (const Button &b : kButtons)
        panel_button(b, (b.id >= kSlot1 && b.id <= kSlot3 && b.id - kSlot1 + 1 == g_slot) || (b.id == kFast && !g_hold_speed));
    std::printf("\x1b[13;3HSLOT\x1b[15;11H%-28s", slot_exists(g_slot) ? "holds a saved state" : "empty");
    std::printf("\x1b[24;3H3D DEPTH %-2lu", static_cast<unsigned long>(ps2::gs::pica::stereo()));
    std::printf("\x1b[28;2H%-38s", g_panel_status);
    std::fflush(stdout);
}

void panel_say(const char *text) {
    std::snprintf(g_panel_status, sizeof g_panel_status, "%s", text);
    panel_draw();
}

// Once per vblank on the main thread.
void panel_step() {
    static int held = 0;
    static std::uint32_t beats = 0;
    const std::uint32_t touch = g_touch.load();
    int over = 0;
    if (touch) {
        const int col = static_cast<int>((touch >> 16) & 0x7FFFu) / 8, row = static_cast<int>(touch & 0xFFFFu) / 8;
        for (const Button &b : kButtons)
            if (col >= b.col && col < b.col + b.cols && row >= b.row && row < b.row + b.rows) over = b.id;
    }
    g_touch_buttons.store(over == kL3 ? 1u << 1 : (over == kR3 ? 1u << 2 : 0u));
    if (over != held) {
        held = over;
        switch (over) {
        case kSave:
            if (g_panel_was_saving) break;
            g_panel_was_saving = true;
            g_panel_saves = g_saves_done.load();
            g_save_slot.store(g_slot);
            panel_say("Saving...");
            break;
        case kLoad:
            if (!slot_exists(g_slot)) {
                panel_say("This slot is empty");
                break;
            }
            if (std::FILE *f = std::fopen("sdmc:/p3fes/next.txt", "w")) {
                std::fprintf(f, "load slot%d.state\n", g_slot);
                std::fclose(f);
                panel_say("Loading: the game starts again...");
                remote::restart();
            }
            break;
        case kSlot1: case kSlot2: case kSlot3:
            g_slot = over - kSlot1 + 1;
            panel_say("");
            break;
        case kFast:
            g_hold_speed = !g_hold_speed;
            panel_say(g_hold_speed ? "Normal speed" : "As fast as it can");
            break;
        case kFlatter: case kDeeper: {
            const std::uint32_t now = ps2::gs::pica::stereo();
            ps2::gs::pica::set_stereo(over == kDeeper ? now + 1 : (now ? now - 1 : 0));
            panel_say("");
            break;
        }
        default:
            break;
        }
    }
    if (g_panel_was_saving && g_saves_done.load() != g_panel_saves) {
        g_panel_was_saving = false;
        char text[40];
        std::snprintf(text, sizeof text, slot_exists(g_slot) ? "Saved to slot %d" : "Could not save to slot %d", g_slot);
        panel_say(text);
    }
    if (++beats % 60 == 0) {   // game frames a second over the last second: 60 is full speed
        const u64 now = svcGetSystemTick();
        const std::uint64_t frames = ps2::frame_count();
        if (g_panel_ticks) {
            const double seconds = static_cast<double>(now - g_panel_ticks) / static_cast<double>(SYSCLOCK_ARM11);
            std::printf("\x1b[1;26H%5.1f a second", static_cast<double>(frames - g_panel_frames) / seconds);
            std::fflush(stdout);
        }
        g_panel_ticks = now;
        g_panel_frames = frames;
    }
}

}

int main() {
    std::set_terminate([] {
        const char *what = "unknown";
        try {
            if (const std::exception_ptr e = std::current_exception()) std::rethrow_exception(e);
        } catch (const std::exception &e) {
            what = e.what();
        } catch (...) {
        }
        const struct mallinfo m = mallinfo();
        std::fprintf(stderr, "[3ds] uncaught exception on %s thread: %s, heap in use %lu KB, largest free block %lu KB\n", threadGetCurrent() == g_vu_handle ? "the VU" : "the game", what, static_cast<unsigned long>(m.uordblks >> 10), static_cast<unsigned long>(m.fordblks >> 10));
        std::fflush(stderr);
        svcBreak(USERBREAK_PANIC);
    });
    gfxInit(GSP_BGR8_OES, GSP_BGR8_OES, false);
    gfxSet3D(true);   // the right eye's buffer is shown only while the 3D slider is up
    gfxSetDoubleBuffering(GFX_TOP, true);
    consoleInit(GFX_BOTTOM, nullptr);
    osSetSpeedupEnable(true);

    mkdir("sdmc:/p3fes", 0777);
    std::freopen("sdmc:/p3fes/log.txt", "w", stderr);
    // A picture can log several lines. Batch them so SD writes do not stall the drawing core per line.
    static char log_buffer[16 * 1024];
    std::setvbuf(stderr, log_buffer, _IOFBF, sizeof log_buffer);
    g_perf = std::fopen("sdmc:/p3fes/perf.txt", "w");

    log_memory();
    u32 mapped = 0;
    const Result rc = svcControlMemory(&mapped, ps2::kRamAddress3ds, 0, kGameStackTop - ps2::kRamAddress3ds, MEMOP_ALLOC,
                                       static_cast<MemPerm>(MEMPERM_READ | MEMPERM_WRITE));
    if (R_FAILED(rc)) log_what_fits();
    std::fprintf(stderr, "[3ds] PS2 RAM at 0x%08lX, result 0x%08lX, heap %lu MB\n", static_cast<unsigned long>(mapped),
                 static_cast<unsigned long>(rc), static_cast<unsigned long>(envGetHeapSize() >> 20));
    if (R_FAILED(rc)) {
        std::printf("Could not get memory for the game: 0x%08lX\nPress Home to leave.\n", static_cast<unsigned long>(rc));
        std::fflush(stderr);
        if (g_link) remote::start();   // the program got too big: another build can still be sent
        while (aptMainLoop() && !remote::wants_exit()) {
            remote::poll(0);
            gspWaitForVBlank();
        }
    } else {
        std::memset(ps2::g_ram, 0, ps2::kRamSize);
        read_settings("sdmc:/p3fes/args.txt");
        read_settings("sdmc:/p3fes/next.txt");
        std::remove("sdmc:/p3fes/next.txt");
        if (g_link) remote::start();
        if (g_sound) audio3ds::start();
        ps2::g_options.on_vblank = on_vblank;
        ps2::g_options.memcard_path = "sdmc:/p3fes/card.ps2";
        register_generated_code();
        register_generated_vu();

        svcGetThreadPriority(&g_priority, CUR_THREAD_HANDLE);
        // The movie decoder shares core 2 with the vector programs and drawing, below them: a movie leaves that core idle.
        if (g_movies && !g_software && movie::start(g_priority + 2, 2)) ps2::ipu::g_csc_watch = movie::watch;
#ifdef PS2_HOT_SAMPLE
        start_sampler(g_priority - 1);
#endif
        Thread thread = threadCreate(emulate, nullptr, kEmulationStack, g_priority + 1, -2, false);
        if (!thread) std::puts("Could not start the game thread.");
        std::uint32_t beats = 0;
        panel_draw();
        while (thread && !g_finished) {
            if (!aptMainLoop() || remote::wants_exit()) {
                g_quit = true;
                ps2::abort_vu_thread();
                break;
            }
            gspWaitForVBlank();
            audio3ds::poll();
            remote::poll(ps2::frame_count());
            panel_step();
            if (++beats % 60 == 0) std::fflush(stderr);
            if (beats % 600 == 0) {
                static std::uint64_t stall_frame = 0;
                const std::uint64_t now_frame = ps2::frame_count();
                if (now_frame == stall_frame) {
                    using namespace ps2::prof;
                    u8 shell = 0xFF;
                    if (R_SUCCEEDED(ptmuInit())) {
                        PTMU_GetShellState(&shell);
                        ptmuExit();
                    }
                    std::fprintf(stderr, "[3ds] stalled: drawing at %d, vu thread idle %lu ms, ee blocked %lu ms, lid %s, 3D slider %.2f\n", g_fes_trace,
                                 static_cast<unsigned long>(g_ticks[Wait].load() / CPU_TICKS_PER_MSEC), static_cast<unsigned long>(g_ticks[Full].load() / CPU_TICKS_PER_MSEC),
                                 shell == 1 ? "open" : shell == 0 ? "closed" : "not known", static_cast<double>(osGet3DSliderState()));
                    char queue[512];
                    FesOverlapDescribe(queue, sizeof queue);
                    std::fprintf(stderr, "[3ds] stalled: %s\n", queue);
                }
                stall_frame = now_frame;
            }
            if (beats % 300 == 0) {   // sound and game speed every 5 s
                static u64 last_tick = 0;
                static std::uint64_t last_frame = 0;
                const u64 tick = svcGetSystemTick();
                const std::uint64_t frame = ps2::frame_count();
                using namespace ps2::prof;
                static std::uint32_t last_wait = 0, last_full = 0;   // 32-bit counters: they wrap every 16 s, their differences do not
                static std::uint32_t last_shown = 0;
                const std::uint32_t shown = ps2::gs::pica::pictures_shown();
                const std::uint32_t wait = g_ticks[Wait].load(), full = g_ticks[Full].load();
                if (last_tick) {
                    const double seconds = static_cast<double>(tick - last_tick) / static_cast<double>(SYSCLOCK_ARM11);
                    // Out of range for a line or two when the counters restart (a state loaded).
                    const auto share = [&](std::uint32_t ticks) {
                        const double percent = 100.0 * ticks / (seconds * SYSCLOCK_ARM11);
                        return percent <= 110.0 ? percent : 0.0;
                    };
                    // ee blocked: the game thread waiting for room in the drawing queue; vu idle: the drawing thread with nothing to do
                    std::fprintf(stderr, "[3ds] game at %.0f%% of full speed (%llu frames in %.1f s), %.1f pictures a second, ee blocked %.0f%%, vu idle %.0f%%\n",
                                 100.0 * (frame - last_frame) / (seconds * 59.94), static_cast<unsigned long long>(frame - last_frame), seconds,
                                 (shown - last_shown) / seconds,
                                 share(full - last_full),
                                 share(wait - last_wait));
                    audio3ds::report(seconds);
                }
                last_tick = tick;
                last_frame = frame;
                last_wait = wait;
                last_full = full;
                last_shown = shown;
            }
            if (beats % 600 == 0)
                std::fprintf(stderr, "[3ds] heartbeat %lu, %llu ms, frame %llu\n", static_cast<unsigned long>(beats),
                             static_cast<unsigned long long>(svcGetSystemTick() / static_cast<u64>(CPU_TICKS_PER_MSEC)),
                             static_cast<unsigned long long>(ps2::frame_count()));
        }
        bool joined = true;
        if (thread) {
            // A stuck game must not block a restart from the Mac: ending the process stops its threads too.
            if (R_SUCCEEDED(threadJoin(thread, 8000000000ull))) threadFree(thread);
            else {
                joined = false;
                // Stop heap-stacked worker threads before __appExit unmaps the heap, or they will data-abort on wake.
                if (!g_vu_stopped.load()) {
                    ps2::stop_vu_thread();
                    if (g_vu_handle) threadJoin(g_vu_handle, 2000000000ull);
                    if (g_gs_handle) threadJoin(g_gs_handle, 2000000000ull);
                }
                ps2::stop_iop_thread();
            }
        }
        audio3ds::stop(joined);
        movie::stop();
        remote::stop();
        std::fflush(stderr);
        remote::chain_restart();
        if (g_perf) std::fclose(g_perf);
        std::fflush(stderr);
        if (joined) gfxExit();
        return 0;
    }

    movie::stop();
    remote::stop();
    std::fflush(stderr);
    remote::chain_restart();
    if (g_perf) std::fclose(g_perf);
    std::fflush(stderr);
    gfxExit();
    return 0;
}
