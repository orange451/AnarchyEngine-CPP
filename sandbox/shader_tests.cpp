// Shader: GLSL source under Assets.Shaders, parsed into Kind, Error, and its
// uniforms as it changes.

#include "support.hpp"

#include "AssetInstances.hpp"
#include "Containment.hpp"
#include "InstanceFile.hpp"
#include "Project.hpp"
#include "Script.hpp"
#include "LuaApi.hpp"
#include "Shader.hpp"
#include "SnapshotPump.hpp"
#include "SurfaceBinding.hpp"
#include "GameObject.hpp"
#include "Folder.hpp"

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

// text as a Shader stores it: its `shader X;` first line turned to spaces.
std::string stored(const char* text) {
    std::string out = text;
    const std::size_t end = out.find(';');
    if (out.rfind("shader ", 0) == 0 && end != std::string::npos) {
        out.replace(0, end + 1, std::string(end + 1, ' '));
    }
    return out;
}

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
    REQUIRE(changed == std::vector<std::string>{"Kind", "Source", "FragmentSource"});
    changed.clear();
    shader.set_source(kBadHint);
    game.events().drain();
    REQUIRE(shader.kind() == ShaderKind::Surface);
    REQUIRE(shader.error() == "fragment 4: unknown hint glow");
    REQUIRE(changed == std::vector<std::string>{"Kind", "Source", "FragmentSource", "Error", "FragmentError"});
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
    REQUIRE(shader.error() == "fragment 4: unknown hint glow");
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
    REQUIRE(shader.source() == stored(kVignette));
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
    REQUIRE(out[1] == "fragment 1: a post shader needs void post()\n");
    REQUIRE(out[2] == "kind\ttrue\n");
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
        REQUIRE(read_text(glsl) == stored(kVignette));
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
    REQUIRE(shader->source() == stored(kVignette));
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
    REQUIRE(shader->source() == stored(kVignette));
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
    REQUIRE(dynamic_cast<const Shader*>(game.instance(clone))->source() == stored(kVignette));
    REQUIRE(dynamic_cast<const Shader*>(game.instance(clone))->kind() == ShaderKind::Post);
    const engine_core::JsonValue file = engine_core::write_instance_file({engine_core::copy_tree(game, shader.id())});
    std::vector<engine_core::CopiedNode> roots;
    std::string error;
    REQUIRE(engine_core::read_instance_file(file, roots, error));
    std::vector<InstanceId> made;
    REQUIRE(engine_core::paste_copies(game, roots, game.service("Shaders"), &made));
    REQUIRE(made.size() == 1);
    REQUIRE(dynamic_cast<const Shader*>(game.instance(made[0]))->source() == stored(kVignette));
}

TEST_CASE("SHA13 a Material takes a surface Shader and refuses a post one", "[shader][lua]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local look = Instance.new("Shader")
        look.Name = "Look"
        look.Parent = game.Assets.Shaders
        local fx = Instance.new("Shader")
        fx.Name = "Fx"
        fx.Source = "shader post;\nvoid post() {}\n"
        fx.Parent = game.Assets.Shaders
        local m = Instance.new("Material")
        m.Name = "Brick"
        m.Parent = game.Assets.Materials
        m.Shader = look
        print("set", m.Shader == look)
        print("post", pcall(function() m.Shader = fx end))
        print("kept", m.Shader == look)
        m.Shader = nil
        print("cleared", m.Shader == nil)
    )");
    const std::vector<std::string> out = printed(rig.runtime.drain_output());
    INFO(rig.runtime.last_error());
    REQUIRE(out.size() == 4);
    REQUIRE(out[0] == "set\ttrue\n");
    REQUIRE(out[1].rfind("post\tfalse\t", 0) == 0);
    REQUIRE(out[1].find("Fx is a post shader; a Material takes a surface shader") != std::string::npos);
    REQUIRE(out[2] == "kept\ttrue\n");
    REQUIRE(out[3] == "cleared\ttrue\n");
}

TEST_CASE("SHA14 Material::shader is the live surface Shader, or null", "[shader]") {
    SimRole role;
    engine_core::Game game;
    Shader& look = add_shader(game, "Look");
    engine_core::Material& brick = game.create<engine_core::Material>();
    game.set_parent(brick.id(), game.service("Materials"));
    REQUIRE(brick.shader(game) == nullptr);
    engine_core::LuaSlot ref;
    ref.kind = engine_core::LuaSlot::Kind::Instance;
    ref.id = look.id();
    REQUIRE(!brick.set_reference(engine_core::Material::kShaderReference, ref));
    REQUIRE(brick.shader(game) == &look);
    // A post shader named by a file (a GUID, unchecked) is not a surface shader.
    look.set_source(kVignette);
    REQUIRE(brick.shader(game) == nullptr);
    look.set_source(std::string("shader surface;\n") + engine_core::kShaderStarterSource);
    REQUIRE(brick.shader(game) == &look);
    game.destroy(look.id());
    REQUIRE(brick.shader(game) == nullptr);
}

