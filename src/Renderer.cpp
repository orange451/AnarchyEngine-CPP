#include "Renderer.hpp"

#include "ShaderFile.hpp"
#include "gl.hpp"

#include <cstdio>

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
    const GLubyte* version = glGetString(GL_VERSION);
    const GLubyte* renderer = glGetString(GL_RENDERER);
    if (version == nullptr) {
        std::fprintf(stderr, "No current OpenGL context.\n");
        return false;
    }
    const char* rendererName =
        renderer != nullptr ? reinterpret_cast<const char*>(renderer) : "(unknown renderer)";
    std::printf("OpenGL %s\n%s\n", reinterpret_cast<const char*>(version), rendererName);
    std::fflush(stdout);

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

    glBindVertexArray(0);
    angleLocation_ = glGetUniformLocation(program_, "uAngle");
    glClearColor(0.06f, 0.07f, 0.09f, 1.0f);

    const GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        std::fprintf(stderr, "OpenGL error during setup: 0x%x\n", error);
        shutdown();
        return false;
    }

    ready_ = true;
    return true;
}

void Renderer::draw(int framebufferWidth, int framebufferHeight, float angleDegrees) {
    if (!ready_ || framebufferWidth <= 0 || framebufferHeight <= 0) {
        return;
    }

    // The UI pass may leave a scissor and blending enabled.
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glViewport(0, 0, framebufferWidth, framebufferHeight);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program_);
    const float radians = angleDegrees * 0.01745329252f;
    if (angleLocation_ >= 0) {
        glUniform1f(angleLocation_, radians);
    }
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
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
