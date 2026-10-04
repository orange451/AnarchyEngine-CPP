#include "bridge/StudioBridge.hpp"
#include "ChangeHistoryService.hpp"
#include "ide/IdeResources.hpp"
#include "AssetInstances.hpp"
#include "Folder.hpp"
#include "ide/McpServer.hpp"
#include "ide/McpTools.hpp"
#include "ide/StudioRegistry.hpp"
#include "SelectionService.hpp"

#include "Engine.hpp"
#include "LuaSource.hpp"
#include "ModuleScript.hpp"
#include "ScriptAnalysis.hpp"
#include "ScriptRuntime.hpp"
#include "Script.hpp"
#include "httplib.h"
#include "profiler/ProfileJson.hpp"
#include "profiler/Profiler.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using engine_core::JsonValue;

int gFailures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message.c_str());
        ++gFailures;
    }
}

// What a missing member or item reads as.
const JsonValue kMissing{};

// The start of value's JSON, for a failure message.
std::string Excerpt(const JsonValue& value) {
    const std::string text = ide::compact_json(value);
    return text.size() <= 200 ? text : text.substr(0, 200) + "...";
}

// The member key of object. A missing one fails the test, naming it, and reads
// as null, so the tests after it still run.
const JsonValue& Member(const JsonValue& object, std::string_view key) {
    if (const JsonValue* value = object.find(key)) {
        return *value;
    }
    Expect(false, "JSON has member \"" + std::string(key) + "\": " + Excerpt(object));
    return kMissing;
}

// The item at index of a list. A missing one fails the test and reads as null.
const JsonValue& Item(const JsonValue& list, std::size_t index) {
    if (index < list.items().size()) {
        return list.items()[index];
    }
    Expect(false, "JSON has item " + std::to_string(index) + ": " + Excerpt(list));
    return kMissing;
}

JsonValue Parse(const std::string& text) {
    JsonValue value;
    std::string error;
    if (!engine_core::parse_json(text, value, error)) {
        Expect(false, "reply parses: " + error + " in " + text);
    }
    return value;
}

// Sends one request through handle, as the listener would, and parses the reply.
JsonValue Request(const ide::McpServer& server, const std::string& method, const std::string& params = "{}",
                  int* status_out = nullptr) {
    int status = 0;
    const std::string body = server.handle(
        R"({"jsonrpc":"2.0","id":1,"method":")" + method + R"(","params":)" + params + "}", status);
    if (status_out != nullptr) {
        *status_out = status;
    }
    return Parse(body);
}

// Calls a tool and returns its structured result. failed says whether the call reported an error.
JsonValue Call(const ide::McpServer& server, const std::string& tool, const std::string& arguments,
               bool* failed = nullptr) {
    const JsonValue reply =
        Request(server, "tools/call", R"({"name":")" + tool + R"(","arguments":)" + arguments + "}");
    const JsonValue* result = reply.find("result");
    Expect(result != nullptr, tool + " returns a result: " + ide::compact_json(reply));
    if (result == nullptr) {
        return {};
    }
    const bool error = result->find("isError") != nullptr && Member(*result, "isError").as_bool();
    if (failed != nullptr) {
        *failed = error;
    } else if (error) {
        const JsonValue& content = Item(Member(*result, "content"), 0);
        Expect(false, tool + " succeeds: " + Member(content, "text").as_string());
    }
    const JsonValue* structured = result->find("structuredContent");
    return structured != nullptr ? *structured : JsonValue();
}

std::string ErrorText(const ide::McpServer& server, const std::string& tool, const std::string& arguments) {
    const JsonValue reply =
        Request(server, "tools/call", R"({"name":")" + tool + R"(","arguments":)" + arguments + "}");
    const JsonValue* result = reply.find("result");
    if (result == nullptr || result->find("isError") == nullptr || !Member(*result, "isError").as_bool()) {
        return {};
    }
    return Member(Item(Member(*result, "content"), 0), "text").as_string();
}

void TestProtocol() {
    ide::McpServer server;
    server.add_tool({"echo", "Returns its arguments.", ide::json_literal(R"({"type":"object"})"),
                     [](const JsonValue& arguments) { return arguments; }});
    server.add_tool({"boom", "Always fails.", ide::json_literal(R"({"type":"object"})"),
                     [](const JsonValue&) -> JsonValue { throw std::runtime_error("it broke"); }});

    Expect(server.activity().client.empty() && server.activity().calls == 0 &&
               server.activity().last_request == std::chrono::steady_clock::time_point{},
           "a new server has seen no client");

    const JsonValue init = Request(server, "initialize", R"({"protocolVersion":"2025-06-18","capabilities":{}})");
    const JsonValue* result = init.find("result");
    Expect(result != nullptr && Member(*result, "protocolVersion").as_string() == "2025-06-18",
           "initialize answers with the client's version when it knows it");
    Expect(result != nullptr && Member(*result, "capabilities").find("tools") != nullptr, "initialize offers tools");
    const JsonValue newer = Request(server, "initialize", R"({"protocolVersion":"2099-01-01"})");
    Expect(Member(Member(newer, "result"), "protocolVersion").as_string() == "2025-11-25",
           "an unknown version gets the newest this server speaks");

    const JsonValue list = Request(server, "tools/list");
    const JsonValue* tools = Member(list, "result").find("tools");
    Expect(tools != nullptr && tools->items().size() == 2 && Member(Item(*tools, 0), "name").as_string() == "echo" &&
               Member(Item(*tools, 0), "inputSchema").is_object(),
           "tools/list names each tool with its schema");

    const JsonValue echoed = Call(server, "echo", R"({"a":1,"b":"two"})");
    Expect(echoed.find("a") != nullptr && Member(echoed, "a").as_number() == 1, "a tool's result is its structured content");

    Expect(ErrorText(server, "boom", "{}") == "it broke", "a throwing tool reports its message as a failed call");

    // What the studio's status bar shows: the client, and its latest tool call.
    Expect(server.activity().last_request != std::chrono::steady_clock::time_point{},
           "a request is noted");
    Expect(server.activity().client.empty(), "an initialize without clientInfo names no client");
    Request(server, "initialize", R"({"protocolVersion":"2025-06-18","clientInfo":{"name":"test-client","version":"1"}})");
    Expect(server.activity().client == "test-client", "initialize's clientInfo names the client");
    Call(server, "echo", "{}");
    Expect(server.activity().calls == 3 && server.activity().last_tool == "echo",
           "each tool call is counted, failed ones too, and the latest is named");
    Request(server, "tools/call", R"({"name":"nope"})");
    Expect(server.activity().calls == 3, "a call to no tool is not counted");

    const JsonValue unknown = Request(server, "tools/call", R"({"name":"nope"})");
    Expect(unknown.find("error") != nullptr && Member(Member(unknown, "error"), "code").as_number() == -32602,
           "an unknown tool is an invalid-params error");
    const JsonValue missing = Request(server, "no/such");
    Expect(missing.find("error") != nullptr && Member(Member(missing, "error"), "code").as_number() == -32601,
           "an unknown method is method-not-found");
    Expect(Request(server, "ping").find("result") != nullptr, "ping answers");

    int status = 0;
    Expect(server.handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})", status).empty() && status == 202,
           "a notification gets 202 and no body");
    const JsonValue bad = Parse(server.handle("{nope", status));
    Expect(bad.find("error") != nullptr && Member(Member(bad, "error"), "code").as_number() == -32700,
           "bad JSON is a parse error");
    const JsonValue batch = Parse(server.handle(
        R"([{"jsonrpc":"2.0","id":1,"method":"ping"},{"jsonrpc":"2.0","method":"notifications/initialized"},)"
        R"({"jsonrpc":"2.0","id":2,"method":"ping"}])",
        status));
    Expect(batch.is_array() && batch.items().size() == 2, "a batch answers each request and skips notifications");

    Expect(ide::compact_json(ide::json_literal(R"({"b":[1,2.5,"x\ny"],"a":null})")) ==
               R"({"a":null,"b":[1,2.5,"x\ny"]})",
           "compact JSON is one line");
}

void AddScript(engine_core::DataModel& game, const char* name, const char* source, engine_core::InstanceId parent) {
    engine_core::Script& script = game.create<engine_core::Script>();
    game.set_name(script.id(), name);
    script.set_source(source);
    game.set_parent(script.id(), parent);
}

