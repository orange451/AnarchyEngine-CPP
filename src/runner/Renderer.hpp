#pragma once

#include "BloomMath.hpp"
#include "DraggerMath.hpp"
#include "DrawBatches.hpp"
#include "EnvironmentMap.hpp"
#include "GpuTimer.hpp"
#include "GridBands.hpp"
#include "InstanceBuffer.hpp"
#include "Matrix4.hpp"
#include "SceneDepth.hpp"
#include "ShadowRenderer.hpp"
#include "SkyMath.hpp"
#include "ViewCapture.hpp"
#include "Visibility.hpp"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <vector>

namespace anarchy::amesh {
class GpuMesh;
}

namespace runner {

// One uploaded AMESH at a GameObject's Transform (column-major, world space),
// shaded by its Material: the diffuse texture times color times the mesh's
// vertex colors, lit by the scene's lights.
struct MeshDraw {
    const anarchy::amesh::GpuMesh* mesh = nullptr;
    engine_core::Matrix4 model = engine_core::matrix4_identity();
    // A GL texture, sampled at the mesh's UVs. 0 draws white.
    unsigned texture = 0;
    // RGBA, 0 to 1, as the Material's Color3 holds it (sRGB). Alpha is unused.
    // The GameObject's Color is tint, not multiplied in here.
    float color[4] = {1.f, 1.f, 1.f, 1.f};
    // The Material's other textures. 0 is none: no normal map, and the
    // roughness, metalness, and emissive values alone.
    unsigned normalTexture = 0;
    unsigned roughnessTexture = 0;
    unsigned metalnessTexture = 0;
    unsigned emissiveTexture = 0;
    // The Material's Emissive, as its Color3 holds it. It scales emissiveTexture.
    float emissive[3] = {0.f, 0.f, 0.f};
    // Each 0 to 1. Transparency 0 is opaque; 1 draws nothing.
    float metalness = 0.f;
    float roughness = 0.4f;
    float reflectivity = 0.5f;
    float transparency = 0.f;
    // The instance that draws it, or 0. A light never shadows itself, so
    // meshes whose owner is a LightDraw's id cast nothing for that light.
    std::uint64_t owner = 0;
    // The GameObject's Color, as its Color3 holds it (sRGB). It multiplies color.
    float tint[3] = {1.f, 1.f, 1.f};
    // Which Prefab Model it draws, numbered from 1 each frame. Draws with the
    // same slot share a mesh and every Material value, and draw as one
    // instanced call. 0 draws alone.
    std::uint32_t slot = 0;
};

// A PointLight, SpotLight, or DirectionalLight, in world space.
struct LightDraw {
    enum class Kind { Point, Spot, Directional };
    Kind kind = Kind::Point;
    // Unused for a DirectionalLight.
    float position[3] = {0.f, 0.f, 0.f};
    // Where a SpotLight or DirectionalLight points. Unused for a PointLight.
    float direction[3] = {0.f, 0.f, -1.f};
    // Linear, as the Color3 holds it.
    float color[3] = {1.f, 1.f, 1.f};
    float intensity = 1.f;
    // Unused for a DirectionalLight, which reaches everywhere.
    float radius = 8.f;
    // A SpotLight's whole cone, in degrees, and the part of it at full brightness.
    float outerFovDegrees = 80.f;
    float innerFovScale = 0.1f;
    // Its instance, so its shadow map is kept from frame to frame and its
    // own meshes cast nothing for it. 0 for none: its map is drawn every frame.
    std::uint64_t id = 0;
    bool shadows = false;
    // A DirectionalLight's: studs from the camera its cascades cover.
    float shadowDistance = 100.f;
};

// The DynamicSky's light from state: directional, id 0, shining the way
// opposite state.light.toward, with kSkyShadowDistance.
LightDraw SkyLightDraw(const SkyState& state, bool shadows);

// What the last draw did with its meshes.
struct RenderStats {
    // MeshDraws given.
    int draws = 0;
    // Opaque and see-through draws in view.
    int visible = 0;
    // Drawable, but outside the view.
    int culled = 0;
    // Draw calls the geometry pass made.
    int runs = 0;
    // GpuMesh::draw_instanced calls in every pass, shadows included.
    int instancedCalls = 0;
};

// The Skybox, as the renderer reads it. Each image is a GL texture as
// TextureCache::getEnvironment uploads it, with its revision.
struct SceneSky {
    // 0 draws no sky: surfaces take the legacy stand-in sky's light.
    unsigned image = 0;
    std::uint64_t imageRevision = 0;
    float exposure = 1.f;
    // Multiplies the light the sky gives surfaces (image-based lighting),
    // not the sky drawn behind them.
    float lightScale = 1.f;
    // Degrees about the world's Y axis.
    float rotationDegrees = 0.f;
    // As the Color3 holds it (sRGB).
    float tint[3] = {1.f, 1.f, 1.f};
};

// Lighting.Antialiasing, as the renderer reads it.
enum class SceneAntialiasing { None = 0, FXAA = 1 };

// The BloomEffect, as the renderer reads it. The defaults draw no bloom.
struct SceneBloom {
    bool enabled = false;
    // 0 to 1: how much of the image moves into its blurred copy.
    float intensity = 0.05f;
    // Pixels at a 1080-pixel-tall view (BloomMath).
    float size = 24.f;
    float threshold = 0.f;
};

// The ScreenSpaceReflections, as the renderer reads it. The defaults trace nothing.
struct SceneReflections {
    bool enabled = false;
    float intensity = 1.f;
    // Studs.
    float maxDistance = 50.f;
    float maxRoughness = 0.3f;
};

// Enum.EffectQuality, as the renderer reads it.
enum class SceneQuality { Low = 0, Medium = 1, High = 2 };

// The AmbientOcclusionEffect, as the renderer reads it. The defaults shade nothing.
struct SceneOcclusion {
    bool enabled = false;
    float intensity = 1.f;
    // Studs.
    float radius = 1.f;
    SceneQuality quality = SceneQuality::Medium;
};

// The DynamicSky, as the renderer reads it, with SkyMath's sun, moon, and
// stars already worked out (SetSkyState). The defaults draw none.
struct SceneDynamicSky {
    bool enabled = false;
    // Toward each body, world space.
    float sunDirection[3] = {0.f, 1.f, 0.f};
    float moonDirection[3] = {0.f, -1.f, 0.f};
    // Column-major, world into the stars' frame.
    float starFrame[9] = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
    float starVisibility = 0.f;
    // The sky's light, toward it, and its color times its intensity, linear;
    // the clouds are lit by it.
    float lightDirection[3] = {0.f, 1.f, 0.f};
    float lightColor[3] = {0.f, 0.f, 0.f};
    // Each disc's linear radiance.
    float sunColor[3] = {0.f, 0.f, 0.f};
    float moonColor[3] = {0.f, 0.f, 0.f};
    float cloudCover = 0.5f;
    float cloudDensity = 0.5f;
    // Studs drifted across X and Z.
    float cloudOffset[2] = {0.f, 0.f};
    // Degrees across.
    float sunSizeDegrees = 2.f;
    float moonSizeDegrees = 2.f;
    // GL textures as TextureCache::get uploads them (sRGB), 0 for the discs.
    unsigned sunTexture = 0;
    unsigned moonTexture = 0;
    SceneQuality reflectionQuality = SceneQuality::Medium;
    // Whether the clouds drift, and the clock they drift by, in seconds.
    bool windy = false;
    double seconds = 0.0;
    // What the lighting cube is drawn from (LightingDue).
    SkyLightingKey key;
};

// Copies state's directions, star frame and visibility, light, and disc colors into out.
void SetSkyState(SceneDynamicSky& out, const SkyState& state);

// Lighting's properties the renderer reads. The defaults are a new Lighting's.
struct SceneLighting {
    float ambient[3] = {0.5f, 0.5f, 0.5f};
    float exposure = 1.f;
    float saturation = 1.2f;
    float gamma = 2.2f;
    SceneAntialiasing antialiasing = SceneAntialiasing::FXAA;
    SceneSky sky;
    SceneDynamicSky dynamicSky;
    SceneBloom bloom;
    SceneReflections reflections;
    SceneOcclusion occlusion;
};

// Draws meshes seen from the camera, through the legacy AnarchyEngine
// pipeline (engine/gl): first the shadow maps of the lights that cast them
// (ShadowRenderer), then a G-buffer of each opaque surface's albedo, normal,
// material, and glow; a light pass that adds the ambient and sky light and
// then each light: a DirectionalLight over the whole view, a PointLight or
// SpotLight over its volume; then the Skybox or DynamicSky behind every surface; screen-space reflections, when a
// ScreenSpaceReflections asks for them (traced at half size from the lit
// image); a forward
// pass that blends see-through surfaces over that, farthest first; a merge;
// bloom, when a BloomEffect asks for it (a chain of half-size levels, down
// and back up); a filmic tone map, onto the pane, or with FXAA into a buffer that FXAA then
// draws onto the pane; and, when set, the floor
// grid and then the outlines over it. Every pass but those last ones draws
// into this renderer's own buffers, the pane's size in pixels. With a
// Skybox or DynamicSky, its image-based lighting (EnvironmentMap) is the sky light, and the
// sky fills the pane wherever nothing opaque was drawn, even with no meshes.
class Renderer {
public:
    // The camera until setCamera: where it is and what it looks at, in world units, Y up.
    static constexpr float kCameraEye[3] = {0.f, 3.f, 7.f};
    static constexpr float kCameraTarget[3] = {0.f, 0.f, 0.f};
    static constexpr float kCameraFovYDegrees = 60.f;
    // The see-through pass lights each surface with at most this many lights,
    // DirectionalLights first, then the first in the list. The opaque pass
    // takes any number.
    static constexpr int kMaxForwardLights = 32;

