// The I/O processor on its own core. In the lobby the game is idle for about 80% of the PS2's time, and the I/O processor's cost
// for those cycles fell between two pictures, on the game's core. Here the game thread only moves a target forward, and a thread
// on core 1 works towards it, so its work overlaps the game's.
//
// The two rarely touch: about seven messages at the start of a picture and one reply. Each chunk of work holds a lock, and
// anything from the game side that reads or changes the I/O processor takes it, so the game waits for one chunk at most.
//
// The picture rate does not change, because that depends on the core running the vector programs. What changes is how fast the
// game runs when enough pictures are skipped.
//
// The I/O processor can be behind the game, and by how much depends on real time. So a run is no longer exactly repeatable, and
// "iopthread 0" in args.txt brings back the old lockstep for picture comparisons.
#include "iop/iop.hpp"
#include "ps2/system.hpp"

#include <3ds.h>

#include <atomic>
#include <cstdio>

namespace ps2 {

namespace {

constexpr std::uint32_t kChunk = 8000;            // EE cycles, about 30 microseconds on the console
constexpr std::uint32_t kMaxLag = 10000000;       // EE cycles, two PS2 frames: past this the game waits
constexpr std::size_t kStack = 1u << 20;
constexpr std::uint32_t kEdges = 32;

struct Edge {
    std::uint32_t at;
    bool start;
};

Thread g_thread = nullptr;
bool g_running = false;
bool g_stop = false;
// A plain spin lock: the game side waits for one short chunk, and a sleeping lock took about 0.3 ms to hand over. g_want stops the
// worker taking it straight back.
std::atomic<bool> g_held{false}, g_want{false};
Handle g_go = 0;
// These count EE cycles and wrap: only their differences mean anything.
std::atomic<std::uint32_t> g_target{0}, g_done{0};
std::atomic<std::uint32_t> g_edge_in{0}, g_edge_out{0};
std::atomic<bool> g_asleep{false};
Edge g_edges[kEdges];
// With "pmu" in args.txt: system ticks the game thread spent waiting for this thread, and this thread spent working.
bool g_timing = false;
u64 g_waited = 0;
volatile u64 g_worked = 0;

struct WaitTimer {
    u64 from = g_timing ? svcGetSystemTick() : 0;
    ~WaitTimer() {
        if (g_timing) g_waited += svcGetSystemTick() - from;
    }
};

void take() {
    for (std::uint32_t tries = 0; g_held.exchange(true, std::memory_order_acquire); ++tries)
        if (tries > 2000) svcSleepThread(50000);   // the system can stop core 1 for a few milliseconds
}

void give() { g_held.store(false, std::memory_order_release); }

void worker_take() {
    while (g_want.load(std::memory_order_relaxed)) {}
    take();
}

void worker(void *) {
    for (;;) {
        const std::uint32_t done = g_done.load(std::memory_order_relaxed);
        const std::uint32_t out = g_edge_out.load(std::memory_order_relaxed);
        const bool edge = out != g_edge_in.load(std::memory_order_acquire);
        const std::uint32_t stop = edge ? g_edges[out % kEdges].at : g_target.load(std::memory_order_acquire);
        if (const std::uint32_t left = stop - done; left != 0 && left < 0x80000000u) {
            const std::uint32_t chunk = left < kChunk ? left : kChunk;
            worker_take();
            const u64 from = g_timing ? svcGetSystemTick() : 0;
            iop::run(chunk);
            if (g_timing) g_worked = g_worked + (svcGetSystemTick() - from);
            give();
            g_done.store(done + chunk, std::memory_order_release);
        } else if (edge) {
            worker_take();
            const u64 from = g_timing ? svcGetSystemTick() : 0;
            iop::vblank(g_edges[out % kEdges].start);
            if (g_timing) g_worked = g_worked + (svcGetSystemTick() - from);
            give();
            g_edge_out.store(out + 1, std::memory_order_release);
        } else {
            g_asleep.store(true);
            // Anything posted before this store is seen by the second look, and anything after it signals.
            const std::uint32_t target = g_target.load(), in = g_edge_in.load();
            if (target == done && in == out) {
                if (g_stop) return;
                svcWaitSynchronization(g_go, U64_MAX);
            }
            g_asleep.store(false);
        }
    }
}

void wake() {
    if (g_asleep.exchange(false)) svcSignalEvent(g_go);
}

}

bool start_iop_thread(int priority, int core) {
    if (R_FAILED(svcCreateEvent(&g_go, RESET_ONESHOT))) return false;
    if (core == 1) APT_SetAppCpuTimeLimit(80);
    g_thread = threadCreate(worker, nullptr, kStack, priority, core, false);
    g_running = g_thread != nullptr;
    return g_running;
}

void stop_iop_thread() {
    if (!g_running) return;
    iop_wait();
    g_stop = true;
    g_running = false;
    svcSignalEvent(g_go);
    threadJoin(g_thread, U64_MAX);
    threadFree(g_thread);
}

bool iop_threaded() { return g_running; }

void iop_timing(bool on) { g_timing = on; }
u64 iop_waited_ticks() { return g_waited; }
u64 iop_worked_ticks() { return g_worked; }

void iop_lock() {
    if (!g_running) return;
    g_want.store(true, std::memory_order_relaxed);
    take();
}

void iop_unlock() {
    if (!g_running) return;
    g_want.store(false, std::memory_order_relaxed);
    give();
}

void iop_advance(std::uint64_t ee_cycles) {
    const std::uint32_t target = g_target.load(std::memory_order_relaxed) + static_cast<std::uint32_t>(ee_cycles);
    g_target.store(target, std::memory_order_release);
    wake();
    if (target - g_done.load(std::memory_order_acquire) <= kMaxLag) return;
    const WaitTimer timer;
    while (target - g_done.load(std::memory_order_acquire) > kMaxLag) svcSleepThread(50000);
}

void iop_edge(bool start) {
    const std::uint32_t in = g_edge_in.load(std::memory_order_relaxed);
    const WaitTimer timer;
    while (in - g_edge_out.load(std::memory_order_acquire) >= kEdges) svcSleepThread(50000);
    g_edges[in % kEdges] = Edge{g_target.load(std::memory_order_relaxed), start};
    g_edge_in.store(in + 1, std::memory_order_release);
    wake();
}

void iop_wait() {
    if (!g_running) return;
    wake();
    const WaitTimer timer;
    while (g_done.load(std::memory_order_acquire) != g_target.load(std::memory_order_relaxed) ||
           g_edge_out.load(std::memory_order_acquire) != g_edge_in.load(std::memory_order_relaxed))
        svcSleepThread(20000);
}

}
