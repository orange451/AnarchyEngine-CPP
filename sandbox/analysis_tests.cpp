#include "AssetInstances.hpp"
#include "DataModel.hpp"
#include "Folder.hpp"
#include "Game.hpp"
#include "GameObject.hpp"
#include "GameService.hpp"
#include "LuaApi.hpp"
#include "LuaSource.hpp"
#include "ModuleScript.hpp"
#include "Project.hpp"
#include "Script.hpp"
#include "ScriptAnalysis.hpp"
#include "ScriptRuntime.hpp"
#include "TaskScheduler.hpp"
#include "types.hpp"

#include "support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string dump(const std::vector<engine_core::Diagnostic>& diagnostics) {
    std::ostringstream out;
    for (const engine_core::Diagnostic& diagnostic : diagnostics) {
        out << diagnostic.code << " @" << diagnostic.range.start.line << ":" << diagnostic.range.start.character << " "
            << diagnostic.message << "\n";
    }
    return out.str();
}

void settle(engine_core::ScriptAnalysis& analysis) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!analysis.idle()) {
        analysis.pump();
        if (std::chrono::steady_clock::now() > deadline) {
            FAIL("script analysis did not settle");
        }
        std::this_thread::yield();
    }
}

bool has_code(const std::vector<engine_core::Diagnostic>& diagnostics, const char* code) {
    for (const engine_core::Diagnostic& diagnostic : diagnostics) {
        if (diagnostic.code == code) {
            return true;
        }
    }
    return false;
}

// About `lines` lines of ordinary code: loops, tables, string formatting, a
// FindFirstChild, and in each function one type error and one unknown global.
// `salt` keeps function names apart when several such scripts share a place.
std::string long_source(int lines, bool strict, int salt) {
    std::ostringstream out;
    if (strict) {
        out << "--!strict\n";
    }
    for (int line = 0, f = 0; line < lines; line += 14, ++f) {
        out << "local function fn" << f << "_" << salt << "(a: number, b: string)\n"
            << "    local t = { x = a, y = b, list = {} }\n"
            << "    for i = 1, a do\n"
            << "        table.insert(t.list, i * 2)\n"
            << "        if i % 3 == 0 then t.x += i else t.x -= 1 end\n"
            << "    end\n"
            << "    local name = string.format(\"%s-%d\", b, #t.list)\n"
            << "    local part = workspace:FindFirstChild(name)\n"
            << "    if part then print(part.Name) end\n"
            << "    local bad: number = \"oops\"\n"
            << "    undefinedThing" << f << "()\n"
            << "    return t.x + #name\n"
            << "end\n"
            << "print(fn" << f << "_" << salt << "(" << f << ", \"k\"))\n";
    }
    return out.str();
}

}  // namespace

TEST_CASE("A1 a syntax error is one Syntax diagnostic and compile still fails", "[A1]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Script& script = add_script(rig.game, "Broken", "local x =");
    settle(analysis);
    const std::vector<engine_core::Diagnostic> diagnostics = analysis.diagnostics(script.id());
    INFO(dump(diagnostics));
    REQUIRE(diagnostics.size() == 1);
    REQUIRE(diagnostics[0].severity == engine_core::Severity::Error);
    REQUIRE(diagnostics[0].code == "Syntax");
    REQUIRE(analysis.get_diagnostics_for_line(script.id(), diagnostics[0].range.start.line).size() == 1);

    std::ostringstream report;
    analysis.print_report(report);
    REQUIRE(report.str().find("Broken") != std::string::npos);
    REQUIRE(report.str().find("Syntax") != std::string::npos);

    rig.game.start_simulation();
    rig.frames(1);
    REQUIRE_FALSE(rig.runtime.last_error().empty());
}

TEST_CASE("A2 nocheck skips the type error and strict reports it", "[A2]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Script& script = add_script(rig.game, "Typed", "--!nocheck\nlocal x: number = \"a\"\n");
    settle(analysis);
    INFO(dump(analysis.diagnostics(script.id())));
    REQUIRE_FALSE(has_code(analysis.diagnostics(script.id()), "Type"));

    script.set_source("--!strict\nlocal x: number = \"a\"\n");
    settle(analysis);
    const std::vector<engine_core::Diagnostic> diagnostics = analysis.diagnostics(script.id());
    INFO(dump(diagnostics));
    REQUIRE(has_code(diagnostics, "Type"));
    bool type_error = false;
    for (const engine_core::Diagnostic& diagnostic : diagnostics) {
        if (diagnostic.code == "Type" && diagnostic.severity == engine_core::Severity::Error) {
            type_error = true;
        }
    }
    REQUIRE(type_error);
}

TEST_CASE("A3 wait is an unknown global and task.wait is clean", "[A3]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Script& script = add_script(rig.game, "Wait", "wait(1)\n");
    settle(analysis);
    const std::vector<engine_core::Diagnostic> waiting = analysis.diagnostics(script.id());
    INFO(dump(waiting));
    bool unknown = false;
    for (const engine_core::Diagnostic& diagnostic : waiting) {
        if (diagnostic.severity == engine_core::Severity::Warning && diagnostic.code == "Lint/UnknownGlobal") {
            unknown = true;
        }
    }
    REQUIRE(unknown);

    script.set_source("task.wait(1)\n");
    settle(analysis);
    INFO(dump(analysis.diagnostics(script.id())));
    REQUIRE(analysis.diagnostics(script.id()).empty());
}

TEST_CASE("A4 PreRender is not declared and Heartbeat is", "[A4]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Script& script = add_script(rig.game, "Render",
                                             "local run = game:GetService(\"RunService\")\n"
                                             "local blocked = run.PreRender\n"
                                             "local beat = run.Heartbeat\n"
                                             "return blocked, beat\n");
    settle(analysis);
    const std::vector<engine_core::Diagnostic> diagnostics = analysis.diagnostics(script.id());
    INFO(dump(diagnostics));
    bool prerender = false;
    bool heartbeat = false;
    for (const engine_core::Diagnostic& diagnostic : diagnostics) {
        if (diagnostic.message.find("PreRender") != std::string::npos) {
            prerender = true;
            REQUIRE(diagnostic.severity == engine_core::Severity::Warning);
        }
        if (diagnostic.message.find("Heartbeat") != std::string::npos) {
            heartbeat = true;
        }
    }
    REQUIRE(prerender);
    REQUIRE_FALSE(heartbeat);
}

TEST_CASE("A5 only the latest source is published", "[A5]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Script& script = add_script(rig.game, "Edit", "return 1\n");
    settle(analysis);
    int publishes = 0;
    const std::uint64_t token = analysis.diagnostics_changed().connect([&](engine_core::InstanceId id) {
        if (id == script.id()) {
            ++publishes;
        }
    });
    script.set_source("local x =");
    script.set_source("return 2\n");
    settle(analysis);
    analysis.diagnostics_changed().disconnect(token);
    INFO(dump(analysis.diagnostics(script.id())));
    REQUIRE(publishes == 1);
    REQUIRE(analysis.diagnostics(script.id()).empty());
}

TEST_CASE("A6 invalidating a required module reanalyzes the script", "[A6]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::ModuleScript& module = rig.game.create<engine_core::ModuleScript>();
    rig.game.set_name(module.id(), "Mod");
    module.set_source("return 1\n");
    rig.game.set_parent(module.id(), workspace_of(rig.game));
    engine_core::Script& script = add_script(rig.game, "Main", "local value = require(script.Parent.Mod)\nreturn value\n");
    settle(analysis);

    int fires = 0;
    analysis.diagnostics_changed().connect([&](engine_core::InstanceId id) {
        if (id == script.id()) {
            ++fires;
        }
    });
    module.set_source("--!strict\nlocal x: number = \"a\"\nreturn x\n");
    settle(analysis);
    INFO(dump(analysis.diagnostics(module.id())));
    REQUIRE(fires >= 1);
    REQUIRE(has_code(analysis.diagnostics(module.id()), "Type"));
}

TEST_CASE("A7 a type error does not block the script", "[A7]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Script& script = add_script(rig.game, "Runs",
                                             "--!strict\n"
                                             "local value: number = \"nope\"\n"
                                             "local box = script:GetChildren()[1]\n"
                                             "box.Name = \"ran\"\n");
    engine_core::GameObject& box = rig.game.create<engine_core::GameObject>();
    rig.game.set_name(box.id(), "0");
    rig.game.set_parent(box.id(), script.id());
    settle(analysis);
    INFO(dump(analysis.diagnostics(script.id())));
    REQUIRE(has_code(analysis.diagnostics(script.id()), "Type"));

    rig.game.start_simulation();
    rig.frames(1);
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    REQUIRE(rig.game.name(box.id()) == "ran");
}

TEST_CASE("A8 stop restores authored diagnostics", "[A8]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    const char* authored = "--!strict\nlocal value: number = \"nope\"\n";
    engine_core::Script& script = add_script(rig.game, "Authored", authored);
    settle(analysis);
    REQUIRE(has_code(analysis.diagnostics(script.id()), "Type"));

    rig.game.start_simulation();
    script.set_source("local x =\n");
    settle(analysis);
    INFO(dump(analysis.diagnostics(script.id())));
    REQUIRE(has_code(analysis.diagnostics(script.id()), "Syntax"));

    rig.game.stop_simulation();
    REQUIRE(script.source() == authored);
    settle(analysis);
    const std::vector<engine_core::Diagnostic> restored = analysis.diagnostics(script.id());
    INFO(dump(restored));
    REQUIRE(has_code(restored, "Type"));
    REQUIRE_FALSE(has_code(restored, "Syntax"));
}

TEST_CASE("A9 pump is the only publisher", "[A9]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Script& script = add_script(rig.game, "Queued", "return 1\n");
    settle(analysis);
    REQUIRE(analysis.diagnostics(script.id()).empty());
    REQUIRE(analysis.analyzed_source(script.id()) == std::string("return 1\n"));
    script.set_source("local x =\n");
    // This pump() captures the tree the check reads. Nothing is finished yet,
    // so it publishes nothing.
    analysis.pump();
    REQUIRE(analysis.analyzed_source(script.id()) == std::string("return 1\n"));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (analysis.busy()) {
        if (std::chrono::steady_clock::now() > deadline) {
            FAIL("script analysis worker did not finish");
        }
        std::this_thread::yield();
    }
    REQUIRE(analysis.diagnostics(script.id()).empty());
    REQUIRE(analysis.analyzed_source(script.id()) == std::string("return 1\n"));
    REQUIRE_FALSE(analysis.idle());
    analysis.pump();
    const std::vector<engine_core::Diagnostic> diagnostics = analysis.diagnostics(script.id());
    INFO(dump(diagnostics));
    REQUIRE(diagnostics.size() == 1);
    REQUIRE(diagnostics[0].code == "Syntax");
    REQUIRE(analysis.analyzed_source(script.id()) == std::string("local x =\n"));
    REQUIRE(analysis.idle());
}

