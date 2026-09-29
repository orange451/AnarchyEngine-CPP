#include "DataModel.hpp"
#include "Folder.hpp"
#include "Game.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"
#include "LuaSource.hpp"
#include "ModuleScript.hpp"
#include "Project.hpp"
#include "Script.hpp"
#include "ScriptAnalysis.hpp"
#include "ScriptRuntime.hpp"
#include "TaskScheduler.hpp"
#include "TestTriangle.hpp"
#include "types.hpp"

#include "support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
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
    rig.game.set_parent(module.id(), rig.game.id());
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
    engine_core::TestTriangle& triangle = rig.game.create<engine_core::TestTriangle>();
    rig.game.set_name(triangle.id(), "Tri0");
    rig.game.set_parent(triangle.id(), rig.game.id());
    engine_core::ScriptAnalysis analysis(rig.game);

    engine_core::Script& bare = add_script(rig.game, "Bare",
                                            "local tri = game:FindFirstChild(\"Tri0\")\n"
                                            "local home = tri.Position\n"
                                            "return home\n");
    settle(analysis);
    INFO(dump(analysis.diagnostics(bare.id())));
    REQUIRE(analysis.diagnostics(bare.id()).empty());

    SECTION("the type has no nil in it") {
        bare.set_source("--!strict\nlocal tri = game:FindFirstChild(\"Tri0\")\nlocal n: number = tri\n");
        settle(analysis);
        const std::string report = dump(analysis.diagnostics(bare.id()));
        INFO(report);
        REQUIRE(report.find("'TestTriangle'") != std::string::npos);
        REQUIRE(report.find("TestTriangle?") == std::string::npos);
    }

    SECTION("a name that is not in the place may be nil") {
        bare.set_source("local tri = game:FindFirstChild(\"Nope\")\nlocal name = tri.Name\nreturn name\n");
        settle(analysis);
        const std::string report = dump(analysis.diagnostics(bare.id()));
        INFO(report);
        REQUIRE(report.find("could be nil") != std::string::npos);
    }

    engine_core::Script& hop = add_script(rig.game, "Hop",
                                           "local tri = game:FindFirstChild(\"Tri0\")\n"
                                           "assert(tri)\n"
                                           "local home = tri.Position\n"
                                           "tri.Position = home + Vector3.new(0.45, 0, 0)\n"
                                           "return home\n");
    settle(analysis);
    INFO(dump(analysis.diagnostics(hop.id())));
    REQUIRE(analysis.diagnostics(hop.id()).empty());

    engine_core::Script& missing = add_script(rig.game, "Missing",
                                               "local tri = game:FindFirstChild(\"Nope\")\n"
                                               "assert(tri)\n"
                                               "local home = tri.Position\n"
                                               "return home\n");
    settle(analysis);
    const std::vector<engine_core::Diagnostic> missing_diagnostics = analysis.diagnostics(missing.id());
    INFO(dump(missing_diagnostics));
    bool missing_position = false;
    for (const engine_core::Diagnostic& diagnostic : missing_diagnostics) {
        if (diagnostic.message.find("Position") != std::string::npos &&
            diagnostic.message.find("not found") != std::string::npos) {
            missing_position = true;
        }
    }
    REQUIRE(missing_position);
}