    // The GL context has to be current, and LoadGl has to have run. False when
    // a pipeline shader does not build.
    bool initialize();

    // Where meshes are seen from: a Camera's Transform, looking down its -Z
    // with its +Y up, and the vertical angle it sees in degrees. Any scale in
    // world is taken out. A world with no inverse, or an angle not between 0
    // and 180, is ignored and the camera stays as it was. Needs no GL context.
    void setCamera(const engine_core::Matrix4& world, float fovYDegrees);
    // The view setCamera last took, world to view space, and its vertical angle.
    const engine_core::Matrix4& view() const { return view_; }
    float fovYDegrees() const { return fovYDegrees_; }
    // How the next draws are lit, until it is set again. Needs no GL context.
    void setLighting(const SceneLighting& lighting) { lighting_ = lighting; }
    // Whether draw lays the editor's floor grid over the pane, as Blender
    // does: the world's Y = 0 plane ruled every 1, 10, and 100 units, with the
    // X axis in red and the Z axis in blue, hidden behind nearer surfaces.
    // Off until set. Needs no GL context.
    void setGridVisible(bool visible) { gridVisible_ = visible; }
    // Line segments draw lays over the pane after the grid, as the Scene View
    // outlines a selected PhysicsObject's collision shape: world space, x, y,
    // and z for each point, two points to a segment. Where a nearer surface
    // hides a line it is drawn faint, so a shape inside a mesh still shows.
    // Copied, and drawn by every draw until set again. Needs no GL context.
    void setOutlines(const float* points, int pointCount);
    // Dragger handles draw lays over everything after the outlines, as
    // handle_mesh builds them: world-space triangles, each corner colored,
    // drawn unlit with straight alpha and no depth test, so a handle behind a
    // surface still shows. Copied, and drawn by every draw until set again.
    // Needs no GL context.
    void setHandles(const engine_core::HandleVertex* vertices, int count);
    // How shadows are drawn, until set again. Needs no GL context.
    void setShadowSettings(const ShadowSettings& settings) { shadowSettings_ = settings; }
    // The shadow atlas texture's pages (ShadowRenderer::atlasPages), 0 with none.
    int shadowAtlasPages() const { return shadows_.atlasPages(); }
    // Whether meshes outside the view are skipped: true unless turned off to compare.
    void setCulling(bool culling) { culling_ = culling; }
    // The last draw's counts. Zero for a draw that drew no meshes.
    const RenderStats& stats() const { return stats_; }

