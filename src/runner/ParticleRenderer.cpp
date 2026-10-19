#include "ParticleRenderer.hpp"

#include "ShaderFile.hpp"
#include "gl.hpp"

#include <algorithm>
#include <cstddef>
#include <numeric>

namespace runner {
namespace {

using engine_core::ParticleBlendMode;
using engine_core::ParticleDraw;
using engine_core::particles::ParticleVertex;

// Units the particle program samples: the texture, then the depth copy.
// Material units, which every mesh draw binds again for itself.
constexpr int kUnitTexture = 0;
constexpr int kUnitDepthCopy = 1;
// The sky cube the air takes its color from.
constexpr int kUnitAtmosphereSky = 2;

float distance2(const ParticleDraw& draw, const float* eye) {
    const float x = (draw.bounds_min.x + draw.bounds_max.x) * 0.5f - eye[0];
    const float y = (draw.bounds_min.y + draw.bounds_max.y) * 0.5f - eye[1];
    const float z = (draw.bounds_min.z + draw.bounds_max.z) * 0.5f - eye[2];
    return x * x + y * y + z * z;
}

// False when every corner of the box is outside one side of the view.
bool in_view(const ParticleDraw& draw, const float* view, const float* projection) {
    float clip[8][4];
    for (int i = 0; i < 8; ++i) {
        const float p[3] = {(i & 1) != 0 ? draw.bounds_max.x : draw.bounds_min.x,
                            (i & 2) != 0 ? draw.bounds_max.y : draw.bounds_min.y,
                            (i & 4) != 0 ? draw.bounds_max.z : draw.bounds_min.z};
        float v[4];
        for (int r = 0; r < 4; ++r) {
            v[r] = view[r] * p[0] + view[4 + r] * p[1] + view[8 + r] * p[2] + view[12 + r];
        }
        for (int r = 0; r < 4; ++r) {
            clip[i][r] = projection[r] * v[0] + projection[4 + r] * v[1] + projection[8 + r] * v[2] +
                         projection[12 + r] * v[3];
        }
    }
    for (int axis = 0; axis < 3; ++axis) {
        for (const float sign : {-1.f, 1.f}) {
            bool all_out = true;
            for (int i = 0; i < 8 && all_out; ++i) {
                all_out = sign * clip[i][axis] > clip[i][3];
            }
            if (all_out) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

bool ParticleRenderer::create() {
    const std::string vertex = LoadShader("pipeline/particle.vert");
    const std::string fragment = LoadShader("pipeline/particle.frag", {"pipeline/atmosphere.glsl"});
    if (vertex.empty() || fragment.empty()) {
        return false;
    }
    program_ = LinkProgram(vertex, fragment, "Particle");
    if (program_ == 0) {
        return false;
    }
    const auto at = [this](const char* name) { return glGetUniformLocation(program_, name); };
    u_.view = at("uView");
    u_.projection = at("uProjection");
    u_.inverseProjection = at("uInverseProjection");
    u_.cameraPosition = at("uCameraPosition");
    u_.cameraRight = at("uCameraRight");
    u_.cameraUp = at("uCameraUp");
    u_.facing = at("uFacing");
    u_.facingAxis = at("uFacingAxis");
    u_.velocityStretch = at("uVelocityStretch");
    u_.grid = at("uGrid");
    u_.texture = at("uTexture");
    u_.hasTexture = at("uHasTexture");
    u_.sceneDepth = at("uSceneDepth");
    u_.targetSize = at("uTargetSize");
    u_.softEdge = at("uSoftEdge");
    u_.brightness = at("uBrightness");
    u_.lightInfluence = at("uLightInfluence");
    u_.blend = at("uBlend");
    u_.ambient = at("uAmbient");
    u_.lightCount = at("uLightCount");
    u_.lightPositionRadius = at("uLightPositionRadius");
    u_.lightColorIntensity = at("uLightColorIntensity");
    u_.lightDirection = at("uLightDirection");
    u_.lightCone = at("uLightCone");
    u_.atmosphere = FindAtmosphere(program_);
    glUseProgram(program_);
    glUniform1i(u_.texture, kUnitTexture);
    glUniform1i(u_.sceneDepth, kUnitDepthCopy);
    glUniform1i(u_.atmosphere.sky, kUnitAtmosphereSky);
    glUseProgram(0);

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    const float corners[] = {-0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f};
    const unsigned indices[] = {0, 1, 2, 0, 2, 3};
    glGenBuffers(1, &quad_);
    glBindBuffer(GL_ARRAY_BUFFER, quad_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glGenBuffers(1, &indices_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glGenBuffers(1, &instances_);
    glBindBuffer(GL_ARRAY_BUFFER, instances_);
    const auto stride = static_cast<GLsizei>(sizeof(ParticleVertex));
    const auto attribute = [stride](GLuint location, GLint size, std::size_t offset) {
        glEnableVertexAttribArray(location);
        glVertexAttribPointer(location, size, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offset));
        glVertexAttribDivisor(location, 1);
    };
    attribute(1, 3, offsetof(ParticleVertex, position));
    attribute(2, 1, offsetof(ParticleVertex, size));
    attribute(3, 4, offsetof(ParticleVertex, color));
    attribute(4, 3, offsetof(ParticleVertex, velocity));
    attribute(5, 1, offsetof(ParticleVertex, rotation));
    attribute(6, 1, offsetof(ParticleVertex, stretch));
    attribute(7, 1, offsetof(ParticleVertex, frame));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

void ParticleRenderer::destroy() {
    if (program_ != 0) {
        glDeleteProgram(program_);
        program_ = 0;
    }
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    const GLuint buffers[] = {quad_, indices_, instances_};
    for (GLuint buffer : buffers) {
        if (buffer != 0) {
            glDeleteBuffers(1, &buffer);
        }
    }
    quad_ = indices_ = instances_ = 0;
    if (depthFbo_ != 0) {
        glDeleteFramebuffers(1, &depthFbo_);
        depthFbo_ = 0;
    }
    if (depthCopy_ != 0) {
        glDeleteTextures(1, &depthCopy_);
        depthCopy_ = 0;
    }
    depthWidth_ = depthHeight_ = 0;
    depthComplete_ = false;
}

bool ParticleRenderer::ensureDepthCopy(int width, int height) {
    if (depthCopy_ != 0 && depthWidth_ == width && depthHeight_ == height) {
        return depthComplete_;
    }
    if (depthFbo_ == 0) {
        glGenFramebuffers(1, &depthFbo_);
    }
    if (depthCopy_ != 0) {
        glDeleteTextures(1, &depthCopy_);
    }
    glGenTextures(1, &depthCopy_);
    glBindTexture(GL_TEXTURE_2D, depthCopy_);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(RT_GL_DEPTH_COMPONENT24), width, height, 0,
                 RT_GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(RT_GL_NEAREST));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(RT_GL_NEAREST));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(RT_GL_CLAMP_TO_EDGE));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(RT_GL_CLAMP_TO_EDGE));
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(RT_GL_FRAMEBUFFER, depthFbo_);
    glFramebufferTexture2D(RT_GL_FRAMEBUFFER, RT_GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthCopy_, 0);
    const bool complete = glCheckFramebufferStatus(RT_GL_FRAMEBUFFER) == RT_GL_FRAMEBUFFER_COMPLETE;
    depthWidth_ = width;
    depthHeight_ = height;
    depthComplete_ = complete;
    return complete;
}

bool ParticleRenderer::draw(const ParticleBatch* batches, int count, const ParticleFrame& frame,
                            unsigned readFramebuffer) {
    if (program_ == 0 || batches == nullptr || count <= 0) {
        return true;
    }
    // The soft edges read a copy of the depth the see-through target writes against.
    const bool copied = ensureDepthCopy(frame.targetWidth, frame.targetHeight);
    lastSoft_ = copied;
    if (copied) {
        glBindFramebuffer(RT_GL_READ_FRAMEBUFFER, readFramebuffer);
        glBindFramebuffer(RT_GL_DRAW_FRAMEBUFFER, depthFbo_);
        glBlitFramebuffer(0, 0, frame.targetWidth, frame.targetHeight, 0, 0, frame.targetWidth, frame.targetHeight,
                          GL_DEPTH_BUFFER_BIT, RT_GL_NEAREST);
    }
    glBindFramebuffer(RT_GL_FRAMEBUFFER, readFramebuffer);

    order_.resize(static_cast<std::size_t>(count));
    std::iota(order_.begin(), order_.end(), 0);
    std::stable_sort(order_.begin(), order_.end(), [&](int a, int b) {
        const ParticleDraw& da = batches[a].draw;
        const ParticleDraw& db = batches[b].draw;
        if (da.draw_order != db.draw_order) {
            return da.draw_order < db.draw_order;
        }
        return distance2(da, frame.cameraPosition) > distance2(db, frame.cameraPosition);
    });

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    // Premultiplied over, for all three blend modes (the shader shapes each).
    glBlendFuncSeparate(RT_GL_ONE, RT_GL_ONE_MINUS_SRC_ALPHA, RT_GL_ONE, RT_GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(program_);
    if (!CanDraw(program_)) {
        return false;
    }
    glUniformMatrix4fv(u_.view, 1, GL_FALSE, frame.view);
    glUniformMatrix4fv(u_.projection, 1, GL_FALSE, frame.projection);
    glUniformMatrix4fv(u_.inverseProjection, 1, GL_FALSE, frame.inverseProjection);
    glUniform3fv(u_.cameraPosition, 1, frame.cameraPosition);
    glUniform3fv(u_.cameraRight, 1, frame.cameraRight);
    glUniform3fv(u_.cameraUp, 1, frame.cameraUp);
    glUniform2f(u_.targetSize, static_cast<float>(frame.targetWidth), static_cast<float>(frame.targetHeight));
    glUniform3fv(u_.ambient, 1, frame.ambient);
    glUniform1i(u_.lightCount, frame.lightCount);
    if (frame.lightCount > 0) {
        glUniform4fv(u_.lightPositionRadius, frame.lightCount, frame.lightPositionRadius);
        glUniform4fv(u_.lightColorIntensity, frame.lightCount, frame.lightColorIntensity);
        glUniform4fv(u_.lightDirection, frame.lightCount, frame.lightDirections);
        glUniform4fv(u_.lightCone, frame.lightCount, frame.lightCones);
    }
    UploadAtmosphere(u_.atmosphere, frame.atmosphere != nullptr ? *frame.atmosphere : AtmosphereFrame{},
                     kUnitAtmosphereSky);
    glActiveTexture(GL_TEXTURE0 + kUnitDepthCopy);
    glBindTexture(GL_TEXTURE_2D, copied ? depthCopy_ : 0);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, instances_);
    for (int index : order_) {
        const ParticleBatch& batch = batches[index];
        const ParticleDraw& d = batch.draw;
        if (!d.particles || d.particles->empty() || !in_view(d, frame.view, frame.projection)) {
            continue;
        }
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(d.particles->size() * sizeof(ParticleVertex)),
                     d.particles->data(), RT_GL_STREAM_DRAW);
        glActiveTexture(GL_TEXTURE0 + kUnitTexture);
        glBindTexture(GL_TEXTURE_2D, batch.texture);
        glUniform1i(u_.hasTexture, batch.texture != 0 ? 1 : 0);
        glUniform1i(u_.facing, static_cast<int>(d.facing));
        glUniform3f(u_.facingAxis, d.facing_axis.x, d.facing_axis.y, d.facing_axis.z);
        glUniform1f(u_.velocityStretch, d.velocity_stretch);
        glUniform2f(u_.grid, static_cast<float>(std::max(1, d.grid_x)), static_cast<float>(std::max(1, d.grid_y)));
        glUniform1f(u_.softEdge, copied ? d.soft_edge : 0.f);
        glUniform1f(u_.brightness, d.brightness);
        glUniform1f(u_.lightInfluence, d.light_influence);
        glUniform1i(u_.blend, static_cast<int>(d.blend));
        glDrawElementsInstanced(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr, static_cast<GLsizei>(d.particles->size()));
    }
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glActiveTexture(GL_TEXTURE0 + kUnitDepthCopy);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    return true;
}

}  // namespace runner