// A script is checked against the place in the explorer. A child that is there
// is found: FindFirstChild gives that child's class with no nil, so no assert
// is needed. Only a name that is not in the place is Instance?.
TEST_CASE("A10 a child in the place is its class and never nil", "[A10]") {
    ScriptRig rig;
    engine_core::GameObject& part = rig.game.create<engine_core::GameObject>();
    rig.game.set_name(part.id(), "Tri0");
    rig.game.set_parent(part.id(), workspace_of(rig.game));
    engine_core::ScriptAnalysis analysis(rig.game);

    engine_core::Script& bare = add_script(rig.game, "Bare",
                                            "local tri = workspace:FindFirstChild(\"Tri0\")\n"
                                            "local home = tri.Transform.Position\n"
                                            "return home\n");
    settle(analysis);
    INFO(dump(analysis.diagnostics(bare.id())));
    REQUIRE(analysis.diagnostics(bare.id()).empty());

    SECTION("the type has no nil in it") {
        bare.set_source("--!strict\nlocal tri = workspace:FindFirstChild(\"Tri0\")\nlocal n: number = tri\n");
        settle(analysis);
        const std::string report = dump(analysis.diagnostics(bare.id()));
        INFO(report);
        REQUIRE(report.find("'GameObject'") != std::string::npos);
        REQUIRE(report.find("GameObject?") == std::string::npos);
    }

    SECTION("a name that is not in the place may be nil") {
        bare.set_source("local tri = workspace:FindFirstChild(\"Nope\")\nlocal name = tri.Name\nreturn name\n");
        settle(analysis);
        const std::string report = dump(analysis.diagnostics(bare.id()));
        INFO(report);
        REQUIRE(report.find("could be nil") != std::string::npos);
    }

    engine_core::Script& hop = add_script(rig.game, "Hop",
                                           "local tri = workspace:FindFirstChild(\"Tri0\")\n"
                                           "assert(tri)\n"
                                           "local home = tri.Transform\n"
                                           "tri.Transform = home + Vector3.new(0.45, 0, 0)\n"
                                           "return home\n");
    settle(analysis);
    INFO(dump(analysis.diagnostics(hop.id())));
    REQUIRE(analysis.diagnostics(hop.id()).empty());

    engine_core::Script& missing = add_script(rig.game, "Missing",
                                               "local tri = workspace:FindFirstChild(\"Nope\")\n"
                                               "assert(tri)\n"
                                               "local home = tri.Transform.Position\n"
                                               "return home\n");
    settle(analysis);
    const std::vector<engine_core::Diagnostic> missing_diagnostics = analysis.diagnostics(missing.id());
    INFO(dump(missing_diagnostics));
    bool missing_transform = false;
    for (const engine_core::Diagnostic& diagnostic : missing_diagnostics) {
        if (diagnostic.message.find("Transform") != std::string::npos &&
            diagnostic.message.find("not found") != std::string::npos) {
            missing_transform = true;
        }
    }
    REQUIRE(missing_transform);
}

TEST_CASE("analysis definitions come from the class registry", "[A11]") {
    const std::string source = engine_core::lua_analysis_definitions();
    REQUIRE(source.find("declare extern type GameObject") != std::string::npos);
    REQUIRE(source.find("Transform: Matrix4") != std::string::npos);
    REQUIRE(source.find("function FindFirstChild(self, name: string): Instance?") != std::string::npos);
    REQUIRE(source.find("Parent: DataModel?") != std::string::npos);
    REQUIRE(source.find("type Vector3 = vector") != std::string::npos);
    REQUIRE(source.find("declare task:") != std::string::npos);
    REQUIRE(source.find("PreRender") == std::string::npos);
    REQUIRE(source.find("RenderStepped: Signal_RunService_RenderStepped") != std::string::npos);
    REQUIRE(source.find("BasePart") == std::string::npos);
    // Service is a DataModel like Game; the scene services extend it, and
    // workspace is a global like game.
    REQUIRE(source.find("declare extern type Service extends DataModel with") != std::string::npos);
    REQUIRE(source.find("declare extern type SceneService extends Service with") != std::string::npos);
    REQUIRE(source.find("declare extern type Workspace extends SceneService with") != std::string::npos);
    REQUIRE(source.find("declare extern type Lighting extends SceneService with") != std::string::npos);
    REQUIRE(source.find("declare workspace: Workspace") != std::string::npos);
    REQUIRE(source.find("GetPropertyChangedSignal") == std::string::npos);
    // DataModel is everything in the tree. Instance is what Instance.new makes.
    // Game is the root alone, and GetService is on it.
    REQUIRE(source.find("declare extern type Instance extends DataModel with") != std::string::npos);
    REQUIRE(source.find("declare extern type Folder extends Instance with") != std::string::npos);
    REQUIRE(source.find("declare extern type Game extends DataModel with") != std::string::npos);
    REQUIRE(source.find("declare game: Game") != std::string::npos);
    const std::size_t data_model = source.find("declare extern type DataModel with");
    REQUIRE(data_model != std::string::npos);
    const std::string data_model_block = source.substr(data_model, source.find("end\n", data_model) - data_model);
    REQUIRE(data_model_block.find("GetService") == std::string::npos);
    const std::size_t game = source.find("declare extern type Game extends DataModel with");
    const std::string game_block = source.substr(game, source.find("end\n", game) - game);
    REQUIRE(game_block.find("function GetService") != std::string::npos);
    // Script and ModuleScript share LuaSource. Only Script has Enabled.
    REQUIRE(source.find("declare extern type LuaSource extends Instance with") != std::string::npos);
    REQUIRE(source.find("declare extern type Script extends LuaSource with") != std::string::npos);
    const std::size_t module = source.find("declare extern type ModuleScript extends LuaSource with");
    REQUIRE(module != std::string::npos);
    const std::string module_block = source.substr(module, source.find("end\n", module) - module);
    REQUIRE(module_block.find("Enabled") == std::string::npos);
    // Vector2's operators come from its registered rows, not a list in analysis.
    const std::size_t vector2 = source.find("declare extern type Vector2 with");
    REQUIRE(vector2 != std::string::npos);
    const std::string vector2_block = source.substr(vector2, source.find("end\n", vector2) - vector2);
    REQUIRE(vector2_block.find("__mul: (Vector2 | number, Vector2 | number) -> Vector2") != std::string::npos);
    REQUIRE(vector2_block.find("__unm: (Vector2) -> Vector2") != std::string::npos);
    REQUIRE(vector2_block.find("__eq: (Vector2, Vector2) -> boolean") != std::string::npos);
    std::vector<engine_core::LuaOperator> operators;
    engine_core::lua_class_operators("Vector2", operators);
    REQUIRE(operators.size() == 7);
    // Two rows for one metamethod are overloads, declared as one intersection.
    engine_core::lua_class_operators("Matrix4", operators);
    REQUIRE(operators.size() == 5);
    const std::size_t matrix4 = source.find("declare extern type Matrix4 with");
    REQUIRE(matrix4 != std::string::npos);
    const std::string matrix4_block = source.substr(matrix4, source.find("end\n", matrix4) - matrix4);
    REQUIRE(matrix4_block.find("__mul: ((Matrix4, Matrix4) -> Matrix4) & ((Matrix4, Vector3) -> Vector3)") !=
            std::string::npos);
    REQUIRE(matrix4_block.find("function ToAxisAngle(self): (Vector3, number)") != std::string::npos);
    REQUIRE(matrix4_block.find("Position: Vector3") != std::string::npos);
    // A GameObject moves through its Transform; it has no Position of its own.
    REQUIRE(source.find("declare extern type PVInstance extends Instance with") != std::string::npos);
    const std::size_t game_object = source.find("declare extern type GameObject extends PVInstance with");
    REQUIRE(game_object != std::string::npos);
    const std::string game_object_block = source.substr(game_object, source.find("end\n", game_object) - game_object);
    REQUIRE(game_object_block.find("Transform: Matrix4") != std::string::npos);
    REQUIRE(game_object_block.find("CFrame") == std::string::npos);
    REQUIRE(game_object_block.find("Position") == std::string::npos);
    REQUIRE(source.find("declare extern type Color with") == std::string::npos);
    // Operators are not members, so completion does not offer them.
    REQUIRE(engine_core::lua_class_find("Vector2", "__add") == nullptr);
}

TEST_CASE("disabling script analysis drops diagnostics", "[A]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Script& script = add_script(rig.game, "Off", "local x =\n");
    settle(analysis);
    REQUIRE_FALSE(analysis.diagnostics(script.id()).empty());
    analysis.set_enabled(false);
    REQUIRE(analysis.diagnostics().empty());
    REQUIRE(analysis.diagnostics(script.id()).empty());
    REQUIRE_FALSE(analysis.analyzed_source(script.id()).has_value());
    REQUIRE(analysis.idle());
}

TEST_CASE("A12 a script is rechecked when the tree it looks into changes", "[A12]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Script& hop = add_script(rig.game, "Hop",
                                           "local tri = workspace:FindFirstChild(\"Tri0\")\n"
                                           "assert(tri)\n"
                                           "local home = tri.Transform.Position\n"
                                           "return home\n");
    settle(analysis);
    INFO(dump(analysis.diagnostics(hop.id())));
    REQUIRE_FALSE(analysis.diagnostics(hop.id()).empty());

    // Tri0 arrives after the script. The script did not change; its answer did.
    engine_core::GameObject& part = rig.game.create<engine_core::GameObject>();
    rig.game.set_name(part.id(), "Tri0");
    rig.game.set_parent(part.id(), workspace_of(rig.game));
    settle(analysis);
    INFO(dump(analysis.diagnostics(hop.id())));
    REQUIRE(analysis.diagnostics(hop.id()).empty());

    // A rename away from the looked-up name brings the warning back.
    rig.game.set_name(part.id(), "Tri9");
    settle(analysis);
    REQUIRE_FALSE(analysis.diagnostics(hop.id()).empty());
}

