#include "TerrainDraws.hpp"

namespace runner {

namespace {

// The upload of view's node (uploading it if new), or null.
const anarchy::amesh::GpuMesh* NodeMesh(const engine_core::TerrainView& view, const engine_core::TerrainNodeView& node,
                                        MeshCache& meshes) {
    if (node.mesh != nullptr) {
        return meshes.getTerrainNode(view.terrain, node.key, *node.mesh, node.revision);
    }
    if (node.compact != nullptr) {
        return meshes.getTerrainNode(view.terrain, node.key, *node.compact, node.revision);
    }
    return nullptr;
}

}  // namespace

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
        // This Terrain's current texture arrays, built
        // a level at a time, smallest first, from view.textures. Layer
        // count 0 (no set published yet, or still building its first pair)
        // selects terrain.frag's flat-color path -- terrain never draws
        // black or vanishes while this is under way.
        unsigned surface[3] = {0, 0, 0};
        int layerCount = 0;
        renderer.terrainArrays(view.terrain, view.textures, surface, layerCount);
        const std::vector<engine_core::TerrainNodeView>& nodes = *view.nodes;
        // Drawn alone (slot 0) and untinted; the Terrain is the instance that draws it.
        const auto push = [&](const engine_core::TerrainNodeView& node, float fade, bool incoming, bool shadowOnly) {
            const anarchy::amesh::GpuMesh* mesh = NodeMesh(view, node, meshes);
            if (mesh == nullptr) {
                return;
            }
            MeshDraw draw;
            draw.mesh = mesh;
            draw.model = view.transform;
            draw.terrainLook = look;
            draw.terrainFade = fade;
            draw.terrainFadeIn = incoming;
            draw.terrainLevel = node.key.level;
            draw.shadowOnly = shadowOnly;
            draw.owner = view.terrain;
            draw.slot = 0;
            draw.terrainSurfaceA = surface[0];
            draw.terrainSurfaceB = surface[1];
            draw.terrainSurfaceC = surface[2];
            draw.terrainLayerCount = layerCount;
            out.push_back(draw);
        };
        SelectTerrainNodes(view, camera, now_seconds, fades, fades.choices);
        for (const NodeChoice& choice : fades.choices) {
            push(nodes[choice.index], choice.fade, choice.incoming, false);
        }
        // R15: terrain out of view still casts, at the selection it would draw
        // at. Those not uploaded yet upload a few a frame (a first frame would
        // otherwise upload the whole Terrain); until then they cast nothing.
        SelectTerrainCasters(view, camera, fades, fades.casters);
        int casterUploads = 0;
        for (const std::size_t index : fades.casters) {
            const engine_core::TerrainNodeView& node = nodes[index];
            if (!meshes.touchTerrainNode(view.terrain, node.key, node.revision)) {
                if (casterUploads >= kTerrainCasterUploads) {
                    continue;
                }
                ++casterUploads;
            }
            push(node, 1.f, true, true);
        }
        // R14: the finer nodes next to switch in, uploaded ahead (a few a frame) and kept.
        SelectTerrainPrefetch(
            view, camera, fades.choices, fades,
            [&](std::size_t index) {
                const engine_core::TerrainNodeView& node = nodes[index];
                return meshes.touchTerrainNode(view.terrain, node.key, node.revision);
            },
            fades.keep, fades.upload);
        for (const std::size_t index : fades.upload) {
            NodeMesh(view, nodes[index], meshes);
        }
    }
    meshes.sweepTerrainNodes(now_seconds);
    renderer.sweepTerrainLooks();
    renderer.sweepTerrainArrays();
    fades.sweep();
}

}  // namespace runner