void TestEngineTools() {
    engine_core::Engine engine;
    engine_core::DataModel& game = engine.datamodel();
    ide::McpServer server;
    ide::add_engine_tools(server, engine, {});

    const JsonValue folder = Call(server, "create_instance", R"({"class":"Folder","name":"Stuff"})");
    Expect(folder.find("path") != nullptr && Member(folder, "path").as_string() == "Workspace.Stuff",
           "create_instance puts it in Workspace");
    const JsonValue inner = Call(server, "create_instance", R"({"class":"Folder","name":"Inner","parent":"Workspace.Stuff"})");
    Expect(Member(inner, "path").as_string() == "Workspace.Stuff.Inner", "create_instance takes a parent path");
    Expect(ErrorText(server, "create_instance", R"({"class":"Banana"})").find("Instance.new cannot make") == 0,
           "an unknown class is refused");
    // game holds the scene services alone, and they stay where they are.
    Expect(ErrorText(server, "create_instance", R"({"class":"Folder","parent":"game"})") ==
               "Only scene services can be children of game; put Folder in Workspace",
           "create_instance refuses game as the parent");
    Expect(ErrorText(server, "delete_instance", R"({"instance":"Lighting"})") == "Lighting cannot be destroyed",
           "delete_instance refuses a scene service");
    Expect(ErrorText(server, "set_property", R"({"instance":"Storage","property":"Parent","value":"Workspace"})")
                   .find("read-only") != std::string::npos,
           "a scene service's Parent is read-only");

    const JsonValue gameTree = Call(server, "get_tree", R"({"instance":"game","depth":2})");
    bool sawAssets = false;
    std::vector<std::string> categories;
    for (const JsonValue& child : Member(Member(gameTree, "tree"), "children").items()) {
        if (Member(child, "name").as_string() == "Assets") {
            sawAssets = true;
            for (const JsonValue& category : Member(child, "children").items()) {
                categories.push_back(Member(category, "name").as_string());
            }
        }
    }
    Expect(sawAssets && categories == std::vector<std::string>{"Materials", "Prefabs", "Meshes", "Textures", "Audio"},
           "get_tree lists Assets and its five categories");

    const JsonValue brick = Call(server, "create_instance",
                                 R"({"class":"Texture","name":"Brick","parent":"Assets.Textures"})");
    Expect(Member(brick, "path").as_string() == "Assets.Textures.Brick", "create_instance puts a Texture where it belongs");
    Expect(ErrorText(server, "set_property", R"({"instance":"Assets.Textures.Brick","property":"Parent","value":"Workspace"})") ==
               "A Texture must be in Assets.Textures",
           "set_property refuses to move a Texture out of Assets.Textures");
    const std::size_t room = game.room_left();
    Expect(ErrorText(server, "create_instance", R"({"class":"Texture"})").find("A Texture must be in Assets.Textures") !=
               std::string::npos,
           "create_instance refuses a Texture in its default parent, Workspace");
    Expect(ErrorText(server, "create_instance", R"({"class":"Folder","parent":"Assets"})")
                   .find("Assets holds only Materials, Prefabs, Meshes, Textures, and Audio") != std::string::npos,
           "create_instance refuses a Folder in Assets");
    Expect(game.room_left() == room, "a refused create_instance makes nothing");

    AddScript(game, "Hello", "print('hi')", game.find_first_child(game.scene_service("Workspace"), "Stuff"));

    const JsonValue tree = Call(server, "get_tree", R"({"instance":"Workspace","depth":1})");
    const JsonValue* top = tree.find("tree");
    Expect(top != nullptr && top->find("children") != nullptr, "get_tree lists the root's children");
    bool saw = false;
    for (const JsonValue& child : Member(Member(tree, "tree"), "children").items()) {
        if (Member(child, "name").as_string() == "Stuff") {
            saw = child.find("children") == nullptr && Member(child, "child_count").as_number() == 2;
        }
    }
    Expect(saw, "a row below depth reports its child count");
    const JsonValue deep = Call(server, "get_tree", R"({"instance":"game.Workspace.Stuff","depth":3})");
    Expect(Member(Member(deep, "tree"), "children").items().size() == 2, "get_tree starts at a path");

    const JsonValue found = Call(server, "find_instances", R"({"name":"inn"})");
    Expect(Member(found, "instances").items().size() == 1 &&
               Member(Item(Member(found, "instances"), 0), "path").as_string() == "Workspace.Stuff.Inner",
           "find_instances matches part of a name, ignoring case");

    const JsonValue props = Call(server, "get_properties", R"({"instance":"Workspace.Stuff.Inner"})");
    const JsonValue* name = props.find("properties") != nullptr ? Member(props, "properties").find("Name") : nullptr;
    Expect(name != nullptr && Member(*name, "value").as_string() == "Inner", "get_properties shows Name");
    const JsonValue* parent = props.find("properties") != nullptr ? Member(props, "properties").find("Parent") : nullptr;
    Expect(parent != nullptr && Member(Member(*parent, "value"), "path").as_string() == "Workspace.Stuff",
           "an Instance property shows its target's path");

    Call(server, "set_property", R"({"instance":"Workspace.Stuff.Inner","property":"Name","value":"Renamed"})");
    Expect(game.find_first_child(game.find_first_child(game.scene_service("Workspace"), "Stuff"), "Renamed") != 0,
           "set_property renames through the property");
    Call(server, "set_property", R"({"instance":"Workspace.Stuff.Renamed","property":"Parent","value":"Workspace"})");
    Expect(game.find_first_child(game.scene_service("Workspace"), "Renamed") != 0, "set_property reparents by path");
    Expect(game.history().can_undo().first, "an edit is on the undo stack");
    Expect(!ErrorText(server, "set_property", R"({"instance":"Workspace.Renamed","property":"Name","value":5})").empty(),
           "a value of the wrong type is refused");
    Expect(ErrorText(server, "get_properties", R"({"instance":"Nope.Missing"})").find("No instance at") == 0,
           "a missing path says where it stopped");
    Expect(ErrorText(server, "get_properties", R"({"instance":-1})") == "No instance has id -1.",
           "a negative id is no instance");
    Expect(ErrorText(server, "get_properties", R"({"instance":1e300})").find("No instance has id") == 0,
           "an id past any instance is no instance");
    Expect(Call(server, "get_tree", R"({"depth":1e300})").find("tree") != nullptr, "a huge depth is the deepest allowed");

    // A Color3 reads as [r, g, b] and takes that or a hex code.
    Call(server, "set_property", R"({"instance":"Lighting","property":"Ambient","value":[1,0.5,0]})");
    auto color_of = [&server]() {
        const JsonValue lighting = Call(server, "get_properties", R"({"instance":"Lighting"})");
        const JsonValue* entry =
            lighting.find("properties") != nullptr ? Member(lighting, "properties").find("Ambient") : nullptr;
        return entry != nullptr ? *entry : JsonValue();
    };
    JsonValue color = color_of();
    Expect(color.find("type") != nullptr && Member(color, "type").as_string() == "Color3" &&
               color.find("readonly") == nullptr,
           "Ambient is a writable Color3");
    Expect(color.find("value") != nullptr && Member(color, "value").items().size() == 3 &&
               Item(Member(color, "value"), 1).as_number() == 0.5,
           "set_property writes [r, g, b] and get_properties reads it back");
    Call(server, "set_property", R"({"instance":"Lighting","property":"Ambient","value":"#0000FF"})");
    color = color_of();
    Expect(Item(Member(color, "value"), 0).as_number() == 0 && Item(Member(color, "value"), 2).as_number() == 1,
           "a hex code sets a Color3");
    Expect(!ErrorText(server, "set_property", R"({"instance":"Lighting","property":"Ambient","value":"blue"})").empty(),
           "a Color3 refuses what is not a color");

    // A GameObject's Transform is a Matrix4, read and written as its position and orientation.
    Call(server, "create_instance", R"({"class":"GameObject","name":"Box"})");
    Call(server, "set_property", R"({"instance":"Workspace.Box","property":"Transform","value":{"position":[1,2,3]}})");
    Call(server, "set_property",
         R"({"instance":"Workspace.Box","property":"Transform","value":{"orientation":[0,90,0]}})");
    const JsonValue box = Call(server, "get_properties", R"({"instance":"Workspace.Box"})");
    const JsonValue* transform =
        box.find("properties") != nullptr ? Member(box, "properties").find("Transform") : nullptr;
    const JsonValue* value = transform != nullptr ? transform->find("value") : nullptr;
    Expect(transform != nullptr && transform->find("type") != nullptr &&
               Member(*transform, "type").as_string() == "Matrix4" && value != nullptr &&
               value->find("position") != nullptr && Item(Member(*value, "position"), 2).as_number() == 3 &&
               value->find("orientation") != nullptr && Item(Member(*value, "orientation"), 1).as_number() == 90,
           "set_property writes a GameObject's Transform a part at a time and get_properties reads it back");
    Expect(Member(box, "properties").find("Position") == nullptr, "a GameObject has no Position of its own");
    Expect(!ErrorText(server, "set_property",
                      R"({"instance":"Workspace.Box","property":"Transform","value":[1,2,3]})")
                .empty(),
           "a Transform refuses a bare Vector3");

    const JsonValue run = Call(server, "run_lua", R"j({"source":"print('from mcp', 1 + 2)"})j");
    const JsonValue* lines = run.find("output");
    Expect(lines != nullptr && lines->items().size() == 1 &&
               Member(Item(*lines, 0), "text").as_string() == "from mcp\t3",
           "run_lua returns what the chunk printed");
    const JsonValue failed = Call(server, "run_lua", R"j({"source":"error('nope')"})j");
    Expect(Member(failed, "error").as_bool(), "run_lua reports an error");

    const JsonValue output = Call(server, "get_output", "{}");
    bool command = false;
    for (const JsonValue& line : Member(output, "lines").items()) {
        command = command || (Member(line, "kind").as_string() == "command" &&
                              Member(line, "text").as_string() == "print('from mcp', 1 + 2)");
    }
    Expect(command, "the chunk run_lua ran shows in the console");
    const double next = Member(output, "next").as_number();
    Call(server, "run_lua", R"j({"source":"print('later')"})j");
    const JsonValue newer = Call(server, "get_output", "{\"since\":" + engine_core::format_json_number(next) + "}");
    Expect(Member(newer, "lines").items().size() == 2, "get_output since returns only newer lines");
    Expect(Member(Call(server, "get_output", R"({"since":1e300})"), "lines").items().empty(),
           "get_output since a line never written returns nothing");

    const JsonValue source = Call(server, "read_script", R"({"instance":"Workspace.Stuff.Hello"})");
    Expect(Member(source, "source").as_string() == "print('hi')", "read_script returns the Source");
    Call(server, "write_script", R"j({"instance":"Workspace.Stuff.Hello","source":"print('bye')"})j");
    const auto* script = dynamic_cast<const engine_core::LuaSource*>(
        game.instance(game.find_first_child(game.find_first_child(game.scene_service("Workspace"), "Stuff"), "Hello")));
    Expect(script != nullptr && script->source() == "print('bye')", "write_script replaces the Source");
    Expect(!ErrorText(server, "read_script", R"({"instance":"Workspace.Stuff"})").empty(), "read_script refuses a Folder");

    Call(server, "set_selection", R"({"instances":["Workspace.Stuff","Workspace.Renamed"]})");
    Expect(game.selection().get().size() == 2, "set_selection selects by path");
    const JsonValue selection = Call(server, "get_selection", "{}");
    Expect(Member(selection, "instances").items().size() == 2 &&
               Member(Item(Member(selection, "instances"), 1), "name").as_string() == "Renamed",
           "get_selection lists them in order");

    const JsonValue classes = Call(server, "list_classes", "{}");
    bool creatable = false;
    for (const JsonValue& entry : Member(classes, "creatable").items()) {
        creatable = creatable || entry.as_string() == "Folder";
    }
    Expect(creatable, "list_classes names Folder as creatable");
    const JsonValue api = Call(server, "get_class", R"({"class":"Folder"})");
    bool has_name = false;
    for (const JsonValue& entry : Member(api, "properties").items()) {
        has_name = has_name || Member(entry, "name").as_string() == "Name";
    }
    Expect(has_name, "get_class includes inherited properties");

    Call(server, "delete_instance", R"({"instance":"Workspace.Stuff"})");
    Expect(game.find_first_child(game.scene_service("Workspace"), "Stuff") == 0, "delete_instance removes it");
    Expect(!ErrorText(server, "delete_instance", R"({"instance":"game"})").empty(), "the root is not deleted");
}

const engine_core::LuaSource* ScriptNamed(engine_core::DataModel& game, const char* name) {
    return dynamic_cast<const engine_core::LuaSource*>(game.instance(game.find_first_child(game.scene_service("Workspace"), name)));
}

bool HasProblem(const JsonValue& problems, const std::string& code, int line) {
    for (const JsonValue& problem : problems.items()) {
        if (Member(problem, "code").as_string() == code && Member(problem, "line").as_number() == line) {
            return true;
        }
    }
    return false;
}

// Scripts are checked, edited in part, searched, and undone.
void TestScriptTools() {
    engine_core::Engine engine;
    engine_core::DataModel& game = engine.datamodel();
    ide::McpServer server;
    ide::add_engine_tools(server, engine, {});
    AddScript(game, "Main", "print('hi')", game.scene_service("Workspace"));

    const JsonValue broken = Call(server, "write_script", R"({"instance":"Workspace.Main","source":"local x ="})");
    const JsonValue* problems = broken.find("problems");
    Expect(problems != nullptr && problems->items().size() == 1 && HasProblem(*problems, "Syntax", 1) &&
               Member(Item(*problems, 0), "severity").as_string() == "error",
           "write_script returns a syntax error, lines from 1: " + ide::compact_json(broken));
    const JsonValue clean = Call(server, "write_script", R"j({"instance":"Workspace.Main","source":"local n = 1\nprint(n)\n"})j");
    Expect(clean.find("problems") != nullptr && Member(clean, "problems").items().empty(),
           "a clean script has no problems: " + ide::compact_json(clean));

    const JsonValue edited = Call(
        server, "edit_script",
        R"j({"instance":"Workspace.Main","edits":[{"old_text":"print(n)","new_text":"print(n + missing)"}]})j");
    Expect(ScriptNamed(game, "Main")->source() == "local n = 1\nprint(n + missing)\n", "edit_script replaces the text");
    Expect(Member(edited, "replaced").as_number() == 1, "edit_script counts its replacements");
    Expect(edited.find("problems") != nullptr && HasProblem(*edited.find("problems"), "Lint/UnknownGlobal", 2),
           "edit_script returns what analysis finds: " + ide::compact_json(edited));

    const JsonValue two = Call(server, "write_script",
                               R"j({"instance":"Workspace.Main","source":"local n = 1\nprint(n + missing)\nlocal x = n"})j");
    const JsonValue* ordered = two.find("problems");
    Expect(ordered != nullptr && ordered->items().size() == 2 &&
               Member(Item(*ordered, 0), "line").as_number() == 2 && Member(Item(*ordered, 1), "line").as_number() == 3,
           "problems come in line order: " + ide::compact_json(two));

    Call(server, "write_script", R"j({"instance":"Workspace.Main","source":"local a = 1\nlocal b = a\nlocal c = a\n"})j");
    const std::string twice =
        ErrorText(server, "edit_script", R"j({"instance":"Workspace.Main","edits":[{"old_text":"= a","new_text":"= 2"}]})j");
    Expect(twice.find("2 times, on lines 2, 3") != std::string::npos,
           "old_text that matches twice is refused, naming the lines: " + twice);
    Expect(ErrorText(server, "edit_script", R"j({"instance":"Workspace.Main","edits":[{"old_text":"nope","new_text":""}]})j")
                   .find("is not in the Source") != std::string::npos,
           "old_text that does not match is refused");
    ErrorText(server, "edit_script",
              R"j({"instance":"Workspace.Main","edits":[{"old_text":"local b","new_text":"local B"},{"old_text":"nope","new_text":""}]})j");
    Expect(ScriptNamed(game, "Main")->source() == "local a = 1\nlocal b = a\nlocal c = a\n",
           "a failed edit leaves the earlier ones unapplied");
    const JsonValue every = Call(
        server, "edit_script", R"j({"instance":"Workspace.Main","edits":[{"old_text":"= a","new_text":"= 2","replace_all":true}]})j");
    Expect(Member(every, "replaced").as_number() == 2 &&
               ScriptNamed(game, "Main")->source() == "local a = 1\nlocal b = 2\nlocal c = 2\n",
           "replace_all replaces each place");

    Call(server, "write_script", R"j({"instance":"Workspace.Main","source":"one\ntwo\nthree\n"})j");
    const JsonValue whole = Call(server, "read_script", R"({"instance":"Workspace.Main"})");
    Expect(Member(whole, "line_count").as_number() == 4 && Member(whole, "source").as_string() == "one\ntwo\nthree\n",
           "read_script returns the whole Source and its line count");
    const JsonValue middle = Call(server, "read_script", R"({"instance":"Workspace.Main","first_line":2,"last_line":3})");
    Expect(Member(middle, "source").as_string() == "two\nthree" && Member(middle, "first_line").as_number() == 2,
           "read_script returns a range of lines");
    const JsonValue tail = Call(server, "read_script", R"({"instance":"Workspace.Main","first_line":3})");
    Expect(Member(tail, "source").as_string() == "three\n" && Member(tail, "last_line").as_number() == 4,
           "a range without last_line runs to the end");
    Expect(ErrorText(server, "read_script", R"({"instance":"Workspace.Main","first_line":9})") == "The Source has 4 lines.",
           "a range past the end is refused");

    const JsonValue lib = Call(server, "create_instance", R"({"class":"Folder","name":"Lib"})");
    engine_core::ModuleScript& module = game.create<engine_core::ModuleScript>();
    game.set_name(module.id(), "Util");
    module.set_source("local Value = 1\nreturn value\n");
    game.set_parent(module.id(), static_cast<engine_core::InstanceId>(Member(lib, "id").as_number()));

    const JsonValue found = Call(server, "search_scripts", R"({"pattern":"VALUE"})");
    const JsonValue& scripts = Member(found, "scripts");
    Expect(scripts.items().size() == 1 && Member(Item(scripts, 0), "path").as_string() == "Workspace.Lib.Util" &&
               Member(Item(scripts, 0), "lines").items().size() == 2 && Member(found, "matches").as_number() == 2,
           "search_scripts ignores case and lists matching lines: " + ide::compact_json(found));
    const JsonValue cased = Call(server, "search_scripts", R"({"pattern":"value","match_case":true})");
    Expect(Member(cased, "matches").as_number() == 1 &&
               Member(Item(Member(Item(Member(cased, "scripts"), 0), "lines"), 0), "line").as_number() == 2,
           "match_case finds only the exact case, on its line");
    const JsonValue pattern = Call(server, "search_scripts", R"({"pattern":"^t\\w+","regex":true})");
    Expect(Member(pattern, "matches").as_number() == 2 &&
               Member(Item(Member(pattern, "scripts"), 0), "path").as_string() == "Workspace.Main",
           "a regex matches per line, scripts in explorer order: " + ide::compact_json(pattern));
    const JsonValue scoped = Call(server, "search_scripts", R"({"pattern":"t","instance":"Workspace.Lib"})");
    Expect(Member(scoped, "scripts").items().size() == 1, "instance limits the search to what is under it");
    const JsonValue capped = Call(server, "search_scripts", R"({"pattern":"e","limit":1})");
    Expect(Member(capped, "matches").as_number() == 1 && capped.find("truncated") != nullptr,
           "limit caps the matches and says so");
    Expect(ErrorText(server, "search_scripts", R"({"pattern":"(","regex":true})").find("is not a regex") != std::string::npos,
           "a bad regex is refused");

    Call(server, "write_script", R"j({"instance":"Workspace.Lib.Util","source":"return {"})j");
    const JsonValue all = Call(server, "get_diagnostics", "{}");
    Expect(Member(all, "checked").as_number() == 2 && Member(all, "errors").as_number() >= 1 &&
               all.find("pending") == nullptr,
           "get_diagnostics checks every script: " + ide::compact_json(all));
    bool util = false;
    for (const JsonValue& entry : Member(all, "scripts").items()) {
        util = util || (Member(entry, "path").as_string() == "Workspace.Lib.Util" && HasProblem(Member(entry, "problems"), "Syntax", 1));
    }
    Expect(util, "get_diagnostics lists a script's problems under its path");
    const JsonValue one = Call(server, "get_diagnostics", R"({"instance":"Workspace.Main"})");
    Expect(Member(one, "checked").as_number() == 1, "get_diagnostics checks only the scripts asked for");
    engine_core::ModuleScript& loose = game.create<engine_core::ModuleScript>();
    const std::string loose_args = "{\"instance\":" + std::to_string(loose.id()) + ",\"source\":\"return {\"}";
    const JsonValue outside = Call(server, "write_script", loose_args);
    Expect(outside.find("problems") == nullptr && Member(outside, "analysis").as_string() == "not checked",
           "a script outside the place is not checked, rather than checked clean: " + ide::compact_json(outside));

    Call(server, "write_script", R"j({"instance":"Workspace.Main","source":"print('v1')"})j");
    Call(server, "write_script", R"j({"instance":"Workspace.Main","source":"print('v2')"})j");
    const JsonValue undone = Call(server, "undo", "{}");
    Expect(ScriptNamed(game, "Main")->source() == "print('v1')" && Member(undone, "undone").items().size() == 1 &&
               Member(undone, "next_redo").is_string(),
           "undo puts the Source back: " + ide::compact_json(undone));
    Call(server, "undo", R"({"redo":true})");
    Expect(ScriptNamed(game, "Main")->source() == "print('v2')", "redo applies it again");

    const JsonValue several = Call(server, "get_properties", R"({"instances":["Workspace.Main","Workspace.Lib"]})");
    Expect(Member(several, "instances").items().size() == 2 &&
               Member(Item(Member(several, "instances"), 1), "name").as_string() == "Lib",
           "get_properties reads several instances in order");

    // run_lua is the command line: a write is an undo step only when the chunk records it.
    game.history().reset_waypoints();
    game.history().mark_saved();
    Call(server, "run_lua", R"({"source":"workspace.Main.Name = 'Loose'"})");
    Expect(!game.history().can_undo().first, "a run_lua write outside a recording is not an undo step");
    Expect(!game.history().dirty(), "and does not mark the place unsaved");
    Call(server, "run_lua",
         R"j({"source":"local h = game:GetService('ChangeHistoryService') local id = h:TryBeginRecording('Rename Main') workspace.Loose.Name = 'Main' h:FinishRecording(id, Enum.FinishRecordingOperation.Commit)"})j");
    Expect(game.history().can_undo().second == "Rename Main", "a chunk that records is one named step");
    Expect(game.history().dirty(), "and marks the place unsaved");

    const std::string instructions = ide::default_instructions();
    Expect(instructions.find("run_lua") != std::string::npos &&
               instructions.find("ChangeHistoryService") != std::string::npos,
           "the instructions say run_lua records only through ChangeHistoryService");
}

// playtest run_for waits while the place plays, and returns what it printed.
void TestPlaytestRun() {
    engine_core::Engine engine;
    engine_core::DataModel& game = engine.datamodel();
    AddScript(game, "Hello", "print('from play')", game.scene_service("Workspace"));
    // What the studio's Test, Pause, Resume, and Stop do to the engine.
    auto state = std::make_shared<std::string>("stopped");
    auto mu = std::make_shared<std::mutex>();
    auto set = [state, mu](const char* next) {
        std::lock_guard<std::mutex> guard(*mu);
        *state = next;
    };
    ide::McpStudio studio;
    studio.start_test = [&engine, set] {
        engine.on_simulation([](engine_core::DataModel& world) {
            if (!world.simulation_running()) {
                world.capture_place();
                world.start_simulation();
            }
        });
        engine.resume();
        set("running");
    };
    studio.pause_test = [&engine, set] {
        engine.pause();
        set("paused");
    };
    studio.resume_test = [&engine, set] {
        engine.resume();
        set("running");
    };
    studio.stop_test = [&engine, set] {
        engine.pause();
        engine.on_simulation([](engine_core::DataModel& world) {
            if (world.simulation_running()) {
                world.stop_simulation();
            }
        });
        set("stopped");
    };
    studio.session = [state, mu] {
        std::lock_guard<std::mutex> guard(*mu);
        return *state;
    };
    ide::McpServer server;
    ide::add_engine_tools(server, engine, studio);
    engine.start();

    const JsonValue ran = Call(server, "playtest", R"({"action":"start","run_for":0.3})");
    bool printed = false;
    for (const JsonValue& line : Member(ran, "output").items()) {
        printed = printed || Member(line, "text").as_string() == "from play";
    }
    Expect(printed, "run_for returns what the place printed: " + ide::compact_json(ran));
    Expect(Member(ran, "session").as_string() == "paused" && Member(ran, "ran_for").as_number() >= 0.3 &&
               ran.find("ended_on_error") == nullptr,
           "run_for runs the whole time, then pauses");
    Expect(Member(Call(server, "playtest", R"({"action":"stop"})"), "session").as_string() == "stopped",
           "stop ends the test");

    Call(server, "write_script", R"j({"instance":"Workspace.Hello","source":"print('about to fail')\nerror('boom')"})j");
    const JsonValue failed = Call(server, "playtest", R"({"action":"start","run_for":10,"then":"stop"})");
    Expect(failed.find("ended_on_error") != nullptr && Member(failed, "ran_for").as_number() < 10 &&
               Member(failed, "errors").as_number() >= 1 && Member(failed, "session").as_string() == "stopped",
           "an error ends the wait early, and then stop ends the test: " + ide::compact_json(failed));
    engine.stop();
}

// While the place plays nothing is checked. get_diagnostics answers at once:
// with the last result from Edit mode for a script that has one for its
// current source, and not checked for any other.
void TestDiagnosticsDuringPlay() {
    engine_core::Engine engine;
    engine_core::DataModel& game = engine.datamodel();
    AddScript(game, "Authored", "local x =", game.scene_service("Workspace"));
    AddScript(game, "Other", "print(missing)", game.scene_service("Workspace"));
    auto state = std::make_shared<std::string>("stopped");
    auto mu = std::make_shared<std::mutex>();
    auto set = [state, mu](const char* next) {
        std::lock_guard<std::mutex> guard(*mu);
        *state = next;
    };
    ide::McpStudio studio;
    studio.start_test = [&engine, set] {
        engine.on_simulation([](engine_core::DataModel& world) {
            if (!world.simulation_running()) {
                world.capture_place();
                world.start_simulation();
            }
        });
        engine.resume();
        set("running");
    };
    studio.pause_test = [&engine, set] {
        engine.pause();
        set("paused");
    };
    studio.resume_test = [&engine, set] {
        engine.resume();
        set("running");
    };
    studio.stop_test = [&engine, set] {
        engine.pause();
        engine.on_simulation([](engine_core::DataModel& world) {
            if (world.simulation_running()) {
                world.stop_simulation();
            }
        });
        set("stopped");
    };
    studio.session = [state, mu] {
        std::lock_guard<std::mutex> guard(*mu);
        return *state;
    };
    ide::McpServer server;
    ide::add_engine_tools(server, engine, studio);
    engine.start();

    const JsonValue before = Call(server, "get_diagnostics", R"({"instance":"Workspace.Authored"})");
    Expect(Member(before, "checked").as_number() == 1 &&
               HasProblem(Member(Item(Member(before, "scripts"), 0), "problems"), "Syntax", 1),
           "Edit mode checks the script: " + ide::compact_json(before));

    Call(server, "playtest", R"({"action":"start","run_for":0.1})");
    const auto started = std::chrono::steady_clock::now();
    const JsonValue kept = Call(server, "get_diagnostics", R"({"instance":"Workspace.Authored"})");
    Expect(Member(kept, "checked").as_number() == 1 && kept.find("pending") == nullptr &&
               HasProblem(Member(Item(Member(kept, "scripts"), 0), "problems"), "Syntax", 1),
           "during play a script keeps its result from Edit mode: " + ide::compact_json(kept));
    const JsonValue written = Call(server, "write_script", R"j({"instance":"Workspace.Other","source":"print(1)"})j");
    Expect(written.find("problems") == nullptr && Member(written, "analysis").as_string() == "not checked",
           "a source changed during play is not checked, rather than checked clean: " + ide::compact_json(written));
    Call(server, "create_instance", R"({"class":"Script","name":"Runtime"})");
    const JsonValue all = Call(server, "get_diagnostics", "{}");
    std::vector<std::string> unchecked;
    if (const JsonValue* list = all.find("unchecked")) {
        for (const JsonValue& path : list->items()) {
            unchecked.push_back(path.as_string());
        }
    }
    std::sort(unchecked.begin(), unchecked.end());
    Expect(all.find("pending") == nullptr && Member(all, "checked").as_number() == 1 &&
               unchecked == std::vector<std::string>{"Workspace.Other", "Workspace.Runtime"},
           "during play nothing waits, and what has no result for its source is not checked: " +
               ide::compact_json(all));
    Expect(std::chrono::steady_clock::now() - started < std::chrono::seconds(5),
           "nothing during play waits for a check");

    Call(server, "playtest", R"({"action":"stop"})");
    const JsonValue after = Call(server, "get_diagnostics", "{}");
    Expect(after.find("pending") == nullptr && after.find("unchecked") == nullptr &&
               Member(after, "checked").as_number() == 2,
           "after Stop the authored scripts are checked: " + ide::compact_json(after));
    engine.stop();
}

// With the engine's threads running, an edit from another thread runs under
// the write lock while paused, and waits for a step while playing.
void TestThreadedEdits() {
    engine_core::Engine engine;
    ide::McpServer server;
    ide::add_engine_tools(server, engine, {});
    engine.start();
    std::thread paused([&] {
        Call(server, "create_instance", R"({"class":"Folder","name":"WhilePaused"})");
        const JsonValue run = Call(server, "run_lua", R"j({"source":"print(workspace:FindFirstChild('WhilePaused') ~= nil)"})j");
        Expect(Member(run, "output").items().size() == 1 && Member(Item(Member(run, "output"), 0), "text").as_string() == "true",
               "run_lua sees an edit made while paused");
    });
    paused.join();
    engine.resume();
    std::thread playing([&] {
        const JsonValue made = Call(server, "create_instance", R"({"class":"Folder","name":"WhilePlaying"})");
        Expect(made.find("path") != nullptr, "an edit while playing waits for the step");
        const JsonValue tree = Call(server, "get_tree", R"({"depth":1})");
        Expect(tree.find("tree") != nullptr, "a read while playing lands between steps");
    });
    playing.join();
    engine.stop();
}

// A real round trip over loopback, with the Origin and token checks.
void TestHttp() {
    ide::McpServer server;
    server.add_tool({"echo", "", ide::json_literal(R"({"type":"object"})"),
                     [](const JsonValue& arguments) { return arguments; }});
    server.set_token("secret");
    std::string error;
    Expect(server.start(0, error), "the server starts: " + error);
    if (!server.running() && server.port() == 0) {
        return;
    }
    httplib::Client client("127.0.0.1", server.port());
    const std::string body = R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"echo","arguments":{"x":1}}})";
    httplib::Headers auth = {{"Authorization", "Bearer secret"}};
    httplib::Result ok = client.Post("/mcp", auth, body, "application/json");
    Expect(ok && ok->status == 200, "a POST with the token is answered");
    if (ok) {
        const JsonValue reply = Parse(ok->body);
        Expect(reply.find("id") != nullptr && Member(reply, "id").as_number() == 7, "the reply carries the request id");
        Expect(ok->get_header_value("Content-Type").find("application/json") == 0, "the reply is JSON");
    }
    httplib::Result no_token = client.Post("/mcp", body, "application/json");
    Expect(no_token && no_token->status == 401, "a POST without the token is refused");
    httplib::Headers page = {{"Authorization", "Bearer secret"}, {"Origin", "https://evil.example"}};
    httplib::Result origin = client.Post("/mcp", page, body, "application/json");
    Expect(origin && origin->status == 403, "a web page's Origin is refused");
    httplib::Headers local = {{"Authorization", "Bearer secret"}, {"Origin", "http://localhost:3000"}};
    httplib::Result local_ok = client.Post("/mcp", local, body, "application/json");
    Expect(local_ok && local_ok->status == 200, "a localhost Origin is allowed");
    httplib::Headers sneaky = {{"Authorization", "Bearer secret"}, {"Origin", "http://localhost.evil.example"}};
    httplib::Result sneaky_result = client.Post("/mcp", sneaky, body, "application/json");
    Expect(sneaky_result && sneaky_result->status == 403, "a host that only starts with localhost is refused");
    httplib::Result get = client.Get("/mcp", auth);
    Expect(get && get->status == 405, "GET has no event stream");
    ide::McpServer rival;
    std::string rival_error;
    Expect(!rival.start(server.port(), rival_error) && !rival_error.empty(),
           "a second server cannot listen on a port another already has");
    server.stop();
    Expect(!server.running(), "stop ends the listener");
    server.stop();
}

