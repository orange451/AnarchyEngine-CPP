// parse_shader: a shader's header, render_mode, groups, and uniforms with
// their hints, read from its GLSL source before any GL sees it.

#include "ShaderSource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

namespace {

using engine_core::LuaSlot;
using engine_core::parse_shader;
using engine_core::ShaderKind;
using engine_core::ShaderParamType;
using engine_core::ShaderParse;

constexpr const char* kDissolve = R"(shader surface;

uniform float Progress : range(0, 1) = 0.0;
uniform vec3 EdgeColor : color = vec3(1.0, 0.5, 0.1);
uniform sampler2D Noise : repeat;

void surface() {
    float n = texture(Noise, UV).r;
    if (n < Progress) {
        discard;
    }
    EMISSIVE += EdgeColor * 4.0 * (1.0 - smoothstep(Progress, Progress + 0.06, n));
}
)";

constexpr const char* kVignette = R"(shader post;

uniform float Strength : range(0, 1) = 0.5;
uniform float Radius : range(0, 1) = 0.75;

void post() {
    float d = length(SCREEN_UV - 0.5) / 0.7071;
    COLOR.rgb *= 1.0 - Strength * smoothstep(Radius, 1.0, d);
}
)";

std::size_t lines(const std::string& text) { return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')); }

// A surface shader holding body above its surface(), for one rule at a time.
std::string surface(const std::string& body) { return "shader surface;\n" + body + "\nvoid surface() {}\n"; }

std::string error_of(const std::string& source) {
    const ShaderParse parse = parse_shader(source);
    return parse.error ? std::to_string(parse.error->line) + ": " + parse.error->message : std::string();
}

}  // namespace

TEST_CASE("SH1 the Dissolve example parses into three uniforms", "[shader]") {
    const ShaderParse parse = parse_shader(kDissolve);
    INFO(error_of(kDissolve));
    REQUIRE(!parse.error);
    REQUIRE(parse.kind == ShaderKind::Surface);
    REQUIRE(parse.params.size() == 3);
    REQUIRE(parse.params[0].name == "Progress");
    REQUIRE(parse.params[0].type == ShaderParamType::Float);
    REQUIRE(parse.params[0].ranged);
    REQUIRE(parse.params[0].min == 0);
    REQUIRE(parse.params[0].max == 1);
    REQUIRE(parse.params[0].fallback.kind == LuaSlot::Kind::Number);
    REQUIRE(parse.params[0].fallback.number == 0);
    REQUIRE(parse.params[0].line == 3);
    REQUIRE(parse.params[1].name == "EdgeColor");
    REQUIRE(parse.params[1].type == ShaderParamType::Color);
    REQUIRE(parse.params[1].fallback.kind == LuaSlot::Kind::Color);
    REQUIRE(parse.params[1].fallback.color.g == 0.5f);
    REQUIRE(parse.params[2].name == "Noise");
    REQUIRE(parse.params[2].type == ShaderParamType::Sampler);
    REQUIRE(!parse.params[2].clamp);
    REQUIRE(parse.params[2].fallback.kind == LuaSlot::Kind::Nil);
    REQUIRE(lines(parse.glsl) == lines(kDissolve));
    REQUIRE(parse.glsl.find("uniform float Progress") != std::string::npos);
    REQUIRE(parse.glsl.find("range(") == std::string::npos);
    REQUIRE(parse.glsl.find("shader surface") == std::string::npos);
    REQUIRE(parse.glsl.find("void surface()") != std::string::npos);
}

TEST_CASE("SH2 the Vignette example is a post shader", "[shader]") {
    const ShaderParse parse = parse_shader(kVignette);
    INFO(error_of(kVignette));
    REQUIRE(!parse.error);
    REQUIRE(parse.kind == ShaderKind::Post);
    REQUIRE(parse.params.size() == 2);
    REQUIRE(parse.params[1].fallback.number == 0.75);
}

TEST_CASE("SH3 a shader starts with its header", "[shader]") {
    const std::string message = "a shader starts with `shader surface;` or `shader post;`";
    REQUIRE(error_of("void surface() {}\n") == "1: " + message);
    REQUIRE(error_of("\n\nshader fancy;\nvoid surface() {}\n") == "3: " + message);
    REQUIRE(error_of("// hello\nuniform float A;\nshader surface;\nvoid surface() {}\n") == "2: " + message);
    REQUIRE(error_of("") == "1: " + message);
}

