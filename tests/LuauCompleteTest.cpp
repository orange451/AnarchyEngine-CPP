#include "LuaApi.hpp"
#include "ide/LuauComplete.hpp"

#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace {

int gFailures = 0;

void fail(const std::string& message) {
    std::fprintf(stderr, "FAIL %s\n", message.c_str());
    ++gFailures;
}

bool has_item(const ide::CompletionList& list, const char* name, bool* call = nullptr) {
    for (const ide::CompletionItem& item : list.items) {
        if (item.name == name) {
            if (call != nullptr) {
                *call = item.call;
            }
            return true;
        }
    }
    return false;
}

void expect_has(const ide::CompletionList& list, const char* name, const char* label) {
    if (!has_item(list, name)) {
        std::string message = std::string(label) + " missing " + name + "; have";
        for (const ide::CompletionItem& item : list.items) {
            message += " " + item.name;
        }
        fail(message);
    }
}

void expect_missing(const ide::CompletionList& list, const char* name, const char* label) {
    if (has_item(list, name)) {
        fail(std::string(label) + " should not offer " + name);
    }
}

void expect_call(const ide::CompletionList& list, const char* name, bool call, const char* label) {
    bool actual = false;
    if (!has_item(list, name, &actual)) {
        fail(std::string(label) + " missing " + name);
        return;
    }
    if (actual != call) {
        fail(std::string(label) + (call ? " should be callable" : " should not be callable"));
    }
}

ide::CompletionList at_end(std::string_view source, const std::vector<engine_core::LuaNode>& world = {},
                           std::uint32_t script_id = 0) {
    return ide::complete_luau(source, static_cast<int>(source.size()), world, script_id);
}

engine_core::LuaNode node(std::uint32_t id, std::uint32_t parent, const char* name, const char* class_name,
                          std::string source = {}) {
    engine_core::LuaNode item;
    item.id = id;
    item.parent = parent;
    item.name = name;
    item.class_name = class_name;
    item.source = std::move(source);
    return item;
}

void testLibraries() {
    const ide::CompletionList task = at_end("task.");
    expect_has(task, "wait", "task.wait");
    expect_has(task, "spawn", "task.spawn");
    expect_has(task, "defer", "task.defer");
    expect_has(task, "delay", "task.delay");
    expect_has(task, "cancel", "task.cancel");
    expect_call(task, "wait", true, "task.wait");
    expect_missing(task, "Name", "task");
    expect_missing(task, "os", "task");

    std::vector<engine_core::LuaSymbol> reflected;
    engine_core::lua_library_members("task", reflected);
    if (task.items.size() != reflected.size()) {
        fail("task completion does not match the loaded task library");
    }
    for (const engine_core::LuaSymbol& symbol : reflected) {
        expect_has(task, symbol.name.c_str(), "task library");
    }

    const ide::CompletionList filtered = at_end("task.w");
    expect_has(filtered, "wait", "task.w");
    expect_missing(filtered, "spawn", "task.w");
    if (filtered.replace_begin != 5 || filtered.replace_end != 6) {
        fail("task.w replace range");
    }

    const ide::CompletionList colon = at_end("task:");
    expect_missing(colon, "wait", "task:wait is not a method");

    const ide::CompletionList math = at_end("math.");
    expect_has(math, "floor", "math.floor");
    expect_has(math, "pi", "math.pi");
    expect_call(math, "floor", true, "math.floor");
    expect_call(math, "pi", false, "math.pi");
    expect_missing(math, "debug", "math");

    std::vector<engine_core::LuaSymbol> math_lib;
    engine_core::lua_library_members("math", math_lib);
    bool saw_floor = false;
    for (const engine_core::LuaSymbol& symbol : math_lib) {
        if (symbol.name == "floor") {
            saw_floor = true;
        }
    }
    if (!saw_floor) {
        fail("the loaded math library has no floor");
    }

    const ide::CompletionList text = at_end("\"hi\":");
    expect_has(text, "sub", "string method");
    expect_call(text, "sub", true, "string.sub");

    std::vector<engine_core::LuaSymbol> globals;
    engine_core::lua_library_globals(globals);
    for (const engine_core::LuaSymbol& symbol : globals) {
        if (symbol.name == "os" || symbol.name == "debug" || symbol.name == "io" || symbol.name == "getfenv") {
            fail(std::string("removed global is loaded: ") + symbol.name);
        }
    }
}