// print records the script whose code called it, so the console can open that script.
void TestPrintSource() {
    engine_core::Engine engine;
    engine_core::DataModel& game = engine.datamodel();
    ide::McpServer server;
    ide::add_engine_tools(server, engine, {});
    engine_core::ModuleScript& module = game.create<engine_core::ModuleScript>();
    game.set_name(module.id(), "Lib");
    module.set_source("print('top')\nreturn { say = function()\n    pcall(print, 'said')\nend }");
    game.set_parent(module.id(), game.scene_service("Workspace"));

    const std::uint64_t since = engine.scripts().output_next();
    Call(server, "run_lua", R"j({"source":"local lib = require(workspace.Lib)\nprint('console')\nlib.say()"})j");
    const engine_core::ScriptRuntime::OutputHistory history = engine.scripts().output_since(since, 16);
    auto find = [&](const std::string& text) -> const engine_core::ScriptRuntime::OutputLine* {
        for (const auto& line : history.lines) {
            if (line.text == text + "\n") {
                return &line;
            }
        }
        return nullptr;
    };
    const auto* top = find("top");
    Expect(top != nullptr && top->script == module.id() && top->line == 1, "a module's print names the module and line");
    const auto* console = find("console");
    Expect(console != nullptr && console->script == 0, "a command's print names no script");
    const auto* said = find("said");
    Expect(said != nullptr && said->script == module.id() && said->line == 3,
           "a module function called from elsewhere, through pcall, still names the module");
    const auto* command = find("local lib = require(workspace.Lib)\nprint('console')\nlib.say()");
    Expect(command == nullptr || command->script == 0, "a command line names no script");
}

