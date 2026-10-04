#include "Renderer.hpp"

#include "profiler/Profiler.hpp"

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

// One pass, timed on the CPU (a Render scope) and on the GPU (a Gpu scope of the same name).
class PassTimer {
public:
    PassTimer(GpuTimer& gpu, profiler::ScopeId cpu, profiler::ScopeId on_gpu) : cpu_(cpu), gpu_(gpu) {
        gpu_.begin(on_gpu);
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
// The light pass reads no Material, so its shadow maps take the Material's units.
constexpr int kUnitShadowAtlas = kUnitDiffuse;
constexpr int kUnitShadowCascades = kUnitNormalMap;
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

constexpr float kNear = 0.1f;
constexpr float kFar = 1000.f;

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
    program.diffuse = at("uDiffuse");
    program.normalMap = at("uNormalMap");
    program.roughnessMap = at("uRoughnessMap");
    program.metalnessMap = at("uMetalnessMap");
    program.color = at("uColor");
    program.emissive = at("uEmissive");
    program.metalness = at("uMetalness");
    program.roughness = at("uRoughness");
    program.reflectivity = at("uReflectivity");
    program.normalMapEnabled = at("uNormalMapEnabled");
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
                     {"pipeline/lighting.glsl", "pipeline/environment.glsl", "pipeline/image_lighting.glsl"}) &&
        buildProgram(sky_, "Sky", "pipeline/fullscreen.vert", "pipeline/sky.frag",
                     {"pipeline/lighting.glsl", "pipeline/environment.glsl"}) &&
        buildProgram(light_, "Light", "pipeline/light.vert", "pipeline/light.frag",
                     {"pipeline/lighting.glsl", "pipeline/shadow.glsl"}) &&
        buildProgram(sun_, "Directional light", "pipeline/fullscreen.vert", "pipeline/light.frag",
                     {"pipeline/lighting.glsl", "pipeline/shadow.glsl"}) &&
        buildProgram(merge_, "Merge", "pipeline/fullscreen.vert", "pipeline/merge.frag", {}) &&
        buildProgram(tonemap_, "Tone map", "pipeline/fullscreen.vert", "pipeline/tonemap.frag", {}) &&
        buildProgram(grid_, "Grid", "pipeline/fullscreen.vert", "pipeline/grid.frag", {}) &&
        buildProgram(outline_, "Outline", "pipeline/outline.vert", "pipeline/outline.frag", {}) &&
        buildProgram(handle_, "Handle", "pipeline/handle.vert", "pipeline/handle.frag", {}) &&
        environment_.initialize() && shadows_.initialize();
    if (!built) {
        shutdown();
        return false;
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

unsigned MakeTarget(GLenum internalFormat, GLenum format, GLenum type, int width, int height) {
    unsigned texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat), width, height, 0, format, type, nullptr);
    // Every pass reads its inputs texel for texel.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(RT_GL_NEAREST));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(RT_GL_NEAREST));
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
    mergeTexture_ = MakeTarget(RT_GL_RGBA16F, GL_RGBA, RT_GL_HALF_FLOAT, width, height);
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
    for (unsigned* fbo : {&gbufferFbo_, &accumulationFbo_, &transparencyFbo_, &mergeFbo_}) {
        if (*fbo != 0) {
            glDeleteFramebuffers(1, fbo);
            *fbo = 0;
        }
    }
    for (unsigned* texture : {&albedoTexture_, &normalTexture_, &materialTexture_, &emissiveTexture_, &depthTexture_,
                              &accumulationTexture_, &transparencyTexture_, &mergeTexture_}) {
        if (*texture != 0) {
            glDeleteTextures(1, texture);
            *texture = 0;
        }
    }
    targetWidth_ = 0;
    targetHeight_ = 0;
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

// Culls the faces turned away from the camera. Meshes are wound CCW, but a
// Transform that mirrors, scaled negative on an odd number of axes, turns
// the winding around, so its back faces are the CW ones.
void CullBackFaces(const float* model) {
    const float* m = model;
    const float determinant = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) +
                              m[8] * (m[1] * m[6] - m[5] * m[2]);
    glCullFace(determinant < 0.f ? RT_GL_FRONT : RT_GL_BACK);
}