void testInstances() {
    const ide::CompletionList game = at_end("game.");
    expect_has(game, "Name", "game.Name");
    expect_has(game, "ClassName", "game.ClassName");
    expect_has(game, "Parent", "game.Parent");
    expect_has(game, "GetService", "game.GetService");
    expect_has(game, "FindFirstChild", "game.FindFirstChild");
    expect_missing(game, "Position", "game.Position");
    expect_missing(game, "Source", "game.Source");
    expect_missing(game, "wait", "game.wait");

    const ide::CompletionList methods = at_end("game:");
    expect_has(methods, "FindFirstChild", "game:FindFirstChild");
    expect_has(methods, "Destroy", "game:Destroy");
    expect_missing(methods, "Name", "game:Name");
    expect_missing(methods, "Changed", "game:Changed");

    const ide::CompletionList script = at_end("script.");
    expect_has(script, "Source", "script.Source");
    expect_has(script, "Enabled", "script.Enabled");
    expect_has(script, "Name", "script.Name");
    expect_missing(script, "Position", "script.Position");
    expect_missing(script, "Color", "script.Color");

    const engine_core::LuaField spark = engine_core::lua_property("Spark", "number", true, nullptr, nullptr);
    engine_core::register_lua_class("Widget", "DataModel", &spark, 1);
    const ide::CompletionList widget = at_end("local part = Instance.new(\"Widget\")\npart.");
    expect_has(widget, "Spark", "Widget.Spark");
    expect_has(widget, "Name", "Widget.Name");
    expect_missing(widget, "Position", "Widget.Position");
    expect_missing(widget, "Source", "Widget.Source");

    std::vector<engine_core::LuaNode> world;
    world.push_back(node(0, 0xffffffffu, "game", "DataModel"));
    world.push_back(node(4, 0, "Tri0", "TestTriangle"));
    world.push_back(node(5, 0, "Main", "Script"));
    const ide::CompletionList triangle = at_end("local tri = game:FindFirstChild(\"Tri0\")\ntri.", world, 5);
    expect_has(triangle, "Position", "Tri0.Position");
    expect_has(triangle, "Name", "Tri0.Name");
    expect_missing(triangle, "Source", "Tri0.Source");
    expect_missing(triangle, "Color", "Tri0.Color");

    const ide::CompletionList service = at_end("game:GetService(\"RunService\").");
    expect_has(service, "Heartbeat", "RunService.Heartbeat");
    expect_has(service, "PreRender", "RunService.PreRender");
    expect_missing(service, "Name", "RunService.Name");

    const ide::CompletionList shadow = at_end("local task = game\ntask.");
    expect_has(shadow, "Name", "shadowed task");
    expect_missing(shadow, "wait", "shadowed task");
}

void testModule() {
    std::vector<engine_core::LuaNode> world;
    world.push_back(node(0, 0xffffffffu, "game", "DataModel"));
    world.push_back(node(8, 0, "Lib", "ModuleScript",
                         "local extra = { zoom = function() end }\nreturn { alpha = 1, beta = function() end, nested = extra }\n"));
    world.push_back(node(9, 0, "Main", "Script", ""));
    const char* source = "local m = require(game:FindFirstChild(\"Lib\"))\nm.";
    const ide::CompletionList list = at_end(source, world, 9);
    expect_has(list, "alpha", "module alpha");
    expect_has(list, "beta", "module beta");
    expect_has(list, "nested", "module nested");
    expect_call(list, "beta", true, "module beta");
    expect_call(list, "alpha", false, "module alpha");
    expect_missing(list, "wait", "module");

    const ide::CompletionList nested = at_end("local m = require(game:FindFirstChild(\"Lib\"))\nm.nested.", world, 9);
    expect_has(nested, "zoom", "module nested.zoom");
    expect_call(nested, "zoom", true, "nested.zoom");
}

int index_of(const ide::CompletionList& list, const char* name) {
    for (int index = 0; index < static_cast<int>(list.items.size()); ++index) {
        if (list.items[static_cast<std::size_t>(index)].name == name) {
            return index;
        }
    }
    return -1;
}

void expect_detail(const ide::CompletionList& list, const char* name, const char* detail, const char* label) {
    for (const ide::CompletionItem& item : list.items) {
        if (item.name == name) {
            if (item.detail != detail) {
                fail(std::string(label) + " " + name + " detail is " + item.detail);
            }
            return;
        }
    }
    fail(std::string(label) + " missing " + name);
}

void expect_name(const ide::CompletionList& list, const char* label) {
    if (list.site != ide::CompleteSite::Name) {
        fail(std::string(label) + " is not a name completion");
    }
}

ide::CompletionList at_caret(std::string_view source, int caret,
                             const std::vector<engine_core::LuaNode>& world = {}, std::uint32_t script_id = 0) {
    return ide::complete_luau(source, caret, world, script_id);
}