namespace fs = std::filesystem;

// An empty folder of its own under the system temp folder.
fs::path TempDir(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() /
                         ("anarchy-mcp-test-" + name + "-" + std::to_string(ide::current_pid()));
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    return dir;
}

void TestRegistry() {
    const fs::path dir = TempDir("registry");
    Expect(ide::list_studios(dir).empty(), "a missing registry lists no studios");
    std::string error;
    const ide::StudioEntry live{ide::current_pid(), 4321, "Alpha", "/somewhere/Alpha", "s3cret"};
    Expect(ide::write_studio(dir, live, error), "an entry is written: " + error);
    // Above any pid a system hands out.
    const ide::StudioEntry gone{0x7ffffff0, 4322, "Gone", ""};
    Expect(ide::write_studio(dir, gone, error), "a second entry is written: " + error);
    std::ofstream(dir / "junk.json") << "{nope";
    const std::vector<ide::StudioEntry> studios = ide::list_studios(dir);
    Expect(studios.size() == 1 && studios[0].project == "Alpha" && studios[0].port == 4321 &&
               studios[0].root == "/somewhere/Alpha" && studios[0].pid == ide::current_pid(),
           "a running studio is listed with its project and folder");
    Expect(studios.size() == 1 && studios[0].token == "s3cret", "a studio's token is listed with it");
    Expect(!fs::exists(dir / (std::to_string(gone.pid) + "-4322.json")), "the entry of an ended process is deleted");
    ide::remove_studio(dir, live);
    Expect(ide::list_studios(dir).empty(), "remove_studio takes the entry out");
    std::error_code ignored;
    fs::remove_all(dir, ignored);
}