// The GL state a draw changes, so the UI pass after it finds its own.
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
    GLint activeTexture = 0;
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
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        seamlessCubes = glIsEnabled(RT_GL_TEXTURE_CUBE_MAP_SEAMLESS);
    }

    static void Set(GLenum cap, GLboolean on) {
        if (on == GL_TRUE) {
            glEnable(cap);
        } else {
            glDisable(cap);
        }
    }

    void restore(const GLint viewport[4]) const {
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
        // The units the passes used are left empty, as the old single pass left unit 0.
        for (int unit = kUnitCount - 1; unit >= 0; --unit) {
            BindArray(unit, 0);
            BindCube(unit, 0);
            BindTexture(unit, 0);
        }
        glActiveTexture(static_cast<GLenum>(activeTexture));
    }
};

}  // namespace

bool Renderer::draw(double x, double y, double width, double height, double sceneWidth, double sceneHeight,
                    const MeshDraw* meshes, int meshCount, const LightDraw* lights, int lightCount) {
    if (!ready_ || width <= 0.0 || height <= 0.0 || sceneWidth <= 0.0 || sceneHeight <= 0.0) {
        return false;
    }
    // The 3D draw runs on the UI thread, but the profiler shows it in the Render section.
    const profiler::RowScope row("Render draw");

    GLint viewport[4] = {};
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] <= 0 || viewport[3] <= 0) {
        return false;
    }

    const PixelRect pane = PanePixels(x, y, width, height, sceneWidth, sceneHeight, viewport);
    if (pane.width <= 0 || pane.height <= 0) {
        return false;
    }

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
    glClear(GL_COLOR_BUFFER_BIT);

    const bool hasMeshes = meshes != nullptr && meshCount > 0;
    if (!hasMeshes) {
        meshes = nullptr;
        meshCount = 0;
    }
    // A sky whose cubes can never be made is drawn as no sky, rather than never drawing.
    const bool hasSky = lighting_.sky.image != 0 && environment_.available();
    const engine_core::Matrix4 projectionMatrix =
        Perspective(fovYDegrees_, static_cast<float>(pane.width) / static_cast<float>(pane.height), kNear, kFar);
    const float* projection = projectionMatrix.m;
    const engine_core::Matrix4 inverseProjection = engine_core::matrix4_inverse(projectionMatrix);

    bool drawn = !hasMeshes && !hasSky;
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

        // The Skybox's cubes, made again only when an image changes.
        skyReady_ = false;
        bool cubesReady = true;
        if (hasSky) {
            const SceneSky& sky = lighting_.sky;
            cubesReady = environment_.update(sky.image, sky.imageRevision, emptyVao_);
            skyReady_ = cubesReady;
            prepareSky();
        }
        glEnable(RT_GL_TEXTURE_CUBE_MAP_SEAMLESS);

        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, targetWidth_, targetHeight_);
        drawn = cubesReady && shadowPass(meshes, meshCount, projection) &&
                geometryPass(meshes, meshCount, projection) && lightPass(projection, inverseProjection.m) &&
                skyPass(inverseProjection.m) && transparencyPass(meshes, meshCount, projection, inverseProjection.m) &&
                mergePass();
    }
    if (drawn && (hasMeshes || hasSky)) {
        // The tone map, blended over the clear: where nothing was drawn the pane shows through.
        glBindFramebuffer(RT_GL_FRAMEBUFFER, static_cast<GLuint>(saved.framebuffer));
        glEnable(GL_SCISSOR_TEST);
        glScissor(clip.x, clip.y, clip.width, clip.height);
        glViewport(pane.x, pane.y, pane.width, pane.height);
        glDisable(GL_DEPTH_TEST);
        glDisable(RT_GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFuncSeparate(RT_GL_ONE, RT_GL_ONE_MINUS_SRC_ALPHA, RT_GL_ZERO, RT_GL_ONE);
        RENDER_PASS("Tone map");
        glUseProgram(tonemap_.id);
        BindTexture(kUnitScene, mergeTexture_);
        glUniform1f(tonemap_.exposure, std::max(lighting_.exposure, 0.f));
        glUniform1f(tonemap_.inverseGamma, 1.f / std::max(lighting_.gamma, 0.01f));
        glUniform1f(tonemap_.saturation, std::max(lighting_.saturation, 0.f));
        glBindVertexArray(emptyVao_);
        drawn = CanDraw(tonemap_.id);
        if (drawn) {
            DrawFullscreen(emptyVao_);
        }
    }
    // Over a pane that got only the clear, the grid would show through the
    // surfaces that should hide it, so it waits for a frame that draws them.
    if (drawn && gridVisible_) {
        gridPass(hasMeshes || hasSky ? depthTexture_ : whiteTexture_, inverseProjection.m);
    }
    // Likewise the outlines, which would show at full strength through them.
    if (drawn && !outlines_.empty()) {
        outlinePass(hasMeshes || hasSky ? depthTexture_ : whiteTexture_, projection, inverseProjection.m);
    }
    // The handles last, over everything, so they can always be grabbed.
    if (drawn && !handles_.empty()) {
        handlePass(projection);
    }

    saved.restore(viewport);
    gpu_.frame();
    return drawn;
}

