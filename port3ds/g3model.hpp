// The g3 batch model: what one MSCNT batch of the game's g3_generic (042) or g3_skin (064) VU1 program computes, from
// VU1 data memory and the registers the program's init left. Plain C++ with no 3DS parts, so tools/g3check can run it
// on a computer against batches captured from the game (port3ds/g3native.cpp, "g3 3").
#pragma once

#include <cstdint>
#include <vector>

namespace ps2 { struct Vu0Regs; }

namespace g3 {

struct DirectStream {
    const std::uint8_t *src;
    std::uint16_t num;
    std::uint8_t vn, vl;
    bool usn;
};

struct DirectStreams {
    std::uint16_t top;
    std::uint8_t stride;
    std::uint8_t mask;
    DirectStream s[5];
};

struct Input {
    int program;                 // 42 or 64
    const std::uint8_t *mem;     // VU1 data memory, 16 KB, as the batch finds it
    std::uint32_t top, itop;     // the VIF's TOP and ITOP for this batch
    const std::uint8_t *vf;      // borrowed bytes of the 32 VU1 float registers for this call
    const DirectStreams *streams = nullptr;   // optional deferred VIF1 vertex streams (skip g_vu1_mem unpack)
};

// How the batch ended, for leave_state.
struct End {
    bool swap = true;        // the output buffers vi13/vi14 changed places (not when the clipper culled the whole batch)
    bool clipper = false;    // the full clipper ran: it leaves vf28-30.z at w - w
};

// Writes the GIF packets the program would XGKICK to out from its start (out grows when too small and is never shrunk, so
// keep it between calls); ends gets each packet's end offset, the last being the bytes used (none when nothing is drawn).
// False: not modelled, run the program instead.
bool run(const Input &in, std::vector<std::uint8_t> &out, std::vector<std::uint32_t> &ends, End &end);

// The MSCAL 0 init (042/064 L_0000-L_0190): computes vf28-vf31, vf2, vi13, vi14 and writes the batch jump target to M[0x3FF].y.
void init_state(int program, std::uint8_t *mem, ps2::Vu0Regs &r);

// The registers a later batch of the program reads, set as the program would have left them.
void leave_state(int program, const End &end, ps2::Vu0Regs &r);

// The GPU path (pica::draw_g3 and the g3 half of port3ds/gs.v.pica): the batch's own vertices, and what the vertex shader
// needs to do the program's work on them. Close to the program's pictures, not exact: the GPU's floats have 24 bits.
struct GpuVertex {
    float x, y, z, u, v;
    std::uint8_t colour[4];      // prelit, 0-255
    std::int8_t normal[4];       // in 1/128 units
    float weight[4];
    std::uint8_t bone[4];        // each weight's bone: its first vector in Gpu::bone
};
static_assert(sizeof(GpuVertex) == 48);

constexpr unsigned kGpuBones = 24;   // the shader's bone uniforms (3 vectors each)

struct Gpu {
    std::uint64_t prim;              // what the GIF tag sets PRIM to
    bool skin;                       // g3_skin: the shader blends bones (g3_generic's bone is the identity, for g3check)
    bool ortho;                      // g3_skin_ortho: screen-space r1.xyz (no perspective divide), linear fog from r1.w
    bool fog;                        // the XYZF2 strip: the fog value from w, and z without the 16 of FTOI4
    bool xyzf;                       // z as XYZF2 takes it (24 bits, without the 16 of XYZ2): the fog strip and lists
    bool clipper;                    // full clipper batch: let PICA200 near-clip instead of clamping z >= -w
    bool keep_negative, keep_positive;   // which triangles the program leaves without ADC, by facing (see facing_negative)
    float proj[4][4];                // vf28-vf31 as the strip uses them (the z of vf28 and vf29 is not used: zero)
    float offset[4];                 // vf02, added to the screen position
    float fog_scale, fog_near;       // 0x3F6 z and w: the fog value is fog_scale * (w - fog_near), clamped to 0-255
    float material[4];               // 0x3FB
    float ambient[4];                // the ambient lights' sum (x, y, z)
    float light_dir[3][4];           // up to three directional lights, unused ones zero: N.L is dot(normal, light_dir)
    float light_colour[3][4];
    // The bone palette, kept between batches (so keep g too): each bone's matrix as 3 vectors, x, y and z each a dot product
    // with (position, 1). A bone keeps its slot while batches go on using it, so the shader's copy rarely needs sending.
    float bone[kGpuBones * 3][4];
    std::uint32_t bone_gen[kGpuBones]; // generation per slot, bumped when bone[3*s..3*s+2] changes
    std::uint32_t bone_nonfinite = 0u; // bit per slot set when any word in bone[3*s..3*s+2] is Inf/NaN
    std::uint32_t slots;             // the palette slots this batch's vertices use (bit per slot)
    // What ambient, light_dir and light_colour were worked out from: light_words words of VU memory from 0x3D0 on (0: nothing yet)
    // and M[0x3FC]'s x and z. Batches of one model share it.
    unsigned light_words = 0;
    std::uint32_t light_x, light_z, light_mem[64];
    bool has_dir_lights = false;     // true when at least one directional light is active
    unsigned n;
    std::uint8_t alpha_min, alpha_max;   // prelit vertex alpha range (0-255) across the batch
    GpuVertex v[512];                // a list batch is turned into a strip, up to 2n - 3 vertices (see gpu_batch)
    std::uint64_t prim2;             // Pass 2 PRIM for dual-texture / EnvMap MatFX
    float uv2[256][2];               // Pass 2 transformed UVs for dual-texture MatFX (program 43/44)
    bool env = false;                // Pass 2 EnvMap active: shader computes UV from normal via env_u/env_v
    float env_u[4]{}, env_v[4]{};    // EnvMap UV projection rows: u = dot(env_u.xyz, n) + env_u.w, v = dot(env_v.xyz, n) + env_v.w
    float env_coeff = 1.0f;          // EnvMap pass-2 RGB scale (M[965].w)
};

struct BatchPlan {
    bool skin, dual, dual_uv1, dual_uv2, env, uv_xform, list, direct, lights, normals;
    std::uint32_t top, n, pos_w, uv_w, nrm_step;
};

// Why gpu_batch did not give a batch to draw.
enum GpuDecline : int {
    kGpuNotModelled = 1,   // what run() declines too
    kGpuBoneCount,         // more bones than the shader holds
    kGpuLights,            // more than three directional lights
    kGpuRange,             // a colour beyond 0-255 or a normal beyond a signed byte
    kGpuPrim,              // not a Gouraud triangle strip with ST coordinates
    kGpuClipDepth,         // the clipper batch has a vertex beyond its near or far plane
    kGpuDeclines
};

// Whether a clipper batch with vertices behind the camera may go to the GPU, which clips them itself. Not in stereo: the shader
// shifts each vertex by its depth, limited near the camera, and between a far vertex and one behind the camera that is not a line.
extern bool g_behind_on_gpu;

// Two-phase GPU batch API for zero-copy direct-to-ring emission:
// gpu_batch_setup validates the batch and fills Gpu headers (everything except v[]/slots/alpha);
// gpu_batch_fill writes vertices directly to dst (e.g. the PICA vertex ring).
int gpu_batch_setup(const Input &in, Gpu &g, End &end, BatchPlan &plan);
int gpu_batch_fill(const Input &in, Gpu &g, const BatchPlan &plan, GpuVertex *dst);

// For the converted-vertex cache: a skin batch's vertices name palette slots, so a kept batch needs its bones back in the
// same slots. last_bones gives the bones the last gpu_batch_fill of a skin batch met (VU address and slot, in the order
// first met; -1 when there were more than kMaxBoneUses). restore_bones puts each bone's matrix from VU memory into its slot
// (a new generation where it changed) and sets g.slots; false when two bones that shared a slot now differ.
constexpr unsigned kMaxBoneUses = 24;
struct BoneUse {
    std::uint16_t address;
    std::uint8_t slot;
};
int last_bones(const BoneUse *&uses);
bool restore_bones(const std::uint8_t *mem, const BoneUse *uses, int count, Gpu &g);

// 0: g filled, draw it. -1: the program draws nothing (end says how it ended, as run()'s). -2: deferred streams did not match, flush unpacks and retry. Otherwise a GpuDecline.
int gpu_batch(const Input &in, Gpu &g, End &end);

}
