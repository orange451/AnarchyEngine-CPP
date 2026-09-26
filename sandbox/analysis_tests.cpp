#include "DataModel.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "Script.hpp"
#include "ScriptAnalysis.hpp"
#include "ScriptRuntime.hpp"
#include "TaskScheduler.hpp"
#include "TestTriangle.hpp"
#include "types.hpp"

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

struct SimRole {
    SimRole() { engine_core::set_thread_role(engine_core::ThreadRole::Simulation); }
    ~SimRole() { engine_core::set_thread_role(engine_core::ThreadRole::Unknown); }
};

struct ScriptRig {
    SimRole role;
    engine_core::DataModel model;
    engine_core::TaskScheduler scheduler;
    engine_core::ScriptRuntime runtime;

    ScriptRig() {
        scheduler.reserve(16);
        model.attach_scheduler(&scheduler);
        runtime.attach(model, scheduler);
    }

    void frames(int count, double dt = 1.0 / 60.0) {
        for (int i = 0; i < count; ++i) {
            scheduler.run_phase(engine_core::Phase::Heartbeat, dt);
            model.events().drain();
            runtime.heartbeat(dt);
            model.events().drain();
        }
    }
};

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

engine_core::Script& add_script(engine_core::DataModel& model, const char* name, const char* source) {
    engine_core::Script& script = model.create<engine_core::Script>();
    model.set_name(script.id(), name);
    script.set_source(source);
    model.set_parent(script.id(), model.id());
    return script;
}

}  // namespace

TEST_CASE("A1 a syntax error is one Syntax diagnostic and compile still fails", "[A1]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.model);
    engine_core::Script& script = add_script(rig.model, "Broken", "local x =");
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

    rig.model.start_simulation();
    rig.frames(1);
    REQUIRE_FALSE(rig.runtime.last_error().empty());
}

