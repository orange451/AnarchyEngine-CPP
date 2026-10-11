#pragma once

#include "MeshCache.hpp"
#include "Renderer.hpp"
#include "SnapshotPump.hpp"
#include "TextureCache.hpp"

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

// brushes' draws, each mesh loaded through meshes; each draws alone (slot 0).
void AppendBrushDraws(const std::vector<engine_core::VisualBrushDraw>& brushes, MeshCache& meshes,
                      TextureCache& textures, std::vector<MeshDraw>& out);

// sky's SceneSky, its image loaded through textures. Leaves out alone when
// sky is not present.
void SkyFor(const engine_core::VisualSky& sky, TextureCache& textures, SceneSky& out);

}  // namespace runner
