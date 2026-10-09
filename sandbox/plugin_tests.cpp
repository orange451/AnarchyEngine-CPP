// Plugins, whose Scripts run in the plugin VM, and the scheduler the console and plugin
// VMs share with the play VM: waits, tasks, and connections that go on while stopped.

#include "support.hpp"

#include "Engine.hpp"
#include "Folder.hpp"
#include "ModuleScript.hpp"
#include "ScriptRuntime.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace {

using engine_core::InstanceId;
using engine_core::ScriptRuntime;

// A Folder with no parent, where a plugin usually sits.
InstanceId add_folder(engine_core::DataModel& game, const char* name,
                      InstanceId parent = engine_core::DataModel::kNoParent) {
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_name(folder.id(), name);
    if (parent != engine_core::DataModel::kNoParent) {
        game.set_parent(folder.id(), parent);
    }
    return folder.id();
}

std::vector<std::string> texts(const ScriptRuntime::OutputBatch& batch) {
    std::vector<std::string> out;
    for (const ScriptRuntime::OutputLine& line : batch.lines) {
        out.push_back(line.text);
    }
    return out;
}

}  // namespace

TEST_CASE("PL1 registering a plugin runs its root and every Script under it", "[PL1]") {
    ScriptRig rig;
    engine_core::Script& root = rig.game.create<engine_core::Script>();
    rig.game.set_name(root.id(), "Root");
    root.set_source("print('root', script.Name, script.Parent == nil)");
    const InstanceId tools = add_folder(rig.game, "Tools", root.id());
    add_script(rig.game, tools, "First", "print('first', script.Parent.Name)");
    engine_core::Script& off = add_script(rig.game, tools, "Off", "print('off')");
    off.set_enabled(false);
    engine_core::ModuleScript& module = rig.game.create<engine_core::ModuleScript>();
    rig.game.set_name(module.id(), "Lib");
    module.set_source("print('module ran alone') return 1");
    rig.game.set_parent(module.id(), tools);
    engine_core::Script& second = add_script(rig.game, root.id(), "Second",
                                             "local f = Instance.new('Folder')\n"
                                             "f.Name = 'MadeByPlugin'\n"
                                             "f.Parent = workspace\n"
                                             "print('second')");
    rig.runtime.drain_output();

    REQUIRE(rig.runtime.register_plugin(root.id()));
    REQUIRE_FALSE(rig.runtime.vm_open());
    REQUIRE(rig.runtime.plugin_vm_open());
    const ScriptRuntime::OutputBatch batch = rig.runtime.drain_output();
    REQUIRE(texts(batch) == std::vector<std::string>{"root\tRoot\ttrue\n", "first\tTools\n", "second\n"});
    // Each print names the plugin Script that made it.
    REQUIRE(batch.lines[0].script == root.id());
    REQUIRE(batch.lines[2].script == second.id());
    REQUIRE(batch.lines[2].line == 4);
    // The plugin edits the live data model.
    REQUIRE(rig.game.find_first_child(workspace_of(rig.game), "MadeByPlugin") != 0);

    REQUIRE(rig.runtime.is_plugin(root.id()));
    REQUIRE(rig.runtime.plugins() == std::vector<InstanceId>{root.id()});
}

TEST_CASE("PL2 a plugin registers once, and only a live instance registers", "[PL2]") {
    ScriptRig rig;
    const InstanceId root = add_folder(rig.game, "Plugin");
    add_script(rig.game, root, "Main", "print('ran')");
    rig.runtime.drain_output();

    REQUIRE(rig.runtime.register_plugin(root));
    REQUIRE_FALSE(rig.runtime.register_plugin(root));
    REQUIRE(rig.runtime.drain_output().lines.size() == 1);

    REQUIRE_FALSE(rig.runtime.unregister_plugin(root + 1000));
    REQUIRE(rig.runtime.unregister_plugin(root));
    REQUIRE_FALSE(rig.runtime.unregister_plugin(root));
    REQUIRE_FALSE(rig.runtime.is_plugin(root));
    REQUIRE_FALSE(rig.runtime.plugin_vm_open());

    // Registering again runs it again.
    REQUIRE(rig.runtime.register_plugin(root));
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"ran\n"});

    const InstanceId gone = add_folder(rig.game, "Gone");
    rig.game.destroy(gone);
    REQUIRE_FALSE(rig.runtime.register_plugin(gone));
}

