#pragma once

#include "ShadowPlanner.hpp"

#include <cstdint>
#include <vector>

namespace runner {

struct MeshDraw;

// What shadow.glsl reads for one light.
struct ShadowLookup {
    // As shadow.glsl's uShadowKind.
    enum Kind : int { kNone = 0, kSpot = 1, kPoint = 2, kCascades = 3 };
    int kind = kNone;
    // World to clip space: a SpotLight's, or each cascade's.
    engine_core::Matrix4 matrices[kMaxCascades] = {};
    int cascades = 0;
    // A PointLight's or SpotLight's world position, near plane, and Radius.
    float light[3] = {0.f, 0.f, 0.f};
    float nearFar[2] = {0.f, 1.f};
    // World units across a texel: [0] per unit of distance from a PointLight
    // or SpotLight; one per cascade for a DirectionalLight.
    float texel[4] = {0.f, 0.f, 0.f, 0.f};
    // One texel of the texture it reads, in that texture's 0 to 1 coordinates.
    float texelUv = 0.f;
    // A SpotLight's tile ([0]) or a PointLight's six: corner x and y, and
    // size, in a page's 0 to 1 coordinates, then the page.
    float tiles[6][4] = {};
    float faceScale = 1.f;
};

// Draws the shadow maps ShadowPlanner picks each frame into one atlas, a
// 2D array of 24-bit depth whose layers are the atlas's pages, made again
// whenever the planner grows the page or adds one. Run before the
// G-buffer: it leaves the shadow framebuffer bound and the viewport,
// scissor, and depth state changed.
class ShadowRenderer {
public:
    // The GL context has to be current. False when the shadow shader does not build.
    bool initialize();
    void shutdown();

    // Draws this frame's due tiles for requests, casting from meshes. False,
    // having committed nothing, when the program cannot draw yet (macOS). A
    // driver that will not draw into the atlas gets every light unshadowed,
    // said once.
    bool draw(const std::vector<ShadowRequest>& requests, const MeshDraw* meshes, int count, const CameraView& camera,
              const ShadowSettings& settings);
    // How the light pass reads key's map: kNone before it has one, or once
    // the driver has refused to draw shadow maps.
    ShadowLookup lookup(std::uint64_t key) const;
    // Draws the sun's cascades when they changed, after draw in the same
    // frame (it casts from the same meshes). sun null draws none.
    bool drawSun(const SunRequest* sun, const MeshDraw* meshes, const CameraView& camera,
                 const ShadowSettings& settings);
    // How the light pass reads the sun's cascades: kNone without them, or once refused.
    ShadowLookup sunLookup() const;

    // The textures, both 2D arrays, or a 1 by 1 by 1 stand-in, so each sampler always has one.
    unsigned atlasMap() const { return atlas_ != 0 ? atlas_ : atlasStandIn_; }
    unsigned cascadeMap() const { return cascades_ != 0 ? cascades_ : cascadeStandIn_; }
    // The atlas texture's pages, 0 with none.
    int atlasPages() const { return atlas_ != 0 ? atlasTexturePages_ : 0; }

private:
    struct DepthProgram {
        unsigned id = 0;
        int model = -1;
        int viewProjection = -1;
    };

    // The atlas texture made pages layers of size texels square, and its
    // first page attached. False, with no atlas, when the GPU will not make
    // it (past its limits or memory) or draw into it.
    bool makeAtlas(int size, int pages);
    // Every light drawn unshadowed from now on, said once.
    void refuse();
    // Depth on, polygon offset on, both sides drawn, scissor on.
    void begin();
    void end();
    // Draws casters (indices into casters_) seen through viewProjection.
    bool drawCasters(const engine_core::Matrix4& viewProjection, const std::vector<int>& casters,
                     const MeshDraw* meshes, bool& asked);

    ShadowPlanner planner_;
    // What draw handed the planner, and the MeshDraw each came from.
    std::vector<ShadowCaster> casters_;
    std::vector<int> casterMeshes_;
    DepthProgram depth_;
    unsigned atlasFbo_ = 0;
    unsigned atlas_ = 0;
    int atlasTextureSize_ = 0;
    int atlasTexturePages_ = 0;
    bool refused_ = false;
    // Whether an atlas the GPU would not make has been said.
    bool capSaid_ = false;
    // The settings the atlas was planned under: a change starts it over.
    int plannedMin_ = 0;
    int plannedMax_ = 0;
    int plannedMaxPages_ = 0;
    int plannedMinTile_ = 0;
    int plannedMaxTile_ = 0;
    unsigned cascadeFbo_ = 0;
    unsigned cascades_ = 0;
    int cascadeSize_ = 0;
    unsigned atlasStandIn_ = 0;
    unsigned cascadeStandIn_ = 0;
};

}  // namespace runner
