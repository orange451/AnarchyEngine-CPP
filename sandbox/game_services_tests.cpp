// Game services: Assets and its five categories under game, the asset classes
// they hold, and references between assets.

#include "Containment.hpp"
#include "DataModel.hpp"
#include "Folder.hpp"
#include "Game.hpp"
#include "ChangeHistoryService.hpp"
#include "Contract.hpp"
#include "GameService.hpp"
#include "AssetInstances.hpp"
#include "LuaApi.hpp"
#include "PropertyReflection.hpp"
#include "Project.hpp"

#include "support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace {

std::string reason(const std::optional<std::string>& error) { return error.value_or("<allowed>"); }

}  // namespace

TEST_CASE("GS1 the placement rules, by class name", "[GS1]") {
    using engine_core::placement_error;

    // The service table, in the order game and Assets hold them.
    REQUIRE(std::size(engine_core::kServices) == 10);
    REQUIRE(std::string(engine_core::kServices[4].class_name) == "Assets");
    REQUIRE(engine_core::kServices[4].parent_class == nullptr);
    REQUIRE(std::string(engine_core::kServices[5].class_name) == "Materials");
    REQUIRE(std::string(engine_core::kServices[5].parent_class) == "Assets");
    REQUIRE(engine_core::find_service("Textures") != nullptr);
    REQUIRE(engine_core::find_service("Texture") == nullptr);
    REQUIRE(engine_core::service_guid("Textures") == "textures");

    REQUIRE(std::string(engine_core::asset_home("Texture")) == "Textures");
    REQUIRE(std::string(engine_core::asset_home("Sound")) == "Audio");
    REQUIRE(std::string(engine_core::asset_home("Model")) == "Prefab");
    REQUIRE(engine_core::asset_home("Folder") == nullptr);
    REQUIRE(engine_core::passes_rule_up("Folder"));
    REQUIRE_FALSE(engine_core::passes_rule_up("Prefab"));

    // game takes only services.
    REQUIRE(reason(placement_error("Game", "Folder", "Box")) ==
            "Only scene services can be children of game; put Box in Workspace");
    REQUIRE_FALSE(placement_error("Game", "Assets", "Assets"));

    // Assets takes only its categories.
    REQUIRE(reason(placement_error("Assets", "Folder", "Box")) ==
            "Assets holds only Materials, Prefabs, Meshes, Textures, and Audio");
    REQUIRE(reason(placement_error("Assets", "Texture", "Brick")) ==
            "Assets holds only Materials, Prefabs, Meshes, Textures, and Audio");

    // A category takes its class and Folders.
    REQUIRE_FALSE(placement_error("Textures", "Texture", "Brick"));
    REQUIRE_FALSE(placement_error("Textures", "Folder", "Bricks"));
    REQUIRE(reason(placement_error("Textures", "Mesh", "Rock")) == "Textures holds Textures and Folders");
    REQUIRE(reason(placement_error("Meshes", "Script", "Main")) == "Meshes holds Meshes and Folders");
    REQUIRE(reason(placement_error("Audio", "Texture", "Brick")) == "Audio holds Sounds and Folders");
    REQUIRE_FALSE(placement_error("Audio", "Sound", "Boom"));
    REQUIRE_FALSE(placement_error("Prefabs", "Prefab", "Crate"));

    // A Prefab takes only Models, and a Model goes only in a Prefab.
    REQUIRE_FALSE(placement_error("Prefab", "Model", "Body"));
    REQUIRE(reason(placement_error("Prefab", "Folder", "Parts")) == "A Prefab holds only Models");
    REQUIRE(reason(placement_error("Workspace", "Model", "Body")) == "A Model must be in a Prefab");
    REQUIRE(reason(placement_error("Prefabs", "Model", "Body")) == "Prefabs holds Prefabs and Folders");

    // Anything else takes anything but an asset.
    REQUIRE_FALSE(placement_error("Workspace", "Folder", "Box"));
    REQUIRE_FALSE(placement_error("GameObject", "Script", "Main"));
    REQUIRE(reason(placement_error("Workspace", "Texture", "Brick")) == "A Texture must be in Assets.Textures");
    REQUIRE(reason(placement_error("Storage", "Material", "Brick")) == "A Material must be in Assets.Materials");
    REQUIRE(reason(placement_error("GameObject", "Sound", "Boom")) == "A Sound must be in Assets.Audio");

    // A leaf asset holds nothing.
    REQUIRE(reason(placement_error("Texture", "Folder", "Box")) == "A Texture holds nothing");
    REQUIRE(reason(placement_error("Mesh", "GameObject", "Box")) == "A Mesh holds nothing");
    REQUIRE(reason(placement_error("Sound", "Script", "Main")) == "A Sound holds nothing");
    REQUIRE(reason(placement_error("Material", "Texture", "Brick")) == "A Material holds nothing");
    REQUIRE(reason(placement_error("Model", "Sound", "Boom")) == "A Model holds nothing");
}

