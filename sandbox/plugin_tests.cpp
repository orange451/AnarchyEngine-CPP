// VMs share with the play VM: waits, tasks, and connections that go on while stopped.

#include "support.hpp"

#include "Engine.hpp"
#include "Folder.hpp"
#include "Gui.hpp"
#include "InstanceFile.hpp"
#include "ModuleScript.hpp"
#include "PluginUi.hpp"
#include "ide/PluginLoader.hpp"
#include "ScriptRuntime.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
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

// Writes a plugin file holding a Folder named name with one Script per source.
void write_plugin(const std::filesystem::path& folder, const std::string& name,
                  const std::vector<std::string>& sources) {
    engine_core::CopiedNode root;
    root.class_name = "Folder";
    root.name = name;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        engine_core::CopiedNode script;
        script.class_name = "Script";
        script.name = "S" + std::to_string(i);
        script.has_source = true;
        script.source = sources[i];
        root.children.push_back(std::move(script));
    }
    std::filesystem::create_directories(folder);
    std::string error;
    REQUIRE(engine_core::save_instance_file(folder / (name + ide::kPluginExtension), {root}, error));
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

TEST_CASE("PL12 user plugins load, unload, and reload with their files", "[PL12]") {
    ScriptRig rig;
    TempDir dir;
    ide::PluginLoader loader;
    write_plugin(dir.path, "Alpha", {"print('alpha 1')", "task.wait(100) print('never')"});
    rig.runtime.drain_output();

    loader.sync_user(rig.game, rig.runtime, ide::scan_plugins(dir.path));
    REQUIRE(loader.user().size() == 1);
    const InstanceId root = loader.user()[0].root;
    REQUIRE(rig.game.parent(root) == rig.game.core());
    REQUIRE(rig.runtime.is_plugin(root));
    REQUIRE(*rig.runtime.plugin_name(root) == "Alpha");
    REQUIRE(rig.runtime.plugins() == std::vector<InstanceId>{root});
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"alpha 1\n"});

    // Unchanged files do nothing.
    loader.sync_user(rig.game, rig.runtime, ide::scan_plugins(dir.path));
    REQUIRE(loader.user()[0].root == root);
    REQUIRE(rig.runtime.drain_output().lines.empty());

    // A changed file reloads with the new source; the old waiting thread is gone.
    write_plugin(dir.path, "Alpha", {"print('alpha 2, longer')"});
    loader.sync_user(rig.game, rig.runtime, ide::scan_plugins(dir.path));
    REQUIRE_FALSE(rig.game.alive(root));
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"alpha 2, longer\n"});
    rig.frames(2, 200.0);
    REQUIRE(rig.runtime.drain_output().lines.empty());

    // A removed file unloads.
    std::filesystem::remove(dir.path / "Alpha.aeplugin");
    loader.sync_user(rig.game, rig.runtime, ide::scan_plugins(dir.path));
    REQUIRE(loader.user().empty());
    REQUIRE(rig.game.get_children(rig.game.core()).empty());
    REQUIRE(rig.runtime.plugins().empty());
}

TEST_CASE("PL13 a bad plugin file logs once, leaves Core clean, and loads once fixed", "[PL13]") {
    ScriptRig rig;
    TempDir dir;
    std::filesystem::create_directories(dir.path);
    std::ofstream(dir.path / "Broken.aeplugin") << R"({"format":"aeinst","vers)";
    ide::PluginLoader loader;
    rig.runtime.drain_output();

    loader.sync_user(rig.game, rig.runtime, ide::scan_plugins(dir.path));
    std::vector<std::string> out = texts(rig.runtime.drain_output());
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].find("Plugin \"Broken\" failed to load") != std::string::npos);
    REQUIRE(rig.game.get_children(rig.game.core()).empty());
    // Polling again with the same stamp does not repeat the error.
    loader.sync_user(rig.game, rig.runtime, ide::scan_plugins(dir.path));
    REQUIRE(rig.runtime.drain_output().lines.empty());

    // A root that is not a Folder is refused too.
    engine_core::CopiedNode script;
    script.class_name = "Script";
    script.name = "Lonely";
    std::string error;
    REQUIRE(engine_core::save_instance_file(dir.path / "Lonely.aeplugin", {script}, error));
    loader.sync_user(rig.game, rig.runtime, ide::scan_plugins(dir.path));
    out = texts(rig.runtime.drain_output());
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].find("Plugin \"Lonely\" failed to load: its root must be one Folder") != std::string::npos);
    REQUIRE(rig.game.get_children(rig.game.core()).empty());

    // Fixed: it loads.
    write_plugin(dir.path, "Broken", {"print('fixed')"});
    loader.sync_user(rig.game, rig.runtime, ide::scan_plugins(dir.path));
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"fixed\n"});
}

