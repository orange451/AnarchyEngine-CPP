// The Connections service: who is connected, by UUID, and which one is me
// (docs/superpowers/specs/2026-10-13-server-client-design.md, section 1).

#include "support.hpp"

#include "ConnectionInstance.hpp"
#include "Connections.hpp"
#include "Containment.hpp"
#include "Project.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

namespace fs = std::filesystem;
using engine_core::ConnectionInstance;
using engine_core::Connections;

namespace {

void require_globals(ScriptRig& rig, std::initializer_list<const char*> names) {
    INFO(rig.runtime.last_error());
    for (const char* name : names) {
        bool value = false;
        INFO(name);
        REQUIRE(rig.runtime.global_boolean(name, value));
        REQUIRE(value);
    }
}

bool is_uuid(const std::string& text) {
    if (text.size() != 36) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? text[i] != '-' : !(std::isxdigit(static_cast<unsigned char>(text[i])) && !std::isupper(static_cast<unsigned char>(text[i])))) {
            return false;
        }
    }
    return true;
}

ConnectionInstance& add_connection(engine_core::DataModel& game) {
    ConnectionInstance& connection = game.create<ConnectionInstance>();
    connection.set_connection_id(engine_core::make_connection_uuid());
    game.set_parent(connection.id(), game.connections());
    return connection;
}

}  // namespace

TEST_CASE("SC28 game holds Connections, hidden while empty and shown once it has a Connection", "[server-client][connections]") {
    SimRole role;
    engine_core::Game game;
    const engine_core::InstanceId id = game.connections();
    REQUIRE(id != 0);
    REQUIRE(game.parent(id) == 0);
    REQUIRE(game.instance(id)->hidden_in_explorer());
    add_connection(game);
    REQUIRE_FALSE(game.instance(id)->hidden_in_explorer());
}

TEST_CASE("SC29 UUIDs are well formed and never repeat", "[server-client][connections]") {
    std::set<std::string> seen;
    for (int i = 0; i < 1000; ++i) {
        const std::string uuid = engine_core::make_connection_uuid();
        REQUIRE(is_uuid(uuid));
        REQUIRE(seen.insert(uuid).second);
    }
}

TEST_CASE("SC30 scripts read Id and LocalConnection but cannot set them or make a Connection", "[server-client][connections]") {
    ScriptRig rig;
    ConnectionInstance& mine = add_connection(rig.game);
    static_cast<Connections*>(rig.game.instance(rig.game.connections()))->set_local_connection(mine.id());
    add_script(rig.game, rig.game.scene_service("Scripts"), "Probe",
               "local c = game:GetService('Connections')\n"
               "_G.same_service = c == game.Connections\n"
               "_G.local_ok = c.LocalConnection ~= nil and #c.LocalConnection.Id == 36\n"
               "_G.no_set_id = not pcall(function() c.LocalConnection.Id = 'x' end)\n"
               "_G.no_set_local = not pcall(function() c.LocalConnection = nil end)\n"
               "_G.no_new = not pcall(function() Instance.new('Connection') end)\n"
               "_G.no_destroy = not pcall(function() c:Destroy() end)");
    rig.game.start_simulation();
    rig.frames(1);
    require_globals(rig, {"same_service", "local_ok", "no_set_id", "no_set_local", "no_new", "no_destroy"});
}

TEST_CASE("SC31 Connections and its Connections never save", "[server-client][connections][project]") {
    SimRole role;
    TempDir dir;
    const fs::path root = dir.path / "Place";
    {
        engine_core::Project project = engine_core::Project::create(root);
        add_connection(project.datamodel());
        project.save();
    }
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        REQUIRE(text.find("\"Connection") == std::string::npos);
    }
    engine_core::Project loaded = engine_core::Project::load(root);
    REQUIRE(loaded.datamodel().first_child(loaded.datamodel().connections()) == 0);
}

TEST_CASE("SC32 only a Connection goes in Connections, and a Connection goes only there", "[server-client][connections]") {
    SimRole role;
    engine_core::Game game;
    REQUIRE(engine_core::placement_error("Connections", "Folder", "F").has_value());
    REQUIRE_FALSE(engine_core::placement_error("Connections", "Connection", "C").has_value());
    REQUIRE(engine_core::placement_error("Workspace", "Connection", "C").has_value());
    REQUIRE_FALSE(engine_core::placement_error("Game", "Connections", "Connections").has_value());
}

TEST_CASE("SC33 LocalConnection reads nil when its Connection is destroyed", "[server-client][connections]") {
    SimRole role;
    engine_core::Game game;
    ConnectionInstance& mine = add_connection(game);
    auto* service = static_cast<Connections*>(game.instance(game.connections()));
    service->set_local_connection(mine.id());
    game.destroy(mine.id());
    REQUIRE(game.instance(service->local_connection()) == nullptr);
}

TEST_CASE("SC34 Play and Stop leave Connections as they are", "[server-client][connections]") {
    ScriptRig rig;
    ConnectionInstance& mine = add_connection(rig.game);
    const engine_core::InstanceId id = mine.id();
    const std::string uuid = mine.connection_id();
    rig.game.start_simulation();
    // One that joins during play is not the place's either.
    ConnectionInstance& joined = add_connection(rig.game);
    const engine_core::InstanceId joined_id = joined.id();
    rig.game.stop_simulation();
    auto* kept = dynamic_cast<ConnectionInstance*>(rig.game.instance(id));
    REQUIRE(kept != nullptr);
    REQUIRE(kept->connection_id() == uuid);
    REQUIRE(rig.game.parent(id) == rig.game.connections());
    REQUIRE(rig.game.instance(joined_id) != nullptr);
    REQUIRE(rig.game.parent(joined_id) == rig.game.connections());
}
