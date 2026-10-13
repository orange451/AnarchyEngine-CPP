#pragma once

#include "ParticleWorld.hpp"

#include <vector>

namespace runner {

// One emitter to draw: its particles, and its texture as a GL name (0 draws
// the built-in round dot).
struct ParticleBatch {
    const engine_core::ParticleDraw* draw = nullptr;
    unsigned texture = 0;
};

// What the particle pass needs from the frame. Lights are in view space, as
// the see-through pass takes them.
struct ParticleFrame {
    const float* view = nullptr;
    const float* projection = nullptr;
    const float* inverseProjection = nullptr;
    float cameraPosition[3] = {};
    float cameraRight[3] = {1.f, 0.f, 0.f};
    float cameraUp[3] = {0.f, 1.f, 0.f};
    int targetWidth = 0;
    int targetHeight = 0;
    float ambient[3] = {};
    int lightCount = 0;
    const float* lightPositionRadius = nullptr;
    const float* lightColorIntensity = nullptr;
    const float* lightDirections = nullptr;
    const float* lightCones = nullptr;
};

// Draws particles as instanced quads into the bound see-through target, one
// call per emitter, DrawOrder first and then far to near, premultiplied
// (spec docs/superpowers/specs/2026-10-12-particle-emitter-design.md). Owns
// its program, quad, instance buffer, and a copy of the scene's depth for
// soft edges, since the see-through target has the depth itself attached.
// UI thread, GL context current.
class ParticleRenderer {
public:
    // False when the shaders do not build.
    bool create();
    void destroy();
    // Copies the depth of readFramebuffer (width x height) into the copy the
    // soft edges read, then draws batches with the target framebuffer bound
    // again. Leaves blending and depth state as it found them: depth test on,
    // depth writes off, blending on.
    bool draw(const ParticleBatch* batches, int count, const ParticleFrame& frame, unsigned readFramebuffer);

private:
    bool ensureDepthCopy(int width, int height);

    unsigned program_ = 0;
    unsigned vao_ = 0;
    unsigned quad_ = 0;
    unsigned indices_ = 0;
    unsigned instances_ = 0;
    unsigned depthCopy_ = 0;
    unsigned depthFbo_ = 0;
    int depthWidth_ = 0;
    int depthHeight_ = 0;
    std::vector<int> order_;
    struct Uniforms {
        int view = -1, projection = -1, inverseProjection = -1, cameraPosition = -1, cameraRight = -1,
            cameraUp = -1, facing = -1, facingAxis = -1, velocityStretch = -1, grid = -1, texture = -1,
            hasTexture = -1, sceneDepth = -1, targetSize = -1, softEdge = -1, brightness = -1,
            lightInfluence = -1, blend = -1, ambient = -1, lightCount = -1, lightPositionRadius = -1,
            lightColorIntensity = -1, lightDirection = -1, lightCone = -1;
    } u_;
};

}  // namespace runner
