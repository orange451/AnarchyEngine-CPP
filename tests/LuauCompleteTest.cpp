#include "Environment.hpp"
#include "Game.hpp"
#include "LuaApi.hpp"
#include "ScriptAnalysis.hpp"
#include "ide/ClassFilter.hpp"
#include "ide/LuauComplete.hpp"
#include "ide/Utf8.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
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

// An analysis of its own, as the editor has, for tests that ask it directly.
engine_core::ScriptAnalysis& typed_analysis() {
    static engine_core::Game game;
    static engine_core::ScriptAnalysis analysis(game);
    return analysis;
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
        if (symbol.name == "os" || symbol.name == "io" || symbol.name == "getfenv") {
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
    // Widget is not creatable, so Instance.new cannot make one; a typed local can hold one.
    const ide::CompletionList widget = at_end("local part: Widget\npart.");
    expect_has(widget, "Spark", "Widget.Spark");
    expect_has(widget, "Name", "Widget.Name");
    expect_missing(widget, "Position", "Widget.Position");
    expect_missing(widget, "Source", "Widget.Source");

    std::vector<engine_core::LuaNode> world;
    world.push_back(node(0, 0xffffffffu, "game", "Game"));
    world.push_back(node(4, 0, "Tri0", "GameObject"));
    world.push_back(node(5, 0, "Main", "Script"));
    const ide::CompletionList triangle = at_end("local tri = game:FindFirstChild(\"Tri0\")\ntri.", world, 5);
    expect_has(triangle, "Transform", "Tri0.Transform");
    expect_missing(triangle, "CFrame", "a GameObject has no CFrame alias");
    expect_missing(triangle, "Position", "a GameObject moves through Transform, not Position");
    expect_has(triangle, "Name", "Tri0.Name");
    expect_missing(triangle, "Source", "Tri0.Source");
    expect_has(triangle, "Color", "Tri0.Color");
    expect_has(triangle, "Transparency", "Tri0.Transparency");
    const ide::CompletionList waited = at_end("local tri = game:WaitForChild(\"Tri0\")\ntri.", world, 5);
    expect_has(waited, "Transform", "WaitForChild Tri0.Transform");
    expect_missing(waited, "Position", "WaitForChild Tri0.Position");
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
    world.push_back(node(4, 0, "Tri0", "GameObject"));
    world.push_back(node(5, 0, "Main", "Script"));
    const ide::CompletionList position =
        at_end("local tri = game:FindFirstChild(\"Tri0\")\ntri.Transform.Position.", world, 5);
    expect_has(position, "X", "Transform.Position.X");
    expect_has(position, "Magnitude", "Transform.Position.Magnitude");
    expect_has(position, "Abs", "Transform.Position.Abs");
    expect_missing(position, "x", "Transform.Position.x");

    const ide::CompletionList named = at_end("Vec");
    expect_has(named, "Vector3", "Vec");

    const ide::CompletionList enums = at_end("Enum.");
    expect_has(enums, "NormalId", "Enum.NormalId");
    expect_has(enums, "Axis", "Enum.Axis");
    expect_has(enums, "KeyCode", "Enum.KeyCode");
    expect_has(enums, "UserInputType", "Enum.UserInputType");
    expect_has(enums, "UserInputState", "Enum.UserInputState");
    const ide::CompletionList keys = at_end("Enum.KeyCode.");
    expect_has(keys, "W", "Enum.KeyCode.W");
    expect_has(keys, "LeftShift", "Enum.KeyCode.LeftShift");
    const ide::CompletionList faces = at_end("Enum.NormalId.");
    expect_has(faces, "Top", "Enum.NormalId.Top");
    const ide::CompletionList item = at_end("Enum.KeyCode.W.");
    expect_has(item, "Name", "Enum.KeyCode.W.Name");

    const ide::CompletionList input = at_end("local input = game:GetService(\"UserInputService\")\ninput.");
    expect_has(input, "InputBegan", "UserInputService.InputBegan");
    expect_has(input, "InputEnded", "UserInputService.InputEnded");
    const ide::CompletionList input_method = at_end("local input = game:GetService(\"UserInputService\")\ninput:");
    expect_has(input_method, "IsKeyDown", "UserInputService:IsKeyDown");
    expect_has(input_method, "GetMouseLocation", "UserInputService:GetMouseLocation");
    const ide::CompletionList mouse = at_end("local input = game:GetService(\"UserInputService\")\ninput:GetMouseLocation().");
    expect_has(mouse, "X", "GetMouseLocation().X");
    expect_has(mouse, "Magnitude", "GetMouseLocation().Magnitude");
    expect_missing(mouse, "Z", "a Vector2 has no Z");
    const ide::CompletionList vector2 = at_end("Vector2.");
    expect_has(vector2, "new", "Vector2.new");
    expect_has(vector2, "zero", "Vector2.zero");
    const ide::CompletionList vector2_value = at_end("local v = Vector2.new(1, 2)\nv:");
    expect_has(vector2_value, "Dot", "Vector2:Dot");
    const ide::CompletionList color3 = at_end("Color3.");
    expect_has(color3, "fromRGB", "Color3.fromRGB");
    expect_has(color3, "fromHex", "Color3.fromHex");
    const ide::CompletionList color3_value = at_end("local c = Color3.fromRGB(255, 0, 0)\nc.");
    expect_has(color3_value, "R", "Color3.R");
    expect_missing(color3_value, "A", "a Color3 has no alpha");
    const ide::CompletionList color3_method = at_end("local c = Color3.new(1, 0, 0)\nc:");
    expect_has(color3_method, "Lerp", "Color3:Lerp");
    expect_has(color3_method, "ToHex", "Color3:ToHex");
    const ide::CompletionList placed = at_end("local part = Instance.new(\"GameObject\")\npart.Transform.Position.");
    expect_has(placed, "X", "GameObject.Transform.Position.X");
    expect_missing(placed, "R", "GameObject.Transform.Position is not a Color3");
    const ide::CompletionList placed_method = at_end("local part = Instance.new(\"GameObject\")\npart.Transform.Position:");
    expect_has(placed_method, "Dot", "GameObject.Transform.Position:Dot");

    // A Connect callback's parameters take the signal's types without an annotation.
    const ide::CompletionList inferred = at_end(
        "game:GetService(\"UserInputService\").InputBegan:Connect(function(input, gameProcessedEvent)\n    input.");
    expect_has(inferred, "KeyCode", "inferred InputObject.KeyCode");
    expect_has(inferred, "UserInputType", "inferred InputObject.UserInputType");
    const ide::CompletionList inferred_local = at_end(
        "local input = game:GetService(\"UserInputService\")\ninput.InputEnded:Connect(function(io)\n"
        "    print(io.KeyCode)\n    io.");
    expect_has(inferred_local, "Position", "inferred InputObject through a local");
    const ide::CompletionList inferred_dot = at_end(
        "local input = game:GetService(\"UserInputService\")\ninput.InputChanged.Connect(input.InputChanged, function(io)\n"
        "    io.");
    expect_has(inferred_dot, "Delta", "inferred InputObject in a dot call");
    const ide::CompletionList inferred_dt =
        at_end("game:GetService(\"RunService\").Heartbeat:Connect(function(dt)\n    local step = dt\n    step.");
    expect_missing(inferred_dt, "KeyCode", "dt is a number");
    const ide::CompletionList annotated = at_end(
        "game:GetService(\"UserInputService\").InputBegan:Connect(function(input: Instance)\n    input.");
    expect_missing(annotated, "KeyCode", "an annotation wins over the signal's type");
    expect_has(annotated, "Name", "an annotated Instance parameter");
    const ide::CompletionList nested = at_end(
        "game:GetService(\"UserInputService\").InputBegan:Connect(print(function(input)\n    input.");
    expect_missing(nested, "KeyCode", "a function that is not the argument itself");
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
    // debug is back with only the profiler's two functions.
    expect_has(at_end("debug"), "debug", "debug for the profiler");
    const ide::CompletionList marks = at_end("debug.");
    expect_has(marks, "profilebegin", "debug.profilebegin");
    expect_has(marks, "profileend", "debug.profileend");
    expect_missing(marks, "traceback", "debug.traceback stays removed");
    expect_missing(marks, "getinfo", "debug.getinfo stays removed");
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
    world.push_back(node(3, 0, "Tri0", "GameObject"));
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

// The part of the signature drawn bold: the parameter being typed.
std::string bolded(const ide::CompletionList& list) {
    if (list.signature_bold_begin < 0 || list.signature_bold_end > static_cast<int>(list.signature.size())) {
        return {};
    }
    return list.signature.substr(static_cast<std::size_t>(list.signature_bold_begin),
                                 static_cast<std::size_t>(list.signature_bold_end - list.signature_bold_begin));
}

void expect_bold(const ide::CompletionList& list, const char* part, const char* label) {
    if (bolded(list) != part) {
        fail(std::string(label) + " bolds '" + bolded(list) + "' of '" + list.signature + "'");
    }
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
    expect_bold(invoke, "a: string", "open call");
    expect_bold(first, "a: string", "first argument");
    expect_bold(second, "b: Instance", "second argument");
    expect_bold(at_end(std::string(defined) + "test_func(\"test\", game, "), "", "past the last parameter");
    expect_bold(at_end("local function log(tag: string, ...)\nend\nlog(\"a\", 1, "), "...", "a variadic argument");

    // A Connect callback: first what Connect takes, then, inside `function(`,
    // what the signal passes, following the parameter being typed.
    const std::string began = "game:GetService(\"UserInputService\").InputBegan:Connect(";
    const ide::CompletionList connect = at_end(began);
    expect_signature(connect, "(callback: (input: InputObject, gameProcessedEvent: boolean) -> ())", "Connect(");
    expect_bold(connect, "callback: (input: InputObject, gameProcessedEvent: boolean) -> ()", "Connect(");
    const ide::CompletionList opened = at_end(began + "function(");
    expect_signature(opened, "function(input: InputObject, gameProcessedEvent: boolean)", "callback parameters");
    expect_bold(opened, "input: InputObject", "callback's first parameter");
    expect_bold(at_end(began + "function(inp"), "input: InputObject", "typing the first parameter");
    const ide::CompletionList next = at_end(began + "function(input, ");
    expect_bold(next, "gameProcessedEvent: boolean", "callback's second parameter");
    expect_has(next, "gameProcessedEvent", "callback's second parameter");
    expect_bold(at_end(began + "function(input, game"), "gameProcessedEvent: boolean", "typing the second parameter");
    if (!at_end(began + "function(input, processed)\n    ").signature.empty()) {
        fail("the callback's body shows no signature");
    }
    // The editor closes the brackets as they are typed, so the caret sits before `))`.
    const std::pair<const char*, const char*> closers[] = {{"function(", "input: InputObject"},
                                                           {"function(input, ", "gameProcessedEvent: boolean"}};
    for (const auto& [head, part] : closers) {
        const std::string typed = began + head;
        const std::string closed = typed + "))";
        expect_bold(ide::complete_luau(closed, static_cast<int>(typed.size())), part, "before the closing brackets");
    }
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
    // Folders, then scripts and CSS, then the world's objects, each cluster A to Z.
    const std::vector<std::string> expected = {
        "Folder",
        "CSS", "ModuleScript", "Script",
        "Camera",
        "GameObject", "Model", "PhysicsObject", "PlayerController", "Terrain",
        "AmbientOcclusionEffect", "BloomEffect", "DynamicSky", "ScreenSpaceReflections", "Skybox",
        "DirectionalLight", "PointLight", "SpotLight",
        "Sound", "SoundEmitter",
        "Attachment",
        "BillboardGui", "ScreenGui",
        "HBox", "ImagePane", "Pane", "VBox",
        "Button", "Label", "TextField",
        "Dragger", "Material", "Mesh", "Prefab", "Texture"};
    if (shown != expected) {
        fail("insert list is every creatable class, in clusters, each A to Z");
    }
    ide::filter_class_names(names, "scr", shown);
    // Names that start with it first, then those that have it inside, each in clusters.
    if (shown != std::vector<std::string>{"Script", "ScreenSpaceReflections", "ScreenGui", "ModuleScript"}) {
        fail("scr lists Script first and ModuleScript after the names that start with it");
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
    if (engine_core::lua_creatable_known("Workspace") || engine_core::lua_creatable_known("DataModel") ||
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
    expect_missing(made, "Workspace", "Instance.new is not every class");
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
    expect_missing(open, "Workspace", "open Instance.new");
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
    expect_has(object, "Transform", "created GameObject");
    expect_missing(object, "Position", "created GameObject");
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

    const char* connected =
        "game:GetService(\"UserInputService\").InputBegan:Connect(function(input, processed)\n    print(input)\nend)";
    expect_hover(ide::hover_luau(connected, find_nth(connected, "input", 1)), "input: InputObject", nullptr, nullptr,
                 "a Connect parameter");
    expect_hover(ide::hover_luau(connected, find_nth(connected, "processed", 0)), "processed: boolean", nullptr, nullptr,
                 "a Connect parameter's declaration");
    // The `function` keyword of a callback names the signal that runs it.
    expect_hover(ide::hover_luau(connected, find_nth(connected, "function", 0)),
                 "function(input: InputObject, processed: boolean)",
                 "callback, runs each time UserInputService.InputBegan fires", "gameProcessedEvent", "a Connect callback");
    const char* dotted =
        "local signal = game:GetService(\"UserInputService\").InputEnded\nsignal.Connect(signal, function(input) end)";
    expect_hover(ide::hover_luau(dotted, find_nth(dotted, "function", 0)), "function(input: InputObject)",
                 "callback, runs each time UserInputService.InputEnded fires", nullptr, "a callback passed with a dot");
    const char* lambda = "local double = function(x: number)\n    return x * 2\nend";
    expect_hover(ide::hover_luau(lambda, find_nth(lambda, "function", 0)), "function(x: number): number",
                 "anonymous function", nullptr, "an anonymous function");
    const char* deferred = "task.defer(function() end)";
    expect_hover(ide::hover_luau(deferred, find_nth(deferred, "function", 0)), "function()",
                 "anonymous function, returns nothing", nullptr, "an anonymous function returning nothing");
    const char* named = "local function greet() end";
    if (ide::hover_luau(named, find_nth(named, "function", 0)).found) {
        fail("the keyword of a named function shows a tip");
    }

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
    world.push_back(node(4, 3, "Tri0", "GameObject"));
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
    expect_detail(configs, "Tri0", "GameObject", "game.Configs.Tri0 detail");
    expect_has(configs, "Name", "game.Configs.Name");
    expect_detail(configs, "Name", "string", "a member wins over a child of that name");
    expect_missing(configs, "My Part", "a child that is not an identifier");
    expect_missing(configs, "Transform", "Folder has no Transform");

    const ide::CompletionList triangle = at_end("game.Configs.Tri0.", world, 5);
    expect_has(triangle, "Transform", "game.Configs.Tri0.Transform");
    expect_missing(triangle, "Source", "game.Configs.Tri0.Source");

    const ide::CompletionList local = at_end("local tri = game.Configs.Tri0\ntri.", world, 5);
    expect_has(local, "Transform", "a local holding a dotted child");

    const ide::CompletionList parent = at_end("script.Parent.Configs.", world, 5);
    expect_has(parent, "Tri0", "script.Parent.Configs.");

    const ide::CompletionList found = at_end("game:FindFirstChild(\"Configs\").", world, 5);
    expect_has(found, "Tri0", "FindFirstChild then a dot");

    const ide::CompletionList colon = at_end("game.Configs:", world, 5);
    expect_missing(colon, "Tri0", "a colon offers methods, not children");

    const ide::CompletionList missing = at_end("game.Nope.", world, 5);
    expect_missing(missing, "Name", "a child that is not there");

    const char* lookup = "local tri = game:FindFirstChild(\"Configs\"):FindFirstChild(\"Tri0\")\nprint(tri)\n";
    expect_hover(ide::hover_luau(lookup, find_nth(lookup, "tri", 1), world, 5), "tri: GameObject", nullptr, nullptr,
                 "a child in the place is not optional");
    const char* dotted = "local tri = game.Configs.Tri0\nprint(tri)\n";
    expect_hover(ide::hover_luau(dotted, find_nth(dotted, "tri", 1), world, 5), "tri: GameObject", nullptr, nullptr,
                 "a dotted child");
    const char* absent = "local gone = game:FindFirstChild(\"Nope\")\nprint(gone)\n";
    expect_hover(ide::hover_luau(absent, find_nth(absent, "gone", 1), world, 5), "gone: Instance?", nullptr, nullptr,
                 "a name that is not there may be nil");
}

std::string repeated(const char* piece, int count) {
    std::string out;
    for (int i = 0; i < count; ++i) {
        out += piece;
    }
    return out;
}

// Generated or pasted code can nest far deeper than anyone writes by hand.
// Completing or hovering after it must not overflow the stack.
void testDeepNesting() {
    const std::string tail = "\nlocal t = {y = 1}\nt.";
    const std::string parens = "local x = " + std::string(100000, '(') + "1" + tail;
    at_end(parens);
    ide::hover_luau(parens, static_cast<int>(parens.size()) - 1);
    at_end("local x = " + repeated("not ", 100000) + "true" + tail);
    at_end("local x = -" + repeated("#-", 100000) + "1" + tail);
    at_end("local s = \"a\"" + repeated(" .. \"a\"", 100000) + tail);
    at_end("local v = " + repeated("{", 100000) + tail);
    at_end(repeated("do ", 100000) + tail);
    at_end(repeated("function f() ", 50000) + tail);
    at_end(repeated("local f = function() ", 50000) + tail);

    // Each required module is read by a parser of its own, started from inside the
    // one that reached the require. They share one depth budget, or a chain of
    // eight modules could each nest as deep as one.
    std::vector<engine_core::LuaNode> chain{node(10, 0, "Chain", "Folder")};
    for (int k = 1; k <= 8; ++k) {
        const std::string inner = k < 8 ? "require(script.Parent.M" + std::to_string(k + 1) + ")" : "{y = 1}";
        const std::string name = "M" + std::to_string(k);
        chain.push_back(node(static_cast<std::uint32_t>(10 + k), 10, name.c_str(), "ModuleScript",
                             "return " + std::string(130, '(') + inner + std::string(130, ')')));
    }
    chain.push_back(node(30, 0, "Main", "Script"));
    at_end("local m = require(game.Chain.M1)\nm.", chain, 30);

    // Nesting as deep as real code goes still completes.
    expect_has(at_end("local t = {y = 1}\nprint(" + std::string(40, '(') + "t."), "y", "t. inside 40 parentheses");
    expect_has(at_end("local t = {y = 1}\n" + repeated("do ", 40) + "t."), "y", "t. inside 40 blocks");
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

// Regression for d25981a: rows copied before their names were set left every member
// of a class unnamed. Every registered class has named members, and the value types
// complete and hover theirs.
void testRegisteredMembers() {
    std::vector<std::string> classes;
    engine_core::lua_class_names(classes);
    if (classes.empty()) {
        fail("no classes are registered");
    }
    for (const std::string& name : classes) {
        std::vector<engine_core::LuaField> fields;
        engine_core::lua_class_own_members(name.c_str(), fields);
        for (const engine_core::LuaField& field : fields) {
            if (field.name == nullptr || field.name[0] == '\0') {
                fail(name + " has a member with no name");
            }
        }
    }
    const char* members[][2] = {
        {"Vector3", "X"},       {"Vector3", "Y"},        {"Vector3", "Z"},     {"Vector3", "Magnitude"},
        {"Vector3", "Unit"},    {"Vector3", "Dot"},      {"Vector3", "Cross"}, {"Vector3", "Lerp"},
        {"EnumItem", "Name"},   {"EnumItem", "Value"},   {"EnumItem", "EnumType"},
    };
    for (const auto& row : members) {
        const engine_core::LuaField* field = engine_core::lua_class_find(row[0], row[1]);
        if (field == nullptr || field->name == nullptr || std::string_view(field->name) != row[1]) {
            fail(std::string(row[0]) + "." + row[1] + " is not registered by name");
        }
    }

    const ide::CompletionList vector = at_end("local v = Vector3.new(1, 2, 3)\nlocal m = v.");
    expect_has(vector, "X", "Vector3 local X");
    expect_has(vector, "Magnitude", "Vector3 local Magnitude");
    const ide::CompletionList method = at_end("local v = Vector3.new(1, 2, 3)\nlocal m = v:");
    expect_has(method, "Dot", "Vector3 local :Dot");
    expect_has(method, "Lerp", "Vector3 local :Lerp");
    const ide::CompletionList axis = at_end("local v = Vector3.xAxis.");
    expect_has(axis, "Unit", "Vector3.xAxis.Unit");

    const std::string read = "local v = Vector3.new(1, 2, 3)\nlocal m = v.Magnitude\n";
    expect_hover(ide::hover_luau(read, find_nth(read, "Magnitude", 0)), "Magnitude: number", nullptr, nullptr,
                 "hover Vector3 Magnitude");
    const std::string unit = "local v = Vector3.new(1, 2, 3)\nlocal u = v.Unit\n";
    expect_hover(ide::hover_luau(unit, find_nth(unit, "Unit", 0)), "Unit: Vector3", nullptr, nullptr,
                 "hover Vector3 Unit");
}

// Regression for 4cf379e: a table constructor in the script being edited, not only in a
// required ModuleScript, gives its keys to completion and hover.
void testScriptTableKeys() {
    const std::string deep = "local t = { a = { b = { c = 1, d = \"x\" } } }\n";
    expect_has(at_end(deep + "local x = t."), "a", "script table a");
    expect_has(at_end(deep + "local x = t.a."), "b", "script table a.b");
    const ide::CompletionList leaves = at_end(deep + "local x = t.a.b.");
    expect_has(leaves, "c", "script table a.b.c");
    expect_has(leaves, "d", "script table a.b.d");
    expect_hover(ide::hover_luau(deep, find_nth(deep, "c", 0)), "c: number", nullptr, nullptr,
                 "hover a key three tables deep");
    expect_hover(ide::hover_luau(deep, find_nth(deep, "d", 0)), "d: string", nullptr, nullptr,
                 "hover a string key where it is written");
    const std::string used = deep + "local x = t.a.b.c\n";
    expect_hover(ide::hover_luau(used, find_nth(used, "c", 1)), "c: number", nullptr, nullptr,
                 "hover a key where it is read");

    // A key whose value is a function completes as a call.
    const ide::CompletionList calls = at_end("local t = { run = function(a) return a end, n = 2 }\nt.");
    expect_has(calls, "run", "function key");
    expect_call(calls, "run", true, "function key is a call");
    expect_call(calls, "n", false, "number key is not a call");

    // A table built inside a function.
    expect_has(at_end("local function f()\n    local inner = { speed = 3 }\n    local x = inner."), "speed",
               "table inside a function");

    // Typing inside a constructor that is not closed yet still completes names around it.
    expect_has(at_end("local alpha = 1\nlocal t = {\n    k = alp"), "alpha", "local inside an unclosed table");
    expect_has(at_end("local alpha = 1\nlocal t = {\n    k = { j = alp"), "alpha", "local inside two unclosed tables");
    // A later, closed table after an unclosed-looking line still reads its keys.
    expect_has(at_end("local t = { a = 1 }\nlocal u = { b = t.a }\nlocal x = u."), "b", "table reading another table");
}

// A `.` that starts a line at the top of a script completes ModuleScripts and
// services, and accepting writes the declaration that reaches them.
void testRequire() {
    std::vector<engine_core::LuaNode> world;
    world.push_back(node(0, 0xffffffffu, "game", "Game"));
    world.push_back(node(1, 0, "Workspace", "Folder"));
    world.push_back(node(2, 1, "Folder", "Folder"));
    world.push_back(node(3, 2, "Config", "ModuleScript", "return {}"));
    world.push_back(node(4, 2, "My Module", "ModuleScript", "return {}"));
    world.push_back(node(5, 1, "Name", "Folder"));
    world.push_back(node(6, 5, "Shadowed", "ModuleScript", "return {}"));
    world.push_back(node(7, 0, "Main", "Script"));
    world.push_back(node(8, 0, "ConfigFolder", "Folder"));
    world.push_back(node(9, 0xffffffffu, "Loose", "ModuleScript", "return {}"));

    auto expect_insert = [](const ide::CompletionList& list, const char* name, const char* insert, const char* label) {
        const ide::CompletionItem* item = find_item(list, name);
        if (item == nullptr) {
            fail(std::string(label) + " missing " + name);
            return;
        }
        if (item->insert != insert) {
            fail(std::string(label) + " inserts '" + item->insert + "'");
        }
        if (item->title != insert) {
            fail(std::string(label) + " title '" + item->title + "'");
        }
    };
    auto expect_require = [](const ide::CompletionList& list, const char* label) {
        if (list.site != ide::CompleteSite::Require) {
            fail(std::string(label) + " is not a require");
        }
    };
    auto expect_not_require = [](const ide::CompletionList& list, const char* label) {
        if (list.site == ide::CompleteSite::Require) {
            fail(std::string(label) + " should not complete a require");
        }
    };

    const ide::CompletionList conf = at_end(".Conf", world, 7);
    expect_require(conf, ".Conf");
    expect_insert(conf, "Config", "local Config = require(game.Workspace.Folder.Config)", ".Conf");
    expect_detail(conf, "Config", "game.Workspace.Folder.Config", ".Conf detail");
    expect_missing(conf, "ConfigFolder", "a Folder is not a module");
    expect_missing(conf, "UserInputService", ".Conf is not a service");
    if (conf.replace_begin != 0 || conf.replace_end != 5 || conf.prefix != "Conf") {
        fail(".Conf replaces the dot and the name");
    }

    const ide::CompletionList input = at_end(".UserIn", world, 7);
    expect_require(input, ".UserIn");
    expect_insert(input, "UserInputService", "local UserInputService = game:GetService(\"UserInputService\")",
                  ".UserIn");
    expect_detail(input, "UserInputService", "service", ".UserIn detail");
    expect_missing(input, "Config", ".UserIn is not a module");

    const ide::CompletionList bare = at_end(".", world, 7);
    expect_require(bare, "a lone dot");
    expect_has(bare, "Config", "a lone dot lists modules");
    expect_has(bare, "RunService", "a lone dot lists services");
    expect_has(bare, "Selection", "a lone dot lists every service");
    expect_missing(bare, "Loose", "a module outside game");
    expect_missing(bare, "Main", "a Script is not a module");

    // A name that is not an identifier is indexed, and its local drops what cannot be in a name.
    expect_insert(at_end(".My", world, 7), "My Module", "local MyModule = require(game.Workspace.Folder[\"My Module\"])",
                  "a module name with a space");
    // A child a member shadows, such as a Folder called Name, is found by name.
    expect_insert(at_end(".Sha", world, 7), "Shadowed",
                  "local Shadowed = require(game.Workspace:FindFirstChild(\"Name\").Shadowed)",
                  "a child a member shadows");

    // The caret inside the name still replaces the whole name.
    const ide::CompletionList middle = ide::complete_luau(".Config\n", 3, world, 7);
    expect_require(middle, "caret inside the name");
    if (middle.replace_begin != 0 || middle.replace_end != 7 || middle.prefix != "Co") {
        fail("caret inside the name replaces through its end");
    }

    // A module does not require itself.
    expect_missing(at_end(".Conf", world, 3), "Config", "the module being edited");

    // After statements at the top of the chunk, and after blocks that have closed.
    expect_require(at_end("local x = 1\n.Conf", world, 7), "after a local");
    expect_require(at_end("print(\"hi\")\n    .Conf", world, 7), "indented after a call");
    expect_require(at_end("local t = { a = 1 }\n.Conf", world, 7), "after a closed table");
    expect_require(at_end("local function f()\nend\n.Conf", world, 7), "after a closed function");
    expect_require(at_end("if ready then\n    print(1)\nelse\n    print(2)\nend\n.Conf", world, 7), "after an if");
    expect_require(at_end("for i = 1, 3 do\nend\n.Conf", world, 7), "after a for");
    expect_require(at_end("while go(function() end) do\nend\n.Conf", world, 7), "after a while");
    expect_require(at_end("repeat\n    step()\nuntil done\n.Conf", world, 7), "after a repeat");
    expect_require(at_end("local y = if a then 1 else 2\n.Conf", world, 7), "an if-expression has no end");
    expect_require(at_end("do\nend;\n.Conf", world, 7), "after a semicolon");

    // Inside a function, a block, or a bracket.
    expect_not_require(at_end("local function f()\n    .Conf", world, 7), "inside a function");
    expect_not_require(at_end("game.Changed:Connect(function()\n    .Conf", world, 7), "inside a callback");
    expect_not_require(at_end("do\n    .Conf", world, 7), "inside do");
    expect_not_require(at_end("if ready then\n    .Conf", world, 7), "inside if");
    expect_not_require(at_end("if ready then\nelse\n    .Conf", world, 7), "inside else");
    expect_not_require(at_end("for i = 1, 3 do\n    .Conf", world, 7), "inside for");
    expect_not_require(at_end("while true do\n    .Conf", world, 7), "inside while");
    expect_not_require(at_end("repeat\n    .Conf", world, 7), "inside repeat");
    expect_not_require(at_end("local t = {\n    .Conf", world, 7), "inside a table");
    expect_not_require(at_end("print(\n    .Conf", world, 7), "inside a call");
    expect_not_require(at_end("local function f()\n    if a then\n    end\n    .Conf", world, 7),
                       "a closed block inside a function");

    // A dot that does not start a statement.
    expect_not_require(at_end("local x = game.Conf", world, 7), "a member after a name");
    expect_not_require(at_end("local x =\n.Conf", world, 7), "the value of an assignment");
    expect_not_require(at_end("local x = 1 +\n.Conf", world, 7), "after an operator");
    expect_not_require(at_end("return\n.Conf", world, 7), "after return");

    // The command line has no script to declare locals in.
    const ide::CompletionList console = ide::complete_luau(".Conf", 5, world, 0, false);
    expect_not_require(console, "the command line");
}


// Arithmetic on a vector is a vector, not a number: the VM's native vector,
// and a class through its registered operators.
void testVectorArithmetic() {
    const char* cases[] = {
        "local w = Vector3.new(1, 2, 3) * 2\nw.",
        "local w = 2 * Vector3.new(1, 2, 3)\nw.",
        "local w = Vector3.new(1, 2, 3) + Vector3.new(4, 5, 6)\nw.",
        "local w = Vector3.new(1, 2, 3) / 2 - Vector3.one\nw.",
        "local w = -Vector3.new(1, 2, 3)\nw.",
    };
    for (const char* source : cases) {
        expect_has(at_end(source), "Lerp", source);
        const std::string text = source;
        expect_hover(ide::hover_luau(text, find_nth(text, "w", 0)), "w: Vector3", nullptr, nullptr, source);
    }
    expect_has(at_end("local p = Vector2.new(1, 2) * 2\np."), "Lerp", "Vector2 times a number");
    expect_has(at_end("local p = Vector2.new(1, 2) + Vector2.new(3, 4)\np."), "Lerp", "Vector2 plus Vector2");
    const char* numbers = "local x = 1 + 2 * 3\nlocal y = -x\nprint(x, y)";
    expect_hover(ide::hover_luau(numbers, find_nth(numbers, "x", 0)), "x: number", nullptr, nullptr, "numbers stay numbers");
    expect_hover(ide::hover_luau(numbers, find_nth(numbers, "y", 0)), "y: number", nullptr, nullptr, "a negated number");
}

void testTypedFallback() {
    const char* account =
        "local Account = {}\n"
        "Account.__index = Account\n"
        "\n"
        "function Account.new(owner: string)\n"
        "    local self = setmetatable({}, Account)\n"
        "    self.owner = owner\n"
        "    self.balance = 0\n"
        "    return self\n"
        "end\n"
        "\n"
        "function Account:Deposit(amount: number)\n"
        "    self.balance += amount\n"
        "end\n"
        "\n"
        "return Account\n";
    std::vector<engine_core::LuaNode> world;
    world.push_back(node(0, 0xffffffffu, "game", "Game"));
    world.push_back(node(9, 0, "Main", "Script"));
    world.push_back(node(8, 0, "Account", "ModuleScript", account));
    world.push_back(node(7, 0, "Parts", "Folder"));
    world.push_back(node(6, 7, "Door", "GameObject"));
    const std::string use = "local Account = require(game.Account)\nlocal a = Account.new(\"me\")\n";

    // A metatable object from a required class.
    const ide::CompletionList methods = at_end(use + "a:", world, 9);
    expect_has(methods, "Deposit", "a required class's method");
    expect_call(methods, "Deposit", true, "a required class's method");
    expect_info(methods, "Deposit", "returns nothing", "function Account:Deposit(self: table, amount: number)", nullptr,
                "a method's row reads as its definition");
    expect_missing(methods, "balance", "a field after ':'");
    expect_missing(methods, "__index", "a metamethod");
    const ide::CompletionList fields = at_end(use + "a.", world, 9);
    expect_has(fields, "balance", "a class instance's field");
    expect_has(fields, "owner", "a class instance's field");
    expect_detail(fields, "owner", "string", "a field's type");
    expect_missing(fields, "__index", "a metamethod after '.'");
    expect_missing(fields, "Deposit", "a method after '.'");
    const ide::CompletionList typed = at_end(use + "a:De", world, 9);
    expect_has(typed, "Deposit", "a typed prefix");
    if (typed.items.size() != 1) {
        fail("a typed prefix keeps only the names it starts");
    }

    // The same class written in the script.
    const std::string local_class = std::string(account).substr(0, std::string(account).rfind("return")) +
                                    "local b = Account.new(\"x\")\nb:";
    expect_has(at_end(local_class), "Deposit", "a class in the script");

    // A loop variable, a string from a library call, and a generic result.
    const ide::CompletionList child = at_end("for _, child in game.Parts:GetChildren() do\n    child.", world, 9);
    expect_has(child, "Name", "a loop variable over GetChildren");
    expect_has(child, "Parent", "a loop variable over GetChildren");
    const ide::CompletionList word = at_end("for index, name in ipairs({ \"a\", \"b\" }) do\n    name:");
    expect_info(word, "upper", "string", "function string:upper(): string", nullptr, "a string method");
    expect_missing(word, "char", "string.char is not a method");
    expect_has(at_end("local words = string.split(\"a b\", \" \")\nwords[1]:"), "lower", "a string from a call");
    expect_has(at_end("local function pick<T>(items: { T }): T\n    return items[1]\nend\nlocal s = pick({ \"x\" })\ns:"),
               "upper", "a generic result");

    // Fields assigned after the table is made, and a pcall result.
    const ide::CompletionList assigned = at_end("local t = {}\nt.alpha = 1\nt.beta = function() end\nt.");
    expect_detail(assigned, "alpha", "number", "a field assigned later");
    expect_call(assigned, "beta", true, "a function assigned later");
    expect_has(at_end("local ok, result = pcall(function() return { value = 1 } end)\nresult."), "value",
               "a pcall result");

    // A module's returned table.
    std::vector<engine_core::LuaNode> lib;
    lib.push_back(node(0, 0xffffffffu, "game", "Game"));
    lib.push_back(node(8, 0, "Lib", "ModuleScript",
                       "local extra = { zoom = function() end }\nreturn { alpha = 1, beta = function() end, nested = extra }\n"));
    lib.push_back(node(9, 0, "Main", "Script", ""));
    const ide::CompletionList returned = at_end("local m = require(game:FindFirstChild(\"Lib\"))\nm.", lib, 9);
    expect_detail(returned, "alpha", "number", "a module's field");
    expect_call(returned, "beta", true, "a module's function");
    expect_has(returned, "nested", "a module's table");
    // A method the module replaced with a plain function is not offered after ':'.
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
    expect_missing(at_end("local m = require(game:FindFirstChild(\"Lib\"))\nm:", replaced_world, 4), "Test",
                   "a replaced method stays off ':'");
    // An annotated table return.
    const ide::CompletionList made = at_end(
        "local function make(): { speed: number, name: string }\n    return { speed = 1, name = \"a\" }\nend\nmake().");
    expect_has(made, "speed", "an annotated table return");
    expect_has(made, "name", "an annotated table return");
    // Instances and Vector3.
    expect_has(at_end("game."), "FindFirstChild", "instance methods after '.'");
    expect_has(at_end("Vector3.new()."), "Lerp", "Vector3 methods");
}

void testTypedHoverAndSignature() {
    const char* account =
        "local Account = {}\n"
        "Account.__index = Account\n"
        "function Account.new(owner: string)\n"
        "    local self = setmetatable({}, Account)\n"
        "    self.owner = owner\n"
        "    self.balance = 0\n"
        "    return self\n"
        "end\n"
        "function Account:Deposit(amount: number, note: string?)\n"
        "    self.balance += amount\n"
        "end\n"
        "return Account\n";
    std::vector<engine_core::LuaNode> world;
    world.push_back(node(0, 0xffffffffu, "game", "Game"));
    world.push_back(node(9, 0, "Main", "Script"));
    world.push_back(node(8, 0, "Account", "ModuleScript", account));
    const std::string use = "local Account = require(game.Account)\nlocal a = Account.new(\"me\")\na:Deposit(5)\nprint(a.owner)\n";

    // A required class's method, named as its definition wrote it.
    const ide::HoverInfo method = ide::hover_luau(use, find_nth(use, "Deposit", 0), world, 9);
    expect_hover(method, "function Account:Deposit(self: table, amount: number, note: string?)", "returns nothing",
                 nullptr, "a required class's method");
    if (method.begin != find_nth(use, "Deposit", 0) || method.end != method.begin + 7) {
        fail("the hover covers the method's name");
    }
    const ide::HoverInfo field = ide::hover_luau(use, find_nth(use, "owner", 0), world, 9);
    expect_hover(field, "owner: string", "", nullptr, "a class instance's field");
    const ide::HoverInfo object = ide::hover_luau(use, find_nth(use, "a", 2), world, 9);
    if (!object.found || object.title.rfind("a: ", 0) != 0 || object.title.size() <= 3) {
        fail("a metatable object's hover has its type: " + object.title);
    }

    // A stray byte that is not UTF-8 counts as one code point, as the editor counts it.
    const std::string stray = "-- 90\xB0 degrees\n" + use;
    const std::size_t a_byte = stray.find("print(a") + 6;
    const ide::HoverInfo after_stray = ide::hover_luau(stray, ide::CodePointsBefore(stray, a_byte), world, 9);
    if (!after_stray.found || after_stray.title.rfind("a: ", 0) != 0 ||
        after_stray.begin != ide::CodePointsBefore(stray, a_byte)) {
        fail("a hover after a malformed byte finds its name: " + after_stray.title);
    }

    // A hover that only says table shows the table's shape.
    const char* shaped = "local t = { a = 1, b = \"x\" }\nprint(t)\n";
    const ide::HoverInfo table = ide::hover_luau(shaped, find_nth(shaped, "t", 1));
    if (table.title.rfind("t: {", 0) != 0 || table.title.find("a: number") == std::string::npos ||
        table.title.find("b: string") == std::string::npos) {
        fail("a table's hover shows its shape: " + table.title);
    }

    const char* counted = "local count = 1\nprint(count)\n";
    expect_hover(ide::hover_luau(counted, find_nth(counted, "count", 1)), "count: number", nullptr, nullptr,
                 "a typed local");

    // A call's signature.
    const ide::CompletionList first = at_end(use + "a:Deposit(", world, 9);
    expect_signature(first, "(amount: number, note: string?)", "a method's signature drops self");
    expect_bold(first, "amount: number", "the first argument");
    const ide::CompletionList second = at_end(use + "a:Deposit(5, ", world, 9);
    expect_bold(second, "note: string?", "the second argument");
    const ide::CompletionList made = at_end(use + "local b = Account.new(", world, 9);
    expect_signature(made, "(owner: string)", "a required constructor");

    expect_signature(at_end("local function move(part: GameObject, by: number)\nend\nmove("),
                     "(part: GameObject, by: number)", "a local function's signature");
    // A host function has its docs' parameters.
    expect_signature(at_end("task.wait("), "(seconds: number?)", "a host function's signature");
    // Outside a call there is none, and neither while a function's own
    // parameters are being named.
    if (!at_end("local x = 1\nx").signature.empty()) {
        fail("no signature outside a call");
    }
    for (const char* defining : {"local function foo(a, ", "function foo(", "function Account.make(first, ",
                                 "function Account:Do("}) {
        // A function's own parameters being named are no call.
        if (!at_end(defining).signature.empty()) {
            fail(std::string("naming parameters shows no signature: ") + defining);
        }
    }
}

bool answered(const std::shared_ptr<const engine_core::LuauAnswer>& answer) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!answer->ready.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// The list a pending request settles to, as the editor's frames take it.
// Nothing after 20 seconds.
std::optional<ide::CompletionList> settled(const ide::PendingCompletion& pending) {
    if (!pending.answer || !answered(pending.answer)) {
        return std::nullopt;
    }
    return ide::take_completion(pending);
}

// Analysis turned off runs no type checks for completion either, and shutdown
// answers what was still queued.
void testTypedSwitchedOff() {
    engine_core::Game game;
    auto analysis = std::make_unique<engine_core::ScriptAnalysis>(game);
    analysis->set_enabled(false);
    const engine_core::LuauFacts off = analysis->luau_facts({}, 0, "local t = {}\nt.", 15, {}, std::chrono::seconds(5));
    if (off.ran || analysis->luau_facts_later({}, 0, "local t = {}\nt.", 15, {}, "completion")) {
        fail("analysis turned off answers no completion");
    }
    ide::CompletionList now;
    if (ide::ask_completion(now, *analysis, "local t = {}\nt.", 15, {}, 0, true, false, "editor")) {
        fail("analysis turned off leaves nothing pending");
    }
    analysis->set_enabled(true);
    if (!analysis->luau_facts({}, 0, "local t = { x = 1 }\nt.", 22, {}, std::chrono::seconds(20)).ran) {
        fail("analysis turned back on answers again");
    }
    std::string big;
    for (int index = 0; index < 300; ++index) {
        big += "local function f" + std::to_string(index) + "(x: number)\n    return x * 2\nend\n";
    }
    const auto running = analysis->luau_facts_later({}, 0, big, big.size(), {}, "busy");
    const auto waiting = analysis->luau_facts_later({}, 0, "local t = {}\nt.", 15, {}, "completion");
    analysis.reset();
    if (!running || !waiting || !running->ready.load() || !waiting->ready.load() || waiting->facts.ran) {
        fail("shutdown answers every request still on its way");
    }
}

void testTypedLater() {
    const char* account =
        "local Account = {}\n"
        "Account.__index = Account\n"
        "function Account.new(owner: string)\n"
        "    return setmetatable({ owner = owner }, Account)\n"
        "end\n"
        "function Account:Deposit(amount: number)\n"
        "end\n"
        "return Account\n";
    std::vector<engine_core::LuaNode> world;
    world.push_back(node(0, 0xffffffffu, "game", "Game"));
    world.push_back(node(9, 0, "Main", "Script"));
    world.push_back(node(8, 0, "Account", "ModuleScript", account));
    const std::string use = "local Account = require(game.Account)\nlocal a = Account.new(\"me\")\n";

    // Nothing shows while Luau answers; its members arrive on a later frame.
    const std::string members = use + "a:";
    const int members_end = static_cast<int>(members.size());
    ide::CompletionList now;
    const std::optional<ide::PendingCompletion> pending =
        ide::ask_completion(now, typed_analysis(), members, members_end, world, 9, true, false, "editor");
    if (!pending) {
        fail("a member list asks Luau");
    } else {
        if (!now.items.empty() || now.site != ide::CompleteSite::Member) {
            fail("a member list shows nothing before Luau answers");
        }
        if (pending->caret != members_end || pending->source != members) {
            fail("the request says what it was asked about");
        }
        const std::optional<ide::CompletionList> list = settled(*pending);
        if (!list) {
            fail("Luau answers a member list");
        } else {
            expect_has(*list, "Deposit", "Luau's members arrive later");
        }
    }

    // A signature arrives the same way.
    const std::string call = use + "a:Deposit(";
    const std::optional<ide::PendingCompletion> signature =
        ide::ask_completion(now, typed_analysis(), call, static_cast<int>(call.size()), world, 9, true, false, "editor");
    const std::optional<ide::CompletionList> signed_list = signature ? settled(*signature) : std::nullopt;
    if (!signed_list) {
        fail("a call's signature arrives later");
    } else {
        expect_signature(*signed_list, "(amount: number)", "Luau's signature arrives later");
    }

    // Meanwhile the popup narrows to what the text alone says: the site, the
    // prefix, and the range the finished list will replace.
    const std::string partial = use + "a:De";
    const ide::CompletionPlan member_plan = ide::plan_completion(partial, static_cast<int>(partial.size()), world, 9, true);
    if (member_plan.frame.site != ide::CompleteSite::Member || member_plan.frame.prefix != "De" ||
        member_plan.frame.replace_begin != static_cast<int>(partial.size()) - 2 ||
        member_plan.frame.replace_end != static_cast<int>(partial.size())) {
        fail("a member list's frame is its prefix and range");
    }
    const std::string argument = "game:FindFirstChild(\"Ho";
    const ide::CompletionPlan string_plan =
        ide::plan_completion(argument, static_cast<int>(argument.size()), world, 9, true);
    if (string_plan.frame.site != ide::CompleteSite::Argument || string_plan.frame.prefix != "Ho" ||
        string_plan.frame.close_quote != '"') {
        fail("a string argument's frame is its prefix and quote");
    }

    // With no answer from Luau, names still list what needs no types.
    const ide::CompletionPlan keyword_plan = ide::plan_completion("wh", 2, {}, 0, true);
    expect_has(ide::finish_completion(keyword_plan, engine_core::LuauFacts{}), "while", "a keyword without Luau");
    expect_has(ide::finish_completion(ide::plan_completion("ga", 2, {}, 0, true), engine_core::LuauFacts{}), "game",
               "game without Luau");

    // A list that needs no types is ready at once.
    ide::CompletionList directive;
    if (ide::ask_completion(directive, typed_analysis(), "--!", 3, {}, 0, true, false, "editor") || directive.items.empty()) {
        fail("a directive list needs no type check");
    }

    // A hover arrives the same way.
    const std::string hovered = use + "a:Deposit(1)\n";
    ide::HoverInfo hover_now;
    const std::optional<ide::PendingHover> hover =
        ide::ask_hover(hover_now, typed_analysis(), hovered, find_nth(hovered, "Deposit", 0), world, 9);
    if (!hover || hover_now.found || !answered(hover->answer)) {
        fail("a hover arrives later");
    } else {
        const std::optional<ide::HoverInfo> info = ide::take_hover(*hover);
        if (!info || info->title != "function Account:Deposit(self: table, amount: number)") {
            fail("Luau's hover arrives later: " + (info ? info->title : std::string()));
        }
    }

    // Typing faster than the worker answers: a newer request in a lane
    // replaces one still waiting there, which then answers with nothing.
    std::string big;
    for (int index = 0; index < 300; ++index) {
        big += "local function f" + std::to_string(index) + "(x: number)\n    return x * 2\nend\n";
    }
    big += "local t = {}\nt.";
    const auto busy = typed_analysis().luau_facts_later({}, 0, big, big.size(), {}, "busy");
    const auto older = typed_analysis().luau_facts_later(world, 9, members, members.size(), {}, "completion");
    const auto newer = typed_analysis().luau_facts_later(world, 9, members, members.size(), {}, "completion");
    if (!busy || !older || !newer || !answered(busy) || !answered(older) || !answered(newer)) {
        fail("every queued request is answered");
    } else {
        if (older->facts.ran) {
            fail("a replaced request answers with nothing");
        }
        if (!newer->facts.ran) {
            fail("the newer request is answered: " + newer->facts.error);
        }
    }
}

// Types a module exports, used from another script: `Module.Type` in an
// annotation, values of those types, literal types, and hovers on type names.
void testExportedTypes() {
    const char* vector =
        "--!strict\n"
        "local Vector2D = {}\n"
        "Vector2D.__index = Vector2D\n"
        "export type Vector2D = typeof(setmetatable({} :: { x: number, y: number }, Vector2D))\n"
        "function Vector2D.new(x: number?, y: number?): Vector2D\n"
        "    return setmetatable({ x = x or 0, y = y or 0 }, Vector2D)\n"
        "end\n"
        "function Vector2D.magnitude(self: Vector2D): number\n"
        "    return math.sqrt(self.x * self.x + self.y * self.y)\n"
        "end\n"
        "return Vector2D\n";
    const char* animal =
        "--!strict\n"
        "local Vector2D = require(script.Parent.Vector2D)\n"
        "local Animal = {}\n"
        "Animal.__index = Animal\n"
        "export type Diet = \"herbivore\" | \"carnivore\" | \"omnivore\"\n"
        "export type AnimalData = {\n"
        "    name: string,\n"
        "    diet: Diet,\n"
        "    position: Vector2D.Vector2D,\n"
        "}\n"
        "export type Animal = typeof(setmetatable({} :: AnimalData, Animal))\n"
        "function Animal.new(name: string, diet: Diet): Animal\n"
        "    local self = setmetatable({} :: AnimalData, Animal)\n"
        "    self.name = name\n"
        "    self.diet = diet\n"
        "    self.position = Vector2D.new()\n"
        "    return self\n"
        "end\n"
        "function Animal.speak(self: Animal): string\n"
        "    return self.name\n"
        "end\n"
        "function Animal.moveTo(self: Animal, target: Vector2D.Vector2D): number\n"
        "    return (target :: any).x\n"
        "end\n"
        "return Animal\n";
    const char* dog =
        "--!strict\n"
        "local Animal = require(script.Parent.Animal)\n"
        "local Dog = setmetatable({}, { __index = Animal })\n"
        "Dog.__index = Dog\n"
        "export type Trick = \"sit\" | \"roll\" | \"fetch\"\n"
        "export type Dog = Animal.Animal & {\n"
        "    breed: string,\n"
        "    learn: (self: Dog, trick: Trick) -> (),\n"
        "}\n"
        "function Dog.new(name: string, breed: string): Dog\n"
        "    local self = Animal.new(name, \"omnivore\") :: any\n"
        "    self.breed = breed\n"
        "    return setmetatable(self, Dog) :: any\n"
        "end\n"
        "function Dog.learn(self: Dog, trick: Trick)\n"
        "end\n"
        "return Dog\n";
    std::vector<engine_core::LuaNode> world;
    world.push_back(node(0, 0xffffffffu, "game", "Game"));
    world.push_back(node(1, 0, "Demo", "Folder"));
    world.push_back(node(2, 1, "Vector2D", "ModuleScript", vector));
    world.push_back(node(3, 1, "Animal", "ModuleScript", animal));
    world.push_back(node(4, 1, "Dog", "ModuleScript", dog));
    world.push_back(node(5, 1, "Main", "Script"));
    const std::string main =
        "local Vector2D = require(script.Parent.Vector2D)\n"
        "local Animal = require(script.Parent.Animal)\n"
        "local Dog = require(script.Parent.Dog)\n"
        "local rex = Dog.new(\"Rex\", \"Beagle\")\n";

    // A module's exported types after `Module.` in an annotation.
    const ide::CompletionList vector_types = at_end(main + "local p: Vector2D.", world, 5);
    expect_detail(vector_types, "Vector2D", "type", "an exported type");
    const ide::CompletionList dog_types = at_end(main + "local d: Dog.", world, 5);
    expect_has(dog_types, "Dog", "an exported type");
    expect_has(dog_types, "Trick", "an exported literal type");
    expect_missing(dog_types, "new", "a value is no type");
    // A type name offers the modules that export types, and local aliases.
    expect_detail(at_end(main + "local p: Vec", world, 5), "Vector2D", "module", "a module in a type");
    const std::string field = animal;
    const std::size_t diet_at = field.find("diet: Diet,") + std::string("diet: Di").size();
    const ide::CompletionList local_type =
        at_caret(field, ide::CodePointsBefore(field, diet_at), world, 3);
    expect_has(local_type, "Diet", "a local type in a table type's field");

    // A value of an exported type: every function that takes it first after ':'.
    const ide::CompletionList methods = at_end(main + "rex:", world, 5);
    expect_has(methods, "speak", "an inherited method written with a dot and self");
    expect_has(methods, "moveTo", "an inherited method");
    expect_has(methods, "learn", "the type's own method");
    expect_missing(methods, "breed", "a field after ':'");
    expect_missing(methods, "new", "a constructor takes no receiver");
    expect_has(at_end(main + "rex.", world, 5), "breed", "a field of an exported type");

    // A literal type offers its strings.
    const ide::CompletionList tricks = at_end(main + "rex:learn(\"", world, 5);
    expect_has(tricks, "sit", "a literal type's string");
    expect_has(tricks, "fetch", "a literal type's string");
    if (tricks.site != ide::CompleteSite::Argument) {
        fail("a literal type's strings are an argument completion");
    }
    expect_has(at_end(main + "Animal.new(\"x\", \"om", world, 5), "omnivore", "a literal type typed so far");
    // Signatures read as the definition wrote them.
    expect_signature(at_end(main + "Animal.new(", world, 5), "(name: string, diet: Diet)", "a written parameter type");
    expect_signature(at_end(main + "rex:moveTo(", world, 5), "(target: Vector2D.Vector2D)",
                     "a ':' call drops the receiver written as self");

    // Hovering a type name shows its declaration.
    expect_hover(ide::hover_luau(animal, find_nth(animal, "Diet", 1), world, 3),
                 "type Diet = \"herbivore\" | \"carnivore\" | \"omnivore\"", "type", nullptr, "a local type");
    const std::string typed = main + "local d: Dog.Dog = rex\n";
    const ide::HoverInfo exported = ide::hover_luau(typed, find_nth(typed, "Dog", 4), world, 5);
    if (!exported.found || exported.title.rfind("type Dog = Animal.Animal & {", 0) != 0) {
        fail("an exported type's hover is its declaration: " + exported.title);
    }
    const ide::HoverInfo alias = ide::hover_luau(typed, find_nth(typed, "Dog", 3), world, 5);
    expect_hover(alias, "Dog", "module", nullptr, "the module before an exported type");
    expect_hover(ide::hover_luau(animal, find_nth(animal, "Vector2D", 3), world, 3),
                 "type Vector2D = typeof(setmetatable({} :: { x: number, y: number }, Vector2D))", "type", nullptr,
                 "an imported type");
}

// `first` is listed, and ahead of `second` when that is listed.
void expect_before(const ide::CompletionList& list, const char* first, const char* second, const char* label) {
    const int a = index_of(list, first);
    const int b = index_of(list, second);
    if (a < 0 || (b >= 0 && a > b)) {
        fail(std::string(label) + ": " + first + " at " + std::to_string(a) + ", " + second + " at " + std::to_string(b));
    }
}

// A value of a written type: the strings a literal type allows, a table type's
// keys, names of the expected type first, and a function for a function-typed
// argument.
void testTypedValues() {
    const std::string types =
        "--!strict\n"
        "export type Diet = \"herbivore\" | \"carnivore\" | \"omnivore\"\n"
        "export type AnimalData = {\n"
        "    name: string,\n"
        "    diet: Diet,\n"
        "}\n"
        "local favorite: Diet = \"omnivore\"\n"
        "local count = 1\n";

    // The type row shows its declaration.
    const ide::CompletionList named = at_end(types + "local test: Die");
    expect_info(named, "Diet", nullptr, "type Diet = \"herbivore\" | \"carnivore\" | \"omnivore\"", nullptr,
                "a type's declaration");

    // Inside the quotes: the strings, as an argument completion.
    const ide::CompletionList quoted = at_end(types + "local test: Diet = \"");
    expect_has(quoted, "herbivore", "a literal type's string in a local");
    expect_has(quoted, "omnivore", "a literal type's string in a local");
    if (quoted.site != ide::CompleteSite::Argument || quoted.close_quote != '"') {
        fail("a local's literal strings complete inside the quotes");
    }
    const ide::CompletionList narrowed = at_end(types + "local test: Diet = \"c");
    expect_has(narrowed, "carnivore", "a literal typed so far");
    expect_missing(narrowed, "herbivore", "a literal typed so far");
    expect_has(at_end(types + "favorite = \""), "carnivore", "a string assigned to a typed local");
    expect_has(at_end(types + "if favorite == \""), "carnivore", "a string compared to a typed local");
    expect_has(at_end(types + "local a: AnimalData = {\n    diet = \""), "carnivore", "a string in a typed table");
    expect_has(at_end(types + "local a: AnimalData = {} :: any\na.diet = \""), "carnivore",
               "a string assigned to a typed field");
    expect_missing(at_end(types + "local test: string = \""), "carnivore", "a plain string has no choices");

    // Before the quotes: the quoted strings first, and the list opens by itself.
    const ide::CompletionList value = at_end(types + "local test: Diet = ");
    if (value.items.empty() || value.items[0].name.front() != '"' || !value.items[0].expected || !value.open_expected) {
        fail("a local's literal strings lead and open the list: " +
             (value.items.empty() ? std::string("none") : value.items[0].name));
    }
    expect_has(value, "\"herbivore\"", "a quoted literal string");
    const ide::CompletionItem* herbivore = find_item(value, "\"herbivore\"");
    if (herbivore != nullptr && !herbivore->snippet) {
        fail("'(' or '.' does not accept a quoted string");
    }
    expect_detail(value, "favorite", "Diet", "a local shows the type its declaration wrote");
    expect_before(value, "favorite", "count", "a local of the expected type");
    expect_before(at_end(types + "favorite = "), "\"carnivore\"", "count", "a string assigned to a typed local");
    expect_missing(at_end(types + "favorite = "), "local", "a value is no statement");
    if (at_end(types + "local test: number = ").open_expected) {
        fail("a type with no strings does not open the list by itself");
    }
    const ide::CompletionList flag = at_end(types + "local ok = true\nlocal flag: boolean = ");
    expect_before(flag, "true", "count", "a boolean's keywords");
    expect_before(flag, "ok", "count", "a local of the expected type");

    // A table type's keys, written as `key = `.
    const ide::CompletionList keys = at_end(types + "local a: AnimalData = { ");
    expect_before(keys, "diet", "favorite", "a table type's key");
    expect_before(keys, "name", "favorite", "a table type's key");
    const ide::CompletionItem* diet = find_item(keys, "diet");
    if (diet == nullptr || diet->insert != "diet = " || !diet->expected) {
        fail("a key writes `diet = `: " + (diet == nullptr ? std::string("missing") : diet->insert));
    }
    if (!keys.open_expected) {
        fail("a table type's keys open the list");
    }
    const ide::CompletionList line = at_end(types + "local a: AnimalData = {\n    ");
    expect_has(line, "diet", "a table type's key on its own line");
    if (line.open_expected) {
        fail("the list does not open by itself at the start of a line");
    }
    expect_before(at_end(types + "local a: AnimalData = {\n    n"), "name", "next", "a key typed so far");
    const std::string before_equals = types + "local a: AnimalData = { na = 1 }";
    const ide::CompletionList renamed =
        at_caret(before_equals, static_cast<int>(before_equals.find("na = 1")) + 2);
    const ide::CompletionItem* name = find_item(renamed, "name");
    if (name == nullptr || !name->insert.empty()) {
        fail("a key already followed by '=' writes only its name");
    }
    expect_before(at_end(types + "local a: AnimalData = {\n    diet = "), "\"carnivore\"", "count",
                  "a typed field's strings");

    // A function-typed argument offers a function with its parameters.
    const ide::CompletionList each = at_end(
        types + "local function each(list: {number}, visit: (value: number, index: number) -> ()) end\neach({1}, fu");
    expect_before(each, "function(value: number, index: number)", "function", "a function-typed argument");
    const ide::CompletionItem* generated = find_item(each, "function(value: number, index: number)");
    if (generated != nullptr && !generated->snippet) {
        fail("a generated function is a snippet");
    }
}

// Keywords a position can hold: `then` after an if's condition, the words a
// statement starts with, a block's closers, and those a value can hold.
void testKeywords() {
    const std::string code = "local count = 1\n";
    const ide::CompletionList then = at_end(code + "if count > 1 t");
    expect_has(then, "then", "an if's condition ends with then");
    expect_missing(then, "tostring", "only a keyword follows a finished condition");
    expect_missing(then, "true", "only a keyword follows a finished condition");

    const ide::CompletionList statement = at_end(code + "f");
    expect_has(statement, "for", "a statement keyword");
    expect_has(statement, "function", "a statement keyword");
    expect_missing(statement, "false", "a value keyword at a statement's start");
    const ide::CompletionList value = at_end(code + "local x = f");
    expect_has(value, "false", "a value keyword");
    expect_has(value, "function", "a value keyword");
    expect_missing(value, "for", "a statement keyword in a value");
    expect_before(at_end(code + "re"), "return", "require", "keywords come before library globals");

    const ide::CompletionList closers = at_end(code + "if count > 1 then\n    print(count)\n    e");
    expect_has(closers, "end", "an if's end");
    expect_has(closers, "else", "an if's else");
    expect_has(closers, "elseif", "an if's elseif");
    expect_missing(at_end(code + "e"), "end", "nothing to end at the top");
    const ide::CompletionList loop = at_end(code + "repeat\n    print(count)\n    u");
    expect_has(loop, "until", "a repeat's until");
    expect_has(at_end(code + "while count > 1 do\n    en"), "end", "a loop's end");
    const ide::CompletionList local = at_end(code + "local fu");
    expect_has(local, "function", "local function");
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
        testRegisteredMembers();
        testScriptTableKeys();
        testRequire();
        testDeepNesting();
        testVectorArithmetic();
        testTypedFallback();
        testTypedHoverAndSignature();
        testTypedLater();
        testTypedSwitchedOff();
        testExportedTypes();
        testTypedValues();
        testKeywords();
    } catch (const std::exception& ex) {
        fail(std::string("exception ") + ex.what());
    }
    return gFailures;
}
