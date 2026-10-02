#pragma once

#include "ShadowAtlas.hpp"
#include "ShadowMath.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace runner {

// How the Scene View draws shadows.
struct ShadowSettings {
    // False draws every light unshadowed, whatever its Shadows says.
    bool enabled = true;
    // The atlas every PointLight's and SpotLight's maps share, in texels
    // square: it starts at atlasMinSize and doubles, up to atlasMaxSize,
    // when the lights' tiles do not fit. Powers of two.
    int atlasMinSize = 1024;
    int atlasMaxSize = 4096;
    // A tile's size range. A SpotLight takes one; a PointLight six, one per cube face.
    int minTile = 64;
    int maxTile = 1024;
    // Texels a tile gets for each pixel the light's reach covers across the view.
    float texelsPerPixel = 1.f;
    // A light that looks smaller than this (ProjectedReach) casts no shadow.
    float minReach = 0.02f;
    // How far past its band a light's ideal tile must go before the tile changes, as a part.
    float hysteresis = 0.15f;
    // Tile texels redrawn in one frame, at most. Past it, lights keep the
    // map they have and wait. At least one light is redrawn every frame.
    std::int64_t maxTexelsPerFrame = 8 * 1024 * 1024;
    // The first shadowed DirectionalLight's cascades.
    int cascadeSize = 2048;
    // 1 to kMaxCascades.
    int cascadeCount = 4;
    // How the cascades split ShadowDistance: 0 evenly, 1 logarithmically.
    float cascadeLambda = 0.75f;
    // How far toward the sun a cascade reaches for casters, in ShadowDistances.
    float cascadePullLimit = 4.f;
};

// A light's tile size in texels: about texelsPerPixel times the pixels its
// reach covers across a view paneHeight pixels tall, as a power of two from
// minTile to maxTile, or 0 when it looks too small for a shadow. previous is
// last frame's size, 0 for none: a light keeps its size until its ideal
// leaves that size's band by the hysteresis part, so one at a boundary does
// not flip between sizes every frame.
int TileSizeFor(float reach, int previous, int paneHeight, const ShadowSettings& settings);

// A mesh that casts shadows, as the planner sees it.
struct ShadowCaster {
    // Which geometry it draws (its GpuMesh's address), and that geometry's
    // revision (its GpuMesh's generation), which changes whenever it is
    // uploaded again in place, a file's or a play session's.
    std::uint64_t mesh = 0;
    std::uint64_t revision = 0;
    // The instance that draws it: a light never shadows itself.
    std::uint64_t owner = 0;
    engine_core::Matrix4 model = engine_core::matrix4_identity();
    // Its bounds, in world space.
    Sphere bounds;
};

enum class ShadowKind : std::uint8_t { Spot, Point };

// A PointLight or SpotLight that wants a shadow this frame, in world space.
struct ShadowRequest {
    // Names its map from frame to frame.
    std::uint64_t key = 0;
    // False draws its map every frame: a light with no instance cannot be
    // told from the one that had its key last frame.
    bool cached = true;
    // Its instance, whose own meshes cast nothing for it. 0 for none.
    std::uint64_t owner = 0;
    ShadowKind kind = ShadowKind::Point;
    engine_core::Vec3 position{};
    // Where a SpotLight points, unit length.
    engine_core::Vec3 direction{0.f, 0.f, -1.f};
    float radius = 8.f;
    float outerFovDegrees = 80.f;
};

// The Scene View's camera, as the planner reads it.
struct CameraView {
    engine_core::Matrix4 world = engine_core::matrix4_identity();
    engine_core::Matrix4 viewProjection = engine_core::matrix4_identity();
    float fovYDegrees = 60.f;
    float aspect = 1.f;
    float nearZ = 0.1f;
    int paneHeight = 1;
};

// A light's map as it was last drawn. The light pass reads it until it is
// drawn again, so a light that moved and waits for a redraw still reads its
// map consistently; find() withholds it instead, rather than a face that was
// never actually drawn from it.
struct LocalShadow {
    ShadowKind kind = ShadowKind::Point;
    // A SpotLight's tile is [0]; a PointLight's are its cube faces in GL's order.
    AtlasTile tiles[6];
    // A SpotLight's world to clip space.
    engine_core::Matrix4 viewProjection = engine_core::matrix4_identity();
    engine_core::Vec3 position{};
    float nearZ = 0.f;
    float farZ = 1.f;
    // World units across a texel, per unit of distance from the light.
    float texelPerDistance = 0.f;
    // A PointLight's faces' widening (CubeFaceViewProjections).
    float faceScale = 1.f;
};

// One tile to draw this frame.
struct TileDraw {
    std::uint64_t key = 0;
    // 0 for a SpotLight; the cube face for a PointLight.
    int face = 0;
    AtlasTile tile;
    engine_core::Matrix4 viewProjection = engine_core::matrix4_identity();
    // Indices into the casters plan was given.
    std::vector<int> casters;
};

struct ShadowPlan {
    std::vector<TileDraw> draws;
    // The atlas changed size: its texture has to be made again.
    bool atlasResized = false;
};

// A ready light that keeps losing the redraw cap to higher-priority lights
// waits at most this many frames before it is aged ahead of them.
constexpr int kMaxShadowWait = 4;

// The first shadowed DirectionalLight this frame.
struct SunRequest {
    std::uint64_t owner = 0;
    // Where it shines, unit length.
    engine_core::Vec3 shine{0.f, -1.f, 0.f};
    float shadowDistance = 100.f;
};

