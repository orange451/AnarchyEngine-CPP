#include "TerrainDraws.hpp"

namespace runner {

void AppendTerrainDraws(const std::vector<engine_core::TerrainView>& terrains, MeshCache& meshes, Renderer& renderer,
                        std::vector<MeshDraw>& out) {
    for (const engine_core::TerrainView& view : terrains) {
        if (view.look == nullptr || view.chunks == nullptr || view.chunks->empty()) {
            continue;
        }
        const std::uint32_t look = renderer.terrainLookTexture(view.terrain, *view.look);
        if (look == 0) {
            // MakeTerrainLookTexture failed (no GL context, out of texture units, ...).
            // A MeshDraw with terrainLook == 0 draws through the material program as
            // plain white instead of being skipped by it, so skip this Terrain's
            // chunks here rather than push draws it would render wrong.
            continue;
        }
        for (const engine_core::TerrainChunkView& chunk : *view.chunks) {
            if (chunk.mesh == nullptr) {
                continue;
            }
            const anarchy::amesh::GpuMesh* mesh =
                meshes.getTerrainChunk(view.terrain, chunk.coord, *chunk.mesh, chunk.revision);
            if (mesh == nullptr) {
                continue;
            }
            // Drawn alone (slot 0) and untinted; the Terrain is the instance that draws it.
            MeshDraw draw;
            draw.mesh = mesh;
            draw.model = view.transform;
            draw.terrainLook = look;
            draw.owner = view.terrain;
            draw.slot = 0;
            out.push_back(draw);
        }
    }
    meshes.sweepTerrainChunks();
    renderer.sweepTerrainLooks();
}

}  // namespace runner