using engine_core::ContractViolation;
using engine_core::DataModel;
using engine_core::Folder;
using engine_core::Game;
using engine_core::InstanceId;

namespace {

std::vector<std::string> child_classes(const DataModel& game, InstanceId parent) {
    std::vector<std::string> out;
    for (InstanceId child : game.get_children(parent)) {
        out.push_back(game.instance(child)->class_name());
    }
    return out;
}

}  // namespace

TEST_CASE("GS2 a new Game holds Assets and its five categories, hidden from the explorer", "[GS2]") {
    Game game;
    REQUIRE(child_classes(game, 0) ==
            std::vector<std::string>{"Workspace", "Lighting", "Storage", "Scripts", "Assets"});
    const InstanceId assets = game.service("Assets");
    REQUIRE(assets != 0);
    REQUIRE(game.parent(assets) == 0);
    REQUIRE(child_classes(game, assets) ==
            std::vector<std::string>{"Materials", "Prefabs", "Meshes", "Textures", "Audio"});
    for (const engine_core::ServiceSpec& spec : engine_core::kServices) {
        INFO(spec.class_name);
        const InstanceId id = game.service(spec.class_name);
        REQUIRE(id != 0);
        const DataModel* service = game.instance(id);
        REQUIRE(service->is_service());
        REQUIRE(game.name(id) == spec.class_name);
        REQUIRE(game.guid(id) == engine_core::service_guid(spec.class_name));
        const bool game_service = std::string(spec.class_name) == "Assets" || spec.parent_class != nullptr;
        REQUIRE(service->hidden_in_explorer() == game_service);
        REQUIRE(service->is_scene_service() == !game_service);
    }
    // scene_service still finds only the four scene services.
    REQUIRE(game.scene_service("Workspace") == game.service("Workspace"));
    REQUIRE(game.scene_service("Assets") == 0);
    REQUIRE(game.service("Folder") == 0);
    REQUIRE_FALSE(game.history().can_undo().first);
}

TEST_CASE("GS3 a game service cannot be moved, renamed, or destroyed", "[GS3]") {
    Game game;
    const InstanceId assets = game.service("Assets");
    const InstanceId textures = game.service("Textures");
    const InstanceId meshes = game.service("Meshes");
    const InstanceId workspace = game.service("Workspace");

    REQUIRE(reason(game.parent_error(assets, workspace)) == "Assets cannot be moved");
    REQUIRE(reason(game.parent_error(textures, meshes)) == "Textures cannot be moved");
    REQUIRE(reason(game.parent_error(textures, 0)) == "Textures cannot be moved");
    REQUIRE(reason(game.parent_error(textures, DataModel::kNoParent)) == "Textures cannot be moved");
    REQUIRE_FALSE(game.parent_error(textures, assets));
    REQUIRE(reason(game.rename_error(textures, "Images")) == "Textures cannot be renamed");
    REQUIRE_FALSE(game.rename_error(textures, "Textures"));
    REQUIRE(reason(game.destroy_error(assets)) == "Assets cannot be destroyed");
    REQUIRE(reason(game.destroy_error(textures)) == "Textures cannot be destroyed");

    REQUIRE_THROWS_AS(game.set_parent(textures, workspace), ContractViolation);
    REQUIRE_THROWS_AS(game.set_name(assets, "Stuff"), ContractViolation);
    REQUIRE_THROWS_AS(game.destroy(textures), ContractViolation);
    REQUIRE_THROWS_AS(game.destroy_tree(assets), ContractViolation);
    REQUIRE(game.parent(textures) == assets);

    // Undo and Stop keep them, as they keep the scene services.
    SimRole role;
    game.start_simulation();
    game.stop_simulation();
    REQUIRE(child_classes(game, assets).size() == 5);
}

namespace {

InstanceId make(DataModel& game, const char* klass, const char* name, InstanceId parent) {
    DataModel* object = engine_core::lua_create_instance(game, klass);
    REQUIRE(object != nullptr);
    game.set_name(object->id(), name);
    if (parent != DataModel::kNoParent) {
        game.set_parent(object->id(), parent);
    }
    return object->id();
}

}  // namespace

