// A client's world before it connects, and the identity a project presents to
// the network: what AnarchyServer and AnarchyPlayer start from
// (docs/superpowers/specs/2026-10-13-server-client-design.md, section 2).

#include "support.hpp"

#include "ClientWorld.hpp"
#include "Folder.hpp"
#include "GameIdentity.hpp"
#include "GameObject.hpp"
#include "Lighting.hpp"
#include "Project.hpp"
#include "ProjectIdentity.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using engine_core::DataModel;
using engine_core::InstanceId;

namespace {

// Every instance under id, id first, but nothing in Core.
void collect(const DataModel& game, InstanceId id, std::vector<InstanceId>& out) {
    out.push_back(id);
    for (const InstanceId child : game.get_children(id)) {
        if (child != game.core()) collect(game, child, out);
    }
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

}  // namespace

TEST_CASE("RP54 clear_for_client leaves only services", "[replication][session]") {
    ScriptRig rig;
    DataModel& game = rig.game;
    const InstanceId workspace = game.scene_service("Workspace");
    engine_core::Folder& level = game.create<engine_core::Folder>();
    game.set_parent(level.id(), workspace);
    engine_core::Folder& inner = game.create<engine_core::Folder>();
    game.set_parent(inner.id(), level.id());
    engine_core::GameObject& prop = game.create<engine_core::GameObject>();
    game.set_parent(prop.id(), game.service("ClientStorage"));
    engine_core::Folder& secrets = game.create<engine_core::Folder>();
    game.set_parent(secrets.id(), game.service("ServerStorage"));
    add_script(game, "Main", "print('hi')");
    auto* lighting = static_cast<engine_core::Lighting*>(game.instance(game.service("Lighting")));
    REQUIRE_FALSE(lighting->set_exposure(2.5));

    std::vector<InstanceId> everything;
    collect(game, 0, everything);
    std::vector<InstanceId> services;
    for (const InstanceId id : everything) {
        if (id != 0 && game.instance(id)->is_service()) services.push_back(id);
    }
    REQUIRE(services.size() > 5);
    const std::size_t core_children = game.get_children(game.core()).size();

    engine_network::clear_for_client(game);

    for (const InstanceId id : services) REQUIRE(game.alive(id));
    REQUIRE(static_cast<engine_core::Lighting*>(game.instance(game.service("Lighting")))->exposure() == 2.5);
    std::vector<InstanceId> after;
    collect(game, 0, after);
    for (const InstanceId id : after) {
        if (id == 0) continue;
        INFO(game.instance(id)->class_name());
        REQUIRE(game.instance(id)->is_service());
    }
    REQUIRE(game.get_children(game.core()).size() == core_children);
    REQUIRE_FALSE(game.alive(level.id()));
    REQUIRE_FALSE(game.alive(inner.id()));
    REQUIRE_FALSE(game.alive(prop.id()));
    REQUIRE_FALSE(game.alive(secrets.id()));
}

TEST_CASE("RP55 project_identity refuses what cannot identify a game", "[replication][session]") {
    SimRole role;
    TempDir dir;
    network::GameIdentity identity;
    std::string error;
    {
        engine_core::Project project = engine_core::Project::create(dir.path);
        REQUIRE(engine_network::project_identity(project, identity, error));
        INFO(error);
        REQUIRE(error.empty());
        REQUIRE(network::format_game_id(identity.game_id) == project.game_id());
        REQUIRE(identity.version == project.version());
        REQUIRE(identity.resources_hash != 0);
        REQUIRE(identity.resources_hash == network::hash_resources(project.resources_root()));
    }
    SECTION("a gameId of all zeros") {
        const std::filesystem::path json = dir.path / "project.json";
        std::string text = read_text(json);
        const std::size_t key = text.find("\"gameId\"");
        REQUIRE(key != std::string::npos);
        const std::size_t open = text.find('"', text.find(':', key));
        const std::size_t close = text.find('"', open + 1);
        REQUIRE(close - open - 1 == 32);
        text.replace(open + 1, 32, std::string(32, '0'));
        write_text(json, text);
        engine_core::Project project = engine_core::Project::load(dir.path);
        REQUIRE(project.game_id() == std::string(32, '0'));
        error.clear();
        REQUIRE_FALSE(engine_network::project_identity(project, identity, error));
        INFO(error);
        REQUIRE(error.find("gameId") != std::string::npos);
    }
    SECTION("no resources folder") {
        engine_core::Project project = engine_core::Project::load(dir.path);
        std::filesystem::remove_all(project.resources_root());
        error.clear();
        REQUIRE_FALSE(engine_network::project_identity(project, identity, error));
        INFO(error);
        REQUIRE(error.find("resources") != std::string::npos);
    }
}
