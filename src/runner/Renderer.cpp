#include "Renderer.hpp"

#include "ShaderFile.hpp"
#include "amesh.hpp"
#include "gl.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace runner {

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

    meshProgram_ = LinkProgram(LoadShader("mesh.vert"), LoadShader("mesh.frag"), "Mesh");
    if (meshProgram_ == 0) {
        shutdown();
        return false;
    }
    modelLocation_ = glGetUniformLocation(meshProgram_, "uModel");
    viewProjectionLocation_ = glGetUniformLocation(meshProgram_, "uViewProjection");
    diffuseLocation_ = glGetUniformLocation(meshProgram_, "uDiffuse");
    colorLocation_ = glGetUniformLocation(meshProgram_, "uColor");

    const unsigned char white[4] = {255, 255, 255, 255};
    glGenTextures(1, &whiteTexture_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, whiteTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
    glBindTexture(GL_TEXTURE_2D, 0);

    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        std::fprintf(stderr, "OpenGL error during setup: 0x%x\n", error);
        shutdown();
        return false;
    }

    ready_ = true;
    return true;
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

}  // namespace

void Renderer::draw(double x, double y, double width, double height, double sceneWidth, double sceneHeight,
                    const MeshDraw* meshes, int meshCount) {
    if (!ready_ || width <= 0.0 || height <= 0.0 || sceneWidth <= 0.0 || sceneHeight <= 0.0) {
        return;
    }

    GLint viewport[4] = {};
    GLint scissorBox[4] = {};
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
    if (viewport[2] <= 0 || viewport[3] <= 0) {
        return;
    }

    const PixelRect pane = PanePixels(x, y, width, height, sceneWidth, sceneHeight, viewport);
    if (pane.width <= 0 || pane.height <= 0) {
        return;
    }

    const GLboolean scissorWasOn = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean blendWasOn = glIsEnabled(GL_BLEND);
    const GLboolean depthWasOn = glIsEnabled(GL_DEPTH_TEST);
    PixelRect clip = pane;
    if (scissorWasOn == GL_TRUE) {
        const PixelRect outer{scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]};
        clip = Intersect(pane, outer);
        if (clip.width <= 0 || clip.height <= 0) {
            return;
        }
    }

    // Scissor limits the clear to this pane. The viewport stays the whole pane
    // so a parent clip cuts pixels without sliding the drawing.
    glEnable(GL_SCISSOR_TEST);
    glScissor(clip.x, clip.y, clip.width, clip.height);
    glViewport(pane.x, pane.y, pane.width, pane.height);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glClearColor(clear_[0], clear_[1], clear_[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    drawMeshes(meshes, meshCount, static_cast<float>(pane.width) / static_cast<float>(pane.height));
    if (depthWasOn != GL_TRUE) {
        glDisable(GL_DEPTH_TEST);
    }

    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    if (scissorWasOn == GL_TRUE) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    if (blendWasOn == GL_TRUE) {
        glEnable(GL_BLEND);
    }
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
    ready_ = false;
    if (meshProgram_ != 0) {
        glDeleteProgram(meshProgram_);
        meshProgram_ = 0;
    }
    if (whiteTexture_ != 0) {
        glDeleteTextures(1, &whiteTexture_);
        whiteTexture_ = 0;
    }
}

namespace {

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

// Right-handed and Y up: the camera looks down its -Z.
void LookAt(const float* eye, const float* target, float* out) {
    float f[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    const float fLength = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (float& v : f) {
        v /= fLength;
    }
    // side = forward x up, with up = +Y.
    float s[3] = {-f[2], 0.f, f[0]};
    const float sLength = std::sqrt(s[0] * s[0] + s[2] * s[2]);
    for (float& v : s) {
        v /= sLength;
    }
    const float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
    const float m[16] = {
        s[0], u[0], -f[0], 0.f,
        s[1], u[1], -f[1], 0.f,
        s[2], u[2], -f[2], 0.f,
        -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]),
        -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]),
        f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2],
        1.f,
    };
    std::copy(m, m + 16, out);
}

// OpenGL clip space: depth -1 at near, 1 at far.
void Perspective(float fovYDegrees, float aspect, float nearZ, float farZ, float* out) {
    const float f = 1.f / std::tan(fovYDegrees * 0.5f * 0.01745329252f);
    std::fill(out, out + 16, 0.f);
    out[0] = f / aspect;
    out[5] = f;
    out[10] = (farZ + nearZ) / (nearZ - farZ);
    out[11] = -1.f;
    out[14] = 2.f * farZ * nearZ / (nearZ - farZ);
}

}  // namespace

engine_core::Matrix4 Renderer::DefaultView() {
    engine_core::Matrix4 view;
    LookAt(kCameraEye, kCameraTarget, view.m);
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

void Renderer::drawMeshes(const MeshDraw* meshes, int count, float aspect) {
    if (meshProgram_ == 0 || meshes == nullptr || count <= 0 || !(aspect > 0.f)) {
        return;
    }
    Matrix projection;
    Matrix viewProjection;
    Perspective(fovYDegrees_, aspect, 0.1f, 1000.f, projection);
    Multiply(projection, view_.m, viewProjection);
    glUseProgram(meshProgram_);
    glUniformMatrix4fv(viewProjectionLocation_, 1, GL_FALSE, viewProjection);
    // Unit 0, the one JadeFX draws with, left active with nothing bound after.
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(diffuseLocation_, 0);
    for (int index = 0; index < count; ++index) {
        const MeshDraw& draw = meshes[index];
        if (draw.mesh == nullptr || !draw.mesh->valid()) {
            continue;
        }
        glUniformMatrix4fv(modelLocation_, 1, GL_FALSE, draw.model.m);
        glUniform4f(colorLocation_, draw.color[0], draw.color[1], draw.color[2], draw.color[3]);
        glBindTexture(GL_TEXTURE_2D, draw.texture != 0 ? draw.texture : whiteTexture_);
        draw.mesh->bind();
        draw.mesh->draw(0);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindVertexArray(0);
}

}  // namespace runner
