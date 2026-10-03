#include "Engine.hpp"
#include "Game.hpp"
#include "LuaApi.hpp"
#include "LuaEngine.hpp"
#include "ModuleScript.hpp"
#include "Script.hpp"
#include "ScriptRuntime.hpp"
#include "Folder.hpp"
#include "runner/Runner.hpp"

#include <vector>

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <stdexcept>
#include <string>

namespace {

int gFailures = 0;

void fail(const std::string& message) {
    std::fprintf(stderr, "FAIL %s\n", message.c_str());
    ++gFailures;
}

void expect(bool condition, const char* label) {
    if (!condition) {
        fail(label);
    }
}

void expectValues(const engine_core::LuaEngine::ScriptResult& result, std::initializer_list<const char*> expected,
                  const char* label) {
    if (!result.ok) {
        fail(std::string(label) + ": " + result.error);
        return;
    }
    if (result.values.size() != expected.size()) {
        fail(std::string(label) + ": expected " + std::to_string(expected.size()) + " values, got " +
             std::to_string(result.values.size()));
        return;
    }
    std::size_t index = 0;
    for (const char* value : expected) {
        if (result.values[index] != value) {
            fail(std::string(label) + ": value " + std::to_string(index) + " is '" + result.values[index] + "'");
        }
        ++index;
    }
}

void expectError(const engine_core::LuaEngine::ScriptResult& result, const char* needle, const char* label) {
    if (result.ok) {
        fail(std::string(label) + ": script succeeded");
        return;
    }
    if (result.error.find(needle) == std::string::npos) {
        fail(std::string(label) + ": error is '" + result.error + "'");
    }
}

void testLibraries() {
    engine_core::LuaEngine engine;
    engine.start();
    expect(engine.running(), "engine is running");
    expectValues(engine.execute("values", "return 1, nil, \"x\", true"), {"1", "nil", "x", "true"}, "plain values");
    expectValues(engine.execute(
                     "libs", "return math.abs(-3), string.upper(\"ab\"), typeof(vector.create(1, 2, 3)), "
                             "typeof(buffer.create(4)), bit32.band(7, 3)"),
                 {"3", "AB", "vector", "buffer", "3"}, "libraries");
    const engine_core::LuaEngine::ScriptResult syntax = engine.execute("syntax", "return (");
    expect(!syntax.ok && !syntax.error.empty(), "syntax error");
    engine.stop();
    engine.stop();
    expect(!engine.execute("after", "return 1").ok, "execute after stop");

    engine_core::LuaEngine again;
    again.start();
    bool threw = false;
    try {
        again.start();
    } catch (const std::logic_error&) {
        threw = true;
    }
    expect(threw, "double start");
}

void testSandboxSurface() {
    engine_core::LuaEngine engine;
    engine.start();
    expectValues(engine.execute("missing",
                                "return debug, os, io, package, require, loadstring, dofile, loadfile, getfenv, "
                                "setfenv, collectgarbage, load, game"),
                 {"nil", "nil", "nil", "nil", "nil", "nil", "nil", "nil", "nil", "nil", "nil", "nil", "nil"},
                 "closed surface");
    expectValues(engine.execute("debug", "local ok = pcall(function() return debug.getregistry() end) return ok"),
                 {"false"}, "no debug registry");
    expectValues(engine.execute("clock", "local ok = pcall(function() return os.clock() end) return ok"), {"false"},
                 "no os.clock");
    expectValues(engine.execute("readonly-math",
                                "local ok = pcall(function() math.abs = function() return 0 end end) return ok"),
                 {"false"}, "math is sealed");
    expectValues(engine.execute("readonly-string",
                                "local ok = pcall(function() getmetatable(\"a\").__index = {} end) return ok"),
                 {"false"}, "string metatable is sealed");
    expectValues(engine.execute("readonly-global", "local ok = pcall(function() _G.print = function() end end) return ok"),
                 {"false"}, "shared globals are sealed");
    expectValues(engine.execute("math-still", "return math.abs(-3)"), {"3"}, "math survived mutation");
}

void testIsolation() {
    engine_core::LuaEngine engine;
    engine.start();
    expectValues(engine.execute("first", "marker = 7 return marker"), {"7"}, "script writes its own global");
    expectValues(engine.execute("second", "return marker"), {"nil"}, "other script does not see it");
    expectValues(engine.execute("shadow", "math = {} return math.abs"), {"nil"}, "script can shadow a library locally");
    expectValues(engine.execute("real-math", "return math.abs(-8)"), {"8"}, "shadow did not replace the library");
}

void testHostFunctions() {
    engine_core::LuaEngine engine;
    engine.bind("add", [](engine_core::HostArgs& args) { args.pushNumber(args.number(1) + args.number(2)); });
    engine.bind("fail", [](engine_core::HostArgs& args) { args.error("nope"); });
    engine.start();

    expectValues(engine.execute("add", "return add(2, 3)"), {"5"}, "host add");
    expectError(engine.execute("bad", "return add(\"x\", 1)"), "number", "host type check");
    expectValues(engine.execute("still", "return add(4, 5)"), {"9"}, "host works after a type error");
    expectError(engine.execute("fail", "fail()"), "nope", "host error");

    expectValues(engine.execute("shadow-host", "add = function() return 0 end return add()"), {"0"},
                 "script shadows a host function locally");
    expectValues(engine.execute("real-host", "return add(20, 22)"), {"42"}, "shadow did not replace the host function");

    bool threw = false;
    try {
        engine.bind("later", [](engine_core::HostArgs&) {});
    } catch (const std::logic_error&) {
        threw = true;
    }
    expect(threw, "bind after start");

    engine_core::LuaEngine clash;
    clash.bind("math", [](engine_core::HostArgs&) {});
    threw = false;
    try {
        clash.start();
    } catch (const std::exception&) {
        threw = true;
    }
    expect(threw, "bind cannot replace math");

    engine_core::LuaEngine badName;
    threw = false;
    try {
        badName.bind("not a name", [](engine_core::HostArgs&) {});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    expect(threw, "bind rejects a bad name");
}

void testPrint() {
    engine_core::LuaEngine engine;
    engine.start();
    expectValues(engine.execute("quiet", "print(\"quiet\") return 1"), {"1"}, "print without a handler");

    std::string printed;
    engine.setPrintHandler([&printed](std::string_view text) { printed.append(text); });
    expectValues(engine.execute("hi", "print(\"hi\", 2)"), {}, "print returns nothing");
    expect(printed == "hi\t2\n", "print handler");
}

void testBudget() {
    engine_core::LuaEngine engine;
    engine.setExecutionBudget(2000);
    engine.start();
    expectValues(engine.execute("small", "return math.abs(-3)"), {"3"}, "small script under budget");
    expectError(engine.execute("spin", "while true do end"), "execution budget", "infinite loop");
    expectError(engine.execute("caught", "pcall(function() while true do end end)"), "execution budget",
                "pcall cannot hold the budget");
    expectError(engine.execute("xp", "xpcall(function() while true do end end, function() while true do end end)"),
                "execution budget", "xpcall handler cannot run forever");
    expectValues(engine.execute("after", "return 6"), {"6"}, "engine survives a budget stop");
}

void testMemory() {
    engine_core::LuaEngine engine;
    engine.setMemoryLimit(1024 * 1024);
    engine.start();
    expectError(engine.execute("bomb", "return string.rep(\"x\", 2 * 1024 * 1024)"), "memory", "allocation cap");
    expectValues(engine.execute("after", "return 1 + 1"), {"2"}, "engine survives an allocation failure");
}

void testRunner() {
    runner::Runner runner;
    runner.start();
    expect(runner.running(), "runner is running");
    expectValues(runner.lua().execute("boot", "return math.abs(-4)"), {"4"}, "runner started lua");
    expectValues(runner.lua().execute("os", "return os"), {"nil"}, "runner lua is sandboxed");
    expectValues(runner.lua().execute("hi", "print(\"hi\", 2)"), {}, "runner print returns nothing");
    const engine_core::ScriptRuntime::OutputBatch printed = runner.simulation().scripts().drain_output();
    expect(printed.lines.size() == 1 && printed.lines[0].kind == engine_core::ScriptRuntime::OutputKind::Print &&
               printed.lines[0].text == "hi\t2\n",
           "runner print reaches the output log");
    expectError(runner.lua().execute("bad", "error(\"nope\")"), "nope", "runner error");
    expect(runner.simulation().scripts().drain_output().lines.empty(), "execute errors stay with the caller");

    bool threw = false;
    try {
        runner.start();
    } catch (const std::logic_error&) {
        threw = true;
    }
    expect(threw, "runner double start");

    runner.stop();
    expect(!runner.running(), "runner stopped");
    threw = false;
    try {
        runner.lua();
    } catch (const std::logic_error&) {
        threw = true;
    }
    expect(threw, "lua() after stop");
}

// Runs a chunk in the script runtime, as the command line does, and returns what it
// printed, with "error: " in front of an uncaught error.
std::string runChunk(runner::Runner& runner, const char* source) {
    engine_core::ScriptRuntime& scripts = runner.simulation().scripts();
    std::string out;
    runner.simulation().on_simulation([&](engine_core::DataModel&) {
        const std::uint64_t from = scripts.output_next();
        scripts.run_chunk(source);
        const std::uint64_t to = scripts.output_next();
        for (const auto& line : scripts.output_since(from, static_cast<std::size_t>(to - from)).lines) {
            if (line.kind == engine_core::ScriptRuntime::OutputKind::Error) {
                out += "error: ";
            }
            out += line.text;
        }
    });
    return out;
}

void expectPrinted(runner::Runner& runner, const char* source, const std::string& wanted, const char* label) {
    const std::string printed = runChunk(runner, source);
    if (printed != wanted + "\n") {
        fail(std::string(label) + ": printed '" + printed + "'");
    }
}

void expectChunkError(runner::Runner& runner, const char* source, const char* needle, const char* label) {
    const std::string printed = runChunk(runner, source);
    if (printed.rfind("error: ", 0) != 0 || printed.find(needle) == std::string::npos) {
        fail(std::string(label) + ": printed '" + printed + "'");
    }
}

// Roblox's Color3: constructors, channels, conversions, and a Color property taking one.
void testColor3() {
    runner::Runner runner;
    runner.start();
    expectPrinted(runner, "local c = Color3.new(1, 0.5) print(c.R, c.G, c.B, typeof(c))", "1\t0.5\t0\tColor3",
                  "Color3.new fills omitted channels with 0");
    expectPrinted(runner, "print(Color3.fromRGB(255, 0, 51):ToHex())", "FF0033", "fromRGB and ToHex");
    expectPrinted(runner, "print(Color3.fromHex('#1a73e8') == Color3.fromRGB(26, 115, 232))", "true",
                  "fromHex matches fromRGB");
    expectPrinted(runner, "print(Color3.fromHex('0f0'):ToHex())", "00FF00", "a three digit hex code");
    expectChunkError(runner, "Color3.fromHex('nope')", "hex", "fromHex refuses bad text");
    expectPrinted(runner, "print(Color3.fromHSV(0.5, 1, 1):ToHSV())", "0.5\t1\t1", "fromHSV and ToHSV round-trip");
    expectPrinted(runner, "print((Color3.toHSV(Color3.new(0, 0, 1))))", "0.6666666666666666", "Color3.toHSV takes a color");
    expectPrinted(runner, "print(tostring(Color3.new(0, 0, 0):Lerp(Color3.new(1, 1, 1), 0.25)))", "0.25, 0.25, 0.25",
                  "Lerp and tostring");
    expectChunkError(runner, "local c = Color3.new() c.R = 1", "cannot be assigned", "Color3 is read-only");
    expectChunkError(runner, "local _ = Color3.new().A", "not a valid member", "a Color3 has no alpha");
    expectPrinted(runner,
                  "local o = Instance.new('GameObject') "
                  "local p = o.Transform.Position "
                  "print(typeof(p), p.X, p.Y, p.Z) "
                  "o.Transform = o.Transform.Rotation + Vector3.new(1, 2, 3) "
                  "p = o.Transform.Position "
                  "print(typeof(p), p.X, p.Y, p.Z)",
                  "Vector3\t0\t0\t0\nVector3\t1\t2\t3", "a GameObject's Transform.Position is a Vector3");
    expectChunkError(runner, "local _ = Instance.new('GameObject').Position", "not a valid member",
                     "a GameObject has no Position; scripts move it through Transform");
    runner.stop();
}

// Matrix4 is a position and rotation over a 4x4 matrix.
void testMatrix4() {
    runner::Runner runner;
    runner.start();
    expectPrinted(runner, "local m = Matrix4.new(1, 2, 3) print(typeof(m), m.X, m.Y, m.Z, m.Position == Vector3.new(1, 2, 3))",
                  "Matrix4\t1\t2\t3\ttrue", "Matrix4.new(x, y, z) is a translation");
    expectPrinted(runner, "print(Matrix4.new(1, 2, 3))", "1, 2, 3, 1, 0, 0, 0, 1, 0, 0, 0, 1",
                  "tostring is the twelve components");
    expectPrinted(runner, "print(Matrix4.new() == Matrix4.identity, select('#', Matrix4.identity:GetComponents()))",
                  "true\t12", "new() is the identity and GetComponents returns twelve");
    expectPrinted(runner,
                  "local m = Matrix4.new(0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9) "
                  "print(m.XVector == Vector3.new(1, 2, 3), m.RightVector == Vector3.new(1, 4, 7), "
                  "m.UpVector == Vector3.new(2, 5, 8), m.LookVector == Vector3.new(-3, -6, -9))",
                  "true\ttrue\ttrue\ttrue", "the twelve-number form takes rows; the axis vectors are columns");
    expectPrinted(runner, "local r = Matrix4.Angles(0.3, 0.2, 0.1) print(Matrix4.new(r:GetComponents()) == r)", "true",
                  "GetComponents round-trips through new");
    expectPrinted(runner,
                  "local r = Matrix4.Angles(0, math.pi / 2, 0) "
                  "print(r.LookVector:FuzzyEq(-Vector3.xAxis), r.RightVector:FuzzyEq(-Vector3.zAxis))",
                  "true\ttrue", "Angles turns right-handed about Y");
    expectPrinted(runner,
                  "local m = Matrix4.new(1, 0, 0) * Matrix4.Angles(0, math.pi / 2, 0) "
                  "print(typeof(m * Vector3.new(0, 0, -1)), (m * Vector3.new(0, 0, -1)):FuzzyEq(Vector3.zero))",
                  "Vector3\ttrue", "Matrix4 * Vector3 rotates then translates");
    expectPrinted(runner,
                  "local a = Matrix4.new(1, 2, 3) * Matrix4.Angles(0.4, -0.2, 1.3) "
                  "local b = Matrix4.new(-4, 0.5, 9) * Matrix4.fromAxisAngle(Vector3.new(1, 1, 0), 2) "
                  "local p = Vector3.new(3, -1, 2) "
                  "print((a:Inverse() * a):FuzzyEq(Matrix4.identity), a:ToWorldSpace(a:ToObjectSpace(b)):FuzzyEq(b), "
                  "a:PointToObjectSpace(a:PointToWorldSpace(p)):FuzzyEq(p), "
                  "a:VectorToWorldSpace(p):FuzzyEq(a.Rotation * p), (a * b):FuzzyEq(a:ToWorldSpace(b)))",
                  "true\ttrue\ttrue\ttrue\ttrue", "inverse and the space conversions agree");
    expectPrinted(runner,
                  "local a, b = Matrix4.new(1, 0, 0):ToWorldSpace(Matrix4.new(0, 1, 0), Matrix4.new(0, 0, 1)) "
                  "print(a == Matrix4.new(1, 1, 0), b == Matrix4.new(1, 0, 1))",
                  "true\ttrue", "the space methods take several values");
    expectPrinted(runner,
                  "local rx, ry, rz = Matrix4.Angles(0.1, 0.2, 0.3):ToEulerAnglesXYZ() "
                  "local ox, oy, oz = Matrix4.fromOrientation(0.4, 0.5, 0.6):ToOrientation() "
                  "print(math.abs(rx - 0.1) < 1e-6, math.abs(ry - 0.2) < 1e-6, math.abs(rz - 0.3) < 1e-6, "
                  "math.abs(ox - 0.4) < 1e-6, math.abs(oy - 0.5) < 1e-6, math.abs(oz - 0.6) < 1e-6)",
                  "true\ttrue\ttrue\ttrue\ttrue\ttrue", "Euler angles round-trip");
    expectPrinted(runner,
                  "print(Matrix4.fromEulerAngles(0.1, 0.2, 0.3, Enum.RotationOrder.YXZ):FuzzyEq("
                  "Matrix4.fromEulerAnglesYXZ(0.1, 0.2, 0.3)), Matrix4.fromEulerAngles(0.1, 0.2, 0.3) == "
                  "Matrix4.Angles(0.1, 0.2, 0.3), Enum.RotationOrder.YXZ.Value)",
                  "true\ttrue\t3", "fromEulerAngles takes a RotationOrder and defaults to XYZ");
    expectPrinted(runner,
                  "local axis, angle = Matrix4.fromAxisAngle(Vector3.new(0, 2, 0), 1):ToAxisAngle() "
                  "print(axis:FuzzyEq(Vector3.yAxis), math.abs(angle - 1) < 1e-6, "
                  "Matrix4.new(0, 0, 0, 0, math.sin(0.5), 0, math.cos(0.5)):FuzzyEq(Matrix4.fromAxisAngle(Vector3.yAxis, 1)))",
                  "true\ttrue\ttrue", "axis-angle and quaternion forms");
    expectPrinted(runner,
                  "local from = Matrix4.new() local to = Matrix4.new(10, 0, 0) * Matrix4.Angles(0, math.pi / 2, 0) "
                  "print(from:Lerp(to, 0.5):FuzzyEq(Matrix4.new(5, 0, 0) * Matrix4.Angles(0, math.pi / 4, 0)), "
                  "from:Lerp(to, 1) == to, from:Lerp(to, 0) == from)",
                  "true\ttrue\ttrue", "Lerp blends position and rotation");
    expectPrinted(runner,
                  "local m = Matrix4.lookAt(Vector3.new(1, 0, 0), Vector3.new(1, 0, 5)) "
                  "local n = Matrix4.new(Vector3.new(1, 0, 0), Vector3.new(1, 0, 5)) "
                  "print(m.LookVector:FuzzyEq(Vector3.zAxis), m.UpVector:FuzzyEq(Vector3.yAxis), m.Position == Vector3.xAxis, "
                  "n == m, Matrix4.lookAlong(Vector3.xAxis, Vector3.new(0, 0, 3)):FuzzyEq(m))",
                  "true\ttrue\ttrue\ttrue\ttrue", "lookAt, lookAlong, and new(pos, lookAt)");
    expectPrinted(runner,
                  "local x = Matrix4.fromRotationBetweenVectors(Vector3.xAxis, Vector3.yAxis) "
                  "local back = Matrix4.fromRotationBetweenVectors(Vector3.xAxis, -Vector3.xAxis) "
                  "print((x * Vector3.xAxis):FuzzyEq(Vector3.yAxis), (back * Vector3.xAxis):FuzzyEq(-Vector3.xAxis))",
                  "true\ttrue", "fromRotationBetweenVectors, including opposite vectors");
    expectPrinted(runner,
                  "local s = Matrix4.fromMatrix(Vector3.zero, Vector3.new(2, 0, 0), Vector3.new(0, 3, 0), Vector3.new(0, 0, 4)) "
                  "print(s * Vector3.one == Vector3.new(2, 3, 4), s:Orthonormalize() == Matrix4.identity, "
                  "(s:Inverse() * s):FuzzyEq(Matrix4.identity))",
                  "true\ttrue\ttrue", "a scaled matrix transforms, inverts, and orthonormalizes");
    expectPrinted(runner,
                  "local m = Matrix4.Angles(0, 1, 0) + Vector3.new(1, 2, 3) "
                  "print(m.Position == Vector3.new(1, 2, 3), (m - Vector3.new(1, 2, 3)) == Matrix4.Angles(0, 1, 0), "
                  "m.Rotation == Matrix4.Angles(0, 1, 0))",
                  "true\ttrue\ttrue", "+ and - move the position");
    expectChunkError(runner, "local m = Matrix4.new() m.X = 1", "cannot be assigned", "Matrix4 is read-only");
    expectChunkError(runner, "local _ = Matrix4.new().Nope", "not a valid member", "an unknown member errors");
    expectChunkError(runner, "Matrix4.new(1, 2, 3, 4)", "Invalid number of arguments", "new refuses four numbers");
    expectChunkError(runner, "local _ = Matrix4.new() * 2", "Matrix4 or Vector3", "* refuses a number");
    expectPrinted(runner,
                  "local o = Instance.new('GameObject') "
                  "print(typeof(o.Transform), o.Transform == Matrix4.identity) "
                  "o.Transform = Matrix4.new(1, 2, 3) * Matrix4.Angles(0, 1, 0) "
                  "print(o.Transform.Position == Vector3.new(1, 2, 3), o.Transform.Rotation == Matrix4.Angles(0, 1, 0), "
                  "o.Transform == Matrix4.new(1, 2, 3) * Matrix4.Angles(0, 1, 0))",
                  "Matrix4\ttrue\ntrue\ttrue\ttrue", "GameObject.Transform is a Matrix4");
    expectChunkError(runner, "local _ = Instance.new('GameObject').CFrame", "not a valid member",
                     "a GameObject has no CFrame alias");
    expectChunkError(runner, "Instance.new('GameObject').Transform = {1, 0, 0, 0}", "expects a Matrix4",
                     "a Transform property refuses a table");
    runner.stop();
}

void testContextActions() {
    engine_core::Game game;
    std::vector<engine_core::ContextAction> actions;
    game.context_actions(actions);
    // The root cannot be deleted.
    expect(actions.size() == 3, "the game has cut, paste, and rename");
    expect(actions.size() == 3 && actions[0].action == engine_core::InstanceAction::Cut && !actions[0].primary, "cut is not primary");
    expect(actions.size() == 3 && actions[1].action == engine_core::InstanceAction::Paste && !actions[1].primary, "paste is not primary");
    expect(actions.size() == 3 && actions[2].action == engine_core::InstanceAction::Rename && !actions[2].primary, "rename is not primary");

    // A scene service takes children, and cannot be cut, renamed, or deleted.
    actions.clear();
    game.instance(game.scene_service("Workspace"))->context_actions(actions);
    expect(actions.size() == 1 && actions[0].action == engine_core::InstanceAction::Paste, "a scene service only pastes");

    engine_core::Script& script = game.create<engine_core::Script>();
    actions.clear();
    script.context_actions(actions);
    expect(actions.size() == 7 && actions[0].action == engine_core::InstanceAction::Edit && actions[0].primary, "a script's edit is primary");
    expect(actions.size() == 7 && actions[1].action == engine_core::InstanceAction::Cut, "a script still has cut");
    expect(actions.size() == 7 && actions[6].action == engine_core::InstanceAction::Delete && !actions[6].primary, "a script can be deleted");

    engine_core::ModuleScript& module = game.create<engine_core::ModuleScript>();
    actions.clear();
    module.context_actions(actions);
    expect(actions.size() == 7 && actions[0].primary && actions[0].action == engine_core::InstanceAction::Edit, "a module script edits");

    engine_core::DataModel* prefab = engine_core::lua_create_instance(game, "Prefab");
    actions.clear();
    if (prefab != nullptr) {
        prefab->context_actions(actions);
    }
    expect(!actions.empty() && actions[0].action == engine_core::InstanceAction::Edit && actions[0].primary,
           "a Prefab's edit is primary");

    engine_core::Folder& folder = game.create<engine_core::Folder>();
    actions.clear();
    folder.context_actions(actions);
    expect(actions.size() == 6 && !actions[0].primary, "a folder uses the plain actions");
    expect(actions.size() == 6 && actions[5].action == engine_core::InstanceAction::Delete, "a folder can be deleted");
}

void testInsertInstance() {
    engine_core::Game game;
    const char* names[] = {"Folder", "GameObject", "Script", "ModuleScript"};
    for (const char* name : names) {
        engine_core::DataModel* made = engine_core::lua_create_instance(game, name);
        expect(made != nullptr && made->class_name() != nullptr && std::string(made->class_name()) == name, name);
        if (made == nullptr) {
            continue;
        }
        game.set_parent(made->id(), game.scene_service("Workspace"));
        expect(game.parent(made->id()) == game.scene_service("Workspace"), "created instance is parented under the row");
        expect(game.name(made->id()) == name, "a new instance is named for its class");
    }
    expect(engine_core::lua_create_instance(game, "DataModel") == nullptr, "DataModel is not inserted");
}

}  // namespace

int RunLuauHighlightTests();
int RunCssHighlightTests();
int RunLuauCompleteTests();
int RunScriptMarksTests();
int RunScriptPairsTests();
int RunTextSearchTests();
int RunTextWrapTests();
int RunUtf8Tests();
int RunUiCallsTests();
int RunViewCaptureTests();
int RunSceneFeedTests();
int RunGamePackTests();

int RunColorLiteralsTests();
int RunAssetBrowserTests();

int main() {
    try {
        testLibraries();
        testSandboxSurface();
        testIsolation();
        testHostFunctions();
        testPrint();
        testBudget();
        testMemory();
        testRunner();
        testColor3();
        testMatrix4();
        testContextActions();
        testInsertInstance();
        gFailures += RunLuauHighlightTests();
        gFailures += RunCssHighlightTests();
        gFailures += RunLuauCompleteTests();
        gFailures += RunScriptMarksTests();
        gFailures += RunScriptPairsTests();
        gFailures += RunTextSearchTests();
        gFailures += RunTextWrapTests();
        gFailures += RunUtf8Tests();
        gFailures += RunUiCallsTests();
        gFailures += RunViewCaptureTests();
        gFailures += RunSceneFeedTests();
        gFailures += RunGamePackTests();
        gFailures += RunColorLiteralsTests();
        gFailures += RunAssetBrowserTests();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "FAIL exception: %s\n", ex.what());
        return EXIT_FAILURE;
    }
    if (gFailures != 0) {
        std::fprintf(stderr, "%d failed\n", gFailures);
        return EXIT_FAILURE;
    }
    std::printf("ok\n");
    return EXIT_SUCCESS;
}
