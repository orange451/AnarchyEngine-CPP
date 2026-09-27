#include "LuaApi.hpp"
#include "ScriptAnalysis.hpp"
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

const ide::CompletionItem* find_item(const ide::CompletionList& list, const char* name) {
    for (const ide::CompletionItem& item : list.items) {
        if (item.name == name) {
            return &item;
        }
    }
    return nullptr;
}

// `returns`, `title`, and `summary_part` are checked when not null.
// An empty string requires that field to be empty.
void expect_info(const ide::CompletionList& list, const char* name, const char* returns, const char* title,
                 const char* summary_part, const char* label) {
    const ide::CompletionItem* item = find_item(list, name);
    if (item == nullptr) {
        fail(std::string(label) + " missing " + name);
        return;
    }
    if (returns != nullptr && item->returns != returns) {
        fail(std::string(label) + " returns '" + item->returns + "'");
    }
    if (title != nullptr && item->title != title) {
        fail(std::string(label) + " title '" + item->title + "'");
    }
    if (summary_part != nullptr && item->summary.find(summary_part) == std::string::npos) {
        fail(std::string(label) + " summary '" + item->summary + "'");
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
    world.push_back(node(0, 0xffffffffu, "game", "Game"));
    world.push_back(node(4, 0, "Tri0", "TestTriangle"));
    world.push_back(node(5, 0, "Main", "Script"));
    const ide::CompletionList triangle = at_end("local tri = game:FindFirstChild(\"Tri0\")\ntri.", world, 5);
    expect_has(triangle, "Position", "Tri0.Position");
    expect_has(triangle, "Name", "Tri0.Name");
    expect_missing(triangle, "Source", "Tri0.Source");
    expect_missing(triangle, "Color", "Tri0.Color");
    const ide::CompletionList waited = at_end("local tri = game:WaitForChild(\"Tri0\")\ntri.", world, 5);
    expect_has(waited, "Position", "WaitForChild Tri0.Position");
    expect_missing(waited, "Source", "WaitForChild Tri0.Source");

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
    world.push_back(node(0, 0xffffffffu, "game", "Game"));
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
    world.push_back(node(0, 0xffffffffu, "game", "Game"));
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

    const ide::CompletionList waited = at_end("local m = require(game:WaitForChild(\"Lib\"))\nm.", world, 9);
    expect_has(waited, "alpha", "WaitForChild module alpha");
}

void testModuleMethods() {
    const char* source =
        "local module = {}\n"
        "\n"
        "function module:Test()\n"
        "    print(\"Hello World!\")\n"
        "end\n"
        "\n"
        "function module.Ping()\n"
        "end\n"
        "\n"
        "function module.Take(self)\n"
        "end\n"
        "\n"
        "module.ready = true\n"
        "return module\n";
    std::vector<engine_core::LuaNode> world;
    world.push_back(node(0, 0xffffffffu, "game", "Game"));
    world.push_back(node(9, 0, "Main", "Script"));
    world.push_back(node(8, 9, "ModuleScript", "ModuleScript", source));

    const char* dot_source = "local Module = require(script:FindFirstChild(\"ModuleScript\"))\nModule.";
    const ide::CompletionList dot = at_end(dot_source, world, 9);
    expect_has(dot, "Ping", "dot function");
    expect_has(dot, "Take", "explicit self stays on dot");
    expect_has(dot, "ready", "dot field");
    expect_missing(dot, "Test", "colon method on dot");
    expect_call(dot, "Ping", true, "dot function");

    const char* colon_source = "local Module = require(script:FindFirstChild(\"ModuleScript\"))\nModule:";
    const ide::CompletionList colon = at_end(colon_source, world, 9);
    expect_has(colon, "Test", "colon method");
    expect_missing(colon, "Ping", "dot function on colon");
    expect_missing(colon, "Take", "explicit self on colon");
    expect_missing(colon, "ready", "field on colon");
    expect_call(colon, "Test", true, "colon method");
    expect_detail(colon, "Test", "function", "colon method");

    const ide::CompletionList prefix =
        at_end("local Module = require(script:FindFirstChild(\"ModuleScript\"))\nModule:Te", world, 9);
    expect_has(prefix, "Test", "Module:Te");
    expect_missing(prefix, "Ping", "Module:Te");

    const ide::CompletionList local_colon = at_end("local module = {}\nfunction module:Test()\nend\nmodule:");
    expect_has(local_colon, "Test", "local module:");
    const ide::CompletionList local_dot = at_end("local module = {}\nfunction module:Test()\nend\nfunction module.Ping()\nend\nmodule.");
    expect_missing(local_dot, "Test", "local module.");
    expect_has(local_dot, "Ping", "local module.Ping");
    const ide::CompletionList local_methods =
        at_end("local module = {}\nfunction module:Test()\nend\nfunction module.Ping()\nend\nmodule:");
    expect_has(local_methods, "Test", "local module:Test");
    expect_missing(local_methods, "Ping", "local module:Ping");

    const char* replaced =
        "local module = {}\n"
        "function module:Test()\n"
        "end\n"
        "module.Test = function()\n"
        "end\n"
        "return module\n";
    std::vector<engine_core::LuaNode> replaced_world;
    replaced_world.push_back(node(0, 0xffffffffu, "game", "Game"));
    replaced_world.push_back(node(3, 0, "Lib", "ModuleScript", replaced));
    replaced_world.push_back(node(4, 0, "Main", "Script"));
    const ide::CompletionList replaced_dot = at_end("local m = require(game:FindFirstChild(\"Lib\"))\nm.", replaced_world, 4);
    expect_has(replaced_dot, "Test", "replaced method is a dot function");
    const ide::CompletionList replaced_colon = at_end("local m = require(game:FindFirstChild(\"Lib\"))\nm:", replaced_world, 4);
    expect_missing(replaced_colon, "Test", "replaced method");

    const char* nested_source =
        "local extra = {}\n"
        "function extra:zoom()\n"
        "end\n"
        "extra.amount = 1\n"
        "return { nested = extra }\n";
    std::vector<engine_core::LuaNode> nested_world;
    nested_world.push_back(node(0, 0xffffffffu, "game", "Game"));
    nested_world.push_back(node(5, 0, "Lib", "ModuleScript", nested_source));
    nested_world.push_back(node(6, 0, "Main", "Script"));
    const ide::CompletionList nested_dot =
        at_end("local m = require(game:FindFirstChild(\"Lib\"))\nm.nested.", nested_world, 6);
    expect_has(nested_dot, "amount", "nested field");
    expect_missing(nested_dot, "zoom", "nested colon method on dot");
    const ide::CompletionList nested_colon =
        at_end("local m = require(game:FindFirstChild(\"Lib\"))\nm.nested:", nested_world, 6);
    expect_has(nested_colon, "zoom", "nested colon method");
    expect_missing(nested_colon, "amount", "nested field on colon");
    expect_call(nested_colon, "zoom", true, "nested zoom");

    const char* inner =
        "local module = {}\n"
        "function module:Test()\n"
        "    print(\"Hello World!\")\n"
        "end\n"
        "return module\n";
    const char* wrap = "return require(game:FindFirstChild(\"Lib\"))\n";
    std::vector<engine_core::LuaNode> wrap_world;
    wrap_world.push_back(node(0, 0xffffffffu, "game", "Game"));
    wrap_world.push_back(node(8, 0, "Lib", "ModuleScript", inner));
    wrap_world.push_back(node(7, 0, "Wrap", "ModuleScript", wrap));
    wrap_world.push_back(node(9, 0, "Main", "Script"));
    const ide::CompletionList wrapped =
        at_end("local Module = require(game:FindFirstChild(\"Wrap\"))\nModule:", wrap_world, 9);
    expect_has(wrapped, "Test", "required module method");
    const ide::CompletionList wrapped_dot =
        at_end("local Module = require(game:FindFirstChild(\"Wrap\"))\nModule.", wrap_world, 9);
    expect_missing(wrapped_dot, "Test", "required module method on dot");
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
    world.push_back(node(0, 0xffffffffu, "game", "Game"));
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
    expect_detail(object, "target", "Game", "game local");
    expect_call(object, "target", false, "game local");

    const ide::CompletionList plain = at_end("local target\ntar");
    expect_detail(plain, "target", "local", "uninitialized local");

    const ide::CompletionList annotated = at_end("local target: number\ntar");
    expect_has(annotated, "target", "annotated local");
    expect_detail(annotated, "target", "number", "annotated local");

    const ide::CompletionList shadow = at_end("local task = game\nta");
    expect_detail(shadow, "task", "Game", "shadowed task");
    expect_call(shadow, "task", false, "shadowed task");

    const ide::CompletionList shadow_game = at_end("local game = 1\nga");
    expect_detail(shadow_game, "game", "number", "shadowed game");

    const ide::CompletionList inner = at_end("local target = game\ndo\nlocal target = 1\ntar");
    expect_detail(inner, "target", "number", "inner target");

    const ide::CompletionList outer = at_end("local target = game\ndo\nlocal target = tar");
    expect_detail(outer, "target", "Game", "outer target in initializer");

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
    expect_detail(names, "target", "Game", "console local");
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
    world.push_back(node(0, 0xffffffffu, "game", "Game"));
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
    world.push_back(node(0, 0xffffffffu, "game", "Game"));
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
    expect_detail(second, "game", "Game", "second argument");

    // An Instance parameter offers what Instance.new makes first. game is a
    // DataModel but not an Instance, so it goes first only where a DataModel fits.
    const char* placed =
        "local gate = Instance.new(\"Folder\")\n"
        "local function place(item: Instance, parent: DataModel)\n"
        "end\n";
    const ide::CompletionList item = at_end(std::string(placed) + "place(g");
    expect_has(item, "gate", "Instance argument");
    expect_has(item, "game", "Instance argument");
    if (index_of(item, "gate") != 0 || index_of(item, "game") < index_of(item, "gate")) {
        fail("an Instance argument should offer instances first, and game is not one");
    }
    const ide::CompletionList parent = at_end(std::string(placed) + "place(gate, g");
    expect_has(parent, "gate", "DataModel argument");
    expect_has(parent, "game", "DataModel argument");
    if (index_of(parent, "game") > 1 || index_of(parent, "gate") > 1) {
        fail("a DataModel argument should offer game and instances first");
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
        engine_core::lua_creatable_known("Instance") || engine_core::lua_creatable_known("Game") ||
        engine_core::lua_creatable_known("RunService") ||
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
    expect_missing(made, "Game", "Instance.new cannot make game");
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

bool ident_char(char unit) {
    return (unit >= 'A' && unit <= 'Z') || (unit >= 'a' && unit <= 'z') || (unit >= '0' && unit <= '9') || unit == '_';
}

int find_nth(std::string_view source, std::string_view name, int nth) {
    std::size_t pos = 0;
    int seen = 0;
    while (pos < source.size()) {
        pos = source.find(name, pos);
        if (pos == std::string_view::npos) {
            return -1;
        }
        const bool left = pos == 0 || !ident_char(source[pos - 1]);
        const std::size_t end = pos + name.size();
        const bool right = end >= source.size() || !ident_char(source[end]);
        if (left && right) {
            if (seen == nth) {
                return static_cast<int>(pos);
            }
            ++seen;
        }
        pos += name.size();
    }
    return -1;
}

void expect_hover(const ide::HoverInfo& info, const char* title, const char* detail, const char* summary_part,
                  const char* label) {
    if (!info.found) {
        fail(std::string(label) + " found nothing");
        return;
    }
    if (title != nullptr && info.title != title) {
        fail(std::string(label) + " title '" + info.title + "'");
    }
    if (detail != nullptr && info.detail != detail) {
        fail(std::string(label) + " detail '" + info.detail + "'");
    }
    if (summary_part != nullptr && info.summary.find(summary_part) == std::string::npos) {
        fail(std::string(label) + " summary '" + info.summary + "'");
    }
}

void testHover() {
    const char* count_source = "local count: number = 1\nprint(count)";
    const ide::HoverInfo count = ide::hover_luau(count_source, find_nth(count_source, "count", 1));
    expect_hover(count, "count: number", "", nullptr, "use of count");

    const ide::HoverInfo declared = ide::hover_luau(count_source, find_nth(count_source, "count", 0));
    expect_hover(declared, "count: number", "", nullptr, "declaration of count");

    const char* untyped = "local value\nprint(value)";
    const ide::HoverInfo blank = ide::hover_luau(untyped, find_nth(untyped, "value", 1));
    expect_hover(blank, "value", "local", nullptr, "untyped local");

    const char* defined = "local function test_func(a: string, b: Instance): boolean\n    return a == \"z\"\nend\n";
    const std::string call = std::string(defined) + "test_func(\"z\", game)";
    const ide::HoverInfo func = ide::hover_luau(call, find_nth(call, "test_func", 1));
    expect_hover(func, "function test_func(a: string, b: Instance): boolean", "", nullptr, "call of test_func");

    const char* printed = "local function greet(name: string): string\n    print(name)\n    return name\nend";
    const ide::HoverInfo greet = ide::hover_luau(printed, find_nth(printed, "greet", 0));
    expect_hover(greet, "function greet(name: string): string", "", nullptr, "return annotation before a call");

    const ide::HoverInfo param = ide::hover_luau(defined, find_nth(defined, "a", 0));
    expect_hover(param, "a: string", "", nullptr, "parameter a");

    const char* inferred = "local function add(x: number, y: number)\n    return x + y\nend\nlocal n = add(1, 2)";
    const ide::HoverInfo add = ide::hover_luau(inferred, find_nth(inferred, "add", 1));
    expect_hover(add, "function add(x: number, y: number): number", "", nullptr, "inferred return");
    const ide::HoverInfo result = ide::hover_luau(inferred, find_nth(inferred, "n", 0));
    expect_hover(result, "n: number", "", nullptr, "call result");

    const char* silent = "local function ping(name: string)\nend";
    const ide::HoverInfo ping = ide::hover_luau(silent, find_nth(silent, "ping", 0));
    expect_hover(ping, "function ping(name: string)", "returns nothing", nullptr, "no return");

    const char* mixed = "local function pick(flag: boolean)\n    if flag then\n        return 1\n    end\n    return \"x\"\nend";
    const ide::HoverInfo pick = ide::hover_luau(mixed, find_nth(mixed, "pick", 0));
    const std::size_t paren = pick.title.rfind(')');
    if (!pick.found || paren == std::string::npos || paren + 1 != pick.title.size() || pick.detail == "returns nothing") {
        fail(std::string("mixed returns should not claim one type: ") + pick.title + " / " + pick.detail);
    }

    const char* task_source = "task.wait(0.5)";
    const ide::HoverInfo library = ide::hover_luau(task_source, find_nth(task_source, "task", 0));
    expect_hover(library, "task", "library", "simulation", "task library");
    const ide::HoverInfo wait = ide::hover_luau(task_source, find_nth(task_source, "wait", 0));
    expect_hover(wait, "function task.wait(seconds: number?)", "returns nothing", "simulation", "task.wait");

    const char* floor_source = "math.floor(1.5)";
    const ide::HoverInfo floor = ide::hover_luau(floor_source, find_nth(floor_source, "floor", 0));
    expect_hover(floor, "function math.floor(n: number): number", "", "integer", "math.floor");

    const char* slice = "local text = \"hi\"\ntext:sub(1, 1)";
    const ide::HoverInfo sub = ide::hover_luau(slice, find_nth(slice, "sub", 0));
    expect_hover(sub, "function string:sub(i: number, j: number?): string", "", "substring", "string method");

    const ide::HoverInfo game = ide::hover_luau("print(game)", find_nth("print(game)", "game", 0));
    expect_hover(game, "game: Game", "", nullptr, "game");

    const char* child = "game:FindFirstChild(\"Hop\")";
    const ide::HoverInfo find = ide::hover_luau(child, find_nth(child, "FindFirstChild", 0));
    expect_hover(find, "function Game:FindFirstChild(name: string): Instance?", "", "child", "FindFirstChild");

    const char* beat = "game:GetService(\"RunService\").Heartbeat:Wait()";
    const ide::HoverInfo heartbeat = ide::hover_luau(beat, find_nth(beat, "Wait", 0));
    expect_hover(heartbeat, "function Signal:Wait(): number", "", "signal", "Heartbeat:Wait");

    const char* shadow = "local task = game\nprint(task)";
    const ide::HoverInfo shadowed = ide::hover_luau(shadow, find_nth(shadow, "task", 1));
    expect_hover(shadowed, "task: Game", "", nullptr, "local shadows task");

    const char* hidden = "do\n    local hidden: number = 1\nend\nprint(hidden)";
    const ide::HoverInfo gone = ide::hover_luau(hidden, find_nth(hidden, "hidden", 1));
    if (gone.found) {
        fail(std::string("a local is not visible after its block: ") + gone.title);
    }

    const char* loop = "for i = 1, 4 do\n    print(i)\nend";
    const ide::HoverInfo index = ide::hover_luau(loop, find_nth(loop, "i", 1));
    expect_hover(index, "i: number", "", nullptr, "numeric for");

    const ide::HoverInfo keyword = ide::hover_luau("local x = 1", 0);
    if (keyword.found) {
        fail("a keyword has no hover");
    }

    const char* note = "-- task.wait";
    const ide::HoverInfo comment = ide::hover_luau(note, find_nth(note, "task", 0));
    if (comment.found) {
        fail("a comment has no hover");
    }

    const char* method = "local t = {}\nfunction t:foo(a: string)\nend";
    const ide::HoverInfo foo = ide::hover_luau(method, find_nth(method, "foo", 0));
    expect_hover(foo, "function t:foo(self: table, a: string)", "returns nothing", nullptr, "method");

    const char* module_source =
        "local module = {}\n"
        "\n"
        "function module:Test()\n"
        "    print(\"Hello World\")\n"
        "end\n"
        "\n"
        "function module.new()\n"
        "    return \"ur mom lol\"\n"
        "end\n"
        "\n"
        "function module.greet(name: string)\n"
        "    if name == \"hi\" then\n"
        "        return name\n"
        "    end\n"
        "    return 1\n"
        "end\n"
        "\n"
        "return module\n";
    std::vector<engine_core::LuaNode> world;
    world.push_back(node(0, 0xffffffffu, "game", "Game"));
    world.push_back(node(9, 0, "Main", "Script"));
    world.push_back(node(8, 9, "ModuleScript", "ModuleScript", module_source));
    const char* use =
        "local Module = require(script:FindFirstChild(\"ModuleScript\"))\n"
        "local x = Module.new()\n"
        "local y = Module.greet(\"hi\")\n"
        "Module:Test()\n";
    const ide::HoverInfo created = ide::hover_luau(use, find_nth(use, "new", 0), world, 9);
    expect_hover(created, "function module.new(): string", "", nullptr, "module.new");
    const ide::HoverInfo local_x = ide::hover_luau(use, find_nth(use, "x", 0), world, 9);
    expect_hover(local_x, "x: string", "", nullptr, "result of module.new");
    const ide::HoverInfo module_greet = ide::hover_luau(use, find_nth(use, "greet", 0), world, 9);
    expect_hover(module_greet, "function module.greet(name: string): string", "", nullptr, "first return");
    const ide::HoverInfo local_y = ide::hover_luau(use, find_nth(use, "y", 0), world, 9);
    expect_hover(local_y, "y: string", "", nullptr, "result of the first return");
    const ide::HoverInfo test = ide::hover_luau(use, find_nth(use, "Test", 0), world, 9);
    expect_hover(test, "function module:Test(self: table)", "returns nothing", nullptr, "module method");

    const char* replaced =
        "local module = {}\n"
        "function module.new()\n"
        "    return \"nope\"\n"
        "end\n"
        "module.new = function()\n"
        "    return 1\n"
        "end\n"
        "return module\n";
    std::vector<engine_core::LuaNode> replaced_world;
    replaced_world.push_back(node(0, 0xffffffffu, "game", "Game"));
    replaced_world.push_back(node(9, 0, "Main", "Script"));
    replaced_world.push_back(node(8, 9, "ModuleScript", "ModuleScript", replaced));
    const char* replaced_use = "local Module = require(script:FindFirstChild(\"ModuleScript\"))\nlocal x = Module.new()\n";
    const ide::HoverInfo replaced_new = ide::hover_luau(replaced_use, find_nth(replaced_use, "new", 0), replaced_world, 9);
    expect_hover(replaced_new, "function new(): number", "", nullptr, "replaced module.new");
    const ide::HoverInfo replaced_x = ide::hover_luau(replaced_use, find_nth(replaced_use, "x", 0), replaced_world, 9);
    expect_hover(replaced_x, "x: number", "", nullptr, "replaced module.new result");

    const char* constructed = "return { new = function()\n    return true\nend }\n";
    std::vector<engine_core::LuaNode> constructed_world;
    constructed_world.push_back(node(0, 0xffffffffu, "game", "Game"));
    constructed_world.push_back(node(9, 0, "Main", "Script"));
    constructed_world.push_back(node(8, 9, "ModuleScript", "ModuleScript", constructed));
    const char* constructed_use = "local Module = require(script:FindFirstChild(\"ModuleScript\"))\nlocal x = Module.new()\n";
    const ide::HoverInfo constructed_new =
        ide::hover_luau(constructed_use, find_nth(constructed_use, "new", 0), constructed_world, 9);
    expect_hover(constructed_new, "function new(): boolean", "", nullptr, "constructed module.new");
    const ide::HoverInfo constructed_x =
        ide::hover_luau(constructed_use, find_nth(constructed_use, "x", 0), constructed_world, 9);
    expect_hover(constructed_x, "x: boolean", "", nullptr, "constructed module.new result");
}

void testCompletionDocs() {
    const ide::CompletionList math = at_end("math.");
    expect_detail(math, "floor", "function", "math.floor kind");
    expect_info(math, "floor", "number", "function math.floor(n: number): number", "integer", "math.floor");
    expect_info(math, "pi", "", "pi: number", "constant", "math.pi");
    expect_detail(math, "pi", "number", "math.pi kind");

    const ide::CompletionList task = at_end("task.");
    expect_info(task, "wait", "returns nothing", "function task.wait(seconds: number?)", "simulation", "task.wait");
    expect_info(task, "spawn", "thread", "function task.spawn(callback: function, ...): thread", "thread", "task.spawn");

    const ide::CompletionList names = at_end("ta");
    expect_info(names, "task", "", "task", "simulation", "task library");
    expect_detail(names, "task", "library", "task kind");
    const ide::CompletionList printed = at_end("pri");
    expect_info(printed, "print", "returns nothing", "function print(...)", "console", "print");
    const ide::CompletionList keyword = at_end("lo");
    expect_info(keyword, "local", "", "", "", "keyword has no explanation");

    const ide::CompletionList required = at_end("req");
    expect_info(required, "require", "", "function require(module: ModuleScript)", "ModuleScript", "require");

    const ide::CompletionList game = at_end("game:");
    expect_info(game, "FindFirstChild", "Instance?", "function Game:FindFirstChild(name: string): Instance?", "child",
                "FindFirstChild");
    expect_info(game, "Destroy", "returns nothing", "function Game:Destroy()", "descendants", "Destroy");
    expect_info(game, "GetChildren", "{Instance}", "function Game:GetChildren(): {Instance}", "children",
                "GetChildren");
    expect_detail(game, "FindFirstChild", "function", "FindFirstChild kind");

    const ide::CompletionList fields = at_end("game.");
    expect_info(fields, "Name", "", "Name: string", "name", "Name");
    expect_detail(fields, "Name", "string", "Name kind");

    const ide::CompletionList text = at_end("\"hi\":");
    expect_info(text, "sub", "string", "function string:sub(i: number, j: number?): string", "substring", "string.sub");

    const ide::CompletionList library = at_end("Vector3.");
    expect_info(library, "new", "Vector3", "function Vector3.new(x: number?, y: number?, z: number?): Vector3", "components",
                "Vector3.new");
    expect_info(library, "zero", "", "zero: Vector3", "0, 0, 0", "Vector3.zero");
    expect_detail(library, "new", "function", "Vector3.new kind");
    expect_detail(library, "zero", "Vector3", "Vector3.zero kind");

    const ide::CompletionList built = at_end("Vector3.new().");
    expect_info(built, "Dot", "number", "function Vector3:Dot(other: Vector3): number", "dot", "Vector3.Dot");
    expect_info(built, "Abs", "Vector3", "function Vector3:Abs(): Vector3", "non-negative", "Vector3.Abs");
    expect_info(built, "X", "", "X: number", "component", "Vector3.X");
    expect_detail(built, "Dot", "function", "Vector3.Dot kind");
    expect_detail(built, "X", "number", "Vector3.X kind");

    const ide::CompletionList beat = at_end("game:GetService(\"RunService\").Heartbeat:");
    expect_info(beat, "Wait", "number", "function Signal:Wait(): number", "signal", "Heartbeat:Wait");
    expect_info(beat, "Connect", "Connection", nullptr, "callback", "Heartbeat:Connect");

    const char* added = "local function add(x: number, y: number)\n    return x + y\nend\nad";
    const ide::CompletionList add = at_end(added);
    expect_detail(add, "add", "(x: number, y: number)", "add parameters");
    expect_info(add, "add", "number", "function add(x: number, y: number): number", "", "add return");

    const char* silent = "local function ping(name: string)\nend\npi";
    const ide::CompletionList ping = at_end(silent);
    expect_info(ping, "ping", "returns nothing", "function ping(name: string)", "", "ping returns nothing");

    const char* mixed = "local function pick(flag: boolean)\n    if flag then\n        return 1\n    end\n    return \"x\"\nend\npic";
    const ide::CompletionList pick = at_end(mixed);
    expect_info(pick, "pick", "", "", "", "mixed return is not claimed");

    const char* method = "local module = {}\nfunction module:Test()\nend\nmodule:";
    const ide::CompletionList colon = at_end(method);
    expect_info(colon, "Test", "returns nothing", "function module:Test(self: table)", "", "colon method returns nothing");
    expect_detail(colon, "Test", "function", "colon method kind");

    const char* annotated = "local module = {}\nfunction module:Test(): string\n    return \"hi\"\nend\nmodule:";
    const ide::CompletionList noted = at_end(annotated);
    expect_info(noted, "Test", "string", "function module:Test(self: table): string", "", "annotated method");

    const ide::CompletionList snippet = at_end("game:GetService(\"RunService\").Heartbeat:Connect(fun");
    expect_info(snippet, "function(dt)", "", "", "", "callback snippet has no function return");

    const ide::CompletionList typed = at_end("local value: Vec");
    expect_info(typed, "Vector3", "", "Vector3", "3D vector", "Vector3 type");
    expect_detail(typed, "Vector3", "type", "Vector3 type kind");
}

std::vector<engine_core::LuaNode> module_world(const char* source) {
    std::vector<engine_core::LuaNode> world;
    world.push_back(node(0, 0xffffffffu, "game", "Game"));
    world.push_back(node(9, 0, "Main", "Script"));
    world.push_back(node(8, 9, "ModuleScript", "ModuleScript", source));
    return world;
}

void testTuples() {
    const char* module_source =
        "local module = {}\n"
        "\n"
        "function module:Test()\n"
        "    return 2+2, \"Test\"\n"
        "end\n"
        "\n"
        "return module\n";
    const std::vector<engine_core::LuaNode> world = module_world(module_source);
    const char* use =
        "local Module = require(script:FindFirstChild(\"ModuleScript\"))\n"
        "local x, y = Module:Test()\n"
        "print(y)\n";
    const ide::HoverInfo test = ide::hover_luau(use, find_nth(use, "Test", 0), world, 9);
    expect_hover(test, "function module:Test(self: table): (number, string)", "", nullptr, "tuple method");
    const ide::HoverInfo local_x = ide::hover_luau(use, find_nth(use, "x", 0), world, 9);
    expect_hover(local_x, "x: number", "", nullptr, "first tuple value");
    const ide::HoverInfo local_y = ide::hover_luau(use, find_nth(use, "y", 0), world, 9);
    expect_hover(local_y, "y: string", "", nullptr, "second tuple value");
    const ide::HoverInfo use_y = ide::hover_luau(use, find_nth(use, "y", 1), world, 9);
    expect_hover(use_y, "y: string", "", nullptr, "later use of y");

    const ide::HoverInfo defined = ide::hover_luau(module_source, find_nth(module_source, "Test", 0));
    expect_hover(defined, "function module:Test(self: table): (number, string)", "", nullptr, "tuple in its module");

    const char* colon_source = "local Module = require(script:FindFirstChild(\"ModuleScript\"))\nModule:";
    const ide::CompletionList colon = at_end(colon_source, world, 9);
    expect_detail(colon, "Test", "function", "tuple method kind");
    expect_info(colon, "Test", "(number, string)", "function module:Test(self: table): (number, string)", "",
                "tuple method completion");
    const ide::CompletionList dot = at_end("local Module = require(script:FindFirstChild(\"ModuleScript\"))\nModule.", world, 9);
    expect_missing(dot, "Test", "tuple method stays on colon");

    const char* names =
        "local Module = require(script:FindFirstChild(\"ModuleScript\"))\n"
        "local x, y = Module:Test()\n";
    expect_detail(at_end(std::string(names) + "x", world, 9), "x", "number", "x is a number");
    expect_detail(at_end(std::string(names) + "y", world, 9), "y", "string", "y is a string");
    const ide::CompletionList text = at_end(std::string(names) + "y:", world, 9);
    expect_has(text, "sub", "y completes as a string");
    const ide::CompletionList number = at_end(std::string(names) + "x:", world, 9);
    expect_missing(number, "sub", "x is not a string");

    const char* third =
        "local Module = require(script:FindFirstChild(\"ModuleScript\"))\n"
        "local x, y, z = Module:Test()\n"
        "print(z)\n";
    const ide::HoverInfo extra = ide::hover_luau(third, find_nth(third, "z", 0), world, 9);
    expect_hover(extra, "z", "local", nullptr, "third name has no value");

    const char* adjusted =
        "local Module = require(script:FindFirstChild(\"ModuleScript\"))\n"
        "local x, y = Module:Test(), true\n";
    expect_hover(ide::hover_luau(adjusted, find_nth(adjusted, "x", 0), world, 9), "x: number", "", nullptr,
                 "call before a comma keeps one value");
    expect_hover(ide::hover_luau(adjusted, find_nth(adjusted, "y", 0), world, 9), "y: boolean", "", nullptr,
                 "the next expression fills y");

    const char* wrapped_call =
        "local Module = require(script:FindFirstChild(\"ModuleScript\"))\n"
        "local x, y = (Module:Test())\n"
        "print(y)\n";
    expect_hover(ide::hover_luau(wrapped_call, find_nth(wrapped_call, "x", 0), world, 9), "x: number", "", nullptr,
                 "parentheses keep the first value");
    expect_hover(ide::hover_luau(wrapped_call, find_nth(wrapped_call, "y", 1), world, 9), "y", "local", nullptr,
                 "parentheses drop the second value");

    const char* both =
        "local Module = require(script:FindFirstChild(\"ModuleScript\"))\n"
        "local a, b, c = Module:Test(), Module:Test()\n";
    expect_hover(ide::hover_luau(both, find_nth(both, "a", 0), world, 9), "a: number", "", nullptr, "first call adjusts");
    expect_hover(ide::hover_luau(both, find_nth(both, "b", 0), world, 9), "b: number", "", nullptr, "second call starts at b");
    expect_hover(ide::hover_luau(both, find_nth(both, "c", 0), world, 9), "c: string", "", nullptr, "second call's extra");

    const char* skipped =
        "local Module = require(script:FindFirstChild(\"ModuleScript\"))\n"
        "local _, y = Module:Test()\n";
    expect_hover(ide::hover_luau(skipped, find_nth(skipped, "y", 0), world, 9), "y: string", "", nullptr,
                 "skipped first value");

    const char* same_file =
        "local function pair(name: string)\n"
        "    return 2 + 2, name\n"
        "end\n"
        "local x, y = pair(\"Test\")\n";
    expect_hover(ide::hover_luau(same_file, find_nth(same_file, "pair", 1)),
                 "function pair(name: string): (number, string)", "", nullptr, "same-file tuple");
    expect_hover(ide::hover_luau(same_file, find_nth(same_file, "x", 0)), "x: number", "", nullptr, "same-file x");
    expect_hover(ide::hover_luau(same_file, find_nth(same_file, "y", 0)), "y: string", "", nullptr, "same-file y");
    const ide::CompletionList listed = at_end("local function pair(name: string)\n    return 2 + 2, name\nend\npai");
    expect_detail(listed, "pair", "(name: string)", "tuple parameter list");
    expect_info(listed, "pair", "(number, string)", "function pair(name: string): (number, string)", "",
                "same-file tuple completion");

    const char* passed =
        "local function pair()\n"
        "    return 2 + 2, \"Test\"\n"
        "end\n"
        "local function wrap()\n"
        "    return pair()\n"
        "end\n"
        "local x, y = wrap()\n";
    expect_hover(ide::hover_luau(passed, find_nth(passed, "wrap", 1)), "function wrap(): (number, string)", "", nullptr,
                 "returned call keeps both values");
    expect_hover(ide::hover_luau(passed, find_nth(passed, "y", 0)), "y: string", "", nullptr, "wrap's second value");

    const char* trimmed =
        "local function pair()\n"
        "    return 2 + 2, \"Test\"\n"
        "end\n"
        "local function wrap()\n"
        "    return pair(), true\n"
        "end\n"
        "local x, y = wrap()\n";
    expect_hover(ide::hover_luau(trimmed, find_nth(trimmed, "wrap", 1)), "function wrap(): (number, boolean)", "", nullptr,
                 "a call before a comma keeps one value");
    expect_hover(ide::hover_luau(trimmed, find_nth(trimmed, "y", 0)), "y: boolean", "", nullptr, "wrap's boolean");

    const char* triple = "local function triple()\n    return 1, \"a\", true\nend\nlocal x, y, z = triple()\n";
    expect_hover(ide::hover_luau(triple, find_nth(triple, "triple", 1)), "function triple(): (number, string, boolean)",
                 "", nullptr, "three values");
    expect_hover(ide::hover_luau(triple, find_nth(triple, "z", 0)), "z: boolean", "", nullptr, "third value");

    const char* noted = "local function pair(): (number, string)\n    return 1, \"a\"\nend\nlocal x, y = pair()\n";
    expect_hover(ide::hover_luau(noted, find_nth(noted, "pair", 1)), "function pair(): (number, string)", "", nullptr,
                 "annotated tuple");
    expect_hover(ide::hover_luau(noted, find_nth(noted, "y", 0)), "y: string", "", nullptr, "annotated second value");

    const char* agreed =
        "local function pair(flag: boolean)\n"
        "    if flag then\n"
        "        return 1, \"a\"\n"
        "    end\n"
        "    return 2, \"b\"\n"
        "end\n";
    expect_hover(ide::hover_luau(agreed, find_nth(agreed, "pair", 0)), "function pair(flag: boolean): (number, string)",
                 "", nullptr, "agreeing tuples");

    const char* mixed =
        "local function pick(flag: boolean)\n"
        "    if flag then\n"
        "        return 1, \"a\"\n"
        "    end\n"
        "    return \"x\"\n"
        "end\n"
        "local x, y = pick(true)\n";
    const ide::HoverInfo pick = ide::hover_luau(mixed, find_nth(mixed, "pick", 1));
    const std::size_t paren = pick.title.rfind(')');
    if (!pick.found || paren == std::string::npos || paren + 1 != pick.title.size() || pick.detail == "returns nothing") {
        fail(std::string("disagreeing tuples should not claim one pack: ") + pick.title + " / " + pick.detail);
    }
    expect_hover(ide::hover_luau(mixed, find_nth(mixed, "y", 0)), "y", "local", nullptr, "disagreeing second value");

    const char* assigned =
        "local Module = require(script:FindFirstChild(\"ModuleScript\"))\n"
        "local x, y\n"
        "x, y = Module:Test()\n"
        "print(y)\n";
    expect_hover(ide::hover_luau(assigned, find_nth(assigned, "y", 2), world, 9), "y: string", "", nullptr,
                 "assignment keeps the second value");

    const char* first_wins =
        "local module = {}\n"
        "function module.greet(name: string)\n"
        "    if name == \"hi\" then\n"
        "        return name, 1\n"
        "    end\n"
        "    return true\n"
        "end\n"
        "return module\n";
    const ide::HoverInfo direct = ide::hover_luau(first_wins, find_nth(first_wins, "greet", 0));
    const std::size_t direct_paren = direct.title.rfind(')');
    if (!direct.found || direct_paren == std::string::npos || direct_paren + 1 != direct.title.size()) {
        fail(std::string("a module edited here does not keep a disagreeing tuple: ") + direct.title);
    }
    const std::vector<engine_core::LuaNode> greet_world = module_world(first_wins);
    const char* greet_use =
        "local Module = require(script:FindFirstChild(\"ModuleScript\"))\n"
        "local a, b = Module.greet(\"hi\")\n";
    expect_hover(ide::hover_luau(greet_use, find_nth(greet_use, "greet", 0), greet_world, 9),
                 "function module.greet(name: string): (string, number)", "", nullptr, "required first return pack");
    expect_hover(ide::hover_luau(greet_use, find_nth(greet_use, "a", 0), greet_world, 9), "a: string", "", nullptr,
                 "required first of the pack");
    expect_hover(ide::hover_luau(greet_use, find_nth(greet_use, "b", 0), greet_world, 9), "b: number", "", nullptr,
                 "required second of the pack");

    const char* replaced =
        "local module = {}\n"
        "function module:Test()\n"
        "    return 2+2, \"Test\"\n"
        "end\n"
        "module.Test = function()\n"
        "    return true\n"
        "end\n"
        "return module\n";
    const std::vector<engine_core::LuaNode> replaced_world = module_world(replaced);
    const char* replaced_use =
        "local Module = require(script:FindFirstChild(\"ModuleScript\"))\n"
        "local x = Module.Test()\n";
    expect_hover(ide::hover_luau(replaced_use, find_nth(replaced_use, "Test", 0), replaced_world, 9),
                 "function Test(): boolean", "", nullptr, "replaced tuple");
    expect_hover(ide::hover_luau(replaced_use, find_nth(replaced_use, "x", 0), replaced_world, 9), "x: boolean", "",
                 nullptr, "replaced tuple result");
    expect_missing(at_end("local Module = require(script:FindFirstChild(\"ModuleScript\"))\nModule:", replaced_world, 9),
                   "Test", "replaced function is not a method");
}

void expect_directive(const ide::CompletionList& list, const char* label) {
    if (list.site != ide::CompleteSite::Directive) {
        fail(std::string(label) + " is not a directive completion");
    }
}

void expect_range(const ide::CompletionList& list, int begin, int end, const char* label) {
    if (list.replace_begin != begin || list.replace_end != end) {
        fail(std::string(label) + " range " + std::to_string(list.replace_begin) + ".." +
             std::to_string(list.replace_end));
    }
}

void expect_closed(const ide::CompletionList& list, const char* label) {
    if (list.site != ide::CompleteSite::None || !list.items.empty()) {
        fail(std::string(label) + " should not complete");
    }
}

void testDirectives() {
    const ide::CompletionList all = at_end("--!");
    expect_directive(all, "--!");
    expect_range(all, 3, 3, "--!");
    if (all.items.size() != 6 || all.items.front().name != "strict") {
        fail("--! should lead with strict");
    }
    expect_has(all, "strict", "--!");
    expect_has(all, "nonstrict", "--!");
    expect_has(all, "nocheck", "--!");
    expect_has(all, "nolint", "--!");
    expect_has(all, "native", "--!");
    expect_has(all, "optimize", "--!");
    expect_missing(all, "local", "--!");
    expect_missing(all, "game", "--!");
    expect_call(all, "strict", false, "--!strict");
    expect_detail(all, "strict", "mode", "--!strict");
    expect_detail(all, "nolint", "lint", "--!nolint");
    expect_info(all, "strict", "", "", "Report type errors as errors.", "--!strict");
    expect_info(all, "nonstrict", "", "", "Report type errors as warnings.", "--!nonstrict");
    expect_info(all, "nocheck", "", "", "Skip type checking.", "--!nocheck");

    const ide::CompletionList filtered = at_end("--!str");
    expect_directive(filtered, "--!str");
    expect_range(filtered, 3, 6, "--!str");
    expect_has(filtered, "strict", "--!str");
    expect_missing(filtered, "nonstrict", "--!str");
    expect_missing(filtered, "nocheck", "--!str");
    if (filtered.prefix != "str") {
        fail("--!str prefix");
    }

    const ide::CompletionList narrow = at_end("--!n");
    expect_directive(narrow, "--!n");
    if (narrow.items.empty() || narrow.items.front().name != "nonstrict") {
        fail("--!n should lead with nonstrict");
    }
    expect_has(narrow, "nocheck", "--!n");
    expect_has(narrow, "nolint", "--!n");
    expect_has(narrow, "native", "--!n");
    expect_missing(narrow, "strict", "--!n");
    expect_missing(narrow, "optimize", "--!n");

    const char* word = "--!strict";
    const ide::CompletionList inside = at_caret(word, 6);
    expect_directive(inside, "caret inside strict");
    expect_range(inside, 3, 9, "caret inside strict");
    expect_has(inside, "strict", "caret inside strict");
    if (inside.prefix != "str") {
        fail("caret inside strict prefix");
    }

    const ide::CompletionList indented = at_end("\n  --!no");
    expect_directive(indented, "indented --!no");
    expect_has(indented, "nocheck", "indented");
    expect_has(indented, "nolint", "indented");
    expect_has(indented, "nonstrict", "indented");
    expect_missing(indented, "native", "indented");
    expect_range(indented, 6, 8, "indented --!no");

    const ide::CompletionList second = at_end("--!strict\n--!nat");
    expect_directive(second, "second header");
    expect_has(second, "native", "second header");
    expect_missing(second, "strict", "second header");

    const ide::CompletionList after_comment = at_end("-- hello\n--!s");
    expect_has(after_comment, "strict", "after a comment");
    const ide::CompletionList after_block = at_end("--[[ note ]]\n--!");
    expect_has(after_block, "optimize", "after a block comment");

    const char* unicode = "-- caf\xC3\xA9\n--!";
    const ide::CompletionList points = at_caret(unicode, 11);
    expect_directive(points, "code points");
    expect_range(points, 11, 11, "code points");
    expect_has(points, "strict", "code points");

    expect_closed(at_end("--! "), "space after bang");
    expect_closed(at_end("--!Strict"), "directive case");
    expect_closed(at_end("local x = 1\n--!"), "directive after code");
    expect_closed(at_end("local x = 1 --!"), "directive on a statement");
    expect_closed(at_end("--[[--!strict"), "unclosed block comment");
    const char* closed_block = "--[[\n--!strict\n]]";
    expect_closed(at_caret(closed_block, 8), "directive inside a block comment");
    expect_closed(at_caret("--!strict", 2), "caret before bang");
    expect_closed(at_end("-- strict"), "comment without bang");
    expect_closed(at_caret("local s = \"--!\"", 14), "bang inside a string");

    const ide::CompletionList rules = at_end("--!nolint ");
    expect_directive(rules, "--!nolint");
    expect_range(rules, 10, 10, "--!nolint ");
    expect_has(rules, "LocalUnused", "--!nolint");
    expect_has(rules, "UnknownGlobal", "--!nolint");
    expect_missing(rules, "Unknown", "--!nolint");
    expect_missing(rules, "strict", "--!nolint");
    std::vector<std::string> rule_names;
    engine_core::lint_rule_names(rule_names);
    if (rules.items.size() != rule_names.size()) {
        fail("--!nolint does not list every lint rule");
    }
    for (const std::string& name : rule_names) {
        expect_has(rules, name.c_str(), "--!nolint");
        expect_detail(rules, name.c_str(), "rule", "--!nolint");
    }

    const ide::CompletionList local_rule = at_end("--!nolint Loc");
    expect_has(local_rule, "LocalUnused", "Loc");
    expect_has(local_rule, "LocalShadow", "Loc");
    expect_missing(local_rule, "UnreachableCode", "Loc");
    expect_range(local_rule, 10, 13, "--!nolint Loc");

    const ide::CompletionList spaced = at_end("--!nolint  Loc");
    expect_has(spaced, "LocalUnused", "two spaces");
    expect_range(spaced, 11, 14, "two spaces");

    const ide::CompletionList levels = at_end("--!optimize ");
    expect_directive(levels, "--!optimize");
    expect_has(levels, "0", "optimize");
    expect_has(levels, "1", "optimize");
    expect_has(levels, "2", "optimize");
    expect_missing(levels, "3", "optimize");
    expect_detail(levels, "1", "level", "optimize 1");
    expect_info(levels, "2", "", "", "Use the full optimization level.", "optimize 2");
    if (levels.items.size() != 3 || levels.items.front().name != "0") {
        fail("optimize levels should be 0, 1, 2");
    }

    const ide::CompletionList one = at_end("--!optimize 1");
    expect_has(one, "1", "optimize 1");
    expect_missing(one, "0", "optimize 1");
    expect_missing(one, "2", "optimize 1");
    expect_range(one, 12, 13, "optimize 1");

    expect_closed(at_end("--!strict "), "strict takes no argument");
    const ide::CompletionList optimize_word = at_end("--!optimize");
    expect_has(optimize_word, "optimize", "optimize word");
    expect_missing(optimize_word, "0", "optimize word");
    expect_missing(optimize_word, "2", "optimize word");
    expect_closed(at_end("--!optimize 2 "), "finished optimize level");
    expect_closed(at_end("--!nolint LocalUnused "), "finished lint rule");

    const ide::CompletionList console = ide::complete_luau("--!no", 5, {}, 0, false);
    expect_has(console, "nocheck", "command line");
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

// `game.Configs.Door` reads children by name, as scripts run. A child that is
// in the place is its class, never optional, whether reached by a dot or by
// FindFirstChild; only a name that is not there may be nil.
void testDotChildren() {
    std::vector<engine_core::LuaNode> world;
    world.push_back(node(0, 0xffffffffu, "game", "Game"));
    world.push_back(node(3, 0, "Configs", "Folder"));
    world.push_back(node(4, 3, "Tri0", "TestTriangle"));
    world.push_back(node(6, 3, "Name", "Folder"));
    world.push_back(node(7, 3, "My Part", "Folder"));
    world.push_back(node(5, 0, "Main", "Script"));

    const ide::CompletionList root = at_end("game.", world, 5);
    expect_has(root, "Configs", "game. child");
    expect_detail(root, "Configs", "Folder", "game.Configs detail");
    expect_has(root, "Main", "game. script child");
    expect_has(root, "FindFirstChild", "game. member");

    const ide::CompletionList configs = at_end("game.Configs.", world, 5);
    expect_has(configs, "Tri0", "game.Configs. child");
    expect_detail(configs, "Tri0", "TestTriangle", "game.Configs.Tri0 detail");
    expect_has(configs, "Name", "game.Configs.Name");
    expect_detail(configs, "Name", "string", "a member wins over a child of that name");
    expect_missing(configs, "My Part", "a child that is not an identifier");
    expect_missing(configs, "Position", "Folder has no Position");

    const ide::CompletionList triangle = at_end("game.Configs.Tri0.", world, 5);
    expect_has(triangle, "Position", "game.Configs.Tri0.Position");
    expect_missing(triangle, "Source", "game.Configs.Tri0.Source");

    const ide::CompletionList local = at_end("local tri = game.Configs.Tri0\ntri.", world, 5);
    expect_has(local, "Position", "a local holding a dotted child");

    const ide::CompletionList parent = at_end("script.Parent.Configs.", world, 5);
    expect_has(parent, "Tri0", "script.Parent.Configs.");

    const ide::CompletionList found = at_end("game:FindFirstChild(\"Configs\").", world, 5);
    expect_has(found, "Tri0", "FindFirstChild then a dot");

    const ide::CompletionList colon = at_end("game.Configs:", world, 5);
    expect_missing(colon, "Tri0", "a colon offers methods, not children");

    const ide::CompletionList missing = at_end("game.Nope.", world, 5);
    expect_missing(missing, "Name", "a child that is not there");

    const char* lookup = "local tri = game:FindFirstChild(\"Configs\"):FindFirstChild(\"Tri0\")\nprint(tri)\n";
    expect_hover(ide::hover_luau(lookup, find_nth(lookup, "tri", 1), world, 5), "tri: TestTriangle", nullptr, nullptr,
                 "a child in the place is not optional");
    const char* dotted = "local tri = game.Configs.Tri0\nprint(tri)\n";
    expect_hover(ide::hover_luau(dotted, find_nth(dotted, "tri", 1), world, 5), "tri: TestTriangle", nullptr, nullptr,
                 "a dotted child");
    const char* absent = "local gone = game:FindFirstChild(\"Nope\")\nprint(gone)\n";
    expect_hover(ide::hover_luau(absent, find_nth(absent, "gone", 1), world, 5), "gone: Instance?", nullptr, nullptr,
                 "a name that is not there may be nil");
}

}  // namespace

// A module table written with nested tables: the name, each key, and the local
// that reads through them all complete and hover.
void testNestedTable() {
    const std::string head =
        "local module = {\n"
        "    Configs = {\n"
        "        ValidateTransactions = true,\n"
        "    },\n"
        "    Currencies = {\n"
        "        Gold = \"Gold\",\n"
        "        Silver = \"Silver\",\n"
        "        Copper = \"Copper\",\n"
        "    }\n"
        "}\n\n";

    expect_has(at_end(head + "local xx = modu"), "module", "nested table name");
    const ide::CompletionList top = at_end(head + "local xx = module.");
    expect_has(top, "Configs", "nested table Configs");
    expect_has(top, "Currencies", "nested table Currencies");
    expect_has(at_end(head + "local xx = module.Con"), "Configs", "nested table Con prefix");
    const ide::CompletionList inner = at_end(head + "local xx = module.Configs.");
    expect_has(inner, "ValidateTransactions", "nested ValidateTransactions");
    expect_has(at_end(head + "local xx = module.Configs.Valid"), "ValidateTransactions", "nested Valid prefix");
    expect_has(at_end(head + "local xx = module.Currencies."), "Silver", "nested Silver");

    // Typed in the middle of a ModuleScript, with the return below the caret.
    std::vector<engine_core::LuaNode> world;
    world.push_back(node(0, 0xffffffffu, "game", "Game"));
    world.push_back(node(4, 0, "Economy", "ModuleScript"));
    const char* typed[][2] = {
        {"local xx = modu", "module"},
        {"local xx = module.Con", "Configs"},
        {"local xx = module.Configs.Valid", "ValidateTransactions"},
    };
    for (const auto& row : typed) {
        const std::string before = head + row[0];
        const std::string whole = before + "\n\nreturn module\n";
        world[1].source = whole;
        const ide::CompletionList list =
            ide::complete_luau(whole, static_cast<int>(before.size()), world, 4);
        expect_has(list, row[1], row[0]);
    }

    const std::string source = head + "local xx = module.Configs.ValidateTransactions\n\nreturn module\n";
    const int use = static_cast<int>(source.find("local xx"));
    const ide::HoverInfo xx = ide::hover_luau(source, find_nth(source, "xx", 0));
    expect_hover(xx, "xx: boolean", "", nullptr, "hover xx");
    const ide::HoverInfo module = ide::hover_luau(source, static_cast<int>(source.find("module", use)));
    expect_hover(module, nullptr, nullptr, nullptr, "hover module");
    const ide::HoverInfo configs = ide::hover_luau(source, static_cast<int>(source.find("Configs", use)));
    expect_hover(configs, nullptr, nullptr, nullptr, "hover Configs");
    const ide::HoverInfo valid = ide::hover_luau(source, static_cast<int>(source.find("ValidateTransactions", use)));
    expect_hover(valid, "ValidateTransactions: boolean", nullptr, nullptr, "hover ValidateTransactions");
    const ide::HoverInfo key = ide::hover_luau(source, find_nth(source, "ValidateTransactions", 0));
    expect_hover(key, "ValidateTransactions: boolean", nullptr, nullptr, "hover key where it is written");
    const ide::HoverInfo gold = ide::hover_luau(source, find_nth(source, "Gold", 0));
    expect_hover(gold, "Gold: string", nullptr, nullptr, "hover Gold key");
    const ide::HoverInfo table_key = ide::hover_luau(source, find_nth(source, "Configs", 0));
    expect_hover(table_key, nullptr, nullptr, nullptr, "hover Configs key");
    const ide::HoverInfo declared = ide::hover_luau(source, find_nth(source, "module", 0));
    expect_hover(declared, nullptr, nullptr, nullptr, "hover module declaration");
}

int RunLuauCompleteTests() {
    try {
        testLibraries();
        testInstances();
        testVector3();
        testModule();
        testModuleMethods();
        testNames();
        testConsole();
        testStringArguments();
        testInsertFilter();
        testInstanceNew();
        testCallbackArguments();
        testFunctionParameters();
        testHover();
        testCompletionDocs();
        testTuples();
        testDirectives();
        testSkipped();
        testDotChildren();
        testNestedTable();
    } catch (const std::exception& ex) {
        fail(std::string("exception ") + ex.what());
    }
    return gFailures;
}
