#pragma once

#include "gl.hpp"

#include <initializer_list>
#include <string>

namespace runner {

// Reads a shader from resources/shaders at runtime. A Mac app keeps the
// resources tree in Contents/Resources, so these files are
// Contents/Resources/shaders. Other launches also accept resources/shaders
// in the working directory, beside the executable, or one directory above it.
std::string LoadShader(const char* filename);

// LoadShader(filename) with each library file spliced in after its #version
// line, as the legacy BaseShader linked several fragment files into one
// program. Empty when any file cannot be read.
std::string LoadShader(const char* filename, std::initializer_list<const char*> libraries);

// Whether program can draw in the state set for it now, its vertex array
// bound. macOS's OpenGL on Metal readies a program for the state it draws in
// when asked, here or at the draw, but not for render buffers made this frame:
// until the next one, a draw there fails with GL_INVALID_OPERATION, which
// stops the window. Asking raises no error. A pass asks once, before its first
// draw; it costs a few microseconds.
bool CanDraw(GLuint program);

// Compiles and links a program. Returns 0 after reporting the failure.
GLuint LinkProgram(const std::string& vertexSource, const std::string& fragmentSource, const char* name);

}  // namespace runner