// A stand-in for a studio: a real server on loopback whose whoami names it.
struct FakeStudio {
    ide::McpServer server;
    ide::StudioEntry entry;
};

std::unique_ptr<FakeStudio> OpenStudio(const fs::path& registry, const std::string& name, const fs::path& root) {
    auto studio = std::make_unique<FakeStudio>();
    studio->server.add_tool({"whoami", "", ide::json_literal(R"({"type":"object"})"), [name](const JsonValue&) {
                                 JsonValue out = JsonValue::object();
                                 out.set("name", JsonValue::string(name));
                                 return out;
                             }});
    std::string error;
    Expect(studio->server.start(0, error), name + " listens: " + error);
    studio->entry = {ide::current_pid(), studio->server.port(), name, ide::utf8_path(root)};
    Expect(ide::write_studio(registry, studio->entry, error), name + " is registered: " + error);
    return studio;
}

std::string Whoami(const ide::McpServer& bridge) {
    bool failed = false;
    const JsonValue out = Call(bridge, "whoami", "{}", &failed);
    return !failed && out.find("name") != nullptr ? Member(out, "name").as_string() : "(failed)";
}

void TestBridge() {
    const fs::path base = TempDir("bridge");
    const fs::path registry = base / "studios";
    fs::create_directories(base / "Alpha");
    fs::create_directories(base / "Beta" / "src");
    const std::vector<ide::McpToolSpec> catalog = {
        {"whoami", "Names the studio.", ide::json_literal(R"({"type":"object"})")}};
    bridge::StudioBridge outside({registry, base, "", ""}, catalog);
    const ide::McpServer& front = outside.server();

    const JsonValue init = Request(front, "initialize", R"({"protocolVersion":"2025-06-18"})");
    const JsonValue* instructions = init.find("result") != nullptr ? Member(init, "result").find("instructions") : nullptr;
    Expect(instructions != nullptr && instructions->as_string().find("select_studio") != std::string::npos,
           "the bridge's instructions explain how a studio is picked");
    const JsonValue tools = Request(front, "tools/list");
    std::vector<std::string> names;
    for (const JsonValue& tool : Member(Member(tools, "result"), "tools").items()) {
        names.push_back(Member(tool, "name").as_string());
    }
    Expect(names == std::vector<std::string>{"whoami", "list_studios", "select_studio"},
           "the bridge lists the studio's tools, then its own");
    Expect(ErrorText(front, "select_studio", R"({"studio":1e300})").find("studio must be") == 0,
           "a number too large to be a pid is refused");

    Expect(ErrorText(front, "whoami", "{}").find("No Anarchy Engine studio is open") == 0,
           "with no studio open, a call says to open one");

    auto alpha = OpenStudio(registry, "Alpha", base / "Alpha");
    Expect(Whoami(front) == "Alpha", "the only open studio takes the call");

    auto beta = OpenStudio(registry, "Beta", base / "Beta");
    const std::string several = ErrorText(front, "whoami", "{}");
    Expect(several.find("2 studios are open") == 0 && several.find("Alpha (") != std::string::npos &&
               several.find("Beta (") != std::string::npos,
           "with two open and neither holding the working directory, a call names both: " + several);

    bridge::StudioBridge inside({registry, base / "Beta" / "src", "", ""}, catalog);
    Expect(Whoami(inside.server()) == "Beta", "the studio whose folder holds the working directory takes the call");
    bridge::StudioBridge pinned({registry, base / "Beta", "alpha", ""}, catalog);
    Expect(Whoami(pinned.server()) == "Alpha", "--project names a studio, ignoring case, over the working directory");

    Call(front, "select_studio", R"({"studio":"ALPHA"})");
    Expect(Whoami(front) == "Alpha", "select_studio picks by name, ignoring case");
    const JsonValue listed = Call(front, "list_studios", "{}");
    bool alpha_selected = false;
    for (const JsonValue& studio : Member(listed, "studios").items()) {
        alpha_selected = alpha_selected ||
                         (Member(studio, "project").as_string() == "Alpha" && Member(studio, "selected").as_bool());
    }
    Expect(alpha_selected && Member(listed, "studios").items().size() == 2 &&
               Member(listed, "selected_by").as_string() == "select_studio",
           "list_studios shows both and marks the pick");
    Call(front, "select_studio",
         "{\"studio\":" + ide::compact_json(JsonValue::string(ide::utf8_path(base / "Beta"))) + "}");
    Expect(Whoami(front) == "Beta", "select_studio picks by folder");
    Expect(ErrorText(front, "select_studio", "{\"studio\":" + std::to_string(ide::current_pid()) + "}")
                   .find("matches several studios") != std::string::npos,
           "a pid two studios share is refused as ambiguous");
    Expect(ErrorText(front, "select_studio", R"({"studio":"Gamma"})").find("No open studio matches Gamma") == 0,
           "select_studio refuses a name nothing matches");

    Call(front, "select_studio", R"({"studio":"alpha"})");
    ide::remove_studio(registry, alpha->entry);
    alpha->server.stop();
    Expect(ErrorText(front, "whoami", "{}").find("has closed") != std::string::npos,
           "a call to a picked studio that closed fails");
    Expect(ErrorText(front, "whoami", "{}").find("has closed") != std::string::npos,
           "and keeps failing, rather than going to another studio");
    Call(front, "select_studio", R"({"studio":""})");
    Expect(Whoami(front) == "Beta", "dropping the pick goes back to the usual order");

    beta->server.stop();
    Expect(ErrorText(front, "whoami", "{}").find("Could not reach Beta") == 0,
           "a registered studio that stopped answering is named in the error");
    ide::remove_studio(registry, beta->entry);
    std::error_code ignored;
    fs::remove_all(base, ignored);
}