TEST_CASE("SHA15 a Material's Shader saves and loads by GUID", "[shader][project]") {
    SimRole role;
    TempDir dir;
    std::string material_guid;
    std::string shader_guid;
    {
        engine_core::Game game;
        Project project = Project::create(dir.path, game);
        Shader& look = add_shader(game, "Look");
        engine_core::Material& brick = game.create<engine_core::Material>();
        game.set_name(brick.id(), "Brick");
        game.set_parent(brick.id(), game.service("Materials"));
        engine_core::LuaSlot ref;
        ref.kind = engine_core::LuaSlot::Kind::Instance;
        ref.id = look.id();
        REQUIRE(!brick.set_reference(engine_core::Material::kShaderReference, ref));
        material_guid = game.guid(brick.id());
        shader_guid = game.guid(look.id());
        project.save();
    }
    engine_core::Game game;
    Project read = Project::load(dir.path, game);
    const auto* brick = dynamic_cast<const engine_core::Material*>(game.instance(*game.find_guid(material_guid)));
    REQUIRE(brick != nullptr);
    REQUIRE(brick->shader(game) != nullptr);
    REQUIRE(game.guid(brick->shader(game)->id()) == shader_guid);
}

namespace {

// Progress has no default, so an unset or mistyped one reads 0.
constexpr const char* kDissolve = R"(shader surface;

uniform float Progress : range(0, 1);
uniform vec3 EdgeColor : color = vec3(1.0, 0.5, 0.0);
uniform sampler2D Noise;

void surface() {
    if (texture(Noise, UV).r < Progress) {
        discard;
    }
    EMISSIVE = EdgeColor;
}
)";

engine_core::LuaSlot instance_ref(InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

engine_core::LuaSlot number_slot(double value) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Number;
    slot.number = value;
    return slot;
}

engine_core::Material& add_material(engine_core::DataModel& game, const char* name, InstanceId shader) {
    engine_core::Material& made = game.create<engine_core::Material>();
    game.set_name(made.id(), name);
    game.set_parent(made.id(), game.service("Materials"));
    if (shader != 0) {
        REQUIRE_FALSE(made.set_reference(engine_core::Material::kShaderReference, instance_ref(shader)));
    }
    return made;
}

}  // namespace

TEST_CASE("SHA16 a surface binding holds each uniform's value, held to its type and range", "[shader]") {
    SimRole role;
    engine_core::Game game;
    Shader& dissolve = add_shader(game, "Dissolve");
    dissolve.set_source(kDissolve);
    engine_core::Material& brick = add_material(game, "Brick", dissolve.id());
    REQUIRE_FALSE(brick.set_attribute("Progress", number_slot(0.3)));

    auto binding = engine_core::make_surface_binding(game, brick);
    REQUIRE(binding != nullptr);
    REQUIRE(binding->shader == dissolve.id());
    REQUIRE(binding->version == dissolve.source_version());
    const auto values = binding->values();
    REQUIRE(values->slots.size() == 3);
    REQUIRE(values->slots[0].kind == engine_core::LuaSlot::Kind::Number);
    REQUIRE(values->slots[0].number == 0.3);
    REQUIRE(values->slots[1].kind == engine_core::LuaSlot::Kind::Color);
    REQUIRE(values->slots[1].color.g == 0.5f);
    REQUIRE(values->slots[2].kind == engine_core::LuaSlot::Kind::Nil);
    REQUIRE(binding->samplers.size() == 1);
    REQUIRE(binding->samplers[0].path.empty());
    REQUIRE_FALSE(binding->reads_time);

    // A string is not a float: the default, 0.
    engine_core::LuaSlot text;
    text.kind = engine_core::LuaSlot::Kind::String;
    text.text = "half";
    REQUIRE_FALSE(brick.set_attribute("Progress", text));
    REQUIRE(engine_core::make_surface_binding(game, brick)->values()->slots[0].number == 0.0);

    // Out of range: held to 1.
    REQUIRE_FALSE(brick.set_attribute("Progress", number_slot(5)));
    REQUIRE(engine_core::make_surface_binding(game, brick)->values()->slots[0].number == 1.0);

    // A Texture attribute is the sampler's path.
    engine_core::Texture& noise = game.create<engine_core::Texture>();
    game.set_parent(noise.id(), game.service("Textures"));
    REQUIRE_FALSE(noise.set_path("textures/noise.png"));
    REQUIRE_FALSE(brick.set_attribute("Noise", instance_ref(noise.id())));
    REQUIRE(engine_core::make_surface_binding(game, brick)->samplers[0].path == "textures/noise.png");

    // No Shader, no binding.
    engine_core::Material& plain = add_material(game, "Plain", 0);
    REQUIRE(engine_core::make_surface_binding(game, plain) == nullptr);
}

TEST_CASE("SHA17 two Materials on one Shader bind their own values", "[shader]") {
    SimRole role;
    engine_core::Game game;
    Shader& dissolve = add_shader(game, "Dissolve");
    dissolve.set_source(kDissolve);
    engine_core::Material& a = add_material(game, "A", dissolve.id());
    engine_core::Material& b = add_material(game, "B", dissolve.id());
    REQUIRE_FALSE(a.set_attribute("Progress", number_slot(0.25)));
    REQUIRE_FALSE(b.set_attribute("Progress", number_slot(0.75)));
    auto first = engine_core::make_surface_binding(game, a);
    auto second = engine_core::make_surface_binding(game, b);
    REQUIRE(first->values()->slots[0].number == 0.25);
    REQUIRE(second->values()->slots[0].number == 0.75);
    REQUIRE(first->values_hash() != second->values_hash());

    // A shader that names TIME redraws its shadows each frame.
    Shader& wave = add_shader(game, "Wave");
    wave.set_source("shader surface;\nvoid surface() { ALBEDO *= 0.5 + 0.5 * sin(TIME); }\n");
    engine_core::Material& water = add_material(game, "Water", wave.id());
    REQUIRE(engine_core::make_surface_binding(game, water)->reads_time);
}