TEST_CASE("analysis definitions come from the class registry", "[A11]") {
    const std::string source = engine_core::lua_analysis_definitions();
    REQUIRE(source.find("declare extern type TestTriangle") != std::string::npos);
    REQUIRE(source.find("Position: Vector3") != std::string::npos);
    REQUIRE(source.find("function FindFirstChild(self, name: string): Instance?") != std::string::npos);
    REQUIRE(source.find("Parent: DataModel?") != std::string::npos);
    REQUIRE(source.find("type Vector3 = vector") != std::string::npos);
    REQUIRE(source.find("declare task:") != std::string::npos);
    REQUIRE(source.find("PreRender") == std::string::npos);
    REQUIRE(source.find("RenderStepped") == std::string::npos);
    REQUIRE(source.find("workspace") == std::string::npos);
    REQUIRE(source.find("BasePart") == std::string::npos);
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
    engine_core::lua_class_operators("Transform", operators);
    REQUIRE(operators.empty());
    const std::size_t game_object = source.find("declare extern type GameObject extends Instance with");
    REQUIRE(game_object != std::string::npos);
    const std::string game_object_block = source.substr(game_object, source.find("end\n", game_object) - game_object);
    REQUIRE(game_object_block.find("Color: Color3") != std::string::npos);
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
                                           "local tri = game:FindFirstChild(\"Tri0\")\n"
                                           "assert(tri)\n"
                                           "local home = tri.Position\n"
                                           "return home\n");
    settle(analysis);
    INFO(dump(analysis.diagnostics(hop.id())));
    REQUIRE_FALSE(analysis.diagnostics(hop.id()).empty());

    // Tri0 arrives after the script. The script did not change; its answer did.
    engine_core::TestTriangle& triangle = rig.game.create<engine_core::TestTriangle>();
    rig.game.set_name(triangle.id(), "Tri0");
    rig.game.set_parent(triangle.id(), rig.game.id());
    settle(analysis);
    INFO(dump(analysis.diagnostics(hop.id())));
    REQUIRE(analysis.diagnostics(hop.id()).empty());

    // A rename away from the looked-up name brings the warning back.
    rig.game.set_name(triangle.id(), "Tri9");
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
    write("src/init.json", "{\"class\": \"DataModel\", \"id\": \"root0\", \"Name\": \"A13\"}\n");
    // Siblings sort by GUID and the loader parents the last one first, so the
    // script is in the tree before Tri0 is.
    write("src/Tri0.aaa.json", "{\"class\": \"TestTriangle\", \"id\": \"aaa\", \"Name\": \"Tri0\"}\n");
    write("src/Hop.zzz.meta.json", "{\"class\": \"Script\", \"id\": \"zzz\", \"Name\": \"Hop\"}\n");
    write("src/Hop.zzz.luau",
          "local tri = game:FindFirstChild(\"Tri0\")\n"
          "assert(tri)\n"
          "local home = tri.Position\n"
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
    game.set_parent(module.id(), game.id());
    return module;
}

bool analyzed(const engine_core::ScriptAnalysis& analysis, engine_core::InstanceId id) {
    return analysis.analyzed_source(id).has_value();
}

}  // namespace

TEST_CASE("A14 open scope checks watched scripts and the modules they require", "[A14]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    analysis.set_scope(engine_core::AnalysisScope::Open);
    const char* kBad = "--!strict\nlocal x: number = \"a\"\nreturn x\n";
    engine_core::ModuleScript& deep = add_module(rig.game, "Deep", kBad);
    engine_core::ModuleScript& mid = add_module(rig.game, "Mid", "return require(script.Parent.Deep)\n");
    engine_core::ModuleScript& other = add_module(rig.game, "Other", kBad);
    engine_core::Script& main = add_script(rig.game, "Main", "local value = require(script.Parent.Mid)\nreturn value\n");
    engine_core::Script& closed = add_script(rig.game, "Closed", kBad);
    settle(analysis);
    REQUIRE_FALSE(analyzed(analysis, main.id()));
    REQUIRE_FALSE(analyzed(analysis, closed.id()));
    REQUIRE_FALSE(analyzed(analysis, deep.id()));

    SECTION("a watched script pulls in its requires, recursively, and nothing else") {
        analysis.watch(main.id());
        settle(analysis);
        REQUIRE(analyzed(analysis, main.id()));
        REQUIRE(analyzed(analysis, mid.id()));
        REQUIRE(analyzed(analysis, deep.id()));
        REQUIRE(has_code(analysis.diagnostics(deep.id()), "Type"));
        REQUIRE_FALSE(analyzed(analysis, other.id()));
        REQUIRE_FALSE(analyzed(analysis, closed.id()));

        // An edit to a closed script is not checked. One on the watched path is.
        closed.set_source("return 2\n");
        deep.set_source("return 3\n");
        settle(analysis);
        REQUIRE_FALSE(analyzed(analysis, closed.id()));
        REQUIRE(analysis.analyzed_source(deep.id()) == std::string("return 3\n"));
        REQUIRE(analysis.diagnostics(deep.id()).empty());

        // Dropping the require drops the modules it brought in.
        main.set_source("return 1\n");
        settle(analysis);
        REQUIRE(analyzed(analysis, main.id()));
        REQUIRE_FALSE(analyzed(analysis, mid.id()));
        REQUIRE_FALSE(analyzed(analysis, deep.id()));

        analysis.unwatch(main.id());
        settle(analysis);
        REQUIRE_FALSE(analyzed(analysis, main.id()));
    }

    SECTION("a module shared by two watched scripts stays until both close") {
        engine_core::Script& second =
            add_script(rig.game, "Second", "local value = require(script.Parent.Deep)\nreturn value\n");
        analysis.watch(main.id());
        analysis.watch(second.id());
        settle(analysis);
        REQUIRE(analyzed(analysis, deep.id()));
        analysis.unwatch(main.id());
        settle(analysis);
        REQUIRE(analyzed(analysis, deep.id()));
        REQUIRE_FALSE(analyzed(analysis, mid.id()));
        analysis.unwatch(second.id());
        settle(analysis);
        REQUIRE_FALSE(analyzed(analysis, deep.id()));
    }

    SECTION("a watched script follows the tree") {
        engine_core::Script& hop = add_script(rig.game, "Hop",
                                               "local tri = game:FindFirstChild(\"Tri0\")\n"
                                               "assert(tri)\n"
                                               "local home = tri.Position\n"
                                               "return home\n");
        analysis.watch(hop.id());
        settle(analysis);
        REQUIRE_FALSE(analysis.diagnostics(hop.id()).empty());
        engine_core::TestTriangle& triangle = rig.game.create<engine_core::TestTriangle>();
        rig.game.set_name(triangle.id(), "Tri0");
        rig.game.set_parent(triangle.id(), rig.game.id());
        settle(analysis);
        INFO(dump(analysis.diagnostics(hop.id())));
        REQUIRE(analysis.diagnostics(hop.id()).empty());
        REQUIRE_FALSE(analyzed(analysis, closed.id()));
    }
}

