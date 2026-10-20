// Shader: GLSL source under Assets.Shaders, parsed into Kind, Error, and its
// uniforms as it changes.

#include "support.hpp"

#include "Containment.hpp"
#include "InstanceFile.hpp"
#include "Project.hpp"
#include "Script.hpp"
#include "LuaApi.hpp"
#include "Shader.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
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

namespace {

using engine_core::Project;
using engine_core::ProjectError;

// Where a save puts id's file when it has no children, as project_tests' leaf.
std::filesystem::path leaf(const engine_core::DataModel& game, InstanceId id, const char* ext) {
    std::string path = engine_core::sanitize_file_name(game.name(id)) + "." + game.guid(id) + ext;
    for (InstanceId up = game.parent(id); up != 0 && up != engine_core::DataModel::kNoParent; up = game.parent(up)) {
        path = engine_core::sanitize_file_name(game.name(up)) + "." + game.guid(up) + "/" + path;
    }
    return std::filesystem::path("src") / path;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string load_error(const std::filesystem::path& root) {
    try {
        Project::load(root);
    } catch (const ProjectError& error) {
        return error.what();
    }
    return std::string();
}

}  // namespace

TEST_CASE("SHA7 a Shader saves as a .glsl file beside its .meta.json", "[shader][project]") {
    SimRole role;
    TempDir dir;
    std::string guid;
    {
        engine_core::Game game;
        Project project = Project::create(dir.path, game);
        Shader& shader = add_shader(game, "Dissolve");
        shader.set_source(kVignette);
        guid = game.guid(shader.id());
        project.save();
        const std::filesystem::path glsl = dir.path / leaf(game, shader.id(), ".glsl");
        const std::filesystem::path meta = dir.path / leaf(game, shader.id(), ".meta.json");
        REQUIRE(std::filesystem::exists(glsl));
        REQUIRE(read_text(glsl) == kVignette);
        REQUIRE(std::filesystem::exists(meta));
        REQUIRE(read_text(meta).find("Source") == std::string::npos);
        REQUIRE(!std::filesystem::exists(dir.path / leaf(game, shader.id(), ".luau")));
        REQUIRE(!std::filesystem::exists(dir.path / leaf(game, shader.id(), ".json")));
    }
    engine_core::Game game;
    Project read = Project::load(dir.path, game);
    const std::optional<InstanceId> id = game.find_guid(guid);
    REQUIRE(id.has_value());
    const auto* shader = dynamic_cast<const Shader*>(game.instance(*id));
    REQUIRE(shader != nullptr);
    REQUIRE(shader->source() == kVignette);
    REQUIRE(shader->kind() == ShaderKind::Post);
}

TEST_CASE("SHA8 a renamed Shader's files follow its name", "[shader][project]") {
    SimRole role;
    TempDir dir;
    engine_core::Game game;
    Project project = Project::create(dir.path, game);
    Shader& shader = add_shader(game, "Old");
    project.save();
    const std::filesystem::path old_glsl = dir.path / leaf(game, shader.id(), ".glsl");
    const std::filesystem::path old_meta = dir.path / leaf(game, shader.id(), ".meta.json");
    REQUIRE(std::filesystem::exists(old_glsl));
    game.set_name(shader.id(), "New");
    project.save();
    REQUIRE(std::filesystem::exists(dir.path / leaf(game, shader.id(), ".glsl")));
    REQUIRE(std::filesystem::exists(dir.path / leaf(game, shader.id(), ".meta.json")));
    REQUIRE(!std::filesystem::exists(old_glsl));
    REQUIRE(!std::filesystem::exists(old_meta));
}

TEST_CASE("SHA9 a Shader in the folder form, init.glsl with init.meta.json, loads", "[shader][project]") {
    // A Shader is an asset, which holds no children, so a save never makes the
    // folder form; a hand-made one still loads, as a script's does.
    SimRole role;
    TempDir dir;
    std::string guid;
    {
        engine_core::Game game;
        Project project = Project::create(dir.path, game);
        Shader& shader = add_shader(game, "Lib");
        shader.set_source(kVignette);
        guid = game.guid(shader.id());
        project.save();
        const std::filesystem::path glsl = dir.path / leaf(game, shader.id(), ".glsl");
        const std::filesystem::path meta = dir.path / leaf(game, shader.id(), ".meta.json");
        const std::filesystem::path folder = dir.path / leaf(game, shader.id(), "");
        std::filesystem::create_directories(folder);
        std::filesystem::rename(glsl, folder / "init.glsl");
        std::filesystem::rename(meta, folder / "init.meta.json");
    }
    engine_core::Game game;
    Project read = Project::load(dir.path, game);
    const auto* shader = dynamic_cast<const Shader*>(game.instance(*game.find_guid(guid)));
    REQUIRE(shader != nullptr);
    REQUIRE(shader->source() == kVignette);
}

TEST_CASE("SHA10 a source file with the other class's extension fails the load", "[shader][project]") {
    SimRole role;
    TempDir dir;
    std::filesystem::path glsl;
    std::filesystem::path luau;
    {
        engine_core::Game game;
        Project project = Project::create(dir.path, game);
        Shader& shader = add_shader(game, "Look");
        engine_core::Script& script = add_script(game, "Main", "print(1)\n");
        project.save();
        glsl = dir.path / leaf(game, shader.id(), ".glsl");
        luau = dir.path / leaf(game, script.id(), ".luau");
    }
    std::filesystem::path renamed = glsl;
    renamed.replace_extension(".luau");
    std::filesystem::rename(glsl, renamed);
    REQUIRE(load_error(dir.path).find("a Shader keeps Source in a .glsl file") != std::string::npos);
    std::filesystem::rename(renamed, glsl);
    REQUIRE(load_error(dir.path).empty());
    std::filesystem::path script_glsl = luau;
    script_glsl.replace_extension(".glsl");
    std::filesystem::rename(luau, script_glsl);
    REQUIRE(load_error(dir.path).find("a Script keeps Source in a .luau file") != std::string::npos);
}

TEST_CASE("SHA11 a new project's .gitattributes keeps .glsl as LF text", "[shader][project]") {
    SimRole role;
    TempDir dir;
    engine_core::Game game;
    Project project = Project::create(dir.path, game);
    REQUIRE(read_text(dir.path / ".gitattributes").find("*.glsl text eol=lf") != std::string::npos);
}

TEST_CASE("SHA12 copy, paste, clone, and instance files keep a Shader's source", "[shader][copy]") {
    SimRole role;
    engine_core::Game game;
    Shader& shader = add_shader(game, "Look");
    shader.set_source(kVignette);
    const InstanceId clone = engine_core::clone_tree(game, shader.id());
    REQUIRE(clone != 0);
    REQUIRE(dynamic_cast<const Shader*>(game.instance(clone))->source() == kVignette);
    REQUIRE(dynamic_cast<const Shader*>(game.instance(clone))->kind() == ShaderKind::Post);
    const engine_core::JsonValue file = engine_core::write_instance_file({engine_core::copy_tree(game, shader.id())});
    std::vector<engine_core::CopiedNode> roots;
    std::string error;
    REQUIRE(engine_core::read_instance_file(file, roots, error));
    std::vector<InstanceId> made;
    REQUIRE(engine_core::paste_copies(game, roots, game.service("Shaders"), &made));
    REQUIRE(made.size() == 1);
    REQUIRE(dynamic_cast<const Shader*>(game.instance(made[0]))->source() == kVignette);
}
