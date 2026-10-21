// AssembleSurface and MapShaderError: a surface shader spliced into the
// engine's passes, and a driver's errors read back at the user's lines.

#include "runner/ShaderAssembly.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {

constexpr const char* kUser = "uniform vec3 Tint;\nvoid surface() {\n    ALBEDO *= Tint;\n}\n";

int line_of(const std::string& text, const std::string& needle) {
    const std::size_t at = text.find(needle);
    if (at == std::string::npos) {
        return 0;
    }
    int line = 1;
    for (std::size_t i = 0; i < at; ++i) {
        line += text[i] == '\n' ? 1 : 0;
    }
    return line;
}

}  // namespace

TEST_CASE("SA1 a surface shader is spliced after the libraries and the hook", "[shader-assembly]") {
    const runner::AssembledSource source = runner::AssembleSurface(
        "pipeline/deferred.frag", {"pipeline/gbuffer.glsl", "pipeline/surface.glsl"}, kUser, true);
    REQUIRE(!source.text.empty());
    REQUIRE(source.text.rfind("#version 330 core\n#define SURFACE_SHADER\n", 0) == 0);
    REQUIRE(source.user_lines == 4);
    REQUIRE(line_of(source.text, "uniform vec3 Tint;") == source.user_first_line);
    REQUIRE(line_of(source.text, "void surface();") < source.user_first_line);
    REQUIRE(line_of(source.text, "void runSurfaceShader(") < source.user_first_line);
    REQUIRE(line_of(source.text, "struct Surface") < line_of(source.text, "void runSurfaceShader("));
    // The main file's own code comes after, and calls the hook.
    REQUIRE(line_of(source.text, "runSurfaceShader(s,") > source.user_first_line);
}

TEST_CASE("SA2 a vertex stage gets only the define", "[shader-assembly]") {
    const runner::AssembledSource source = runner::AssembleSurface("pipeline/shadow.vert", {}, "", false);
    REQUIRE(source.text.rfind("#version 330 core\n#define SURFACE_SHADER\n", 0) == 0);
    REQUIRE(source.text.find("void surface();") == std::string::npos);
    REQUIRE(source.text.find("vWorldPosition") != std::string::npos);
}

TEST_CASE("SA3 a missing file gives no text", "[shader-assembly]") {
    REQUIRE(runner::AssembleSurface("pipeline/nope.frag", {}, kUser, true).text.empty());
}

TEST_CASE("SA4 driver errors map to the user's lines", "[shader-assembly]") {
    runner::AssembledSource source;
    source.user_first_line = 100;
    source.user_lines = 10;
    REQUIRE(runner::MapShaderError("0(103) : error C1008: undefined variable \"Tnt\"\n", source) ==
            "4: undefined variable \"Tnt\"");
    REQUIRE(runner::MapShaderError("0:102(7): error: `Tnt' undeclared\n", source) == "3: `Tnt' undeclared");
    REQUIRE(runner::MapShaderError("ERROR: 0:105: 'Tnt' : undeclared identifier\n", source) ==
            "6: 'Tnt' : undeclared identifier");
    REQUIRE(runner::MapShaderError("0(40) : error C0000: in the engine\n", source) == "1: in the engine");
    REQUIRE(runner::MapShaderError("0(400) : error C0000: after it\n", source) == "10: after it");
    REQUIRE(runner::MapShaderError("", source).empty());
    // A warning before the error is skipped; the error is reported.
    REQUIRE(runner::MapShaderError("0(101) : warning C7022: unused\n0(102) : error C0000: bad\n", source) == "3: bad");
    // A log with no line still reports at line 1.
    REQUIRE(runner::MapShaderError("Link failed: too many samplers\n", source) == "1: Link failed: too many samplers");
}

TEST_CASE("SA5 a vertex stage is spliced into geometry.vert after the define, at its own first line", "[shader-assembly]") {
    const std::string user = "void vertex() {\n    VERTEX.y += 1.0;\n}\n";
    const runner::AssembledSource source = runner::AssembleVertex("pipeline/geometry.vert", user);
    REQUIRE(!source.text.empty());
    REQUIRE(source.text.rfind("#version", 0) == 0);
    const std::size_t define = source.text.find("#define VERTEX_SHADER\n");
    REQUIRE(define != std::string::npos);
    REQUIRE(define < source.text.find("void vertex();"));
    REQUIRE(source.user_lines == 3);
    // The user's first line is where it says.
    std::size_t at = 0;
    for (int line = 1; line < source.user_first_line; ++line) {
        at = source.text.find('\n', at) + 1;
    }
    REQUIRE(source.text.compare(at, 15, "void vertex() {") == 0);
    // The engine's main follows the user's text.
    REQUIRE(source.text.find("runVertexShader(", at) != std::string::npos);
    REQUIRE(runner::AssembleVertex("pipeline/shadow.vert", user).text.find("#define VERTEX_SHADER") != std::string::npos);
}

TEST_CASE("SA6 a vertex log maps to the user's line", "[shader-assembly]") {
    const runner::AssembledSource source = runner::AssembleVertex("pipeline/geometry.vert", "void vertex() {\n    VERTEX.y += ;\n}\n");
    const std::string log = "0(" + std::to_string(source.user_first_line + 1) + ") : error C0000: syntax error\n";
    REQUIRE(runner::MapShaderError(log, source) == "2: syntax error");
}
