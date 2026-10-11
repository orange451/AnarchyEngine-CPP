#pragma once

#include "MeshCache.hpp"
#include "Renderer.hpp"
#include "SnapshotPump.hpp"
#include "TextureCache.hpp"
#include "ViewportQueue.hpp"
#include "types.hpp"

#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

namespace jadefx {
class UiRenderer;
}

namespace runner {

// Draws a host's ViewportPanes, each into a texture of its own, and composites
// them as their nodes paint. A host is a Scene View or a plugin pane, one GL
// context each.
// - A Renderer of its own, never the host's, draws them. Output is
//   transparent, shadows are off, targets are pooled, and up to three skies
//   keep their cubes.
// - The host's MeshCache and TextureCache are used when it has them, so an
//   asset in the world and in a pane is uploaded once.
// - ViewportQueue decides which panes draw each frame. A pane is drawn from its
//   own Camera, lit by its own lights and Ambient, with a new Lighting's tone
//   settings.
// - renderPending runs in the host's paint before its GUI. composite runs as
//   each pane's node paints.
// - Every call but the constructor, setScene, setResourcesRoot, and the
//   destructor needs the host's GL context current. release deletes
//   everything in it.
class ViewportRenderer {
public:
    // meshes and textures: the host's caches, or null to own a pair.
    ViewportRenderer(MeshCache* meshes, TextureCache* textures);
    // Needs no GL: one still holding names is one whose context is gone.
    ~ViewportRenderer();
    ViewportRenderer(const ViewportRenderer&) = delete;
    ViewportRenderer& operator=(const ViewportRenderer&) = delete;

    void setScene(engine_core::InstanceId pane, std::shared_ptr<const engine_core::VisualViewport> scene);
    // The folder an owned pair of caches loads from. A host's own caches keep theirs.
    void setResourcesRoot(const std::filesystem::path& root);
    // Paint: pane's last image over x, y, width, height (window points), at
    // opacity, and notes the pane shown at its pixel size.
    void composite(jadefx::UiRenderer& ui, engine_core::InstanceId pane, double x, double y, double width,
                   double height, float opacity);
    // The host's paint, before its GUI, with no clip or occluder: renders the panes due this frame.
    void renderPending();
    // The host's sceneChanged, with the old context current: deletes every GL object.
    void release();
    // For checks: renders job's pane now, whatever the budget.
    ViewportQueue::Result renderNow(const ViewportQueue::Job& job);
    // For checks: pane's texture as last drawn, top row first, premultiplied
    // RGBA. False when it has none.
    bool read(engine_core::InstanceId pane, ViewPixels& out);

private:
    struct Target {
        unsigned framebuffer = 0;
        unsigned color = 0;
        int width = 0;
        int height = 0;
    };

    bool ensureGraphics();
    // pane's target at width by height, made or remade. Null when the driver refuses it.
    Target* ensureTarget(engine_core::InstanceId pane, int width, int height);
    void deleteTarget(Target& target);

    Renderer renderer_;
    std::unique_ptr<MeshCache> ownMeshes_;
    std::unique_ptr<TextureCache> ownTextures_;
    MeshCache* meshes_;
    TextureCache* textures_;
    ViewportQueue queue_;
    std::unordered_map<engine_core::InstanceId, Target> targets_;
    std::vector<engine_core::InstanceId> forgotten_;
    std::vector<std::vector<MeshDraw>> prefabDraws_;
    std::vector<MeshDraw> draws_;
    std::vector<LightDraw> lights_;
    bool attempted_ = false;
    bool ready_ = false;
};

}  // namespace runner
