#pragma once

#include "MeshCache.hpp"
#include "Renderer.hpp"
#include "TerrainWorld.hpp"

#include <vector>

namespace runner {

// Appends a MeshDraw to out for each meshed chunk of each Terrain in
// terrains (a VisualSnapshot's): the chunk's upload from meshes, at the
// Terrain's Transform, drawn with its look table from renderer. Then sweeps
// both caches, so chunks and looks no Terrain shows any more are deleted.
// Once a frame, on RenderThread with the GL context current. The views are
// the snapshot's immutable copies, so it takes no DataModel lock. GameView's
// collectMeshes calls it, and scene-render-check, to draw what the Scene View draws.
void AppendTerrainDraws(const std::vector<engine_core::TerrainView>& terrains, MeshCache& meshes, Renderer& renderer,
                        std::vector<MeshDraw>& out);

}  // namespace runner