TEST_CASE("GS4 each category takes its class and Folders", "[GS4]") {
    Game game;
    const InstanceId textures = game.service("Textures");
    const InstanceId brick = make(game, "Texture", "Brick", DataModel::kNoParent);
    const InstanceId rock = make(game, "Mesh", "Rock", DataModel::kNoParent);
    const InstanceId folder = make(game, "Folder", "Walls", textures);

    REQUIRE_FALSE(game.parent_error(brick, textures));
    REQUIRE_FALSE(game.parent_error(brick, folder));
    REQUIRE(reason(game.parent_error(rock, textures)) == "Textures holds Textures and Folders");
    REQUIRE(reason(game.parent_error(rock, folder)) == "Textures holds Textures and Folders");
    REQUIRE(reason(game.parent_error(brick, game.service("Workspace"))) == "A Texture must be in Assets.Textures");
    REQUIRE(reason(game.parent_error(brick, game.service("Assets"))) ==
            "Assets holds only Materials, Prefabs, Meshes, Textures, and Audio");
    REQUIRE(reason(game.parent_error(folder, game.service("Assets"))) ==
            "Assets holds only Materials, Prefabs, Meshes, Textures, and Audio");
    game.set_parent(brick, folder);
    REQUIRE(game.parent(brick) == folder);

    // Prefab and Model.
    const InstanceId crate = make(game, "Prefab", "Crate", game.service("Prefabs"));
    const InstanceId body = make(game, "Model", "Body", crate);
    REQUIRE(game.parent(body) == crate);
    REQUIRE(reason(game.parent_error(folder, crate)) == "A Prefab holds only Models");
    REQUIRE(reason(game.parent_error(body, game.service("Workspace"))) == "A Model must be in a Prefab");
    REQUIRE(reason(game.parent_error(body, game.service("Prefabs"))) == "Prefabs holds Prefabs and Folders");
    // Out of the tree is always allowed.
    REQUIRE_FALSE(game.parent_error(body, DataModel::kNoParent));

    // A leaf asset holds nothing, not even a Folder or a plain instance.
    const InstanceId box = make(game, "GameObject", "Box", DataModel::kNoParent);
    const InstanceId loose = make(game, "Folder", "Loose", DataModel::kNoParent);
    REQUIRE(reason(game.parent_error(box, brick)) == "A Texture holds nothing");
    REQUIRE(reason(game.parent_error(loose, brick)) == "A Texture holds nothing");
    REQUIRE(reason(game.parent_error(loose, body)) == "A Model holds nothing");
    REQUIRE_THROWS_AS(game.set_parent(box, brick), ContractViolation);
    REQUIRE(game.get_children(brick).empty());
}

TEST_CASE("GS5 a folder carries its assets' rules with it", "[GS5]") {
    Game game;
    const InstanceId walls = make(game, "Folder", "Walls", game.service("Textures"));
    const InstanceId inner = make(game, "Folder", "Inner", walls);
    make(game, "Texture", "Brick", inner);

    REQUIRE(reason(game.parent_error(walls, game.service("Workspace"))) == "A Texture must be in Assets.Textures");
    REQUIRE(reason(game.parent_error(walls, game.service("Meshes"))) == "Meshes holds Meshes and Folders");
    REQUIRE_THROWS_AS(game.set_parent(walls, game.service("Meshes")), ContractViolation);
    REQUIRE(game.parent(walls) == game.service("Textures"));

    // A folder of plain instances moves between scene services as before.
    const InstanceId box = make(game, "Folder", "Box", game.service("Workspace"));
    make(game, "Script", "Main", box);
    REQUIRE_FALSE(game.parent_error(box, game.service("Storage")));
    REQUIRE(reason(game.parent_error(box, game.service("Textures"))) == "Textures holds Textures and Folders");

    // A folder out of the tree takes anything; putting it back is checked.
    game.set_parent(walls, DataModel::kNoParent);
    REQUIRE(reason(game.parent_error(walls, game.service("Workspace"))) == "A Texture must be in Assets.Textures");
    REQUIRE_FALSE(game.parent_error(walls, game.service("Textures")));
}

TEST_CASE("GS6 scripts see game services and meet the same rules", "[GS6]") {
    ScriptRig rig;
    add_script(rig.game, "Assets", R"(
        local function refuses(fn, expected)
            local ok, message = pcall(fn)
            return not ok and string.find(message, expected, 1, true) ~= nil
        end
        _G.path = game.Assets.Textures.ClassName == "Textures" and game:GetService("Assets") == game.Assets
        _G.isa = game.Assets:IsA("GameService") and game.Assets:IsA("Service") and not game.Assets:IsA("Instance")
            and workspace:IsA("Service")
        _G.listed = #game:GetChildren() == 5
        _G.no_move = refuses(function() game.Assets.Textures.Parent = workspace end, "Textures cannot be moved")
        _G.no_rename = refuses(function() game.Assets.Name = "Stuff" end, "Assets cannot be renamed")
        _G.no_destroy = refuses(function() game.Assets.Audio:Destroy() end, "Audio cannot be destroyed")
        _G.no_new = refuses(function() Instance.new("Textures") end, "unknown class Textures")
        _G.no_nested = not pcall(function() return game:GetService("Textures") end)

        local brick = Instance.new("Texture")
        brick.Parent = game.Assets.Textures
        _G.placed = brick.Parent == game.Assets.Textures
        _G.no_workspace = refuses(function() brick.Parent = workspace end, "A Texture must be in Assets.Textures")
        local model = Instance.new("Model")
        _G.no_model = refuses(function() model.Parent = workspace end, "A Model must be in a Prefab")
    )");
    rig.game.start_simulation();
    rig.frames(1, 0.05);
    INFO(rig.runtime.last_error());
    for (const char* name : {"path", "isa", "listed", "no_move", "no_rename", "no_destroy", "no_new", "no_nested",
                             "placed", "no_workspace", "no_model"}) {
        bool value = false;
        INFO(name);
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }
}

