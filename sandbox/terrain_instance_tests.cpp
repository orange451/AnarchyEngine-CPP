// Terrain and TerrainMaterial instances, and the engine rules they rely on.

#include "support.hpp"

#include "DataModel.hpp"
#include "Folder.hpp"
#include "LuaApi.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace {

using engine_core::InstanceId;

// A Folder that keeps its parent, for the locked-parent rule.
class LockedFolder : public engine_core::Folder {
public:
    using Folder::Folder;
    bool parent_locked() const override { return true; }
};

std::string reason(const std::optional<std::string>& value) { return value ? *value : std::string(); }

}  // namespace

TEST_CASE("TE1 a locked parent refuses to change, but can be set from none", "[terrain]") {
    SimRole role;
    engine_core::Game game;
    LockedFolder& locked = game.create<LockedFolder>();
    REQUIRE_FALSE(game.parent_error(locked.id(), workspace_of(game)));
    game.set_parent(locked.id(), workspace_of(game));
    engine_core::Folder& other = game.create<engine_core::Folder>();
    game.set_parent(other.id(), workspace_of(game));
    REQUIRE(reason(game.parent_error(locked.id(), other.id())) == "Folder cannot be reparented");
    REQUIRE(reason(game.parent_error(locked.id(), engine_core::DataModel::kNoParent)) == "Folder cannot be reparented");
    game.destroy(locked.id());
}

TEST_CASE("TE2 a paste-only creatable is made by lua_create_instance but not by scripts", "[terrain]") {
    engine_core::register_lua_creatable(
        "TestPasteOnly", [](engine_core::DataModel& world) -> engine_core::DataModel& {
            return world.create<engine_core::Folder>();
        }, false);
    REQUIRE(engine_core::lua_creatable_known("TestPasteOnly"));
    REQUIRE_FALSE(engine_core::lua_script_creatable("TestPasteOnly"));
    REQUIRE(engine_core::lua_script_creatable("Folder"));
    std::vector<std::string> names;
    engine_core::lua_creatable_names(names);
    REQUIRE(std::find(names.begin(), names.end(), std::string("TestPasteOnly")) == names.end());
}