// import_assets hands the files to the studio's hook, then makes what the hook
// returns where edits run, as one undo step, and reports each file.
void TestImportAssets() {
    engine_core::Engine engine;
    engine_core::DataModel& game = engine.datamodel();
    const std::string brick = ide::utf8_path(fs::temp_directory_path() / "Brick.png");
    const std::string crate = ide::utf8_path(fs::temp_directory_path() / "Crate.fbx");
    const std::string broken = ide::utf8_path(fs::temp_directory_path() / "Broken.obj");
    std::vector<std::string> asked;
    ide::McpStudio studio;
    // What the studio's hook does, without reading any file: a Texture, a
    // model's Prefab with a Mesh in its folder, and a file that failed.
    studio.import_files = [&asked, brick, crate, broken](const std::vector<std::string>& files) -> ide::McpPlaceImports {
        asked = files;
        return [brick, crate, broken](engine_core::DataModel& world) {
            auto& texture = world.create<engine_core::Texture>();
            world.set_name(texture.id(), "Brick");
            (void)texture.set_path("textures/Brick.png");
            world.set_parent(texture.id(), world.service("Textures"));
            auto& folder = world.create<engine_core::Folder>();
            world.set_name(folder.id(), "Crate");
            world.set_parent(folder.id(), world.service("Meshes"));
            auto& mesh = world.create<engine_core::Mesh>();
            world.set_name(mesh.id(), "Wood");
            world.set_parent(mesh.id(), folder.id());
            auto& prefab = world.create<engine_core::Prefab>();
            world.set_name(prefab.id(), "Crate");
            world.set_parent(prefab.id(), world.service("Prefabs"));
            std::vector<ide::McpImport> imports(3);
            imports[0].file = brick;
            imports[0].kind = "texture";
            imports[0].root = texture.id();
            imports[0].made = {texture.id()};
            imports[1].file = crate;
            imports[1].kind = "model";
            imports[1].root = prefab.id();
            imports[1].made = {folder.id(), mesh.id(), prefab.id()};
            imports[1].notes = {"Animations are not imported yet"};
            imports[2].file = broken;
            imports[2].kind = "model";
            imports[2].error = "it holds no triangles";
            return imports;
        };
    };
    ide::McpServer server;
    ide::add_engine_tools(server, engine, studio);

    JsonValue files = JsonValue::array();
    for (const std::string& file : {brick, crate, broken}) {
        files.items().push_back(JsonValue::string(file));
    }
    JsonValue arguments = JsonValue::object();
    arguments.set("files", files);
    const JsonValue result = Call(server, "import_assets", ide::compact_json(arguments));
    Expect(asked == std::vector<std::string>{brick, crate, broken}, "import_assets hands the files to the studio");
    Expect(Member(result, "imported").as_number() == 2 && Member(result, "failed").as_number() == 1,
           "and counts what was imported and what failed: " + ide::compact_json(result));
    const JsonValue& rows = Member(result, "files");
    Expect(rows.items().size() == 3, "one row for each file, in order");
    if (rows.items().size() == 3) {
        const JsonValue& texture = Item(rows, 0);
        Expect(Member(texture, "kind").as_string() == "texture" &&
                   Member(Member(texture, "texture"), "path").as_string() == "Assets.Textures.Brick" &&
                   Member(texture, "resource_path").as_string() == "textures/Brick.png",
               "an image's row names its Texture and the file's Path: " + ide::compact_json(texture));
        const JsonValue& model = Item(rows, 1);
        Expect(Member(Member(model, "prefab"), "path").as_string() == "Assets.Prefabs.Crate" &&
                   Member(model, "meshes").as_number() == 1 && Member(model, "materials").as_number() == 0,
               "a model's row names its Prefab and counts its assets: " + ide::compact_json(model));
        Expect(Member(model, "folders").items().size() == 1 &&
                   Member(Item(Member(model, "folders"), 0), "path").as_string() == "Assets.Meshes.Crate",
               "and lists the folders that hold them");
        Expect(Member(model, "left_out").items().size() == 1 &&
                   Item(Member(model, "left_out"), 0).as_string() == "Animations are not imported yet",
               "and what the model left out");
        const JsonValue& failed = Item(rows, 2);
        Expect(Member(failed, "error").as_string() == "it holds no triangles" && failed.find("prefab") == nullptr,
               "a file that failed says why, and names nothing");
    }

    Call(server, "undo", "{}");
    Expect(game.get_children(game.service("Textures")).empty() && game.get_children(game.service("Prefabs")).empty(),
           "one undo takes the whole import back");

    asked.clear();
    Expect(ErrorText(server, "import_assets", R"({"files":["Brick.png"]})").find("is not an absolute path") !=
               std::string::npos,
           "a relative path is refused");
    Expect(ErrorText(server, "import_assets", R"({"files":[]})").find("files is required") == 0,
           "so is an empty list");
    Expect(asked.empty(), "and neither reaches the studio");

    ide::McpStudio playing;
    playing.import_files = [](const std::vector<std::string>&) -> ide::McpPlaceImports {
        throw std::runtime_error("Stop the test first.");
    };
    ide::McpServer refusing;
    ide::add_engine_tools(refusing, engine, playing);
    arguments.set("files", JsonValue::array({JsonValue::string(brick)}));
    Expect(ErrorText(refusing, "import_assets", ide::compact_json(arguments)) == "Stop the test first.",
           "a studio that cannot take files now says why");
}

