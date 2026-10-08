#include "Renderer.hpp"

#include "profiler/Profiler.hpp"

#include "OcclusionMath.hpp"
#include "RenderMath.hpp"
#include "ShaderFile.hpp"
#include "amesh.hpp"
#include "gl.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace runner {
namespace {

// One pass, timed on the CPU (a Render scope), and on the GPU (a Gpu scope of
// the same name) only when per-pass GPU detail is on.
class PassTimer {
public:
    PassTimer(GpuTimer& gpu, profiler::ScopeId cpu, profiler::ScopeId on_gpu) : cpu_(cpu), gpu_(gpu) {
        gpu_.begin(on_gpu, profiler::gpu_detail());
    }
    ~PassTimer() { gpu_.end(); }
    PassTimer(const PassTimer&) = delete;
    PassTimer& operator=(const PassTimer&) = delete;

private:
    profiler::Scope cpu_;
    GpuTimer& gpu_;
};

}  // namespace

#define RENDER_PASS(name)                                                                              \
    static const profiler::ScopeId PROFILER_JOIN(pass_cpu_, __LINE__) =                                \
        profiler::intern(name, profiler::Group::Render);                                               \
    static const profiler::ScopeId PROFILER_JOIN(pass_gpu_, __LINE__) =                                \
        profiler::intern(name, profiler::Group::Gpu);                                                  \
    PassTimer PROFILER_JOIN(pass_timer_, __LINE__)(gpu_, PROFILER_JOIN(pass_cpu_, __LINE__),             \
                                                   PROFILER_JOIN(pass_gpu_, __LINE__))