void Renderer::gridPass(unsigned depth, const float* inverseProjection) {
    RENDER_PASS("Grid");
    glDisable(GL_DEPTH_TEST);
    glDisable(RT_GL_CULL_FACE);
    glEnable(GL_BLEND);
    // Straight alpha over the pane, leaving its alpha as it was.
    glBlendFuncSeparate(RT_GL_SRC_ALPHA, RT_GL_ONE_MINUS_SRC_ALPHA, RT_GL_ZERO, RT_GL_ONE);
    glUseProgram(grid_.id);
    BindTexture(kUnitDepth, depth);
    const engine_core::Matrix4 inverseView = engine_core::matrix4_inverse(view_);
    glUniformMatrix4fv(grid_.inverseProjection, 1, GL_FALSE, inverseProjection);
    glUniformMatrix4fv(grid_.inverseView, 1, GL_FALSE, inverseView.m);
    glBindVertexArray(emptyVao_);
    // A grid the driver is not ready for is left out of this frame, not the scene with it.
    if (CanDraw(grid_.id)) {
        DrawFullscreen(emptyVao_);
    }
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
    glUniform4f(program.color, draw.color[0], draw.color[1], draw.color[2], draw.color[3]);
    glUniform3f(program.emissive, draw.emissive[0], draw.emissive[1], draw.emissive[2]);
    glUniform1f(program.metalness, std::clamp(draw.metalness, 0.f, 1.f));
    glUniform1f(program.roughness, std::clamp(draw.roughness, 0.f, 1.f));
    glUniform1f(program.reflectivity, std::clamp(draw.reflectivity, 0.f, 1.f));
    glUniform1f(program.normalMapEnabled, draw.normalTexture != 0 ? 1.f : 0.f);
    glUniform1f(program.transparency, std::clamp(draw.transparency, 0.f, 1.f));
    glUniformMatrix4fv(program.model, 1, GL_FALSE, draw.model.m);
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
    // Then the sky turned back by its Rotation: what the camera sees at a
    // world direction is the sky's at that direction turned by -Rotation.
    const float angle = -lighting_.sky.rotationDegrees * 0.01745329252f;
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
        skyColor_[channel] = exposure * std::pow(std::max(lighting_.sky.tint[channel], 0.f), 2.2f);
    }
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
    glUniform1f(program.skyLightScale, std::max(lighting_.sky.lightScale, 0.f));
    glUniform1f(program.prefilteredMaxLod, EnvironmentMap::prefilteredMaxLod());
    BindTexture(kUnitSky, lighting_.sky.image);
    BindCube(kUnitIrradiance, environment_.irradiance());
    BindCube(kUnitPrefiltered, environment_.prefiltered());
    BindTexture(kUnitBrdf, environment_.brdf());
}

void Renderer::bindGBuffer(const Program& program) {
    (void)program;
    BindTexture(kUnitDepth, depthTexture_);
    BindTexture(kUnitAlbedo, albedoTexture_);
    BindTexture(kUnitNormal, normalTexture_);
    BindTexture(kUnitMaterial, materialTexture_);
    BindTexture(kUnitEmissive, emissiveTexture_);
}

