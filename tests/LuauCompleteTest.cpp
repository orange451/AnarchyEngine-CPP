#include "LuaApi.hpp"
#include "ide/ClassFilter.hpp"
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

void expect_detail(const ide::CompletionList& list, const char* name, const char* detail, const char* label);

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

void testVector3() {
    const ide::CompletionList library = at_end("Vector3.");
    expect_has(library, "new", "Vector3.new");
    expect_has(library, "zero", "Vector3.zero");
    expect_has(library, "one", "Vector3.one");
    expect_has(library, "xAxis", "Vector3.xAxis");
    expect_has(library, "FromNormalId", "Vector3.FromNormalId");
    expect_has(library, "FromAxis", "Vector3.FromAxis");
    expect_call(library, "new", true, "Vector3.new");
    expect_call(library, "zero", false, "Vector3.zero");
    expect_missing(library, "X", "Vector3 library");
    expect_detail(library, "new", "function", "Vector3.new");
    expect_detail(library, "zero", "Vector3", "Vector3.zero");

    const ide::CompletionList built = at_end("Vector3.new().");
    expect_has(built, "X", "Vector3.X");
    expect_has(built, "Y", "Vector3.Y");
    expect_has(built, "Z", "Vector3.Z");
    expect_has(built, "Magnitude", "Vector3.Magnitude");
    expect_has(built, "Unit", "Vector3.Unit");
    expect_has(built, "Abs", "Vector3.Abs");
    expect_has(built, "Dot", "Vector3.Dot");
    expect_has(built, "FuzzyEq", "Vector3.FuzzyEq");
    expect_call(built, "X", false, "Vector3.X");
    expect_call(built, "Abs", true, "Vector3.Abs");
    expect_detail(built, "X", "number", "Vector3.X");
    expect_detail(built, "Unit", "Vector3", "Vector3.Unit");
    expect_detail(built, "Dot", "function", "Vector3.Dot");
    expect_missing(built, "new", "Vector3 value");

    const ide::CompletionList colon = at_end("Vector3.new():");
    expect_has(colon, "Cross", "Vector3:Cross");
    expect_has(colon, "Lerp", "Vector3:Lerp");
    expect_missing(colon, "X", "Vector3:X");
    expect_missing(colon, "Magnitude", "Vector3:Magnitude");

    const ide::CompletionList constant = at_end("Vector3.zero.");
    expect_has(constant, "Z", "Vector3.zero.Z");
    expect_has(constant, "Unit", "Vector3.zero.Unit");

    const ide::CompletionList normal = at_end("Vector3.FromNormalId().");
    expect_has(normal, "X", "FromNormalId().X");

    std::vector<engine_core::LuaNode> world;
    world.push_back(node(0, 0xffffffffu, "game", "DataModel"));
    world.push_back(node(4, 0, "Tri0", "TestTriangle"));
    world.push_back(node(5, 0, "Main", "Script"));
    const ide::CompletionList position = at_end("local tri = game:FindFirstChild(\"Tri0\")\ntri.Position.", world, 5);
    expect_has(position, "X", "Position.X");
    expect_has(position, "Magnitude", "Position.Magnitude");
    expect_has(position, "Abs", "Position.Abs");
    expect_missing(position, "x", "Position.x");

    const ide::CompletionList named = at_end("Vec");
    expect_has(named, "Vector3", "Vec");

    const ide::CompletionList enums = at_end("Enum.");
    expect_has(enums, "NormalId", "Enum.NormalId");
    expect_has(enums, "Axis", "Enum.Axis");
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
    expect_detail(annotated, "target", "number", "annotated local");

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
    expect_detail(param, "target", "local", "parameter");
    expect_has(param, "take", "local function");
    expect_detail(param, "take", "(target)", "local function");
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

void expect_argument(const ide::CompletionList& list, const char* label) {
    if (list.site != ide::CompleteSite::Argument) {
        fail(std::string(label) + " is not an argument completion");
    }
}

void testStringArguments() {
    // Remains registered in this process. Nothing after this case lists services.
    engine_core::register_lua_service("TestService");

    const std::string service_source = "game:GetService(\"Ru";
    const ide::CompletionList service = at_end(service_source);
    expect_argument(service, "GetService");
    expect_has(service, "RunService", "GetService prefix");
    expect_missing(service, "TestService", "GetService prefix");
    expect_missing(service, "DataModel", "GetService is not every class");
    expect_missing(service, "Script", "GetService is not every class");
    expect_detail(service, "RunService", "service", "GetService");
    expect_call(service, "RunService", false, "GetService");
    if (service.prefix != "Ru" || service.close_quote != '"' || !service.unclosed) {
        fail("GetService string prefix");
    }
    const int service_begin = static_cast<int>(std::string("game:GetService(\"").size());
    if (service.replace_begin != service_begin || service.replace_end != static_cast<int>(service_source.size())) {
        fail("GetService replace range");
    }

    const ide::CompletionList open_service = at_end("game:GetService(\"");
    expect_has(open_service, "RunService", "GetService open quote");
    expect_has(open_service, "TestService", "registered service");
    if (index_of(open_service, "RunService") > index_of(open_service, "TestService")) {
        fail("services are alphabetical");
    }
    if (!open_service.prefix.empty() || !open_service.unclosed) {
        fail("GetService open quote prefix");
    }

    const ide::CompletionList registered = at_end("game:GetService(\"Te");
    expect_has(registered, "TestService", "TestService prefix");
    expect_missing(registered, "RunService", "TestService prefix");

    const ide::CompletionList single = at_end("game:GetService('Ru");
    expect_has(single, "RunService", "single quoted service");
    if (single.close_quote != '\'' || !single.unclosed) {
        fail("single quoted service quote");
    }

    const ide::CompletionList spaced = at_end("game:GetService( \"Ru");
    expect_has(spaced, "RunService", "space before the service string");

    const ide::CompletionList bare = at_end("game:GetService \"Ru");
    expect_has(bare, "RunService", "service string call");

    const ide::CompletionList dotted = at_end("game.GetService(\"Ru");
    if (dotted.site != ide::CompleteSite::None || !dotted.items.empty()) {
        fail("a dot call does not pass self");
    }

    const ide::CompletionList unknown_service = at_end("game:GetService(\"Zz");
    expect_argument(unknown_service, "unknown service");
    if (!unknown_service.items.empty()) {
        fail("unknown service should offer nothing");
    }

    const std::string closed = "game:GetService(\"RunService\").Heartbeat";
    const int inside = static_cast<int>(std::string("game:GetService(\"Ru").size());
    const ide::CompletionList middle = at_caret(closed, inside);
    expect_has(middle, "RunService", "caret inside a service string");
    if (middle.unclosed || middle.replace_end != static_cast<int>(std::string("game:GetService(\"RunService").size())) {
        fail("closed service string replace range");
    }

    const std::string escaped = "game:GetService(\"Ru\\\"x\")";
    const ide::CompletionList escaped_quote = at_caret(escaped, inside);
    expect_has(escaped_quote, "RunService", "escaped quote in a service string");
    if (escaped_quote.replace_end != static_cast<int>(std::string("game:GetService(\"Ru\\\"x").size())) {
        fail("escaped quote is not the end of the string");
    }

    const ide::CompletionList members = at_end("game:GetService(\"RunService\").");
    expect_has(members, "Heartbeat", "closed GetService still completes members");
    if (members.site == ide::CompleteSite::Argument) {
        fail("a closed service call is not an argument");
    }

    const ide::CompletionList printed = at_end("print(\"Ru");
    if (printed.site != ide::CompleteSite::None || !printed.items.empty()) {
        fail("print does not complete service names");
    }
    const ide::CompletionList broken = at_end("game:GetService(\"Ru\nlocal x = 1");
    if (broken.site != ide::CompleteSite::None || !broken.items.empty()) {
        fail("a newline ends the service string");
    }

    std::vector<engine_core::LuaNode> world;
    world.push_back(node(0, 0xffffffffu, "game", "DataModel"));
    world.push_back(node(1, 0, "HopSlow", "Script"));
    world.push_back(node(2, 0, "HopFast", "Script"));
    world.push_back(node(3, 0, "Tri0", "TestTriangle"));
    world.push_back(node(4, 1, "Inner", "GameObject"));
    world.push_back(node(5, 0, "Main", "Script"));

    const std::string child_source = "game:FindFirstChild(\"Ho";
    const ide::CompletionList children = at_end(child_source, world, 5);
    expect_argument(children, "FindFirstChild");
    expect_has(children, "HopFast", "FindFirstChild prefix");
    expect_has(children, "HopSlow", "FindFirstChild prefix");
    expect_missing(children, "Tri0", "FindFirstChild prefix");
    expect_missing(children, "Inner", "FindFirstChild is direct children");
    expect_missing(children, "RunService", "FindFirstChild is not a service");
    expect_detail(children, "HopFast", "Script", "FindFirstChild");
    expect_call(children, "HopFast", false, "FindFirstChild");
    if (index_of(children, "HopFast") > index_of(children, "HopSlow")) {
        fail("children are alphabetical");
    }
    if (children.prefix != "Ho" || children.close_quote != '"' || !children.unclosed) {
        fail("FindFirstChild string prefix");
    }
    if (children.replace_begin != static_cast<int>(std::string("game:FindFirstChild(\"").size()) ||
        children.replace_end != static_cast<int>(child_source.size())) {
        fail("FindFirstChild replace range");
    }

    const ide::CompletionList all_children = at_end("game:FindFirstChild(\"", world, 5);
    expect_has(all_children, "HopFast", "all children");
    expect_has(all_children, "HopSlow", "all children");
    expect_has(all_children, "Tri0", "all children");
    expect_has(all_children, "Main", "all children");
    expect_missing(all_children, "Inner", "all children");
    expect_missing(all_children, "game", "the root is not its own child");
    if (index_of(all_children, "HopSlow") > index_of(all_children, "Main") ||
        index_of(all_children, "Main") > index_of(all_children, "Tri0")) {
        fail("all children are alphabetical");
    }

    const ide::CompletionList via_parent = at_end("script.Parent:FindFirstChild(\"Ho", world, 5);
    expect_has(via_parent, "HopFast", "script.Parent children");
    expect_has(via_parent, "HopSlow", "script.Parent children");
    expect_missing(via_parent, "Inner", "script.Parent children");

    const ide::CompletionList local_root = at_end("local root = game\nroot:FindFirstChild(\"Tr", world, 5);
    expect_has(local_root, "Tri0", "local receiver");
    expect_missing(local_root, "HopFast", "local receiver");

    const ide::CompletionList nested = at_end("game:FindFirstChild(\"HopSlow\"):FindFirstChild(\"In", world, 5);
    expect_has(nested, "Inner", "nested FindFirstChild");
    expect_detail(nested, "Inner", "GameObject", "nested FindFirstChild");
    expect_missing(nested, "HopFast", "nested FindFirstChild");

    const ide::CompletionList dotted_child = at_end("game.FindFirstChild(\"Ho", world, 5);
    if (dotted_child.site != ide::CompleteSite::None || !dotted_child.items.empty()) {
        fail("a dot call does not pass self");
    }

    const ide::CompletionList second = at_end("game:FindFirstChild(\"HopSlow\", \"Ho", world, 5);
    if (second.site != ide::CompleteSite::None || !second.items.empty()) {
        fail("the second argument is not a child name");
    }

    const ide::CompletionList unknown = at_end("local x\nx:FindFirstChild(\"Ho", world, 5);
    if (unknown.site != ide::CompleteSite::None || !unknown.items.empty()) {
        fail("an unknown receiver has no children");
    }

    const ide::CompletionList no_world = at_end("game:FindFirstChild(\"Ho");
    expect_argument(no_world, "FindFirstChild without a tree");
    if (!no_world.items.empty()) {
        fail("FindFirstChild without a tree offers names");
    }

    const char* command = "game:FindFirstChild(\"Ho";
    const ide::CompletionList console =
        ide::complete_luau(command, static_cast<int>(std::string_view(command).size()), world, 0, false);
    expect_has(console, "HopFast", "console FindFirstChild");
    expect_has(console, "HopSlow", "console FindFirstChild");
}

bool is_snippet(const ide::CompletionList& list, const char* name) {
    for (const ide::CompletionItem& item : list.items) {
        if (item.name == name) {
            return item.snippet;
        }
    }
    return false;
}

void testCallbackArguments() {
    const char* phases[] = {"Heartbeat", "PreSimulation", "PostSimulation", "PreAnimation", "PreRender",
                            "RenderStepped"};
    for (const char* phase : phases) {
        const std::string source = std::string("game:GetService(\"RunService\").") + phase + ":Connect(fun";
        const ide::CompletionList list = at_end(source);
        const std::string label = std::string(phase) + " callback";
        expect_name(list, label.c_str());
        expect_has(list, "function(dt)", label.c_str());
        expect_missing(list, "function", label.c_str());
        expect_detail(list, "function(dt)", "number", label.c_str());
        expect_call(list, "function(dt)", false, label.c_str());
        if (!is_snippet(list, "function(dt)")) {
            fail(label + " should be a snippet");
        }
        if (index_of(list, "function(dt)") != 0) {
            fail(label + " snippet should be the first row");
        }
        const int begin = static_cast<int>(source.size()) - 3;
        if (list.prefix != "fun" || list.replace_begin != begin || list.replace_end != static_cast<int>(source.size())) {
            fail(label + " replace range");
        }
    }

    const ide::CompletionList typed = at_end("game:GetService(\"RunService\").Heartbeat:Connect(function");
    expect_has(typed, "function(dt)", "typed function");
    expect_missing(typed, "function", "typed function");

    const ide::CompletionList spaced = at_end("game:GetService(\"RunService\").Heartbeat:Connect( fun");
    expect_has(spaced, "function(dt)", "space before function");

    const ide::CompletionList broken = at_end("game:GetService(\"RunService\").Heartbeat:Connect(\nfun");
    expect_has(broken, "function(dt)", "function on the next line");

    const ide::CompletionList aliased =
        at_end("local heartbeat = game:GetService(\"RunService\").Heartbeat\nheartbeat:Connect(fun");
    expect_has(aliased, "function(dt)", "local signal");
    expect_detail(aliased, "function(dt)", "number", "local signal");

    const ide::CompletionList dotted =
        at_end("game:GetService(\"RunService\").Heartbeat.Connect(game, fun");
    expect_has(dotted, "function(dt)", "dot call passes self");

    const ide::CompletionList dotted_self = at_end("game:GetService(\"RunService\").Heartbeat.Connect(fun");
    expect_missing(dotted_self, "function(dt)", "dot call first argument is self");
    expect_has(dotted_self, "function", "dot call first argument is self");

    const ide::CompletionList second =
        at_end("game:GetService(\"RunService\").Heartbeat:Connect(callback, fun");
    expect_missing(second, "function(dt)", "second argument is not the callback");
    expect_has(second, "function", "second argument is not the callback");

    const ide::CompletionList waiting = at_end("game:GetService(\"RunService\").Heartbeat:Wait(fun");
    expect_missing(waiting, "function(dt)", "Wait does not take a callback");
    expect_has(waiting, "function", "Wait does not take a callback");

    const ide::CompletionList spawned = at_end("task.spawn(fun");
    expect_missing(spawned, "function(dt)", "task.spawn has no fixed parameters");
    expect_has(spawned, "function", "task.spawn still offers the keyword");

    const ide::CompletionList plain = at_end("fun");
    expect_has(plain, "function", "a bare function is the keyword");
    expect_missing(plain, "function(dt)", "a bare function is the keyword");

    const ide::CompletionList parameter =
        at_end("game:GetService(\"RunService\").Heartbeat:Connect(function(d");
    expect_name(parameter, "parameter");
    expect_has(parameter, "dt", "open parameter list");
    expect_missing(parameter, "function(dt)", "open parameter list");
    expect_detail(parameter, "dt", "number", "open parameter list");
    expect_call(parameter, "dt", false, "open parameter list");
    if (index_of(parameter, "dt") != 0) {
        fail("dt should be the first parameter suggestion");
    }

    const ide::CompletionList used =
        at_end("game:GetService(\"RunService\").Heartbeat:Connect(function(dt, d");
    expect_missing(used, "dt", "dt is already a parameter");

    const ide::CompletionList changed = at_end("game.Changed:Connect(fun");
    expect_has(changed, "function(property)", "Changed callback");
    expect_missing(changed, "function", "Changed callback");
    expect_missing(changed, "function(dt)", "Changed is not a phase signal");
    expect_detail(changed, "function(property)", "string", "Changed callback");

    std::vector<engine_core::LuaNode> world;
    world.push_back(node(0, 0xffffffffu, "game", "DataModel"));
    world.push_back(node(5, 0, "Main", "Script"));
    const ide::CompletionList script_changed = at_end("script.Changed:Connect(function(prop", world, 5);
    expect_has(script_changed, "property", "script.Changed parameter");
    expect_detail(script_changed, "property", "string", "script.Changed parameter");
    expect_missing(script_changed, "dt", "script.Changed parameter");

    const ide::CompletionList changed_dot = at_end("game.Changed.Connect(game, fun");
    expect_has(changed_dot, "function(property)", "Changed dot call");
    const ide::CompletionList changed_dot_self = at_end("game.Changed.Connect(fun");
    expect_missing(changed_dot_self, "function(property)", "Changed dot call without self");
    expect_has(changed_dot_self, "function", "Changed dot call without self");
}

void expect_signature(const ide::CompletionList& list, const char* signature, const char* label) {
    if (list.signature != signature) {
        fail(std::string(label) + " signature is '" + list.signature + "'");
    }
}

void testFunctionParameters() {
    const char* body = "local function test_func(a: string, b: Instance)\n    print(\"Test function!\", ";
    const ide::CompletionList text = at_end(std::string(body) + "a");
    expect_name(text, "parameter a");
    expect_detail(text, "a", "string", "parameter a");
    expect_call(text, "a", false, "parameter a");
    expect_missing(text, "b", "parameter a");

    const ide::CompletionList object = at_end(std::string(body) + "b");
    expect_detail(object, "b", "Instance", "parameter b");
    expect_call(object, "b", false, "parameter b");
    expect_missing(object, "a", "parameter b");

    const ide::CompletionList fields = at_end("local function test_func(a: string, b: Instance)\n    b.");
    expect_has(fields, "Name", "Instance parameter");
    expect_has(fields, "FindFirstChild", "Instance parameter");
    expect_missing(fields, "Source", "Instance parameter");

    const ide::CompletionList letters = at_end("local function test_func(a: string, b: Instance)\n    a:");
    expect_has(letters, "sub", "string parameter");
    expect_call(letters, "sub", true, "string parameter");

    const ide::CompletionList untyped = at_end("local function plain(a, b: Instance)\n    a");
    expect_detail(untyped, "a", "local", "untyped parameter");
    const ide::CompletionList mixed = at_end("local function plain(a, b: Instance)\nend\npl");
    expect_detail(mixed, "plain", "(a, b: Instance)", "mixed parameters");

    const ide::CompletionList open_list = at_end("local function test_func(a: string, b");
    expect_missing(open_list, "a", "parameter list still open");

    const char* defined =
        "local greeting = \"hi\"\n"
        "local function test_func(a: string, b: Instance)\n"
        "    print(\"Test function!\", a, b)\n"
        "end\n";
    const ide::CompletionList named = at_end(std::string(defined) + "test_f");
    expect_name(named, "call test_func");
    expect_has(named, "test_func", "call test_func");
    expect_detail(named, "test_func", "(a: string, b: Instance)", "call test_func");
    expect_call(named, "test_func", true, "call test_func");
    if (!named.signature.empty()) {
        fail("naming a function is not a call");
    }

    const ide::CompletionList invoke = at_end(std::string(defined) + "test_func(");
    expect_name(invoke, "open call");
    expect_signature(invoke, "(a: string, b: Instance)", "open call");

    const ide::CompletionList first = at_end(std::string(defined) + "test_func(g");
    expect_signature(first, "(a: string, b: Instance)", "first argument");
    expect_has(first, "greeting", "first argument");
    expect_detail(first, "greeting", "string", "first argument");
    expect_has(first, "game", "first argument");
    if (index_of(first, "greeting") != 0 || index_of(first, "game") < index_of(first, "greeting")) {
        fail("a string argument should offer string values first");
    }

    const ide::CompletionList second = at_end(std::string(defined) + "test_func(\"test\", g");
    expect_signature(second, "(a: string, b: Instance)", "second argument");
    expect_has(second, "game", "second argument");
    expect_detail(second, "game", "DataModel", "second argument");
    if (index_of(second, "game") != 0 || index_of(second, "greeting") < index_of(second, "game")) {
        fail("an Instance argument should offer instances first");
    }

    const ide::CompletionList quoted = at_end(std::string(defined) + "test_func(\"");
    expect_argument(quoted, "string argument");
    expect_signature(quoted, "(a: string, b: Instance)", "string argument");
    if (!quoted.items.empty()) {
        fail("a string parameter has no value list");
    }

    const ide::CompletionList quoted_second = at_end(std::string(defined) + "test_func(\"test\", \"");
    expect_signature(quoted_second, "(a: string, b: Instance)", "second string argument");

    const ide::CompletionList expression = at_end(
        "local test_func = function(a: string, b: Instance)\nend\ntest_f");
    expect_detail(expression, "test_func", "(a: string, b: Instance)", "function value");

    const ide::CompletionList global = at_end("function test_func(a: string, b: Instance)\nend\ntest_f");
    expect_detail(global, "test_func", "(a: string, b: Instance)", "global function");

    const ide::CompletionList optional = at_end("local function test_func(a: string?)\n    a");
    expect_detail(optional, "a", "string", "optional string");
    const ide::CompletionList optional_fn = at_end("local function test_func(a: string?)\nend\ntest_f");
    expect_detail(optional_fn, "test_func", "(a: string?)", "optional string");

    const ide::CompletionList variadic = at_end("local function test_func(a: string, ...)\nend\ntest_f");
    expect_detail(variadic, "test_func", "(a: string, ...)", "variadic function");

    const ide::CompletionList printing = at_end("print(g");
    if (!printing.signature.empty()) {
        fail("print has no declared parameters");
    }
}

void testInsertFilter() {
    std::vector<std::string> names;
    engine_core::lua_creatable_names(names);
    std::vector<std::string> shown;
    ide::filter_class_names(names, "", shown);
    if (shown.size() != 4 || shown[0] != "Folder" || shown[1] != "GameObject" || shown[2] != "ModuleScript" ||
        shown[3] != "Script") {
        fail("insert list is every creatable class, A to Z");
    }
    ide::filter_class_names(names, "scr", shown);
    if (shown.size() != 2 || shown[0] != "Script" || shown[1] != "ModuleScript") {
        fail("scr lists Script before ModuleScript");
    }
    ide::filter_class_names(names, "Ga", shown);
    if (shown.size() != 1 || shown[0] != "GameObject") {
        fail("Ga is GameObject");
    }
    ide::filter_class_names(names, "zzz", shown);
    if (!shown.empty()) {
        fail("zzz matches nothing");
    }
    ide::filter_class_names(names, "  folder ", shown);
    if (shown.size() != 1 || shown[0] != "Folder") {
        fail("search ignores surrounding spaces and case");
    }
}

void testInstanceNew() {
    if (!engine_core::lua_creatable_known("Folder") || !engine_core::lua_creatable_known("GameObject") ||
        !engine_core::lua_creatable_known("Script") || !engine_core::lua_creatable_known("ModuleScript")) {
        fail("Instance.new classes are not registered");
    }
    if (engine_core::lua_creatable_known("TestTriangle") || engine_core::lua_creatable_known("DataModel") ||
        engine_core::lua_creatable_known("Instance") || engine_core::lua_creatable_known("RunService") ||
        engine_core::lua_creatable_known("Vector3")) {
        fail("Instance.new registered a class it cannot create");
    }

    const std::string source = "Instance.new(\"Ga";
    const ide::CompletionList made = at_end(source);
    expect_argument(made, "Instance.new");
    expect_has(made, "GameObject", "Instance.new prefix");
    expect_missing(made, "Folder", "Instance.new prefix");
    expect_missing(made, "Script", "Instance.new prefix");
    expect_missing(made, "ModuleScript", "Instance.new prefix");
    expect_missing(made, "TestTriangle", "Instance.new is not every class");
    expect_missing(made, "DataModel", "Instance.new is not every class");
    expect_missing(made, "RunService", "Instance.new is not a service");
    expect_detail(made, "GameObject", "class", "Instance.new");
    expect_call(made, "GameObject", false, "Instance.new");
    if (made.prefix != "Ga" || made.close_quote != '"' || !made.unclosed) {
        fail("Instance.new string prefix");
    }
    const int begin = static_cast<int>(std::string("Instance.new(\"").size());
    if (made.replace_begin != begin || made.replace_end != static_cast<int>(source.size())) {
        fail("Instance.new replace range");
    }

    const ide::CompletionList open = at_end("Instance.new(\"");
    expect_has(open, "Folder", "open Instance.new");
    expect_has(open, "GameObject", "open Instance.new");
    expect_has(open, "ModuleScript", "open Instance.new");
    expect_has(open, "Script", "open Instance.new");
    expect_missing(open, "TestTriangle", "open Instance.new");
    expect_missing(open, "Widget", "open Instance.new");
    if (index_of(open, "Folder") > index_of(open, "GameObject") ||
        index_of(open, "GameObject") > index_of(open, "ModuleScript") ||
        index_of(open, "ModuleScript") > index_of(open, "Script")) {
        fail("classes are alphabetical");
    }
    if (!open.prefix.empty() || !open.unclosed) {
        fail("Instance.new open quote prefix");
    }

    const ide::CompletionList single = at_end("Instance.new('Sc");
    expect_has(single, "Script", "single quoted class");
    expect_missing(single, "ModuleScript", "single quoted class");
    if (single.close_quote != '\'' || !single.unclosed) {
        fail("single quoted class quote");
    }

    const ide::CompletionList spaced = at_end("Instance.new( \"Fo");
    expect_has(spaced, "Folder", "space before the class string");

    const ide::CompletionList bare = at_end("Instance.new \"Mo");
    expect_has(bare, "ModuleScript", "class string call");
    expect_missing(bare, "GameObject", "class string call");

    const ide::CompletionList colon = at_end("Instance:new(\"Fo");
    if (colon.site != ide::CompleteSite::None || !colon.items.empty()) {
        fail("a colon call passes Instance as the first argument");
    }

    const ide::CompletionList second = at_end("Instance.new(\"Folder\", \"Fo");
    if (second.site != ide::CompleteSite::None || !second.items.empty()) {
        fail("the parent argument is not a class name");
    }

    const ide::CompletionList aliased = at_end("local make = Instance.new\nmake(\"Sc");
    expect_has(aliased, "Script", "local Instance.new");
    expect_missing(aliased, "ModuleScript", "local Instance.new");

    const ide::CompletionList unknown = at_end("Instance.new(\"Zz");
    expect_argument(unknown, "unknown class");
    if (!unknown.items.empty()) {
        fail("unknown class should offer nothing");
    }

    const ide::CompletionList vector = at_end("Vector3.new(\"");
    if (vector.site != ide::CompleteSite::None || !vector.items.empty()) {
        fail("Vector3.new does not create instances");
    }

    const std::string closed = "Instance.new(\"GameObject\").Name";
    const int inside = static_cast<int>(std::string("Instance.new(\"Ga").size());
    const ide::CompletionList middle = at_caret(closed, inside);
    expect_has(middle, "GameObject", "caret inside a class string");
    if (middle.unclosed || middle.replace_end != static_cast<int>(std::string("Instance.new(\"GameObject").size())) {
        fail("closed class string replace range");
    }

    const ide::CompletionList object = at_end("local part = Instance.new(\"GameObject\")\npart.");
    expect_has(object, "Color", "created GameObject");
    expect_missing(object, "Source", "created GameObject");

    const ide::CompletionList folder = at_end("local folder = Instance.new(\"Folder\")\nfolder.");
    expect_has(folder, "Name", "created Folder");
    expect_missing(folder, "Color", "created Folder");
    expect_missing(folder, "Source", "created Folder");

    const ide::CompletionList script = at_end("local made = Instance.new(\"Script\")\nmade.");
    expect_has(script, "Source", "created Script");
    expect_has(script, "Enabled", "created Script");

    const char* command = "Instance.new(\"Fo";
    const ide::CompletionList console =
        ide::complete_luau(command, static_cast<int>(std::string_view(command).size()), {}, 0, false);
    expect_has(console, "Folder", "console Instance.new");
    expect_missing(console, "GameObject", "console Instance.new");
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
        testVector3();
        testModule();
        testNames();
        testConsole();
        testStringArguments();
        testInsertFilter();
        testInstanceNew();
        testCallbackArguments();
        testFunctionParameters();
        testSkipped();
    } catch (const std::exception& ex) {
        fail(std::string("exception ") + ex.what());
    }
    return gFailures;
}