namespace {

// Texture units. A pass binds what it reads to these, and each program's
// samplers are pointed at them once, when it links.
constexpr int kUnitDiffuse = 0;
constexpr int kUnitNormalMap = 1;
constexpr int kUnitRoughnessMap = 2;
constexpr int kUnitMetalnessMap = 3;
constexpr int kUnitDepth = 4;
constexpr int kUnitAlbedo = 5;
constexpr int kUnitNormal = 6;
constexpr int kUnitMaterial = 7;
constexpr int kUnitEmissive = 8;
constexpr int kUnitAccumulation = 9;
constexpr int kUnitTransparency = 10;
constexpr int kUnitScene = 11;
// The Skybox: its image, cubes, and lookup table.
constexpr int kUnitSky = 12;
constexpr int kUnitIrradiance = 13;
constexpr int kUnitPrefiltered = 14;
constexpr int kUnitBrdf = 15;
constexpr int kUnitCount = 16;
// The surface passes write the G-buffer's albedo, never read it, so a
// Material's EmissiveTexture takes its unit. bindGBuffer binds it back.
constexpr int kUnitEmissiveMap = kUnitAlbedo;
// The light pass reads no Material, so its shadow maps take the Material's units.
constexpr int kUnitShadowAtlas = kUnitDiffuse;
constexpr int kUnitShadowCascades = kUnitNormalMap;
// The tone map reads no G-buffer, so the bloom takes the accumulation buffer's unit.
constexpr int kUnitBloom = kUnitAccumulation;
// The merge reads no scene image, so the reflection trace takes its unit.
constexpr int kUnitReflections = kUnitScene;
// Ambient occlusion shares the metalness map's unit: none of the passes that
// read it (the light pass, the merge, its own blur) reads a Material.
constexpr int kUnitOcclusion = kUnitMetalnessMap;
// A light with no instance names its map for one frame only.
constexpr std::uint64_t kUncachedShadowKey = 1ull << 63;
// A ViewLight's shadow: the sun's cascades.
constexpr int kSunShadow = -2;

// The legacy pipeline's stand-in sky when there is no Skybox: a flat dark
// gray (64 of 255) times its light multiplier of 1/255.
constexpr float kSkyRadiance = (64.f / 255.f) / 255.f;

// A light volume is a sphere of triangles inside the true sphere. This much
// larger, it holds the whole Radius.
constexpr float kSphereSlack = 1.05f;
constexpr int kSphereStacks = 12;
constexpr int kSphereSlices = 16;

void BindTexture(int unit, unsigned texture) {
    glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
    glBindTexture(GL_TEXTURE_2D, texture);
}

void BindCube(int unit, unsigned texture) {
    glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
    glBindTexture(RT_GL_TEXTURE_CUBE_MAP, texture);
}

void BindArray(int unit, unsigned texture) {
    glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
    glBindTexture(RT_GL_TEXTURE_2D_ARRAY, texture);
}

void DrawFullscreen(unsigned emptyVao) {
    glBindVertexArray(emptyVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

}  // namespace

void SetSkyState(SceneDynamicSky& out, const SkyState& state) {
    const auto copy = [](SkyVector v, float* to) {
        to[0] = v.x;
        to[1] = v.y;
        to[2] = v.z;
    };
    copy(state.sun, out.sunDirection);
    copy(state.moon, out.moonDirection);
    std::copy(state.starFrame, state.starFrame + 9, out.starFrame);
    out.starVisibility = state.starVisibility;
    copy(state.light.toward, out.lightDirection);
    for (int channel = 0; channel < 3; ++channel) {
        out.lightColor[channel] = state.light.color[channel] * state.light.intensity;
        out.sunColor[channel] = state.sunColor[channel];
        out.moonColor[channel] = state.moonColor[channel];
    }
}

LightDraw SkyLightDraw(const SkyState& state, bool shadows) {
    LightDraw light;
    light.kind = LightDraw::Kind::Directional;
    // Toward the body, so it shines the other way.
    light.direction[0] = -state.light.toward.x;
    light.direction[1] = -state.light.toward.y;
    light.direction[2] = -state.light.toward.z;
    std::copy(state.light.color, state.light.color + 3, light.color);
    light.intensity = state.light.intensity;
    light.id = 0;
    light.shadows = shadows;
    light.shadowDistance = kSkyShadowDistance;
    return light;
}

bool Renderer::buildProgram(Program& program, const char* name, const char* vertex, const char* fragment,
                            std::initializer_list<const char*> libraries) {
    const std::string fragmentSource = LoadShader(fragment, libraries);
    program = Program{};
    if (fragmentSource.empty()) {
        return false;
    }
    program.id = LinkProgram(LoadShader(vertex), fragmentSource, name);
    if (program.id == 0) {
        return false;
    }
    const unsigned id = program.id;
    const auto at = [id](const char* uniform) { return glGetUniformLocation(id, uniform); };
    program.model = at("uModel");
    program.view = at("uView");
    program.projection = at("uProjection");
    program.viewProjection = at("uViewProjection");
    program.inverseProjection = at("uInverseProjection");
    program.inverseView = at("uInverseView");
    program.texel = at("uTexel");
    program.ambient = at("uAmbient");
    program.skyRadiance = at("uSkyRadiance");
    program.skyEnabled = at("uSkyEnabled");
    program.viewToSky = at("uViewToSky");
    program.skyColor = at("uSkyColor");
    program.skyLightScale = at("uSkyLightScale");
    program.prefilteredMaxLod = at("uPrefilteredMaxLod");
    program.skyDrawn = at("uSkyDrawn");
    program.diffuse = at("uDiffuse");
    program.normalMap = at("uNormalMap");
    program.roughnessMap = at("uRoughnessMap");
    program.metalnessMap = at("uMetalnessMap");
    program.emissiveMap = at("uEmissiveMap");
    program.color = at("uColor");
    program.emissive = at("uEmissive");
    program.metalness = at("uMetalness");
    program.roughness = at("uRoughness");
    program.reflectivity = at("uReflectivity");
    program.normalMapEnabled = at("uNormalMapEnabled");
    program.emissiveMapEnabled = at("uEmissiveMapEnabled");
    program.transparency = at("uTransparency");
    program.depth = at("uDepth");
    program.albedo = at("uAlbedo");
    program.normal = at("uNormal");
    program.material = at("uMaterial");
    program.emissiveBuffer = at("uEmissive");
    program.accumulation = at("uAccumulation");
    program.transparencyBuffer = at("uTransparency");
    program.scene = at("uScene");
    program.lightPosition = at("uLightPosition");
    program.lightDirection = at("uLightDirection");
    program.lightCone = at("uLightCone");
    program.lightColor = at("uLightColor");
    program.lightRadius = at("uLightRadius");
    program.lightIntensity = at("uLightIntensity");
    program.shadowKind = at("uShadowKind");
    program.shadowMatrix = at("uShadowMatrix");
    program.shadowCascadeCount = at("uShadowCascadeCount");
    program.shadowLight = at("uShadowLight");
    program.shadowNearFar = at("uShadowNearFar");
    program.shadowTexel = at("uShadowTexel");
    program.shadowTexelUv = at("uShadowTexelUv");
    program.shadowTiles = at("uShadowTiles");
    program.shadowFaceScale = at("uShadowFaceScale");
    program.lightCount = at("uLightCount");
    program.lightPositionRadius = at("uLightPositionRadius");
    program.lightColorIntensity = at("uLightColorIntensity");
    program.lightDirections = at("uLightDirection");
    program.lightCones = at("uLightCone");
    program.exposure = at("uExposure");
    program.inverseGamma = at("uInverseGamma");
    program.saturation = at("uSaturation");
    program.prefilter = at("uPrefilter");
    program.threshold = at("uThreshold");
    program.radius = at("uRadius");
    program.bloomIntensity = at("uBloomIntensity");
    program.bloomLevelScale = at("uBloomLevelScale");
    program.bloomThreshold = at("uBloomThreshold");
    program.screenSize = at("uScreenSize");
    program.nearPlane = at("uNear");
    program.maxDistance = at("uMaxDistance");
    program.maxRoughness = at("uMaxRoughness");
    program.chainLevels = at("uChainLevels");
    program.sourceLevel = at("uSourceLevel");
    program.reflectionsEnabled = at("uReflectionsEnabled");
    program.reflectionsIntensity = at("uReflectionsIntensity");
    program.occlusionRadius = at("uOcclusionRadius");
    program.projectionScale = at("uProjectionScale");
    program.occlusionScale = at("uOcclusionScale");
    program.slices = at("uSlices");
    program.blurDirection = at("uBlurDirection");
    program.blurRadius = at("uBlurRadius");
    program.occlusionEnabled = at("uOcclusionEnabled");
    program.occlusionIntensity = at("uOcclusionIntensity");
    program.face = at("uFace");
    program.sunDirection = at("uSunDirection");
    program.moonDirection = at("uMoonDirection");
    program.starFrame = at("uStarFrame");
    program.starVisibility = at("uStarVisibility");
    program.starClock = at("uStarClock");
    program.bodyLightDirection = at("uBodyLightDirection");
    program.bodyLightColor = at("uBodyLightColor");
    program.sunColor = at("uSunColor");
    program.moonColor = at("uMoonColor");
    program.cloudCover = at("uCloudCover");
    program.cloudDensity = at("uCloudDensity");
    program.cloudOffset = at("uCloudOffset");
    program.sunSize = at("uSunSize");
    program.moonSize = at("uMoonSize");
    program.sunTextureEnabled = at("uSunTextureEnabled");
    program.moonTextureEnabled = at("uMoonTextureEnabled");

    // Samplers keep their unit for the program's life. A name two passes use
    // for different things (uEmissive, uTransparency) is a sampler only in
    // merge.frag, which has no material uniforms.
    glUseProgram(id);
    const auto sampler = [&](const char* uniform, int unit) {
        const int location = at(uniform);
        if (location >= 0) {
            glUniform1i(location, unit);
        }
    };
    sampler("uDiffuse", kUnitDiffuse);
    sampler("uNormalMap", kUnitNormalMap);
    sampler("uRoughnessMap", kUnitRoughnessMap);
    sampler("uMetalnessMap", kUnitMetalnessMap);
    sampler("uEmissiveMap", kUnitEmissiveMap);
    sampler("uDepth", kUnitDepth);
    sampler("uAlbedo", kUnitAlbedo);
    sampler("uNormal", kUnitNormal);
    sampler("uMaterial", kUnitMaterial);
    sampler("uAccumulation", kUnitAccumulation);
    sampler("uScene", kUnitScene);
    sampler("uSky", kUnitSky);
    sampler("uIrradiance", kUnitIrradiance);
    sampler("uPrefiltered", kUnitPrefiltered);
    sampler("uBrdf", kUnitBrdf);
    sampler("uShadowAtlas", kUnitShadowAtlas);
    sampler("uShadowCascades", kUnitShadowCascades);
    // Each bloom step reads the level before it where the tone map reads the scene.
    sampler("uSource", kUnitScene);
    sampler("uBloom", kUnitBloom);
    sampler("uEmissiveLight", kUnitEmissive);
    sampler("uReflections", kUnitReflections);
    sampler("uOcclusion", kUnitOcclusion);
    // The blur reads its source where the merge reads the reflection trace, in another pass.
    sampler("uOcclusionSource", kUnitScene);
    // The sky pass reads no Material, so the DynamicSky's textures take its units.
    sampler("uSunTexture", kUnitDiffuse);
    sampler("uMoonTexture", kUnitNormalMap);
    if (&program == &merge_) {
        sampler("uEmissive", kUnitEmissive);
        sampler("uTransparency", kUnitTransparency);
    }
    glUseProgram(0);
    return true;
}

bool Renderer::initialize() {
    if (ready_) {
        return true;
    }

    if (glGetString(GL_VERSION) == nullptr) {
        std::fprintf(stderr, "No current OpenGL context.\n");
        return false;
    }
    // Errors a pass before this one left behind are not setup's. Bounded, since
    // a lost context can report an error on every call.
    for (int stale = 0; stale < 32 && glGetError() != GL_NO_ERROR; ++stale) {
    }

    const bool built =
        buildProgram(geometry_, "G-buffer", "pipeline/geometry.vert", "pipeline/deferred.frag",
                     {"pipeline/surface.glsl"}) &&
        buildProgram(forward_, "Transparency", "pipeline/geometry.vert", "pipeline/forward.frag",
                     {"pipeline/surface.glsl", "pipeline/lighting.glsl", "pipeline/environment.glsl",
                      "pipeline/image_lighting.glsl"}) &&
        buildProgram(ibl_, "IBL", "pipeline/fullscreen.vert", "pipeline/ibl.frag",
                     {"pipeline/lighting.glsl", "pipeline/environment.glsl", "pipeline/occlusion.glsl",
                      "pipeline/image_lighting.glsl"}) &&
        buildProgram(sky_, "Sky", "pipeline/fullscreen.vert", "pipeline/sky.frag",
                     {"pipeline/lighting.glsl", "pipeline/environment.glsl"}) &&
        buildProgram(light_, "Light", "pipeline/light.vert", "pipeline/light.frag",
                     {"pipeline/lighting.glsl", "pipeline/shadow.glsl"}) &&
        buildProgram(sun_, "Directional light", "pipeline/fullscreen.vert", "pipeline/light.frag",
                     {"pipeline/lighting.glsl", "pipeline/shadow.glsl"}) &&
        buildProgram(merge_, "Merge", "pipeline/fullscreen.vert", "pipeline/merge.frag",
                     {"pipeline/lighting.glsl", "pipeline/environment.glsl", "pipeline/occlusion.glsl",
                      "pipeline/image_lighting.glsl"}) &&
        buildProgram(tonemap_, "Tone map", "pipeline/fullscreen.vert", "pipeline/tonemap.frag",
                     {"pipeline/bloom.glsl"}) &&
        buildProgram(bloomDown_, "Bloom down", "pipeline/fullscreen.vert", "pipeline/bloom_down.frag",
                     {"pipeline/bloom.glsl"}) &&
        buildProgram(bloomUp_, "Bloom up", "pipeline/fullscreen.vert", "pipeline/bloom_up.frag", {}) &&
        buildProgram(fxaa_, "FXAA", "pipeline/fullscreen.vert", "pipeline/fxaa.frag", {}) &&
        buildProgram(ssrScene_, "Reflections scene", "pipeline/fullscreen.vert", "pipeline/ssr_scene.frag", {}) &&
        buildProgram(ssrBlur_, "Reflections blur", "pipeline/fullscreen.vert", "pipeline/ssr_blur.frag",
                     {"pipeline/ssr.glsl"}) &&
        buildProgram(ssr_, "Reflections", "pipeline/fullscreen.vert", "pipeline/ssr.frag",
                     {"pipeline/lighting.glsl", "pipeline/environment.glsl", "pipeline/image_lighting.glsl",
                      "pipeline/ssr.glsl"}) &&
        buildProgram(gtao_, "Ambient occlusion", "pipeline/fullscreen.vert", "pipeline/gtao.frag",
                     {"pipeline/lighting.glsl"}) &&
        buildProgram(aoBlur_, "Occlusion blur", "pipeline/fullscreen.vert", "pipeline/ao_blur.frag",
                     {"pipeline/lighting.glsl"}) &&
        buildProgram(grid_, "Grid", "pipeline/fullscreen.vert", "pipeline/grid.frag", {}) &&
        buildProgram(gridBands_, "Grid bands", "pipeline/grid_band.vert", "pipeline/grid.frag", {}) &&
        buildProgram(outline_, "Outline", "pipeline/outline.vert", "pipeline/outline.frag", {}) &&
        buildProgram(handle_, "Handle", "pipeline/handle.vert", "pipeline/handle.frag", {}) &&
        environment_.initialize() && shadows_.initialize();
    if (!built) {
        shutdown();
        return false;
    }

    dynamicSkyBuilt_ =
        buildProgram(dynamicSky_, "Dynamic sky", "pipeline/fullscreen.vert", "pipeline/dynamic_sky.frag",
                     {"pipeline/lighting.glsl", "pipeline/environment.glsl", "pipeline/procedural_sky.glsl"}) &&
        buildProgram(dynamicSkyCube_, "Dynamic sky cube", "pipeline/fullscreen.vert",
                     "pipeline/dynamic_sky_cube.frag", {"pipeline/environment.glsl", "pipeline/procedural_sky.glsl"});
    if (!dynamicSkyBuilt_) {
        std::fprintf(stderr, "The DynamicSky's shaders did not build; a DynamicSky draws no sky.\n");
    }

    const unsigned char white[4] = {255, 255, 255, 255};
    glGenTextures(1, &whiteTexture_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, whiteTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
    glBindTexture(GL_TEXTURE_2D, 0);

    const unsigned char black[4] = {0, 0, 0, 255};
    glGenTextures(1, &blackCube_);
    glBindTexture(RT_GL_TEXTURE_CUBE_MAP, blackCube_);
    for (int face = 0; face < 6; ++face) {
        glTexImage2D(RT_GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(face), 0, static_cast<GLint>(GL_RGBA8), 1,
                     1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
    }
    glTexParameteri(RT_GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR));
    glTexParameteri(RT_GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
    glBindTexture(RT_GL_TEXTURE_CUBE_MAP, 0);

    glGenVertexArrays(1, &emptyVao_);
    createSphere();
    glGenVertexArrays(1, &gridBandVao_);
    glBindVertexArray(gridBandVao_);
    glGenBuffers(1, &gridBandVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, gridBandVbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    gridBandsBuilt_ = false;
    gridBandsValid_ = false;
    fxaaValid_ = false;
    reflectionsValid_ = false;
    occlusionValid_ = false;
    depthFramebuffer_ = -1;
    glGenVertexArrays(1, &outlineVao_);
    glBindVertexArray(outlineVao_);
    glGenBuffers(1, &outlineVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, outlineVbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Handle corners: position, then color.
    glGenVertexArrays(1, &handleVao_);
    glBindVertexArray(handleVao_);
    glGenBuffers(1, &handleVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, handleVbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(engine_core::HandleVertex), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(engine_core::HandleVertex),
                          reinterpret_cast<const void*>(3 * sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        std::fprintf(stderr, "OpenGL error during setup: 0x%x\n", error);
        shutdown();
        return false;
    }

    ready_ = true;
    gpu_.init();
    return true;
}

void Renderer::createSphere() {
    std::vector<float> positions;
    positions.reserve(static_cast<std::size_t>((kSphereStacks + 1) * (kSphereSlices + 1) * 3));
    for (int stack = 0; stack <= kSphereStacks; ++stack) {
        const float phi = 3.14159265f * static_cast<float>(stack) / kSphereStacks;
        for (int slice = 0; slice <= kSphereSlices; ++slice) {
            const float theta = 6.28318531f * static_cast<float>(slice) / kSphereSlices;
            positions.push_back(std::sin(phi) * std::cos(theta));
            positions.push_back(std::cos(phi));
            positions.push_back(std::sin(phi) * std::sin(theta));
        }
    }
    // Counter-clockwise seen from outside, so culling front faces keeps the inside.
    std::vector<unsigned short> indices;
    for (int stack = 0; stack < kSphereStacks; ++stack) {
        for (int slice = 0; slice < kSphereSlices; ++slice) {
            const auto a = static_cast<unsigned short>(stack * (kSphereSlices + 1) + slice);
            const auto b = static_cast<unsigned short>(a + kSphereSlices + 1);
            const auto c = static_cast<unsigned short>(b + 1);
            const auto d = static_cast<unsigned short>(a + 1);
            indices.insert(indices.end(), {a, c, b, a, d, c});
        }
    }
    sphereIndexCount_ = static_cast<int>(indices.size());

    glGenVertexArrays(1, &sphereVao_);
    glBindVertexArray(sphereVao_);
    glGenBuffers(1, &sphereVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, sphereVbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(positions.size() * sizeof(float)), positions.data(),
                 GL_STATIC_DRAW);
    glGenBuffers(1, &sphereEbo_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sphereEbo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(unsigned short)),
                 indices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

namespace {

unsigned MakeTarget(GLenum internalFormat, GLenum format, GLenum type, int width, int height,
                    GLenum filter = RT_GL_NEAREST) {
    unsigned texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat), width, height, 0, format, type, nullptr);
    // Every pass but bloom's reads its inputs texel for texel.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(filter));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(filter));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(RT_GL_CLAMP_TO_EDGE));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(RT_GL_CLAMP_TO_EDGE));
    return texture;
}

// The framebuffer bound now, with these attached. False when it cannot be drawn into.
bool Attach(std::initializer_list<unsigned> colors, unsigned depth) {
    GLenum buffers[4] = {};
    GLsizei count = 0;
    for (const unsigned color : colors) {
        buffers[count] = RT_GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(count);
        glFramebufferTexture2D(RT_GL_FRAMEBUFFER, buffers[count], GL_TEXTURE_2D, color, 0);
        ++count;
    }
    if (depth != 0) {
        glFramebufferTexture2D(RT_GL_FRAMEBUFFER, RT_GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth, 0);
    }
    glDrawBuffers(count, buffers);
    return glCheckFramebufferStatus(RT_GL_FRAMEBUFFER) == RT_GL_FRAMEBUFFER_COMPLETE;
}

// A new framebuffer, left bound, drawing into one mip level of texture. 0
// when it cannot be drawn into.
unsigned MakeLevelFbo(unsigned texture, int level) {
    unsigned fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(RT_GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(RT_GL_FRAMEBUFFER, RT_GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, level);
    const GLenum buffer = RT_GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1, &buffer);
    if (glCheckFramebufferStatus(RT_GL_FRAMEBUFFER) != RT_GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &fbo);
        return 0;
    }
    return fbo;
}

}  // namespace

bool Renderer::ensureTargets(int width, int height) {
    if (gbufferFbo_ != 0 && width == targetWidth_ && height == targetHeight_) {
        return true;
    }
    destroyTargets();
    targetWidth_ = width;
    targetHeight_ = height;
    albedoTexture_ = MakeTarget(RT_GL_RGBA16F, GL_RGBA, RT_GL_HALF_FLOAT, width, height);
    normalTexture_ = MakeTarget(RT_GL_RGBA16F, GL_RGBA, RT_GL_HALF_FLOAT, width, height);
    materialTexture_ = MakeTarget(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, width, height);
    emissiveTexture_ = MakeTarget(RT_GL_RGBA16F, GL_RGBA, RT_GL_HALF_FLOAT, width, height);
    depthTexture_ = MakeTarget(RT_GL_DEPTH_COMPONENT24, RT_GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, width, height);
    accumulationTexture_ = MakeTarget(RT_GL_RGBA16F, GL_RGBA, RT_GL_HALF_FLOAT, width, height);
    transparencyTexture_ = MakeTarget(RT_GL_RGBA16F, GL_RGBA, RT_GL_HALF_FLOAT, width, height);
    // Linear for bloom's first step, which reads between texels; the tone map
    // reads it texel for texel either way.
    mergeTexture_ = MakeTarget(RT_GL_RGBA16F, GL_RGBA, RT_GL_HALF_FLOAT, width, height, GL_LINEAR);
    // FXAA reads between texels.
    ldrTexture_ = MakeTarget(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, width, height, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    bool complete = true;
    glGenFramebuffers(1, &gbufferFbo_);
    glBindFramebuffer(RT_GL_FRAMEBUFFER, gbufferFbo_);
    complete = Attach({albedoTexture_, normalTexture_, materialTexture_, emissiveTexture_}, depthTexture_) && complete;
    glGenFramebuffers(1, &accumulationFbo_);
    glBindFramebuffer(RT_GL_FRAMEBUFFER, accumulationFbo_);
    complete = Attach({accumulationTexture_}, 0) && complete;
    glGenFramebuffers(1, &transparencyFbo_);
    glBindFramebuffer(RT_GL_FRAMEBUFFER, transparencyFbo_);
    complete = Attach({transparencyTexture_}, depthTexture_) && complete;
    glGenFramebuffers(1, &mergeFbo_);
    glBindFramebuffer(RT_GL_FRAMEBUFFER, mergeFbo_);
    complete = Attach({mergeTexture_}, 0) && complete;
    glGenFramebuffers(1, &ldrFbo_);
    glBindFramebuffer(RT_GL_FRAMEBUFFER, ldrFbo_);
    complete = Attach({ldrTexture_}, 0) && complete;
    if (!complete) {
        if (!targetsRefused_) {
            std::fprintf(stderr, "The Scene View's %d by %d render buffers are not supported.\n", width, height);
            targetsRefused_ = true;
        }
        destroyTargets();
        return false;
    }
    return true;
}

void Renderer::destroyTargets() {
    for (unsigned* fbo : {&gbufferFbo_, &accumulationFbo_, &transparencyFbo_, &mergeFbo_, &ldrFbo_}) {
        if (*fbo != 0) {
            glDeleteFramebuffers(1, fbo);
            *fbo = 0;
        }
    }
    for (unsigned* texture : {&albedoTexture_, &normalTexture_, &materialTexture_, &emissiveTexture_, &depthTexture_,
                              &accumulationTexture_, &transparencyTexture_, &mergeTexture_, &ldrTexture_}) {
        DeleteTexture(*texture);
    }
    destroyBloomChain();
    destroyReflectionBuffers();
    destroyOcclusionBuffers();
    fxaaValid_ = false;
    targetWidth_ = 0;
    targetHeight_ = 0;
}

bool Renderer::ensureBloomChain(int width, int height) {
    if (bloomLevelsMade_ > 0 && width == bloomWidth_ && height == bloomHeight_) {
        return true;
    }
    if (width == bloomRefusedWidth_ && height == bloomRefusedHeight_) {
        return false;
    }
    destroyBloomChain();
    // Every level the pane has room for, so a change of Size never makes the chain again.
    const int levels = BloomLevelsAvailable(width, height);
    bool complete = levels > 0;
    for (int k = 0; k < levels; ++k) {
        bloomTextures_[k] =
            MakeTarget(RT_GL_RGBA16F, GL_RGBA, RT_GL_HALF_FLOAT, width >> (k + 1), height >> (k + 1), GL_LINEAR);
        glGenFramebuffers(1, &bloomFbos_[k]);
        glBindFramebuffer(RT_GL_FRAMEBUFFER, bloomFbos_[k]);
        complete = Attach({bloomTextures_[k]}, 0) && complete;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    if (!complete) {
        std::fprintf(stderr, "The Scene View's %d by %d bloom buffers are not supported; drawing without bloom.\n",
                     width, height);
        destroyBloomChain();
        bloomRefusedWidth_ = width;
        bloomRefusedHeight_ = height;
        return false;
    }
    bloomLevelsMade_ = levels;
    bloomWidth_ = width;
    bloomHeight_ = height;
    return true;
}

void Renderer::destroyBloomChain() {
    for (int k = 0; k < kBloomMaxLevels; ++k) {
        if (bloomFbos_[k] != 0) {
            glDeleteFramebuffers(1, &bloomFbos_[k]);
            bloomFbos_[k] = 0;
        }
        DeleteTexture(bloomTextures_[k]);
    }
    bloomLevelsMade_ = 0;
    bloomWidth_ = 0;
    bloomHeight_ = 0;
}

bool Renderer::ensureReflectionBuffers(int width, int height) {
    if (reflectionFbo_ != 0 && width == reflectionWidth_ && height == reflectionHeight_) {
        return true;
    }
    if (width == reflectionRefusedWidth_ && height == reflectionRefusedHeight_) {
        return false;
    }
    destroyReflectionBuffers();
    const int halfWidth = std::max(width >> 1, 1);
    const int halfHeight = std::max(height >> 1, 1);
    reflectSceneTexture_ = MakeTarget(RT_GL_RGBA16F, GL_RGBA, RT_GL_HALF_FLOAT, halfWidth, halfHeight, GL_LINEAR);
    // Mip-filtered: rough reflections read a blurrier level instead of taking more taps.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR_MIPMAP_LINEAR));
    glGenerateMipmap(GL_TEXTURE_2D);
    reflectSceneLevels_ = 1;
    for (int side = std::max(halfWidth, halfHeight); side > 1; side >>= 1) {
        ++reflectSceneLevels_;
    }
    // The blur's halfway levels, read texel for texel at a chosen level.
    reflectBlurTexture_ = MakeTarget(RT_GL_RGBA16F, GL_RGBA, RT_GL_HALF_FLOAT, halfWidth, halfHeight);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(RT_GL_NEAREST_MIPMAP_NEAREST));
    glGenerateMipmap(GL_TEXTURE_2D);
    reflectionTexture_ = MakeTarget(RT_GL_RGBA16F, GL_RGBA, RT_GL_HALF_FLOAT, halfWidth, halfHeight);
    glBindTexture(GL_TEXTURE_2D, 0);
    bool complete = true;
    glGenFramebuffers(1, &reflectSceneFbo_);
    glBindFramebuffer(RT_GL_FRAMEBUFFER, reflectSceneFbo_);
    complete = Attach({reflectSceneTexture_}, 0) && complete;
    for (int level = 1; level < reflectSceneLevels_; ++level) {
        reflectBlurFbos_.push_back(MakeLevelFbo(reflectBlurTexture_, level));
        reflectSceneLevelFbos_.push_back(MakeLevelFbo(reflectSceneTexture_, level));
        complete = reflectBlurFbos_.back() != 0 && reflectSceneLevelFbos_.back() != 0 && complete;
    }
    glGenFramebuffers(1, &reflectionFbo_);
    glBindFramebuffer(RT_GL_FRAMEBUFFER, reflectionFbo_);
    complete = Attach({reflectionTexture_}, 0) && complete;
    if (!complete) {
        std::fprintf(stderr,
                     "The Scene View's %d by %d reflection buffers are not supported; drawing without reflections.\n",
                     width, height);
        destroyReflectionBuffers();
        reflectionRefusedWidth_ = width;
        reflectionRefusedHeight_ = height;
        return false;
    }
    reflectionWidth_ = width;
    reflectionHeight_ = height;
    return true;
}

void Renderer::destroyReflectionBuffers() {
    for (unsigned* fbo : {&reflectSceneFbo_, &reflectionFbo_}) {
        if (*fbo != 0) {
            glDeleteFramebuffers(1, fbo);
            *fbo = 0;
        }
    }
    for (std::vector<unsigned>* fbos : {&reflectBlurFbos_, &reflectSceneLevelFbos_}) {
        for (unsigned& fbo : *fbos) {
            if (fbo != 0) {
                glDeleteFramebuffers(1, &fbo);
            }
        }
        fbos->clear();
    }
    DeleteTexture(reflectSceneTexture_);
    DeleteTexture(reflectBlurTexture_);
    DeleteTexture(reflectionTexture_);
    reflectSceneLevels_ = 0;
    reflectionsValid_ = false;
    reflectionWidth_ = 0;
    reflectionHeight_ = 0;
}

bool Renderer::ensureOcclusionBuffers(int width, int height, int scale) {
    if (occlusionFbo_ != 0 && width == occlusionWidth_ && height == occlusionHeight_ && scale == occlusionScale_) {
        return true;
    }
    if (width == occlusionRefusedWidth_ && height == occlusionRefusedHeight_ && scale == occlusionRefusedScale_) {
        return false;
    }
    destroyOcclusionBuffers();
    const int w = std::max(width / scale, 1);
    const int h = std::max(height / scale, 1);
    occlusionTexture_ = MakeTarget(RT_GL_R8, RT_GL_RED, GL_UNSIGNED_BYTE, w, h);
    occlusionBlurTexture_ = MakeTarget(RT_GL_R8, RT_GL_RED, GL_UNSIGNED_BYTE, w, h);
    glBindTexture(GL_TEXTURE_2D, 0);
    bool complete = true;
    glGenFramebuffers(1, &occlusionFbo_);
    glBindFramebuffer(RT_GL_FRAMEBUFFER, occlusionFbo_);
    complete = Attach({occlusionTexture_}, 0) && complete;
    glGenFramebuffers(1, &occlusionBlurFbo_);
    glBindFramebuffer(RT_GL_FRAMEBUFFER, occlusionBlurFbo_);
    complete = Attach({occlusionBlurTexture_}, 0) && complete;
    if (!complete) {
        std::fprintf(stderr,
                     "The Scene View's %d by %d occlusion buffers are not supported; drawing without ambient "
                     "occlusion.\n",
                     w, h);
        destroyOcclusionBuffers();
        occlusionRefusedWidth_ = width;
        occlusionRefusedHeight_ = height;
        occlusionRefusedScale_ = scale;
        return false;
    }
    occlusionWidth_ = width;
    occlusionHeight_ = height;
    occlusionScale_ = scale;
    return true;
}

void Renderer::destroyOcclusionBuffers() {
    for (unsigned* fbo : {&occlusionFbo_, &occlusionBlurFbo_}) {
        if (*fbo != 0) {
            glDeleteFramebuffers(1, fbo);
            *fbo = 0;
        }
    }
    DeleteTexture(occlusionTexture_);
    DeleteTexture(occlusionBlurTexture_);
    occlusionWidth_ = 0;
    occlusionHeight_ = 0;
    occlusionScale_ = 0;
    occlusionValid_ = false;
}

namespace {

struct PixelRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

PixelRect Intersect(PixelRect a, PixelRect b) {
    const int x1 = std::max(a.x, b.x);
    const int y1 = std::max(a.y, b.y);
    const int x2 = std::min(a.x + a.width, b.x + b.width);
    const int y2 = std::min(a.y + a.height, b.y + b.height);
    PixelRect out;
    out.x = x1;
    out.y = y1;
    out.width = std::max(0, x2 - x1);
    out.height = std::max(0, y2 - y1);
    return out;
}

// Window points, origin top left, mapped into the current GL viewport.
// The viewport is the framebuffer; its origin is the bottom left.
PixelRect PanePixels(double x, double y, double width, double height, double sceneWidth, double sceneHeight,
                     const GLint viewport[4]) {
    const double scaleX = static_cast<double>(viewport[2]) / sceneWidth;
    const double scaleY = static_cast<double>(viewport[3]) / sceneHeight;
    const int x0 = viewport[0] + static_cast<int>(std::floor(x * scaleX));
    const int x1 = viewport[0] + static_cast<int>(std::ceil((x + width) * scaleX));
    const int top = static_cast<int>(std::floor(y * scaleY));
    const int bottom = static_cast<int>(std::ceil((y + height) * scaleY));
    PixelRect rect;
    rect.x = x0;
    rect.width = std::max(0, x1 - x0);
    rect.height = std::max(0, bottom - top);
    rect.y = viewport[1] + viewport[3] - bottom;
    return rect;
}

// Column-major 4x4, as Transform and GLSL store them.
using Matrix = float[16];

void Multiply(const float* a, const float* b, float* out) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.f;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[column * 4 + k];
            }
            out[column * 4 + row] = sum;
        }
    }
}

// The GL state a draw changes, so the UI pass after it finds its own. That
// includes what every texture unit has bound to each target the passes use.
// JadeFX binds unit 7 only in setOccluder, to the scene's depth texture,
// and its UI draws sample it there until clearOccluder; outside that window
// the unit is free. A 3D draw issued inside the window would otherwise break
// the occluder, so a draw must leave unit 7's 2D binding as it found it, and
// GL_TEXTURE0 active; JadeFX binds unit 0 again on every text and image draw
// itself. Any other GL drawn between UI draws must do the same. Saved once a
// draw, not once a pass: a few dozen queries for each 3D view each frame.
struct SavedState {
    GLint framebuffer = 0;
    GLint scissorBox[4] = {};
    GLboolean scissor = GL_FALSE;
    GLboolean blend = GL_FALSE;
    GLint blendSrcRgb = 0;
    GLint blendDstRgb = 0;
    GLint blendSrcAlpha = 0;
    GLint blendDstAlpha = 0;
    GLboolean depthTest = GL_FALSE;
    GLboolean depthMask = GL_TRUE;
    GLint depthFunc = 0;
    GLboolean cull = GL_FALSE;
    GLint cullMode = 0;
    GLint program = 0;
    GLint vertexArray = 0;
    GLint arrayBuffer = 0;
    GLint activeTexture = 0;
    GLint textures[kUnitCount] = {};
    GLint cubes[kUnitCount] = {};
    GLint arrays[kUnitCount] = {};
    GLboolean seamlessCubes = GL_FALSE;

    SavedState() {
        glGetIntegerv(RT_GL_FRAMEBUFFER_BINDING, &framebuffer);
        glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
        blend = glIsEnabled(GL_BLEND);
        glGetIntegerv(RT_GL_BLEND_SRC_RGB, &blendSrcRgb);
        glGetIntegerv(RT_GL_BLEND_DST_RGB, &blendDstRgb);
        glGetIntegerv(RT_GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
        glGetIntegerv(RT_GL_BLEND_DST_ALPHA, &blendDstAlpha);
        depthTest = glIsEnabled(GL_DEPTH_TEST);
        glGetBooleanv(RT_GL_DEPTH_WRITEMASK, &depthMask);
        glGetIntegerv(RT_GL_DEPTH_FUNC, &depthFunc);
        cull = glIsEnabled(RT_GL_CULL_FACE);
        glGetIntegerv(RT_GL_CULL_FACE_MODE, &cullMode);
        glGetIntegerv(RT_GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(RT_GL_VERTEX_ARRAY_BINDING, &vertexArray);
        glGetIntegerv(RT_GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        for (int unit = 0; unit < kUnitCount; ++unit) {
            glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &textures[unit]);
            glGetIntegerv(RT_GL_TEXTURE_BINDING_CUBE_MAP, &cubes[unit]);
            glGetIntegerv(RT_GL_TEXTURE_BINDING_2D_ARRAY, &arrays[unit]);
        }
        glActiveTexture(static_cast<GLenum>(activeTexture));
        seamlessCubes = glIsEnabled(RT_GL_TEXTURE_CUBE_MAP_SEAMLESS);
    }

    static void Set(GLenum cap, GLboolean on) {
        if (on == GL_TRUE) {
            glEnable(cap);
        } else {
            glDisable(cap);
        }
    }

    // deleted holds the texture names the draw deleted: a unit that had one
    // bound is left empty, since binding a deleted name is a GL error.
    void restore(const GLint viewport[4], const DeletedTextures& deleted) const {
        glBindFramebuffer(RT_GL_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
        Set(GL_SCISSOR_TEST, scissor);
        Set(GL_BLEND, blend);
        glBlendFuncSeparate(static_cast<GLenum>(blendSrcRgb), static_cast<GLenum>(blendDstRgb),
                            static_cast<GLenum>(blendSrcAlpha), static_cast<GLenum>(blendDstAlpha));
        Set(GL_DEPTH_TEST, depthTest);
        glDepthMask(depthMask);
        glDepthFunc(static_cast<GLenum>(depthFunc));
        Set(RT_GL_CULL_FACE, cull);
        glCullFace(static_cast<GLenum>(cullMode));
        Set(RT_GL_TEXTURE_CUBE_MAP_SEAMLESS, seamlessCubes);
        glUseProgram(static_cast<GLuint>(program));
        glBindVertexArray(static_cast<GLuint>(vertexArray));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer));
        // Every unit gets back what it had on the 2D, cube map, and 2D array
        // targets, except a name the draw deleted, which is left at 0 even if
        // GL has since given that name to a new texture. A unit left empty
        // draws wrong pixels at worst; a deleted name bound back is an error.
        const auto kept = [&deleted](GLint texture) {
            const GLuint name = static_cast<GLuint>(texture);
            return deleted.contains(name) ? 0u : name;
        };
        for (int unit = kUnitCount - 1; unit >= 0; --unit) {
            glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
            glBindTexture(RT_GL_TEXTURE_2D_ARRAY, kept(arrays[unit]));
            glBindTexture(RT_GL_TEXTURE_CUBE_MAP, kept(cubes[unit]));
            glBindTexture(GL_TEXTURE_2D, kept(textures[unit]));
        }
        glActiveTexture(static_cast<GLenum>(activeTexture));
    }
};

}  // namespace

bool Renderer::draw(double x, double y, double width, double height, double sceneWidth, double sceneHeight,
                    const MeshDraw* meshes, int meshCount, const LightDraw* lights, int lightCount) {
    // Nothing to hide behind, and no depth under the probe, until this draw leaves some.
    sceneDepth_ = SceneDepth{};
    probedDepth_.reset();
    stats_ = RenderStats{};
    if (!ready_ || width <= 0.0 || height <= 0.0 || sceneWidth <= 0.0 || sceneHeight <= 0.0) {
        return false;
    }
    // The 3D draw runs on the UI thread, but the profiler shows it in the Render section.
    const profiler::RowScope row("Render draw");
    // On the GPU, the whole draw is one scope, unless each pass is timed instead:
    // every timed pass stalls the CPU on macOS, and inflates what it measures.
    static const profiler::ScopeId kScene = profiler::intern("3D scene", profiler::Group::Gpu);
    struct SceneTimer {
        GpuTimer& gpu;
        SceneTimer(GpuTimer& timer, profiler::ScopeId scope) : gpu(timer) { gpu.begin(scope, !profiler::gpu_detail()); }
        ~SceneTimer() { gpu.end(); }
        SceneTimer(const SceneTimer&) = delete;
        SceneTimer& operator=(const SceneTimer&) = delete;
    } sceneTimer(gpu_, kScene);
    // The same draw on the CPU: the root its passes nest under on the Render draw row.
    PROFILE_SCOPE("3D scene", profiler::Group::Render);

    GLint viewport[4] = {};
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] <= 0 || viewport[3] <= 0) {
        return false;
    }

    const PixelRect pane = PanePixels(x, y, width, height, sceneWidth, sceneHeight, viewport);
    if (pane.width <= 0 || pane.height <= 0) {
        return false;
    }
    pixelsPerPoint_ = static_cast<float>(static_cast<double>(viewport[3]) / sceneHeight);

    // Open before the state is saved, so every texture the draw deletes,
    // its targets on a resize or its shadow maps, is known to restore.
    DeletedTextures deleted;
    const SavedState saved;
    PixelRect clip = pane;
    if (saved.scissor == GL_TRUE) {
        const PixelRect outer{saved.scissorBox[0], saved.scissorBox[1], saved.scissorBox[2], saved.scissorBox[3]};
        clip = Intersect(pane, outer);
        if (clip.width <= 0 || clip.height <= 0) {
            return false;
        }
    }

    // Scissor limits the clear to this pane. The viewport stays the whole pane
    // so a parent clip cuts pixels without sliding the drawing.
    glEnable(GL_SCISSOR_TEST);
    glScissor(clip.x, clip.y, clip.width, clip.height);
    glViewport(pane.x, pane.y, pane.width, pane.height);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glClearColor(clear_[0], clear_[1], clear_[2], 1.0f);
    // The floor grid's bands mark the pane's depth as they draw. Cleared here, with
    // the color, rather than just before the grid: a clear between draws to the
    // pane would make the GPU write the pane out and read it back in between.
    if (gridVisible_ && paneHasDepth(saved.framebuffer)) {
        glDepthMask(GL_TRUE);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    } else {
        glClear(GL_COLOR_BUFFER_BIT);
    }

    const bool hasMeshes = meshes != nullptr && meshCount > 0;
    if (!hasMeshes) {
        meshes = nullptr;
        meshCount = 0;
    }
    // A sky whose cubes can never be made is drawn as no sky, rather than never drawing.
    const bool hasSky = environment_.available() && (dynamicSkyDrawn() || lighting_.sky.image != 0);
    const engine_core::Matrix4 projectionMatrix =
        Perspective(fovYDegrees_, static_cast<float>(pane.width) / static_cast<float>(pane.height), kSceneNear,
                    kSceneFar);
    const float* projection = projectionMatrix.m;
    const engine_core::Matrix4 inverseProjection = engine_core::matrix4_inverse(projectionMatrix);

    bool drawn = !hasMeshes && !hasSky;
    int bloomLevels = 0;
    if (!drawn && ensureTargets(pane.width, pane.height)) {
        // The lights in view space, as every pass takes them.
        viewLights_.clear();
        shadowRequests_.clear();
        hasSunShadow_ = false;
        for (int index = 0; lights != nullptr && index < lightCount; ++index) {
            const LightDraw& light = lights[index];
            const bool directional = light.kind == LightDraw::Kind::Directional;
            if ((!directional && !(light.radius > 0.f)) || !(light.intensity > 0.f)) {
                continue;
            }
            ViewLight out{};
            const float* v = view_.m;
            const float* p = light.position;
            const float* d = light.direction;
            for (int row = 0; row < 3; ++row) {
                out.position[row] = v[row] * p[0] + v[4 + row] * p[1] + v[8 + row] * p[2] + v[12 + row];
                out.direction[row] = v[row] * d[0] + v[4 + row] * d[1] + v[8 + row] * d[2];
            }
            const float length = std::sqrt(out.direction[0] * out.direction[0] + out.direction[1] * out.direction[1] +
                                           out.direction[2] * out.direction[2]);
            for (float& value : out.direction) {
                value = length > 0.f ? value / length : 0.f;
            }
            if (directional) {
                // A sun that faces no way, as with a Transform scaled to nothing, lights nothing.
                if (!(length > 0.f)) {
                    continue;
                }
                out.cone[0] = -4.f;
                out.cone[1] = -4.f;
            } else if (light.kind == LightDraw::Kind::Spot) {
                constexpr float kHalfDegree = 0.5f * 0.01745329252f;
                const float outer = std::clamp(light.outerFovDegrees, 0.f, 180.f);
                const float inner = outer * std::clamp(light.innerFovScale, 0.f, 1.f);
                out.cone[0] = std::cos(outer * kHalfDegree);
                // Equal edges would leave no fade, which smoothstep cannot take.
                out.cone[1] = std::max(std::cos(inner * kHalfDegree), out.cone[0] + 1e-4f);
            } else {
                out.cone[0] = -2.f;
                out.cone[1] = -2.f;
            }
            std::copy(light.color, light.color + 3, out.color);
            out.radius = light.radius;
            out.intensity = light.intensity;
            if (light.shadows && shadowSettings_.enabled && !directional) {
                ShadowRequest request;
                request.key = light.id != 0 ? light.id : kUncachedShadowKey | static_cast<std::uint64_t>(index);
                request.cached = light.id != 0;
                request.owner = light.id;
                request.kind = light.kind == LightDraw::Kind::Spot ? ShadowKind::Spot : ShadowKind::Point;
                request.position = {p[0], p[1], p[2]};
                request.direction = Normalize({d[0], d[1], d[2]});
                request.radius = light.radius;
                request.outerFovDegrees = light.outerFovDegrees;
                out.shadow = static_cast<int>(shadowRequests_.size());
                shadowRequests_.push_back(request);
            }
            if (light.shadows && shadowSettings_.enabled && directional && !hasSunShadow_) {
                sunShadow_.owner = light.id;
                sunShadow_.shine = Normalize({d[0], d[1], d[2]});
                sunShadow_.shadowDistance = light.shadowDistance;
                hasSunShadow_ = true;
                out.shadow = kSunShadow;
            }
            viewLights_.push_back(out);
        }
        // Suns first, so the see-through pass, which takes kMaxForwardLights,
        // keeps every light that reaches everything.
        std::stable_partition(viewLights_.begin(), viewLights_.end(),
                              [](const ViewLight& light) { return light.cone[0] < -3.f; });

        // The sky's cubes: an image's made again only when it changes; a
        // DynamicSky's when LightingDue says, and never failing the frame.
        skyReady_ = false;
        skyVisible_ = false;
        bool cubesReady = true;
        if (hasSky && dynamicSkyDrawn()) {
            skyReady_ = updateDynamicSkyLighting();
            // Drawn straight from the shader every frame it is due, whether
            // or not its lighting cube (skyReady_) is ready yet.
            skyVisible_ = true;
            prepareSky();
        } else if (hasSky) {
            const SceneSky& sky = lighting_.sky;
            cubesReady = environment_.update(sky.image, sky.imageRevision, emptyVao_);
            skyReady_ = cubesReady;
            skyVisible_ = skyReady_;
            prepareSky();
        }
        glEnable(RT_GL_TEXTURE_CUBE_MAP_SEAMLESS);

        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, targetWidth_, targetHeight_);
        const CameraView camera = cameraView(projection);
        findVisible(meshes, meshCount, camera);
        drawn = cubesReady && shadowPass(meshes, meshCount, camera) &&
                geometryPass(meshes, projection);
        // Occlusion never fails the frame: without it, surfaces are lit as if open.
        if (drawn) {
            occlusionPass(projection, inverseProjection.m);
        } else {
            occlusionReady_ = false;
        }
        drawn = drawn && lightPass(projection, inverseProjection.m) && skyPass(inverseProjection.m);
        // Reflections never fail the frame: without them, surfaces keep their sky reflection.
        const bool reflected = drawn && reflectionsPass(projection, inverseProjection.m);
        glViewport(0, 0, targetWidth_, targetHeight_);
        drawn = drawn && transparencyPass(meshes, projection, inverseProjection.m) &&
                mergePass(reflected, inverseProjection.m);
        // Bloom never fails the frame: without it, the tone map draws the scene as it is.
        bloomLevels = drawn ? bloomPass(targetWidth_, targetHeight_) : 0;
    }
    if (drawn && (hasMeshes || hasSky)) {
        glDisable(GL_DEPTH_TEST);
        glDisable(RT_GL_CULL_FACE);
        // The pane, with its parent's clip.
        const auto bindPane = [&] {
            glBindFramebuffer(RT_GL_FRAMEBUFFER, static_cast<GLuint>(saved.framebuffer));
            glEnable(GL_SCISSOR_TEST);
            glScissor(clip.x, clip.y, clip.width, clip.height);
            glViewport(pane.x, pane.y, pane.width, pane.height);
        };
        // Blended over the clear: where nothing was drawn the pane shows through.
        const auto blendOverClear = [] {
            glEnable(GL_BLEND);
            glBlendFuncSeparate(RT_GL_ONE, RT_GL_ONE_MINUS_SRC_ALPHA, RT_GL_ZERO, RT_GL_ONE);
        };
        bool onPane = false;
        if (lighting_.antialiasing == SceneAntialiasing::FXAA) {
            // The tone map into ldrTexture_, over the pane's clear color, so it
            // holds what the pane would have shown; then FXAA onto the pane.
            glBindFramebuffer(RT_GL_FRAMEBUFFER, ldrFbo_);
            glDisable(GL_SCISSOR_TEST);
            glViewport(0, 0, targetWidth_, targetHeight_);
            glDisable(GL_BLEND);
            glClearColor(clear_[0], clear_[1], clear_[2], 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            blendOverClear();
            if (toneMapPass(bloomLevels)) {
                bindPane();
                glDisable(GL_BLEND);
                onPane = fxaaPass();
            }
        }
        // Without FXAA, or when it cannot draw yet: the tone map straight onto the pane.
        if (!onPane) {
            bindPane();
            blendOverClear();
            onPane = toneMapPass(bloomLevels);
        }
        drawn = onPane;
        if (drawn) {
            sceneDepth_ = SceneDepth{depthTexture_, pane.x, pane.y, pane.width, pane.height};
            readProbe(pane.x, pane.y, pane.width, pane.height, sceneWidth, sceneHeight, viewport);
        }
    }
    // Over a pane that got only the clear, the grid would show through the
    // surfaces that should hide it, so it waits for a frame that draws them.
    if (drawn && gridVisible_) {
        gridPass(hasMeshes || hasSky ? depthTexture_ : whiteTexture_, projection, inverseProjection.m, pane.width,
                 pane.height);
    }
    // Likewise the outlines, which would show at full strength through them.
    if (drawn && !outlines_.empty()) {
        outlinePass(hasMeshes || hasSky ? depthTexture_ : whiteTexture_, projection, inverseProjection.m);
    }
    // The handles last, over everything, so they can always be grabbed.
    if (drawn && !handles_.empty()) {
        handlePass(projection);
    }

    saved.restore(viewport, deleted);
    gpu_.frame();
    return drawn;
}

void Renderer::readProbe(int paneX, int paneY, int paneWidth, int paneHeight, double sceneWidth,
                         double sceneHeight, const int viewport[4]) {
    if (probeBuffers_[0] == 0) {
        glGenBuffers(2, probeBuffers_);
        for (unsigned buffer : probeBuffers_) {
            glBindBuffer(RT_GL_PIXEL_PACK_BUFFER, buffer);
            glBufferData(RT_GL_PIXEL_PACK_BUFFER, sizeof(float), nullptr, RT_GL_STREAM_READ);
        }
        glBindBuffer(RT_GL_PIXEL_PACK_BUFFER, 0);
    }
    // Last draw's read, which the GPU has long finished.
    const int previous = 1 - probeNext_;
    probedDepth_.reset();
    if (probeFilled_[previous]) {
        glBindBuffer(RT_GL_PIXEL_PACK_BUFFER, probeBuffers_[previous]);
        if (const void* mapped = glMapBufferRange(RT_GL_PIXEL_PACK_BUFFER, 0, sizeof(float), RT_GL_MAP_READ_BIT)) {
            probedDepth_ = *static_cast<const float*>(mapped);
            glUnmapBuffer(RT_GL_PIXEL_PACK_BUFFER);
        }
        probeFilled_[previous] = false;
    }
    probeFilled_[probeNext_] = false;
    if (probeX_ >= 0.0 && probeY_ >= 0.0) {
        const double scaleX = static_cast<double>(viewport[2]) / sceneWidth;
        const double scaleY = static_cast<double>(viewport[3]) / sceneHeight;
        const int px = viewport[0] + static_cast<int>(std::floor(probeX_ * scaleX)) - paneX;
        const int py = viewport[1] + viewport[3] - 1 - static_cast<int>(std::floor(probeY_ * scaleY)) - paneY;
        if (px >= 0 && py >= 0 && px < paneWidth && py < paneHeight) {
            glBindFramebuffer(RT_GL_READ_FRAMEBUFFER, gbufferFbo_);
            glBindBuffer(RT_GL_PIXEL_PACK_BUFFER, probeBuffers_[probeNext_]);
            glReadPixels(px, py, 1, 1, RT_GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
            probeFilled_[probeNext_] = true;
        }
    }
    glBindBuffer(RT_GL_PIXEL_PACK_BUFFER, 0);
    probeNext_ = previous;
}

bool Renderer::paneHasDepth(int framebuffer) {
    if (framebuffer == depthFramebuffer_) {
        return depthFramebufferHas_;
    }
    depthFramebuffer_ = framebuffer;
    depthFramebufferHas_ = false;
    if (rt_glGetFramebufferAttachmentParameteriv != nullptr) {
        // The window's own framebuffer names its depth buffer GL_DEPTH; one made with
        // glGenFramebuffers, GL_DEPTH_ATTACHMENT. Either reads GL_NONE without one.
        GLint type = 0;
        glGetFramebufferAttachmentParameteriv(RT_GL_DRAW_FRAMEBUFFER,
                                              framebuffer == 0 ? RT_GL_DEPTH : RT_GL_DEPTH_ATTACHMENT,
                                              RT_GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
        depthFramebufferHas_ = type != 0;
    }
    return depthFramebufferHas_;
}

void Renderer::gridPass(unsigned depth, const float* projection, const float* inverseProjection, int width,
                        int height) {
    RENDER_PASS("Floor grid");
    glDisable(GL_DEPTH_TEST);
    glDisable(RT_GL_CULL_FACE);
    glEnable(GL_BLEND);
    // Straight alpha over the pane, leaving its alpha as it was.
    glBlendFuncSeparate(RT_GL_SRC_ALPHA, RT_GL_ONE_MINUS_SRC_ALPHA, RT_GL_ZERO, RT_GL_ONE);
    GLint framebuffer = 0;
    glGetIntegerv(RT_GL_FRAMEBUFFER_BINDING, &framebuffer);
    // The grid shades only GridBands' bands around its lines. Where they cross,
    // a pixel must still be blended once, which the pane's depth buffer keeps
    // track of; without one, the whole pane is shaded as it used to be.
    const bool banded = paneHasDepth(framebuffer);
    const Program& program = banded ? gridBands_ : grid_;
    glUseProgram(program.id);
    BindTexture(kUnitDepth, depth);
    const engine_core::Matrix4 inverseView = engine_core::matrix4_inverse(view_);
    glUniformMatrix4fv(program.inverseProjection, 1, GL_FALSE, inverseProjection);
    glUniformMatrix4fv(program.inverseView, 1, GL_FALSE, inverseView.m);
    // Validation looks at the bound vertex array too, so it is bound first.
    glBindVertexArray(banded ? gridBandVao_ : emptyVao_);
    // A grid the driver is not ready for is left out of this frame, not the scene
    // with it. Validating the banded program is slow on macOS, so it is asked
    // until it passes, then trusted until the programs are made again.
    if (!banded) {
        if (CanDraw(program.id)) {
            DrawFullscreen(emptyVao_);
        }
        return;
    }
    if (!gridBandsValid_) {
        if (!CanDraw(program.id)) {
            return;
        }
        gridBandsValid_ = true;
    }
    GridView view;
    view.view = view_;
    std::copy(projection, projection + 16, view.projection.m);
    view.width = width;
    view.height = height;
    glBindBuffer(GL_ARRAY_BUFFER, gridBandVbo_);
    // Built again only when the camera or the pane changes.
    if (!gridBandsBuilt_ || !engine_core::same_matrix4(view.view, gridBandView_.view) ||
        !engine_core::same_matrix4(view.projection, gridBandView_.projection) || view.width != gridBandView_.width ||
        view.height != gridBandView_.height) {
        build_grid_bands(view, gridBandTriangles_);
        gridBandView_ = view;
        gridBandsBuilt_ = true;
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(gridBandTriangles_.size() * sizeof(float)),
                     gridBandTriangles_.empty() ? nullptr : gridBandTriangles_.data(), RT_GL_STREAM_DRAW);
    }
    if (gridBandTriangles_.empty()) {
        return;
    }
    // Each pixel once: the first band to reach it marks it in depth, which draw
    // cleared with the pane.
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(RT_GL_LESS);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(gridBandTriangles_.size() / 2));
    glDisable(GL_DEPTH_TEST);
}

void Renderer::setOutlines(const float* points, int pointCount) {
    outlines_.assign(points, points + std::max(pointCount, 0) / 2 * 2 * 3);
}

void Renderer::outlinePass(unsigned depth, const float* projection, const float* inverseProjection) {
    RENDER_PASS("Outlines");
    glDisable(GL_DEPTH_TEST);
    glDisable(RT_GL_CULL_FACE);
    glEnable(GL_BLEND);
    // Straight alpha over the pane, leaving its alpha as it was.
    glBlendFuncSeparate(RT_GL_SRC_ALPHA, RT_GL_ONE_MINUS_SRC_ALPHA, RT_GL_ZERO, RT_GL_ONE);
    glUseProgram(outline_.id);
    BindTexture(kUnitDepth, depth);
    glUniformMatrix4fv(outline_.view, 1, GL_FALSE, view_.m);
    glUniformMatrix4fv(outline_.projection, 1, GL_FALSE, projection);
    glUniformMatrix4fv(outline_.inverseProjection, 1, GL_FALSE, inverseProjection);
    // The UI pass after this one finds the buffer it had bound.
    GLint arrayBuffer = 0;
    glGetIntegerv(RT_GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
    glBindVertexArray(outlineVao_);
    glBindBuffer(GL_ARRAY_BUFFER, outlineVbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(outlines_.size() * sizeof(float)), outlines_.data(),
                 GL_DYNAMIC_DRAW);
    if (CanDraw(outline_.id)) {
        glDrawArrays(RT_GL_LINES, 0, static_cast<GLsizei>(outlines_.size() / 3));
    }
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer));
}

