#pragma once

// A very small profiler for the 3DS build: PS2_PROF(ps2::prof::Vu); at the top of a function adds its time to that counter.
// pica.cpp prints the counters with "stats 1". Spu is part of Iop and Gif is part of Vu; drawing is inside Gif. Whatever is left
// over is the translated game code.

#include <atomic>
#include <cstdint>

extern "C" std::uint64_t svcGetSystemTick(void);

namespace ps2::prof {

// Wait is the VU thread finding nothing to do, Full is the EE thread finding the queue to it full.
enum Counter { Vu, Gif, Upload, Draw, Iop, Spu, Ipu, Vif, Dma, Events, Kernel, Present, Wait, Full, Count };
// Cumulative counters. Each reader takes its own unsigned delta across wraparound.
extern std::atomic<std::uint32_t> g_ticks[Count];

#ifdef PS2_PROF_ALL
// With "pmu A B", the two hardware events on the game thread by activity, each nested activity apart from the one around it.
// g_events[Count] is whatever ran outside every activity: the translated game code. The kExtra buckets after it are for a
// throwaway split of something: PS2_PROF_EXTRA(k) at the top of a block counts that block in bucket k, printed as x<k>.
inline constexpr int kExtra = 10;
extern std::uint64_t g_events[Count + 1 + kExtra][2];
extern int g_events_at;
extern void *g_game_thread;
void switch_events(int to);   // main3ds.cpp
inline bool on_game_thread() {
    void *tls;
    asm("mrc p15, 0, %0, c13, c0, 3" : "=r"(tls));
    return tls == g_game_thread;
}
struct Extra {
    int outer = -1;
    explicit Extra(int k) {
        if (on_game_thread()) {
            outer = g_events_at;
            switch_events(Count + 1 + k);
        }
    }
    ~Extra() {
        if (outer >= 0) switch_events(outer);
    }
};
#endif

// Reading the clock is a system call, and thousands of these run per game frame. So a normal build times only the two waits, which
// choose how many pictures to skip; build with PROF=1 for the rest.
template <Counter C>
struct Scope {
#ifdef PS2_PROF_ALL
    static constexpr bool kTimed = true;
#else
    static constexpr bool kTimed = C == Wait || C == Full;
#endif
    std::uint64_t start;
#ifdef PS2_PROF_ALL
    int outer = -1;
#endif
    Scope() {
        if constexpr (kTimed) start = svcGetSystemTick();
#ifdef PS2_PROF_ALL
        if (on_game_thread()) {
            outer = g_events_at;
            switch_events(C);
        }
#endif
    }
    ~Scope() {
        if constexpr (kTimed)
            g_ticks[C].fetch_add(static_cast<std::uint32_t>(svcGetSystemTick() - start), std::memory_order_relaxed);
#ifdef PS2_PROF_ALL
        if (outer >= 0) switch_events(outer);
#endif
    }
};

}

#define PS2_PROF(counter) ps2::prof::Scope<counter> ps2_prof_scope
#ifdef PS2_PROF_ALL
#define PS2_PROF_EXTRA(k) ps2::prof::Extra ps2_prof_extra(k)
#else
#define PS2_PROF_EXTRA(k)
#endif