TEST_CASE("A13 a loaded project is analyzed against the whole loaded tree", "[A13]") {
    namespace fs = std::filesystem;
    const TempDir temp;
    const fs::path& dir = temp.path;
    auto write = [&dir](const char* path, const std::string& bytes) {
        fs::create_directories((dir / path).parent_path());
        std::ofstream(dir / path, std::ios::binary) << bytes;
    };
    write("project.json", "{\"format\": 1, \"name\": \"A13\", \"tree\": {\"src\": \"src\"}}\n");
    write("src/init.json", "{\"class\": \"Game\", \"id\": \"root0\", \"Name\": \"A13\"}\n");
    write("src/Workspace.workspace/init.json", "{\"class\": \"Workspace\", \"id\": \"workspace\", \"Name\": \"Workspace\"}\n");
    // Siblings sort by GUID and the loader parents the last one first, so the
    // script is in the tree before Tri0 is.
    write("src/Workspace.workspace/Tri0.aaa.json", "{\"class\": \"GameObject\", \"id\": \"aaa\", \"Name\": \"Tri0\"}\n");
    write("src/Workspace.workspace/Hop.zzz.meta.json", "{\"class\": \"Script\", \"id\": \"zzz\", \"Name\": \"Hop\"}\n");
    write("src/Workspace.workspace/Hop.zzz.luau",
          "local tri = workspace:FindFirstChild(\"Tri0\")\n"
          "assert(tri)\n"
          "local home = tri.Transform.Position\n"
          "return home\n");
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Project loaded = engine_core::Project::load(dir, rig.game);
    settle(analysis);
    const engine_core::InstanceId hop = *rig.game.find_guid("zzz");
    INFO(dump(analysis.diagnostics(hop)));
    REQUIRE(analysis.diagnostics(hop).empty());
}

namespace {

engine_core::ModuleScript& add_module(engine_core::DataModel& game, const char* name, const char* source) {
    engine_core::ModuleScript& module = game.create<engine_core::ModuleScript>();
    game.set_name(module.id(), name);
    module.set_source(source);
    game.set_parent(module.id(), workspace_of(game));
    return module;
}

}  // namespace

TEST_CASE("A16 a require path that reaches a ModuleScript takes its type", "[A16]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Folder& modules = rig.game.create<engine_core::Folder>();
    rig.game.set_name(modules.id(), "Modules");
    rig.game.set_parent(modules.id(), workspace_of(rig.game));
    engine_core::ModuleScript& config = add_module(rig.game, "Config",
                                                   "local Config = {}\n"
                                                   "Config.Currencies = { Gold = \"Gold\" }\n"
                                                   "Config.Settings = { Enabled = false }\n"
                                                   "return Config\n");
    rig.game.set_parent(config.id(), modules.id());
    engine_core::Script& script = add_script(rig.game, "Test",
                                             "local Config = require(workspace:FindFirstChild(\"Modules\"):FindFirstChild(\"Config\"))\n"
                                             "local currency = Config.Currencies.Gold\n"
                                             "Config.Settings.Enabled = true\n"
                                             "print(\"Currency:\", currency, Config.Settings.Enabled)\n");
    settle(analysis);
    INFO(dump(analysis.diagnostics(script.id())));
    REQUIRE(dump(analysis.diagnostics(script.id())).find("Unknown require") == std::string::npos);

    SECTION("the module's type reaches the script") {
        script.set_source("--!strict\n"
                          "local Config = require(workspace:FindFirstChild(\"Modules\"):FindFirstChild(\"Config\"))\n"
                          "local gold: number = Config.Currencies.Gold\n");
        settle(analysis);
        INFO(dump(analysis.diagnostics(script.id())));
        const std::string report = dump(analysis.diagnostics(script.id()));
        REQUIRE(report.find("@2:") != std::string::npos);
        REQUIRE(report.find("Unknown require") == std::string::npos);
    }

    SECTION("a path to nothing still warns") {
        script.set_source("local Config = require(workspace:FindFirstChild(\"Modules\"):FindFirstChild(\"Missing\"))\n");
        settle(analysis);
        INFO(dump(analysis.diagnostics(script.id())));
        REQUIRE(dump(analysis.diagnostics(script.id())).find("Unknown require") != std::string::npos);
    }

    SECTION("an edit to the module reaches the script") {
        config.set_source("return { Currencies = {} }\n");
        settle(analysis);
        INFO(dump(analysis.diagnostics(script.id())));
        REQUIRE(dump(analysis.diagnostics(script.id())).find("Settings") != std::string::npos);
    }
}

TEST_CASE("A17 any method the API marks resolves_child walks a require path", "[A17]") {
    // Stands in for WaitForChild or any later lookup: analysis must follow the
    // API's flag, not the name FindFirstChild.
    static const engine_core::LuaField probe[] = {
        engine_core::lua_method("ProbeChild", "Instance?", nullptr, /*class_from_arg*/ false, /*resolves_child*/ true)};
    engine_core::register_lua_class("ProbeLookup", "DataModel", probe, 1);
    REQUIRE(engine_core::lua_method_resolves_child("FindFirstChild"));
    REQUIRE(engine_core::lua_method_resolves_child("ProbeChild"));
    REQUIRE_FALSE(engine_core::lua_method_resolves_child("GetChildren"));

    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Folder& modules = rig.game.create<engine_core::Folder>();
    rig.game.set_name(modules.id(), "Modules");
    rig.game.set_parent(modules.id(), workspace_of(rig.game));
    engine_core::ModuleScript& config = add_module(rig.game, "Config", "return { Gold = 1 }\n");
    rig.game.set_parent(config.id(), modules.id());
    engine_core::Script& script = add_script(
        rig.game, "Test", "local Config = require(workspace:ProbeChild(\"Modules\"):ProbeChild(\"Config\"))\nreturn Config\n");
    settle(analysis);
    INFO(dump(analysis.diagnostics(script.id())));
    // An undocumented method must not break the definitions and leave nothing checked.
    REQUIRE_FALSE(has_code(analysis.diagnostics(script.id()), "Analysis"));
    REQUIRE(dump(analysis.diagnostics(script.id())).find("Unknown require") == std::string::npos);

    // The same path records the dependency, so editing the module rechecks the script.
    int fires = 0;
    analysis.diagnostics_changed().connect([&](engine_core::InstanceId id) {
        if (id == script.id()) {
            ++fires;
        }
    });
    config.set_source("return { Gold = 2 }\n");
    settle(analysis);
    REQUIRE(fires >= 1);
}

TEST_CASE("A18 WaitForChild gives the child's class without nil and walks a require path", "[A18]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Folder& modules = rig.game.create<engine_core::Folder>();
    rig.game.set_name(modules.id(), "Modules");
    rig.game.set_parent(modules.id(), workspace_of(rig.game));
    engine_core::ModuleScript& config = add_module(rig.game, "Config",
                                                   "local Config = {}\n"
                                                   "Config.Currencies = { Gold = \"Gold\" }\n"
                                                   "Config.Settings = { Enabled = false }\n"
                                                   "return Config\n");
    rig.game.set_parent(config.id(), modules.id());
    engine_core::Script& script = add_script(rig.game, "Test",
                                             "local Config = require(workspace:WaitForChild(\"Modules\"):WaitForChild(\"Config\"))\n"
                                             "local currency = Config.Currencies.Gold\n"
                                             "Config.Settings.Enabled = true\n"
                                             "print(\"Currency:\", currency, Config.Settings.Enabled)\n");
    settle(analysis);
    INFO(dump(analysis.diagnostics(script.id())));
    REQUIRE(analysis.diagnostics(script.id()).empty());

    SECTION("the child keeps its class") {
        script.set_source("--!strict\nlocal folder = workspace:WaitForChild(\"Modules\")\nlocal n: number = folder\n");
        settle(analysis);
        const std::string report = dump(analysis.diagnostics(script.id()));
        INFO(report);
        REQUIRE(report.find("'Folder'") != std::string::npos);
        REQUIRE(report.find("Folder?") == std::string::npos);
    }

    SECTION("a timeout is accepted") {
        script.set_source("--!strict\nlocal folder = workspace:WaitForChild(\"Modules\", 2)\nreturn folder\n");
        settle(analysis);
        INFO(dump(analysis.diagnostics(script.id())));
        REQUIRE(analysis.diagnostics(script.id()).empty());
    }
}

TEST_CASE("A19 a dotted name that reaches a child is not an unknown member", "[A19]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::GameObject& door = rig.game.create<engine_core::GameObject>();
    rig.game.set_name(door.id(), "Door");
    rig.game.set_parent(door.id(), workspace_of(rig.game));
    engine_core::Script& script = add_script(rig.game, "Dot",
                                              "local d = workspace.Door\n"
                                              "d.Name = \"x\"\n"
                                              "local c = workspace.Door.Transform\n"
                                              "local m = workspace.Nope\n"
                                              "return c, m\n");
    settle(analysis);
    std::vector<engine_core::Diagnostic> found = analysis.diagnostics(script.id());
    INFO(dump(found));
    REQUIRE(found.size() == 1);
    REQUIRE(found[0].range.start.line == 3);
    REQUIRE(found[0].message.find("Nope") != std::string::npos);

    // A rename away from Door makes the first two reads unknown again.
    rig.game.set_name(door.id(), "Gate");
    settle(analysis);
    found = analysis.diagnostics(script.id());
    INFO(dump(found));
    REQUIRE(found.size() == 3);
}