void testNames() {
    const ide::CompletionList task = at_end("ta");
    expect_name(task, "ta");
    expect_has(task, "task", "ta");
    expect_has(task, "table", "ta");
    expect_missing(task, "tostring", "ta");
    expect_missing(task, "wait", "ta");
    expect_missing(task, "game", "ta");
    if (task.prefix != "ta" || task.replace_begin != 0 || task.replace_end != 2) {
        fail("ta replace range");
    }

    const ide::CompletionList game = at_end("ga");
    expect_name(game, "ga");
    expect_has(game, "game", "ga");
    expect_missing(game, "task", "ga");
    expect_missing(game, "GetService", "ga");

    const ide::CompletionList script = at_end("sc");
    expect_has(script, "script", "sc");
    expect_detail(script, "script", "Script", "sc");

    std::vector<engine_core::LuaNode> world;
    world.push_back(node(0, 0xffffffffu, "game", "DataModel"));
    world.push_back(node(3, 0, "Lib", "ModuleScript"));
    const ide::CompletionList module_script = at_end("sc", world, 3);
    expect_detail(module_script, "script", "ModuleScript", "module script");

    const ide::CompletionList keyword = at_end("loc");
    expect_has(keyword, "local", "loc");

    const char* mid = "table";
    const ide::CompletionList middle = at_caret(mid, 2);
    expect_has(middle, "task", "ta|ble");
    expect_has(middle, "table", "ta|ble");
    if (middle.replace_begin != 0 || middle.replace_end != 5) {
        fail("ta|ble replace range");
    }

    const ide::CompletionList local_target = at_end("local target = 1\nta");
    expect_has(local_target, "target", "local target");
    expect_has(local_target, "task", "local target");
    expect_detail(local_target, "target", "number", "local target");
    const int target_at = index_of(local_target, "target");
    const int task_at = index_of(local_target, "task");
    if (target_at < 0 || task_at < 0 || target_at > task_at) {
        fail("an in-scope local should precede the library");
    }

    const ide::CompletionList typed = at_end("local label = \"hi\"\nlab");
    expect_detail(typed, "label", "string", "string local");

    const ide::CompletionList object = at_end("local target = game\ntar");
    expect_detail(object, "target", "DataModel", "game local");
    expect_call(object, "target", false, "game local");

    const ide::CompletionList plain = at_end("local target\ntar");
    expect_detail(plain, "target", "local", "uninitialized local");

    const ide::CompletionList annotated = at_end("local target: number\ntar");
    expect_has(annotated, "target", "annotated local");

    const ide::CompletionList shadow = at_end("local task = game\nta");
    expect_detail(shadow, "task", "DataModel", "shadowed task");
    expect_call(shadow, "task", false, "shadowed task");

    const ide::CompletionList shadow_game = at_end("local game = 1\nga");
    expect_detail(shadow_game, "game", "number", "shadowed game");

    const ide::CompletionList inner = at_end("local target = game\ndo\nlocal target = 1\ntar");
    expect_detail(inner, "target", "number", "inner target");

    const ide::CompletionList outer = at_end("local target = game\ndo\nlocal target = tar");
    expect_detail(outer, "target", "DataModel", "outer target in initializer");

    const ide::CompletionList defining = at_end("local target = tar");
    expect_missing(defining, "target", "local initializer");

    const ide::CompletionList unfinished = at_end("local target = 1 +\ntar");
    expect_missing(unfinished, "target", "unfinished local");

    const ide::CompletionList same_line = at_end("local target = 1 tar");
    expect_has(same_line, "target", "same line");

    const char* later = "tar\nlocal target = 1";
    const ide::CompletionList before = at_caret(later, 3);
    expect_missing(before, "target", "local declared later");

    const ide::CompletionList ended = at_end("do\nlocal target = 1\nend\nta");
    expect_missing(ended, "target", "after do");
    expect_has(ended, "task", "after do");

    const ide::CompletionList inside = at_end("local target = 1\ndo\nta");
    expect_has(inside, "target", "inside do");

    const ide::CompletionList after_if = at_end("if true then\nlocal target = 1\nend\nta");
    expect_missing(after_if, "target", "after if");

    const ide::CompletionList in_if = at_end("if true then\nlocal target = 1\nta");
    expect_has(in_if, "target", "inside if");

    const ide::CompletionList param = at_end("local function take(target)\nta");
    expect_has(param, "target", "parameter");
    expect_has(param, "take", "local function");
    expect_detail(param, "take", "function", "local function");
    expect_call(param, "take", true, "local function");

    const ide::CompletionList param_list = at_end("local function take(target, tar");
    expect_missing(param_list, "target", "parameter list");

    const ide::CompletionList fn_value = at_end("local target = function()\nta");
    expect_missing(fn_value, "target", "function value initializer");

    const ide::CompletionList fn_closed = at_end("local target = function()\nend\nta");
    expect_has(fn_closed, "target", "after function value");
    expect_call(fn_closed, "target", true, "after function value");

    const ide::CompletionList global_fn = at_end("function take()\nend\nta");
    expect_has(global_fn, "take", "global function");
    expect_detail(global_fn, "take", "function", "global function");
    expect_call(global_fn, "take", true, "global function");

    const ide::CompletionList inside_fn = at_end("function take()\nta");
    expect_has(inside_fn, "take", "inside global function");

    const ide::CompletionList nested_global =
        at_end("do\nlocal function outer()\nfunction take()\nend\nend\nend\nta");
    expect_has(nested_global, "take", "nested global function");
    expect_missing(nested_global, "outer", "nested global function");

    const ide::CompletionList sibling =
        at_end("local function alpha()\nlocal target = 1\nend\nlocal function beta()\n");
    expect_missing(sibling, "target", "sibling function");
    expect_has(sibling, "alpha", "sibling function");
    expect_has(sibling, "beta", "sibling function");

    const ide::CompletionList upvalue = at_end("local target = 1\nlocal function follow()\n");
    expect_has(upvalue, "target", "upvalue");
    expect_has(upvalue, "follow", "upvalue");

    const ide::CompletionList loop = at_end("for target = 1, 4 do\nta");
    expect_has(loop, "target", "for body");

    const ide::CompletionList header = at_end("for target = 1, tar");
    expect_missing(header, "target", "for header");

    const ide::CompletionList until = at_end("repeat\nlocal target = 1\nuntil tar");
    expect_has(until, "target", "until");

    const ide::CompletionList after_until = at_end("repeat\nlocal target = 1\nuntil true\nta");
    expect_missing(after_until, "target", "after until");

    const ide::CompletionList assigned = at_end("do\ntake = 1\nend\nta");
    expect_has(assigned, "take", "global assignment");
    expect_detail(assigned, "take", "number", "global assignment");

    const ide::CompletionList assigned_local = at_end("do\nlocal take = 1\nend\nta");
    expect_missing(assigned_local, "take", "block local");

    const ide::CompletionList rhs = at_end("take = tar");
    expect_missing(rhs, "take", "assignment initializer");

    const ide::CompletionList removed = at_end("os");
    expect_missing(removed, "os", "removed global");
    expect_missing(at_end("debug"), "debug", "removed global");
    expect_missing(at_end("getfenv"), "getfenv", "removed global");
}

