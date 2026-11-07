#pragma once

#include <cstddef>
#include <cstdint>

namespace ps2::gs {

struct State;
struct Vertex;
struct DisplayRegs;

// Draws PS2 GS shapes with the New 3DS GPU (PICA200) instead of the software renderer.
// GS memory stays up to date for uploads, so textures and palettes are read from it.
namespace pica {

bool init();                                     // once, on the thread that will draw
bool active();                                   // true after init(): the GS sends shapes here
void set_present_every(std::uint32_t vblanks);   // show one picture every N PS2 vblanks, 0 to stop showing
void set_shot_every(std::uint32_t frames);       // save the picture every N game frames to sdmc:/p3fes/shots
void set_draw_every(std::uint32_t pictures);     // draw one picture in N; 0 draws only what keeps up with the game, 1 draws all
void set_stats(bool on);                         // print one line of counters per shown picture
void set_dump_frame(std::uint64_t frame);        // print the GS settings of every batch in that game frame
void set_skip_attr(std::uint32_t attr);          // test aid: leave out shapes with these PRIM bits
void set_hide(std::uint32_t from, std::uint32_t to);   // test aid: leave out these batches of each picture
void set_debug(std::uint32_t bits);              // test aid: 1 no depth test, 2 no blending

void draw(const State &s, std::uint32_t serial, std::uint64_t clamp);
std::uint32_t next_picture();   // on the game thread at each PS2 vblank: what the next picture is, passed on to vblank()
void vblank(std::uint32_t frame, const DisplayRegs &regs, std::uint32_t picture);   // a PS2 vblank, with what was on show then
void note_transfer(const State &s, std::uint64_t dir);   // a transfer into GS memory starts
void note_data(const std::uint8_t *data, std::size_t bytes);   // bytes of that transfer
void note_clut();                                        // the palette in g_state.clut was reloaded
void shutdown();

}

}
