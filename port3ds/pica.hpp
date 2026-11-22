#pragma once

#include <cstddef>
#include <cstdint>

namespace g3 { struct Gpu; struct Input; struct BatchPlan; }

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
void set_stereo(std::uint32_t strength);         // how deep the picture looks with the 3D slider up, 0 (flat) to 10; 5 to begin with
std::uint32_t stereo();
std::uint32_t pictures_shown();                  // pictures put on the screen so far (for the speed log)

void draw(const State &s, std::uint32_t serial, std::uint64_t clamp);
// A batch of the game's model programs, transformed and lit by the vertex shader (port3ds/g3native.cpp, "g3 4"). PRIM must
// already hold the batch's own (its GIF tag has gone through the GIF). False: the frame's command list is nearly full, send the batch the GS way instead.
// g3_ready: init found the shader's g3 uniforms where draw_g3 expects them.
bool g3_ready();
bool draw_g3(const State &s, std::uint64_t clamp, const ::g3::Gpu &g);
int draw_g3_direct(const State &s, std::uint64_t clamp, const ::g3::Input &in, ::g3::Gpu &g, const ::g3::BatchPlan &plan);
bool draw_im2d_tris(const State &s, std::uint64_t clamp, const std::uint8_t *vu_mem, std::uint32_t top, std::uint32_t count, bool xyzf);
extern bool g_after_hidden;      // set by next_picture(): the picture after the one it chose will be skipped
extern bool g_render_left_out;   // set by lossy_return (native3ds.cpp): next_picture() must not show the picture being built
std::uint32_t next_picture();   // on the game thread at each PS2 vblank: what the next picture is, passed on to vblank()
void advance_vu_picture(std::uint32_t picture);
void vblank(std::uint32_t frame, const DisplayRegs &regs, std::uint32_t picture);   // a PS2 vblank, with what was on show then
void note_transfer(const State &s, std::uint64_t dir);   // a transfer into GS memory starts
void note_data(const std::uint8_t *data, std::size_t bytes, std::uint32_t known_hash = 0);   // bytes of that transfer
void note_clut();                                        // the palette in g_state.clut was reloaded
void shutdown();

}

}
