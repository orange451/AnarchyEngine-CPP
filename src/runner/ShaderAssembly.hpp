#pragma once

#include <initializer_list>
#include <string>

namespace runner {

// A custom program's source, and where the user's lines sit in it.
struct AssembledSource {
    std::string text;
    // The 1-based line of text where the user's glsl begins, and how many lines it has.
    int user_first_line = 0;
    int user_lines = 0;
};

// main_file's #version line, "#define SURFACE_SHADER", each library, then for a
// fragment stage surface_shader.glsl and user_glsl (a ShaderParse's glsl), then
// the rest of main_file. Files are under resources/shaders, as LoadShader reads
// them. Empty text when any file cannot be read.
AssembledSource AssembleSurface(const char* main_file, std::initializer_list<const char*> libraries,
                                const std::string& user_glsl, bool fragment);

// main_file's #version line (geometry.vert or shadow.vert), "#define
// VERTEX_SHADER", vertex_shader.glsl, user_glsl (a vertex stage's ShaderParse
// glsl), then the rest of main_file. Empty text when any file cannot be read.
AssembledSource AssembleVertex(const char* main_file, const std::string& user_glsl);

// The first error in a driver's compile or link log as "<line>: <message>", the
// line counted in the user's glsl: NVIDIA's "0(12) : error C0000: msg", Mesa's
// and Intel's "0:12(3): error: msg", and Apple's and ANGLE's "ERROR: 0:12: msg".
// A line before the user's text reads 1, one after it the user's last line; a log
// whose error names no line reads 1 with its first line. Empty for an empty log.
std::string MapShaderError(const std::string& log, const AssembledSource& source);

}  // namespace runner