TEST_CASE("PL14 plugin file names and the scan order", "[PL14]") {
    REQUIRE(ide::plugin_file_name("Terrain Tools") == "Terrain Tools");
    REQUIRE(ide::plugin_file_name("a/b:c*?") == "a_b_c__");
    REQUIRE(ide::plugin_file_name("trailing. ") == "trailing");
    REQUIRE(ide::plugin_file_name("") == "Plugin");
    TempDir dir;
    write_plugin(dir.path, "Zed", {});
    write_plugin(dir.path, "Apple", {});
    std::ofstream(dir.path / "notes.txt") << "x";
    const std::vector<ide::PluginStamp> stamps = ide::scan_plugins(dir.path);
    REQUIRE(stamps.size() == 2);
    REQUIRE(stamps[0].name == "Apple");
    REQUIRE(stamps[1].name == "Zed");
    REQUIRE(ide::scan_plugins(dir.path / "missing").empty());
}

TEST_CASE("PL15 a plugin with no enabled Script still loads, and its tree survives the round trip", "[PL15]") {
    ScriptRig rig;
    TempDir dir;
    engine_core::Folder& source = rig.game.create<engine_core::Folder>();
    rig.game.set_name(source.id(), "Quiet");
    rig.game.set_parent(source.id(), workspace_of(rig.game));
    engine_core::Script& off = add_script(rig.game, source.id(), "Off", "print('off')");
    off.set_enabled(false);
    add_folder(rig.game, "Empty", source.id());
    std::filesystem::create_directories(dir.path);
    std::string error;
    REQUIRE(engine_core::save_instance_file(dir.path / "Quiet.aeplugin", {engine_core::copy_tree(rig.game, source.id())},
                                            error));
    rig.runtime.drain_output();
    ide::PluginLoader loader;
    loader.sync_user(rig.game, rig.runtime, ide::scan_plugins(dir.path));
    REQUIRE(loader.user().size() == 1);
    const InstanceId root = loader.user()[0].root;
    REQUIRE(rig.runtime.is_plugin(root));
    REQUIRE(rig.runtime.drain_output().lines.empty());
    REQUIRE(rig.game.find_first_child(root, "Empty") != 0);
    const auto* loaded = dynamic_cast<engine_core::Script*>(rig.game.instance(rig.game.find_first_child(root, "Off")));
    REQUIRE(loaded != nullptr);
    REQUIRE_FALSE(loaded->enabled());
}

TEST_CASE("PL16 a Folder offers Save as Plugin; other classes do not", "[PL16]") {
    ScriptRig rig;
    const InstanceId folder = add_folder(rig.game, "F", workspace_of(rig.game));
    auto offers = [&](InstanceId id) {
        std::vector<engine_core::ContextAction> actions;
        rig.game.instance(id)->context_actions(actions);
        return std::any_of(actions.begin(), actions.end(), [](const engine_core::ContextAction& action) {
            return action.action == engine_core::InstanceAction::SaveAsPlugin;
        });
    };
    REQUIRE(offers(folder));
    REQUIRE(std::string(engine_core::action_label(engine_core::InstanceAction::SaveAsPlugin)) == "Save as Plugin");
    REQUIRE_FALSE(offers(add_script(rig.game, folder, "S", "").id()));
}