void Renderer::setHandles(const engine_core::HandleVertex* vertices, int count) {
    handles_.assign(vertices, vertices + std::max(count, 0) / 3 * 3);
}

void Renderer::handlePass(const float* projection) {
    RENDER_PASS("Handles");
    glDisable(GL_DEPTH_TEST);
    glDisable(RT_GL_CULL_FACE);
    glEnable(GL_BLEND);
    // Straight alpha over the pane, leaving its alpha as it was.
    glBlendFuncSeparate(RT_GL_SRC_ALPHA, RT_GL_ONE_MINUS_SRC_ALPHA, RT_GL_ZERO, RT_GL_ONE);
    glUseProgram(handle_.id);
    glUniformMatrix4fv(handle_.view, 1, GL_FALSE, view_.m);
    glUniformMatrix4fv(handle_.projection, 1, GL_FALSE, projection);
    // The UI pass after this one finds the buffer it had bound.
    GLint arrayBuffer = 0;
    glGetIntegerv(RT_GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
    glBindVertexArray(handleVao_);
    glBindBuffer(GL_ARRAY_BUFFER, handleVbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(handles_.size() * sizeof(engine_core::HandleVertex)),
                 handles_.data(), GL_DYNAMIC_DRAW);
    if (CanDraw(handle_.id)) {
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(handles_.size()));
    }
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer));
}

