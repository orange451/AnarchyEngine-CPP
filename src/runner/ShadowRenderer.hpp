#pragma once

#include "InstanceBuffer.hpp"
#include "ShadowPlanner.hpp"
#include "Visibility.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace anarchy::amesh {
class GpuMesh;
}

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

    // Draws this frame's due tiles for requests, casting from meshes, whose
    // world spheres are spheres (one per mesh). False, having committed nothing,
    // when the program cannot draw yet (macOS). A driver that will not draw into
    // the atlas gets every light unshadowed, said once.
    bool draw(const std::vector<ShadowRequest>& requests, const MeshDraw* meshes, int count, const Sphere* spheres,
              const CameraView& camera, const ShadowSettings& settings);
    // The frame's DrawItems, one per MeshDraw, whose boneBase each caster row
    // takes; null casts every mesh unposed. Set before draw and drawSun.
    void setDrawItems(const DrawItem* items) { items_ = items; }
    // One per mesh, nonzero for one that casts nothing this frame
    // (VisibilityResult::outOfBand), or null. Set before draw and drawSun.
    // hidden, and its revision (VisibilityResult::outOfBandRevision); live,
    // when given, is every caster that can cast (VisibilityResult::casters).
    void setHidden(const std::uint8_t* hidden, std::uint64_t revision, const std::vector<int>* live = nullptr) {
        hidden_ = hidden;
        hiddenRevision_ = revision;
        live_ = live;
    }
    // Which meshes changed since the last frame's (Renderer::findVisible's
    // changed draws), or all of them. Kept until draw reads them, so a frame
    // that draws no shadows loses nothing.
    void noteDraws(bool all, const std::vector<DrawRange>& changed);
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
    // Instanced calls this frame's draw and drawSun made; draw starts the count over.
    int calls() const { return calls_; }

private:
    struct DepthProgram {
        unsigned id = 0;
        int viewProjection = -1;
    };

    // The atlas texture made pages layers of size texels square, and its
    // first page attached. False, with no atlas, when the GPU will not make
    // it (past its limits or memory) or draw into it.
    bool makeAtlas(int size, int pages);
    // Every light drawn unshadowed from now on, said once.
    void refuse(int size, int pages);
    // Depth on, polygon offset on, both sides drawn, scissor on.
    void begin();
    void end();
    // Adds casters' runs, grouped by mesh, to casterRuns_ and their rows to
    // casterRows_; returns the first run's index. Their end is casterRuns_.size().
    // cutouts false casts every caster whole, holes and all.
    int addCasterRuns(const std::vector<int>& casters, const MeshDraw* meshes, bool cutouts);
    // Draws runs first to end (indices into casterRuns_) seen through
    // viewProjection: those cast whole, then the cut-outs.
    bool drawCasters(const engine_core::Matrix4& viewProjection, int first, int end, bool& asked);

    ShadowPlanner planner_;
    // What the planner reads of each mesh, kept from frame to frame and made
    // again only for the meshes that changed: caster i is mesh i, and one
    // that casts nothing has fingerprint 0. Its bounds are draw's spheres.
    std::vector<std::uint64_t> casterOwners_;
    std::vector<std::uint64_t> casterFingerprints_;
    CasterView casterView_;
    const MeshDraw* castersMeshes_ = nullptr;
    // What noteDraws said since draw last made the casters.
    bool castersAll_ = true;
    std::vector<DrawRange> castersChanged_;
    // Not owned: setDrawItems'.
    const DrawItem* items_ = nullptr;
    // Not owned: setHidden's.
    const std::uint8_t* hidden_ = nullptr;
    std::uint64_t hiddenRevision_ = 0;
    // Not owned: setHidden's.
    const std::vector<int>* live_ = nullptr;
    // Moves on when a caster's owner or fingerprint really changes, not when
    // the same draw is only written again (as terrain is, every paint).
    std::uint64_t casterRevision_ = 1;
    const std::uint8_t* revisionHidden_ = nullptr;
    std::uint64_t revisionHiddenAt_ = 0;
    // The casters of every tile or cascade drawn this frame, as instanced runs.
    struct CasterRun {
        const anarchy::amesh::GpuMesh* mesh = nullptr;
        // The texture whose holes it leaves out, or 0 for one cast whole.
        unsigned cutout = 0;
        int first = 0;
        int count = 0;
    };
    std::vector<CasterRun> casterRuns_;
    std::vector<InstanceData> casterRows_;
    std::vector<int> casterOrder_;
    // Each tile's or cascade's runs, first to end in casterRuns_.
    std::vector<std::pair<int, int>> tileRuns_;
    InstanceBuffer instances_;
    int calls_ = 0;
    DepthProgram depth_;
    // depth_ built with CUTOUT: alpha-tests each caster's texture.
    DepthProgram cutout_;
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
