// Game services: Assets and its five categories under game, the asset classes
// they hold, and references between assets.

#include "Containment.hpp"

#include "support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

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
