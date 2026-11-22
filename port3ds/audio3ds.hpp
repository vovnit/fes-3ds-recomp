// 3DS sound output: receives 48 kHz stereo 16-bit PCM frames from SPU2 (on core 1)
// and streams them to the hardware DSP via NDSP (with a CSND fallback if dspfirm.cdc is missing).
#pragma once

#include <cstddef>
#include <cstdint>

namespace audio3ds {

bool start();
void stop(bool clear_sink = true);
void poll();   // called once per vblank on the main thread (only needed for CSND fallback)
void push(const std::int16_t *samples, std::size_t frames);
void report(double seconds);   // logs what came in and how often the DSP ran dry since the last call

}
