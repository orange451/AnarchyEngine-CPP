// The Connections service: who is connected, by UUID, and which one is me
// (docs/superpowers/specs/2026-10-13-server-client-design.md, section 1).

#include "support.hpp"

#include "ConnectionInstance.hpp"
#include "Connections.hpp"
#include "Containment.hpp"
#include "Folder.hpp"
#include "Project.hpp"
#include "ScriptAnalysis.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <chrono>
#include <thread>

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
               "_G.no_destroy = not pcall(function() c:Destroy() end)\n"
               "local mine = c.LocalConnection\n"
               "_G.no_conn_destroy = not pcall(function() mine:Destroy() end)\n"
               "_G.no_conn_nil = not pcall(function() mine.Parent = nil end)\n"
               "_G.no_conn_move = not pcall(function() mine.Parent = workspace end)\n"
               "_G.no_conn_clone = not pcall(function() return mine:Clone() end)\n"
               "_G.still_there = mine.Parent == c");
    rig.game.start_simulation();
    rig.frames(1);
    require_globals(rig, {"same_service", "local_ok", "no_set_id", "no_set_local", "no_new", "no_destroy", "no_conn_destroy", "no_conn_nil", "no_conn_move", "no_conn_clone", "still_there"});
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
    REQUIRE(service->local_connection() == 0);
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

namespace {

// Every live instance has one parent and appears once among that parent's children.
void require_consistent_tree(engine_core::DataModel& game) {
    std::vector<engine_core::InstanceId> roots = game.get_children(0);
    std::set<engine_core::InstanceId> seen;
    for (std::size_t at = 0; at < roots.size(); ++at) {
        REQUIRE(seen.insert(roots[at]).second);
        REQUIRE(game.parent(roots[at]) == (at < game.get_children(0).size() ? 0 : game.parent(roots[at])));
        for (engine_core::InstanceId child : game.get_children(roots[at])) {
            REQUIRE(game.parent(child) == roots[at]);
            roots.push_back(child);
        }
    }
}

}  // namespace

TEST_CASE("SC35 what sits under a Connection survives Play and Stop, twice", "[server-client][connections]") {
    ScriptRig rig;
    ConnectionInstance& mine = add_connection(rig.game);
    for (int round = 0; round < 2; ++round) {
        rig.game.start_simulation();
        auto& folder = rig.game.create<engine_core::Folder>();
        rig.game.set_parent(folder.id(), mine.id());
        auto& leaf = rig.game.create<engine_core::Folder>();
        rig.game.set_parent(leaf.id(), folder.id());
        rig.game.stop_simulation();
        REQUIRE(rig.game.parent(folder.id()) == mine.id());
        REQUIRE(rig.game.parent(leaf.id()) == folder.id());
        require_consistent_tree(rig.game);
    }
    REQUIRE(rig.game.get_children(mine.id()).size() == 2);
}

TEST_CASE("SC35b a place instance moved under a Connection returns to the place at Stop", "[server-client][connections]") {
    ScriptRig rig;
    ConnectionInstance& mine = add_connection(rig.game);
    auto& folder = rig.game.create<engine_core::Folder>();
    const engine_core::InstanceId workspace = rig.game.scene_service("Workspace");
    rig.game.set_parent(folder.id(), workspace);
    auto& child = rig.game.create<engine_core::Folder>();
    rig.game.set_parent(child.id(), folder.id());
    rig.game.start_simulation();
    rig.game.set_parent(folder.id(), mine.id());
    auto& made = rig.game.create<engine_core::Folder>();
    rig.game.set_parent(made.id(), folder.id());
    rig.game.stop_simulation();
    REQUIRE(rig.game.parent(folder.id()) == workspace);
    REQUIRE(rig.game.parent(child.id()) == folder.id());
    REQUIRE(rig.game.get_children(mine.id()).empty());
    REQUIRE_FALSE(rig.game.alive(made.id()));
    require_consistent_tree(rig.game);
}

TEST_CASE("SC36 a signal's ScriptConnection and a Connection instance are distinct classes", "[server-client][connections]") {
    ScriptRig rig;
    ConnectionInstance& mine = add_connection(rig.game);
    static_cast<Connections*>(rig.game.instance(rig.game.connections()))->set_local_connection(mine.id());
    add_script(rig.game, rig.game.scene_service("Scripts"), "Probe",
               "local c = game:GetService(\"RunService\").Heartbeat:Connect(function() end)\n"
               "_G.signal_live = c.Connected == true\n"
               "c:Disconnect()\n"
               "_G.signal_dead = c.Connected == false\n"
               "local mine = game.Connections.LocalConnection\n"
               "_G.instance_ok = mine:IsA('Instance') and mine:IsA('Connection') and #mine.Id == 36\n"
               "_G.instance_not_signal = not pcall(function() return mine.Connected end) and not pcall(function() mine:Disconnect() end)\n");
    rig.game.start_simulation();
    rig.frames(1);
    require_globals(rig, {"signal_live", "signal_dead", "instance_ok", "instance_not_signal"});
}

TEST_CASE("SC37 the checker knows ScriptConnection as what Connect returns", "[server-client][connections]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Script& script = add_script(rig.game, "Typed",
                                             "--!strict\n"
                                             "local c: ScriptConnection = game:GetService(\"RunService\").Heartbeat:Connect(function() end)\n"
                                             "c:Disconnect()\n"
                                             "return c.Connected\n");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!analysis.idle() && std::chrono::steady_clock::now() < deadline) {
        analysis.pump();
        std::this_thread::yield();
    }
    const auto diagnostics = analysis.diagnostics(script.id());
    std::string text;
    for (const auto& diagnostic : diagnostics) text += diagnostic.message + "\n";
    INFO(text);
    REQUIRE(diagnostics.empty());
}