TEST_CASE("PL3 plugins share the plugin VM, apart from the console, until the last one goes", "[PL3]") {
    ScriptRig rig;
    const InstanceId a = add_folder(rig.game, "A");
    add_script(rig.game, a, "SetA", "_G.fromA = 'a'");
    const InstanceId b = add_folder(rig.game, "B");
    add_script(rig.game, b, "ReadB", "print(_G.fromA)");
    rig.runtime.drain_output();

    REQUIRE(rig.runtime.register_plugin(a));
    REQUIRE(rig.runtime.register_plugin(b));
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"a\n"});
    REQUIRE(rig.runtime.plugins() == std::vector<InstanceId>{a, b});

    rig.runtime.run_chunk("print(_G.fromA)");
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"nil\n"});

    REQUIRE(rig.runtime.unregister_plugin(a));
    REQUIRE(rig.runtime.plugin_vm_open());
    REQUIRE(rig.runtime.unregister_plugin(b));
    REQUIRE_FALSE(rig.runtime.plugin_vm_open());

    // A new plugin VM starts clean.
    REQUIRE(rig.runtime.register_plugin(b));
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"nil\n"});
}

TEST_CASE("PL4 a plugin Script may wait, yield, and connect, and its failure does not stop the next", "[PL4]") {
    ScriptRig rig;
    const InstanceId root = add_folder(rig.game, "Plugin");
    add_script(rig.game, root, "Waits", "print('w1')\ntask.wait(1)\nprint('w2')");
    add_script(rig.game, root, "Yields", "coroutine.yield()\nprint('y2')");
    add_script(rig.game, root, "Throws", "error('boom')");
    add_script(rig.game, root, "Broken", "print(");
    add_script(rig.game, root, "Connects",
               "local n = 0\n"
               "game:GetService('RunService').Heartbeat:Connect(function(dt)\n"
               "    n += 1\n"
               "    if n == 1 then print('beat', dt) end\n"
               "end)");
    add_script(rig.game, root, "Last", "print('last')");
    rig.runtime.drain_output();

    // Each Script's first run is now, in order, until it finishes or yields.
    REQUIRE(rig.runtime.register_plugin(root));
    ScriptRuntime::OutputBatch batch = rig.runtime.drain_output();
    REQUIRE(batch.lines.size() == 4);
    REQUIRE(batch.lines[0].text == "w1\n");
    REQUIRE(batch.lines[1].kind == ScriptRuntime::OutputKind::Error);
    REQUIRE(batch.lines[1].text.find("Throws:1:") != std::string::npos);
    REQUIRE(batch.lines[1].text.find("boom") != std::string::npos);
    REQUIRE(batch.lines[2].kind == ScriptRuntime::OutputKind::Error);
    REQUIRE(batch.lines[2].text.rfind("Broken:1:", 0) == 0);
    REQUIRE(batch.lines[3].text == "last\n");

    // The play session is closed, so step_tools fires Heartbeat itself.
    rig.runtime.step_tools(0.5);
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"beat\t0.5\n", "y2\n"});
    rig.runtime.step_tools(0.6);
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"w2\n"});
}

TEST_CASE("PL5 a plugin requires a ModuleScript once per run", "[PL5]") {
    ScriptRig rig;
    const InstanceId root = add_folder(rig.game, "Plugin");
    engine_core::ModuleScript& module = rig.game.create<engine_core::ModuleScript>();
    rig.game.set_name(module.id(), "Config");
    module.set_source("return { count = 0 }");
    rig.game.set_parent(module.id(), root);
    add_script(rig.game, root, "One", "local c = require(script.Parent.Config) c.count += 1 print(c.count)");
    add_script(rig.game, root, "Two", "local c = require(script.Parent.Config) c.count += 1 print(c.count)");
    rig.runtime.drain_output();

    REQUIRE(rig.runtime.register_plugin(root));
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"1\n", "2\n"});

    // The next registration reads the module again.
    module.set_source("return { count = 10 }");
    REQUIRE(rig.runtime.unregister_plugin(root));
    REQUIRE(rig.runtime.register_plugin(root));
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"11\n", "12\n"});
}

TEST_CASE("PL6 plugins run during play and stay registered after it", "[PL6]") {
    ScriptRig rig;
    rig.game.set_name(0, "Place");
    const InstanceId first = add_folder(rig.game, "First", rig.game.scene_service("Storage"));
    add_script(rig.game, first, "Tag", "_G.seen = (_G.seen or 0) + 1");
    const InstanceId second = add_folder(rig.game, "Second", rig.game.scene_service("Storage"));
    add_script(rig.game, second, "Report", "print(game.Name, _G.seen)");

    rig.game.start_simulation();
    rig.runtime.drain_output();
    REQUIRE(rig.runtime.register_plugin(first));
    REQUIRE(rig.runtime.vm_open());
    rig.game.stop_simulation();
    REQUIRE_FALSE(rig.runtime.vm_open());

    // The place came back with the same ids, and the plugin VM kept its globals.
    REQUIRE(rig.runtime.is_plugin(first));
    REQUIRE(rig.runtime.plugin_vm_open());
    rig.runtime.drain_output();
    REQUIRE(rig.runtime.register_plugin(second));
    const ScriptRuntime::OutputBatch batch = rig.runtime.drain_output();
    REQUIRE(texts(batch) == std::vector<std::string>{"Place\t1\n"});
}

