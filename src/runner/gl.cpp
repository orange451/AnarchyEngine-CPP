#include "gl.hpp"

#include <cstdio>

namespace runner {

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
void (*rt_glValidateProgram)(GLuint) = nullptr;
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
void (*rt_glVertexAttribIPointer)(GLuint, GLint, GLenum, GLsizei, const void*) = nullptr;
void (*rt_glDrawElements)(GLenum, GLsizei, GLenum, const void*) = nullptr;
void (*rt_glEnable)(GLenum) = nullptr;
void (*rt_glDisable)(GLenum) = nullptr;
GLboolean (*rt_glIsEnabled)(GLenum) = nullptr;
void (*rt_glGetIntegerv)(GLenum, GLint*) = nullptr;
void (*rt_glScissor)(GLint, GLint, GLsizei, GLsizei) = nullptr;
GLint (*rt_glGetUniformLocation)(GLuint, const GLchar*) = nullptr;
void (*rt_glUniform1f)(GLint, GLfloat) = nullptr;
void (*rt_glUniform3f)(GLint, GLfloat, GLfloat, GLfloat) = nullptr;
void (*rt_glUniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*) = nullptr;
void (*rt_glUniformMatrix3fv)(GLint, GLsizei, GLboolean, const GLfloat*) = nullptr;
void (*rt_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*) = nullptr;
void (*rt_glGenTextures)(GLsizei, GLuint*) = nullptr;
void (*rt_glDeleteTextures)(GLsizei, const GLuint*) = nullptr;
void (*rt_glBindTexture)(GLenum, GLuint) = nullptr;
void (*rt_glActiveTexture)(GLenum) = nullptr;
void (*rt_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) = nullptr;
void (*rt_glTexParameteri)(GLenum, GLenum, GLint) = nullptr;
void (*rt_glGenerateMipmap)(GLenum) = nullptr;
void (*rt_glPixelStorei)(GLenum, GLint) = nullptr;
void (*rt_glUniform1i)(GLint, GLint) = nullptr;
void (*rt_glUniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
void (*rt_glUniform2f)(GLint, GLfloat, GLfloat) = nullptr;
void (*rt_glUniform1fv)(GLint, GLsizei, const GLfloat*) = nullptr;
void (*rt_glUniform3fv)(GLint, GLsizei, const GLfloat*) = nullptr;
void (*rt_glUniform4fv)(GLint, GLsizei, const GLfloat*) = nullptr;
void (*rt_glGenFramebuffers)(GLsizei, GLuint*) = nullptr;
void (*rt_glDeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
void (*rt_glBindFramebuffer)(GLenum, GLuint) = nullptr;
void (*rt_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
GLenum (*rt_glCheckFramebufferStatus)(GLenum) = nullptr;
void (*rt_glDrawBuffers)(GLsizei, const GLenum*) = nullptr;
void (*rt_glTexImage3D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) = nullptr;
void (*rt_glFramebufferTextureLayer)(GLenum, GLenum, GLuint, GLint, GLint) = nullptr;
void (*rt_glPolygonOffset)(GLfloat, GLfloat) = nullptr;
void (*rt_glReadBuffer)(GLenum) = nullptr;
void (*rt_glBlendFunc)(GLenum, GLenum) = nullptr;
void (*rt_glBlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum) = nullptr;
void (*rt_glCullFace)(GLenum) = nullptr;
void (*rt_glDepthFunc)(GLenum) = nullptr;
void (*rt_glDepthMask)(GLboolean) = nullptr;
void (*rt_glGetBooleanv)(GLenum, GLboolean*) = nullptr;
void (*rt_glGenQueries)(GLsizei, GLuint*) = nullptr;
void (*rt_glDeleteQueries)(GLsizei, const GLuint*) = nullptr;
void (*rt_glQueryCounter)(GLuint, GLenum) = nullptr;
void (*rt_glBeginQuery)(GLenum, GLuint) = nullptr;
void (*rt_glEndQuery)(GLenum) = nullptr;
void (*rt_glGetQueryObjectiv)(GLuint, GLenum, GLint*) = nullptr;
void (*rt_glGetQueryObjectui64v)(GLuint, GLenum, GLuint64*) = nullptr;
void (*rt_glGetInteger64v)(GLenum, GLint64*) = nullptr;

bool GlTimerQueries() {
    return rt_glGenQueries != nullptr && rt_glDeleteQueries != nullptr && rt_glBeginQuery != nullptr &&
           rt_glEndQuery != nullptr && rt_glGetQueryObjectiv != nullptr && rt_glGetQueryObjectui64v != nullptr;
}

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
    LOAD(ValidateProgram);
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
    LOAD(VertexAttribIPointer);
    LOAD(DrawElements);
    LOAD(Enable);
    LOAD(Disable);
    LOAD(IsEnabled);
    LOAD(GetIntegerv);
    LOAD(Scissor);
    LOAD(GetUniformLocation);
    LOAD(Uniform1f);
    LOAD(Uniform3f);
    LOAD(UniformMatrix4fv);
    LOAD(UniformMatrix3fv);
    LOAD(ReadPixels);
    LOAD(GenTextures);
    LOAD(DeleteTextures);
    LOAD(BindTexture);
    LOAD(ActiveTexture);
    LOAD(TexImage2D);
    LOAD(TexParameteri);
    LOAD(GenerateMipmap);
    LOAD(PixelStorei);
    LOAD(Uniform1i);
    LOAD(Uniform4f);
    LOAD(Uniform2f);
    LOAD(Uniform1fv);
    LOAD(Uniform3fv);
    LOAD(Uniform4fv);
    LOAD(GenFramebuffers);
    LOAD(DeleteFramebuffers);
    LOAD(BindFramebuffer);
    LOAD(FramebufferTexture2D);
    LOAD(CheckFramebufferStatus);
    LOAD(DrawBuffers);
    LOAD(TexImage3D);
    LOAD(FramebufferTextureLayer);
    LOAD(PolygonOffset);
    LOAD(ReadBuffer);
    LOAD(BlendFunc);
    LOAD(BlendFuncSeparate);
    LOAD(CullFace);
    LOAD(DepthFunc);
    LOAD(DepthMask);
    LOAD(GetBooleanv);

#undef LOAD
    // Optional entry points: missing ones stay null.
#define LOAD_OPTIONAL(suffix) \
    rt_gl##suffix = reinterpret_cast<decltype(rt_gl##suffix)>(get_proc("gl" #suffix))
    LOAD_OPTIONAL(GenQueries);
    LOAD_OPTIONAL(DeleteQueries);
    LOAD_OPTIONAL(QueryCounter);
    LOAD_OPTIONAL(BeginQuery);
    LOAD_OPTIONAL(EndQuery);
    LOAD_OPTIONAL(GetQueryObjectiv);
    LOAD_OPTIONAL(GetQueryObjectui64v);
    LOAD_OPTIONAL(GetInteger64v);
#undef LOAD_OPTIONAL
    return true;
}

}  // namespace runner
