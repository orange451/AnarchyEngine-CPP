#include "Renderer.hpp"

#include "ShaderFile.hpp"
#include "gl.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace runner {
namespace {
    // Red, green, and blue corners. Interpolation fills in yellow, cyan, and magenta.
    const float kVertices[] = {
        // x,      y,     r,    g,    b
        -0.75f, -0.65f,  1.0f, 0.0f, 0.0f,
        0.75f, -0.65f,  0.0f, 1.0f, 0.0f,
        0.00f,  0.75f,  0.0f, 0.0f, 1.0f,
    };
}  // namespace

bool Renderer::initialize() {
    if (ready_) {
        return true;
    }

    if (glGetString(GL_VERSION) == nullptr) {
        std::fprintf(stderr, "No current OpenGL context.\n");
        return false;
    }

    program_ = LinkProgram(LoadShader("triangle.vert"), LoadShader("triangle.frag"), "Triangle");
    if (program_ == 0) {
        shutdown();
        return false;
    }

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof kVertices, kVertices, GL_STATIC_DRAW);

    const GLsizei stride = 5 * static_cast<GLsizei>(sizeof(float));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(
        1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(2 * sizeof(float)));

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    angleLocation_ = glGetUniformLocation(program_, "uAngle");
    positionLocation_ = glGetUniformLocation(program_, "uPosition");
    // Same dark gray as the Scene View pane, so a one-pixel seam does not show.
    glClearColor(30.f / 255.f, 30.f / 255.f, 30.f / 255.f, 1.0f);

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
                    const TriangleDraw* triangles, int count) {
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
    // so a parent clip cuts pixels without sliding the triangle.
    glEnable(GL_SCISSOR_TEST);
    glScissor(clip.x, clip.y, clip.width, clip.height);
    glViewport(pane.x, pane.y, pane.width, pane.height);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glClearColor(30.f / 255.f, 30.f / 255.f, 30.f / 255.f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(program_);
    glBindVertexArray(vao_);
    if (triangles != nullptr && count > 0) {
        for (int index = 0; index < count; ++index) {
            const TriangleDraw& triangle = triangles[index];
            if (angleLocation_ >= 0) {
                glUniform1f(angleLocation_, triangle.angleDegrees * 0.01745329252f);
            }
            if (positionLocation_ >= 0) {
                glUniform3f(positionLocation_, triangle.x, triangle.y, triangle.z);
            }
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    }
    glBindVertexArray(0);
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

void Renderer::shutdown() {
    ready_ = false;
    if (vbo_ != 0) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    if (program_ != 0) {
        glDeleteProgram(program_);
        program_ = 0;
    }
}

}  // namespace runner