TEST_CASE("PL17 plugin is one object per plugin, with its Name; Play and Console have none", "[PL17]") {
    ScriptRig rig;
    const InstanceId folder = add_folder(rig.game, "Tools", rig.game.core());
    engine_core::ModuleScript& lib = rig.game.create<engine_core::ModuleScript>();
    rig.game.set_name(lib.id(), "Lib");
    lib.set_source("return function() return plugin end");
    rig.game.set_parent(lib.id(), folder);
    add_script(rig.game, folder, "A", "_G.pa = plugin print(plugin.Name, tostring(plugin))");
    add_script(rig.game, folder, "B",
               "print(plugin == _G.pa, require(script.Parent.Lib)() == plugin)\n"
               "task.spawn(function() print('spawned', plugin == _G.pa) end)\n"
               "print(pcall(function() plugin.Name = 'x' end))");
    rig.runtime.drain_output();
    REQUIRE(rig.runtime.register_plugin(folder, "ToolsFile"));
    std::vector<std::string> out = texts(rig.runtime.drain_output());
    REQUIRE(out.size() == 4);
    REQUIRE(out[0] == "ToolsFile\tPlugin\n");
    REQUIRE(out[1] == "true\ttrue\n");
    REQUIRE(out[2].rfind("false\t", 0) == 0);
    // What a Script spawns runs after it, as in one pass of a play step.
    REQUIRE(out[3] == "spawned\ttrue\n");

    rig.runtime.run_chunk("print(plugin)");
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"nil\n"});
    add_script(rig.game, "Play", "print('play', plugin)");
    rig.game.start_simulation();
    rig.frames(1);
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"play\tnil\n"});
    rig.game.stop_simulation();
}

TEST_CASE("PL18 Unloading runs before the plugin's threads stop", "[PL18]") {
    ScriptRig rig;
    TempDir dir;
    write_plugin(dir.path, "Bye", {"plugin.Unloading:Connect(function() print('unloading', plugin.Name) end)"});
    ide::PluginLoader loader;
    loader.sync_user(rig.game, rig.runtime, ide::scan_plugins(dir.path));
    rig.runtime.drain_output();
    std::filesystem::remove(dir.path / "Bye.aeplugin");
    loader.sync_user(rig.game, rig.runtime, ide::scan_plugins(dir.path));
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"unloading\tBye\n"});
    REQUIRE(rig.runtime.plugins().empty());
}

TEST_CASE("PL19 icon paths stay inside icons/", "[PL19]") {
    REQUIRE(engine_core::plugin_icon_path_ok("icons/Brush.png"));
    REQUIRE_FALSE(engine_core::plugin_icon_path_ok("Brush.png"));
    REQUIRE_FALSE(engine_core::plugin_icon_path_ok("icons/../shaders/x.png"));
    REQUIRE_FALSE(engine_core::plugin_icon_path_ok("icons/"));
    REQUIRE_FALSE(engine_core::plugin_icon_path_ok("icons\\x.png"));
}