TEST_CASE("SH4 render_mode takes the kind's words", "[shader]") {
    const ShaderParse parse = parse_shader("shader surface;\nrender_mode transparent, two_sided;\nvoid surface() {}\n");
    REQUIRE(!parse.error);
    REQUIRE(parse.header_modes == std::vector<std::string>{"transparent", "two_sided"});
    REQUIRE(error_of("shader surface;\nrender_mode hdr;\nvoid surface() {}\n") ==
            "2: unknown render_mode hdr; a surface shader has transparent, two_sided, no_shadow");
    REQUIRE(error_of("shader post;\nrender_mode bogus;\nvoid post() {}\n") ==
            "2: unknown render_mode bogus; a post shader has hdr");
    REQUIRE(!parse_shader("shader post;\nrender_mode hdr;\nvoid post() {}\n").error);
}

TEST_CASE("SH5 every refusal says why, on its line", "[shader]") {
    SECTION("types") {
        for (const char* type : {"vec4", "mat3", "mat4", "sampler3D", "samplerCube", "double"}) {
            INFO(type);
            REQUIRE(error_of(surface(std::string("uniform ") + type + " X;")) ==
                    "2: a uniform is float, int, bool, vec2, vec3, or sampler2D");
        }
    }
    SECTION("array") { REQUIRE(error_of(surface("uniform float X[4];")) == "2: a uniform cannot be an array"); }
    SECTION("block") {
        REQUIRE(error_of(surface("uniform Block { float x; };")) == "2: uniform blocks are not supported");
    }
    SECTION("reserved") {
        REQUIRE(error_of(surface("uniform float ALBEDO;")) == "2: ALBEDO is the engine's name");
        REQUIRE(error_of(surface("uniform float uColor;")) == "2: uColor is the engine's name");
        REQUIRE(error_of(surface("uniform float gl_Thing;")) == "2: gl_Thing is the engine's name");
        REQUIRE(!parse_shader(surface("uniform float under;")).error);
    }
    SECTION("twice") {
        REQUIRE(error_of(surface("uniform float A;\nuniform vec3 A;")) == "3: A is declared twice");
    }
    SECTION("hints") {
        REQUIRE(error_of(surface("uniform float A : glow;")) == "2: unknown hint glow");
        REQUIRE(error_of(surface("uniform bool A : range(0, 1);")) == "2: range is for float and int");
        REQUIRE(error_of(surface("uniform float A : color;")) == "2: color is for vec3 and sampler2D");
        REQUIRE(error_of(surface("uniform float A : repeat;")) == "2: repeat is for sampler2D");
        REQUIRE(error_of(surface("uniform vec3 A : nearest;")) == "2: nearest is for sampler2D");
        REQUIRE(error_of(surface("uniform float A : range(1, 1);")) ==
                "2: range takes two numbers, the first below the second");
        REQUIRE(error_of(surface("uniform float A : range(1);")) ==
                "2: range takes two numbers, the first below the second");
    }
    SECTION("defaults") {
        REQUIRE(error_of(surface("uniform float A = true;")) == "2: the default must be a float");
        REQUIRE(error_of(surface("uniform vec3 A = vec2(1.0, 2.0);")) == "2: the default must be a vec3");
        REQUIRE(error_of(surface("uniform bool A = 1;")) == "2: the default must be a bool");
        REQUIRE(error_of(surface("uniform sampler2D A = 1;")) == "2: a sampler2D has no default; use white or black");
    }
    SECTION("functions") {
        REQUIRE(error_of("shader surface;\nuniform float A;\n") == "2: a surface shader needs void surface()");
        REQUIRE(error_of("shader post;\nvoid surface() {}\n") == "2: a post shader needs void post()");
    }
}

