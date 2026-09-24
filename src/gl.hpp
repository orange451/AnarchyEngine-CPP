#pragma once

#include <cstddef>

// Windows ships OpenGL 1.1 in opengl32.dll. macOS and Linux export newer entry
// points from the driver library. Load every call this program makes after a
// context exists, so the same source links on all three.

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
#ifdef GL_BLEND
#undef GL_BLEND
#endif
#ifdef GL_SCISSOR_TEST
#undef GL_SCISSOR_TEST
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
constexpr GLenum GL_BLEND = 0x0BE2;
constexpr GLenum GL_SCISSOR_TEST = 0x0C11;

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
extern void (*rt_glDisable)(GLenum cap);
extern GLint (*rt_glGetUniformLocation)(GLuint program, const GLchar* name);
extern void (*rt_glUniform1f)(GLint location, GLfloat v0);

#define glGetString rt_glGetString
#define glGetError rt_glGetError
#define glClear rt_glClear
#define glClearColor rt_glClearColor
#define glViewport rt_glViewport
#define glCreateShader rt_glCreateShader
#define glShaderSource rt_glShaderSource
#define glCompileShader rt_glCompileShader
#define glGetShaderiv rt_glGetShaderiv
#define glGetShaderInfoLog rt_glGetShaderInfoLog
#define glDeleteShader rt_glDeleteShader
#define glCreateProgram rt_glCreateProgram
#define glAttachShader rt_glAttachShader
#define glLinkProgram rt_glLinkProgram
#define glDeleteProgram rt_glDeleteProgram
#define glGetProgramiv rt_glGetProgramiv
#define glGetProgramInfoLog rt_glGetProgramInfoLog
#define glUseProgram rt_glUseProgram
#define glGenVertexArrays rt_glGenVertexArrays
#define glDeleteVertexArrays rt_glDeleteVertexArrays
#define glBindVertexArray rt_glBindVertexArray
#define glGenBuffers rt_glGenBuffers
#define glDeleteBuffers rt_glDeleteBuffers
#define glBindBuffer rt_glBindBuffer
#define glBufferData rt_glBufferData
#define glEnableVertexAttribArray rt_glEnableVertexAttribArray
#define glVertexAttribPointer rt_glVertexAttribPointer
#define glDrawArrays rt_glDrawArrays
#define glDisable rt_glDisable
#define glGetUniformLocation rt_glGetUniformLocation
#define glUniform1f rt_glUniform1f

using GlGetProcAddress = void* (*)(const char* name);

bool LoadGl(GlGetProcAddress get_proc);