TEST_CASE("SHA18 the snapshot shares one binding per Material across its Models", "[shader][render]") {
    SimRole role;
    engine_core::Game game;
    engine_core::SnapshotPump pump;
    pump.reserve(engine_core::DataModel::kMaxInstances);
    const auto frame = [&] {
        pump.prepare_copy(game);
        pump.publish();
    };
    Shader& dissolve = add_shader(game, "Dissolve");
    dissolve.set_source(kDissolve);
    engine_core::Material& brick = add_material(game, "Brick", dissolve.id());
    engine_core::Material& plain = add_material(game, "Plain", 0);
    engine_core::Mesh& body = game.create<engine_core::Mesh>();
    game.set_parent(body.id(), game.service("Meshes"));
    REQUIRE_FALSE(body.set_path("meshes/body.amesh"));
    engine_core::Prefab& crate = game.create<engine_core::Prefab>();
    game.set_parent(crate.id(), game.service("Prefabs"));
    for (InstanceId look : {brick.id(), brick.id(), plain.id()}) {
        engine_core::Model& part = game.create<engine_core::Model>();
        game.set_parent(part.id(), crate.id());
        REQUIRE_FALSE(part.set_reference(engine_core::Model::kMeshReference, instance_ref(body.id())));
        REQUIRE_FALSE(part.set_reference(engine_core::Model::kMaterialReference, instance_ref(look)));
    }
    engine_core::GameObject& box = game.create_game_object();
    game.set_parent(box.id(), workspace_of(game));
    REQUIRE_FALSE(box.set_prefab(instance_ref(crate.id())));
    const auto meshes = [&]() -> const std::vector<engine_core::VisualMesh>& {
        const engine_core::VisualInstance* row = pump.find(box.id());
        REQUIRE(row != nullptr);
        const auto& list = pump.front().prefabs[row->prefab].meshes;
        REQUIRE(list.size() == 3);
        return list;
    };

    frame();
    REQUIRE(meshes()[0].surface != nullptr);
    REQUIRE(meshes()[0].surface == meshes()[1].surface);
    REQUIRE(meshes()[2].surface == nullptr);

    // Unchanged, the next frame keeps the same binding; so does a changed
    // value, which the binding's values carry, so no draw of the Material changes.
    const auto before = meshes()[0].surface;
    const std::uint64_t hash = before->values_hash();
    frame();
    REQUIRE(meshes()[0].surface == before);
    REQUIRE_FALSE(brick.set_attribute("Progress", number_slot(0.5)));
    frame();
    REQUIRE(meshes()[0].surface == before);
    REQUIRE(meshes()[0].surface->values()->slots[0].number == 0.5);
    REQUIRE(meshes()[0].surface->values_hash() != hash);
    REQUIRE(meshes()[0].surface == meshes()[1].surface);
    // A Texture on a sampler is a new binding: the draws' textures change.
    engine_core::Texture& noise = game.create<engine_core::Texture>();
    game.set_parent(noise.id(), game.service("Textures"));
    REQUIRE_FALSE(noise.set_path("textures/noise.png"));
    REQUIRE_FALSE(brick.set_attribute("Noise", instance_ref(noise.id())));
    frame();
    REQUIRE(meshes()[0].surface != before);
    REQUIRE(meshes()[0].surface->samplers[0].path == "textures/noise.png");

    // A source edit is a new version.
    dissolve.set_source(std::string(kDissolve) + "\n");
    frame();
    REQUIRE(meshes()[0].surface->version == dissolve.source_version());
}

TEST_CASE("SHA19 a compile error shows on Error for its version only", "[shader]") {
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
    const std::uint64_t version = shader.source_version();
    shader.set_compile_error(version, engine_core::ShaderStage::Fragment, "3: 'wobble' : no matching overloaded function found");
    game.events().drain();
    REQUIRE(shader.error() == "fragment 3: 'wobble' : no matching overloaded function found");
    REQUIRE(shader.fragment_error() == "3: 'wobble' : no matching overloaded function found");
    REQUIRE(changed == std::vector<std::string>{"Error", "FragmentError"});
    // The same again changes nothing.
    changed.clear();
    shader.set_compile_error(version, engine_core::ShaderStage::Fragment, "3: 'wobble' : no matching overloaded function found");
    game.events().drain();
    REQUIRE(changed.empty());

    // A new Source clears it; a late error for the old version is dropped.
    shader.set_source(std::string(engine_core::kShaderStarterSource) + "\n");
    game.events().drain();
    REQUIRE(shader.error().empty());
    changed.clear();
    shader.set_compile_error(version, engine_core::ShaderStage::Fragment, "3: stale");
    game.events().drain();
    REQUIRE(shader.error().empty());
    REQUIRE(changed.empty());

    // A parse error wins over a compile error.
    shader.set_compile_error(shader.source_version(), engine_core::ShaderStage::Fragment, "5: late");
    shader.set_source(kBadHint);
    shader.set_compile_error(shader.source_version(), engine_core::ShaderStage::Fragment, "2: driver");
    REQUIRE(shader.error() == "fragment 4: unknown hint glow");
}

