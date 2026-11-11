// What the parts of the iOS front end share: the PS2 pad bits and the state of the on-screen controls.
#pragma once

#include <atomic>
#include <cstdint>

namespace fe {

enum Button : std::uint16_t {
    kSelect = 1u << 0, kL3 = 1u << 1, kR3 = 1u << 2, kStart = 1u << 3,
    kUp = 1u << 4, kRight = 1u << 5, kDown = 1u << 6, kLeft = 1u << 7,
    kL2 = 1u << 8, kR2 = 1u << 9, kL1 = 1u << 10, kR1 = 1u << 11,
    kTriangle = 1u << 12, kCircle = 1u << 13, kCross = 1u << 14, kSquare = 1u << 15,
};

// written by the touch controls (main thread), read where the pad is polled
extern std::atomic<std::uint16_t> g_touch_buttons;
extern std::atomic<std::uint16_t> g_touch_stick;   // left stick: x in the low byte, y in the high byte, 0x8080 when centred

}
