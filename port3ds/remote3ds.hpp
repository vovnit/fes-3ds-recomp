#pragma once

#include <cstdint>

// Wireless test link: anyone on the same network can read logs and crash reports, press buttons, replace the game and
// restart it, so a test needs no SD card trip. See tools/p3fes_3ds_link.py. It works on the main thread, between vblanks.
namespace remote {

void start();                                     // after the SD card is ready; a failure only leaves the link off
void poll(std::uint64_t frame);                   // once per vblank on the main thread
void stop();
bool wants_exit();                                // a QUIT or RUN came in
void chain_restart();                             // after RUN: the next start of this title loads the game again
std::uint16_t buttons(std::uint64_t frame);       // PS2 pad bits pressed from the Mac

}
