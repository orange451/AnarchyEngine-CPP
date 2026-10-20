// RemoteEvent in a standalone world: the class, its payloads, and loopback
// through the event queue (docs/superpowers/specs/2026-10-20-remote-events-design.md).

#include "support.hpp"

#include "Project.hpp"
#include "RemoteEvent.hpp"
#include "ScriptAnalysis.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <initializer_list>
#include <string>
#include <thread>
#include <vector>

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

// Runs source as a Script under Workspace in a playing standalone world for frames.
void play(ScriptRig& rig, const char* source, int frames = 2) {
    add_script(rig.game, "Probe", source);
    rig.game.start_simulation();
    rig.frames(frames);
}

std::vector<std::string> diagnostics_of(ScriptRig& rig, const char* source) {
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Script& script = add_script(rig.game, "Typed", source);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!analysis.idle() && std::chrono::steady_clock::now() < deadline) {
        analysis.pump();
        std::this_thread::yield();
    }
    std::vector<std::string> out;
    for (const auto& diagnostic : analysis.diagnostics(script.id())) {
        out.push_back(diagnostic.message);
    }
    return out;
}

}  // namespace

TEST_CASE("RE1 RemoteEvent is creatable and Unreliable is saved", "[remote]") {
    {
        ScriptRig rig;
        play(rig,
             "local r = Instance.new('RemoteEvent')\n"
             "_G.made = r:IsA('RemoteEvent')\n"
             "_G.reliable = r.Unreliable == false\n"
             "r.Unreliable = true\n"
             "_G.set = r.Unreliable == true\n");
        require_globals(rig, {"made", "reliable", "set"});
    }

    SimRole role;
    TempDir dir;
    engine_core::Game game;
    engine_core::Project project = engine_core::Project::create(dir.path, game);
    auto& remote = game.create<engine_core::RemoteEvent>();
    game.set_name(remote.id(), "Shout");
    game.set_parent(remote.id(), game.scene_service("Workspace"));
    REQUIRE_FALSE(remote.unreliable());
    remote.set_unreliable(true);
    auto& plain = game.create<engine_core::RemoteEvent>();
    game.set_name(plain.id(), "Plain");
    game.set_parent(plain.id(), game.scene_service("Workspace"));
    project.save();

    engine_core::Game other;
    engine_core::Project read = engine_core::Project::load(dir.path, other);
    const engine_core::InstanceId loaded = other.find_first_child(other.scene_service("Workspace"), "Shout");
    REQUIRE(loaded != 0);
    const auto* back = dynamic_cast<const engine_core::RemoteEvent*>(other.instance(loaded));
    REQUIRE(back != nullptr);
    REQUIRE(back->unreliable());
    const auto* plain_back = dynamic_cast<const engine_core::RemoteEvent*>(
        other.instance(other.find_first_child(other.scene_service("Workspace"), "Plain")));
    REQUIRE(plain_back != nullptr);
    REQUIRE_FALSE(plain_back->unreliable());
}

TEST_CASE("RE2 in a standalone world FireServer reaches OnServerEvent with a nil connection", "[remote]") {
    ScriptRig rig;
    play(rig,
         "local r = Instance.new('RemoteEvent')\n"
         "r.Parent = workspace\n"
         "r.OnServerEvent:Connect(function(c, a, b)\n"
         "    _G.got = {c == nil, a, b}\n"
         "    _G.ok = _G.got[1] == true and _G.got[2] == 5 and _G.got[3] == 'x'\n"
         "end)\n"
         "r:FireServer(5, 'x')\n",
         3);
    require_globals(rig, {"ok"});
}

TEST_CASE("RE3 the argument count survives, trailing nils included", "[remote]") {
    ScriptRig rig;
    play(rig,
         "local r = Instance.new('RemoteEvent')\n"
         "local counts = {}\n"
         "r.OnServerEvent:Connect(function(c, ...)\n"
         "    table.insert(counts, select('#', ...))\n"
         "    _G.counts = table.concat(counts, ',')\n"
         "end)\n"
         "r:FireServer(1, nil)\n"
         "r:FireServer()\n"
         "r:FireServer(nil, nil, nil)\n",
         3);
    std::string counts;
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.global_string("counts", counts));
    REQUIRE(counts == "2,0,3");
}

