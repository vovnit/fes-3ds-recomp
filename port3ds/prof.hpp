#pragma once

// A very small profiler for the 3DS build: PS2_PROF(ps2::prof::Vu); at the top of a function adds its time to that counter.
// pica.cpp prints the counters with "stats 1". Spu is part of Iop and Gif is part of Vu; drawing is inside Gif. Whatever is left
// over is the translated game code.

#include <cstdint>

extern "C" std::uint64_t svcGetSystemTick(void);

namespace ps2::prof {

// Wait is the VU thread finding nothing to do, Full is the EE thread finding the queue to it full.
enum Counter { Vu, Gif, Upload, Draw, Iop, Spu, Ipu, Vif, Dma, Events, Kernel, Present, Wait, Full, Count };
extern std::uint64_t g_ticks[Count];

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
    Scope() {
        if constexpr (kTimed) start = svcGetSystemTick();
    }
    ~Scope() {
        if constexpr (kTimed) g_ticks[C] += svcGetSystemTick() - start;
    }
};

}

#define PS2_PROF(counter) ps2::prof::Scope<counter> ps2_prof_scope
