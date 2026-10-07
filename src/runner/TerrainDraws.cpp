#include "TerrainDraws.hpp"

namespace runner {

void AppendTerrainDraws(const std::vector<engine_core::TerrainView>& terrains, const TerrainCamera& camera,
                        double now_seconds, TerrainFadeState& fades, MeshCache& meshes, Renderer& renderer,
                        std::vector<MeshDraw>& out) {
    for (const engine_core::TerrainView& view : terrains) {
        if (view.look == nullptr || view.nodes == nullptr || view.nodes->empty()) {
            continue;
        }
        const std::uint32_t look = renderer.terrainLookTexture(view.terrain, *view.look);
        if (look == 0) {
            // MakeTerrainLookTexture failed (no GL context, out of texture units, ...).
            // A MeshDraw with terrainLook == 0 draws through the material program as
            // plain white instead of being skipped by it, so skip this Terrain's
            // nodes here rather than push draws it would render wrong.
            continue;
        }
        SelectTerrainNodes(view, camera, now_seconds, fades, fades.choices);
        for (const NodeChoice& choice : fades.choices) {
            const engine_core::TerrainNodeView& node = (*view.nodes)[choice.index];
            const anarchy::amesh::GpuMesh* mesh = nullptr;
            if (node.mesh != nullptr) {
                mesh = meshes.getTerrainNode(view.terrain, node.key, *node.mesh, node.revision);
            } else if (node.compact != nullptr) {
                mesh = meshes.getTerrainNode(view.terrain, node.key, *node.compact, node.revision);
            }
            if (mesh == nullptr) {
                continue;
            }
            // Drawn alone (slot 0) and untinted; the Terrain is the instance that draws it.
            MeshDraw draw;
            draw.mesh = mesh;
            draw.model = view.transform;
            draw.terrainLook = look;
            draw.terrainFade = choice.fade;
            draw.terrainFadeIn = choice.incoming;
            draw.owner = view.terrain;
            draw.slot = 0;
            out.push_back(draw);
        }
    }
    meshes.sweepTerrainNodes(now_seconds);
    renderer.sweepTerrainLooks();
    fades.sweep();
}

}  // namespace runner
