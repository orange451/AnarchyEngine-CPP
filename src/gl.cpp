#include "gl.hpp"

#include <cstdio>

const GLubyte* (*rt_glGetString)(GLenum) = nullptr;
GLenum (*rt_glGetError)() = nullptr;
void (*rt_glClear)(GLbitfield) = nullptr;
void (*rt_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
void (*rt_glViewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
GLuint (*rt_glCreateShader)(GLenum) = nullptr;
void (*rt_glShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
void (*rt_glCompileShader)(GLuint) = nullptr;
void (*rt_glGetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
void (*rt_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
void (*rt_glDeleteShader)(GLuint) = nullptr;
GLuint (*rt_glCreateProgram)() = nullptr;
void (*rt_glAttachShader)(GLuint, GLuint) = nullptr;
void (*rt_glLinkProgram)(GLuint) = nullptr;
void (*rt_glDeleteProgram)(GLuint) = nullptr;
void (*rt_glGetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
void (*rt_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
void (*rt_glUseProgram)(GLuint) = nullptr;
void (*rt_glGenVertexArrays)(GLsizei, GLuint*) = nullptr;
void (*rt_glDeleteVertexArrays)(GLsizei, const GLuint*) = nullptr;
void (*rt_glBindVertexArray)(GLuint) = nullptr;
void (*rt_glGenBuffers)(GLsizei, GLuint*) = nullptr;
void (*rt_glDeleteBuffers)(GLsizei, const GLuint*) = nullptr;
void (*rt_glBindBuffer)(GLenum, GLuint) = nullptr;
void (*rt_glBufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
void (*rt_glEnableVertexAttribArray)(GLuint) = nullptr;
void (*rt_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) = nullptr;
void (*rt_glDrawArrays)(GLenum, GLint, GLsizei) = nullptr;
void (*rt_glDisable)(GLenum) = nullptr;
GLint (*rt_glGetUniformLocation)(GLuint, const GLchar*) = nullptr;
void (*rt_glUniform1f)(GLint, GLfloat) = nullptr;

bool LoadGl(GlGetProcAddress get_proc) {
    if (get_proc == nullptr) {
        std::fprintf(stderr, "No OpenGL entry-point loader.\n");
        return false;
    }

#define LOAD(suffix)                                                                 \
    do {                                                                             \
        rt_gl##suffix =                                                            \
            reinterpret_cast<decltype(rt_gl##suffix)>(get_proc("gl" #suffix));     \
        if (rt_gl##suffix == nullptr) {                                            \
            std::fprintf(stderr, "OpenGL entry point gl" #suffix " is unavailable.\n"); \
            return false;                                                            \
        }                                                                            \
    } while (0)

    LOAD(GetString);
    LOAD(GetError);
    LOAD(Clear);
    LOAD(ClearColor);
    LOAD(Viewport);
    LOAD(CreateShader);
    LOAD(ShaderSource);
    LOAD(CompileShader);
    LOAD(GetShaderiv);
    LOAD(GetShaderInfoLog);
    LOAD(DeleteShader);
    LOAD(CreateProgram);
    LOAD(AttachShader);
    LOAD(LinkProgram);
    LOAD(DeleteProgram);
    LOAD(GetProgramiv);
    LOAD(GetProgramInfoLog);
    LOAD(UseProgram);
    LOAD(GenVertexArrays);
    LOAD(DeleteVertexArrays);
    LOAD(BindVertexArray);
    LOAD(GenBuffers);
    LOAD(DeleteBuffers);
    LOAD(BindBuffer);
    LOAD(BufferData);
    LOAD(EnableVertexAttribArray);
    LOAD(VertexAttribPointer);
    LOAD(DrawArrays);
    LOAD(Disable);
    LOAD(GetUniformLocation);
    LOAD(Uniform1f);

#undef LOAD
    return true;
}