void Renderer::bindMaterial(const Program& program, const MeshDraw& draw) {
    BindTexture(kUnitDiffuse, draw.texture != 0 ? draw.texture : whiteTexture_);
    BindTexture(kUnitNormalMap, draw.normalTexture != 0 ? draw.normalTexture : whiteTexture_);
    BindTexture(kUnitRoughnessMap, draw.roughnessTexture != 0 ? draw.roughnessTexture : whiteTexture_);
    BindTexture(kUnitMetalnessMap, draw.metalnessTexture != 0 ? draw.metalnessTexture : whiteTexture_);
    // Also bound with no map: it puts the G-buffer's albedo, which the
    // geometry pass draws into, off this unit.
    BindTexture(kUnitEmissiveMap, draw.emissiveTexture != 0 ? draw.emissiveTexture : whiteTexture_);
    glUniform4f(program.color, draw.color[0], draw.color[1], draw.color[2], draw.color[3]);
    glUniform3f(program.emissive, draw.emissive[0], draw.emissive[1], draw.emissive[2]);
    glUniform1f(program.metalness, std::clamp(draw.metalness, 0.f, 1.f));
    glUniform1f(program.roughness, std::clamp(draw.roughness, 0.f, 1.f));
    glUniform1f(program.reflectivity, std::clamp(draw.reflectivity, 0.f, 1.f));
    glUniform1f(program.normalMapEnabled, draw.normalTexture != 0 ? 1.f : 0.f);
    glUniform1f(program.emissiveMapEnabled, draw.emissiveTexture != 0 ? 1.f : 0.f);
    glUniform1f(program.transparency, std::clamp(draw.transparency, 0.f, 1.f));
}