void testConsole() {
    const char* command = "local target = game tar";
    const ide::CompletionList names =
        ide::complete_luau(command, static_cast<int>(std::string_view(command).size()), {}, 0, false);
    expect_name(names, "console name");
    expect_has(names, "target", "console local");
    expect_detail(names, "target", "DataModel", "console local");
    expect_missing(names, "script", "console script");

    const ide::CompletionList typed = ide::complete_luau("ga", 2, {}, 0, false);
    expect_has(typed, "game", "console ga");
    expect_missing(typed, "script", "console ga");

    const ide::CompletionList members = ide::complete_luau("script.", 7, {}, 0, false);
    expect_missing(members, "Source", "console script.Source");
    expect_missing(members, "Name", "console script.Name");

    const ide::CompletionList task = ide::complete_luau("task.", 5, {}, 0, false);
    expect_has(task, "wait", "console task.wait");

    const ide::CompletionList shared = at_end("sh");
    expect_has(shared, "shared", "shared");
    const ide::CompletionList global = at_end("_G");
    expect_has(global, "_G", "_G");
}

void testSkipped() {
    const ide::CompletionList comment = at_end("-- task.");
    if (comment.site != ide::CompleteSite::None || !comment.items.empty()) {
        fail("a comment offers completions");
    }
    const ide::CompletionList quoted = at_end("local s = \"task.");
    if (quoted.site != ide::CompleteSite::None) {
        fail("a string offers completions");
    }
}

}  // namespace

int RunLuauCompleteTests() {
    try {
        testLibraries();
        testInstances();
        testModule();
        testNames();
        testConsole();
        testSkipped();
    } catch (const std::exception& ex) {
        fail(std::string("exception ") + ex.what());
    }
    return gFailures;
}