// A dotted child has that child's type, so what follows the dot is checked
// too: its members, its own children, a local holding it, and a require.
TEST_CASE("A20 a dotted name is the child it reaches, with its type", "[A20]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Folder& configs = rig.game.create<engine_core::Folder>();
    rig.game.set_name(configs.id(), "Configs");
    rig.game.set_parent(configs.id(), workspace_of(rig.game));
    engine_core::GameObject& inner = rig.game.create<engine_core::GameObject>();
    rig.game.set_name(inner.id(), "SomeInstance");
    rig.game.set_parent(inner.id(), configs.id());
    // A child named like a property: the property wins, as at run time.
    engine_core::Folder& named = rig.game.create<engine_core::Folder>();
    rig.game.set_name(named.id(), "Name");
    rig.game.set_parent(named.id(), configs.id());
    engine_core::ModuleScript& config = add_module(rig.game, "Config", "return { Gold = 1 }\n");
    rig.game.set_parent(config.id(), configs.id());
    engine_core::Script& script = add_script(rig.game, "Test",
                                             "local some = workspace.Configs.SomeInstance\n"
                                             "local home = some.Transform\n"
                                             "workspace.Configs.SomeInstance.Transform = home\n"
                                             "local again = script.Parent.Configs.SomeInstance.Transform\n"
                                             "local folder = workspace.Configs\n"
                                             "local found = folder:FindFirstChild(\"SomeInstance\").Transform\n"
                                             "local label: string = workspace.Configs.Name\n"
                                             "local gold = require(workspace.Configs.Config).Gold\n"
                                             "return again, found, label, gold\n");
    settle(analysis);
    INFO(dump(analysis.diagnostics(script.id())));
    REQUIRE(analysis.diagnostics(script.id()).empty());

    SECTION("the child keeps its class") {
        script.set_source("--!strict\nlocal n: number = workspace.Configs.SomeInstance\n");
        settle(analysis);
        const std::string report = dump(analysis.diagnostics(script.id()));
        INFO(report);
        REQUIRE(report.find("'GameObject'") != std::string::npos);
    }

    SECTION("a member the child's class lacks is reported") {
        script.set_source("local s = workspace.Configs.SomeInstance.Source\nreturn s\n");
        settle(analysis);
        const std::string report = dump(analysis.diagnostics(script.id()));
        INFO(report);
        REQUIRE(report.find("Source") != std::string::npos);
    }

    SECTION("a child that is not there is reported, past the first dot too") {
        script.set_source("local a = workspace.Configs.Missing\nreturn a\n");
        settle(analysis);
        const std::string report = dump(analysis.diagnostics(script.id()));
        INFO(report);
        REQUIRE(report.find("Missing") != std::string::npos);
    }

    SECTION("a required module's type comes through a dotted path") {
        script.set_source("--!strict\nlocal gold: string = require(workspace.Configs.Config).Gold\n");
        settle(analysis);
        const std::string report = dump(analysis.diagnostics(script.id()));
        INFO(report);
        REQUIRE(report.find("Unknown require") == std::string::npos);
        REQUIRE(report.find("@1:") != std::string::npos);
    }

    SECTION("a child is read-only") {
        script.set_source("workspace.Configs.SomeInstance = nil\n");
        settle(analysis);
        INFO(dump(analysis.diagnostics(script.id())));
        REQUIRE_FALSE(analysis.diagnostics(script.id()).empty());
    }
}

// The script from the editor: every lookup reaches something in the explorer,
// so nothing is nil and there is nothing to report.
TEST_CASE("A21 FindFirstChild chains to a module in the place with no nil warning", "[A21]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Folder& modules = rig.game.create<engine_core::Folder>();
    rig.game.set_name(modules.id(), "Modules");
    rig.game.set_parent(modules.id(), workspace_of(rig.game));
    engine_core::ModuleScript& config = add_module(rig.game, "Config",
                                                   "local Config = {}\n"
                                                   "Config.Currencies = { Gold = \"Gold\" }\n"
                                                   "Config.Settings = { DeleteEveryFileOnTheComputer = false }\n"
                                                   "return Config\n");
    rig.game.set_parent(config.id(), modules.id());
    const char* source =
        "local Config = require(workspace:FindFirstChild(\"Modules\"):FindFirstChild(\"Config\"))\n"
        "\n"
        "local currency = Config.Currencies.Gold\n"
        "\n"
        "Config.Settings.DeleteEveryFileOnTheComputer = true\n"
        "\n"
        "print(\"Currency:\", currency, \"Setting:\", Config.Settings.DeleteEveryFileOnTheComputer)\n";
    engine_core::Script& script = add_script(rig.game, "Test", source);
    settle(analysis);
    INFO(dump(analysis.diagnostics(script.id())));
    REQUIRE(analysis.diagnostics(script.id()).empty());

    SECTION("in strict mode too") {
        script.set_source(std::string("--!strict\n") + source);
        settle(analysis);
        INFO(dump(analysis.diagnostics(script.id())));
        REQUIRE(analysis.diagnostics(script.id()).empty());
    }

    SECTION("renaming the folder away brings the nil warning back") {
        rig.game.set_name(modules.id(), "Elsewhere");
        settle(analysis);
        const std::string report = dump(analysis.diagnostics(script.id()));
        INFO(report);
        REQUIRE(report.find("could be nil") != std::string::npos);
    }
}

// DataModel is everything in the tree, so a Parent takes a Folder or game.
// Instance is what Instance.new makes: game is a DataModel but not an Instance,
// and only game has GetService.
TEST_CASE("A22 Parent takes any DataModel, and game is not an Instance", "[A22]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Folder& box = rig.game.create<engine_core::Folder>();
    rig.game.set_name(box.id(), "Box");
    rig.game.set_parent(box.id(), workspace_of(rig.game));
    engine_core::GameObject& tri = rig.game.create<engine_core::GameObject>();
    rig.game.set_name(tri.id(), "Tri0");
    rig.game.set_parent(tri.id(), box.id());
    const char* source =
        "local box = workspace:FindFirstChild(\"Box\")\n"
        "local tri = workspace.Box.Tri0\n"
        "tri.Parent = box\n"
        "tri.Parent = workspace\n"
        "tri.Parent = nil\n"
        "local folder = Instance.new(\"Folder\", game)\n"
        "folder.Parent = workspace\n"
        "tri.Parent = folder\n"
        "for _, child in workspace:GetChildren() do\n"
        "    child.Parent = box\n"
        "end\n"
        "local root: DataModel = game\n"
        "local made: Instance = folder\n"
        "local found: Instance? = box:FindFirstChild(\"Tri0\")\n"
        "local up: DataModel? = box.Parent\n"
        "game:GetService(\"Selection\"):Set({box, tri})\n"
        "local beat = game:GetService(\"RunService\").Heartbeat\n"
        "return root, made, found, up, beat\n";
    engine_core::Script& script = add_script(rig.game, "Parenting", source);
    settle(analysis);
    INFO(dump(analysis.diagnostics(script.id())));
    REQUIRE(analysis.diagnostics(script.id()).empty());

    SECTION("in strict mode too") {
        script.set_source(std::string("--!strict\n") + source);
        settle(analysis);
        INFO(dump(analysis.diagnostics(script.id())));
        REQUIRE(analysis.diagnostics(script.id()).empty());
    }

    SECTION("game is not an Instance") {
        script.set_source("--!strict\nlocal made: Instance = game\nreturn made\n");
        settle(analysis);
        const std::string report = dump(analysis.diagnostics(script.id()));
        INFO(report);
        REQUIRE(report.find("'Instance', but got 'Game'") != std::string::npos);
    }

    SECTION("GetService is only on game") {
        script.set_source("local service = workspace.Box:GetService(\"RunService\")\nreturn service\n");
        settle(analysis);
        const std::string report = dump(analysis.diagnostics(script.id()));
        INFO(report);
        REQUIRE(report.find("GetService") != std::string::npos);
    }

    SECTION("a value that is not an instance is still refused") {
        script.set_source("--!strict\nlocal tri = workspace.Box.Tri0\ntri.Parent = 5\n");
        settle(analysis);
        const std::string report = dump(analysis.diagnostics(script.id()));
        INFO(report);
        REQUIRE(report.find("@2:") != std::string::npos);
        REQUIRE(report.find("DataModel?") != std::string::npos);
    }

    SECTION("a missing child still reads as Instance?") {
        script.set_source("--!strict\nlocal gone = workspace:FindFirstChild(\"Nope\")\nlocal n: number = gone\n");
        settle(analysis);
        const std::string report = dump(analysis.diagnostics(script.id()));
        INFO(report);
        REQUIRE(report.find("Instance?") != std::string::npos);
    }
}

// Enabled is Script's. A module that reads its own Enabled is told it has none.
TEST_CASE("A23 a ModuleScript has no Enabled; a Script does", "[A23]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::ModuleScript& module = add_module(rig.game, "Mod", "local on = script.Enabled\nreturn { on = on }\n");
    engine_core::Script& script = add_script(rig.game, "Main",
                                             "script.Enabled = false\nlocal text: string = script.Source\n"
                                             "local mod = script.Parent.Mod\nlocal body: string = mod.Source\n"
                                             "return text, body\n");
    settle(analysis);
    INFO(dump(analysis.diagnostics(script.id())));
    REQUIRE(analysis.diagnostics(script.id()).empty());
    const std::string report = dump(analysis.diagnostics(module.id()));
    INFO(report);
    REQUIRE(report.find("Enabled") != std::string::npos);
}

TEST_CASE("A24 Enum, Vector2, and UserInputService are declared to analysis", "[A24]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Script& script = add_script(rig.game, "Input", R"(
local UserInputService = game:GetService("UserInputService")
UserInputService.InputBegan:Connect(function(input, gameProcessedEvent)
    print(input.KeyCode, input.UserInputType)
    if input.KeyCode == Enum.KeyCode.A and not gameProcessedEvent then
        print("You pressed A!", input.KeyCode.Name, input.KeyCode.Value, input.Position.Z)
    end
end)
local held: boolean = UserInputService:IsKeyDown(Enum.KeyCode.W)
local face: Vector3 = Vector3.FromNormalId(Enum.NormalId.Top) + Vector3.FromAxis(Enum.Axis.X)
local state: EnumItem = Enum.UserInputState.Begin
local mouse: Vector2 = UserInputService:GetMouseLocation()
local x: number = mouse.X + mouse.Magnitude
local moved: Vector2 = (mouse + Vector2.new(1, 2) - Vector2.one) * 2 / 2
local scaled: Vector2 = 2 * mouse
local negated: Vector2 = -mouse
local unit: Vector2 = mouse.Unit:Lerp(Vector2.zero, 0.5):Max(Vector2.xAxis)
local dot: number = mouse:Dot(Vector2.yAxis) + mouse:Cross(Vector2.one)
print(held, face, state, x, moved, scaled, negated, unit, dot, mouse == Vector2.zero)
)");
    settle(analysis);
    const std::vector<engine_core::Diagnostic> diagnostics = analysis.diagnostics(script.id());
    INFO(dump(diagnostics));
    REQUIRE(diagnostics.empty());

    script.set_source("print(Enum.KeyCode.NotAKey, Enum.NotAnEnum)\n");
    settle(analysis);
    const std::vector<engine_core::Diagnostic> unknown = analysis.diagnostics(script.id());
    INFO(dump(unknown));
    REQUIRE(unknown.size() == 2);
    REQUIRE(dump(unknown).find("NotAKey") != std::string::npos);
    // The item's error names the enum's type, not every item in it.
    REQUIRE(dump(unknown).find("EnumKeyCode") != std::string::npos);
    REQUIRE(dump(unknown).find("Backspace") == std::string::npos);
    REQUIRE(dump(unknown).find("NotAnEnum") != std::string::npos);

    script.set_source("local v = Vector2.new(1, 2) + 1\nlocal z = Vector2.zero.Z\n");
    settle(analysis);
    const std::vector<engine_core::Diagnostic> wrong = analysis.diagnostics(script.id());
    INFO(dump(wrong));
    REQUIRE(has_code(wrong, "Type"));
    REQUIRE(dump(wrong).find("@0:") != std::string::npos);
    REQUIRE(dump(wrong).find("@1:") != std::string::npos);
}

