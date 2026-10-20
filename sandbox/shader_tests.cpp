// Shader: GLSL source under Assets.Shaders, parsed into Kind, Error, and its
// uniforms as it changes.

#include "support.hpp"

#include "Containment.hpp"
#include "LuaApi.hpp"
#include "Shader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {

using engine_core::InstanceId;
using engine_core::Shader;
using engine_core::ShaderKind;

constexpr const char* kVignette = R"(shader post;

uniform float Strength : range(0, 1) = 0.5;

void post() {
    COLOR.rgb *= 1.0 - Strength;
}
)";

// A surface shader whose line 4 has an unknown hint.
constexpr const char* kBadHint = R"(shader surface;

// glow is not a hint
uniform float Shine : glow;

void surface() {}
)";

Shader& add_shader(engine_core::DataModel& game, const char* name) {
    Shader& shader = game.create<Shader>();
    game.set_name(shader.id(), name);
    game.set_parent(shader.id(), game.service("Shaders"));
    return shader;
}

std::vector<std::string> printed(const engine_core::ScriptRuntime::OutputBatch& batch) {
    std::vector<std::string> out;
    for (const auto& line : batch.lines) {
        out.push_back(line.text);
    }
    return out;
}

}  // namespace

TEST_CASE("SHA1 a new Shader starts with the starter source and no error", "[shader]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(game.service("Shaders") != 0);
    Shader& shader = add_shader(game, "Tint");
    REQUIRE(shader.source() == engine_core::kShaderStarterSource);
    REQUIRE(shader.source_extension() == std::string("glsl"));
    REQUIRE(shader.kind() == ShaderKind::Surface);
    REQUIRE(shader.error().empty());
    REQUIRE(shader.parse() != nullptr);
    REQUIRE(shader.parse()->params.size() == 1);
    REQUIRE(shader.parse()->params[0].name == "Tint");
}

TEST_CASE("SHA2 Source sets Kind and Error, and each fires Changed once", "[shader]") {
    SimRole role;
    engine_core::Game game;
    Shader& shader = add_shader(game, "Look");
    std::vector<std::string> changed;
    game.changed(shader.id()).connect([&](InstanceId, engine_core::Field field) {
        if (field == engine_core::Field::Source) {
            changed.push_back("Source");
            return;
        }
        const char* name = engine_core::lua_property_name(static_cast<std::uint32_t>(game.events().payload()));
        changed.push_back(name != nullptr ? name : "");
    });
    shader.set_source(kVignette);
    game.events().drain();
    REQUIRE(shader.kind() == ShaderKind::Post);
    REQUIRE(shader.error().empty());
    REQUIRE(changed == std::vector<std::string>{"Source", "Kind"});
    changed.clear();
    shader.set_source(kBadHint);
    game.events().drain();
    REQUIRE(shader.kind() == ShaderKind::Surface);
    REQUIRE(shader.error() == "4: unknown hint glow");
    REQUIRE(changed == std::vector<std::string>{"Source", "Kind", "Error"});
}

TEST_CASE("SHA3 a Shader lives in Assets.Shaders and nowhere else", "[shader]") {
    REQUIRE(!engine_core::placement_error("Shaders", "Shader", "Look"));
    REQUIRE(engine_core::placement_error("Workspace", "Shader", "Look").has_value());
    REQUIRE(engine_core::placement_error("Materials", "Shader", "Look") == "Materials holds Materials and Folders");
    REQUIRE(engine_core::placement_error("Shaders", "Material", "Brick") == "Shaders holds Shaders and Folders");
    REQUIRE(!engine_core::placement_error("Shaders", "Folder", "Lib"));
    REQUIRE(!engine_core::placement_error("Assets", "Shaders", "Shaders"));
}

TEST_CASE("SHA4 undo puts Source, Kind, and Error back", "[shader]") {
    SimRole role;
    engine_core::Game game;
    Shader& shader = add_shader(game, "Look");
    game.history().reset_waypoints();
    begin_step(game);
    shader.set_source(kBadHint);
    end_step(game);
    REQUIRE(!shader.error().empty());
    game.history().undo();
    REQUIRE(shader.source() == engine_core::kShaderStarterSource);
    REQUIRE(shader.error().empty());
    game.history().redo();
    REQUIRE(shader.error() == "4: unknown hint glow");
}

TEST_CASE("SHA5 Stop puts Source, Kind, and Error back", "[shader]") {
    SimRole role;
    engine_core::Game game;
    Shader& shader = add_shader(game, "Look");
    shader.set_source(kVignette);
    game.capture_place();
    game.start_simulation();
    shader.set_source(kBadHint);
    REQUIRE(shader.kind() == ShaderKind::Surface);
    game.stop_simulation();
    REQUIRE(shader.source() == kVignette);
    REQUIRE(shader.kind() == ShaderKind::Post);
    REQUIRE(shader.error().empty());
}

TEST_CASE("SHA6 scripts make a Shader and read its Kind and Error", "[shader][lua]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local s = Instance.new("Shader")
        s.Name = "Fx"
        s.Source = "shader post;\nvoid post() {}\n"
        s.Parent = game.Assets.Shaders
        print(s.Kind, s.Error == "", game.Assets.Shaders.Fx == s)
        s.Source = "shader post;\n"
        print(s.Error)
        print("kind", pcall(function() s.Kind = Enum.ShaderKind.Surface end))
        print("error", pcall(function() s.Error = "" end))
    )");
    const std::vector<std::string> out = printed(rig.runtime.drain_output());
    INFO(rig.runtime.last_error());
    REQUIRE(out.size() == 4);
    REQUIRE(out[0] == "Enum.ShaderKind.Post\ttrue\ttrue\n");
    REQUIRE(out[1] == "1: a post shader needs void post()\n");
    REQUIRE(out[2].rfind("kind\tfalse\t", 0) == 0);
    REQUIRE(out[3].rfind("error\tfalse\t", 0) == 0);
}