TEST_CASE("GS7 Path is relative to the resources folder, and saves and undoes", "[GS7]") {
    SimRole role;
    Game game;
    const InstanceId id = make(game, "Texture", "Brick", game.service("Textures"));
    auto& brick = *dynamic_cast<engine_core::Texture*>(game.instance(id));
    REQUIRE(brick.path().empty());

    for (const char* bad : {"/abs/brick.png", "C:/brick.png", "textures\\brick.png", "../brick.png",
                            "textures/../../brick.png"}) {
        INFO(bad);
        REQUIRE(reason(brick.set_path(bad)) == "Path must be relative to the resources folder");
    }
    REQUIRE(brick.path().empty());

    game.history().set_pending_gesture("Set Path");
    REQUIRE_FALSE(brick.set_path("textures/brick.png"));
    game.history().end_gesture();
    REQUIRE(brick.path() == "textures/brick.png");
    engine_core::PropertyBag saved;
    brick.save_properties(saved);
    REQUIRE(engine_core::bag_find(saved, "Path") != nullptr);
    REQUIRE(engine_core::bag_find(saved, "Path")->as_string() == "textures/brick.png");

    game.history().undo();
    REQUIRE(brick.path().empty());
    game.history().redo();
    REQUIRE(brick.path() == "textures/brick.png");

    // A default Path saves nothing.
    const InstanceId other = make(game, "Sound", "Boom", game.service("Audio"));
    engine_core::PropertyBag none;
    game.instance(other)->save_properties(none);
    REQUIRE(none.empty());

    // Stop puts it back.
    game.start_simulation();
    REQUIRE_FALSE(brick.set_path("textures/other.png"));
    game.stop_simulation();
    REQUIRE(brick.path() == "textures/brick.png");
}

