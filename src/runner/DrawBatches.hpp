#pragma once

#include "Matrix4.hpp"
#include "Visibility.hpp"

#include <cstdint>
#include <vector>

namespace runner {

// One instance as vertex slots 7 to 15 read it (amesh.hpp's kAttribInstance*),
// 116 bytes with no padding.
struct InstanceData {
    // World space, column-major: slots 7 to 10.
    float model[16];
    // The inverse transpose of model's 3 by 3, column-major, or zeros when it
    // has no inverse: slots 11 to 13.
    float normal[9];
    // The GameObject's Color made linear, as surface.glsl's toLinear: slot 14.
    float tint[3];
    // The first texel of its skinning matrices in the bone texture, or -1 for
    // an unskinned instance: slot 15. A float, which holds every texel index
    // the texture can have exactly.
    float boneBase;
};
static_assert(sizeof(InstanceData) == 116, "vertex slots 7 to 15 read 116 bytes per instance");

// Instances first to first + count, all drawn with MeshDraw draw's mesh and Material.
struct DrawRun {
    int first = 0;
    int count = 0;
    int draw = 0;
    std::uint8_t lod = 0;
    // Its models turn the mesh inside out, so front faces are culled instead of back.
    bool mirrored = false;
};

// One frame's runs, reused frame to frame so it allocates only while it grows.
struct DrawBatches {
    // The opaque runs, then one run of 1 per see-through draw, farthest first.
    std::vector<DrawRun> runs;
    std::vector<InstanceData> instances;
    int opaqueRuns = 0;

    // Sort scratch, kept between frames.
    struct Entry {
        std::uint64_t key = 0;
        float depth = 0.f;
        int index = 0;
        std::uint8_t lod = 0;
    };
    std::vector<Entry> order;
};

// Sorts visible's opaque draws by BatchKey, nearest first within a key (a
// terrain chunk by the middle of its own mesh box, as one Terrain's chunks
// share a model; anything else by its model's origin), into
// runs; a slot 0 draw or a terrain chunk is always a run of its own, and
// terrain chunks come after every other opaque run. Then each see-through draw,
// farthest first by its Transform's depth in view, as a run of 1.
void BuildBatches(const DrawItem* items, const VisibilityResult& visible, const engine_core::Matrix4& view,
                  DrawBatches& out);

// slot << 32 | lod << 1 | mirrored.
std::uint64_t BatchKey(std::uint32_t slot, std::uint8_t lod, bool mirrored);
// Whether model's 3 by 3 has a negative determinant.
bool Mirrored(const engine_core::Matrix4& model);
// The inverse transpose of model's 3 by 3, column-major; zeros when it has no inverse.
void NormalMatrix(const engine_core::Matrix4& model, float out[9]);

}  // namespace runner