namespace {

constexpr const char* kLift = "void vertex() {\n    VERTEX.y += 1.0;\n}\n";

}  // namespace

TEST_CASE("SHA20 a Shader's vertex text is a second stage with its own version and Changed", "[shader]") {
    SimRole role;
    engine_core::Game game;
    Shader& shader = add_shader(game, "Look");
    REQUIRE(shader.stage_count() == 2);
    REQUIRE(std::string(shader.source_extension(Shader::kFragmentStage)) == "glsl");
    REQUIRE(std::string(shader.source_extension(Shader::kVertexStage)) == "vert.glsl");
    REQUIRE(!shader.has_vertex_stage());
    std::vector<engine_core::Field> fields;
    game.changed(shader.id()).connect([&](InstanceId, engine_core::Field field) { fields.push_back(field); });
    const std::uint64_t version = shader.source_version();
    game.history().reset_waypoints();
    begin_step(game);
    shader.set_vertex_source(kLift);
    end_step(game);
    game.events().drain();
    REQUIRE(shader.vertex_source() == kLift);
    REQUIRE(shader.source(Shader::kVertexStage) == kLift);
    REQUIRE(shader.has_vertex_stage());
    REQUIRE(shader.source_version() > version);
    REQUIRE(fields == std::vector<engine_core::Field>{engine_core::Field::VertexSource});
    // Undo puts it back, and redo brings it again.
    game.history().undo();
    REQUIRE(!shader.has_vertex_stage());
    game.history().redo();
    REQUIRE(shader.vertex_source() == kLift);
    // A Script keeps one stage.
    engine_core::Script& script = game.create<engine_core::Script>();
    REQUIRE(script.stage_count() == 1);
}

TEST_CASE("SHA21 the vertex stage saves as <Name>.<guid>.vert.glsl beside the .glsl, only when it has text", "[shader][project]") {
    SimRole role;
    TempDir dir;
    std::string guid;
    {
        engine_core::Game game;
        Project project = Project::create(dir.path, game);
        Shader& shader = add_shader(game, "Ocean");
        shader.set_source(kVignette);
        guid = game.guid(shader.id());
        project.save();
        REQUIRE(!std::filesystem::exists(dir.path / leaf(game, shader.id(), ".vert.glsl")));
        shader.set_vertex_source(kLift);
        project.save();
        const std::filesystem::path vert = dir.path / leaf(game, shader.id(), ".vert.glsl");
        REQUIRE(std::filesystem::exists(vert));
        REQUIRE(read_text(vert) == kLift);
        REQUIRE(read_text(dir.path / leaf(game, shader.id(), ".glsl")) == stored(kVignette));
        // A rename moves both.
        game.set_name(shader.id(), "Sea");
        project.save();
        REQUIRE(!std::filesystem::exists(vert));
        REQUIRE(std::filesystem::exists(dir.path / leaf(game, shader.id(), ".vert.glsl")));
    }
    {
        engine_core::Game game;
        Project read = Project::load(dir.path, game);
        auto* shader = dynamic_cast<Shader*>(game.instance(*game.find_guid(guid)));
        REQUIRE(shader != nullptr);
        REQUIRE(shader->source() == stored(kVignette));
        REQUIRE(shader->vertex_source() == kLift);
        // Emptied, the file goes.
        shader->set_vertex_source("");
        read.save();
        REQUIRE(!std::filesystem::exists(dir.path / leaf(game, shader->id(), ".vert.glsl")));
        REQUIRE(std::filesystem::exists(dir.path / leaf(game, shader->id(), ".glsl")));
    }
}

TEST_CASE("SHA22 a phase 1 project, leaf or folder form, loads with no vertex stage and is not rewritten", "[shader][project]") {
    SimRole role;
    TempDir dir;
    std::string leaf_guid;
    std::string folder_guid;
    std::filesystem::path glsl;
    std::filesystem::path init_glsl;
    {
        engine_core::Game game;
        Project project = Project::create(dir.path, game);
        Shader& one = add_shader(game, "One");
        one.set_source(kVignette);
        Shader& two = add_shader(game, "Two");
        two.set_source(kVignette);
        leaf_guid = game.guid(one.id());
        folder_guid = game.guid(two.id());
        project.save();
        glsl = dir.path / leaf(game, one.id(), ".glsl");
        const std::filesystem::path folder = dir.path / leaf(game, two.id(), "");
        std::filesystem::create_directories(folder);
        std::filesystem::rename(dir.path / leaf(game, two.id(), ".glsl"), folder / "init.glsl");
        std::filesystem::rename(dir.path / leaf(game, two.id(), ".meta.json"), folder / "init.meta.json");
        init_glsl = folder / "init.glsl";
    }
    const auto written = std::filesystem::last_write_time(glsl);
    engine_core::Game game;
    Project read = Project::load(dir.path, game);
    const auto* one = dynamic_cast<const Shader*>(game.instance(*game.find_guid(leaf_guid)));
    const auto* two = dynamic_cast<const Shader*>(game.instance(*game.find_guid(folder_guid)));
    REQUIRE(one != nullptr);
    REQUIRE(two != nullptr);
    REQUIRE(one->source() == stored(kVignette));
    REQUIRE(!one->has_vertex_stage());
    REQUIRE(two->source() == stored(kVignette));
    REQUIRE(!two->has_vertex_stage());
    read.save();
    REQUIRE(std::filesystem::last_write_time(glsl) == written);
    // A save never keeps the folder form (SHA9): Two becomes a leaf, with no vertex file.
    REQUIRE(!std::filesystem::exists(init_glsl.parent_path() / "init.vert.glsl"));
    REQUIRE(read_text(dir.path / leaf(game, two->id(), ".glsl")) == stored(kVignette));
    REQUIRE(!std::filesystem::exists(dir.path / leaf(game, two->id(), ".vert.glsl")));
}