TEST_CASE("SH6 group names the uniforms after it", "[shader]") {
    const ShaderParse parse =
        parse_shader(surface("uniform float A;\ngroup Edge;\nuniform float B;\nuniform float C;\ngroup;\nuniform float D;"));
    REQUIRE(!parse.error);
    REQUIRE(parse.params.size() == 4);
    REQUIRE(parse.params[0].group.empty());
    REQUIRE(parse.params[1].group == "Edge");
    REQUIRE(parse.params[2].group == "Edge");
    REQUIRE(parse.params[3].group.empty());
}

TEST_CASE("SH7 defaults take their type's shape", "[shader]") {
    const ShaderParse parse = parse_shader(surface(
        "uniform int Steps = 2.9;\nuniform vec3 Gray = vec3(0.5);\nuniform bool On = true;\nuniform vec3 Tint : color;\n"
        "uniform float Zero;\nuniform vec2 Flat = vec2(1.0, 2.0);\nuniform sampler2D Mask : white, clamp, nearest, hidden;"));
    INFO((parse.error ? parse.error->message : std::string()));
    REQUIRE(!parse.error);
    REQUIRE(parse.params[0].type == ShaderParamType::Int);
    REQUIRE(parse.params[0].fallback.number == 2);
    REQUIRE(parse.params[1].fallback.kind == LuaSlot::Kind::Vec3);
    REQUIRE(parse.params[1].fallback.vec.z == 0.5f);
    REQUIRE(parse.params[2].fallback.kind == LuaSlot::Kind::Bool);
    REQUIRE(parse.params[2].fallback.flag);
    REQUIRE(parse.params[3].fallback.kind == LuaSlot::Kind::Color);
    REQUIRE(parse.params[3].fallback.color.r == 1.f);
    REQUIRE(parse.params[3].fallback.color.b == 1.f);
    REQUIRE(parse.params[4].fallback.number == 0);
    REQUIRE(parse.params[5].fallback.kind == LuaSlot::Kind::Vec2);
    REQUIRE(parse.params[5].fallback.vec.y == 2.f);
    REQUIRE(parse.params[6].clamp);
    REQUIRE(parse.params[6].nearest);
    REQUIRE(parse.params[6].hidden);
    REQUIRE(!parse.params[6].black);
}

TEST_CASE("SH8 comments are skipped, preprocessor branches are not evaluated", "[shader]") {
    const ShaderParse parse = parse_shader(
        surface("/* uniform vec4 Bad; */\n// uniform vec4 Worse;\n#if 0\nuniform float Y;\n#endif\nuniform float Z; // trailing"));
    INFO((parse.error ? parse.error->message : std::string()));
    REQUIRE(!parse.error);
    REQUIRE(parse.params.size() == 2);
    REQUIRE(parse.params[0].name == "Y");
    REQUIRE(parse.params[1].name == "Z");
    REQUIRE(parse.params[1].line == 7);
    // A uniform inside a function body is the body's business, not a parameter.
    REQUIRE(parse_shader("shader surface;\nvoid surface() { float uniformish = 1.0; }\n").params.empty());
}

TEST_CASE("SH9 a shader holds at most 64 uniforms and a few samplers", "[shader]") {
    std::string many;
    for (int i = 0; i < 64; ++i) {
        many += "uniform float P" + std::to_string(i) + ";\n";
    }
    REQUIRE(!parse_shader(surface(many)).error);
    REQUIRE(error_of(surface(many + "uniform float Extra;")) == "66: a shader has at most 64 uniforms");
    const std::string three = "uniform sampler2D A;\nuniform sampler2D B;\nuniform sampler2D C;\n";
    REQUIRE(!parse_shader(surface(three)).error);
    REQUIRE(error_of(surface(three + "uniform sampler2D D;")) == "5: a surface shader has at most 3 sampler2D");
    const std::string four = "shader post;\n" + three + "uniform sampler2D D;\n";
    REQUIRE(!parse_shader(four + "void post() {}\n").error);
    REQUIRE(error_of(four + "uniform sampler2D E;\nvoid post() {}\n") == "6: a post shader has at most 4 sampler2D");
}

TEST_CASE("SH10 an error reads as its line and message", "[shader]") {
    REQUIRE(engine_core::shader_error_text({9, "x"}) == "9: x");
    REQUIRE(engine_core::shader_reserved_name("TIME"));
    REQUIRE(!engine_core::shader_reserved_name("Time"));
}

