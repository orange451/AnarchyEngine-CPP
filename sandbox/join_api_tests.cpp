// Connections:Join, Leave, JoinState, Joined, and Left: the Lua side of
// joining a server, through a fake program host
// (docs/superpowers/specs/2026-10-19-join-from-lua-design.md, "Lua API").

#include "support.hpp"

#include "Connections.hpp"
#include "Folder.hpp"
#include "JoinHost.hpp"
#include "LuaApi.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {

using engine_core::InstanceId;
using engine_core::JoinState;

engine_core::Connections& connections_of(engine_core::DataModel& game) {
    auto* connections = dynamic_cast<engine_core::Connections*>(game.instance(game.connections()));
    REQUIRE(connections != nullptr);
    return *connections;
}

// Records each call. A join reports Joining, as a real host does, unless it
// has a refusal to give.
class FakeJoinHost : public engine_core::IJoinHost {
public:
    explicit FakeJoinHost(engine_core::DataModel& game) : game_(game) {}

    std::string join(const std::string& address) override {
        addresses.push_back(address);
        if (!refusal.empty()) {
            return refusal;
        }
        connections_of(game_).set_join_state(JoinState::Joining);
        return {};
    }

    void leave() override { ++leaves; }

    std::vector<std::string> addresses;
    int leaves = 0;
    std::string refusal;

private:
    engine_core::DataModel& game_;
};

// A line that ends with text: an error keeps the chunk's name and line ahead of the message.
bool has_line_ending(const engine_core::ScriptRuntime::OutputBatch& batch, const std::string& text) {
    for (const auto& line : batch.lines) {
        if (line.text.size() >= text.size() && line.text.compare(line.text.size() - text.size(), text.size(), text) == 0) {
            return true;
        }
    }
    return false;
}

// What Join raises in the console, as one printed line.
std::string join_error(ScriptRig& rig) {
    rig.runtime.drain_output();
    rig.runtime.run_chunk(R"(
local ok, message = pcall(function() game:GetService("Connections"):Join("example:7777") end)
print(ok and "joined" or ("refused: " .. tostring(message)))
)");
    rig.frames(1);
    const engine_core::ScriptRuntime::OutputBatch batch = rig.runtime.drain_output();
    for (const auto& line : batch.lines) {
        if (line.text == "joined\n" || line.text.rfind("refused: ", 0) == 0) {
            return line.text;
        }
    }
    return "nothing printed";
}

bool refused_with(const std::string& line, const std::string& message) {
    return line.rfind("refused: ", 0) == 0 && line.size() >= message.size() + 1 &&
           line.compare(line.size() - message.size() - 1, message.size() + 1, message + "\n") == 0;
}

}  // namespace

TEST_CASE("JN15 Join hands the address to the host and JoinState reads Joining", "[join]") {
    ScriptRig rig;
    FakeJoinHost host(rig.game);
    rig.game.set_join_host(&host);
    rig.game.capture_place();
    rig.game.start_simulation();
    rig.frames(1);
    rig.runtime.drain_output();

    rig.runtime.run_chunk(R"(
local connections = game:GetService("Connections")
print("before", connections.JoinState == Enum.JoinState.Idle)
connections:Join("example:7777")
print("after", connections.JoinState == Enum.JoinState.Joining, connections.JoinState.Name)
)");
    rig.frames(1);
    const engine_core::ScriptRuntime::OutputBatch output = rig.runtime.drain_output();

    REQUIRE(host.addresses == std::vector<std::string>{"example:7777"});
    REQUIRE(connections_of(rig.game).join_state() == JoinState::Joining);
    REQUIRE(has_line_ending(output, "before\ttrue\n"));
    REQUIRE(has_line_ending(output, "after\ttrue\tJoining\n"));
    rig.game.set_join_host(nullptr);
}

TEST_CASE("JN16 Joined and Left reach scripts on the simulation thread", "[join]") {
    ScriptRig rig;
    FakeJoinHost host(rig.game);
    rig.game.set_join_host(&host);
    add_script(rig.game, "Listener",
               "_G.heard = ''\n"
               "local connections = game:GetService('Connections')\n"
               "connections.Joined:Connect(function()\n"
               "    _G.heard ..= 'joined;'\n"
               "end)\n"
               "connections.Left:Connect(function(reason)\n"
               "    _G.heard ..= 'left:' .. reason .. ';'\n"
               "end)\n");
    rig.game.capture_place();
    rig.game.start_simulation();
    rig.frames(1);
    engine_core::Connections& connections = connections_of(rig.game);

    connections.note_joined();
    REQUIRE(connections.join_state() == JoinState::Joined);
    // Queued, not run: handlers wait for the drain.
    std::string heard;
    REQUIRE(rig.runtime.global_string("heard", heard));
    REQUIRE(heard.empty());
    connections.note_left("The server closed");
    rig.game.events().drain();

    REQUIRE(rig.runtime.global_string("heard", heard));
    REQUIRE(heard == "joined;left:The server closed;");
    REQUIRE(connections.join_state() == JoinState::Idle);
    rig.game.set_join_host(nullptr);
}