TEST_CASE("PL7 a plugin's loop goes on across a play session and stops when it is unregistered", "[PL7]") {
    ScriptRig rig;
    const InstanceId root = add_folder(rig.game, "Ticker", rig.game.scene_service("Storage"));
    add_script(rig.game, root, "Loop", "while true do task.wait(1) print('tick') end");
    rig.runtime.drain_output();

    REQUIRE(rig.runtime.register_plugin(root));
    rig.runtime.step_tools(1.0);
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"tick\n"});

    // Play steps it too.
    rig.game.start_simulation();
    rig.frames(2, 0.6);
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"tick\n"});
    rig.game.stop_simulation();
    rig.runtime.step_tools(1.0);
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"tick\n"});

    REQUIRE(rig.runtime.unregister_plugin(root));
    REQUIRE_FALSE(rig.runtime.plugin_vm_open());
    REQUIRE_FALSE(rig.runtime.tools_open());
    rig.runtime.step_tools(5.0);
    REQUIRE(rig.runtime.drain_output().lines.empty());
}

TEST_CASE("PL8 unregistering one plugin stops only its threads and connections", "[PL8]") {
    ScriptRig rig;
    const InstanceId a = add_folder(rig.game, "A");
    add_script(rig.game, a, "Beat", "game:GetService('RunService').Heartbeat:Connect(function() print('A') end)");
    const InstanceId b = add_folder(rig.game, "B");
    add_script(rig.game, b, "Beat", "game:GetService('RunService').Heartbeat:Connect(function() print('B') end)");
    add_script(rig.game, b, "Loop", "while true do task.wait(1) print('B loop') end");

    REQUIRE(rig.runtime.register_plugin(a));
    REQUIRE(rig.runtime.register_plugin(b));
    rig.runtime.step_tools(1.0);
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"A\n", "B\n", "B loop\n"});

    REQUIRE(rig.runtime.unregister_plugin(a));
    rig.runtime.step_tools(1.0);
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"B\n", "B loop\n"});
}

TEST_CASE("PL9 a plugin Script that is destroyed stops", "[PL9]") {
    ScriptRig rig;
    const InstanceId root = add_folder(rig.game, "Plugin");
    engine_core::Script& loop = add_script(rig.game, root, "Loop", "while true do task.wait(1) print('alive') end");
    add_script(rig.game, root, "Other", "while true do task.wait(1) print('other') end");
    REQUIRE(rig.runtime.register_plugin(root));
    rig.runtime.step_tools(1.0);
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"alive\n", "other\n"});

    rig.game.destroy(loop.id());
    rig.runtime.step_tools(1.0);
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"other\n"});
    REQUIRE(rig.runtime.is_plugin(root));
}

TEST_CASE("PL10 a plugin's handles and connections outlive a play session", "[PL10]") {
    ScriptRig rig;
    const InstanceId root = add_folder(rig.game, "Watcher", rig.game.scene_service("Storage"));
    add_script(rig.game, root, "Watch",
               "local folder = script.Parent\n"
               "folder.Changed:Connect(function(property) print(property, folder.Name) end)");
    REQUIRE(rig.runtime.register_plugin(root));
    rig.runtime.drain_output();

    rig.game.set_name(root, "Renamed");
    rig.runtime.step_tools(0.1);
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"Name\tRenamed\n"});

    rig.game.start_simulation();
    rig.frames(1);
    rig.game.stop_simulation();
    rig.runtime.drain_output();

    rig.game.set_name(root, "Again");
    rig.runtime.step_tools(0.1);
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"Name\tAgain\n"});
}

TEST_CASE("CV1 a command waits while the play session is closed", "[CV1]") {
    ScriptRig rig;
    rig.runtime.run_chunk("print('a') task.wait(0.5) print('b')");
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"a\n"});
    REQUIRE(rig.runtime.tools_open());
    rig.runtime.step_tools(0.25);
    REQUIRE(rig.runtime.drain_output().lines.empty());
    rig.runtime.step_tools(0.3);
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"b\n"});
}