TEST_CASE("RE4 FireClientAll and FireClient loop back to OnClientEvent once per call", "[remote]") {
    ScriptRig rig;
    play(rig,
         "local r = Instance.new('RemoteEvent')\n"
         "local seen = {}\n"
         "r.OnClientEvent:Connect(function(...)\n"
         "    table.insert(seen, select('#', ...) .. ':' .. tostring((...)))\n"
         "    _G.seen = table.concat(seen, ',')\n"
         "end)\n"
         "r:FireClientAll(1)\n"
         "r:FireClient(nil, 2)\n"
         "r:FireClientExt({}, 3)\n",
         3);
    std::string seen;
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.global_string("seen", seen));
    REQUIRE(seen == "1:1,1:2,1:3");
}

TEST_CASE("RE5 tables, engine values and instances arrive intact", "[remote]") {
    ScriptRig rig;
    play(rig,
         "local r = Instance.new('RemoteEvent')\n"
         "local sent = {1, 2, {nested = Vector3.new(1, 2, 3)}, name = 'n', [true] = 1}\n"
         "r.OnServerEvent:Connect(function(c, t, color, inst, item)\n"
         "    _G.array = t[1] == 1 and t[2] == 2 and #t == 3\n"
         "    _G.nested = t[3].nested == Vector3.new(1, 2, 3)\n"
         "    _G.name = t.name == 'n'\n"
         "    local keys = 0\n"
         "    for _ in pairs(t) do keys += 1 end\n"
         "    _G.dropped = t[true] == nil and keys == 4\n"
         "    _G.copied = t ~= sent\n"
         "    _G.color = color == Color3.new(1, 0, 0)\n"
         "    _G.inst = inst == workspace\n"
         "    _G.item = item == Enum.KeyCode.A\n"
         "end)\n"
         "r:FireServer(sent, Color3.new(1, 0, 0), workspace, Enum.KeyCode.A)\n",
         3);
    require_globals(rig, {"array", "nested", "name", "dropped", "copied", "color", "inst", "item"});
}

TEST_CASE("RE6 unsendable values raise on the sender", "[remote]") {
    ScriptRig rig;
    play(rig,
         "local r = Instance.new('RemoteEvent')\n"
         "local function raises(expected, ...)\n"
         "    local ok, err = pcall(r.FireServer, r, ...)\n"
         "    return not ok and string.find(tostring(err), expected, 1, true) ~= nil\n"
         "end\n"
         "_G.fn = raises('FireServer cannot send a function', function() end)\n"
         "_G.thread = raises('FireServer cannot send a thread', coroutine.create(function() end))\n"
         "_G.nested_fn = raises('FireServer cannot send a function', {a = {print}})\n"
         "local s = {}\n"
         "s.me = s\n"
         "_G.cycle = raises('FireServer cannot send a table that contains itself', s)\n"
         "local function nest(levels)\n"
         "    local t = {}\n"
         "    local cur = t\n"
         "    for i = 2, levels do local n = {} cur[1] = n cur = n end\n"
         "    return t\n"
         "end\n"
         "_G.deep = raises('FireServer tables nest deeper than 32', nest(33))\n"
         "_G.just_deep_enough = pcall(r.FireServer, r, nest(32))\n"
         "local shared = {1}\n"
         "_G.shared_twice = pcall(r.FireServer, r, {shared, shared})\n");
    require_globals(rig, {"fn", "thread", "nested_fn", "cycle", "deep", "just_deep_enough", "shared_twice"});
}