TEST_CASE("JN17 Join refuses with the exact messages", "[join]") {
    SECTION("no host") {
        ScriptRig rig;
        rig.game.capture_place();
        rig.game.start_simulation();
        REQUIRE(refused_with(join_error(rig), "Only a client can join a server"));
    }
    SECTION("a Server world") {
        ScriptRig rig;
        FakeJoinHost host(rig.game);
        rig.game.set_join_host(&host);
        rig.game.set_world_role(engine_core::WorldRole::Server);
        rig.game.capture_place();
        rig.game.start_simulation();
        REQUIRE(refused_with(join_error(rig), "Join needs a standalone world that is playing"));
        REQUIRE(host.addresses.empty());
        rig.game.set_join_host(nullptr);
    }
    SECTION("a Client world") {
        ScriptRig rig;
        FakeJoinHost host(rig.game);
        rig.game.set_join_host(&host);
        rig.game.set_world_role(engine_core::WorldRole::Client);
        rig.game.capture_place();
        rig.game.start_simulation();
        REQUIRE(refused_with(join_error(rig), "Join needs a standalone world that is playing"));
        REQUIRE(host.addresses.empty());
        rig.game.set_join_host(nullptr);
    }
    SECTION("a stopped world") {
        ScriptRig rig;
        FakeJoinHost host(rig.game);
        rig.game.set_join_host(&host);
        REQUIRE_FALSE(rig.game.simulation_running());
        REQUIRE(refused_with(join_error(rig), "Join needs a standalone world that is playing"));
        REQUIRE(host.addresses.empty());
        rig.game.set_join_host(nullptr);
    }
    SECTION("state Joining") {
        ScriptRig rig;
        FakeJoinHost host(rig.game);
        rig.game.set_join_host(&host);
        rig.game.capture_place();
        rig.game.start_simulation();
        REQUIRE(join_error(rig) == "joined\n");
        REQUIRE(refused_with(join_error(rig), "Already joining or joined; call Leave first"));
        REQUIRE(host.addresses.size() == 1);
        rig.game.set_join_host(nullptr);
    }
    SECTION("the host's own refusal") {
        ScriptRig rig;
        FakeJoinHost host(rig.game);
        host.refusal = "Could not find the server: no such host";
        rig.game.set_join_host(&host);
        rig.game.capture_place();
        rig.game.start_simulation();
        REQUIRE(refused_with(join_error(rig), "Could not find the server: no such host"));
        REQUIRE(connections_of(rig.game).join_state() == JoinState::Idle);
        rig.game.set_join_host(nullptr);
    }
}

TEST_CASE("JN18 Leave calls the host only while joining or joined", "[join]") {
    ScriptRig rig;
    FakeJoinHost host(rig.game);
    rig.game.capture_place();
    rig.game.start_simulation();
    engine_core::Connections& connections = connections_of(rig.game);
    const char* leave = "game:GetService('Connections'):Leave()";

    // No host: nothing to call, and no error.
    rig.runtime.drain_output();
    rig.runtime.run_chunk("print('left', pcall(function() game:GetService('Connections'):Leave() end))");
    rig.frames(1);
    REQUIRE(has_line_ending(rig.runtime.drain_output(), "left\ttrue\n"));

    rig.game.set_join_host(&host);
    rig.runtime.run_chunk(leave);
    REQUIRE(host.leaves == 0);

    connections.set_join_state(JoinState::Joining);
    rig.runtime.run_chunk(leave);
    REQUIRE(host.leaves == 1);

    connections.set_join_state(JoinState::Joined);
    rig.runtime.run_chunk(leave);
    REQUIRE(host.leaves == 2);

    connections.set_join_state(JoinState::Idle);
    rig.runtime.run_chunk(leave);
    REQUIRE(host.leaves == 2);
    rig.game.set_join_host(nullptr);
}

TEST_CASE("JN14 Properties greys Persistent out on a service and on game", "[join][persistent]") {
    ScriptRig rig;
    engine_core::DataModel& game = rig.game;
    const engine_core::LuaField* field = engine_core::lua_class_find("Folder", "Persistent");
    REQUIRE(field != nullptr);
    REQUIRE(field->enabled_when != nullptr);
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_parent(folder.id(), game.scene_service("Workspace"));

    REQUIRE(field->enabled_when(game, folder));
    REQUIRE_FALSE(field->enabled_when(game, *game.instance(game.scene_service("Workspace"))));
    REQUIRE_FALSE(field->enabled_when(game, game));
}
