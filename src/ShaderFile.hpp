#pragma once

#include "gl.hpp"

#include <string>

// Reads a shader from shaders/ at runtime. A Mac app keeps those files in
// Contents/Resources/shaders. Other launches also accept shaders/ in the
// working directory, beside the executable, or one directory above it.
std::string LoadShader(const char* filename);

// Compiles and links a program. Returns 0 after reporting the failure.
GLuint LinkProgram(const std::string& vertexSource, const std::string& fragmentSource, const char* name);