TEST_CASE("A32 Matrix4 is declared to analysis, operators and all", "[A32]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Script& script = add_script(rig.game, "Pose", R"(--!strict
local m: Matrix4 = Matrix4.new(1, 2, 3) * Matrix4.Angles(0, math.pi, 0)
local p: Vector3 = m * Vector3.new(1, 0, 0)
local moved: Matrix4 = m + Vector3.one - Vector3.xAxis
local look: Vector3 = m.LookVector + m.Position
local rx: number, ry: number, rz: number = m:ToEulerAnglesXYZ()
local axis: Vector3, angle: number = m:ToAxisAngle()
local same: boolean = m == Matrix4.identity
local blended: Matrix4 = m:Lerp(Matrix4.lookAt(Vector3.zero, Vector3.one), 0.5):Inverse()
local world: Vector3 = m:PointToWorldSpace(p)
local turned: Matrix4 = Matrix4.fromEulerAngles(1, 2, 3, Enum.RotationOrder.YXZ)
print(moved, look, rx, ry, rz, axis, angle, same, blended, world, turned, m.X)
)");
    settle(analysis);
    const std::vector<engine_core::Diagnostic> diagnostics = analysis.diagnostics(script.id());
    INFO(dump(diagnostics));
    REQUIRE(diagnostics.empty());

    script.set_source("--!strict\nlocal v: Matrix4 = Matrix4.new() * Vector3.one\nlocal n = Matrix4.new().Nope\n");
    settle(analysis);
    const std::vector<engine_core::Diagnostic> wrong = analysis.diagnostics(script.id());
    INFO(dump(wrong));
    REQUIRE(dump(wrong).find("@1:") != std::string::npos);
    REQUIRE(dump(wrong).find("@2:") != std::string::npos);
}

TEST_CASE("A25 a table type is linted without a crash, and a duplicate key still warns", "[A25]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Script& script = add_script(rig.game, "Enemies",
                                             "--!strict\n"
                                             "type Enemy = { name: string, health: number }\n"
                                             "local rates = { slow = 1, slow = 2 }\n"
                                             "local boss: Enemy = { name = \"Boss\", health = 100 }\n"
                                             "print(rates.slow, boss.name)\n");
    settle(analysis);
    const std::vector<engine_core::Diagnostic> diagnostics = analysis.diagnostics(script.id());
    INFO(dump(diagnostics));
    REQUIRE(has_code(diagnostics, "Lint/TableLiteral"));
    REQUIRE_FALSE(has_code(diagnostics, "Type"));
}

TEST_CASE("A26 a destroyed script's module leaves the checker's cache", "[A26]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    for (int i = 0; i < 4; ++i) {
        engine_core::Script& script = add_script(rig.game, "Churn", "local x = 1\n");
        settle(analysis);
        rig.game.destroy(script.id());
    }
    add_script(rig.game, "Kept", "local y = 2\n");
    settle(analysis);
    REQUIRE(analysis.cached_modules() == 1);
}

TEST_CASE("A27 --!nonstrict below a first comment line still checks unknown members", "[A27]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Script& script = add_script(rig.game, "Render",
                                             "-- Render loop\n"
                                             "--!nonstrict\n"
                                             "local run = game:GetService(\"RunService\")\n"
                                             "local blocked = run.PreRender\n"
                                             "return blocked\n");
    settle(analysis);
    const std::vector<engine_core::Diagnostic> diagnostics = analysis.diagnostics(script.id());
    INFO(dump(diagnostics));
    bool prerender = false;
    for (const engine_core::Diagnostic& diagnostic : diagnostics) {
        if (diagnostic.message.find("PreRender") != std::string::npos) {
            prerender = true;
            REQUIRE(diagnostic.severity == engine_core::Severity::Warning);
        }
    }
    REQUIRE(prerender);
}

namespace {

// The place as completion sends it: every instance, with `buffer` as the source
// of `edited`.
std::vector<engine_core::LuaNode> completion_nodes(engine_core::DataModel& game, engine_core::InstanceId edited,
                                                   const std::string& buffer) {
    std::vector<engine_core::LuaNode> nodes;
    engine_core::LuaNode root;
    root.id = 0;
    root.parent = engine_core::DataModel::kNoParent;
    root.name = game.name(0);
    root.class_name = game.class_name();
    nodes.push_back(root);
    game.for_each_instance([&](engine_core::DataModel& object) {
        if (object.id() == 0) {
            return;
        }
        engine_core::LuaNode node;
        node.id = object.id();
        node.parent = game.parent(object.id());
        node.name = game.name(object.id());
        node.class_name = object.class_name();
        if (auto* source = dynamic_cast<engine_core::LuaSource*>(&object)) {
            node.source = object.id() == edited ? buffer : source->source();
        }
        nodes.push_back(node);
    });
    return nodes;
}

}  // namespace

TEST_CASE("A28 completion against an unsaved buffer never reaches another script's diagnostics", "[A28]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::ModuleScript& module = rig.game.create<engine_core::ModuleScript>();
    rig.game.set_name(module.id(), "Mod");
    module.set_source("return { a = 1 }\n");
    rig.game.set_parent(module.id(), workspace_of(rig.game));
    engine_core::Script& script =
        add_script(rig.game, "Main", "--!strict\nlocal M = require(script.Parent.Mod)\nlocal value: number = M.b\n");
    settle(analysis);
    const std::string before = dump(analysis.diagnostics(script.id()));
    INFO(before);
    REQUIRE(before.find("b") != std::string::npos);

    // The module is being edited to add b, and the edit is not saved.
    const std::string buffer = "return { a = 1, b = 2 }\n";
    for (int round = 0; round < 3; ++round) {
        const engine_core::LuauFacts asked = analysis.luau_facts(completion_nodes(rig.game, module.id(), buffer),
                                                                 module.id(), buffer, buffer.size(), {},
                                                                 std::chrono::seconds(20));
        REQUIRE(asked.ran);
        analysis.invalidate(script.id());
        settle(analysis);
        INFO(dump(analysis.diagnostics(script.id())));
        REQUIRE(dump(analysis.diagnostics(script.id())) == before);
    }

    // Once the edit is saved, the script sees it.
    module.set_source(buffer);
    settle(analysis);
    INFO(dump(analysis.diagnostics(script.id())));
    REQUIRE(dump(analysis.diagnostics(script.id())).find("M.b") == std::string::npos);
    REQUIRE_FALSE(has_code(analysis.diagnostics(script.id()), "Type"));
}

TEST_CASE("A29 a completion snapshot in another sibling order leaves no stale place types", "[A29]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    // Two folders named Dup. The second one made comes first among the siblings,
    // and only it holds M, so workspace.Dup.M reads through it.
    engine_core::Folder& first_made = rig.game.create<engine_core::Folder>();
    rig.game.set_name(first_made.id(), "Dup");
    engine_core::Folder& second_made = rig.game.create<engine_core::Folder>();
    rig.game.set_name(second_made.id(), "Dup");
    rig.game.set_parent(second_made.id(), workspace_of(rig.game));
    rig.game.set_parent(first_made.id(), workspace_of(rig.game));
    engine_core::Folder& inner = rig.game.create<engine_core::Folder>();
    rig.game.set_name(inner.id(), "M");
    rig.game.set_parent(inner.id(), second_made.id());
    engine_core::Script& script = add_script(rig.game, "Main", "--!strict\nlocal found = workspace.Dup.M\nprint(found)\n");
    settle(analysis);
    const std::string clean = dump(analysis.diagnostics(script.id()));
    INFO(clean);
    REQUIRE_FALSE(has_code(analysis.diagnostics(script.id()), "Type"));

    // The tree changes, and a completion request arrives before analysis
    // checks it, with its instances in slot order: the first-made Dup first.
    engine_core::Folder& extra = rig.game.create<engine_core::Folder>();
    rig.game.set_parent(extra.id(), workspace_of(rig.game));
    std::vector<engine_core::LuaNode> slot_order = completion_nodes(rig.game, script.id(), script.source());
    const engine_core::LuauFacts asked = analysis.luau_facts(slot_order, script.id(), script.source(),
                                                             script.source().size(), {}, std::chrono::seconds(20));
    REQUIRE(asked.ran);

    // Analysis then checks the tree in its own order, and workspace.Dup is still
    // the Dup that holds M.
    analysis.note_world_changed();
    analysis.invalidate(script.id());
    settle(analysis);
    INFO(dump(analysis.diagnostics(script.id())));
    REQUIRE_FALSE(has_code(analysis.diagnostics(script.id()), "Type"));
}

