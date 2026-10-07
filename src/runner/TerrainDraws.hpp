#pragma once

#include "MeshCache.hpp"
#include "Renderer.hpp"
#include "TerrainSelection.hpp"
#include "TerrainWorld.hpp"

#include <vector>

namespace runner {

// Appends a MeshDraw for each LOD node SelectTerrainNodes chooses of each
// Terrain in terrains (a VisualSnapshot's), seen from camera at now_seconds
// (any origin, never decreasing): the node's upload from meshes, at the
// Terrain's Transform, drawn with its look table from renderer, with its fade
// (MeshDraw::terrainFade, terrainFadeIn). fades carries the cross-fades from
// frame to frame. Then sweeps: node uploads not drawn for
// MeshCache::kTerrainNodeGraceSeconds, looks no Terrain shows, and fades of
// Terrains gone. Once a frame, on RenderThread with the GL context current.
// The views are the snapshot's immutable copies, so it takes no DataModel
// lock. GameView's collectMeshes calls it, and scene-render-check, to draw
// what the Scene View draws.
void AppendTerrainDraws(const std::vector<engine_core::TerrainView>& terrains, const TerrainCamera& camera,
                        double now_seconds, TerrainFadeState& fades, MeshCache& meshes, Renderer& renderer,
                        std::vector<MeshDraw>& out);

}  // namespace runner