TEST_CASE("SHA23 copy, paste, clone, and instance files keep both stages", "[shader][copy]") {
    SimRole role;
    engine_core::Game game;
    Shader& shader = add_shader(game, "Look");
    shader.set_source(kVignette);
    shader.set_vertex_source(kLift);
    const InstanceId clone = engine_core::clone_tree(game, shader.id());
    REQUIRE(dynamic_cast<const Shader*>(game.instance(clone))->vertex_source() == kLift);
    const engine_core::JsonValue file = engine_core::write_instance_file({engine_core::copy_tree(game, shader.id())});
    std::vector<engine_core::CopiedNode> roots;
    std::string error;
    REQUIRE(engine_core::read_instance_file(file, roots, error));
    std::vector<InstanceId> made;
    REQUIRE(engine_core::paste_copies(game, roots, game.service("Shaders"), &made));
    REQUIRE(made.size() == 1);
    REQUIRE(dynamic_cast<const Shader*>(game.instance(made[0]))->vertex_source() == kLift);
    REQUIRE(dynamic_cast<const Shader*>(game.instance(made[0]))->source() == stored(kVignette));
    // Deleted and undone, it comes back with both.
    game.history().reset_waypoints();
    game.destroy(shader.id());
    game.history().undo();
    const auto* back = dynamic_cast<const Shader*>(game.instance(*game.find_guid(game.guid(made[0]))));
    REQUIRE(back != nullptr);
}

TEST_CASE("SHA24 a Source with phase 1's header lines sets Kind and reads back without them, as one step", "[shader]") {
    SimRole role;
    engine_core::Game game;
    Shader& shader = add_shader(game, "Look");
    REQUIRE(shader.kind() == ShaderKind::Surface);
    REQUIRE(std::string(engine_core::kShaderStarterSource).find("shader surface") == std::string::npos);
    game.history().reset_waypoints();
    begin_step(game);
    shader.set_source("shader post;\nrender_mode hdr;\nvoid post() {\n}\n");
    end_step(game);
    REQUIRE(shader.kind() == ShaderKind::Post);
    REQUIRE(shader.source() == "            \n                \nvoid post() {\n}\n");
    REQUIRE(shader.error().empty());
    game.history().undo();
    REQUIRE(shader.kind() == ShaderKind::Surface);
    REQUIRE(shader.source() == engine_core::kShaderStarterSource);
    game.history().redo();
    REQUIRE(shader.kind() == ShaderKind::Post);
    REQUIRE(shader.source().find("void post()") != std::string::npos);
}

TEST_CASE("SHA25 render_mode sets the booleans it names and leaves them alone when absent", "[shader]") {
    SimRole role;
    engine_core::Game game;
    Shader& shader = add_shader(game, "Look");
    REQUIRE(!shader.transparent());
    REQUIRE(!shader.two_sided());
    REQUIRE(shader.casts_shadow());
    shader.set_source("render_mode two_sided;\nvoid surface() {}\n");
    REQUIRE(shader.two_sided());
    REQUIRE(!shader.transparent());
    REQUIRE(shader.casts_shadow());
    REQUIRE_FALSE(shader.set_transparent(true));
    shader.set_source("void surface() { ALBEDO *= 0.5; }\n");
    REQUIRE(shader.transparent());
    REQUIRE(shader.two_sided());
    // Named again without two_sided: off.
    shader.set_source("render_mode no_shadow;\nvoid surface() {}\n");
    REQUIRE(!shader.two_sided());
    REQUIRE(!shader.transparent());
    REQUIRE(!shader.casts_shadow());
    REQUIRE(shader.set_displacement_margin(-1) == "DisplacementMargin must not be negative");
    REQUIRE(shader.displacement_margin() == 1.0);
    REQUIRE_FALSE(shader.set_displacement_margin(2.5));
    REQUIRE(shader.displacement_margin() == 2.5);
    // A post Shader has no vertex stage to speak of, and a Material refuses it.
    REQUIRE_FALSE(shader.set_kind(ShaderKind::Post));
    REQUIRE(shader.kind() == ShaderKind::Post);
    REQUIRE(shader.error() == "fragment 2: a post shader needs void post()");
}

