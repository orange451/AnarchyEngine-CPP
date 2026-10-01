#pragma once

#include <cstddef>

// Windows ships OpenGL 1.1 in opengl32.dll. macOS and Linux export newer entry
// points from the driver library. Load every call this program makes after a
// context exists, so the same source links on all three.

namespace runner {

using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLint = int;
using GLuint = unsigned int;
using GLsizei = int;
using GLfloat = float;
using GLubyte = unsigned char;
using GLchar = char;
using GLsizeiptr = std::ptrdiff_t;

// A platform OpenGL header may already have defined these as macros.
#ifdef GL_FALSE
#undef GL_FALSE
#endif
#ifdef GL_VIEWPORT
#undef GL_VIEWPORT
#endif
#ifdef GL_SCISSOR_BOX
#undef GL_SCISSOR_BOX
#endif
#ifdef GL_TRUE
#undef GL_TRUE
#endif
#ifdef GL_NO_ERROR
#undef GL_NO_ERROR
#endif
#ifdef GL_TRIANGLES
#undef GL_TRIANGLES
#endif
#ifdef GL_COLOR_BUFFER_BIT
#undef GL_COLOR_BUFFER_BIT
#endif
#ifdef GL_VERSION
#undef GL_VERSION
#endif
#ifdef GL_RENDERER
#undef GL_RENDERER
#endif
#ifdef GL_FLOAT
#undef GL_FLOAT
#endif
#ifdef GL_ARRAY_BUFFER
#undef GL_ARRAY_BUFFER
#endif
#ifdef GL_STATIC_DRAW
#undef GL_STATIC_DRAW
#endif
#ifdef GL_FRAGMENT_SHADER
#undef GL_FRAGMENT_SHADER
#endif
#ifdef GL_VERTEX_SHADER
#undef GL_VERTEX_SHADER
#endif
#ifdef GL_COMPILE_STATUS
#undef GL_COMPILE_STATUS
#endif
#ifdef GL_LINK_STATUS
#undef GL_LINK_STATUS
#endif
#ifdef GL_VALIDATE_STATUS
#undef GL_VALIDATE_STATUS
#endif
#ifdef GL_BLEND
#undef GL_BLEND
#endif
#ifdef GL_SCISSOR_TEST
#undef GL_SCISSOR_TEST
#endif
#ifdef GL_DEPTH_TEST
#undef GL_DEPTH_TEST
#endif
#ifdef GL_DEPTH_BUFFER_BIT
#undef GL_DEPTH_BUFFER_BIT
#endif
#ifdef GL_RGBA
#undef GL_RGBA
#endif
#ifdef GL_UNSIGNED_BYTE
#undef GL_UNSIGNED_BYTE
#endif
#ifdef GL_UNSIGNED_SHORT
#undef GL_UNSIGNED_SHORT
#endif
#ifdef GL_UNSIGNED_INT
#undef GL_UNSIGNED_INT
#endif
#ifdef GL_ELEMENT_ARRAY_BUFFER
#undef GL_ELEMENT_ARRAY_BUFFER
#endif
#ifdef GL_DYNAMIC_DRAW
#undef GL_DYNAMIC_DRAW
#endif
#ifdef GL_TEXTURE_2D
#undef GL_TEXTURE_2D
#endif
#ifdef GL_TEXTURE0
#undef GL_TEXTURE0
#endif
#ifdef GL_ACTIVE_TEXTURE
#undef GL_ACTIVE_TEXTURE
#endif
#ifdef GL_TEXTURE_BINDING_2D
#undef GL_TEXTURE_BINDING_2D
#endif
#ifdef GL_TEXTURE_MIN_FILTER
#undef GL_TEXTURE_MIN_FILTER
#endif
#ifdef GL_TEXTURE_MAG_FILTER
#undef GL_TEXTURE_MAG_FILTER
#endif
#ifdef GL_TEXTURE_WRAP_S
#undef GL_TEXTURE_WRAP_S
#endif
#ifdef GL_TEXTURE_WRAP_T
#undef GL_TEXTURE_WRAP_T
#endif
#ifdef GL_LINEAR
#undef GL_LINEAR
#endif
#ifdef GL_LINEAR_MIPMAP_LINEAR
#undef GL_LINEAR_MIPMAP_LINEAR
#endif
#ifdef GL_REPEAT
#undef GL_REPEAT
#endif
#ifdef GL_RGBA8
#undef GL_RGBA8
#endif
#ifdef GL_UNPACK_ALIGNMENT
#undef GL_UNPACK_ALIGNMENT
#endif
constexpr GLboolean GL_FALSE = 0;
constexpr GLboolean GL_TRUE = 1;
constexpr GLenum GL_NO_ERROR = 0;
constexpr GLenum GL_TRIANGLES = 0x0004;
constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x00004000;
constexpr GLenum GL_VERSION = 0x1F02;
constexpr GLenum GL_RENDERER = 0x1F01;
constexpr GLenum GL_FLOAT = 0x1406;
constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
constexpr GLenum GL_STATIC_DRAW = 0x88E4;
constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
constexpr GLenum GL_LINK_STATUS = 0x8B82;
constexpr GLenum GL_VALIDATE_STATUS = 0x8B83;
constexpr GLenum GL_BLEND = 0x0BE2;
constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
constexpr GLenum GL_DEPTH_TEST = 0x0B71;
constexpr GLbitfield GL_DEPTH_BUFFER_BIT = 0x00000100;
constexpr GLenum GL_VIEWPORT = 0x0BA2;
constexpr GLenum GL_SCISSOR_BOX = 0x0C10;
constexpr GLenum GL_RGBA = 0x1908;
constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
constexpr GLenum GL_UNSIGNED_SHORT = 0x1403;
constexpr GLenum GL_UNSIGNED_INT = 0x1405;
constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
constexpr GLenum GL_DYNAMIC_DRAW = 0x88E8;
constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
constexpr GLenum GL_TEXTURE0 = 0x84C0;
constexpr GLenum GL_ACTIVE_TEXTURE = 0x84E0;
constexpr GLenum GL_TEXTURE_BINDING_2D = 0x8069;
constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
constexpr GLenum GL_LINEAR = 0x2601;
constexpr GLenum GL_LINEAR_MIPMAP_LINEAR = 0x2703;
constexpr GLenum GL_REPEAT = 0x2901;
constexpr GLenum GL_RGBA8 = 0x8058;
constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;

// The Scene View's offscreen passes. Prefixed RT_ so no platform header's
// macros of the GL names can collide with them.
constexpr GLenum RT_GL_FRAMEBUFFER = 0x8D40;
constexpr GLenum RT_GL_FRAMEBUFFER_BINDING = 0x8CA6;
constexpr GLenum RT_GL_FRAMEBUFFER_COMPLETE = 0x8CD5;
constexpr GLenum RT_GL_COLOR_ATTACHMENT0 = 0x8CE0;
constexpr GLenum RT_GL_DEPTH_ATTACHMENT = 0x8D00;
constexpr GLenum RT_GL_RGBA16F = 0x881A;
constexpr GLenum RT_GL_HALF_FLOAT = 0x140B;
constexpr GLenum RT_GL_DEPTH_COMPONENT = 0x1902;
constexpr GLenum RT_GL_DEPTH_COMPONENT24 = 0x81A6;
constexpr GLenum RT_GL_NEAREST = 0x2600;
constexpr GLenum RT_GL_CLAMP_TO_EDGE = 0x812F;
constexpr GLenum RT_GL_CULL_FACE = 0x0B44;
constexpr GLenum RT_GL_CULL_FACE_MODE = 0x0B45;
constexpr GLenum RT_GL_FRONT = 0x0404;
constexpr GLenum RT_GL_BACK = 0x0405;
constexpr GLenum RT_GL_LESS = 0x0201;
constexpr GLenum RT_GL_DEPTH_FUNC = 0x0B74;
constexpr GLenum RT_GL_DEPTH_WRITEMASK = 0x0B72;
constexpr GLenum RT_GL_ZERO = 0;
constexpr GLenum RT_GL_ONE = 1;
constexpr GLenum RT_GL_SRC_ALPHA = 0x0302;
constexpr GLenum RT_GL_ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr GLenum RT_GL_BLEND_SRC_RGB = 0x80C9;
constexpr GLenum RT_GL_BLEND_DST_RGB = 0x80C8;
constexpr GLenum RT_GL_BLEND_SRC_ALPHA = 0x80CB;
constexpr GLenum RT_GL_BLEND_DST_ALPHA = 0x80CA;
constexpr GLenum RT_GL_CURRENT_PROGRAM = 0x8B8D;
constexpr GLenum RT_GL_VERTEX_ARRAY_BINDING = 0x85B5;

// Names are prefixed so they do not collide with libGL's exported functions.
extern const GLubyte* (*rt_glGetString)(GLenum name);
extern GLenum (*rt_glGetError)();
extern void (*rt_glClear)(GLbitfield mask);
extern void (*rt_glClearColor)(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
extern void (*rt_glViewport)(GLint x, GLint y, GLsizei width, GLsizei height);
extern GLuint (*rt_glCreateShader)(GLenum type);
extern void (*rt_glShaderSource)(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length);
extern void (*rt_glCompileShader)(GLuint shader);
extern void (*rt_glGetShaderiv)(GLuint shader, GLenum pname, GLint* params);
extern void (*rt_glGetShaderInfoLog)(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
extern void (*rt_glDeleteShader)(GLuint shader);
extern GLuint (*rt_glCreateProgram)();
extern void (*rt_glAttachShader)(GLuint program, GLuint shader);
extern void (*rt_glLinkProgram)(GLuint program);
extern void (*rt_glDeleteProgram)(GLuint program);
extern void (*rt_glGetProgramiv)(GLuint program, GLenum pname, GLint* params);
extern void (*rt_glValidateProgram)(GLuint program);
extern void (*rt_glGetProgramInfoLog)(GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
extern void (*rt_glUseProgram)(GLuint program);
extern void (*rt_glGenVertexArrays)(GLsizei n, GLuint* arrays);
extern void (*rt_glDeleteVertexArrays)(GLsizei n, const GLuint* arrays);
extern void (*rt_glBindVertexArray)(GLuint array);
extern void (*rt_glGenBuffers)(GLsizei n, GLuint* buffers);
extern void (*rt_glDeleteBuffers)(GLsizei n, const GLuint* buffers);
extern void (*rt_glBindBuffer)(GLenum target, GLuint buffer);
extern void (*rt_glBufferData)(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
extern void (*rt_glEnableVertexAttribArray)(GLuint index);
extern void (*rt_glVertexAttribPointer)(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer);
extern void (*rt_glDrawArrays)(GLenum mode, GLint first, GLsizei count);
extern void (*rt_glVertexAttribIPointer)(GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer);
extern void (*rt_glDrawElements)(GLenum mode, GLsizei count, GLenum type, const void* indices);
extern void (*rt_glEnable)(GLenum cap);
extern void (*rt_glDisable)(GLenum cap);
extern GLboolean (*rt_glIsEnabled)(GLenum cap);
extern void (*rt_glGetIntegerv)(GLenum pname, GLint* data);
extern void (*rt_glScissor)(GLint x, GLint y, GLsizei width, GLsizei height);
extern GLint (*rt_glGetUniformLocation)(GLuint program, const GLchar* name);
extern void (*rt_glUniform1f)(GLint location, GLfloat v0);
extern void (*rt_glUniform3f)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
extern void (*rt_glUniformMatrix4fv)(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);
extern void (*rt_glReadPixels)(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels);
extern void (*rt_glGenTextures)(GLsizei n, GLuint* textures);
extern void (*rt_glDeleteTextures)(GLsizei n, const GLuint* textures);
extern void (*rt_glBindTexture)(GLenum target, GLuint texture);
extern void (*rt_glActiveTexture)(GLenum texture);
extern void (*rt_glTexImage2D)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels);
extern void (*rt_glTexParameteri)(GLenum target, GLenum pname, GLint param);
extern void (*rt_glGenerateMipmap)(GLenum target);
extern void (*rt_glPixelStorei)(GLenum pname, GLint param);
extern void (*rt_glUniform1i)(GLint location, GLint v0);
extern void (*rt_glUniform4f)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
extern void (*rt_glUniform2f)(GLint location, GLfloat v0, GLfloat v1);
extern void (*rt_glUniform1fv)(GLint location, GLsizei count, const GLfloat* value);
extern void (*rt_glUniform3fv)(GLint location, GLsizei count, const GLfloat* value);
extern void (*rt_glUniform4fv)(GLint location, GLsizei count, const GLfloat* value);
extern void (*rt_glGenFramebuffers)(GLsizei n, GLuint* framebuffers);
extern void (*rt_glDeleteFramebuffers)(GLsizei n, const GLuint* framebuffers);
extern void (*rt_glBindFramebuffer)(GLenum target, GLuint framebuffer);
extern void (*rt_glFramebufferTexture2D)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
extern GLenum (*rt_glCheckFramebufferStatus)(GLenum target);
extern void (*rt_glDrawBuffers)(GLsizei n, const GLenum* bufs);
extern void (*rt_glBlendFunc)(GLenum sfactor, GLenum dfactor);
extern void (*rt_glBlendFuncSeparate)(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha);
extern void (*rt_glCullFace)(GLenum mode);
extern void (*rt_glDepthFunc)(GLenum func);
extern void (*rt_glDepthMask)(GLboolean flag);
extern void (*rt_glGetBooleanv)(GLenum pname, GLboolean* data);

using GlGetProcAddress = void* (*)(const char* name);

bool LoadGl(GlGetProcAddress get_proc);

}  // namespace runner

// The names stay unqualified in call sites. The pointers live in this package.
#define glGetString ::runner::rt_glGetString
#define glGetError ::runner::rt_glGetError
#define glClear ::runner::rt_glClear
#define glClearColor ::runner::rt_glClearColor
#define glViewport ::runner::rt_glViewport
#define glCreateShader ::runner::rt_glCreateShader
#define glShaderSource ::runner::rt_glShaderSource
#define glCompileShader ::runner::rt_glCompileShader
#define glGetShaderiv ::runner::rt_glGetShaderiv
#define glGetShaderInfoLog ::runner::rt_glGetShaderInfoLog
#define glDeleteShader ::runner::rt_glDeleteShader
#define glCreateProgram ::runner::rt_glCreateProgram
#define glAttachShader ::runner::rt_glAttachShader
#define glLinkProgram ::runner::rt_glLinkProgram
#define glDeleteProgram ::runner::rt_glDeleteProgram
#define glGetProgramiv ::runner::rt_glGetProgramiv
#define glValidateProgram ::runner::rt_glValidateProgram
#define glGetProgramInfoLog ::runner::rt_glGetProgramInfoLog
#define glUseProgram ::runner::rt_glUseProgram
#define glGenVertexArrays ::runner::rt_glGenVertexArrays
#define glDeleteVertexArrays ::runner::rt_glDeleteVertexArrays
#define glBindVertexArray ::runner::rt_glBindVertexArray
#define glGenBuffers ::runner::rt_glGenBuffers
#define glDeleteBuffers ::runner::rt_glDeleteBuffers
#define glBindBuffer ::runner::rt_glBindBuffer
#define glBufferData ::runner::rt_glBufferData
#define glEnableVertexAttribArray ::runner::rt_glEnableVertexAttribArray
#define glVertexAttribPointer ::runner::rt_glVertexAttribPointer
#define glDrawArrays ::runner::rt_glDrawArrays
#define glVertexAttribIPointer ::runner::rt_glVertexAttribIPointer
#define glDrawElements ::runner::rt_glDrawElements
#define glEnable ::runner::rt_glEnable
#define glDisable ::runner::rt_glDisable
#define glIsEnabled ::runner::rt_glIsEnabled
#define glGetIntegerv ::runner::rt_glGetIntegerv
#define glScissor ::runner::rt_glScissor
#define glGetUniformLocation ::runner::rt_glGetUniformLocation
#define glUniform1f ::runner::rt_glUniform1f
#define glUniform3f ::runner::rt_glUniform3f
#define glUniformMatrix4fv ::runner::rt_glUniformMatrix4fv
#define glReadPixels ::runner::rt_glReadPixels
#define glGenTextures ::runner::rt_glGenTextures
#define glDeleteTextures ::runner::rt_glDeleteTextures
#define glBindTexture ::runner::rt_glBindTexture
#define glActiveTexture ::runner::rt_glActiveTexture
#define glTexImage2D ::runner::rt_glTexImage2D
#define glTexParameteri ::runner::rt_glTexParameteri
#define glGenerateMipmap ::runner::rt_glGenerateMipmap
#define glPixelStorei ::runner::rt_glPixelStorei
#define glUniform1i ::runner::rt_glUniform1i
#define glUniform4f ::runner::rt_glUniform4f
#define glUniform2f ::runner::rt_glUniform2f
#define glUniform1fv ::runner::rt_glUniform1fv
#define glUniform3fv ::runner::rt_glUniform3fv
#define glUniform4fv ::runner::rt_glUniform4fv
#define glGenFramebuffers ::runner::rt_glGenFramebuffers
#define glDeleteFramebuffers ::runner::rt_glDeleteFramebuffers
#define glBindFramebuffer ::runner::rt_glBindFramebuffer
#define glFramebufferTexture2D ::runner::rt_glFramebufferTexture2D
#define glCheckFramebufferStatus ::runner::rt_glCheckFramebufferStatus
#define glDrawBuffers ::runner::rt_glDrawBuffers
#define glBlendFunc ::runner::rt_glBlendFunc
#define glBlendFuncSeparate ::runner::rt_glBlendFuncSeparate
#define glCullFace ::runner::rt_glCullFace
#define glDepthFunc ::runner::rt_glDepthFunc
#define glDepthMask ::runner::rt_glDepthMask
#define glGetBooleanv ::runner::rt_glGetBooleanv