TEST_CASE("PL20 toolbars and buttons show in PluginUi, and a click fires Click", "[PL20]") {
    ScriptRig rig;
    const InstanceId folder = add_folder(rig.game, "Tools", rig.game.core());
    add_script(rig.game, folder, "Main",
               "local tb = plugin:CreateToolbar('Terrain Tools')\n"
               "local b = tb:CreateButton('Smooth', 'Smooth it', 'icons/Brush.png', 'Smooth')\n"
               "b.Click:Connect(function() print('clicked', b.Name) b:SetActive(true) end)\n"
               "local ok, err = pcall(function() tb:CreateButton('Smooth', '', '', 'Again') end)\n"
               "print(ok, string.find(err, 'already') ~= nil)\n"
               "ok, err = pcall(function() tb:CreateButton('Bad', '', '../x.png', 'Bad') end)\n"
               "print(ok, string.find(err, 'icons/') ~= nil)\n"
               "local off = tb:CreateButton('Off', '', '', 'Off') off.Enabled = false\n"
               "off.Click:Connect(function() print('never') end)\n"
               "print(off.Enabled, b.Enabled)");
    rig.runtime.drain_output();
    const std::uint64_t before = rig.runtime.plugin_ui().revision();
    REQUIRE(rig.runtime.register_plugin(folder, "Tools"));
    REQUIRE(texts(rig.runtime.drain_output()) ==
            std::vector<std::string>{"false\ttrue\n", "false\ttrue\n", "false\ttrue\n"});
    REQUIRE(rig.runtime.plugin_ui().revision() != before);

    std::vector<engine_core::PluginToolbarState> bars = rig.runtime.plugin_ui().toolbars();
    REQUIRE(bars.size() == 1);
    REQUIRE(bars[0].name == "Terrain Tools");
    REQUIRE(bars[0].plugin == "Tools");
    REQUIRE(bars[0].buttons.size() == 2);
    REQUIRE(bars[0].buttons[0].icon == "icons/Brush.png");
    REQUIRE(bars[0].buttons[0].tooltip == "Smooth it");
    REQUIRE_FALSE(bars[0].buttons[1].enabled);

    REQUIRE(rig.runtime.plugin_ui().click(bars[0].buttons[0].id));
    REQUIRE_FALSE(rig.runtime.plugin_ui().click(bars[0].buttons[1].id));
    rig.game.events().drain();
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"clicked\tSmooth\n"});
    REQUIRE(rig.runtime.plugin_ui().toolbars()[0].buttons[0].active);

    REQUIRE(rig.runtime.unregister_plugin(folder));
    REQUIRE(rig.runtime.plugin_ui().toolbars().empty());
}

TEST_CASE("PL21 two plugins with one toolbar name keep two groups, each going with its plugin", "[PL21]") {
    ScriptRig rig;
    const InstanceId a = add_folder(rig.game, "A", rig.game.core());
    add_script(rig.game, a, "M", "plugin:CreateToolbar('Tools'):CreateButton('x', '', '', 'X')");
    const InstanceId b = add_folder(rig.game, "B", rig.game.core());
    add_script(rig.game, b, "M", "plugin:CreateToolbar('Tools'):CreateButton('y', '', '', 'Y')");
    REQUIRE(rig.runtime.register_plugin(b, "B"));
    REQUIRE(rig.runtime.register_plugin(a, "A"));
    std::vector<engine_core::PluginToolbarState> bars = rig.runtime.plugin_ui().toolbars();
    REQUIRE(bars.size() == 2);
    // By plugin name, not by the order they registered.
    REQUIRE(bars[0].plugin == "A");
    REQUIRE(rig.runtime.unregister_plugin(a));
    bars = rig.runtime.plugin_ui().toolbars();
    REQUIRE(bars.size() == 1);
    REQUIRE(bars[0].plugin == "B");
}

TEST_CASE("PL22 CreateDockWidget makes a DockWidget under the plugin, once per id", "[PL22]") {
    ScriptRig rig;
    const InstanceId folder = add_folder(rig.game, "Tools", rig.game.core());
    add_script(rig.game, folder, "Main",
               "local w = plugin:CreateDockWidget('Panel', {Title = 'Terrain', InitialDock = 'Bottom', Enabled = true,"
               " Width = 250, Height = 120})\n"
               "print(w.ClassName, w.Parent == script.Parent, w.Title, w.Enabled)\n"
               "local d = plugin:CreateDockWidget('Plain')\n"
               "print(d.Title, d.Enabled)\n"
               "print(pcall(function() plugin:CreateDockWidget('Panel') end))\n"
               "print(pcall(function() plugin:CreateDockWidget('X', {InitialDock = 'Up'}) end))\n"
               "print(pcall(function() Instance.new('DockWidget') end))\n"
               "w.Enabled = false print(w.Enabled)");
    rig.runtime.drain_output();
    REQUIRE(rig.runtime.register_plugin(folder, "Tools"));
    const std::vector<std::string> out = texts(rig.runtime.drain_output());
    REQUIRE(out.size() == 6);
    REQUIRE(out[0] == "DockWidget\ttrue\tTerrain\ttrue\n");
    REQUIRE(out[1] == "Plain\tfalse\n");
    REQUIRE(out[2].rfind("false\t", 0) == 0);
    REQUIRE(out[2].find("already") != std::string::npos);
    REQUIRE(out[3].find("InitialDock") != std::string::npos);
    REQUIRE(out[4].rfind("false\t", 0) == 0);
    REQUIRE(out[5] == "false\n");
    // The refused InitialDock left nothing behind.
    REQUIRE(rig.game.find_first_child(folder, "X") == 0);

    const InstanceId panel = rig.game.find_first_child(folder, "Panel");
    const auto* widget = dynamic_cast<const engine_core::DockWidget*>(rig.game.instance(panel));
    REQUIRE(widget != nullptr);
    REQUIRE(widget->pane_name() == "plugin:Tools/Panel");
    REQUIRE(widget->initial_dock == engine_core::DockSide::Bottom);
    REQUIRE(widget->width == 250);
    REQUIRE(widget->height == 120);
    REQUIRE_FALSE(widget->enabled());
}