TEST_CASE("SHA26 Error names the stage, and each stage keeps its own", "[shader]") {
    SimRole role;
    engine_core::Game game;
    Shader& shader = add_shader(game, "Look");
    shader.set_vertex_source("void vertex() {\n    VERTEX.y += 1.0\n}\n");
    REQUIRE(shader.vertex_error().empty());
    REQUIRE(shader.error().empty());
    shader.set_compile_error(shader.source_version(), engine_core::ShaderStage::Vertex, "3: syntax error");
    REQUIRE(shader.vertex_error() == "3: syntax error");
    REQUIRE(shader.fragment_error().empty());
    REQUIRE(shader.error() == "vertex 3: syntax error");
    // A vertex stage with no vertex() is the vertex stage's parse error.
    shader.set_vertex_source("uniform float A;\n");
    REQUIRE(shader.vertex_error() == "1: a vertex stage needs void vertex()");
    REQUIRE(shader.error() == "vertex 1: a vertex stage needs void vertex()");
    shader.set_vertex_source("");
    REQUIRE(shader.error().empty());
    // A uniform declared differently in the two stages errors in both.
    shader.set_source("uniform float A = 1.0;\nvoid surface() {}\n");
    shader.set_vertex_source("uniform float A = 2.0;\nvoid vertex() {}\n");
    REQUIRE(shader.fragment_error() == "1: A is declared differently in the vertex stage");
    REQUIRE(shader.vertex_error() == "1: A is declared differently in the fragment stage");
}

TEST_CASE("SHA27 the merged parse holds a uniform declared only in the vertex stage, and the binding knows the stage", "[shader]") {
    SimRole role;
    engine_core::Game game;
    Shader& shader = add_shader(game, "Look");
    shader.set_source("uniform float A = 1.0;\nvoid surface() {}\n");
    shader.set_vertex_source("uniform float Speed = 2.0;\nvoid vertex() {\n    VERTEX.y += sin(TIME * Speed);\n}\n");
    REQUIRE(shader.parse()->params.size() == 2);
    REQUIRE(shader.parse()->params[1].name == "Speed");
    REQUIRE(shader.stage_parse(engine_core::ShaderStage::Vertex)->reads_time);
    engine_core::Material& brick = add_material(game, "Brick", shader.id());
    REQUIRE_FALSE(shader.set_two_sided(true));
    REQUIRE_FALSE(shader.set_displacement_margin(3));
    auto binding = engine_core::make_surface_binding(game, brick);
    REQUIRE(binding->has_vertex);
    REQUIRE(binding->vertex_reads_time);
    REQUIRE(binding->vertex_glsl.find("void vertex()") != std::string::npos);
    REQUIRE(binding->displacement_margin == 3.0f);
    REQUIRE(binding->modes.two_sided);
    REQUIRE(!binding->modes.no_shadow);
    REQUIRE(binding->values()->slots.size() == 2);
    // A Kind or mode change is a new version, so programs build again.
    const std::uint64_t version = shader.source_version();
    REQUIRE_FALSE(shader.set_transparent(true));
    REQUIRE(shader.source_version() > version);
    // A broken vertex stage is no vertex stage to draw.
    shader.set_vertex_source("uniform float A;\n");
    REQUIRE(!engine_core::make_surface_binding(game, brick)->has_vertex);
}

TEST_CASE("SHA28 a Shader with no vertex text, saved, reads as in sync with the disk", "[shader][project][disk]") {
    SimRole role;
    TempDir dir;
    engine_core::Game game;
    Project project = Project::create(dir.path, game);
    Shader& shader = add_shader(game, "Plain");
    shader.set_source(kVignette);
    Shader& other = add_shader(game, "Other");
    other.set_source(kVignette);
    project.save();
    // Another file changes outside the studio: the scan compares the whole tree.
    std::ofstream(dir.path / leaf(game, other.id(), ".glsl"), std::ios::binary | std::ios::trunc)
        << "void surface() {}\n";
    const engine_core::DiskScan scan = project.scan_disk();
    REQUIRE(scan.conflicts.empty());
    REQUIRE(scan.has_disk_changes);
    const engine_core::DiskScan applied = project.apply_disk();
    REQUIRE(applied.loaded == std::vector<std::string>{"Other"});
    // Loaded, nothing is left to load: Plain's missing vertex file is its empty stage.
    const engine_core::DiskScan again = project.scan_disk();
    REQUIRE(again.conflicts.empty());
    REQUIRE_FALSE(again.has_disk_changes);
}

