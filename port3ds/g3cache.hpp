// Converted-vertex cache for the g3 GPU path (pica::draw_g3_direct). A batch whose vertex streams all come straight from
// PS2 RAM (models the game sends by reference) is converted once into the arena, a block of GPU-visible memory, and drawn
// from there by index as long as the same streams come again: about nine in ten of the vertices in the lobby and the first
// battle, and in those scenes the game never changed the data behind a stream it sent again.
//
// Plain C++ with no 3DS parts; pica.cpp owns the memory, the indices and the drawing.
#pragma once

#include "g3model.hpp"

#include <cstdint>

namespace g3cache {

extern int g_mode;   // "g3cache N": 0 off, 1 on, 2 on and report every 600 frames
extern int g_skin;    // "g3skin N": skinned batches too, 1 (the default) or 0
// "g3index N", how batches from the arena are drawn. 0 (the default): each on its own by vertex range (pica.cpp,
// G3Pending::arrays). 1: unskinned ones joined into strips by index. 2: skinned ones by index too. Tests only: on the
// console (2026-11-22) skinned vertices drawn by index hung the GPU at the first picture every time (until it was switched
// off), the arena flushed whole or not, and a cold boot with 1 hung it once; by vertex range they run. Azahar draws all fine.
extern int g_index;

// The arena: capacity vertices at arena (at most 65536, for 16-bit indices).
void init(g3::GpuVertex *arena, std::uint32_t capacity);
bool ready();

// A new picture is being recorded: entries drawn in this one or the one before (which the GPU may still be reading) stay.
void begin_picture();

// Where the batch's g.n vertices are in the arena, converting them there first when they are not kept: 0 or more is the
// first vertex; -1 means not here (no key, or no room), draw it the usual way. On a conversion, fill_rc gets
// gpu_batch_fill's result (the arena space is given back when it is not 0). g's alpha fields are set as the fill sets them.
int place(const g3::Input &in, const g3::BatchPlan &plan, g3::Gpu &g, int &fill_rc);

// The range of the arena written since the last call, for the data cache flush (lo == hi: none).
void take_dirty(std::uint32_t &lo, std::uint32_t &hi);

}
