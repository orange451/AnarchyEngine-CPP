#pragma once

#include "MeshCache.hpp"
#include "Renderer.hpp"
#include "SnapshotPump.hpp"
#include "TextureCache.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

// What a snapshot draws, as Renderer takes it: shared by the Scene View and
// the ViewportPanes, so a GameObject draws the same in either.
namespace runner {

// prefab's Models as MeshDraw templates: each mesh and texture loaded through
// the caches, Material values copied, slots numbered from nextSlot (which
// moves on). A Model whose mesh does not load adds nothing.
void LoadPrefabDraws(const engine_core::VisualPrefab& prefab, MeshCache& meshes, TextureCache& textures,
                     std::uint32_t& nextSlot, std::vector<MeshDraw>& out);

// row's light, when the row is an enabled light; false otherwise.
bool LightDrawFor(const engine_core::VisualInstance& row, LightDraw& out);

// Appends row drawn with templates: scaled by its Scale, tinted by its Color,
// faded by its Transparency, and posed by its Pose. The row's pose must
// outlive the draws.
void AppendRowDraws(const engine_core::VisualInstance& row, const std::vector<MeshDraw>& templates,
                    std::vector<MeshDraw>& out);

// AppendRowDraws's draws written from out on, one per template; returns
// where they end.
MeshDraw* WriteRowDraws(const engine_core::VisualInstance& row, const std::vector<MeshDraw>& templates,
                        MeshDraw* out);

// Every field of a and b the same, bit for bit.
bool SameDraw(const MeshDraw& a, const MeshDraw& b);

// A snapshot's row draws, kept from frame to frame. What update leaves in
// draws is what one loop over the rows makes: each live row's draws
// (AppendRowDraws, with templates[row.prefab]; a row whose prefab is 0 or past
// templates draws none), in row order, and each one's light (LightDrawFor).
// But rows come in chunks (VisualRows::chunk), and a chunk that is the very
// one drawn last time, with the same templates, keeps its draws where they
// are: with a hundred thousand rows holding still, a frame draws almost
// nothing again. The chunks it does draw again run on worker threads.
// Templates are the same when every draw is (SameDraw) and each mesh holds the
// same upload (GpuMesh::generation): a mesh uploaded again in place draws
// every row again.
class RowDrawCache {
public:
    // draws must be the vector given last time, with what update left in it
    // first; whatever was appended after is dropped. lights gets every row's
    // light appended. written becomes the ranges of draws written this time,
    // in order: everything else in draws is as the last update left it.
    void update(const engine_core::VisualRows& rows, const std::vector<std::vector<MeshDraw>>& templates,
                std::vector<MeshDraw>& draws, std::vector<LightDraw>& lights, std::vector<DrawRange>& written);
    // Forgets what was drawn: the next update draws every row again.
    void clear();

private:
    struct Chunk {
        // The rows drawn, kept so no other rows can be stored at that address meanwhile.
        std::shared_ptr<const engine_core::VisualRows::Chunk> rows;
        // Its draws in draws, and its rows' lights.
        std::size_t first = 0;
        std::size_t count = 0;
        std::vector<LightDraw> lights;
    };
    std::vector<Chunk> chunks_;
    // The templates the kept draws were made with, and each one's mesh's
    // generation then (0 for none).
    std::vector<std::vector<MeshDraw>> templates_;
    std::vector<std::vector<std::uint64_t>> generations_;
    // How many draws the last update left.
    std::size_t drawn_ = 0;
    // Scratch: which chunks this update counts again, and which it writes.
    std::vector<std::size_t> recount_;
    std::vector<std::size_t> rewrite_;
};

// brushes' draws, each mesh loaded through meshes; each draws alone (slot 0).
void AppendBrushDraws(const std::vector<engine_core::VisualBrushDraw>& brushes, MeshCache& meshes,
                      TextureCache& textures, std::vector<MeshDraw>& out);

// sky's SceneSky, its image loaded through textures. Leaves out alone when
// sky is not present.
void SkyFor(const engine_core::VisualSky& sky, TextureCache& textures, SceneSky& out);

}  // namespace runner