// Asset classes and their reference properties: Texture, Material, and the
// Assets service tree resolve through the same registered classes as everything
// else, so a script that uses them type-checks clean. The assets are placed in
// the explorer, as Containment requires, and read back by dotted name so the
// annotations exercise Texture? without depending on Instance.new's narrowing.
TEST_CASE("A31 a script that uses assets and their references type-checks", "[A31]") {
    ScriptRig rig;
    engine_core::Texture& tex = rig.game.create<engine_core::Texture>();
    rig.game.set_name(tex.id(), "Tex");
    rig.game.set_parent(tex.id(), rig.game.service("Textures"));
    engine_core::Material& mat = rig.game.create<engine_core::Material>();
    rig.game.set_name(mat.id(), "Mat");
    rig.game.set_parent(mat.id(), rig.game.service("Materials"));
    engine_core::Prefab& prefab = rig.game.create<engine_core::Prefab>();
    rig.game.set_name(prefab.id(), "Prefab");
    rig.game.set_parent(prefab.id(), rig.game.service("Prefabs"));
    engine_core::Model& model = rig.game.create<engine_core::Model>();
    rig.game.set_name(model.id(), "Mdl");
    rig.game.set_parent(model.id(), prefab.id());

    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Script& script = add_script(rig.game, "Assets",
                                             "local tex: Texture = game.Assets.Textures.Tex\n"
                                             "tex.Path = \"textures/brick.png\"\n"
                                             "local mat = game.Assets.Materials.Mat\n"
                                             "mat.DiffuseTexture = tex\n"
                                             "local same: Texture? = mat.DiffuseTexture\n"
                                             "local assets = game:GetService(\"Assets\")\n"
                                             "local textures = game.Assets.Textures\n"
                                             "local model = game.Assets.Prefabs.Prefab.Mdl\n"
                                             "model.Mesh = nil\n"
                                             "model.Material = mat\n"
                                             "print(assets, textures, same)\n");
    settle(analysis);
    INFO(dump(analysis.diagnostics(script.id())));
    REQUIRE(analysis.diagnostics(script.id()).empty());

    SECTION("assigning the wrong instance type to a reference property is a type error") {
        script.set_source("--!strict\n"
                          "local mat = game.Assets.Materials.Mat\n"
                          "mat.DiffuseTexture = workspace\n");
        settle(analysis);
        const std::vector<engine_core::Diagnostic> diagnostics = analysis.diagnostics(script.id());
        INFO(dump(diagnostics));
        REQUIRE(has_code(diagnostics, "Type"));
    }
}

TEST_CASE("A30 a module the analyzer checked still reads as the functions it replaced", "[A30]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    add_module(rig.game, "Mod",
               "local module = {}\n"
               "function module.new()\n    return \"nope\"\nend\n"
               "module.new = function()\n    return 1\nend\n"
               "function module:Test()\nend\n"
               "module.Test = function()\nend\n"
               "return module\n");
    const std::string use = "local M = require(script.Parent.Mod)\nlocal x = M.new()\n";
    engine_core::Script& script = add_script(rig.game, "Main", use.c_str());
    // The analyzer checks the module first, as it does for an open script.
    settle(analysis);

    const engine_core::LuauFacts typed =
        analysis.luau_facts(completion_nodes(rig.game, script.id(), use), script.id(), use, std::string::npos,
                            {use.find("x =")}, std::chrono::seconds(20));
    REQUIRE(typed.ran);
    REQUIRE(typed.types.size() == 1);
    CHECK(typed.types[0].described.type == "number");

    const std::string colon = use + "M:";
    const engine_core::LuauFacts members = analysis.luau_facts(completion_nodes(rig.game, script.id(), colon),
                                                               script.id(), colon, colon.size(), {},
                                                               std::chrono::seconds(20));
    REQUIRE(members.ran);
    bool listed = false;
    for (const engine_core::LuauSuggestion& row : members.completion.items) {
        if (row.name == "Test") {
            listed = true;
            CHECK_FALSE(row.method);
        }
    }
    CHECK(listed);
}

TEST_CASE("A33 ChangeHistoryService's documented use type-checks, with GetCanUndo's two values", "[A33]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Script& script = add_script(rig.game, "Uses", R"(
local history = game:GetService("ChangeHistoryService")
local can, name = history:GetCanUndo()
local again, other = history:GetCanRedo()
print(can, name, again, other)
local id = history:TryBeginRecording("Paint")
if id then
    history:FinishRecording(id, Enum.FinishRecordingOperation.Commit)
end
history.OnUndo:Connect(function(stepName) print(stepName) end)
)");
    settle(analysis);
    const std::vector<engine_core::Diagnostic> diagnostics = analysis.diagnostics(script.id());
    INFO(dump(diagnostics));
    REQUIRE(diagnostics.empty());
}