    // x, y, width, and height are the pane in window points, origin at the top
    // left. sceneWidth and sceneHeight are the window in the same units.
    // The current GL viewport is the framebuffer. Drawing restores the
    // framebuffer binding, viewport, scissor, blend, depth, and culling state
    // so a UI pass can continue. Where no surface is drawn, the pane is the
    // clear color.
    // meshes may be null when meshCount is 0, and lights when lightCount is 0.
    // The pane is still cleared, and the Skybox, if any, drawn.
    // True when the meshes were drawn, or there were none. False when the pane
    // got only the clear: the render buffers were refused, or a pass cannot draw
    // yet. macOS's OpenGL on Metal cannot ready a program for render buffers
    // made in this frame until the next one, and a draw there would fail with
    // GL_INVALID_OPERATION, so each pass asks first; drawing again in a later
    // frame draws them. False with nothing done before initialize, or for an
    // empty pane.
    bool draw(double x, double y, double width, double height, double sceneWidth, double sceneHeight,
              const MeshDraw* meshes, int meshCount, const LightDraw* lights = nullptr, int lightCount = 0);
    // Reads back what draw just drew for the same pane, top row first. Only
    // the part inside the framebuffer and the current scissor. False when
    // there is nothing to read.
    bool read(double x, double y, double width, double height, double sceneWidth, double sceneHeight,
              ViewPixels& out) const;
    void shutdown();
    // What draw clears the pane to, 0 to 1 per channel. The Scene View passes
    // its theme color, so the clear matches the pane around it.
    void setClearColor(float r, float g, float b);
    // The depth this frame's surfaces left, and the framebuffer rectangle it
    // covers, for UI drawn inside the pane to hide behind (as the grid does).
    // texture is 0 when the last draw drew no meshes or sky, or failed.
    SceneDepth sceneDepth() const { return sceneDepth_; }
    // Where the next draw reads one depth value back, in window points as
    // draw's x and y take them; negative x or y for none. Read without
    // stalling: probedDepth has it one draw later.
    void setDepthProbe(double x, double y) {
        probeX_ = x;
        probeY_ = y;
    }
    // The scene depth under the probe point as of the draw before last, 0 near
    // to 1 far; none when the point was outside the pane or nothing was drawn.
    std::optional<float> probedDepth() const { return probedDepth_; }

private:
    // A pass's program and the uniforms Renderer sets on it. -1 for one the
    // program does not use.
    struct Program {
        unsigned id = 0;
        int model = -1;
        int view = -1;
        int projection = -1;
        int viewProjection = -1;
        int inverseProjection = -1;
        int inverseView = -1;
        int texel = -1;
        int ambient = -1;
        int skyRadiance = -1;
        // Skybox (image_lighting.glsl, sky.frag, merge.frag).
        int skyEnabled = -1;
        int viewToSky = -1;
        int skyColor = -1;
        int skyLightScale = -1;
        int prefilteredMaxLod = -1;
        // merge.frag only: whether the sky pass drew into the accumulation
        // buffer this frame, which is not always uSkyEnabled (a DynamicSky
        // draws even when its lighting cube, uSkyEnabled's cubesReady, is not).
        int skyDrawn = -1;
        // Material.
        int diffuse = -1;
        int normalMap = -1;
        int roughnessMap = -1;
        int metalnessMap = -1;
        int emissiveMap = -1;
        int color = -1;
        int emissive = -1;
        int metalness = -1;
        int roughness = -1;
        int reflectivity = -1;
        int normalMapEnabled = -1;
        int emissiveMapEnabled = -1;
        int transparency = -1;
        // G-buffer inputs.
        int depth = -1;
        int albedo = -1;
        int normal = -1;
        int material = -1;
        int emissiveBuffer = -1;
        int accumulation = -1;
        int transparencyBuffer = -1;
        int scene = -1;
        // One light (light.frag).
        int lightPosition = -1;
        int lightDirection = -1;
        int lightCone = -1;
        int lightColor = -1;
        int lightRadius = -1;
        int lightIntensity = -1;
        // One light's shadow (shadow.glsl).
        int shadowKind = -1;
        int shadowMatrix = -1;
        int shadowCascadeCount = -1;
        int shadowLight = -1;
        int shadowNearFar = -1;
        int shadowTexel = -1;
        int shadowTexelUv = -1;
        int shadowTiles = -1;
        int shadowFaceScale = -1;
        // Every light (forward.frag).
        int lightCount = -1;
        int lightPositionRadius = -1;
        int lightColorIntensity = -1;
        int lightDirections = -1;
        int lightCones = -1;
        // Tone map.
        int exposure = -1;
        int inverseGamma = -1;
        int saturation = -1;
        // Bloom (bloom_down.frag, bloom_up.frag) and its mix in the tone map.
        int prefilter = -1;
        int threshold = -1;
        int radius = -1;
        int bloomIntensity = -1;
        int bloomLevelScale = -1;
        int bloomThreshold = -1;
        // Screen-space reflections (ssr.frag, merge.frag).
        int screenSize = -1;
        int nearPlane = -1;
        int maxDistance = -1;
        int maxRoughness = -1;
        int chainLevels = -1;
        // ssr_blur.frag: the level of its source the taps read.
        int sourceLevel = -1;
        int reflectionsEnabled = -1;
        int reflectionsIntensity = -1;
        // Ambient occlusion (gtao.frag, ao_blur.frag, occlusion.glsl).
        int occlusionRadius = -1;
        int projectionScale = -1;
        int occlusionScale = -1;
        int slices = -1;
        int blurDirection = -1;
        int blurRadius = -1;
        int occlusionEnabled = -1;
        int occlusionIntensity = -1;
        // DynamicSky (procedural_sky.glsl).
        int face = -1;
        int sunDirection = -1;
        int moonDirection = -1;
        int starFrame = -1;
        int starVisibility = -1;
        int bodyLightDirection = -1;
        int bodyLightColor = -1;
        int sunColor = -1;
        int moonColor = -1;
        int cloudCover = -1;
        int cloudDensity = -1;
        int cloudOffset = -1;
        int sunSize = -1;
        int moonSize = -1;
        int sunTextureEnabled = -1;
        int moonTextureEnabled = -1;
    };