TEST_CASE("PL23 a BillboardGui inside a DockWidget is not drawn in the world", "[PL23]") {
    ScriptRig rig;
    const InstanceId folder = add_folder(rig.game, "Tools", rig.game.core());
    add_script(rig.game, folder, "Main",
               "local w = plugin:CreateDockWidget('Panel')\n"
               "local b = Instance.new('BillboardGui') b.Name = 'Board' b.Parent = w");
    REQUIRE(rig.runtime.register_plugin(folder, "Tools"));
    const InstanceId board = rig.game.find_first_child(rig.game.find_first_child(folder, "Panel"), "Board");
    const auto* gui = dynamic_cast<const engine_core::BillboardGui*>(rig.game.instance(board));
    REQUIRE(gui != nullptr);
    REQUIRE_FALSE(gui->drawn());
}

TEST_CASE("PL24 a user plugin may not take a built-in plugin's name", "[PL24]") {
    ScriptRig rig;
    ide::PluginLoader loader;
    loader.load(rig.game, rig.runtime, {ide::PluginFile{"SceneTool", "print('built-in')"}});
    TempDir dir;
    write_plugin(dir.path, "SceneTool", {"print('user copy')"});
    rig.runtime.drain_output();
    loader.sync_user(rig.game, rig.runtime, ide::scan_plugins(dir.path));
    const std::vector<std::string> out = texts(rig.runtime.drain_output());
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].find("Plugin \"SceneTool\" failed to load: a built-in plugin has that name") != std::string::npos);
    REQUIRE(rig.runtime.plugins().size() == 1);
}

TEST_CASE("PL25 InitialDock Center opens a dock widget in the middle dock", "[PL25]") {
    ScriptRig rig;
    const InstanceId folder = add_folder(rig.game, "Tools", rig.game.core());
    add_script(rig.game, folder, "Main", "plugin:CreateDockWidget('Mid', {InitialDock = 'Center'})");
    REQUIRE(rig.runtime.register_plugin(folder, "Tools"));
    const auto* widget =
        dynamic_cast<const engine_core::DockWidget*>(rig.game.instance(rig.game.find_first_child(folder, "Mid")));
    REQUIRE(widget != nullptr);
    REQUIRE(widget->initial_dock == engine_core::DockSide::Center);
}