void Renderer::prepareSky() {
    // The view's turn back to the world's: view_ is a rotation and a
    // translation, so its 3 by 3 transposed. Column-major, as GLSL takes it.
    float viewToWorld[9];
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            viewToWorld[column * 3 + row] = view_.m[row * 4 + column];
        }
    }
    // Then a Skybox turned back by its Rotation (a DynamicSky does not turn): what the camera sees at a
    // world direction is the sky's at that direction turned by -Rotation.
    const bool dynamic = dynamicSkyDrawn();
    const float angle = dynamic ? 0.f : -lighting_.sky.rotationDegrees * 0.01745329252f;
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const float turn[9] = {c, 0.f, -s, 0.f, 1.f, 0.f, s, 0.f, c};
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            float sum = 0.f;
            for (int k = 0; k < 3; ++k) {
                sum += turn[k * 3 + row] * viewToWorld[column * 3 + k];
            }
            viewToSky_[column * 3 + row] = sum;
        }
    }
    // Tint is a color as picked, sRGB, made linear as surface.glsl makes a Material's.
    const float exposure = std::max(lighting_.sky.exposure, 0.f);
    for (int channel = 0; channel < 3; ++channel) {
        skyColor_[channel] =
            dynamic ? 1.f : exposure * std::pow(std::max(lighting_.sky.tint[channel], 0.f), 2.2f);
    }
    skyLightScale_ = dynamic ? 1.f : std::max(lighting_.sky.lightScale, 0.f);
    skyImage_ = dynamic ? whiteTexture_ : lighting_.sky.image;
}