TEST_CASE("CV2 the command line spawns, defers, and delays", "[CV2]") {
    ScriptRig rig;
    rig.runtime.run_chunk("task.spawn(print, 's') task.defer(print, 'd') task.delay(0.2, print, 'l') print('end')");
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"end\n", "s\n"});
    rig.runtime.step_tools(0.1);
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"d\n"});
    rig.runtime.step_tools(0.15);
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"l\n"});
}

TEST_CASE("CV3 a waiting command does not hold up the next, and reset_console stops it", "[CV3]") {
    ScriptRig rig;
    rig.runtime.run_chunk("while true do task.wait(1) print('loop') end");
    rig.runtime.run_chunk("print('next')");
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"next\n"});
    rig.runtime.step_tools(1.0);
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"loop\n"});

    rig.runtime.reset_console();
    REQUIRE_FALSE(rig.runtime.tools_open());
    rig.runtime.step_tools(2.0);
    REQUIRE(rig.runtime.drain_output().lines.empty());
    rig.runtime.run_chunk("print(1)");
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"1\n"});
}

TEST_CASE("CV4 a command's connection fires while stopped and outlives a play session", "[CV4]") {
    ScriptRig rig;
    const InstanceId folder = add_folder(rig.game, "Box", workspace_of(rig.game));
    rig.runtime.run_chunk("_G.box = workspace.Box\n"
                          "_G.connection = _G.box.Changed:Connect(function(p) print(p, _G.box.Name) end)");
    rig.game.set_name(folder, "Crate");
    rig.runtime.step_tools(0.1);
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"Name\tCrate\n"});

    rig.game.start_simulation();
    rig.frames(1);
    rig.game.stop_simulation();
    rig.runtime.drain_output();

    rig.runtime.run_chunk("print(_G.connection.Connected, _G.box == workspace.Crate)");
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"true\ttrue\n"});
    rig.game.set_name(folder, "Chest");
    rig.runtime.step_tools(0.1);
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"Name\tChest\n"});

    rig.runtime.run_chunk("_G.connection:Disconnect()");
    rig.game.set_name(folder, "Trunk");
    rig.runtime.step_tools(0.1);
    REQUIRE(rig.runtime.drain_output().lines.empty());
}

TEST_CASE("CV5 a runaway command is still stopped", "[CV5]") {
    ScriptRig rig;
    rig.runtime.run_chunk("while true do end");
    const ScriptRuntime::OutputBatch batch = rig.runtime.drain_output();
    REQUIRE(batch.lines.size() == 1);
    REQUIRE(batch.lines[0].kind == ScriptRuntime::OutputKind::Error);
    REQUIRE(batch.lines[0].text.find("ScriptTimeout") != std::string::npos);
}

TEST_CASE("CV6 a paused engine keeps the command line's time", "[CV6]") {
    engine_core::Engine engine;
    engine.start();
    REQUIRE(engine.paused());
    engine.on_simulation([&](engine_core::DataModel&) { engine.scripts().run_chunk("task.wait(0.05) print('woke')"); });
    bool woke = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!woke && std::chrono::steady_clock::now() < deadline) {
        for (const ScriptRuntime::OutputLine& line : engine.scripts().output_since(0, 64).lines) {
            woke = woke || line.text == "woke\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    engine.stop();
    REQUIRE(woke);
}

TEST_CASE("PL11 a Folder registered in Core is one plugin, and its Scripts do not register alone", "[PL11]") {
    ScriptRig rig;
    const InstanceId folder = add_folder(rig.game, "Tools", rig.game.core());
    add_script(rig.game, folder, "A", "print('a')");
    add_script(rig.game, folder, "B", "print('b')");
    rig.runtime.drain_output();

    REQUIRE(rig.runtime.register_plugin(folder, "ToolsFile"));
    rig.runtime.start_core_scripts();
    REQUIRE(rig.runtime.plugins() == std::vector<InstanceId>{folder});
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"a\n", "b\n"});
    REQUIRE(*rig.runtime.plugin_name(folder) == "ToolsFile");
    REQUIRE(rig.runtime.plugin_serial(folder) != 0);
    REQUIRE(rig.runtime.plugin_name(folder + 1000) == nullptr);

    // A Script added later under the registered Folder still does not become its own plugin.
    add_script(rig.game, folder, "C", "print('c')");
    rig.runtime.start_core_scripts();
    REQUIRE(rig.runtime.plugins() == std::vector<InstanceId>{folder});

    // A Folder in Core that is not registered keeps today's behaviour: each Script is a plugin.
    const InstanceId loose = add_folder(rig.game, "Loose", rig.game.core());
    const InstanceId s = add_script(rig.game, loose, "S", "print('s')").id();
    rig.runtime.start_core_scripts();
    REQUIRE(rig.runtime.is_plugin(s));
}