// The bridge lists engine_tool_specs without an engine, so they must be what a
// studio registers: the same tools, descriptions, and schemas, in the same order.
void TestToolSpecs() {
    engine_core::Engine engine;
    ide::McpStudio studio;
    studio.start_test = [] {};
    studio.pause_test = [] {};
    studio.resume_test = [] {};
    studio.stop_test = [] {};
    studio.session = [] { return std::string("stopped"); };
    studio.info = [] { return JsonValue::object(); };
    studio.capture_view = [](int) { return ide::McpImage{}; };
    studio.import_files = [](const std::vector<std::string>&) { return ide::McpPlaceImports{}; };
    ide::McpServer every;
    ide::add_engine_tools(every, engine, studio);
    const std::vector<ide::McpToolSpec> specs = ide::engine_tool_specs();
    const bridge::StudioBridge bridge(bridge::BridgeOptions{}, specs);

    const JsonValue offered = Member(Member(Request(every, "tools/list"), "result"), "tools");
    const JsonValue listed = Member(Member(Request(bridge.server(), "tools/list"), "result"), "tools");
    Expect(offered.items().size() == specs.size(), "a studio with every hook offers every tool");
    Expect(listed.items().size() == offered.items().size() + 2, "the bridge lists the studio's tools, then its own two");
    for (std::size_t i = 0; i < offered.items().size(); ++i) {
        const JsonValue& tool = offered.items()[i];
        Expect(Item(listed, i) == tool, "the bridge lists " + Member(tool, "name").as_string() +
                                            " as the studio registers it: " + Excerpt(Item(listed, i)));
    }

    ide::McpServer plain;
    ide::add_engine_tools(plain, engine, {});
    std::vector<std::string> names;
    for (const ide::McpTool& tool : plain.tools()) {
        names.push_back(tool.name);
    }
    std::vector<std::string> expected;
    for (const ide::McpToolSpec& spec : specs) {
        if (spec.name != "playtest" && spec.name != "screenshot" && spec.name != "get_studio_info" &&
            spec.name != "import_assets") {
            expected.push_back(spec.name);
        }
    }
    Expect(names == expected, "a studio without the hooks leaves out playtest, screenshot, get_studio_info, and import_assets");
}

