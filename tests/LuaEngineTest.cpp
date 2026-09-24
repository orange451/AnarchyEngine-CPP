#include "LuaEngine.hpp"
#include "runner/Runner.hpp"

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

}  // namespace

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
