// Game services: Assets and its five categories under game, the asset classes
// they hold, and references between assets.

#include "Containment.hpp"
#include "DataModel.hpp"
#include "Folder.hpp"
#include "Game.hpp"
#include "ChangeHistoryService.hpp"
#include "Contract.hpp"
#include "GameService.hpp"

#include "support.hpp"

#include <catch2/catch_test_macros.hpp>

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
    REQUIRE(reason(placement_error("Model", "Sound", "Boom")) == "A Sound must be in Assets.Audio");
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