bool Renderer::shadowPass(const MeshDraw* meshes, int count, const float* projection) {
    RENDER_PASS("Shadows");
    shadowLookups_.assign(shadowRequests_.size(), ShadowLookup{});
    sunLookup_ = ShadowLookup{};
    if (shadowRequests_.empty() && !hasSunShadow_) {
        return true;
    }
    CameraView camera;
    camera.world = engine_core::matrix4_inverse(view_);
    Matrix viewProjection;
    Multiply(projection, view_.m, viewProjection);
    std::copy(viewProjection, viewProjection + 16, camera.viewProjection.m);
    camera.fovYDegrees = fovYDegrees_;
    camera.aspect = static_cast<float>(targetWidth_) / static_cast<float>(targetHeight_);
    camera.nearZ = kNear;
    camera.paneHeight = targetHeight_;
    if (!shadows_.draw(shadowRequests_, meshes, count, camera, shadowSettings_)) {
        return false;
    }
    if (!shadows_.drawSun(hasSunShadow_ ? &sunShadow_ : nullptr, meshes, camera, shadowSettings_)) {
        return false;
    }
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

bool Renderer::geometryPass(const MeshDraw* meshes, int count, const float* projection) {
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
    transparent_.clear();
    bool asked = false;
    for (int index = 0; index < count; ++index) {
        const MeshDraw& draw = meshes[index];
        if (draw.mesh == nullptr || !draw.mesh->valid() || draw.transparency >= 1.f) {
            continue;
        }
        // See-through surfaces wait for the forward pass, as the legacy pipeline queued them.
        if (draw.transparency > 0.f) {
            transparent_.push_back(index);
            continue;
        }
        bindMaterial(geometry_, draw);
        CullBackFaces(draw.model.m);
        draw.mesh->bind();
        if (!asked && !CanDraw(geometry_.id)) {
            return false;
        }
        asked = true;
        draw.mesh->draw(0);
    }
    glDisable(RT_GL_CULL_FACE);
    glCullFace(RT_GL_BACK);
    return true;
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
    if (!skyReady_) {
        return true;
    }
    // Only where no opaque surface is, which no light pass wrote: no blending needed.
    glBindFramebuffer(RT_GL_FRAMEBUFFER, accumulationFbo_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(sky_.id);
    BindTexture(kUnitDepth, depthTexture_);
    glUniformMatrix4fv(sky_.inverseProjection, 1, GL_FALSE, inverseProjection);
    bindSky(sky_);
    glBindVertexArray(emptyVao_);
    if (!CanDraw(sky_.id)) {
        return false;
    }
    DrawFullscreen(emptyVao_);
    return true;
}

bool Renderer::transparencyPass(const MeshDraw* meshes, int count, const float* projection,
                                const float* inverseProjection) {
    RENDER_PASS("Transparency");
    glBindFramebuffer(RT_GL_FRAMEBUFFER, transparencyFbo_);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (transparent_.empty()) {
        return true;
    }

    // Farthest first, by each Transform's distance along the view.
    transparentDepth_.assign(static_cast<std::size_t>(count), 0.f);
    for (const int index : transparent_) {
        const float* v = view_.m;
        const float* t = meshes[index].model.m + 12;
        transparentDepth_[static_cast<std::size_t>(index)] = v[2] * t[0] + v[6] * t[1] + v[10] * t[2] + v[14];
    }
    std::stable_sort(transparent_.begin(), transparent_.end(), [this](int a, int b) {
        return transparentDepth_[static_cast<std::size_t>(a)] < transparentDepth_[static_cast<std::size_t>(b)];
    });

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
    for (const int index : transparent_) {
        const MeshDraw& draw = meshes[index];
        bindMaterial(forward_, draw);
        CullBackFaces(draw.model.m);
        draw.mesh->bind();
        if (!asked && !CanDraw(forward_.id)) {
            return false;
        }
        asked = true;
        draw.mesh->draw(0);
    }
    glDisable(RT_GL_CULL_FACE);
    glCullFace(RT_GL_BACK);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    return true;
}

bool Renderer::mergePass() {
    RENDER_PASS("Merge");
    glBindFramebuffer(RT_GL_FRAMEBUFFER, mergeFbo_);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(merge_.id);
    BindTexture(kUnitDepth, depthTexture_);
    BindTexture(kUnitEmissive, emissiveTexture_);
    BindTexture(kUnitAccumulation, accumulationTexture_);
    BindTexture(kUnitTransparency, transparencyTexture_);
    glUniform1f(merge_.skyEnabled, skyReady_ ? 1.f : 0.f);
    glBindVertexArray(emptyVao_);
    if (!CanDraw(merge_.id)) {
        return false;
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
         {&geometry_, &forward_, &ibl_, &light_, &sun_, &merge_, &tonemap_, &sky_, &grid_, &outline_, &handle_}) {
        if (program->id != 0) {
            glDeleteProgram(program->id);
        }
        *program = Program{};
    }
    environment_.shutdown();
    shadows_.shutdown();
    skyReady_ = false;
    for (unsigned* texture : {&whiteTexture_, &blackCube_}) {
        if (*texture != 0) {
            glDeleteTextures(1, texture);
            *texture = 0;
        }
    }
    for (unsigned* vao : {&emptyVao_, &sphereVao_, &outlineVao_, &handleVao_}) {
        if (*vao != 0) {
            glDeleteVertexArrays(1, vao);
            *vao = 0;
        }
    }
    for (unsigned* buffer : {&sphereVbo_, &sphereEbo_, &outlineVbo_, &handleVbo_}) {
        if (*buffer != 0) {
            glDeleteBuffers(1, buffer);
            *buffer = 0;
        }
    }
    sphereIndexCount_ = 0;
    destroyTargets();
    targetsRefused_ = false;
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