void Renderer::bindSky(const Program& program) {
    glUniform1f(program.skyEnabled, skyReady_ ? 1.f : 0.f);
    if (!skyReady_) {
        // Unread with no sky, but a sampler with nothing whole bound makes
        // macOS's driver complain, so each gets something.
        BindTexture(kUnitSky, whiteTexture_);
        BindCube(kUnitIrradiance, blackCube_);
        BindCube(kUnitPrefiltered, blackCube_);
        BindTexture(kUnitBrdf, whiteTexture_);
        return;
    }
    glUniformMatrix3fv(program.viewToSky, 1, GL_FALSE, viewToSky_);
    glUniform3f(program.skyColor, skyColor_[0], skyColor_[1], skyColor_[2]);
    glUniform1f(program.skyLightScale, skyLightScale_);
    glUniform1f(program.prefilteredMaxLod, EnvironmentMap::prefilteredMaxLod());
    BindTexture(kUnitSky, skyImage_);
    BindCube(kUnitIrradiance, environment_.irradiance());
    BindCube(kUnitPrefiltered, environment_.prefiltered());
    BindTexture(kUnitBrdf, environment_.brdf());
}

void Renderer::bindDynamicSky(const Program& program, const SceneDynamicSky& sky) {
    constexpr float kHalfDegree = 0.5f * 0.01745329252f;
    glUniform3fv(program.sunDirection, 1, sky.sunDirection);
    glUniform3fv(program.moonDirection, 1, sky.moonDirection);
    glUniformMatrix3fv(program.starFrame, 1, GL_FALSE, sky.starFrame);
    glUniform1f(program.starVisibility, sky.starVisibility);
    // Wrapped so the float keeps its precision; the stars skip once in hours.
    glUniform1f(program.starClock, static_cast<float>(std::fmod(sky.seconds, 10000.0)));
    glUniform3fv(program.bodyLightDirection, 1, sky.lightDirection);
    glUniform3fv(program.bodyLightColor, 1, sky.lightColor);
    glUniform3fv(program.sunColor, 1, sky.sunColor);
    glUniform3fv(program.moonColor, 1, sky.moonColor);
    glUniform1f(program.cloudCover, std::clamp(sky.cloudCover, 0.f, 1.f));
    glUniform1f(program.cloudDensity, std::clamp(sky.cloudDensity, 0.f, 1.f));
    glUniform2f(program.cloudOffset, sky.cloudOffset[0], sky.cloudOffset[1]);
    glUniform1f(program.sunSize, std::tan(std::clamp(sky.sunSizeDegrees, 0.1f, 20.f) * kHalfDegree));
    glUniform1f(program.moonSize, std::tan(std::clamp(sky.moonSizeDegrees, 0.1f, 20.f) * kHalfDegree));
    glUniform1f(program.sunTextureEnabled, sky.sunTexture != 0 ? 1.f : 0.f);
    glUniform1f(program.moonTextureEnabled, sky.moonTexture != 0 ? 1.f : 0.f);
    BindTexture(kUnitDiffuse, sky.sunTexture != 0 ? sky.sunTexture : whiteTexture_);
    BindTexture(kUnitNormalMap, sky.moonTexture != 0 ? sky.moonTexture : whiteTexture_);
}

bool Renderer::updateDynamicSkyLighting() {
    const SceneDynamicSky& sky = lighting_.dynamicSky;
    if (!environment_.drawingProcedural()) {
        const bool made = skyLightingValid_ && environment_.holdsProcedural();
        if (!LightingDue(skyLightingMade_, sky.key, skyLightingMadeAt_, sky.seconds, sky.windy, made)) {
            return made;
        }
        const EnvironmentSizes sizes = EnvironmentSizesFor(sky.key.quality);
        if (!environment_.startProcedural(sizes.environment, sizes.prefiltered)) {
            return environment_.holdsProcedural();
        }
        // Every slice draws this, so the faces match however the sky moves meanwhile.
        skyLightingDrawing_ = sky;
    }
    RENDER_PASS("Sky lighting");
    // The textures as they are now, in case the old ones are gone; the cube draws no discs anyway.
    skyLightingDrawing_.sunTexture = sky.sunTexture;
    skyLightingDrawing_.moonTexture = sky.moonTexture;
    const Program& program = dynamicSkyCube_;
    bool bound = false;
    const bool finished = environment_.continueProcedural(emptyVao_, [&](int face) {
        if (!bound) {
            glUseProgram(program.id);
            bindDynamicSky(program, skyLightingDrawing_);
            glBindVertexArray(emptyVao_);
            if (!CanDraw(program.id)) {
                return false;
            }
            bound = true;
        }
        glUniform1i(program.face, face);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        return true;
    });
    if (finished) {
        skyLightingMade_ = skyLightingDrawing_.key;
        skyLightingMadeAt_ = skyLightingDrawing_.seconds;
        skyLightingValid_ = true;
    }
    return environment_.holdsProcedural();
}

void Renderer::bindGBuffer(const Program& program) {
    (void)program;
    BindTexture(kUnitDepth, depthTexture_);
    BindTexture(kUnitAlbedo, albedoTexture_);
    BindTexture(kUnitNormal, normalTexture_);
    BindTexture(kUnitMaterial, materialTexture_);
    BindTexture(kUnitEmissive, emissiveTexture_);
}

CameraView Renderer::cameraView(const float* projection) const {
    CameraView camera;
    camera.world = engine_core::matrix4_inverse(view_);
    Matrix viewProjection;
    Multiply(projection, view_.m, viewProjection);
    std::copy(viewProjection, viewProjection + 16, camera.viewProjection.m);
    camera.fovYDegrees = fovYDegrees_;
    camera.aspect = static_cast<float>(targetWidth_) / static_cast<float>(targetHeight_);
    camera.nearZ = kSceneNear;
    camera.paneHeight = targetHeight_;
    return camera;
}

void Renderer::findVisible(const MeshDraw* meshes, int count, const CameraView& camera) {
    {
        PROFILE_SCOPE("Visibility", profiler::Group::Render);
        drawItems_.resize(static_cast<std::size_t>(count));
        for (int index = 0; index < count; ++index) {
            const MeshDraw& draw = meshes[index];
            DrawItem& item = drawItems_[static_cast<std::size_t>(index)];
            item = DrawItem{};
            item.model = &draw.model;
            item.transparency = draw.transparency;
            // As the passes have always skipped: no mesh, not uploaded, or wholly see-through.
            item.drawable = draw.mesh != nullptr && draw.mesh->valid() && !(draw.transparency >= 1.f);
            if (item.drawable) {
                item.boundsMin = draw.mesh->bounds_min();
                item.boundsMax = draw.mesh->bounds_max();
            }
            item.tint = draw.tint;
            item.slot = draw.slot;
        }
        FindVisible(drawItems_.data(), count, camera, culling_, visibility_);
        stats_.draws = count;
        stats_.visible = static_cast<int>(visibility_.opaque.size() + visibility_.transparent.size());
        stats_.culled = visibility_.culled;
    }
    PROFILE_SCOPE("Batches", profiler::Group::Render);
    BuildBatches(drawItems_.data(), visibility_, view_, batches_);
}

bool Renderer::shadowPass(const MeshDraw* meshes, int count, const CameraView& camera) {
    RENDER_PASS("Shadows");
    shadowLookups_.assign(shadowRequests_.size(), ShadowLookup{});
    sunLookup_ = ShadowLookup{};
    if (shadowRequests_.empty() && !hasSunShadow_) {
        return true;
    }
    if (!shadows_.draw(shadowRequests_, meshes, count, visibility_.spheres.data(), camera, shadowSettings_)) {
        return false;
    }
    if (!shadows_.drawSun(hasSunShadow_ ? &sunShadow_ : nullptr, meshes, camera, shadowSettings_)) {
        return false;
    }
    stats_.instancedCalls += shadows_.calls();
    sunLookup_ = shadows_.sunLookup();
    for (std::size_t index = 0; index < shadowRequests_.size(); ++index) {
        shadowLookups_[index] = shadows_.lookup(shadowRequests_[index].key);
    }
    return true;
}

void Renderer::bindShadow(const Program& program, const ShadowLookup& lookup) {
    BindArray(kUnitShadowAtlas, shadows_.atlasMap());
    BindArray(kUnitShadowCascades, shadows_.cascadeMap());
    glUniform1i(program.shadowKind, lookup.kind);
    if (lookup.kind == ShadowLookup::kNone) {
        return;
    }
    float matrices[16 * kMaxCascades];
    for (int i = 0; i < kMaxCascades; ++i) {
        std::copy(lookup.matrices[i].m, lookup.matrices[i].m + 16, matrices + 16 * i);
    }
    glUniformMatrix4fv(program.shadowMatrix, kMaxCascades, GL_FALSE, matrices);
    glUniform1i(program.shadowCascadeCount, lookup.cascades);
    glUniform3f(program.shadowLight, lookup.light[0], lookup.light[1], lookup.light[2]);
    glUniform2f(program.shadowNearFar, lookup.nearFar[0], lookup.nearFar[1]);
    glUniform4f(program.shadowTexel, lookup.texel[0], lookup.texel[1], lookup.texel[2], lookup.texel[3]);
    glUniform1f(program.shadowTexelUv, lookup.texelUv);
    glUniform4fv(program.shadowTiles, 6, lookup.tiles[0]);
    glUniform1f(program.shadowFaceScale, lookup.faceScale);
}

