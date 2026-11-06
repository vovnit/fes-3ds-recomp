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
//                               "draw N" (draw one picture in N; the default 0 draws what keeps up with the game),
//                               "uncapped 1" (let a light scene run faster than the PS2, to measure how much room is left),
//                               "hide A-B" (leave out batches A up to B of each picture, to find which one draws something),
//                               "pmu 0 8" (hardware counters of the game's core in perf.txt, by event number, see below),
//                               "link 0" (no wireless test link)
//   sdmc:/p3fes/log.txt         everything the runtime prints
//   sdmc:/p3fes/perf.txt        one line of timing every 30 game frames
#include "pica.hpp"
#include "remote3ds.hpp"
#include "ps2/disc.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <3ds.h>

#include <cstdint>
#include <malloc.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <sys/stat.h>
#include <vector>

extern "C" {
// Program + these + the PS2 RAM must fit the 124 MB a New 3DS application gets.
u32 __ctru_heap_size = 28u << 20;
u32 __ctru_linear_heap_size = 13u << 20;
}

static_assert(ps2::kRamAddress3ds == OS_HEAP_AREA_BEGIN + (28u << 20), "the PS2 RAM follows the heap with no gap");

void register_generated_code();
void register_generated_vu();

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
constexpr std::size_t kEmulationStack = 3u << 19;   // 1.5 MB
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
std::uint32_t g_draw_every = 0;
bool g_link = true;
s32 g_priority = 0x30;
bool g_iop_thread = true;
bool g_hold_speed = true;
volatile bool g_vu_ready = false, g_vu_go = false, g_vu_failed = false;
Thread g_vu_handle = nullptr;
std::FILE *g_perf = nullptr;
u64 g_start_ticks = 0;
u64 g_last_ticks = 0;
std::uint64_t g_last_prims = 0;

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
}

// The game counts vblanks, so without this a scene lighter than one vblank of work plays fast. Two frames of credit even out a slow one.
void hold_speed() {
    constexpr u64 kClock = SYSCLOCK_ARM11, kFrame = kClock * 1001 / 60000;
    static u64 due = 0;
    const u64 now = svcGetSystemTick();
    due = std::max(due, now - std::min(now, 2 * kFrame)) + kFrame;
    if (due > now) svcSleepThread(static_cast<s64>((due - now) * 1000000 / kClock * 1000));
}

void on_vblank() {
    if (g_hold_speed) hold_speed();
    if (g_quit) {
        ps2::halt("closed from the 3DS");
        return;
    }
    hidScanInput();
    circlePosition pad{}, cstick{};
    hidCircleRead(&pad);
    hidCstickRead(&cstick);
    if (ps2::g_options.presses.empty()) {   // scripted test runs keep their own inputs
        ps2::set_pad_buttons(read_buttons(hidKeysHeld()) | remote::buttons(ps2::frame_count()));
        ps2::set_pad_axis(0, stick_axis(cstick.dx));
        ps2::set_pad_axis(1, stick_axis(-cstick.dy));
        ps2::set_pad_axis(2, stick_axis(pad.dx));
        ps2::set_pad_axis(3, stick_axis(-pad.dy));
    }
    const std::uint64_t frame = ps2::frame_count();
    if (g_software && g_show_every && frame % g_show_every == 0) show_frame();
    if (frame % 30 == 0 && g_perf) {
        const u64 now = svcGetSystemTick();
        const std::uint64_t prims = ps2::primitives_drawn();
        std::fprintf(g_perf, "frame %llu ms_total %llu ms_30frames %llu prims_30frames %llu", static_cast<unsigned long long>(frame),
                     static_cast<unsigned long long>((now - g_start_ticks) / static_cast<u64>(CPU_TICKS_PER_MSEC)),
                     static_cast<unsigned long long>((now - g_last_ticks) / static_cast<u64>(CPU_TICKS_PER_MSEC)),
                     static_cast<unsigned long long>(prims - g_last_prims));
        if (g_pmu_a >= 0) pmu_line();
        std::fputc('\n', g_perf);
        std::fflush(g_perf);
        g_last_ticks = now;
        g_last_prims = prims;
    }
}