// One cascade to draw this frame: its layer of the cascade texture.
struct CascadeDraw {
    int layer = 0;
    engine_core::Matrix4 viewProjection = engine_core::matrix4_identity();
    // Indices into the casters planCascades was given.
    std::vector<int> casters;
};

// The cascades as last drawn, which the light pass reads.
struct CascadeShadow {
    int count = 0;
    engine_core::Matrix4 viewProjection[kMaxCascades] = {};
    float texelWorld[kMaxCascades] = {};
};

// Decides, with no GL, which PointLight and SpotLight shadow tiles to draw
// each frame, in an atlas it keeps. A light's map is redrawn only when its
// fingerprint changes (the light, its tile, and every caster within its
// Radius); a cube face the camera cannot see waits. Priority: a light with
// a face the camera can see that was never actually drawn from its current
// map goes first (find() withholds the map until that light is committed,
// whether or not this frame's cap let it draw); then a light that has lost
// the cap kMaxShadowWait frames running; then the rest, biggest look first,
// up to the frame's texel budget.
class ShadowPlanner {
public:
    ShadowPlan plan(const std::vector<ShadowRequest>& requests, const std::vector<ShadowCaster>& casters,
                    const CameraView& camera, const ShadowSettings& settings);
    // Once plan's draws are drawn: they become what find returns, each
    // drawn face now matching the committed map (a dirty face left undrawn
    // does not, until it too is drawn), the light's redraw wait reset to 0,
    // and it made readable again. A plan that is never committed is
    // dropped, and its lights stay due.
    void commit();
    // The light's map as last drawn, or null when it has none, or when a
    // face the camera can currently see was never actually drawn from it
    // (newly visible, or in a tile the atlas just reused) and that draw has
    // not yet reached a commit() — whether the cap held it back this frame
    // or it was scheduled but the caller has not committed it yet. A light
    // that is not cached has a map only once this frame's draw of it commits.
    const LocalShadow* find(std::uint64_t key) const;
    int atlasSize() const { return atlas_.atlasSize(); }
    std::int64_t atlasFreeTexels() const { return atlas_.freeTexels(); }
    // Forgets every map and the atlas, as when the settings or the context change.
    void clear();

    // The sun's cascades to draw this frame: none when nothing they are fitted
    // to or cast from changed since the last committed ones, or with no sun.
    std::vector<CascadeDraw> planCascades(const SunRequest* sun, const std::vector<ShadowCaster>& casters,
                                          const CameraView& camera, const ShadowSettings& settings);
    void commitCascades();
    // The cascades as last drawn, or null with none.
    const CascadeShadow* cascades() const { return cascadeReady_ ? &cascade_ : nullptr; }
    // Drops the cascades, as when their texture is made again.
    void forgetCascades() { cascadeReady_ = false; }

private:
    struct Record {
        ShadowKind kind = ShadowKind::Point;
        // The size it asked for, and the size it got, which may be smaller.
        int wanted = 0;
        int size = 0;
        int tileCount = 0;
        AtlasTile tiles[6];
        bool dirty[6] = {};
        // Whether a face's content in drawn actually came from drawn's own
        // state: false right after its tile is (re)allocated, and for any
        // face left dirty and undrawn at a commit; a hash change alone
        // (the light moved, a caster changed) never clears it, because the
        // old, slightly stale drawn is still self-consistent.
        bool matches[6] = {};
        // Frames in a row a visible due face has lost the redraw cap.
        int waited = 0;
        // The fingerprint its map was drawn from, and the one it should be now.
        std::uint64_t hash = 0;
        std::uint64_t wantHash = 0;
        bool ready = false;
        // False whenever this light must draw (plan() set it so for every
        // such light, whether or not the cap let it through this frame):
        // find() will not hand out drawn with a visible face unmatched
        // until the light's own commit() makes it true again; nor, for a
        // light that is not cached, anything at all. A scheduled
        // draw that never reaches commit() (the caller's draw calls failed,
        // or the plan was simply never committed) leaves it false.
        bool readable = false;
        bool seen = false;
        LocalShadow drawn;
    };
    // What commit applies for one light.
    struct Pending {
        std::uint64_t key = 0;
        LocalShadow shadow;
        std::uint64_t hash = 0;
        int faces[6] = {};
        int faceCount = 0;
    };

    // Gives each light, in order, tiles of its wanted size or the biggest
    // smaller size that fits. False when one got less than it wanted.
    bool allocate(const std::vector<ShadowRequest>& requests, const std::vector<int>& order,
                  const std::vector<int>& wanted, const ShadowSettings& settings);
    void releaseTiles(Record& record);
    void resetAtlas(int size, int minTile);

    ShadowAtlasAllocator atlas_;
    std::unordered_map<std::uint64_t, Record> records_;
    std::vector<Pending> pending_;
    CascadeShadow cascade_;
    std::uint64_t cascadeHash_ = 0;
    bool cascadeReady_ = false;
    CascadeShadow pendingCascade_;
    std::uint64_t pendingCascadeHash_ = 0;
    bool cascadePending_ = false;
    // Scratch kept between calls: each caster's fingerprint, and the casters inside each cascade.
    std::vector<std::uint64_t> casterHashes_;
    std::vector<int> cascadeCasters_[kMaxCascades];
};

}  // namespace runner