TEST_CASE("A15 switching to open scope drops what is not watched", "[A15]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Script& kept = add_script(rig.game, "Kept", "return 1\n");
    engine_core::Script& dropped = add_script(rig.game, "Dropped", "return 2\n");
    settle(analysis);
    REQUIRE(analyzed(analysis, kept.id()));
    REQUIRE(analyzed(analysis, dropped.id()));
    analysis.watch(kept.id());
    analysis.set_scope(engine_core::AnalysisScope::Open);
    settle(analysis);
    REQUIRE(analyzed(analysis, kept.id()));
    REQUIRE_FALSE(analyzed(analysis, dropped.id()));
    // A watched script that is destroyed does not leave analysis busy.
    rig.game.destroy(kept.id());
    settle(analysis);
    REQUIRE(analysis.idle());
}

TEST_CASE("A16 a require path that reaches a ModuleScript takes its type", "[A16]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.game);
    engine_core::Folder& modules = rig.game.create<engine_core::Folder>();
    rig.game.set_name(modules.id(), "Modules");
    rig.game.set_parent(modules.id(), rig.game.id());
    engine_core::ModuleScript& config = add_module(rig.game, "Config",
                                                   "local Config = {}\n"
                                                   "Config.Currencies = { Gold = \"Gold\" }\n"
                                                   "Config.Settings = { Enabled = false }\n"
                                                   "return Config\n");
    rig.game.set_parent(config.id(), modules.id());
    engine_core::Script& script = add_script(rig.game, "Test",
                                             "local Config = require(game:FindFirstChild(\"Modules\"):FindFirstChild(\"Config\"))\n"
                                             "local currency = Config.Currencies.Gold\n"
                                             "Config.Settings.Enabled = true\n"
                                             "print(\"Currency:\", currency, Config.Settings.Enabled)\n");
    settle(analysis);
    INFO(dump(analysis.diagnostics(script.id())));
    REQUIRE(dump(analysis.diagnostics(script.id())).find("Unknown require") == std::string::npos);

    SECTION("the module's type reaches the script") {
        script.set_source("--!strict\n"
                          "local Config = require(game:FindFirstChild(\"Modules\"):FindFirstChild(\"Config\"))\n"
                          "local gold: number = Config.Currencies.Gold\n");
        settle(analysis);
        INFO(dump(analysis.diagnostics(script.id())));
        const std::string report = dump(analysis.diagnostics(script.id()));
        REQUIRE(report.find("@2:") != std::string::npos);
        REQUIRE(report.find("Unknown require") == std::string::npos);
    }

    SECTION("a path to nothing still warns") {
        script.set_source("local Config = require(game:FindFirstChild(\"Modules\"):FindFirstChild(\"Missing\"))\n");
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
    rig.game.set_parent(modules.id(), rig.game.id());
    engine_core::ModuleScript& config = add_module(rig.game, "Config", "return { Gold = 1 }\n");
    rig.game.set_parent(config.id(), modules.id());
    engine_core::Script& script = add_script(
        rig.game, "Test", "local Config = require(game:ProbeChild(\"Modules\"):ProbeChild(\"Config\"))\nreturn Config\n");
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
    rig.game.set_parent(modules.id(), rig.game.id());
    engine_core::ModuleScript& config = add_module(rig.game, "Config",
                                                   "local Config = {}\n"
                                                   "Config.Currencies = { Gold = \"Gold\" }\n"
                                                   "Config.Settings = { Enabled = false }\n"
                                                   "return Config\n");
    rig.game.set_parent(config.id(), modules.id());
    engine_core::Script& script = add_script(rig.game, "Test",
                                             "local Config = require(game:WaitForChild(\"Modules\"):WaitForChild(\"Config\"))\n"
                                             "local currency = Config.Currencies.Gold\n"
                                             "Config.Settings.Enabled = true\n"
                                             "print(\"Currency:\", currency, Config.Settings.Enabled)\n");
    settle(analysis);
    INFO(dump(analysis.diagnostics(script.id())));
    REQUIRE(analysis.diagnostics(script.id()).empty());

    SECTION("the child keeps its class") {
        script.set_source("--!strict\nlocal folder = game:WaitForChild(\"Modules\")\nlocal n: number = folder\n");
        settle(analysis);
        const std::string report = dump(analysis.diagnostics(script.id()));
        INFO(report);
        REQUIRE(report.find("'Folder'") != std::string::npos);
        REQUIRE(report.find("Folder?") == std::string::npos);
    }

    SECTION("a timeout is accepted") {
        script.set_source("--!strict\nlocal folder = game:WaitForChild(\"Modules\", 2)\nreturn folder\n");
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
    rig.game.set_parent(door.id(), rig.game.id());
    engine_core::Script& script = add_script(rig.game, "Dot",
                                              "local d = game.Door\n"
                                              "d.Name = \"x\"\n"
                                              "local c = game.Door.Color\n"
                                              "local m = game.Nope\n"
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
    rig.game.set_parent(configs.id(), rig.game.id());
    engine_core::TestTriangle& inner = rig.game.create<engine_core::TestTriangle>();
    rig.game.set_name(inner.id(), "SomeInstance");
    rig.game.set_parent(inner.id(), configs.id());
    // A child named like a property: the property wins, as at run time.
    engine_core::Folder& named = rig.game.create<engine_core::Folder>();
    rig.game.set_name(named.id(), "Name");
    rig.game.set_parent(named.id(), configs.id());
    engine_core::ModuleScript& config = add_module(rig.game, "Config", "return { Gold = 1 }\n");
    rig.game.set_parent(config.id(), configs.id());
    engine_core::Script& script = add_script(rig.game, "Test",
                                             "local some = game.Configs.SomeInstance\n"
                                             "local home = some.Position\n"
                                             "game.Configs.SomeInstance.Position = home\n"
                                             "local again = script.Parent.Configs.SomeInstance.Position\n"
                                             "local folder = game.Configs\n"
                                             "local found = folder:FindFirstChild(\"SomeInstance\").Position\n"
                                             "local label: string = game.Configs.Name\n"
                                             "local gold = require(game.Configs.Config).Gold\n"
                                             "return again, found, label, gold\n");
    settle(analysis);
    INFO(dump(analysis.diagnostics(script.id())));
    REQUIRE(analysis.diagnostics(script.id()).empty());

    SECTION("the child keeps its class") {
        script.set_source("--!strict\nlocal n: number = game.Configs.SomeInstance\n");
        settle(analysis);
        const std::string report = dump(analysis.diagnostics(script.id()));
        INFO(report);
        REQUIRE(report.find("'TestTriangle'") != std::string::npos);
    }

    SECTION("a member the child's class lacks is reported") {
        script.set_source("local s = game.Configs.SomeInstance.Source\nreturn s\n");
        settle(analysis);
        const std::string report = dump(analysis.diagnostics(script.id()));
        INFO(report);
        REQUIRE(report.find("Source") != std::string::npos);
    }

    SECTION("a child that is not there is reported, past the first dot too") {
        script.set_source("local a = game.Configs.Missing\nreturn a\n");
        settle(analysis);
        const std::string report = dump(analysis.diagnostics(script.id()));
        INFO(report);
        REQUIRE(report.find("Missing") != std::string::npos);
    }

    SECTION("a required module's type comes through a dotted path") {
        script.set_source("--!strict\nlocal gold: string = require(game.Configs.Config).Gold\n");
        settle(analysis);
        const std::string report = dump(analysis.diagnostics(script.id()));
        INFO(report);
        REQUIRE(report.find("Unknown require") == std::string::npos);
        REQUIRE(report.find("@1:") != std::string::npos);
    }

    SECTION("a child is read-only") {
        script.set_source("game.Configs.SomeInstance = nil\n");
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
    analysis.set_scope(engine_core::AnalysisScope::Open);
    engine_core::Folder& modules = rig.game.create<engine_core::Folder>();
    rig.game.set_name(modules.id(), "Modules");
    rig.game.set_parent(modules.id(), rig.game.id());
    engine_core::ModuleScript& config = add_module(rig.game, "Config",
                                                   "local Config = {}\n"
                                                   "Config.Currencies = { Gold = \"Gold\" }\n"
                                                   "Config.Settings = { DeleteEveryFileOnTheComputer = false }\n"
                                                   "return Config\n");
    rig.game.set_parent(config.id(), modules.id());
    const char* source =
        "local Config = require(game:FindFirstChild(\"Modules\"):FindFirstChild(\"Config\"))\n"
        "\n"
        "local currency = Config.Currencies.Gold\n"
        "\n"
        "Config.Settings.DeleteEveryFileOnTheComputer = true\n"
        "\n"
        "print(\"Currency:\", currency, \"Setting:\", Config.Settings.DeleteEveryFileOnTheComputer)\n";
    engine_core::Script& script = add_script(rig.game, "Test", source);
    analysis.watch(script.id());
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
    rig.game.set_parent(box.id(), rig.game.id());
    engine_core::TestTriangle& tri = rig.game.create<engine_core::TestTriangle>();
    rig.game.set_name(tri.id(), "Tri0");
    rig.game.set_parent(tri.id(), box.id());
    const char* source =
        "local box = game:FindFirstChild(\"Box\")\n"
        "local tri = game.Box.Tri0\n"
        "tri.Parent = box\n"
        "tri.Parent = game\n"
        "tri.Parent = nil\n"
        "local folder = Instance.new(\"Folder\", game)\n"
        "folder.Parent = game\n"
        "tri.Parent = folder\n"
        "for _, child in game:GetChildren() do\n"
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
        script.set_source("local service = game.Box:GetService(\"RunService\")\nreturn service\n");
        settle(analysis);
        const std::string report = dump(analysis.diagnostics(script.id()));
        INFO(report);
        REQUIRE(report.find("GetService") != std::string::npos);
    }

    SECTION("a value that is not an instance is still refused") {
        script.set_source("--!strict\nlocal tri = game.Box.Tri0\ntri.Parent = 5\n");
        settle(analysis);
        const std::string report = dump(analysis.diagnostics(script.id()));
        INFO(report);
        REQUIRE(report.find("@2:") != std::string::npos);
        REQUIRE(report.find("DataModel?") != std::string::npos);
    }

    SECTION("a missing child still reads as Instance?") {
        script.set_source("--!strict\nlocal gone = game:FindFirstChild(\"Nope\")\nlocal n: number = gone\n");
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
    rig.game.set_parent(module.id(), rig.game.id());
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
    // and only it holds M, so game.Dup.M reads through it.
    engine_core::Folder& first_made = rig.game.create<engine_core::Folder>();
    rig.game.set_name(first_made.id(), "Dup");
    engine_core::Folder& second_made = rig.game.create<engine_core::Folder>();
    rig.game.set_name(second_made.id(), "Dup");
    rig.game.set_parent(second_made.id(), rig.game.id());
    rig.game.set_parent(first_made.id(), rig.game.id());
    engine_core::Folder& inner = rig.game.create<engine_core::Folder>();
    rig.game.set_name(inner.id(), "M");
    rig.game.set_parent(inner.id(), second_made.id());
    engine_core::Script& script = add_script(rig.game, "Main", "--!strict\nlocal found = game.Dup.M\nprint(found)\n");
    settle(analysis);
    const std::string clean = dump(analysis.diagnostics(script.id()));
    INFO(clean);
    REQUIRE_FALSE(has_code(analysis.diagnostics(script.id()), "Type"));

    // The tree changes, and a completion request arrives before analysis
    // checks it, with its instances in slot order: the first-made Dup first.
    engine_core::Folder& extra = rig.game.create<engine_core::Folder>();
    rig.game.set_parent(extra.id(), rig.game.id());
    std::vector<engine_core::LuaNode> slot_order = completion_nodes(rig.game, script.id(), script.source());
    const engine_core::LuauFacts asked = analysis.luau_facts(slot_order, script.id(), script.source(),
                                                             script.source().size(), {}, std::chrono::seconds(20));
    REQUIRE(asked.ran);

    // Analysis then checks the tree in its own order, and game.Dup is still
    // the Dup that holds M.
    analysis.note_world_changed();
    analysis.invalidate(script.id());
    settle(analysis);
    INFO(dump(analysis.diagnostics(script.id())));
    REQUIRE_FALSE(has_code(analysis.diagnostics(script.id()), "Type"));
}