TEST_CASE("A2 nocheck skips the type error and strict reports it", "[A2]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.model);
    engine_core::Script& script = add_script(rig.model, "Typed", "--!nocheck\nlocal x: number = \"a\"\n");
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
    engine_core::ScriptAnalysis analysis(rig.model);
    engine_core::Script& script = add_script(rig.model, "Wait", "wait(1)\n");
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
    engine_core::ScriptAnalysis analysis(rig.model);
    engine_core::Script& script = add_script(rig.model, "Render",
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
    engine_core::ScriptAnalysis analysis(rig.model);
    engine_core::Script& script = add_script(rig.model, "Edit", "return 1\n");
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
    engine_core::ScriptAnalysis analysis(rig.model);
    engine_core::ModuleScript& module = rig.model.create<engine_core::ModuleScript>();
    rig.model.set_name(module.id(), "Mod");
    module.set_source("return 1\n");
    rig.model.set_parent(module.id(), rig.model.id());
    engine_core::Script& script = add_script(rig.model, "Main", "local value = require(script.Parent.Mod)\nreturn value\n");
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
    engine_core::ScriptAnalysis analysis(rig.model);
    engine_core::Script& script = add_script(rig.model, "Runs",
                                             "--!strict\n"
                                             "local value: number = \"nope\"\n"
                                             "local box = script:GetChildren()[1]\n"
                                             "box.Name = \"ran\"\n");
    engine_core::GameObject& box = rig.model.create<engine_core::GameObject>();
    rig.model.set_name(box.id(), "0");
    rig.model.set_parent(box.id(), script.id());
    settle(analysis);
    INFO(dump(analysis.diagnostics(script.id())));
    REQUIRE(has_code(analysis.diagnostics(script.id()), "Type"));

    rig.model.start_simulation();
    rig.frames(1);
    INFO(rig.runtime.last_error());
    REQUIRE(rig.runtime.last_error().empty());
    REQUIRE(rig.model.name(box.id()) == "ran");
}

TEST_CASE("A8 stop restores authored diagnostics", "[A8]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.model);
    const char* authored = "--!strict\nlocal value: number = \"nope\"\n";
    engine_core::Script& script = add_script(rig.model, "Authored", authored);
    settle(analysis);
    REQUIRE(has_code(analysis.diagnostics(script.id()), "Type"));

    rig.model.start_simulation();
    script.set_source("local x =\n");
    settle(analysis);
    INFO(dump(analysis.diagnostics(script.id())));
    REQUIRE(has_code(analysis.diagnostics(script.id()), "Syntax"));

    rig.model.stop_simulation();
    REQUIRE(script.source() == authored);
    settle(analysis);
    const std::vector<engine_core::Diagnostic> restored = analysis.diagnostics(script.id());
    INFO(dump(restored));
    REQUIRE(has_code(restored, "Type"));
    REQUIRE_FALSE(has_code(restored, "Syntax"));
}

TEST_CASE("A9 pump is the only publisher", "[A9]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.model);
    engine_core::Script& script = add_script(rig.model, "Queued", "return 1\n");
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

TEST_CASE("A10 a known child keeps its class and may still be nil", "[A10]") {
    ScriptRig rig;
    engine_core::TestTriangle& triangle = rig.model.create<engine_core::TestTriangle>();
    rig.model.set_name(triangle.id(), "Tri0");
    rig.model.set_parent(triangle.id(), rig.model.id());
    engine_core::ScriptAnalysis analysis(rig.model);

    engine_core::Script& bare = add_script(rig.model, "Bare",
                                            "local tri = game:FindFirstChild(\"Tri0\")\n"
                                            "local home = tri.Position\n"
                                            "return home\n");
    settle(analysis);
    const std::vector<engine_core::Diagnostic> bare_diagnostics = analysis.diagnostics(bare.id());
    INFO(dump(bare_diagnostics));
    bool position_missing = false;
    bool nil_warning = false;
    for (const engine_core::Diagnostic& diagnostic : bare_diagnostics) {
        if (diagnostic.message.find("Position") != std::string::npos &&
            diagnostic.message.find("not found") != std::string::npos) {
            position_missing = true;
        }
        if (diagnostic.message.find("nil") != std::string::npos) {
            nil_warning = true;
        }
    }
    REQUIRE_FALSE(position_missing);
    REQUIRE(nil_warning);

    engine_core::Script& hop = add_script(rig.model, "Hop",
                                           "local tri = game:FindFirstChild(\"Tri0\")\n"
                                           "assert(tri)\n"
                                           "local home = tri.Position\n"
                                           "tri.Position = home + Vector3.new(0.45, 0, 0)\n"
                                           "return home\n");
    settle(analysis);
    INFO(dump(analysis.diagnostics(hop.id())));
    REQUIRE(analysis.diagnostics(hop.id()).empty());

    engine_core::Script& missing = add_script(rig.model, "Missing",
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
    REQUIRE(source.find("Parent: Instance?") != std::string::npos);
    REQUIRE(source.find("type Vector3 = vector") != std::string::npos);
    REQUIRE(source.find("declare task:") != std::string::npos);
    REQUIRE(source.find("PreRender") == std::string::npos);
    REQUIRE(source.find("RenderStepped") == std::string::npos);
    REQUIRE(source.find("workspace") == std::string::npos);
    REQUIRE(source.find("BasePart") == std::string::npos);
    REQUIRE(source.find("GetPropertyChangedSignal") == std::string::npos);
}

TEST_CASE("disabling script analysis drops diagnostics", "[A]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.model);
    engine_core::Script& script = add_script(rig.model, "Off", "local x =\n");
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
    engine_core::ScriptAnalysis analysis(rig.model);
    engine_core::Script& hop = add_script(rig.model, "Hop",
                                           "local tri = game:FindFirstChild(\"Tri0\")\n"
                                           "assert(tri)\n"
                                           "local home = tri.Position\n"
                                           "return home\n");
    settle(analysis);
    INFO(dump(analysis.diagnostics(hop.id())));
    REQUIRE_FALSE(analysis.diagnostics(hop.id()).empty());

    // Tri0 arrives after the script. The script did not change; its answer did.
    engine_core::TestTriangle& triangle = rig.model.create<engine_core::TestTriangle>();
    rig.model.set_name(triangle.id(), "Tri0");
    rig.model.set_parent(triangle.id(), rig.model.id());
    settle(analysis);
    INFO(dump(analysis.diagnostics(hop.id())));
    REQUIRE(analysis.diagnostics(hop.id()).empty());

    // A rename away from the looked-up name brings the warning back.
    rig.model.set_name(triangle.id(), "Tri9");
    settle(analysis);
    REQUIRE_FALSE(analysis.diagnostics(hop.id()).empty());
}

TEST_CASE("A13 a loaded project is analyzed against the whole loaded tree", "[A13]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("ae-a13-" + std::to_string(std::random_device{}()));
    fs::remove_all(dir);
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
    engine_core::ScriptAnalysis analysis(rig.model);
    engine_core::Project loaded = engine_core::Project::load(dir, rig.model);
    settle(analysis);
    const engine_core::InstanceId hop = *rig.model.find_guid("zzz");
    INFO(dump(analysis.diagnostics(hop)));
    REQUIRE(analysis.diagnostics(hop).empty());
    fs::remove_all(dir);
}

namespace {

engine_core::ModuleScript& add_module(engine_core::DataModel& model, const char* name, const char* source) {
    engine_core::ModuleScript& module = model.create<engine_core::ModuleScript>();
    model.set_name(module.id(), name);
    module.set_source(source);
    model.set_parent(module.id(), model.id());
    return module;
}

bool analyzed(const engine_core::ScriptAnalysis& analysis, engine_core::InstanceId id) {
    return analysis.analyzed_source(id).has_value();
}

}  // namespace

TEST_CASE("A14 open scope checks watched scripts and the modules they require", "[A14]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.model);
    analysis.set_scope(engine_core::AnalysisScope::Open);
    const char* kBad = "--!strict\nlocal x: number = \"a\"\nreturn x\n";
    engine_core::ModuleScript& deep = add_module(rig.model, "Deep", kBad);
    engine_core::ModuleScript& mid = add_module(rig.model, "Mid", "return require(script.Parent.Deep)\n");
    engine_core::ModuleScript& other = add_module(rig.model, "Other", kBad);
    engine_core::Script& main = add_script(rig.model, "Main", "local value = require(script.Parent.Mid)\nreturn value\n");
    engine_core::Script& closed = add_script(rig.model, "Closed", kBad);
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
            add_script(rig.model, "Second", "local value = require(script.Parent.Deep)\nreturn value\n");
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
        engine_core::Script& hop = add_script(rig.model, "Hop",
                                               "local tri = game:FindFirstChild(\"Tri0\")\n"
                                               "assert(tri)\n"
                                               "local home = tri.Position\n"
                                               "return home\n");
        analysis.watch(hop.id());
        settle(analysis);
        REQUIRE_FALSE(analysis.diagnostics(hop.id()).empty());
        engine_core::TestTriangle& triangle = rig.model.create<engine_core::TestTriangle>();
        rig.model.set_name(triangle.id(), "Tri0");
        rig.model.set_parent(triangle.id(), rig.model.id());
        settle(analysis);
        INFO(dump(analysis.diagnostics(hop.id())));
        REQUIRE(analysis.diagnostics(hop.id()).empty());
        REQUIRE_FALSE(analyzed(analysis, closed.id()));
    }
}

TEST_CASE("A15 switching to open scope drops what is not watched", "[A15]") {
    ScriptRig rig;
    engine_core::ScriptAnalysis analysis(rig.model);
    engine_core::Script& kept = add_script(rig.model, "Kept", "return 1\n");
    engine_core::Script& dropped = add_script(rig.model, "Dropped", "return 2\n");
    settle(analysis);
    REQUIRE(analyzed(analysis, kept.id()));
    REQUIRE(analyzed(analysis, dropped.id()));
    analysis.watch(kept.id());
    analysis.set_scope(engine_core::AnalysisScope::Open);
    settle(analysis);
    REQUIRE(analyzed(analysis, kept.id()));
    REQUIRE_FALSE(analyzed(analysis, dropped.id()));
    // A watched script that is destroyed does not leave analysis busy.
    rig.model.destroy(kept.id());
    settle(analysis);
    REQUIRE(analysis.idle());
}