    // A light as the shaders take it, in view space.
    struct ViewLight {
        float position[3];
        float direction[3];
        // A SpotLight's outer and inner cosines. x is -2 for a PointLight and
        // -4 for a DirectionalLight, as lighting.glsl's shadeLight reads it.
        float cone[2];
        float color[3];
        float radius;
        float intensity;
        // Its entry in shadowRequests_ and shadowLookups_, -1 for none, or
        // kSunShadow for the sun's cascades.
        int shadow = -1;
    };

    bool buildProgram(Program& program, const char* name, const char* vertex, const char* fragment,
                      std::initializer_list<const char*> libraries);
    // Makes the offscreen buffers width by height pixels, if they are not.
    // False, with nothing made, when the driver will not render into them.
    bool ensureTargets(int width, int height);
    void destroyTargets();
    void createSphere();

    // Each pass is false, having stopped before its first draw, when its program cannot draw yet.
    bool geometryPass(const MeshDraw* meshes, const float* projection);
    bool lightPass(const float* projection, const float* inverseProjection);
    // The Skybox where no opaque surface was drawn. True with no Skybox.
    bool skyPass(const float* inverseProjection);
    bool transparencyPass(const MeshDraw* meshes, const float* projection, const float* inverseProjection);
    bool mergePass(bool reflected, const float* inverseProjection);
    // Screen-space reflections into reflectionTexture_, from the lit opaque
    // image. False, with nothing to resolve, when none are asked for, the
    // buffers are refused, or a program cannot draw yet.
    bool reflectionsPass(const float* projection, const float* inverseProjection);
    // Ambient occlusion into occlusionTexture_ at the Quality's scale. False,
    // with the light pass reading white, when none is asked for, the buffers
    // are refused, or a program cannot draw yet.
    bool occlusionPass(const float* projection, const float* inverseProjection);
    bool ensureOcclusionBuffers(int width, int height, int scale);
    void destroyOcclusionBuffers();
    // Points program's occlusion uniforms and unit at this frame's result, or white.
    void bindOcclusion(const Program& program);
    bool ensureReflectionBuffers(int width, int height);
    void destroyReflectionBuffers();
    // Bloom's chain from the merge image, width by height pixels: the levels
    // drawn, 0 when there is no bloom this frame (none asked for, no room for
    // a level, the chain refused, or a program that cannot draw yet).
    int bloomPass(int width, int height);
    // The tone map, with this frame's uniforms and textures, into whatever
    // framebuffer, viewport, and blend are set. False when it cannot draw yet.
    bool toneMapPass(int bloomLevels);
    // ldrTexture_ onto whatever is bound, opaque. False when it cannot draw yet.
    bool fxaaPass();
    // Makes the chain for a width by height pane, if it is not made. False,
    // with nothing made, when the driver will not render into it.
    bool ensureBloomChain(int width, int height);
    void destroyBloomChain();
    // The floor grid over the pane, on the pane's framebuffer. depth is the
    // scene's, or a texture of 1s where nothing was drawn.
    void gridPass(unsigned depth, const float* projection, const float* inverseProjection, int width, int height);
    // Whether the framebuffer the pane draws into has a depth buffer, which the
    // grid's bands use to shade each pixel once. Asked once per framebuffer.
    bool paneHasDepth(int framebuffer);
    // outlines_ over the pane, on the pane's framebuffer, with depth as gridPass takes it.
    void outlinePass(unsigned depth, const float* projection, const float* inverseProjection);
    void handlePass(const float* projection);
    void bindMaterial(const Program& program, const MeshDraw& draw);
    void bindGBuffer(const Program& program);
    // viewToSky_ and skyColor_ from the camera and the Skybox.
    void prepareSky();
    // The Skybox's uniforms and cubes, or uSkyEnabled 0 with none.
    void bindSky(const Program& program);
    // sky's uniforms and its two textures.
    void bindDynamicSky(const Program& program, const SceneDynamicSky& sky);
    // Starts drawing the DynamicSky's lighting cube again when LightingDue
    // says so, and draws a slice of a redraw under way, a slice a frame
    // (LightingSliceAt); the cubes in use stay until the redraw finishes.
    // The first redraw, or one at a new ReflectionQuality, draws all at once.
    // True once the cubes hold a DynamicSky's lighting, whether a finished
    // redraw's or an earlier one's left in place meanwhile; false before
    // they ever have. Either way the visible sky (skyPass) draws from the
    // shader and does not wait on this.
    bool updateDynamicSkyLighting();
    // Whether this draw's sky is the DynamicSky.
    bool dynamicSkyDrawn() const { return lighting_.dynamicSky.enabled && dynamicSkyBuilt_; }
    // The camera as the shadow planner and visibility see it, for this frame's targets.
    CameraView cameraView(const float* projection) const;
    // Fills drawItems_ from meshes and finds what the camera sees into visibility_.
    void findVisible(const MeshDraw* meshes, int count, const CameraView& camera);
    // Draws this frame's due shadow maps, before the G-buffer, and fills shadowLookups_.
    bool shadowPass(const MeshDraw* meshes, int count, const CameraView& camera);
    // Points program at lookup's map, and every shadow sampler at a texture of its kind.
    void bindShadow(const Program& program, const ShadowLookup& lookup);