TEST_CASE("RE7 delivery waits for the event queue", "[remote]") {
    ScriptRig rig;
    play(rig,
         "local r = Instance.new('RemoteEvent')\n"
         "r.OnServerEvent:Connect(function() _G.ran = true _G.not_inline = _G.firing == false end)\n"
         "_G.firing = true\n"
         "r:FireServer()\n"
         "_G.firing = false\n"
         "_G.before = _G.ran == nil\n"
         "task.wait()\n",
         3);
    // Not inside the call: the handler ran after the script went on past it.
    require_globals(rig, {"before", "ran", "not_inline"});
}

TEST_CASE("RE8 the wrong side's method and a world with no session raise", "[remote]") {
    ScriptRig rig;
    rig.game.set_world_role(engine_core::WorldRole::Server);
    play(rig,
         "local r = Instance.new('RemoteEvent')\n"
         "local function says(expected, f)\n"
         "    local ok, err = pcall(f)\n"
         "    return not ok and string.find(tostring(err), expected, 1, true) ~= nil\n"
         "end\n"
         "_G.server = says('FireServer is for clients; the server sends with FireClient, FireClientExt or "
         "FireClientAll', function() r:FireServer(1) end)\n"
         "_G.no_conn = says('FireClient needs a Connection of this server', function() r:FireClient(nil, 1) end)\n"
         "_G.not_conn = says('FireClient needs a Connection of this server', function() r:FireClient(workspace, 1) end)\n"
         "_G.no_sink = says('Only a networked world can send this', function() r:FireClientAll(1) end)\n");
    require_globals(rig, {"server", "no_conn", "not_conn", "no_sink"});

    ScriptRig client;
    client.game.set_world_role(engine_core::WorldRole::Client);
    // A client world runs only Client and Any scripts.
    REQUIRE_FALSE(add_script(client.game, "Probe",
         "local r = Instance.new('RemoteEvent')\n"
         "local function says(expected, f)\n"
         "    local ok, err = pcall(f)\n"
         "    return not ok and string.find(tostring(err), expected, 1, true) ~= nil\n"
         "end\n"
         "_G.c1 = says('FireClient is for the server; a client sends with FireServer', function() r:FireClient(nil) end)\n"
         "_G.c2 = says('FireClientExt is for the server; a client sends with FireServer', function() r:FireClientExt({}) end)\n"
         "_G.c3 = says('FireClientAll is for the server; a client sends with FireServer', function() r:FireClientAll() end)\n"
         "_G.c4 = says('Only a networked world can send this', function() r:FireServer() end)\n")
                      .set_run_context(static_cast<int>(engine_core::RunContext::Client)));
    client.game.start_simulation();
    client.frames(2);
    require_globals(client, {"c1", "c2", "c3", "c4"});
}

TEST_CASE("RE9 the checker types OnServerEvent's connection and every method", "[remote]") {
    ScriptRig rig;
    const std::vector<std::string> clean = diagnostics_of(rig,
                                                          "--!strict\n"
                                                          "local r = Instance.new('RemoteEvent')\n"
                                                          "r.OnServerEvent:Connect(function(connection, ...)\n"
                                                          "    local _id: string = connection.Id\n"
                                                          "    local _n: number = select('#', ...)\n"
                                                          "end)\n"
                                                          "r.OnClientEvent:Connect(function(a, b) end)\n"
                                                          "r:FireServer(1, nil)\n"
                                                          "r:FireClient(nil, 2)\n"
                                                          "r:FireClientExt({}, 3)\n"
                                                          "r:FireClientAll()\n"
                                                          "r.Unreliable = true\n");
    std::string text;
    for (const std::string& message : clean) text += message + "\n";
    INFO(text);
    REQUIRE(clean.empty());

    ScriptRig wrong;
    const std::vector<std::string> typed = diagnostics_of(wrong,
                                                          "--!strict\n"
                                                          "local r = Instance.new('RemoteEvent')\n"
                                                          "r.OnServerEvent:Connect(function(connection, ...)\n"
                                                          "    local _n: number = connection\n"
                                                          "end)\n");
    std::string typed_text;
    for (const std::string& message : typed) typed_text += message + "\n";
    INFO(typed_text);
    REQUIRE(typed_text.find("Connection") != std::string::npos);
}