TEST_CASE("SHA29 renaming a Folder of a Shader with a vertex stage moves its vertex file, out of sync or not", "[shader][project][disk]") {
    SimRole role;
    TempDir dir;
    std::string guid;
    std::filesystem::path old_folder;
    {
        engine_core::Game game;
        Project project = Project::create(dir.path, game);
        engine_core::Folder& water = game.create<engine_core::Folder>();
        game.set_name(water.id(), "Water");
        game.set_parent(water.id(), game.service("Shaders"));
        Shader& shader = game.create<Shader>();
        game.set_name(shader.id(), "Ocean");
        game.set_parent(shader.id(), water.id());
        shader.set_source(kVignette);
        shader.set_vertex_source(kLift);
        Shader& other = add_shader(game, "Other");
        other.set_source(kVignette);
        guid = game.guid(shader.id());
        project.save();
        old_folder = dir.path / leaf(game, water.id(), "");
        REQUIRE(std::filesystem::exists(dir.path / leaf(game, shader.id(), ".vert.glsl")));
        // Something else changes outside the studio, so the next save plans the whole tree.
        std::ofstream(dir.path / leaf(game, other.id(), ".glsl"), std::ios::binary | std::ios::trunc)
            << "void surface() {}\n";
        game.set_name(water.id(), "Sea");
        project.save();
        REQUIRE(std::filesystem::exists(dir.path / leaf(game, shader.id(), ".vert.glsl")));
    }
    REQUIRE_FALSE(std::filesystem::exists(old_folder));
    engine_core::Game game;
    Project read = Project::load(dir.path, game);
    const auto* shader = dynamic_cast<const Shader*>(game.instance(*game.find_guid(guid)));
    REQUIRE(shader != nullptr);
    REQUIRE(shader->vertex_source() == kLift);
}

TEST_CASE("SHA30 a post Shader builds without its vertex text, which it never uses", "[shader]") {
    SimRole role;
    engine_core::Game game;
    Shader& shader = add_shader(game, "Fade");
    shader.set_vertex_source("uniform float A;\n");
    REQUIRE(shader.error() == "vertex 1: a vertex stage needs void vertex()");
    shader.set_source(kVignette);
    REQUIRE(shader.kind() == ShaderKind::Post);
    REQUIRE(shader.error().empty());
    REQUIRE(shader.vertex_error().empty());
    REQUIRE(shader.parse()->params.size() == 1);
    // Back to a surface, the vertex text counts again.
    REQUIRE_FALSE(shader.set_kind(ShaderKind::Surface));
    REQUIRE(shader.vertex_error() == "1: a vertex stage needs void vertex()");
}

TEST_CASE("SHA31 header and render_mode lines count only in the fragment stage", "[shader]") {
    SimRole role;
    engine_core::Game game;
    Shader& shader = add_shader(game, "Look");
    const std::string headed = "shader post;\nvoid vertex() {}\n";
    shader.set_vertex_source(headed);
    REQUIRE(shader.kind() == ShaderKind::Surface);
    REQUIRE(shader.vertex_source() == headed);
    REQUIRE(shader.vertex_error() == "1: the shader line belongs in the fragment stage");
    const std::string moded = "render_mode transparent;\nvoid vertex() {}\n";
    shader.set_vertex_source(moded);
    REQUIRE(!shader.transparent());
    REQUIRE(shader.vertex_source() == moded);
    REQUIRE(shader.vertex_error() == "1: render_mode belongs in the fragment stage");
}

TEST_CASE("SHA32 a mode change clears the driver's error, which was for the old program", "[shader]") {
    SimRole role;
    engine_core::Game game;
    Shader& shader = add_shader(game, "Look");
    shader.set_vertex_source(kLift);
    shader.set_compile_error(shader.source_version(), engine_core::ShaderStage::Fragment, "3: driver");
    shader.set_compile_error(shader.source_version(), engine_core::ShaderStage::Vertex, "2: driver");
    REQUIRE(shader.error() == "vertex 2: driver");
    REQUIRE_FALSE(shader.set_transparent(true));
    REQUIRE(shader.error().empty());
    REQUIRE(shader.fragment_error().empty());
    REQUIRE(shader.vertex_error().empty());
    shader.set_compile_error(shader.source_version(), engine_core::ShaderStage::Fragment, "3: driver");
    REQUIRE_FALSE(shader.set_two_sided(true));
    REQUIRE(shader.error().empty());
    shader.set_compile_error(shader.source_version(), engine_core::ShaderStage::Fragment, "3: driver");
    REQUIRE_FALSE(shader.set_casts_shadow(false));
    REQUIRE(shader.error().empty());
}

TEST_CASE("SHA33 reading a stage leaves the others where they are", "[shader]") {
    SimRole role;
    engine_core::Game game;
    Shader& shader = add_shader(game, "Look");
    const Shader& held = shader;
    const std::string* vertex = &held.source(Shader::kVertexStage);
    REQUIRE(held.source(7).empty());
    REQUIRE(vertex == &held.source(Shader::kVertexStage));
    REQUIRE(held.stage_count() == 2);
}

TEST_CASE("SHA34 FragmentSource fires Changed beside Source, as VertexSource does", "[shader]") {
    SimRole role;
    engine_core::Game game;
    Shader& shader = add_shader(game, "Look");
    std::vector<std::string> changed;
    game.changed(shader.id()).connect([&](InstanceId, engine_core::Field field) {
        if (field != engine_core::Field::Reflected) {
            changed.push_back(field == engine_core::Field::Source ? "Source" : "VertexSource");
            return;
        }
        const char* name = engine_core::lua_property_name(static_cast<std::uint32_t>(game.events().payload()));
        changed.push_back(name != nullptr ? name : "");
    });
    shader.set_source("void surface() {}\n");
    shader.set_vertex_source(kLift);
    game.events().drain();
    REQUIRE(changed == std::vector<std::string>{"Source", "FragmentSource", "VertexSource"});
}

