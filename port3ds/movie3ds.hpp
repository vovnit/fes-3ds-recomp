// The 3DS build's own movie pictures. See tools/p3fes_3ds_movies.py for the files and the idea.
#pragma once

#include <cstddef>
#include <cstdint>

#include <citro3d.h>

namespace movie {

// Reads sdmc:/p3fes/movies/movies.cut and starts the decoder. False when there are no movie files: the game's own then play.
bool start(int priority, int core);
void stop();

// After a read from the disc image: sectors of a movie that has its 3DS copy become the cheap movie.
void disc_read(std::uint64_t offset, std::uint8_t *data, std::size_t size);

// The IPU asks this for each macroblock it is about to convert to RGB, with its number in the command and the command's
// count. It reads the marker from the top row; true means the conversion can be left out, because the picture will not be seen.
bool watch(const std::uint8_t *raw, unsigned index, unsigned count);

// On the drawing thread. The four textures the pictures go through take 1 MB of linear memory, which the lobby's own textures
// need, so they exist only while a movie plays and for two seconds after.
struct Picture {
    bool playing;    // a movie of ours is on: the game's own picture must not be shown
    C3D_Tex *tex;    // what to show, 400x240 in the corner the game's textures start from; null while the first picture is on its way
};
Picture picture(bool (*free_a_texture)());   // free_a_texture gives up one of the game's textures; false when none is left

}