namespace {

engine_core::LuaSlot read_field(DataModel& game, InstanceId id, const char* property) {
    DataModel* object = game.instance(id);
    const engine_core::LuaField* field = engine_core::lua_class_find(object->class_name(), property);
    REQUIRE(field != nullptr);
    engine_core::LuaSlot slot;
    REQUIRE(field->read(game, *object, slot));
    return slot;
}

bool write_field(DataModel& game, InstanceId id, const char* property, engine_core::LuaSlot slot,
                 std::string* error = nullptr) {
    DataModel* object = game.instance(id);
    const engine_core::LuaField* field = engine_core::lua_class_find(object->class_name(), property);
    REQUIRE(field != nullptr);
    const bool ok = field->write(game, *object, slot);
    if (error != nullptr) {
        *error = slot.error;
    }
    return ok;
}

engine_core::LuaSlot instance_slot(InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

}  // namespace

TEST_CASE("GS8 a reference takes its class, saves as a GUID, and reads the live target", "[GS8]") {
    SimRole role;
    Game game;
    const InstanceId brick = make(game, "Texture", "Brick", game.service("Textures"));
    const InstanceId rock = make(game, "Mesh", "Rock", game.service("Meshes"));
    const InstanceId mat = make(game, "Material", "Wall", game.service("Materials"));

    REQUIRE(engine_core::reference_class("Texture?") == "Texture");
    REQUIRE(engine_core::reference_class("Texture").empty());
    REQUIRE(engine_core::reference_class("number").empty());

    // nil by default, and a default saves nothing.
    REQUIRE(read_field(game, mat, "DiffuseTexture").kind == engine_core::LuaSlot::Kind::Nil);
    engine_core::PropertyBag saved;
    game.instance(mat)->save_properties(saved);
    REQUIRE(saved.empty());

    std::string error;
    REQUIRE_FALSE(write_field(game, mat, "DiffuseTexture", instance_slot(rock), &error));
    REQUIRE(error == "DiffuseTexture must be a Texture");
    REQUIRE(write_field(game, mat, "DiffuseTexture", instance_slot(brick)));
    const engine_core::LuaSlot read = read_field(game, mat, "DiffuseTexture");
    REQUIRE(read.kind == engine_core::LuaSlot::Kind::Instance);
    REQUIRE(read.id == brick);
    REQUIRE(read.text == game.guid(brick));

    game.instance(mat)->save_properties(saved);
    REQUIRE(engine_core::bag_find(saved, "DiffuseTexture")->as_string() == game.guid(brick));

    // A load names the target by GUID, which need not exist yet.
    Game other;
    const InstanceId copy = make(other, "Material", "Wall", other.service("Materials"));
    std::string load_error;
    REQUIRE(other.instance(copy)->load_property("DiffuseTexture", engine_core::JsonValue::string("zzzz"),
                                                load_error));
    REQUIRE(load_error.empty());
    const engine_core::LuaSlot dangling = read_field(other, copy, "DiffuseTexture");
    REQUIRE(dangling.kind == engine_core::LuaSlot::Kind::Nil);
    REQUIRE(dangling.text == "zzzz");
    // It saves back as it was read.
    engine_core::PropertyBag kept;
    other.instance(copy)->save_properties(kept);
    REQUIRE(engine_core::bag_find(kept, "DiffuseTexture")->as_string() == "zzzz");
    // Once an instance holds that GUID, the reference finds it.
    const InstanceId late = make(other, "Texture", "Late", other.service("Textures"));
    other.set_guid(late, "zzzz");
    REQUIRE(read_field(other, copy, "DiffuseTexture").id == late);

    // null clears.
    REQUIRE(other.instance(copy)->load_property("DiffuseTexture", engine_core::JsonValue(), load_error));
    REQUIRE(read_field(other, copy, "DiffuseTexture").text.empty());

    // Model's two references.
    const InstanceId crate = make(game, "Prefab", "Crate", game.service("Prefabs"));
    const InstanceId body = make(game, "Model", "Body", crate);
    REQUIRE(write_field(game, body, "Mesh", instance_slot(rock)));
    REQUIRE(write_field(game, body, "Material", instance_slot(mat)));
    REQUIRE_FALSE(write_field(game, body, "Material", instance_slot(brick), &error));
    REQUIRE(error == "Material must be a Material");
}

TEST_CASE("GS9 a reference to a destroyed asset reads nil, and undo brings it back", "[GS9]") {
    SimRole role;
    Game game;
    const InstanceId brick = make(game, "Texture", "Brick", game.service("Textures"));
    const InstanceId mat = make(game, "Material", "Wall", game.service("Materials"));
    // Close the implicit gesture the two creates opened, so it does not
    // absorb the property change below into the same undo step.
    game.history().end_gesture();
    game.history().set_pending_gesture("Set DiffuseTexture");
    REQUIRE(write_field(game, mat, "DiffuseTexture", instance_slot(brick)));
    game.history().end_gesture();
    REQUIRE(game.history().can_undo().second == "Set DiffuseTexture");

    game.history().set_pending_gesture("Delete");
    game.destroy_tree(brick);
    game.history().end_gesture();
    REQUIRE(read_field(game, mat, "DiffuseTexture").kind == engine_core::LuaSlot::Kind::Nil);

    game.history().undo();
    REQUIRE(game.alive(brick));
    REQUIRE(read_field(game, mat, "DiffuseTexture").id == brick);

    game.history().undo();
    REQUIRE(read_field(game, mat, "DiffuseTexture").kind == engine_core::LuaSlot::Kind::Nil);
    game.history().redo();
    REQUIRE(read_field(game, mat, "DiffuseTexture").id == brick);
}

TEST_CASE("GS10 Stop restores references and drops assets made in play", "[GS10]") {
    ScriptRig rig;
    DataModel& game = rig.game;
    const InstanceId brick = make(game, "Texture", "Brick", game.service("Textures"));
    const InstanceId mat = make(game, "Material", "Wall", game.service("Materials"));
    REQUIRE(write_field(game, mat, "DiffuseTexture", instance_slot(brick)));
    add_script(game, "Play", R"(
        local wall = game.Assets.Materials.Wall
        _G.reads = wall.DiffuseTexture == game.Assets.Textures.Brick
        local made = Instance.new("Texture")
        made.Name = "Made"
        made.Parent = game.Assets.Textures
        wall.NormalTexture = made
        wall.DiffuseTexture = nil
        _G.refused = not pcall(function() wall.RoughnessTexture = workspace end)
        _G.done = wall.NormalTexture == made and wall.DiffuseTexture == nil
    )");
    game.start_simulation();
    rig.frames(1, 0.05);
    for (const char* name : {"reads", "refused", "done"}) {
        bool value = false;
        INFO(name);
        INFO(rig.runtime.last_error());
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }
    game.stop_simulation();
    REQUIRE(read_field(game, mat, "DiffuseTexture").id == brick);
    REQUIRE(read_field(game, mat, "NormalTexture").kind == engine_core::LuaSlot::Kind::Nil);
    REQUIRE(game.get_children(game.service("Textures")).size() == 1);
}

namespace {

void write_text(const std::filesystem::path& path, const std::string& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

std::string file_of(const char* klass, const char* guid, const char* name, const std::string& extra = "") {
    return std::string("{\n  \"class\": \"") + klass + "\",\n  \"id\": \"" + guid + "\",\n  \"Name\": \"" + name +
           "\"" + extra + "\n}\n";
}

// A place saved before Assets existed: only Workspace and a Folder in it.
void write_old_place(const std::filesystem::path& root) {
    write_text(root / "project.json",
               "{\"format\": 1, \"name\": \"Old\", \"engine\": \"engine_core\", \"tree\": {\"src\": \"src\"}, "
               "\"resources\": {\"root\": \"resources\"}}\n");
    write_text(root / "src" / "init.json", file_of("Game", "root0", "Old"));
    const std::filesystem::path workspace = root / "src" / "Workspace.workspace";
    write_text(workspace / "init.json", file_of("Workspace", "workspace", "Workspace"));
    write_text(workspace / "Box.cccc.json", file_of("Folder", "cccc", "Box"));
}

}  // namespace

TEST_CASE("GS11 the Assets tree and its references round-trip through a project", "[GS11][project]") {
    SimRole role;
    TempDir dir;
    namespace fs = std::filesystem;
    std::string brick_guid;
    {
        engine_core::Project project = engine_core::Project::create(dir.path);
        DataModel& game = project.datamodel();
        const InstanceId walls = make(game, "Folder", "Walls", game.service("Textures"));
        const InstanceId brick = make(game, "Texture", "Brick", walls);
        brick_guid = game.guid(brick);
        REQUIRE_FALSE(dynamic_cast<engine_core::Texture*>(game.instance(brick))->set_path("textures/brick.png"));
        const InstanceId mat = make(game, "Material", "Wall", game.service("Materials"));
        REQUIRE(write_field(game, mat, "DiffuseTexture", instance_slot(brick)));
        const InstanceId crate = make(game, "Prefab", "Crate", game.service("Prefabs"));
        const InstanceId body = make(game, "Model", "Body", crate);
        REQUIRE(write_field(game, body, "Material", instance_slot(mat)));
        project.save();
    }
    REQUIRE(fs::is_directory(dir.path / "src" / "Assets.assets"));
    REQUIRE(fs::exists(dir.path / "src" / "Assets.assets" / "init.json"));
    REQUIRE(fs::exists(dir.path / "src" / "Assets.assets" / "Audio.audio.json"));
    REQUIRE(fs::is_directory(dir.path / "src" / "Assets.assets" / "Textures.textures"));

    engine_core::Project loaded = engine_core::Project::load(dir.path);
    DataModel& game = loaded.datamodel();
    const InstanceId assets = game.service("Assets");
    REQUIRE(child_classes(game, assets) ==
            std::vector<std::string>{"Materials", "Prefabs", "Meshes", "Textures", "Audio"});
    const InstanceId brick = *game.find_guid(brick_guid);
    REQUIRE(game.name(game.parent(brick)) == "Walls");
    REQUIRE(dynamic_cast<engine_core::Texture*>(game.instance(brick))->path() == "textures/brick.png");
    const InstanceId mat = game.find_first_child(game.service("Materials"), "Wall");
    REQUIRE(read_field(game, mat, "DiffuseTexture").id == brick);
    const InstanceId body = game.find_first_child(game.find_first_child(game.service("Prefabs"), "Crate"), "Body");
    REQUIRE(read_field(game, body, "Material").id == mat);

    // Nothing differs from disk, and a second save writes nothing.
    REQUIRE_FALSE(loaded.unsaved());
    loaded.save();
    REQUIRE(loaded.last_save().written.empty());
}

TEST_CASE("GS12 a reference to a missing GUID loads, reads nil, and saves unchanged", "[GS12][project]") {
    SimRole role;
    TempDir dir;
    {
        engine_core::Project project = engine_core::Project::create(dir.path);
        project.save();
    }
    const std::filesystem::path mat_file =
        dir.path / "src" / "Assets.assets" / "Materials.materials" / "Wall.eeee.json";
    // Materials may be a leaf file until it has children; the load reads either.
    std::filesystem::remove(dir.path / "src" / "Assets.assets" / "Materials.materials.json");
    write_text(dir.path / "src" / "Assets.assets" / "Materials.materials" / "init.json",
               file_of("Materials", "materials", "Materials"));
    const std::string bytes = file_of("Material", "eeee", "Wall", ",\n  \"DiffuseTexture\": \"gone\"");
    write_text(mat_file, bytes);
    engine_core::Project project = engine_core::Project::load(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId mat = *game.find_guid("eeee");
    REQUIRE(read_field(game, mat, "DiffuseTexture").kind == engine_core::LuaSlot::Kind::Nil);
    project.save();
    std::ifstream in(mat_file, std::ios::binary);
    const std::string after((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(after.find("\"DiffuseTexture\": \"gone\"") != std::string::npos);
}

TEST_CASE("GS13 a place saved before Assets loads with the whole tree made", "[GS13][project]") {
    SimRole role;
    TempDir dir;
    write_old_place(dir.path);
    engine_core::Project project = engine_core::Project::load(dir.path);
    DataModel& game = project.datamodel();
    REQUIRE(child_classes(game, 0) ==
            std::vector<std::string>{"Workspace", "Lighting", "Storage", "Scripts", "Assets"});
    REQUIRE(child_classes(game, game.service("Assets")).size() == 5);
    REQUIRE(project.unsaved());
    REQUIRE_FALSE(std::filesystem::exists(dir.path / "src" / "Assets.assets"));
    project.save();
    REQUIRE(std::filesystem::exists(dir.path / "src" / "Assets.assets" / "Textures.textures.json"));
    REQUIRE_FALSE(project.unsaved());
}

TEST_CASE("GS14 an Assets folder that lacks a category gets it", "[GS14][project]") {
    SimRole role;
    TempDir dir;
    write_old_place(dir.path);
    const std::filesystem::path assets = dir.path / "src" / "Assets.assets";
    write_text(assets / "init.json", file_of("Assets", "assets", "Assets"));
    write_text(assets / "Textures.textures.json", file_of("Textures", "textures", "Textures"));
    engine_core::Project project = engine_core::Project::load(dir.path);
    DataModel& game = project.datamodel();
    REQUIRE(child_classes(game, game.service("Assets")) ==
            std::vector<std::string>{"Materials", "Prefabs", "Meshes", "Textures", "Audio"});
    REQUIRE(game.guid(game.service("Textures")) == "textures");
}

TEST_CASE("GS15 a file that breaks a placement rule fails the load and names the file", "[GS15][project]") {
    SimRole role;
    auto expect_failure = [](const std::function<void(const std::filesystem::path&)>& setup,
                             const std::string& expected) {
        TempDir dir;
        write_old_place(dir.path);
        setup(dir.path);
        try {
            engine_core::Project::load(dir.path);
            FAIL("the load should refuse: " << expected);
        } catch (const engine_core::ProjectError& error) {
            INFO(error.what());
            REQUIRE(std::string(error.what()).find(expected) != std::string::npos);
        }
    };
    const std::filesystem::path src("src");
    expect_failure(
        [&](const std::filesystem::path& root) {
            write_text(root / src / "Workspace.workspace" / "Brick.ffff.json", file_of("Texture", "ffff", "Brick"));
        },
        "Brick.ffff.json: A Texture must be in Assets.Textures");
    expect_failure(
        [&](const std::filesystem::path& root) {
            const std::filesystem::path assets = root / src / "Assets.assets";
            write_text(assets / "init.json", file_of("Assets", "assets", "Assets"));
            write_text(assets / "Loose.gggg.json", file_of("Folder", "gggg", "Loose"));
        },
        "Loose.gggg.json: Assets holds only Materials, Prefabs, Meshes, Textures, and Audio");
    expect_failure(
        [&](const std::filesystem::path& root) {
            const std::filesystem::path textures = root / src / "Assets.assets" / "Textures.textures";
            write_text(root / src / "Assets.assets" / "init.json", file_of("Assets", "assets", "Assets"));
            write_text(textures / "init.json", file_of("Textures", "textures", "Textures"));
            write_text(textures / "Rock.hhhh.json", file_of("Mesh", "hhhh", "Rock"));
        },
        "Rock.hhhh.json: Textures holds Textures and Folders");
    expect_failure(
        [&](const std::filesystem::path& root) {
            const std::filesystem::path materials = root / src / "Assets.assets" / "Materials.materials";
            write_text(root / src / "Assets.assets" / "init.json", file_of("Assets", "assets", "Assets"));
            write_text(materials / "init.json", file_of("Materials", "materials", "Materials"));
            write_text(materials / "Wall.eeee" / "init.json", file_of("Material", "eeee", "Wall"));
            write_text(materials / "Wall.eeee" / "Box.iiii.json", file_of("Folder", "iiii", "Box"));
        },
        "Box.iiii.json: A Material holds nothing");
    expect_failure(
        [&](const std::filesystem::path& root) {
            write_text(root / src / "Workspace.workspace" / "Textures.textures.json",
                       file_of("Textures", "textures", "Textures"));
        },
        "Textures must be a child of Assets with GUID textures");
}

TEST_CASE("GS16 apply_disk reorders assets directly under a category, but Assets stays in table order",
          "[GS16][project]") {
    SimRole role;
    TempDir dir;
    namespace fs = std::filesystem;
    engine_core::Project project = engine_core::Project::create(dir.path);
    DataModel& game = project.datamodel();
    const InstanceId materials = game.service("Materials");
    const InstanceId a = make(game, "Material", "A", materials);
    const InstanceId b = make(game, "Material", "B", materials);
    project.save();

    // Swap the two Materials on disk, as another editor would reorder them.
    const std::string a_guid = game.guid(a);
    const std::string b_guid = game.guid(b);
    const fs::path materials_dir = dir.path / "src" / "Assets.assets" / "Materials.materials";
    REQUIRE(fs::exists(materials_dir / "init.json"));
    write_text(materials_dir / "init.json",
               file_of("Materials", "materials", "Materials",
                       ",\n  \"children\": [\"" + b_guid + "\", \"" + a_guid + "\"]"));
    project.apply_disk();
    REQUIRE(game.get_children(materials) == std::vector<InstanceId>{b, a});

    // Editing Assets' own "children" on disk does not reorder the categories:
    // Assets always holds them in table order.
    const fs::path assets_dir = dir.path / "src" / "Assets.assets";
    write_text(assets_dir / "init.json",
               file_of("Assets", "assets", "Assets",
                       ",\n  \"children\": [\"audio\", \"textures\", \"meshes\", \"prefabs\", \"materials\"]"));
    project.apply_disk();
    REQUIRE(child_classes(game, game.service("Assets")) ==
            std::vector<std::string>{"Materials", "Prefabs", "Meshes", "Textures", "Audio"});
}

TEST_CASE("GS17 GameObject.Prefab is nil by default, takes a Prefab, undoes, and Stop restores it", "[GS17]") {
    ScriptRig rig;
    DataModel& game = rig.game;
    const InstanceId statue = make(game, "Prefab", "Statue", game.service("Prefabs"));
    const InstanceId brick = make(game, "Texture", "Brick", game.service("Textures"));
    const InstanceId body = make(game, "GameObject", "Body", game.scene_service("Workspace"));

    // nil by default, and a default saves nothing.
    REQUIRE(read_field(game, body, "Prefab").kind == engine_core::LuaSlot::Kind::Nil);
    engine_core::PropertyBag saved;
    game.instance(body)->save_properties(saved);
    REQUIRE(engine_core::bag_find(saved, "Prefab") == nullptr);

    std::string error;
    REQUIRE_FALSE(write_field(game, body, "Prefab", instance_slot(brick), &error));
    REQUIRE(error == "Prefab must be a Prefab");

    // Close the implicit gesture the three creates opened, so it does not
    // absorb the property change below into the same undo step.
    game.history().end_gesture();
    game.history().set_pending_gesture("Set Prefab");
    REQUIRE(write_field(game, body, "Prefab", instance_slot(statue)));
    game.history().end_gesture();
    const engine_core::LuaSlot read = read_field(game, body, "Prefab");
    REQUIRE(read.kind == engine_core::LuaSlot::Kind::Instance);
    REQUIRE(read.id == statue);
    game.instance(body)->save_properties(saved);
    REQUIRE(engine_core::bag_find(saved, "Prefab")->as_string() == game.guid(statue));

    // A destroyed Prefab reads nil; undo brings it back.
    game.history().set_pending_gesture("Delete");
    game.destroy_tree(statue);
    game.history().end_gesture();
    REQUIRE(read_field(game, body, "Prefab").kind == engine_core::LuaSlot::Kind::Nil);
    game.history().undo();
    REQUIRE(game.alive(statue));
    REQUIRE(read_field(game, body, "Prefab").id == statue);

    // Set during play; Stop restores.
    add_script(game, "Play", R"(
        local body = workspace.Body
        _G.reads = body.Prefab == game.Assets.Prefabs.Statue
        body.Prefab = nil
        _G.cleared = body.Prefab == nil
    )");
    game.start_simulation();
    rig.frames(1, 0.05);
    for (const char* name : {"reads", "cleared"}) {
        bool value = false;
        INFO(name);
        INFO(rig.runtime.last_error());
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }
    game.stop_simulation();
    REQUIRE(read_field(game, body, "Prefab").id == statue);
}

TEST_CASE("GS17b a GameObject's Prefab round-trips through a project", "[GS17b][project]") {
    SimRole role;
    TempDir dir;
    std::string body_guid;
    {
        engine_core::Project project = engine_core::Project::create(dir.path);
        DataModel& game = project.datamodel();
        const InstanceId statue = make(game, "Prefab", "Statue", game.service("Prefabs"));
        const InstanceId body = make(game, "GameObject", "Body", game.scene_service("Workspace"));
        body_guid = game.guid(body);
        REQUIRE(write_field(game, body, "Prefab", instance_slot(statue)));
        project.save();
    }
    engine_core::Project loaded = engine_core::Project::load(dir.path);
    DataModel& game = loaded.datamodel();
    const InstanceId body = *game.find_guid(body_guid);
    const InstanceId statue = game.find_first_child(game.service("Prefabs"), "Statue");
    REQUIRE(statue != 0);
    REQUIRE(read_field(game, body, "Prefab").id == statue);
    REQUIRE_FALSE(loaded.unsaved());
}