    Program geometry_;
    Program forward_;
    Program ibl_;
    Program light_;
    // light.frag over the whole view, for a DirectionalLight.
    Program sun_;
    Program merge_;
    Program tonemap_;
    Program bloomDown_;
    Program bloomUp_;
    Program fxaa_;
    Program ssrScene_;
    Program ssrBlur_;
    Program ssr_;
    Program gtao_;
    Program aoBlur_;
    Program sky_;
    // The DynamicSky's, built apart from the others: when they do not build,
    // only the DynamicSky goes undrawn.
    Program dynamicSky_;
    Program dynamicSkyCube_;
    bool dynamicSkyBuilt_ = false;
    // What the lighting cube was last drawn from, and when.
    SkyLightingKey skyLightingMade_;
    double skyLightingMadeAt_ = 0.0;
    // What the redraw under way draws, as it was when it started.
    SceneDynamicSky skyLightingDrawing_;
    bool skyLightingValid_ = false;
    Program grid_;
    Program outline_;
    Program handle_;
    EnvironmentMap environment_;
    // Whether this draw has a Skybox or DynamicSky whose cubes are made,
    // which surfaces (image_lighting.glsl) light from and reflect.
    bool skyReady_ = false;
    // Whether skyPass draws into the accumulation buffer this frame: always
    // when skyReady_ is, and also for a DynamicSky whose lighting cube
    // (skyReady_) is not ready yet — the visible sky does not wait on it.
    bool skyVisible_ = false;
    // From view space to the sky's, column-major, and Exposure times Tint, linear.
    float viewToSky_[9] = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f};
    float skyColor_[3] = {1.f, 1.f, 1.f};
    // The sky's LightScale and the 2D image bound for it: a DynamicSky's are 1 and white.
    float skyLightScale_ = 1.f;
    unsigned skyImage_ = 0;
    // 1 by 1 white, bound for a texture a draw does not have.
    unsigned whiteTexture_ = 0;
    // 1 by 1 black on each face, bound for the Skybox's cubes when there is none.
    unsigned blackCube_ = 0;
    // No attributes: the full-screen triangle comes from gl_VertexID.
    unsigned emptyVao_ = 0;
    // The floor grid drawn on GridBands' triangles, and the camera they were
    // built for; they are built again only when it changes.
    Program gridBands_;
    unsigned gridBandVao_ = 0;
    unsigned gridBandVbo_ = 0;
    std::vector<float> gridBandTriangles_;
    GridView gridBandView_;
    bool gridBandsBuilt_ = false;
    bool gridBandsValid_ = false;
    int depthFramebuffer_ = -1;
    bool depthFramebufferHas_ = false;
    // A unit sphere, positions only, for each light's volume.
    unsigned sphereVao_ = 0;
    unsigned sphereVbo_ = 0;
    unsigned sphereEbo_ = 0;
    int sphereIndexCount_ = 0;
    // The outlines' points, uploaded again by every draw that has any.
    unsigned outlineVao_ = 0;
    unsigned outlineVbo_ = 0;
    unsigned handleVao_ = 0;
    unsigned handleVbo_ = 0;

