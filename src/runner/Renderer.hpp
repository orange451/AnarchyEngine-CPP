#pragma once

#include "Matrix4.hpp"
#include "ViewCapture.hpp"

#include <initializer_list>
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
    float color[4] = {1.f, 1.f, 1.f, 1.f};
    // The Material's other textures. 0 is none: no normal map, and the
    // roughness and metalness numbers alone.
    unsigned normalTexture = 0;
    unsigned roughnessTexture = 0;
    unsigned metalnessTexture = 0;
    // The Material's Emissive, as its Color3 holds it.
    float emissive[3] = {0.f, 0.f, 0.f};
    // Each 0 to 1. Transparency 0 is opaque; 1 draws nothing.
    float metalness = 0.f;
    float roughness = 0.4f;
    float reflectivity = 0.5f;
    float transparency = 0.f;
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
};

// Lighting's properties the renderer reads. The defaults are a new Lighting's.
struct SceneLighting {
    float ambient[3] = {0.5f, 0.5f, 0.5f};
    float exposure = 1.f;
    float saturation = 1.2f;
    float gamma = 2.2f;
};

// Draws meshes seen from the camera, through the legacy AnarchyEngine
// pipeline (engine/gl): a G-buffer of each opaque surface's albedo, normal,
// material, and glow; a light pass that adds the ambient and sky light and
// then each light: a DirectionalLight over the whole view, a PointLight or
// SpotLight over its volume; a forward pass that blends see-through surfaces
// over that, farthest first; a merge; and a filmic tone map onto the pane.
// Every pass but the last draws into this renderer's own buffers, the pane's
// size in pixels.
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
    // How the next draws are lit, until it is set again. Needs no GL context.
    void setLighting(const SceneLighting& lighting) { lighting_ = lighting; }

    // x, y, width, and height are the pane in window points, origin at the top
    // left. sceneWidth and sceneHeight are the window in the same units.
    // The current GL viewport is the framebuffer. Drawing restores the
    // framebuffer binding, viewport, scissor, blend, depth, and culling state
    // so a UI pass can continue. Where no surface is drawn, the pane is the
    // clear color.
    // meshes may be null when meshCount is 0, and lights when lightCount is 0.
    // The pane is still cleared.
    void draw(double x, double y, double width, double height, double sceneWidth, double sceneHeight,
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
        int texel = -1;
        int ambient = -1;
        int skyRadiance = -1;
        // Material.
        int diffuse = -1;
        int normalMap = -1;
        int roughnessMap = -1;
        int metalnessMap = -1;
        int color = -1;
        int emissive = -1;
        int metalness = -1;
        int roughness = -1;
        int reflectivity = -1;
        int normalMapEnabled = -1;
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
    };

    bool buildProgram(Program& program, const char* name, const char* vertex, const char* fragment,
                      std::initializer_list<const char*> libraries);
    // Makes the offscreen buffers width by height pixels, if they are not.
    // False, with nothing made, when the driver will not render into them.
    bool ensureTargets(int width, int height);
    void destroyTargets();
    void createSphere();

    void geometryPass(const MeshDraw* meshes, int count, const float* projection);
    void lightPass(const float* projection, const float* inverseProjection);
    void transparencyPass(const MeshDraw* meshes, int count, const float* projection, const float* inverseProjection);
    void mergePass();
    void bindMaterial(const Program& program, const MeshDraw& draw);
    void bindGBuffer(const Program& program);

    Program geometry_;
    Program forward_;
    Program ibl_;
    Program light_;
    // light.frag over the whole view, for a DirectionalLight.
    Program sun_;
    Program merge_;
    Program tonemap_;
    // 1 by 1 white, bound for a texture a draw does not have.
    unsigned whiteTexture_ = 0;
    // No attributes: the full-screen triangle comes from gl_VertexID.
    unsigned emptyVao_ = 0;
    // A unit sphere, positions only, for each light's volume.
    unsigned sphereVao_ = 0;
    unsigned sphereVbo_ = 0;
    unsigned sphereEbo_ = 0;
    int sphereIndexCount_ = 0;

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

    bool ready_ = false;
    float clear_[3] = {30.f / 255.f, 30.f / 255.f, 30.f / 255.f};
    // The inverse of the camera's world, column-major.
    engine_core::Matrix4 view_ = DefaultView();
    float fovYDegrees_ = kCameraFovYDegrees;
    SceneLighting lighting_;

    // Per draw, reused.
    std::vector<ViewLight> viewLights_;
    std::vector<int> transparent_;
    std::vector<float> transparentDepth_;

    static engine_core::Matrix4 DefaultView();
};

}  // namespace runner