TEST_CASE("A34 an editor request is answered while a long check runs", "[A34]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Script& small = add_script(rig.game, "Small", "local x = 1\n");
    settle(analysis);
    add_script(rig.game, "Huge", long_source(60000, false, 0).c_str());
    // Pumping clears world_stale and moves Huge from pending into the running
    // check, so busy() below reflects real checker work, not the stale flag.
    for (const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
         std::chrono::steady_clock::now() < deadline;) {
        analysis.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(analysis.busy());
    const engine_core::LuauFacts asked =
        analysis.luau_facts(completion_nodes(rig.game, small.id(), small.source()), small.id(), small.source(),
                            small.source().size(), {}, std::chrono::seconds(20));
    REQUIRE(asked.ran);
    // Answered before Huge's check finished, not after it.
    REQUIRE(analysis.busy());
    settle(analysis);
}

namespace {

// Ten modules, every other one with a strict-mode error, and twenty scripts that
// require them and each have a type error and an unknown global. Returns the
// modules' ids, then the scripts'.
std::vector<engine_core::InstanceId> build_place(engine_core::DataModel& game) {
    std::vector<engine_core::InstanceId> ids;
    for (int i = 0; i < 10; ++i) {
        const std::string name = "Mod" + std::to_string(i);
        const char* source = i % 2 == 0 ? "local M = {}\n"
                                          "function M.add(a: number, b: number): number\n"
                                          "    return a + b\n"
                                          "end\n"
                                          "return M\n"
                                        : "--!strict\n"
                                          "local M = {}\n"
                                          "local wrong: number = \"x\"\n"
                                          "function M.add(a: number, b: number): number\n"
                                          "    return a + b + wrong\n"
                                          "end\n"
                                          "return M\n";
        ids.push_back(add_module(game, name.c_str(), source).id());
    }
    for (int j = 0; j < 20; ++j) {
        const std::string name = "User" + std::to_string(j);
        const std::string source = "local M = require(workspace.Mod" + std::to_string(j % 10) +
                                   ")\nprint(M.add(1, \"two\"))\nprint(undefined" + std::to_string(j) + ")\n";
        ids.push_back(add_script(game, name.c_str(), source.c_str()).id());
    }
    return ids;
}

std::vector<std::string> place_report(unsigned threads) {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game, threads);
    REQUIRE(analysis.threads() == threads);
    const std::vector<engine_core::InstanceId> ids = build_place(rig.game);
    settle(analysis);
    std::vector<std::string> out;
    for (engine_core::InstanceId id : ids) {
        out.push_back(std::string(rig.game.name(id)) + "\n" + dump(analysis.diagnostics(id)));
    }
    return out;
}

bool contains(const std::vector<engine_core::InstanceId>& ids, engine_core::InstanceId id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

}  // namespace

TEST_CASE("A35 a script's reached set is the instances its expressions are typed as", "[A35]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Folder& lights = rig.game.create<engine_core::Folder>();
    rig.game.set_name(lights.id(), "Lights");
    rig.game.set_parent(lights.id(), workspace_of(rig.game));
    engine_core::GameObject& lamp = rig.game.create<engine_core::GameObject>();
    rig.game.set_name(lamp.id(), "Lamp");
    rig.game.set_parent(lamp.id(), lights.id());
    engine_core::GameObject& crate = rig.game.create<engine_core::GameObject>();
    rig.game.set_name(crate.id(), "Crate");
    rig.game.set_parent(crate.id(), workspace_of(rig.game));
    // Lamp is reached only through a parameter and FindFirstChild.
    engine_core::Script& script = add_script(rig.game, "Show",
                                             "local folder = workspace.Lights\n"
                                             "local function show(f: typeof(folder))\n"
                                             "    print(f:FindFirstChild(\"Lamp\"))\n"
                                             "end\n"
                                             "show(folder)\n");
    settle(analysis);
    const std::vector<engine_core::InstanceId> reached = analysis.reached(script.id());
    REQUIRE(contains(reached, workspace_of(rig.game)));
    REQUIRE(contains(reached, lights.id()));
    REQUIRE(contains(reached, lamp.id()));
    REQUIRE_FALSE(contains(reached, crate.id()));
    REQUIRE(analysis.checks(script.id()) >= 1);
}

TEST_CASE("A36 checking on several threads finds what checking on one does", "[A36]") {
    const std::vector<std::string> serial = place_report(1);
    const std::vector<std::string> parallel = place_report(4);
    REQUIRE(serial == parallel);
    // The first script found its type error, so the comparison is about something.
    INFO(serial[10]);
    REQUIRE(serial[10].find("Type") != std::string::npos);
}

TEST_CASE("A37 turning analysis off or destroying it during a batch stops cleanly", "[A37]") {
    ScriptRig rig;
    SECTION("off during a batch, then on again") {
        engine_core::ScriptAnalysis analysis(rig.game);
        const std::vector<engine_core::InstanceId> ids = build_place(rig.game);
        add_script(rig.game, "Huge", long_source(20000, false, 0).c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        analysis.set_enabled(false);
        settle(analysis);
        REQUIRE(analysis.diagnostics().empty());
        analysis.set_enabled(true);
        settle(analysis);
        REQUIRE_FALSE(analysis.diagnostics(ids[10]).empty());
    }
    SECTION("destroyed during a batch") {
        auto analysis = std::make_unique<engine_core::ScriptAnalysis>(rig.game);
        build_place(rig.game);
        add_script(rig.game, "Huge", long_source(20000, false, 0).c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto started = std::chrono::steady_clock::now();
        analysis.reset();
        REQUIRE(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    }
}

TEST_CASE("A38 a require cycle and a module with a syntax error still finish", "[A38]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game, 4);
    engine_core::ModuleScript& a = add_module(rig.game, "A", "local B = require(script.Parent.B)\nreturn {}\n");
    engine_core::ModuleScript& b = add_module(rig.game, "B", "local A = require(script.Parent.A)\nreturn {}\n");
    engine_core::ModuleScript& broken = add_module(rig.game, "Broken", "return {\n");
    engine_core::Script& user =
        add_script(rig.game, "User", "local Broken = require(workspace.Broken)\nprint(Broken, undefinedName)\n");
    settle(analysis);
    REQUIRE(analysis.analyzed_source(a.id()).has_value());
    REQUIRE(analysis.analyzed_source(b.id()).has_value());
    INFO(dump(analysis.diagnostics(broken.id())));
    REQUIRE(has_code(analysis.diagnostics(broken.id()), "Syntax"));
    INFO(dump(analysis.diagnostics(user.id())));
    REQUIRE(has_code(analysis.diagnostics(user.id()), "Lint/UnknownGlobal"));
}

TEST_CASE("A39 a script edited while a batch checks it ends with its newest source", "[A39]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game, 4);
    add_script(rig.game, "Huge", long_source(20000, false, 0).c_str());
    engine_core::Script& script = add_script(rig.game, "Edited", "print(first)\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    script.set_source("print(second)\n");
    settle(analysis);
    REQUIRE(analysis.analyzed_source(script.id()) == std::optional<std::string>("print(second)\n"));
    const std::string report = dump(analysis.diagnostics(script.id()));
    INFO(report);
    // Luau's message ends "consider assigning to it first", so look for the quoted name.
    REQUIRE(report.find("'second'") != std::string::npos);
    REQUIRE(report.find("'first'") == std::string::npos);
}

TEST_CASE("A40 a script destroyed before a batch reaches it as a dependent is never published again", "[A40]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game, 4);
    engine_core::ModuleScript& module = add_module(rig.game, "Shared", "return { value = 1 }\n");
    const engine_core::InstanceId user =
        add_script(rig.game, "User", "local Shared = require(workspace.Shared)\nprint(Shared.value)\n").id();
    settle(analysis);
    REQUIRE(analysis.analyzed_source(user).has_value());
    // The pump() after the edit captures a tree that still holds User.
    // Destroying User takes no capture of its own, so the batch the edit starts
    // finds User in that tree as a dependent of Shared after User is gone.
    module.set_source("return { value = 2 }\n");
    analysis.pump();
    rig.game.destroy(user);
    REQUIRE_FALSE(analysis.analyzed_source(user).has_value());
    // No pump yet: pump() would capture a new tree before the batch starts.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    settle(analysis);
    REQUIRE_FALSE(analysis.analyzed_source(user).has_value());
    REQUIRE(analysis.diagnostics(user).empty());
    REQUIRE(analysis.analyzed_source(module.id()) == std::optional<std::string>("return { value = 2 }\n"));
}

TEST_CASE("A41 a module's instance types still match after a rename it does not reach", "[A41]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::GameObject& door = rig.game.create<engine_core::GameObject>();
    rig.game.set_name(door.id(), "Door");
    rig.game.set_parent(door.id(), workspace_of(rig.game));
    engine_core::Folder& props = rig.game.create<engine_core::Folder>();
    rig.game.set_name(props.id(), "Props");
    rig.game.set_parent(props.id(), workspace_of(rig.game));
    engine_core::GameObject& crate = rig.game.create<engine_core::GameObject>();
    rig.game.set_name(crate.id(), "Crate");
    rig.game.set_parent(crate.id(), props.id());
    engine_core::ModuleScript& doors =
        add_module(rig.game, "Doors", "--!strict\nlocal Doors = {}\nDoors.main = workspace.Door\nreturn Doors\n");
    engine_core::Script& user = add_script(rig.game, "User",
                                           "--!strict\n"
                                           "local Doors = require(workspace.Doors)\n"
                                           "local door: typeof(workspace.Door) = Doors.main\n"
                                           "print(door)\n");
    settle(analysis);
    INFO(dump(analysis.diagnostics(user.id())));
    REQUIRE(analysis.diagnostics(user.id()).empty());
    const std::uint64_t doors_checks = analysis.checks(doors.id());

    // Inside Props, which neither script reached: Doors keeps its cached types,
    // and User is checked again against them.
    rig.game.set_name(crate.id(), "Box");
    user.set_source(user.source() + "\n");
    settle(analysis);
    REQUIRE(analysis.checks(doors.id()) == doors_checks);
    INFO(dump(analysis.diagnostics(user.id())));
    REQUIRE(analysis.diagnostics(user.id()).empty());
}

TEST_CASE("A42 a change rechecks only the scripts it can affect", "[A42]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    const engine_core::InstanceId workspace = workspace_of(rig.game);
    const auto folder = [&rig](const char* name, engine_core::InstanceId parent) -> engine_core::Folder& {
        engine_core::Folder& made = rig.game.create<engine_core::Folder>();
        rig.game.set_name(made.id(), name);
        rig.game.set_parent(made.id(), parent);
        return made;
    };
    const auto part = [&rig](const char* name, engine_core::InstanceId parent) -> engine_core::GameObject& {
        engine_core::GameObject& made = rig.game.create<engine_core::GameObject>();
        rig.game.set_name(made.id(), name);
        rig.game.set_parent(made.id(), parent);
        return made;
    };
    engine_core::Folder& props = folder("Props", workspace);
    engine_core::GameObject& crate = part("Crate", props.id());
    engine_core::Folder& lights = folder("Lights", workspace);
    engine_core::GameObject& lamp = part("Lamp", lights.id());
    engine_core::Folder& extra = folder("Extra", workspace);
    engine_core::ModuleScript& util = add_module(rig.game, "Util",
                                                 "local Util = {}\n"
                                                 "function Util.add(a: number, b: number): number\n"
                                                 "    return a + b\n"
                                                 "end\n"
                                                 "return Util\n");
    engine_core::Script& uses_util =
        add_script(rig.game, "UsesUtil", "--!strict\nlocal Util = require(workspace.Util)\nprint(Util.add(1, 2))\n");
    engine_core::Script& uses_props =
        add_script(rig.game, "UsesProps", "--!strict\nlocal crate = workspace.Props.Crate\nprint(crate)\n");
    engine_core::Script& uses_lamp = add_script(rig.game, "UsesLamp",
                                                "--!strict\n"
                                                "local lights = workspace.Lights\n"
                                                "local function show(folder: typeof(lights))\n"
                                                "    print(folder.Lamp)\n"
                                                "end\n"
                                                "show(lights)\n");
    engine_core::Script& plain = add_script(rig.game, "Plain", "print(\"hi\")\n");
    settle(analysis);
    const std::vector<engine_core::InstanceId> watched{util.id(), uses_util.id(), uses_props.id(), uses_lamp.id(),
                                                       plain.id()};
    for (engine_core::InstanceId id : watched) {
        INFO(rig.game.name(id) << "\n" << dump(analysis.diagnostics(id)));
        REQUIRE(analysis.diagnostics(id).empty());
    }
    const auto counts = [&] {
        std::vector<std::uint64_t> out;
        for (engine_core::InstanceId id : watched) {
            out.push_back(analysis.checks(id));
        }
        return out;
    };
    // Which of util, uses_util, uses_props, uses_lamp, plain were checked again since `before`.
    const auto rechecked = [&](const std::vector<std::uint64_t>& before) {
        const std::vector<std::uint64_t> now = counts();
        std::vector<bool> out;
        for (std::size_t i = 0; i < now.size(); ++i) {
            out.push_back(now[i] != before[i]);
        }
        return out;
    };
    const std::vector<std::uint64_t> before = counts();

    SECTION("editing a module rechecks what requires it, and nothing else") {
        // Luau does not flag an extra argument, so the edit changes a parameter's type.
        util.set_source("local Util = {}\nfunction Util.add(a: number, b: string): number\n    return a\nend\nreturn Util\n");
        settle(analysis);
        REQUIRE(rechecked(before) == std::vector<bool>{true, true, false, false, false});
        REQUIRE_FALSE(analysis.diagnostics(uses_util.id()).empty());
    }
    SECTION("a rename inside a folder rechecks only the scripts that reached the folder") {
        rig.game.set_name(crate.id(), "Box");
        settle(analysis);
        REQUIRE(rechecked(before) == std::vector<bool>{false, false, true, false, false});
        REQUIRE_FALSE(analysis.diagnostics(uses_props.id()).empty());
    }
    SECTION("a child reached through a parameter is followed both ways") {
        rig.game.set_name(lamp.id(), "Bulb");
        settle(analysis);
        REQUIRE(rechecked(before) == std::vector<bool>{false, false, false, true, false});
        REQUIRE_FALSE(analysis.diagnostics(uses_lamp.id()).empty());
        rig.game.set_name(lamp.id(), "Lamp");
        settle(analysis);
        REQUIRE(analysis.diagnostics(uses_lamp.id()).empty());
    }
    SECTION("a property change rechecks nothing") {
        crate.set_position(engine_core::Vec3{1.f, 2.f, 3.f});
        settle(analysis);
        REQUIRE(rechecked(before) == std::vector<bool>{false, false, false, false, false});
    }
    SECTION("a script added where nothing looked checks only itself") {
        engine_core::Script& added = add_script(rig.game, extra.id(), "Added", "print(1)\n");
        settle(analysis);
        REQUIRE(rechecked(before) == std::vector<bool>{false, false, false, false, false});
        REQUIRE(analysis.checks(added.id()) >= 1);
    }
    SECTION("destroying a module rechecks what required it") {
        rig.game.destroy(util.id());
        settle(analysis);
        REQUIRE(rechecked(before)[1]);
        REQUIRE_FALSE(rechecked(before)[4]);
        REQUIRE_FALSE(analysis.diagnostics(uses_util.id()).empty());
        REQUIRE_FALSE(analysis.analyzed_source(util.id()).has_value());
    }
    SECTION("destroying a folder of scripts drops them and rechecks what required them") {
        engine_core::Folder& group = folder("Group", extra.id());
        engine_core::ModuleScript& inner = add_module(rig.game, "Inner", "return { value = 1 }\n");
        rig.game.set_parent(inner.id(), group.id());
        engine_core::Script& uses_inner = add_script(rig.game, extra.id(), "UsesInner",
                                                     "--!strict\nlocal Inner = require(script.Parent.Group.Inner)\n"
                                                     "print(Inner.value)\n");
        settle(analysis);
        REQUIRE(analysis.diagnostics(uses_inner.id()).empty());
        const std::vector<std::uint64_t> grouped = counts();
        rig.game.destroy(group.id());
        settle(analysis);
        REQUIRE_FALSE(analysis.analyzed_source(inner.id()).has_value());
        REQUIRE_FALSE(analysis.diagnostics(uses_inner.id()).empty());
        REQUIRE(rechecked(grouped) == std::vector<bool>{false, false, false, false, false});
    }
    SECTION("undoing the destroy of a folder of scripts checks them again") {
        engine_core::Folder& group = folder("Group", extra.id());
        engine_core::ModuleScript& inner = add_module(rig.game, "Inner", "return { value = 1 }\n");
        rig.game.set_parent(inner.id(), group.id());
        engine_core::Script& uses_inner = add_script(rig.game, extra.id(), "UsesInner",
                                                     "--!strict\nlocal Inner = require(script.Parent.Group.Inner)\n"
                                                     "print(Inner.value)\n");
        settle(analysis);
        REQUIRE(analysis.diagnostics(uses_inner.id()).empty());
        begin_step(rig.game, "Delete");
        rig.game.destroy(group.id());
        end_step(rig.game);
        settle(analysis);
        REQUIRE_FALSE(analysis.analyzed_source(inner.id()).has_value());
        REQUIRE_FALSE(analysis.diagnostics(uses_inner.id()).empty());
        const std::vector<std::uint64_t> destroyed = counts();
        rig.game.history().undo();
        settle(analysis);
        REQUIRE(rig.game.parent(inner.id()) == group.id());
        REQUIRE(analysis.analyzed_source(inner.id()).has_value());
        INFO(dump(analysis.diagnostics(uses_inner.id())));
        REQUIRE(analysis.diagnostics(uses_inner.id()).empty());
        REQUIRE(rechecked(destroyed) == std::vector<bool>{false, false, false, false, false});
    }
    SECTION("a reparent rechecks only the scripts that reached either folder") {
        rig.game.set_parent(crate.id(), lights.id());
        settle(analysis);
        REQUIRE(rechecked(before) == std::vector<bool>{false, false, true, true, false});
        REQUIRE_FALSE(analysis.diagnostics(uses_props.id()).empty());
    }
    SECTION("a module leaving the place through its parent is dropped, and what required it rechecked") {
        rig.game.set_parent(util.id(), engine_core::DataModel::kNoParent);
        settle(analysis);
        REQUIRE_FALSE(analysis.analyzed_source(util.id()).has_value());
        REQUIRE(analysis.diagnostics(util.id()).empty());
        REQUIRE(rechecked(before)[1]);
        REQUIRE_FALSE(rechecked(before)[4]);
        REQUIRE_FALSE(analysis.diagnostics(uses_util.id()).empty());
        // Back in the place, it is checked again.
        rig.game.set_parent(util.id(), workspace);
        settle(analysis);
        REQUIRE(analysis.analyzed_source(util.id()).has_value());
        REQUIRE(analysis.diagnostics(uses_util.id()).empty());
    }
    SECTION("a module added under a name a script requires rechecks the script") {
        engine_core::Script& by_name = add_script(rig.game, "ByName",
                                                  "--!strict\nlocal Missing = require(\"Missing\")\n"
                                                  "print(Missing.value)\n");
        settle(analysis);
        INFO(dump(analysis.diagnostics(by_name.id())));
        REQUIRE_FALSE(analysis.diagnostics(by_name.id()).empty());
        const std::uint64_t by_name_checks = analysis.checks(by_name.id());
        engine_core::ModuleScript& missing = add_module(rig.game, "Missing", "return { value = 1 }\n");
        rig.game.set_parent(missing.id(), extra.id());
        settle(analysis);
        REQUIRE(analysis.checks(by_name.id()) > by_name_checks);
        INFO(dump(analysis.diagnostics(by_name.id())));
        REQUIRE(analysis.diagnostics(by_name.id()).empty());
        // Renamed away, the require fails again.
        rig.game.set_name(missing.id(), "Gone");
        settle(analysis);
        REQUIRE_FALSE(analysis.diagnostics(by_name.id()).empty());
    }
    SECTION("undoing a rename rechecks what the rename did") {
        begin_step(rig.game, "Rename");
        rig.game.set_name(crate.id(), "Box");
        end_step(rig.game);
        settle(analysis);
        REQUIRE_FALSE(analysis.diagnostics(uses_props.id()).empty());
        const std::vector<std::uint64_t> renamed = counts();
        rig.game.history().undo();
        settle(analysis);
        REQUIRE(rechecked(renamed) == std::vector<bool>{false, false, true, false, false});
        REQUIRE(analysis.diagnostics(uses_props.id()).empty());
    }
}

TEST_CASE("A43 a playtest's tree changes leave the authored results as they were", "[A43]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Folder& holder = rig.game.create<engine_core::Folder>();
    rig.game.set_name(holder.id(), "Holder");
    rig.game.set_parent(holder.id(), workspace_of(rig.game));
    engine_core::Script& kept =
        add_script(rig.game, "Kept", "--!strict\nlocal value: number = \"nope\"\nprint(workspace.Holder.Moved)\n");
    engine_core::Script& doomed = add_script(rig.game, "Doomed", "--!strict\nprint(workspace.Kept.Name)\n");
    engine_core::Script& moved = add_script(rig.game, holder.id(), "Moved", "--!strict\nprint(script.Parent.Name)\n");
    settle(analysis);
    const std::vector<engine_core::InstanceId> authored{kept.id(), doomed.id(), moved.id()};
    std::vector<std::string> before;
    for (engine_core::InstanceId id : authored) {
        REQUIRE(analysis.analyzed_source(id).has_value());
        before.push_back(*analysis.analyzed_source(id) + "\n" + dump(analysis.diagnostics(id)));
    }
    REQUIRE(has_code(analysis.diagnostics(kept.id()), "Type"));

    rig.game.start_simulation();
    rig.frames(1);
    rig.game.destroy(doomed.id());
    rig.game.set_parent(moved.id(), engine_core::DataModel::kNoParent);
    const engine_core::InstanceId runtime =
        add_script(rig.game, "Runtime", "--!strict\nprint(workspace.Kept.Name)\n").id();
    settle(analysis);
    rig.game.stop_simulation();
    settle(analysis);

    REQUIRE_FALSE(analysis.analyzed_source(runtime).has_value());
    REQUIRE(analysis.diagnostics(runtime).empty());
    for (std::size_t at = 0; at < authored.size(); ++at) {
        INFO(rig.game.name(authored[at]));
        REQUIRE(analysis.analyzed_source(authored[at]).has_value());
        REQUIRE(*analysis.analyzed_source(authored[at]) + "\n" + dump(analysis.diagnostics(authored[at])) ==
                before[at]);
    }
}

namespace {

double settle_ms(engine_core::ScriptAnalysis& analysis) {
    const auto started = std::chrono::steady_clock::now();
    settle(analysis);
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

}  // namespace

// Hidden: run with ./build/sandbox "[.perf]". Each time includes the 75 ms debounce.
TEST_CASE("A-perf timing of a large script and a large place", "[.perf]") {
    for (bool strict : {false, true}) {
        ScriptRig rig;
        engine_core::ScriptAnalysis analysis(rig.game);
        const std::string source = long_source(1000, strict, 0);
        engine_core::Script& script = add_script(rig.game, "Big", source.c_str());
        const double first = settle_ms(analysis);
        script.set_source(source + "\n-- edit\n");
        const double again = settle_ms(analysis);
        std::printf("1000 lines %s: first %.0f ms, after an edit %.0f ms\n", strict ? "strict" : "nonstrict", first,
                    again);
    }
    for (unsigned threads : {1u, 0u}) {
        ScriptRig rig;
        engine_core::ScriptAnalysis analysis(rig.game, threads);
        std::vector<engine_core::Script*> scripts;
        for (int i = 0; i < 20; ++i) {
            const std::string name = "Big" + std::to_string(i);
            scripts.push_back(&add_script(rig.game, name.c_str(), long_source(1000, false, i).c_str()));
        }
        const double place = settle_ms(analysis);
        scripts[0]->set_source(scripts[0]->source() + "\n-- edit\n");
        const double edit = settle_ms(analysis);
        engine_core::Folder& away = rig.game.create<engine_core::Folder>();
        rig.game.set_parent(away.id(), scripts[0]->id());
        const std::uint64_t before = analysis.checks(scripts[1]->id());
        rig.game.set_name(away.id(), "Renamed");
        const double rename = settle_ms(analysis);
        std::printf("20 x 1000 lines on %u threads: place %.0f ms, one edit %.0f ms, unrelated rename %.0f ms "
                    "(script 2 rechecked: %s)\n",
                    analysis.threads(), place, edit, rename,
                    analysis.checks(scripts[1]->id()) != before ? "yes" : "no");
    }
}

// Hidden: run with ./build/sandbox "[.perf]". The cost of a change in a large
// place: 16000 plain instances, near the most a place holds, and 50 small
// scripts. Each time is the change itself, then until analysis settles, which
// includes the debounce.
TEST_CASE("A-perf a change in a large place", "[.perf]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    const engine_core::InstanceId workspace = workspace_of(rig.game);
    std::vector<engine_core::Script*> scripts;
    for (int i = 0; i < 50; ++i) {
        const std::string name = "Base" + std::to_string(i);
        const std::string source = "local found = workspace:FindFirstChild(\"Group" + std::to_string(i) +
                                   "\")\nprint(found)\n";
        scripts.push_back(&add_script(rig.game, name.c_str(), source.c_str()));
    }
    for (int group = 0; group < 160; ++group) {
        engine_core::Folder& folder = rig.game.create<engine_core::Folder>();
        rig.game.set_name(folder.id(), "Group" + std::to_string(group));
        for (int i = 0; i < 99; ++i) {
            engine_core::GameObject& part = rig.game.create<engine_core::GameObject>();
            rig.game.set_name(part.id(), "Part" + std::to_string(i));
            rig.game.set_parent(part.id(), folder.id());
        }
        rig.game.set_parent(folder.id(), workspace);
    }
    settle(analysis);

    // A model of 50 scripts, built outside the place and then added at once, as a paste is.
    auto started = std::chrono::steady_clock::now();
    engine_core::Folder& model = rig.game.create<engine_core::Folder>();
    rig.game.set_name(model.id(), "Model");
    std::vector<engine_core::InstanceId> pasted;
    for (int i = 0; i < 50; ++i) {
        const std::string name = "Pasted" + std::to_string(i);
        pasted.push_back(add_script(rig.game, model.id(), name.c_str(), "local x = script.Parent\nprint(x)\n").id());
    }
    rig.game.set_parent(model.id(), workspace);
    const double add_edit = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    const double add_settle = settle_ms(analysis);
    for (engine_core::InstanceId id : pasted) {
        REQUIRE(analysis.analyzed_source(id).has_value());
    }

    started = std::chrono::steady_clock::now();
    scripts[0]->set_source(scripts[0]->source() + "\n-- edit\n");
    const double one_edit = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    const double one_settle = settle_ms(analysis);
    std::size_t instances = 0;
    rig.game.for_each_instance([&instances](engine_core::DataModel&) { ++instances; });
    std::printf("%zu instances, 50 scripts: adding a model of 50 scripts %.2f ms to change, %.0f ms to settle; "
                "one script edit %.3f ms to change, %.0f ms to settle\n",
                instances, add_edit, add_settle, one_edit, one_settle);
}