void read_settings() {
    std::FILE *f = std::fopen("sdmc:/p3fes/args.txt", "r");
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
        else if (!std::strcmp(key, "iopthread")) g_iop_thread = std::strtoul(value, nullptr, 10) != 0;
        else if (!std::strcmp(key, "uncapped")) g_hold_speed = std::strtoul(value, nullptr, 10) == 0;
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

// The vector programs and all drawing, on a core of their own. The game thread only follows the VIF's registers and queues the
// data for here.
void vu_main(void *) {
    g_vu_failed = !start_gpu();
    g_vu_ready = true;
    if (g_vu_failed) return;
    while (!g_vu_go) svcSleepThread(1000000);
    ps2::run_vu_thread();
    ps2::gs::pica::shutdown();
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
    while (!g_vu_ready) svcSleepThread(1000000);
    if (g_vu_failed) {
        threadJoin(thread, U64_MAX);
        threadFree(thread);
        g_finished = true;
        return nullptr;
    }
    ps2::start_render_thread();
    ps2::start_vu_thread();
    g_vu_go = true;
    std::fprintf(stderr, "[3ds] VU thread on core %d\n", core);
    return thread;
}

void emulate(void *) {
    Thread vu_thread = nullptr;
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
        g_finished = true;
        return;
    }
    if (!ps2::load_elf("sdmc:/p3fes/SLUS_216.21", entry)) {
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
        ps2::boot(entry, {"SLUS_216.21"});
        if (g_state_path.empty() || ps2::load_state(g_state_path)) ps2::run();
        ps2::stop_iop_thread();
    }
    {
        const struct mallinfo m = mallinfo();
        std::fprintf(stderr, "[3ds] heap in use %lu KB of %lu KB\n", static_cast<unsigned long>(m.uordblks >> 10), static_cast<unsigned long>(envGetHeapSize() >> 10));
    }
    if (vu_thread) {
        ps2::stop_vu_thread();
        threadJoin(vu_thread, U64_MAX);
        threadFree(vu_thread);
    } else {
        ps2::gs::pica::shutdown();
    }
    g_finished = true;
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
    gfxSetDoubleBuffering(GFX_TOP, true);
    consoleInit(GFX_BOTTOM, nullptr);
    osSetSpeedupEnable(true);

    mkdir("sdmc:/p3fes", 0777);
    std::freopen("sdmc:/p3fes/log.txt", "w", stderr);
    std::setvbuf(stderr, nullptr, _IOLBF, 512);
    g_perf = std::fopen("sdmc:/p3fes/perf.txt", "w");

    log_memory();
    u32 mapped = 0;
    const Result rc = svcControlMemory(&mapped, ps2::kRamAddress3ds, 0, ps2::kRamSize + 0x1000u, MEMOP_ALLOC,
                                       static_cast<MemPerm>(MEMPERM_READ | MEMPERM_WRITE));
    if (R_FAILED(rc)) log_what_fits();
    std::fprintf(stderr, "[3ds] PS2 RAM at 0x%08lX, result 0x%08lX, heap %lu MB\n", static_cast<unsigned long>(mapped),
                 static_cast<unsigned long>(rc), static_cast<unsigned long>(envGetHeapSize() >> 20));
    if (R_FAILED(rc)) {
        std::printf("Could not get memory for the game: 0x%08lX\nPress Home to leave.\n", static_cast<unsigned long>(rc));
        std::fflush(stderr);
        while (aptMainLoop()) gspWaitForVBlank();
    } else {
        std::memset(ps2::g_ram, 0, ps2::kRamSize);
        read_settings();
        if (g_link) remote::start();
        ps2::g_options.on_vblank = on_vblank;
        ps2::g_options.memcard_path = "sdmc:/p3fes/card.ps2";
        register_generated_code();
        register_generated_vu();

        svcGetThreadPriority(&g_priority, CUR_THREAD_HANDLE);
        Thread thread = threadCreate(emulate, nullptr, kEmulationStack, g_priority + 1, -2, false);
        if (!thread) std::puts("Could not start the game thread.");
        std::puts("Persona 3 FES New 3DS test build\nProgress goes to sdmc:/p3fes/log.txt");
        std::uint32_t beats = 0;
        while (thread && !g_finished) {
            if (!aptMainLoop() || remote::wants_exit()) {
                g_quit = true;
                break;
            }
            gspWaitForVBlank();
            remote::poll(ps2::frame_count());
            if (++beats % 600 == 0)
                std::fprintf(stderr, "[3ds] heartbeat %lu, %llu ms, frame %llu\n", static_cast<unsigned long>(beats),
                             static_cast<unsigned long long>(svcGetSystemTick() / static_cast<u64>(CPU_TICKS_PER_MSEC)),
                             static_cast<unsigned long long>(ps2::frame_count()));
        }
        if (thread) {
            // A stuck game must not block a restart from the Mac: ending the process stops its threads too.
            if (R_SUCCEEDED(threadJoin(thread, 5000000000ull))) threadFree(thread);
        }
    }

    remote::stop();
    remote::chain_restart();
    if (g_perf) std::fclose(g_perf);
    std::fflush(stderr);
    gfxExit();
    return 0;
}