TEST_CASE("SH11 a continued directive and one after a comment are skipped whole", "[shader]") {
    const ShaderParse continued =
        parse_shader(surface("#define SAT(x) \\\n    clamp(x, 0.0, 1.0)\nuniform float A : range(0, 1);"));
    INFO((continued.error ? continued.error->message : std::string()));
    REQUIRE(!continued.error);
    REQUIRE(continued.params.size() == 1);
    REQUIRE(continued.params[0].name == "A");
    REQUIRE(continued.params[0].line == 4);
    REQUIRE(continued.glsl.find("range(") == std::string::npos);
    const ShaderParse commented = parse_shader(surface("/* c */ #define X 1\nuniform float B;"));
    INFO((commented.error ? commented.error->message : std::string()));
    REQUIRE(!commented.error);
    REQUIRE(commented.params.size() == 1);
    REQUIRE(commented.params[0].name == "B");
}

TEST_CASE("SH-T the parse says whether the code names TIME and cuts holes, comments aside", "[shader]") {
    const ShaderParse plain = parse_shader("shader surface;\n// TIME, discard, and ALPHA in a comment\n/* discard */\nvoid surface() { ALBEDO *= 0.5; }\n");
    REQUIRE_FALSE(plain.reads_time);
    REQUIRE_FALSE(plain.cuts);
    const ShaderParse timed = parse_shader("shader surface;\nvoid surface() { ALBEDO *= sin(TIME); }\n");
    REQUIRE(timed.reads_time);
    REQUIRE_FALSE(timed.cuts);
    const ShaderParse cut = parse_shader("shader surface;\nvoid surface() { if (UV.x < 0.5) discard; }\n");
    REQUIRE(cut.cuts);
    const ShaderParse alpha = parse_shader("shader surface;\nvoid surface() { ALPHA = 0.5; }\n");
    REQUIRE(alpha.cuts);
    const ShaderParse near = parse_shader("shader surface;\nfloat ALPHAS = 1.0;\nfloat MYTIME = 2.0;\nvoid surface() {}\n");
    REQUIRE_FALSE(near.cuts);
    REQUIRE_FALSE(near.reads_time);
}

TEST_CASE("SH12 under a given kind the header is optional and, present, is recorded and blanked", "[shader]") {
    using engine_core::ShaderStage;
    const ShaderParse bare = parse_shader("uniform float A = 1.0;\nvoid surface() {}\n", ShaderStage::Fragment, ShaderKind::Surface);
    REQUIRE(!bare.error);
    REQUIRE(bare.kind == ShaderKind::Surface);
    REQUIRE(!bare.header_kind.has_value());
    REQUIRE(!bare.header_modes.has_value());
    REQUIRE(bare.params.size() == 1);
    const ShaderParse headed = parse_shader("shader post;\nvoid post() {}\n", ShaderStage::Fragment, ShaderKind::Surface);
    REQUIRE(!headed.error);
    REQUIRE(headed.header_kind == ShaderKind::Post);
    REQUIRE(headed.kind == ShaderKind::Post);
    REQUIRE(headed.glsl.find("shader post") == std::string::npos);
    // The nullopt overload keeps phase 1's rule.
    REQUIRE(error_of("void surface() {}\n").find("starts with") != std::string::npos);
}

TEST_CASE("SH13 render_mode is recorded as the header modes, an empty one too", "[shader]") {
    using engine_core::ShaderStage;
    const ShaderParse modes = parse_shader("render_mode two_sided;\nvoid surface() {}\n", ShaderStage::Fragment, ShaderKind::Surface);
    REQUIRE(!modes.error);
    REQUIRE(modes.header_modes.has_value());
    REQUIRE(*modes.header_modes == std::vector<std::string>{"two_sided"});
    const ShaderParse none = parse_shader("render_mode;\nvoid surface() {}\n", ShaderStage::Fragment, ShaderKind::Surface);
    REQUIRE(!none.error);
    REQUIRE(none.header_modes.has_value());
    REQUIRE(none.header_modes->empty());
}