bool Renderer::geometryPass(const MeshDraw* meshes, const float* projection) {
    RENDER_PASS("Geometry");
    glViewport(0, 0, targetWidth_, targetHeight_);
    glBindFramebuffer(RT_GL_FRAMEBUFFER, gbufferFbo_);
    glDisable(GL_BLEND);
    glEnable(RT_GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(RT_GL_LESS);
    glDepthMask(GL_TRUE);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(geometry_.id);
    glUniformMatrix4fv(geometry_.view, 1, GL_FALSE, view_.m);
    glUniformMatrix4fv(geometry_.projection, 1, GL_FALSE, projection);
    // Every instance of the frame, opaque and see-through, in one upload.
    instances_.upload(batches_.instances.data(), static_cast<int>(batches_.instances.size()));
    bool asked = false;
    for (int index = 0; index < batches_.opaqueRuns; ++index) {
        const DrawRun& run = batches_.runs[static_cast<std::size_t>(index)];
        const MeshDraw& draw = meshes[run.draw];
        bindMaterial(geometry_, draw);
        glCullFace(run.mirrored ? RT_GL_FRONT : RT_GL_BACK);
        draw.mesh->bind();
        instances_.attach(run.first);
        if (!asked && !CanDraw(geometry_.id)) {
            return false;
        }
        asked = true;
        draw.mesh->draw_instanced(run.lod, run.count);
        ++stats_.runs;
        ++stats_.instancedCalls;
    }
    glDisable(RT_GL_CULL_FACE);
    glCullFace(RT_GL_BACK);
    return true;
}

bool Renderer::occlusionPass(const float* projection, const float* inverseProjection) {
    occlusionReady_ = false;
    const SceneOcclusion& occlusion = lighting_.occlusion;
    if (!occlusion.enabled || !(occlusion.intensity > 0.f) || !(occlusion.radius > 0.f)) {
        return false;
    }
    const OcclusionQuality settings = QualitySettings(static_cast<int>(occlusion.quality), pixelsPerPoint_);
    if (!ensureOcclusionBuffers(targetWidth_, targetHeight_, settings.scale)) {
        return false;
    }
    RENDER_PASS("Ambient occlusion");
    const int w = std::max(targetWidth_ / settings.scale, 1);
    const int h = std::max(targetHeight_ / settings.scale, 1);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glViewport(0, 0, w, h);
    glBindVertexArray(emptyVao_);
    BindTexture(kUnitDepth, depthTexture_);
    BindTexture(kUnitNormal, normalTexture_);
    const float texelX = 1.f / static_cast<float>(targetWidth_);
    const float texelY = 1.f / static_cast<float>(targetHeight_);

    glBindFramebuffer(RT_GL_FRAMEBUFFER, occlusionFbo_);
    glUseProgram(gtao_.id);
    glUniformMatrix4fv(gtao_.inverseProjection, 1, GL_FALSE, inverseProjection);
    glUniform2f(gtao_.texel, texelX, texelY);
    glUniform1f(gtao_.occlusionRadius, occlusion.radius);
    // Full-size pixels per stud at view depth 1: half the height times the projection's [1][1].
    glUniform1f(gtao_.projectionScale, 0.5f * static_cast<float>(targetHeight_) * projection[5]);
    glUniform1f(gtao_.occlusionScale, static_cast<float>(settings.scale));
    glUniform1f(gtao_.slices, static_cast<float>(settings.slices));
    // Validated until it passes, then trusted until the programs or buffers are made again.
    if (!occlusionValid_ && !CanDraw(gtao_.id)) {
        glViewport(0, 0, targetWidth_, targetHeight_);
        return false;
    }
    DrawFullscreen(emptyVao_);

    // Across into the halfway buffer, then down back into occlusionTexture_.
    glUseProgram(aoBlur_.id);
    glUniformMatrix4fv(aoBlur_.inverseProjection, 1, GL_FALSE, inverseProjection);
    glUniform2f(aoBlur_.texel, texelX, texelY);
    glUniform1f(aoBlur_.occlusionScale, static_cast<float>(settings.scale));
    glUniform1f(aoBlur_.blurRadius, static_cast<float>(settings.blurRadius));
    glBindFramebuffer(RT_GL_FRAMEBUFFER, occlusionBlurFbo_);
    BindTexture(kUnitScene, occlusionTexture_);
    glUniform2f(aoBlur_.blurDirection, 1.f, 0.f);
    if (!occlusionValid_ && !CanDraw(aoBlur_.id)) {
        glViewport(0, 0, targetWidth_, targetHeight_);
        return false;
    }
    DrawFullscreen(emptyVao_);
    glBindFramebuffer(RT_GL_FRAMEBUFFER, occlusionFbo_);
    BindTexture(kUnitScene, occlusionBlurTexture_);
    glUniform2f(aoBlur_.blurDirection, 0.f, 1.f);
    DrawFullscreen(emptyVao_);
    glViewport(0, 0, targetWidth_, targetHeight_);
    occlusionValid_ = true;
    occlusionReady_ = true;
    return true;
}

void Renderer::bindOcclusion(const Program& program) {
    glUniform1f(program.occlusionEnabled, occlusionReady_ ? 1.f : 0.f);
    glUniform1f(program.occlusionIntensity, std::min(lighting_.occlusion.intensity, 4.f));
    glUniform1f(program.occlusionScale, static_cast<float>(std::max(occlusionScale_, 1)));
    // With none, any texture keeps the sampler loadable.
    BindTexture(kUnitOcclusion, occlusionReady_ ? occlusionTexture_ : whiteTexture_);
}

bool Renderer::lightPass(const float* projection, const float* inverseProjection) {
    RENDER_PASS("Lighting");
    const engine_core::Matrix4 inverseView = engine_core::matrix4_inverse(view_);
    glBindFramebuffer(RT_GL_FRAMEBUFFER, accumulationFbo_);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(RT_GL_ONE, RT_GL_ONE);

    // The ambient and the sky, on every opaque surface.
    glUseProgram(ibl_.id);
    bindGBuffer(ibl_);
    glUniformMatrix4fv(ibl_.inverseProjection, 1, GL_FALSE, inverseProjection);
    glUniform3f(ibl_.ambient, lighting_.ambient[0], lighting_.ambient[1], lighting_.ambient[2]);
    glUniform3f(ibl_.skyRadiance, kSkyRadiance, kSkyRadiance, kSkyRadiance);
    bindSky(ibl_);
    bindOcclusion(ibl_);
    glBindVertexArray(emptyVao_);
    if (!CanDraw(ibl_.id)) {
        return false;
    }
    DrawFullscreen(emptyVao_);

    const auto useLightProgram = [&](const Program& program) {
        glUseProgram(program.id);
        bindGBuffer(program);
        glUniformMatrix4fv(program.inverseProjection, 1, GL_FALSE, inverseProjection);
        glUniform2f(program.texel, 1.f / static_cast<float>(targetWidth_), 1.f / static_cast<float>(targetHeight_));
        glUniformMatrix4fv(program.inverseView, 1, GL_FALSE, inverseView.m);
        // Each shadow sampler gets a texture before CanDraw asks.
        bindShadow(program, ShadowLookup{});
    };
    const auto setLight = [](const Program& program, const ViewLight& light) {
        glUniform3f(program.lightPosition, light.position[0], light.position[1], light.position[2]);
        glUniform3f(program.lightDirection, light.direction[0], light.direction[1], light.direction[2]);
        glUniform2f(program.lightCone, light.cone[0], light.cone[1]);
        glUniform3f(program.lightColor, light.color[0], light.color[1], light.color[2]);
        glUniform1f(program.lightRadius, light.radius);
        glUniform1f(program.lightIntensity, light.intensity);
    };
    const auto isSun = [](const ViewLight& light) { return light.cone[0] < -3.f; };
    const auto shadowOf = [this](const ViewLight& light) {
        if (light.shadow == kSunShadow) {
            return sunLookup_;
        }
        return light.shadow >= 0 ? shadowLookups_[static_cast<std::size_t>(light.shadow)] : ShadowLookup{};
    };

    // Each DirectionalLight, on every pixel.
    bool sunsBound = false;
    for (const ViewLight& light : viewLights_) {
        if (!isSun(light)) {
            continue;
        }
        if (!sunsBound) {
            useLightProgram(sun_);
            glBindVertexArray(emptyVao_);
            if (!CanDraw(sun_.id)) {
                return false;
            }
            sunsBound = true;
        }
        setLight(sun_, light);
        bindShadow(sun_, shadowOf(light));
        DrawFullscreen(emptyVao_);
    }

    // Each PointLight and SpotLight, on the pixels its volume covers. Its
    // inside faces draw, so a camera within the volume still sees the light.
    if (std::any_of(viewLights_.begin(), viewLights_.end(), [&](const ViewLight& light) { return !isSun(light); })) {
        Matrix viewProjection;
        Multiply(projection, view_.m, viewProjection);
        const engine_core::Matrix4 world = engine_core::matrix4_inverse(view_);
        useLightProgram(light_);
        glUniformMatrix4fv(light_.viewProjection, 1, GL_FALSE, viewProjection);
        glEnable(RT_GL_CULL_FACE);
        glCullFace(RT_GL_FRONT);
        glBindVertexArray(sphereVao_);
        if (!CanDraw(light_.id)) {
            return false;
        }
        for (const ViewLight& light : viewLights_) {
            if (isSun(light)) {
                continue;
            }
            // The volume's world position, from the view-space one.
            const float* w = world.m;
            const float* p = light.position;
            const float scale = light.radius * kSphereSlack;
            float model[16] = {};
            model[0] = scale;
            model[5] = scale;
            model[10] = scale;
            for (int row = 0; row < 3; ++row) {
                model[12 + row] = w[row] * p[0] + w[4 + row] * p[1] + w[8 + row] * p[2] + w[12 + row];
            }
            model[15] = 1.f;
            glUniformMatrix4fv(light_.model, 1, GL_FALSE, model);
            setLight(light_, light);
            bindShadow(light_, shadowOf(light));
            glDrawElements(GL_TRIANGLES, sphereIndexCount_, GL_UNSIGNED_SHORT, nullptr);
        }
        glDisable(RT_GL_CULL_FACE);
        glCullFace(RT_GL_BACK);
    }
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    return true;
}

bool Renderer::skyPass(const float* inverseProjection) {
    RENDER_PASS("Sky");
    if (!skyVisible_) {
        return true;
    }
    const bool dynamic = dynamicSkyDrawn();
    const Program& program = dynamic ? dynamicSky_ : sky_;
    // Only where no opaque surface is, which no light pass wrote: no blending needed.
    glBindFramebuffer(RT_GL_FRAMEBUFFER, accumulationFbo_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(program.id);
    BindTexture(kUnitDepth, depthTexture_);
    glUniformMatrix4fv(program.inverseProjection, 1, GL_FALSE, inverseProjection);
    if (dynamic) {
        // dynamic_sky.frag reads no IBL uniform, only uViewToSky: bindSky
        // uploads that one only when skyReady_, so upload it here instead,
        // since the DynamicSky draws even when skyReady_ (its lighting
        // cube) is not.
        glUniformMatrix3fv(program.viewToSky, 1, GL_FALSE, viewToSky_);
        bindDynamicSky(program, lighting_.dynamicSky);
    } else {
        bindSky(program);
    }
    glBindVertexArray(emptyVao_);
    if (!CanDraw(program.id)) {
        return false;
    }
    DrawFullscreen(emptyVao_);
    return true;
}

bool Renderer::reflectionsPass(const float* projection, const float* inverseProjection) {
    const SceneReflections& reflections = lighting_.reflections;
    if (!reflections.enabled || !(reflections.intensity > 0.f) || !(reflections.maxDistance > 0.f) ||
        !(reflections.maxRoughness > 0.f)) {
        return false;
    }
    if (!ensureReflectionBuffers(targetWidth_, targetHeight_)) {
        return false;
    }
    RENDER_PASS("Reflections");
    const int halfWidth = std::max(targetWidth_ >> 1, 1);
    const int halfHeight = std::max(targetHeight_ >> 1, 1);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glViewport(0, 0, halfWidth, halfHeight);
    glBindVertexArray(emptyVao_);

    // The lit opaque image, with its glow, at half size; then its mips.
    glBindFramebuffer(RT_GL_FRAMEBUFFER, reflectSceneFbo_);
    glUseProgram(ssrScene_.id);
    BindTexture(kUnitAccumulation, accumulationTexture_);
    BindTexture(kUnitEmissive, emissiveTexture_);
    // Validated until it passes, then trusted until the programs or buffers are made again.
    if (!reflectionsValid_ && !CanDraw(ssrScene_.id)) {
        return false;
    }
    DrawFullscreen(emptyVao_);
    // Each level down, a Gaussian one of its texels wide: the level above,
    // blurred across into the blur buffer's level (the bilinear taps halving
    // it), then that blurred down into this level. No buffer is read while
    // it is drawn into.
    glUseProgram(ssrBlur_.id);
    for (int level = 1; level < reflectSceneLevels_; ++level) {
        const int levelWidth = std::max(halfWidth >> level, 1);
        const int levelHeight = std::max(halfHeight >> level, 1);
        glViewport(0, 0, levelWidth, levelHeight);
        glUniform1f(ssrBlur_.prefilter, level == 1 ? 1.f : 0.f);
        glBindFramebuffer(RT_GL_FRAMEBUFFER, reflectBlurFbos_[level - 1]);
        BindTexture(kUnitScene, reflectSceneTexture_);
        glUniform1f(ssrBlur_.sourceLevel, static_cast<float>(level - 1));
        glUniform2f(ssrBlur_.blurDirection, 1.f / static_cast<float>(levelWidth), 0.f);
        if (level == 1 && !reflectionsValid_ && !CanDraw(ssrBlur_.id)) {
            return false;
        }
        DrawFullscreen(emptyVao_);
        glBindFramebuffer(RT_GL_FRAMEBUFFER, reflectSceneLevelFbos_[level - 1]);
        BindTexture(kUnitScene, reflectBlurTexture_);
        glUniform1f(ssrBlur_.sourceLevel, static_cast<float>(level));
        glUniform2f(ssrBlur_.blurDirection, 0.f, 1.f / static_cast<float>(levelHeight));
        DrawFullscreen(emptyVao_);
    }
    glViewport(0, 0, halfWidth, halfHeight);

    // The trace.
    glBindFramebuffer(RT_GL_FRAMEBUFFER, reflectionFbo_);
    glUseProgram(ssr_.id);
    bindGBuffer(ssr_);
    bindSky(ssr_);
    BindTexture(kUnitScene, reflectSceneTexture_);
    glUniformMatrix4fv(ssr_.projection, 1, GL_FALSE, projection);
    glUniformMatrix4fv(ssr_.inverseProjection, 1, GL_FALSE, inverseProjection);
    glUniform2f(ssr_.screenSize, static_cast<float>(targetWidth_), static_cast<float>(targetHeight_));
    glUniform1f(ssr_.nearPlane, kSceneNear);
    glUniform1f(ssr_.maxDistance, reflections.maxDistance);
    glUniform1f(ssr_.maxRoughness, std::min(reflections.maxRoughness, 1.f));
    glUniform1f(ssr_.chainLevels, static_cast<float>(reflectSceneLevels_));
    if (!reflectionsValid_ && !CanDraw(ssr_.id)) {
        return false;
    }
    DrawFullscreen(emptyVao_);
    reflectionsValid_ = true;
    glViewport(0, 0, targetWidth_, targetHeight_);
    return true;
}

bool Renderer::transparencyPass(const MeshDraw* meshes, const float* projection, const float* inverseProjection) {
    RENDER_PASS("Transparency");
    glBindFramebuffer(RT_GL_FRAMEBUFFER, transparencyFbo_);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (batches_.opaqueRuns == static_cast<int>(batches_.runs.size())) {
        return true;
    }

    // Tested against the opaque surfaces' depth, but writing none, so each
    // see-through surface blends over every one behind it.
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(RT_GL_LESS);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    // Color blends over; alpha gathers coverage, so the result is premultiplied.
    glBlendFuncSeparate(RT_GL_SRC_ALPHA, RT_GL_ONE_MINUS_SRC_ALPHA, RT_GL_ONE, RT_GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(forward_.id);
    glUniformMatrix4fv(forward_.view, 1, GL_FALSE, view_.m);
    glUniformMatrix4fv(forward_.projection, 1, GL_FALSE, projection);
    glUniformMatrix4fv(forward_.inverseProjection, 1, GL_FALSE, inverseProjection);
    glUniform3f(forward_.ambient, lighting_.ambient[0], lighting_.ambient[1], lighting_.ambient[2]);
    glUniform3f(forward_.skyRadiance, kSkyRadiance, kSkyRadiance, kSkyRadiance);
    bindSky(forward_);
    const int lightCount = std::min(static_cast<int>(viewLights_.size()), kMaxForwardLights);
    float positionRadius[kMaxForwardLights * 4] = {};
    float colorIntensity[kMaxForwardLights * 4] = {};
    float directions[kMaxForwardLights * 4] = {};
    float cones[kMaxForwardLights * 4] = {};
    for (int index = 0; index < lightCount; ++index) {
        const ViewLight& light = viewLights_[static_cast<std::size_t>(index)];
        float* pr = positionRadius + index * 4;
        float* ci = colorIntensity + index * 4;
        float* di = directions + index * 4;
        float* co = cones + index * 4;
        std::copy(light.position, light.position + 3, pr);
        pr[3] = light.radius;
        std::copy(light.color, light.color + 3, ci);
        ci[3] = light.intensity;
        std::copy(light.direction, light.direction + 3, di);
        co[0] = light.cone[0];
        co[1] = light.cone[1];
    }
    glUniform1i(forward_.lightCount, lightCount);
    if (lightCount > 0) {
        glUniform4fv(forward_.lightPositionRadius, lightCount, positionRadius);
        glUniform4fv(forward_.lightColorIntensity, lightCount, colorIntensity);
        glUniform4fv(forward_.lightDirections, lightCount, directions);
        glUniform4fv(forward_.lightCones, lightCount, cones);
    }
    glEnable(RT_GL_CULL_FACE);
    bool asked = false;
    for (std::size_t index = static_cast<std::size_t>(batches_.opaqueRuns); index < batches_.runs.size(); ++index) {
        const DrawRun& run = batches_.runs[index];
        const MeshDraw& draw = meshes[run.draw];
        bindMaterial(forward_, draw);
        glCullFace(run.mirrored ? RT_GL_FRONT : RT_GL_BACK);
        draw.mesh->bind();
        instances_.attach(run.first);
        if (!asked && !CanDraw(forward_.id)) {
            return false;
        }
        asked = true;
        draw.mesh->draw_instanced(run.lod, run.count);
        ++stats_.instancedCalls;
    }
    glDisable(RT_GL_CULL_FACE);
    glCullFace(RT_GL_BACK);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    return true;
}

bool Renderer::mergePass(bool reflected, const float* inverseProjection) {
    RENDER_PASS("Merge");
    glBindFramebuffer(RT_GL_FRAMEBUFFER, mergeFbo_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(merge_.id);
    BindTexture(kUnitDepth, depthTexture_);
    BindTexture(kUnitEmissive, emissiveTexture_);
    BindTexture(kUnitAccumulation, accumulationTexture_);
    BindTexture(kUnitTransparency, transparencyTexture_);
    glUniformMatrix4fv(merge_.inverseProjection, 1, GL_FALSE, inverseProjection);
    glUniform1f(merge_.reflectionsEnabled, reflected ? 1.f : 0.f);
    glUniform1f(merge_.reflectionsIntensity, std::min(lighting_.reflections.intensity, 1.f));
    glUniform3f(merge_.ambient, lighting_.ambient[0], lighting_.ambient[1], lighting_.ambient[2]);
    glUniform3f(merge_.skyRadiance, kSkyRadiance, kSkyRadiance, kSkyRadiance);
    bindGBuffer(merge_);
    bindSky(merge_);
    // uSkyEnabled (bindSky, above) is whether surfaces light and reflect from
    // the cubes; uSkyDrawn is whether skyPass drew into the accumulation
    // buffer at all, which for a DynamicSky is true even with no cubes.
    glUniform1f(merge_.skyDrawn, skyVisible_ ? 1.f : 0.f);
    bindOcclusion(merge_);
    // With no trace, any texture keeps the sampler loadable.
    BindTexture(kUnitReflections, reflected ? reflectionTexture_ : whiteTexture_);
    glBindVertexArray(emptyVao_);
    if (!CanDraw(merge_.id)) {
        return false;
    }
    DrawFullscreen(emptyVao_);
    return true;
}

int Renderer::bloomPass(int width, int height) {
    const SceneBloom& bloom = lighting_.bloom;
    if (!bloom.enabled || !(bloom.intensity > 0.f)) {
        return 0;
    }
    const BloomPlan plan = PlanBloom(bloom.size, width, height);
    if (plan.levels == 0 || !ensureBloomChain(width, height)) {
        return 0;
    }
    RENDER_PASS("Bloom");
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glBindVertexArray(emptyVao_);

    // Down: the merge image into level 0, then each level into the next. Each
    // program is validated once, at its first step: nothing validation looks
    // at changes between steps, and on macOS each validation costs.
    glUseProgram(bloomDown_.id);
    glUniform1f(bloomDown_.threshold, std::max(bloom.threshold, 0.f));
    for (int k = 0; k < plan.levels; ++k) {
        glBindFramebuffer(RT_GL_FRAMEBUFFER, bloomFbos_[k]);
        glViewport(0, 0, width >> (k + 1), height >> (k + 1));
        BindTexture(kUnitScene, k == 0 ? mergeTexture_ : bloomTextures_[k - 1]);
        glUniform2f(bloomDown_.texel, 1.f / static_cast<float>(width >> k), 1.f / static_cast<float>(height >> k));
        glUniform1f(bloomDown_.prefilter, k == 0 ? 1.f : 0.f);
        if (k == 0 && !CanDraw(bloomDown_.id)) {
            return 0;
        }
        DrawFullscreen(emptyVao_);
    }

    // Up: each level added into the next larger one, so level 0 ends up the sum of them all.
    glUseProgram(bloomUp_.id);
    glUniform1f(bloomUp_.radius, plan.radius);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(RT_GL_ONE, RT_GL_ONE, RT_GL_ONE, RT_GL_ONE);
    for (int k = plan.levels - 1; k > 0; --k) {
        glBindFramebuffer(RT_GL_FRAMEBUFFER, bloomFbos_[k - 1]);
        glViewport(0, 0, width >> k, height >> k);
        BindTexture(kUnitScene, bloomTextures_[k]);
        glUniform2f(bloomUp_.texel, 1.f / static_cast<float>(width >> (k + 1)),
                    1.f / static_cast<float>(height >> (k + 1)));
        if (k == plan.levels - 1 && !CanDraw(bloomUp_.id)) {
            glDisable(GL_BLEND);
            return 0;
        }
        DrawFullscreen(emptyVao_);
    }
    glDisable(GL_BLEND);
    return plan.levels;
}

bool Renderer::toneMapPass(int bloomLevels) {
    RENDER_PASS("Tone map");
    glUseProgram(tonemap_.id);
    BindTexture(kUnitScene, mergeTexture_);
    glUniform1f(tonemap_.exposure, std::max(lighting_.exposure, 0.f));
    glUniform1f(tonemap_.inverseGamma, 1.f / std::max(lighting_.gamma, 0.01f));
    glUniform1f(tonemap_.saturation, std::max(lighting_.saturation, 0.f));
    // With no bloom the shader skips it; any texture keeps the sampler loadable.
    BindTexture(kUnitBloom, bloomLevels > 0 ? bloomTextures_[0] : whiteTexture_);
    glUniform1f(tonemap_.bloomIntensity, bloomLevels > 0 ? std::min(lighting_.bloom.intensity, 1.f) : 0.f);
    glUniform1f(tonemap_.bloomLevelScale, bloomLevels > 0 ? 1.f / static_cast<float>(bloomLevels) : 0.f);
    glUniform1f(tonemap_.bloomThreshold, std::max(lighting_.bloom.threshold, 0.f));
    glBindVertexArray(emptyVao_);
    if (!CanDraw(tonemap_.id)) {
        return false;
    }
    DrawFullscreen(emptyVao_);
    return true;
}

bool Renderer::fxaaPass() {
    RENDER_PASS("FXAA");
    glUseProgram(fxaa_.id);
    BindTexture(kUnitScene, ldrTexture_);
    glUniform2f(fxaa_.texel, 1.f / static_cast<float>(targetWidth_), 1.f / static_cast<float>(targetHeight_));
    glBindVertexArray(emptyVao_);
    if (!fxaaValid_) {
        if (!CanDraw(fxaa_.id)) {
            return false;
        }
        fxaaValid_ = true;
    }
    DrawFullscreen(emptyVao_);
    return true;
}

bool Renderer::read(double x, double y, double width, double height, double sceneWidth, double sceneHeight,
                    ViewPixels& out) const {
    out = ViewPixels{};
    if (!ready_ || width <= 0.0 || height <= 0.0 || sceneWidth <= 0.0 || sceneHeight <= 0.0) {
        return false;
    }
    GLint viewport[4] = {};
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] <= 0 || viewport[3] <= 0) {
        return false;
    }
    // Only what draw could reach: the pane, inside the framebuffer and any parent clip.
    PixelRect clip = Intersect(PanePixels(x, y, width, height, sceneWidth, sceneHeight, viewport),
                               PixelRect{viewport[0], viewport[1], viewport[2], viewport[3]});
    if (glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE) {
        GLint scissorBox[4] = {};
        glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
        clip = Intersect(clip, PixelRect{scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]});
    }
    if (clip.width <= 0 || clip.height <= 0) {
        return false;
    }
    // RGBA rows are whole words, so the default pack alignment of 4 adds no padding.
    std::vector<unsigned char> bottomUp(static_cast<std::size_t>(clip.width) * clip.height * 4);
    glReadPixels(clip.x, clip.y, clip.width, clip.height, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data());
    out.width = clip.width;
    out.height = clip.height;
    out.rgba.resize(bottomUp.size());
    const std::size_t row = static_cast<std::size_t>(clip.width) * 4;
    for (int line = 0; line < clip.height; ++line) {
        std::copy_n(bottomUp.data() + static_cast<std::size_t>(clip.height - 1 - line) * row, row,
                    out.rgba.data() + static_cast<std::size_t>(line) * row);
    }
    return true;
}

void Renderer::setClearColor(float r, float g, float b) {
    clear_[0] = r;
    clear_[1] = g;
    clear_[2] = b;
}

void Renderer::shutdown() {
    gpu_.shutdown();
    ready_ = false;
    for (Program* program :
         {&geometry_, &forward_, &ibl_, &light_, &sun_, &merge_, &tonemap_, &bloomDown_, &bloomUp_, &fxaa_, &ssrScene_, &ssrBlur_, &ssr_, &gtao_, &aoBlur_, &sky_, &grid_,
          &gridBands_, &outline_, &handle_, &dynamicSky_, &dynamicSkyCube_}) {
        if (program->id != 0) {
            glDeleteProgram(program->id);
        }
        *program = Program{};
    }
    environment_.shutdown();
    shadows_.shutdown();
    instances_.destroy();
    skyReady_ = false;
    skyVisible_ = false;
    dynamicSkyBuilt_ = false;
    skyLightingValid_ = false;
    for (unsigned* texture : {&whiteTexture_, &blackCube_}) {
        DeleteTexture(*texture);
    }
    for (unsigned* vao : {&emptyVao_, &sphereVao_, &gridBandVao_, &outlineVao_, &handleVao_}) {
        if (*vao != 0) {
            glDeleteVertexArrays(1, vao);
            *vao = 0;
        }
    }
    for (unsigned* buffer : {&sphereVbo_, &sphereEbo_, &gridBandVbo_, &outlineVbo_, &handleVbo_}) {
        if (*buffer != 0) {
            glDeleteBuffers(1, buffer);
            *buffer = 0;
        }
    }
    sphereIndexCount_ = 0;
    if (probeBuffers_[0] != 0) {
        glDeleteBuffers(2, probeBuffers_);
    }
    probeBuffers_[0] = 0;
    probeBuffers_[1] = 0;
    probeFilled_[0] = false;
    probeFilled_[1] = false;
    probedDepth_.reset();
    sceneDepth_ = SceneDepth{};
    destroyTargets();
    targetsRefused_ = false;
    bloomRefusedWidth_ = 0;
    bloomRefusedHeight_ = 0;
    reflectionRefusedWidth_ = 0;
    reflectionRefusedHeight_ = 0;
    occlusionRefusedWidth_ = 0;
    occlusionRefusedHeight_ = 0;
    occlusionRefusedScale_ = 0;
}

engine_core::Matrix4 Renderer::DefaultView() {
    engine_core::Matrix4 view;
    view = LookAtView({kCameraEye[0], kCameraEye[1], kCameraEye[2]},
                      {kCameraTarget[0], kCameraTarget[1], kCameraTarget[2]}, {0.f, 1.f, 0.f});
    return view;
}

void Renderer::setCamera(const engine_core::Matrix4& world, float fovYDegrees) {
    if (!(fovYDegrees > 0.f && fovYDegrees < 180.f)) {
        return;
    }
    const engine_core::Matrix4 view = engine_core::matrix4_inverse(engine_core::matrix4_orthonormalize(world));
    for (const float value : view.m) {
        if (!std::isfinite(value)) {
            return;
        }
    }
    view_ = view;
    fovYDegrees_ = fovYDegrees;
}

}  // namespace runner