    // The offscreen buffers, all targetWidth_ by targetHeight_.
    int targetWidth_ = 0;
    int targetHeight_ = 0;
    // Albedo, normal, material, emissive, and the depth every pass shares.
    unsigned gbufferFbo_ = 0;
    unsigned albedoTexture_ = 0;
    unsigned normalTexture_ = 0;
    unsigned materialTexture_ = 0;
    unsigned emissiveTexture_ = 0;
    unsigned depthTexture_ = 0;
    // The light each opaque surface took.
    unsigned accumulationFbo_ = 0;
    unsigned accumulationTexture_ = 0;
    // See-through surfaces, premultiplied, depth tested against the G-buffer's.
    unsigned transparencyFbo_ = 0;
    unsigned transparencyTexture_ = 0;
    // The linear image the tone map reads.
    unsigned mergeFbo_ = 0;
    unsigned mergeTexture_ = 0;
    // Whether the driver refused a size, so the refusal is reported once.
    bool targetsRefused_ = false;
    // Bloom's chain: bloomTextures_[k] is the pane halved k + 1 times, linear
    // RGBA16F. Made for bloomWidth_ by bloomHeight_ on the first frame that
    // blooms, apart from the other buffers, so a place with no bloom has none.
    unsigned bloomTextures_[kBloomMaxLevels] = {};
    unsigned bloomFbos_[kBloomMaxLevels] = {};
    int bloomLevelsMade_ = 0;
    int bloomWidth_ = 0;
    int bloomHeight_ = 0;
    // The size the driver last refused, so it is reported once and not tried each frame.
    int bloomRefusedWidth_ = 0;
    int bloomRefusedHeight_ = 0;
    // The tone-mapped image FXAA reads, the pane's size, RGBA8 with linear
    // filtering. Made and resized with the other buffers.
    unsigned ldrFbo_ = 0;
    unsigned ldrTexture_ = 0;
    // Whether fxaa_ has passed validation since the programs or buffers were
    // last made. Validating is slow on macOS, and nothing it checks changes
    // between frames, so it is asked until it passes, as the grid's bands are.
    bool fxaaValid_ = false;
    // Screen-space reflections, each half the pane's size: the lit opaque
    // image with its mips, which the trace reads, and the trace itself.
    // Made on the first frame that reflects, apart from the other buffers.
    unsigned reflectSceneFbo_ = 0;
    unsigned reflectSceneTexture_ = 0;
    int reflectSceneLevels_ = 0;
    // The mip chain's blur: each level k from 1 is drawn across into
    // reflectBlurTexture_'s level k, then down into reflectSceneTexture_'s.
    // Element k - 1 of each list draws into level k.
    unsigned reflectBlurTexture_ = 0;
    std::vector<unsigned> reflectBlurFbos_;
    std::vector<unsigned> reflectSceneLevelFbos_;
    unsigned reflectionFbo_ = 0;
    unsigned reflectionTexture_ = 0;
    int reflectionWidth_ = 0;
    int reflectionHeight_ = 0;
    int reflectionRefusedWidth_ = 0;
    int reflectionRefusedHeight_ = 0;
    // Whether ssrScene_, ssrBlur_ and ssr_ have passed validation since the programs or
    // these buffers were last made, as fxaaValid_ is for FXAA.
    bool reflectionsValid_ = false;
    // Ambient occlusion, one channel, 1 open: the trace and the blur's halfway
    // buffer, the pane's size divided by occlusionScale_. Made on the first
    // frame that shades, apart from the other buffers.
    unsigned occlusionFbo_ = 0;
    unsigned occlusionTexture_ = 0;
    unsigned occlusionBlurFbo_ = 0;
    unsigned occlusionBlurTexture_ = 0;
    int occlusionWidth_ = 0;
    int occlusionHeight_ = 0;
    int occlusionScale_ = 0;
    // The last draw's framebuffer pixels a window point: 2 on a Retina display.
    float pixelsPerPoint_ = 1.f;
    int occlusionRefusedWidth_ = 0;
    int occlusionRefusedHeight_ = 0;
    int occlusionRefusedScale_ = 0;
    bool occlusionValid_ = false;
    // Whether this frame shaded occlusion: when not, the passes read white.
    bool occlusionReady_ = false;

