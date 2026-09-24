#pragma once

#include "gl.hpp"

#include <string>

namespace runner {

// Reads a shader from resources/shaders at runtime. A Mac app keeps the
// resources tree in Contents/Resources, so these files are
// Contents/Resources/shaders. Other launches also accept resources/shaders
// in the working directory, beside the executable, or one directory above it.
std::string LoadShader(const char* filename);

// Compiles and links a program. Returns 0 after reporting the failure.
GLuint LinkProgram(const std::string& vertexSource, const std::string& fragmentSource, const char* name);

}  // namespace runner