TEST_CASE("SH14 a vertex stage needs vertex() and takes no render_mode", "[shader]") {
    using engine_core::ShaderStage;
    const ShaderParse missing = parse_shader("uniform float A;\n", ShaderStage::Vertex, ShaderKind::Surface);
    REQUIRE(missing.error);
    REQUIRE(missing.error->line == 1);
    REQUIRE(missing.error->message == "a vertex stage needs void vertex()");
    REQUIRE(missing.stage == ShaderStage::Vertex);
    const ShaderParse good = parse_shader("uniform float A;\nvoid vertex() {\n    VERTEX.y += A * sin(TIME);\n}\n",
                                          ShaderStage::Vertex, ShaderKind::Surface);
    REQUIRE(!good.error);
    REQUIRE(good.reads_time);
    REQUIRE(!good.cuts);
    const ShaderParse mode = parse_shader("render_mode two_sided;\nvoid vertex() {}\n", ShaderStage::Vertex, ShaderKind::Surface);
    REQUIRE(mode.error);
    REQUIRE(mode.error->message == "render_mode belongs in the fragment stage");
}

TEST_CASE("SH15 VERTEX is the engine's name", "[shader]") {
    REQUIRE(error_of(surface("uniform float VERTEX;")) == "2: VERTEX is the engine's name");
    REQUIRE(engine_core::shader_reserved_name("VERTEX"));
}

TEST_CASE("SH16 the two stages' uniforms merge, and a mismatch errors in both", "[shader]") {
    using engine_core::merge_shader_params;
    using engine_core::ShaderStage;
    const ShaderParse fragment = parse_shader("uniform float A = 1.0;\nuniform vec3 Tint : color;\nvoid surface() {}\n",
                                              ShaderStage::Fragment, ShaderKind::Surface);
    const ShaderParse same = parse_shader("uniform float A = 1.0;\nuniform float B;\nvoid vertex() {}\n",
                                          ShaderStage::Vertex, ShaderKind::Surface);
    const engine_core::MergedParams merged = merge_shader_params(fragment, &same);
    REQUIRE(!merged.fragment_error);
    REQUIRE(!merged.vertex_error);
    REQUIRE(merged.params.size() == 3);
    REQUIRE(merged.params[0].name == "A");
    REQUIRE(merged.params[1].name == "Tint");
    REQUIRE(merged.params[2].name == "B");
    const ShaderParse other = parse_shader("void vertex() {}\nuniform float A = 2.0;\n", ShaderStage::Vertex, ShaderKind::Surface);
    const engine_core::MergedParams clash = merge_shader_params(fragment, &other);
    REQUIRE(clash.fragment_error);
    REQUIRE(clash.fragment_error->line == 1);
    REQUIRE(clash.fragment_error->message == "A is declared differently in the vertex stage");
    REQUIRE(clash.vertex_error);
    REQUIRE(clash.vertex_error->line == 2);
    REQUIRE(clash.vertex_error->message == "A is declared differently in the fragment stage");
    // No vertex stage: the fragment's list as it is.
    REQUIRE(merge_shader_params(fragment, nullptr).params.size() == 2);
}

TEST_CASE("SH40 the post stage's function names are the engine's, and hdr is a post mode", "[shader]") {
    using engine_core::ShaderStage;
    REQUIRE(engine_core::shader_reserved_name("depthAt"));
    REQUIRE(engine_core::shader_reserved_name("viewPositionAt"));
    REQUIRE(engine_core::shader_reserved_name("worldPositionAt"));
    REQUIRE(engine_core::shader_reserved_name("normalAt"));
    REQUIRE(engine_core::shader_reserved_name("SCREEN"));
    REQUIRE(error_of(surface("uniform float depthAt;")) == "2: depthAt is the engine's name");
    const ShaderParse hdr = parse_shader("render_mode hdr;\nvoid post() {}\n", ShaderStage::Fragment, ShaderKind::Post);
    REQUIRE(!hdr.error);
    REQUIRE(hdr.header_modes == std::vector<std::string>{"hdr"});
    const ShaderParse wrong = parse_shader("render_mode hdr;\nvoid surface() {}\n", ShaderStage::Fragment, ShaderKind::Surface);
    REQUIRE(wrong.error);
    REQUIRE(wrong.error->message == "unknown render_mode hdr; a surface shader has transparent, two_sided, no_shadow");
}