    void readProbe(int paneX, int paneY, int paneWidth, int paneHeight, double sceneWidth, double sceneHeight,
                   const int viewport[4]);
    SceneDepth sceneDepth_;
    double probeX_ = -1;
    double probeY_ = -1;
    std::optional<float> probedDepth_;
    // Two one-float buffers in turn: one is read back while the other fills.
    unsigned probeBuffers_[2] = {0, 0};
    bool probeFilled_[2] = {false, false};
    int probeNext_ = 0;

    bool ready_ = false;

    // Times each pass on the GPU for the profiler, while it records.

    GpuTimer gpu_;
    float clear_[3] = {30.f / 255.f, 30.f / 255.f, 30.f / 255.f};
    // The inverse of the camera's world, column-major.
    engine_core::Matrix4 view_ = DefaultView();
    float fovYDegrees_ = kCameraFovYDegrees;
    SceneLighting lighting_;
    bool gridVisible_ = false;
    std::vector<float> outlines_;
    std::vector<engine_core::HandleVertex> handles_;

    // Per draw, reused.
    std::vector<ViewLight> viewLights_;
    DrawBatches batches_;
    InstanceBuffer instances_;
    std::vector<DrawItem> drawItems_;
    VisibilityResult visibility_;
    bool culling_ = true;
    RenderStats stats_;
    std::vector<ShadowRequest> shadowRequests_;
    std::vector<ShadowLookup> shadowLookups_;
    // The first shadowed DirectionalLight this frame, if any, and its lookup.
    SunRequest sunShadow_;
    bool hasSunShadow_ = false;
    ShadowLookup sunLookup_;
    ShadowRenderer shadows_;
    ShadowSettings shadowSettings_;

    static engine_core::Matrix4 DefaultView();
};

}  // namespace runner