TEST_CASE("PL26 one plugin is active at a time, and Deactivation says when it stops", "[PL26]") {
    ScriptRig rig;
    const InstanceId a = add_folder(rig.game, "A", rig.game.core());
    add_script(rig.game, a, "M",
               "_G.a = plugin\n"
               "plugin.Deactivation:Connect(function() print('A off', plugin:IsActivated()) end)\n"
               "plugin:Activate()\n"
               "print('A on', plugin:IsActivated())");
    const InstanceId b = add_folder(rig.game, "B", rig.game.core());
    add_script(rig.game, b, "M",
               "_G.b = plugin\n"
               "plugin.Deactivation:Connect(function() print('B off') end)");
    rig.runtime.drain_output();
    REQUIRE(rig.runtime.register_plugin(a, "A"));
    REQUIRE(rig.runtime.register_plugin(b, "B"));
    rig.game.events().drain();
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"A on\ttrue\n"});
    REQUIRE(rig.runtime.plugin_ui().mouse_held());

    // B taking over turns A off.
    rig.runtime.plugin_ui().activate(rig.runtime.plugin_serial(b));
    REQUIRE(rig.runtime.plugin_ui().active() == rig.runtime.plugin_serial(b));
    rig.game.events().drain();
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"A off\tfalse\n"});

    // Deactivate, and unloading the active plugin, let go of the mouse.
    rig.runtime.plugin_ui().deactivate_all();
    rig.game.events().drain();
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"B off\n"});
    REQUIRE_FALSE(rig.runtime.plugin_ui().mouse_held());
    rig.runtime.plugin_ui().activate(rig.runtime.plugin_serial(a));
    REQUIRE(rig.runtime.plugin_ui().mouse_held());
    REQUIRE(rig.runtime.unregister_plugin(a));
    REQUIRE_FALSE(rig.runtime.plugin_ui().mouse_held());
}

TEST_CASE("PL27 the active plugin's mouse hears the Scene View", "[PL27]") {
    ScriptRig rig;
    const InstanceId folder = add_folder(rig.game, "Brush", rig.game.core());
    add_script(rig.game, folder, "M",
               "local mouse = plugin:GetMouse()\n"
               "print(mouse == plugin:GetMouse())\n"
               "mouse.Button1Down:Connect(function()\n"
               "    local ray = mouse.UnitRay\n"
               "    local hit = mouse.Hit\n"
               "    print('down', mouse.X, mouse.Y, mouse.Shift, mouse.Ctrl, mouse.Alt, ray.Direction.Z,\n"
               "          math.floor(hit.Position.Z + 0.5), mouse.Target)\n"
               "end)\n"
               "mouse.Button1Up:Connect(function() print('up') end)\n"
               "mouse.Move:Connect(function() print('move', mouse.X) end)\n"
               "mouse.WheelForward:Connect(function() print('wheel') end)");
    rig.runtime.drain_output();
    REQUIRE(rig.runtime.register_plugin(folder, "Brush"));
    REQUIRE(texts(rig.runtime.drain_output()) == std::vector<std::string>{"true\n"});

    engine_core::PluginMouseEvent press;
    press.kind = engine_core::PluginMouseEvent::Kind::Button1Down;
    press.x = 40;
    press.y = 30;
    press.origin = engine_core::Vec3{0.f, 0.f, 0.f};
    press.direction = engine_core::Vec3{0.f, 0.f, -1.f};
    press.shift = true;
    // Not active: nothing fires.
    rig.runtime.plugin_mouse_event(press);
    rig.game.events().drain();
    REQUIRE(rig.runtime.drain_output().lines.empty());

    rig.runtime.plugin_ui().activate(rig.runtime.plugin_serial(folder));
    // Handlers run at the next drain and read the mouse as it is then, so each
    // event drains here as a simulation step would.
    rig.runtime.plugin_mouse_event(press);
    rig.game.events().drain();
    engine_core::PluginMouseEvent move = press;
    move.kind = engine_core::PluginMouseEvent::Kind::Move;
    move.x = 41;
    rig.runtime.plugin_mouse_event(move);
    rig.game.events().drain();
    engine_core::PluginMouseEvent up = move;
    up.kind = engine_core::PluginMouseEvent::Kind::Button1Up;
    rig.runtime.plugin_mouse_event(up);
    rig.game.events().drain();
    engine_core::PluginMouseEvent wheel = move;
    wheel.kind = engine_core::PluginMouseEvent::Kind::WheelForward;
    rig.runtime.plugin_mouse_event(wheel);
    rig.game.events().drain();
    // Nothing in the world here: Hit is 1000 units along the ray, and Target is nil.
    REQUIRE(texts(rig.runtime.drain_output()) ==
            std::vector<std::string>{"down\t40\t30\ttrue\tfalse\tfalse\t-1\t-1000\tnil\n", "move\t41\n", "up\n",
                                     "wheel\n"});
}