// An image a tool returns goes out as image content, beside the JSON text.
void TestImages() {
    Expect(ide::base64_encode("") == "" && ide::base64_encode("f") == "Zg==" && ide::base64_encode("fo") == "Zm8=" &&
               ide::base64_encode("foo") == "Zm9v" && ide::base64_encode("foob") == "Zm9vYg==" &&
               ide::base64_encode("foobar") == "Zm9vYmFy" && ide::base64_encode("\xff\xfe") == "//4=",
           "base64 matches RFC 4648");

    engine_core::Engine engine;
    ide::McpStudio studio;
    studio.capture_view = [](int max_size) {
        Expect(max_size == 256, "screenshot passes max_size on");
        return ide::McpImage{"\x89PNG", 4, 3};
    };
    ide::McpServer server;
    ide::add_engine_tools(server, engine, studio);
    const JsonValue reply =
        Request(server, "tools/call", R"({"name":"screenshot","arguments":{"max_size":256}})");
    const JsonValue* result = reply.find("result");
    const JsonValue* content = result != nullptr ? result->find("content") : nullptr;
    Expect(content != nullptr && content->items().size() == 2 &&
               Member(Item(*content, 1), "type").as_string() == "image" &&
               Member(Item(*content, 1), "data").as_string() == ide::base64_encode("\x89PNG") &&
               Member(Item(*content, 1), "mimeType").as_string() == "image/png",
           "screenshot sends the PNG as image content: " + ide::compact_json(reply));
    Expect(content != nullptr && Member(Item(*content, 0), "text").as_string() == R"({"height":3,"width":4})",
           "the text holds the rest, without the image");
    Expect(result != nullptr && Member(*result, "structuredContent").find(ide::kImageMember) == nullptr,
           "structured content leaves the image out");

    // Through the bridge, the image still reaches the client.
    const fs::path base = TempDir("images");
    const fs::path registry = base / "studios";
    fs::create_directories(base / "Pics");
    ide::McpServer pics;
    // A studio's own token, which the bridge reads from the registry.
    pics.set_token("s3cret");
    pics.add_tool({"picture", "", ide::json_literal(R"({"type":"object"})"), [](const JsonValue&) {
                       JsonValue image = JsonValue::object();
                       image.set("data", JsonValue::string("AAAA"));
                       image.set("mimeType", JsonValue::string("image/png"));
                       JsonValue out = JsonValue::object();
                       out.set("width", JsonValue::number(1));
                       out.set(ide::kImageMember, std::move(image));
                       return out;
                   }});
    std::string error;
    Expect(pics.start(0, error), "the picture studio listens: " + error);
    ide::StudioEntry entry{ide::current_pid(), pics.port(), "Pics", ide::utf8_path(base / "Pics"), "s3cret"};
    Expect(ide::write_studio(registry, entry, error), "the picture studio is registered: " + error);
    bridge::StudioBridge bridge({registry, base, "", ""}, {{"picture", "", ide::json_literal(R"({"type":"object"})")}});
    const JsonValue forwarded = Request(bridge.server(), "tools/call", R"({"name":"picture","arguments":{}})");
    const JsonValue* items = forwarded.find("result") != nullptr ? Member(forwarded, "result").find("content") : nullptr;
    Expect(items != nullptr && items->items().size() == 2 && Member(Item(*items, 1), "data").as_string() == "AAAA" &&
               Member(Item(*items, 0), "text").as_string() == R"({"width":1})",
           "the bridge forwards the image as image content: " + ide::compact_json(forwarded));
    entry.token = "stale";
    Expect(ide::write_studio(registry, entry, error), "the entry is registered again: " + error);
    const JsonValue refused = Request(bridge.server(), "tools/call", R"({"name":"picture","arguments":{}})");
    const JsonValue* failed = refused.find("result");
    Expect(failed != nullptr && failed->find("isError") != nullptr && Member(*failed, "isError").as_bool() &&
               Member(Item(Member(*failed, "content"), 0), "text").as_string().find("refused") != std::string::npos,
           "a token that does not match is refused: " + ide::compact_json(refused));
    pics.stop();
    ide::remove_studio(registry, entry);
    std::error_code ignored;
    fs::remove_all(base, ignored);
}

// get_profile: records for a while when nothing records, reads a paused history
// at once, and saves a capture file when given a path.
void TestProfileTool() {
    namespace fs = std::filesystem;
    profiler::reset_for_testing();
    engine_core::Engine engine;
    ide::McpServer server;
    ide::add_engine_tools(server, engine, {});
    engine.start();
    engine.resume();
    // Nothing is recording, as with the studio minimized: the tool collects for itself.
    const JsonValue report = Call(server, "get_profile", R"({"seconds":0.5,"top":5})");
    Expect(Member(report, "frames").as_number() >= 5, "get_profile records frames: " + Excerpt(report));
    Expect(!Member(report, "scopes").items().empty() && Member(report, "scopes").items().size() <= 5,
           "top limits the scope rows");
    Expect(report.find("slowest_frame") != nullptr && report.find("frame_ms") != nullptr &&
               report.find("gpu_lag_frames") != nullptr,
           "the report has the slowest frame, frame times, and the GPU lag");
    Expect(Member(report, "recorded_for").as_number() >= 0.45, "it recorded for the seconds asked");
    Expect(!profiler::enabled(), "and stopped recording after");

    const fs::path file = fs::temp_directory_path() / "anarchy-mcp-profile.aprof.json";
    fs::remove(file);
    const JsonValue saved = Call(server, "get_profile",
                                 std::string("{\"seconds\":0.3,\"include_timeline\":false,\"path\":\"") + file.generic_string() + "\"}");
    Expect(saved.find("slowest_frame") == nullptr, "include_timeline false leaves the tree out");
    std::ifstream in(file, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    profiler::History read;
    std::string error;
    Expect(profiler::read_capture(text, read, error) && !read.frames.empty(),
           "path writes a capture that opens again: " + error);
    fs::remove(file);

    // Paused, it reads the frozen history without waiting.
    profiler::acquire();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    profiler::collect();
    profiler::set_paused(true);
    std::size_t frozen = 0;
    profiler::with_view([&](const profiler::History& history) { frozen = history.frames.size(); });
    const auto began = std::chrono::steady_clock::now();
    const JsonValue paused = Call(server, "get_profile", R"({"seconds":5})");
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    Expect(took < 1.0, "a paused profile is read at once");
    Expect(Member(paused, "frames").as_number() == static_cast<double>(frozen), "the paused history's frames");
    profiler::set_paused(false);
    profiler::release();
    engine.stop();
    profiler::reset_for_testing();
}

}  // namespace

int main() {
    TestProtocol();
    TestEngineTools();
    TestScriptTools();
    TestPlaytestRun();
    TestDiagnosticsDuringPlay();
    TestPrintSource();
    TestThreadedEdits();
    TestHttp();
    TestRegistry();
    TestBridge();
    TestImportAssets();
    TestToolSpecs();
    TestImages();
    TestProfileTool();
    if (gFailures == 0) {
        std::printf("mcp tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d mcp tests failed\n", gFailures);
    return 1;
}