TEST_CASE("SHA35 a phase 1 Shader file, header lines and no Kind in its json, loads as it meant", "[shader][project]") {
    SimRole role;
    TempDir dir;
    std::string glass_guid;
    std::string fade_guid;
    {
        engine_core::Game game;
        Project project = Project::create(dir.path, game);
        Shader& glass = add_shader(game, "Glass");
        Shader& fade = add_shader(game, "Fade");
        glass_guid = game.guid(glass.id());
        fade_guid = game.guid(fade.id());
        project.save();
        // As phase 1 wrote them: the header lines in the .glsl, and only class, id, and Name in the json.
        const auto phase1 = [&](InstanceId id, const std::string& guid, const char* name, const char* text) {
            std::ofstream(dir.path / leaf(game, id, ".meta.json"), std::ios::binary | std::ios::trunc)
                << "{\n  \"class\": \"Shader\",\n  \"id\": \"" << guid << "\",\n  \"Name\": \"" << name << "\"\n}\n";
            std::ofstream(dir.path / leaf(game, id, ".glsl"), std::ios::binary | std::ios::trunc) << text;
        };
        phase1(glass.id(), glass_guid, "Glass",
               "shader surface;\nrender_mode transparent;\nvoid surface() {\n    ALPHA = 0.5;\n}\n");
        phase1(fade.id(), fade_guid, "Fade", kVignette);
        REQUIRE(read_text(dir.path / leaf(game, glass.id(), ".meta.json")).find("Kind") == std::string::npos);
    }
    engine_core::Game game;
    Project read = Project::load(dir.path, game);
    const auto* glass = dynamic_cast<const Shader*>(game.instance(*game.find_guid(glass_guid)));
    const auto* fade = dynamic_cast<const Shader*>(game.instance(*game.find_guid(fade_guid)));
    REQUIRE(glass != nullptr);
    REQUIRE(fade != nullptr);
    REQUIRE(glass->kind() == ShaderKind::Surface);
    REQUIRE(glass->transparent());
    REQUIRE(!glass->two_sided());
    REQUIRE(glass->casts_shadow());
    REQUIRE(glass->source() == std::string(15, ' ') + "\n" + std::string(24, ' ') + "\nvoid surface() {\n    ALPHA = 0.5;\n}\n");
    REQUIRE(!glass->has_vertex_stage());
    REQUIRE(glass->error().empty());
    REQUIRE(fade->kind() == ShaderKind::Post);
    REQUIRE(fade->source() == stored(kVignette));
    REQUIRE(!fade->has_vertex_stage());
    REQUIRE(fade->error().empty());
}

TEST_CASE("SHA36 Hdr is a post Shader's saved mode, set by a render_mode line as the surface modes are", "[shader]") {
    SimRole role;
    engine_core::Game game;
    Shader& shader = add_shader(game, "Fade");
    REQUIRE_FALSE(shader.set_kind(ShaderKind::Post));
    REQUIRE(!shader.hdr());
    shader.set_source("render_mode hdr;\nvoid post() {}\n");
    REQUIRE(shader.hdr());
    REQUIRE(shader.source().rfind("                \nvoid post() {}\n", 0) == 0);
    REQUIRE(shader.error().empty());
    shader.set_source("render_mode;\nvoid post() {}\n");
    REQUIRE(!shader.hdr());
    // A property of its own: a version, Changed, undo as one step, and the place bytes.
    std::vector<std::string> changed;
    game.changed(shader.id()).connect([&](InstanceId, engine_core::Field) {
        const char* name = engine_core::lua_property_name(static_cast<std::uint32_t>(game.events().payload()));
        changed.push_back(name != nullptr ? name : "");
    });
    const std::uint64_t version = shader.source_version();
    game.history().reset_waypoints();
    begin_step(game);
    REQUIRE_FALSE(shader.set_hdr(true));
    end_step(game);
    game.events().drain();
    REQUIRE(shader.source_version() > version);
    REQUIRE(changed == std::vector<std::string>{"Hdr"});
    game.history().undo();
    REQUIRE(!shader.hdr());
    game.history().redo();
    REQUIRE(shader.hdr());
    game.capture_place();
    game.start_simulation();
    REQUIRE_FALSE(shader.set_hdr(false));
    game.stop_simulation();
    REQUIRE(shader.hdr());
    // On a surface Shader the line stays an error, and the property is kept but ignored.
    REQUIRE_FALSE(shader.set_kind(ShaderKind::Surface));
    REQUIRE(shader.hdr());
    shader.set_source("render_mode hdr;\nvoid surface() {}\n");
    REQUIRE(shader.error() == "fragment 1: unknown render_mode hdr; a surface shader has transparent, two_sided, no_shadow");
}

TEST_CASE("SHA36b scripts read and write Hdr", "[shader][lua]") {
    ScriptRig rig;
    rig.runtime.run_chunk(R"(
        local s = Instance.new("Shader")
        s.Kind = Enum.ShaderKind.Post
        s.Source = "void post() {}\n"
        s.Parent = game.Assets.Shaders
        print(s.Hdr)
        s.Hdr = true
        print(s.Hdr)
    )");
    const std::vector<std::string> out = printed(rig.runtime.drain_output());
    INFO(rig.runtime.last_error());
    REQUIRE(out == std::vector<std::string>{"false\n", "true\n"});
}
