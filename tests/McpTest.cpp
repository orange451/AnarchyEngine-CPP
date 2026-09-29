#include "bridge/StudioBridge.hpp"
#include "ide/IdeResources.hpp"
#include "ide/McpServer.hpp"
#include "ide/McpTools.hpp"
#include "ide/StudioRegistry.hpp"

#include "Engine.hpp"
#include "LuaSource.hpp"
#include "ModuleScript.hpp"
#include "ScriptAnalysis.hpp"
#include "ScriptRuntime.hpp"
#include "Script.hpp"
#include "httplib.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
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
    const bool error = result->find("isError") != nullptr && result->find("isError")->as_bool();
    if (failed != nullptr) {
        *failed = error;
    } else if (error) {
        const JsonValue& content = result->find("content")->items()[0];
        Expect(false, tool + " succeeds: " + content.find("text")->as_string());
    }
    const JsonValue* structured = result->find("structuredContent");
    return structured != nullptr ? *structured : JsonValue();
}

std::string ErrorText(const ide::McpServer& server, const std::string& tool, const std::string& arguments) {
    const JsonValue reply =
        Request(server, "tools/call", R"({"name":")" + tool + R"(","arguments":)" + arguments + "}");
    const JsonValue* result = reply.find("result");
    if (result == nullptr || result->find("isError") == nullptr || !result->find("isError")->as_bool()) {
        return {};
    }
    return result->find("content")->items()[0].find("text")->as_string();
}

void TestProtocol() {
    ide::McpServer server;
    server.add_tool({"echo", "Returns its arguments.", ide::json_literal(R"({"type":"object"})"),
                     [](const JsonValue& arguments) { return arguments; }});
    server.add_tool({"boom", "Always fails.", ide::json_literal(R"({"type":"object"})"),
                     [](const JsonValue&) -> JsonValue { throw std::runtime_error("it broke"); }});

    const JsonValue init = Request(server, "initialize", R"({"protocolVersion":"2025-06-18","capabilities":{}})");
    const JsonValue* result = init.find("result");
    Expect(result != nullptr && result->find("protocolVersion")->as_string() == "2025-06-18",
           "initialize answers with the client's version when it knows it");
    Expect(result != nullptr && result->find("capabilities")->find("tools") != nullptr, "initialize offers tools");
    const JsonValue newer = Request(server, "initialize", R"({"protocolVersion":"2099-01-01"})");
    Expect(newer.find("result")->find("protocolVersion")->as_string() == "2025-11-25",
           "an unknown version gets the newest this server speaks");

    const JsonValue list = Request(server, "tools/list");
    const JsonValue* tools = list.find("result")->find("tools");
    Expect(tools != nullptr && tools->items().size() == 2 && tools->items()[0].find("name")->as_string() == "echo" &&
               tools->items()[0].find("inputSchema")->is_object(),
           "tools/list names each tool with its schema");

    const JsonValue echoed = Call(server, "echo", R"({"a":1,"b":"two"})");
    Expect(echoed.find("a") != nullptr && echoed.find("a")->as_number() == 1, "a tool's result is its structured content");

    Expect(ErrorText(server, "boom", "{}") == "it broke", "a throwing tool reports its message as a failed call");

    const JsonValue unknown = Request(server, "tools/call", R"({"name":"nope"})");
    Expect(unknown.find("error") != nullptr && unknown.find("error")->find("code")->as_number() == -32602,
           "an unknown tool is an invalid-params error");
    const JsonValue missing = Request(server, "no/such");
    Expect(missing.find("error") != nullptr && missing.find("error")->find("code")->as_number() == -32601,
           "an unknown method is method-not-found");
    Expect(Request(server, "ping").find("result") != nullptr, "ping answers");

    int status = 0;
    Expect(server.handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})", status).empty() && status == 202,
           "a notification gets 202 and no body");
    const JsonValue bad = Parse(server.handle("{nope", status));
    Expect(bad.find("error") != nullptr && bad.find("error")->find("code")->as_number() == -32700,
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
    Expect(folder.find("path") != nullptr && folder.find("path")->as_string() == "Stuff",
           "create_instance puts it under the root");
    const JsonValue inner = Call(server, "create_instance", R"({"class":"Folder","name":"Inner","parent":"Stuff"})");
    Expect(inner.find("path")->as_string() == "Stuff.Inner", "create_instance takes a parent path");
    Expect(ErrorText(server, "create_instance", R"({"class":"Banana"})").find("Instance.new cannot make") == 0,
           "an unknown class is refused");
    AddScript(game, "Hello", "print('hi')", game.find_first_child(game.id(), "Stuff"));

    const JsonValue tree = Call(server, "get_tree", R"({"depth":1})");
    const JsonValue* top = tree.find("tree");
    Expect(top != nullptr && top->find("children") != nullptr, "get_tree lists the root's children");
    bool saw = false;
    for (const JsonValue& child : top->find("children")->items()) {
        if (child.find("name")->as_string() == "Stuff") {
            saw = child.find("children") == nullptr && child.find("child_count")->as_number() == 2;
        }
    }
    Expect(saw, "a row below depth reports its child count");
    const JsonValue deep = Call(server, "get_tree", R"({"instance":"game.Stuff","depth":3})");
    Expect(deep.find("tree")->find("children")->items().size() == 2, "get_tree starts at a path");

    const JsonValue found = Call(server, "find_instances", R"({"name":"inn"})");
    Expect(found.find("instances")->items().size() == 1 &&
               found.find("instances")->items()[0].find("path")->as_string() == "Stuff.Inner",
           "find_instances matches part of a name, ignoring case");

    const JsonValue props = Call(server, "get_properties", R"({"instance":"Stuff.Inner"})");
    const JsonValue* name = props.find("properties") != nullptr ? props.find("properties")->find("Name") : nullptr;
    Expect(name != nullptr && name->find("value")->as_string() == "Inner", "get_properties shows Name");
    const JsonValue* parent = props.find("properties") != nullptr ? props.find("properties")->find("Parent") : nullptr;
    Expect(parent != nullptr && parent->find("value")->find("path")->as_string() == "Stuff",
           "an Instance property shows its target's path");

    Call(server, "set_property", R"({"instance":"Stuff.Inner","property":"Name","value":"Renamed"})");
    Expect(game.find_first_child(game.find_first_child(game.id(), "Stuff"), "Renamed") != 0,
           "set_property renames through the property");
    Call(server, "set_property", R"({"instance":"Stuff.Renamed","property":"Parent","value":"game"})");
    Expect(game.find_first_child(game.id(), "Renamed") != 0, "set_property reparents by path");
    Expect(game.history().can_undo().first, "an edit is on the undo stack");
    Expect(!ErrorText(server, "set_property", R"({"instance":"Renamed","property":"Name","value":5})").empty(),
           "a value of the wrong type is refused");
    Expect(ErrorText(server, "get_properties", R"({"instance":"Nope.Missing"})").find("No instance at") == 0,
           "a missing path says where it stopped");
    Expect(ErrorText(server, "get_properties", R"({"instance":-1})") == "No instance has id -1.",
           "a negative id is no instance");
    Expect(ErrorText(server, "get_properties", R"({"instance":1e300})").find("No instance has id") == 0,
           "an id past any instance is no instance");
    Expect(Call(server, "get_tree", R"({"depth":1e300})").find("tree") != nullptr, "a huge depth is the deepest allowed");

    // A Color3 reads as [r, g, b] and takes that or a hex code.
    Call(server, "create_instance", R"({"class":"GameObject","name":"Box"})");
    Call(server, "set_property", R"({"instance":"Box","property":"Color","value":[1,0.5,0]})");
    auto color_of = [&server]() {
        const JsonValue box = Call(server, "get_properties", R"({"instance":"Box"})");
        const JsonValue* entry = box.find("properties") != nullptr ? box.find("properties")->find("Color") : nullptr;
        return entry != nullptr ? *entry : JsonValue();
    };
    JsonValue color = color_of();
    Expect(color.find("type") != nullptr && color.find("type")->as_string() == "Color3" &&
               color.find("readonly") == nullptr,
           "Color is a writable Color3");
    Expect(color.find("value") != nullptr && color.find("value")->items().size() == 3 &&
               color.find("value")->items()[1].as_number() == 0.5,
           "set_property writes [r, g, b] and get_properties reads it back");
    Call(server, "set_property", R"({"instance":"Box","property":"Color","value":"#0000FF"})");
    color = color_of();
    Expect(color.find("value")->items()[0].as_number() == 0 && color.find("value")->items()[2].as_number() == 1,
           "a hex code sets a Color3");
    Expect(!ErrorText(server, "set_property", R"({"instance":"Box","property":"Color","value":"blue"})").empty(),
           "a Color3 refuses what is not a color");

    const JsonValue run = Call(server, "run_lua", R"j({"source":"print('from mcp', 1 + 2)"})j");
    const JsonValue* lines = run.find("output");
    Expect(lines != nullptr && lines->items().size() == 1 &&
               lines->items()[0].find("text")->as_string() == "from mcp\t3",
           "run_lua returns what the chunk printed");
    const JsonValue failed = Call(server, "run_lua", R"j({"source":"error('nope')"})j");
    Expect(failed.find("error")->as_bool(), "run_lua reports an error");

    const JsonValue output = Call(server, "get_output", "{}");
    bool command = false;
    for (const JsonValue& line : output.find("lines")->items()) {
        command = command || (line.find("kind")->as_string() == "command" &&
                              line.find("text")->as_string() == "print('from mcp', 1 + 2)");
    }
    Expect(command, "the chunk run_lua ran shows in the console");
    const double next = output.find("next")->as_number();
    Call(server, "run_lua", R"j({"source":"print('later')"})j");
    const JsonValue newer = Call(server, "get_output", "{\"since\":" + engine_core::format_json_number(next) + "}");
    Expect(newer.find("lines")->items().size() == 2, "get_output since returns only newer lines");
    Expect(Call(server, "get_output", R"({"since":1e300})").find("lines")->items().empty(),
           "get_output since a line never written returns nothing");

    const JsonValue source = Call(server, "read_script", R"({"instance":"Stuff.Hello"})");
    Expect(source.find("source")->as_string() == "print('hi')", "read_script returns the Source");
    Call(server, "write_script", R"j({"instance":"Stuff.Hello","source":"print('bye')"})j");
    const auto* script = dynamic_cast<const engine_core::LuaSource*>(
        game.instance(game.find_first_child(game.find_first_child(game.id(), "Stuff"), "Hello")));
    Expect(script != nullptr && script->source() == "print('bye')", "write_script replaces the Source");
    Expect(!ErrorText(server, "read_script", R"({"instance":"Stuff"})").empty(), "read_script refuses a Folder");

    Call(server, "set_selection", R"({"instances":["Stuff","Renamed"]})");
    Expect(game.selection().get().size() == 2, "set_selection selects by path");
    const JsonValue selection = Call(server, "get_selection", "{}");
    Expect(selection.find("instances")->items().size() == 2 &&
               selection.find("instances")->items()[1].find("name")->as_string() == "Renamed",
           "get_selection lists them in order");

    const JsonValue classes = Call(server, "list_classes", "{}");
    bool creatable = false;
    for (const JsonValue& entry : classes.find("creatable")->items()) {
        creatable = creatable || entry.as_string() == "Folder";
    }
    Expect(creatable, "list_classes names Folder as creatable");
    const JsonValue api = Call(server, "get_class", R"({"class":"Folder"})");
    bool has_name = false;
    for (const JsonValue& entry : api.find("properties")->items()) {
        has_name = has_name || entry.find("name")->as_string() == "Name";
    }
    Expect(has_name, "get_class includes inherited properties");

    Call(server, "delete_instance", R"({"instance":"Stuff"})");
    Expect(game.find_first_child(game.id(), "Stuff") == 0, "delete_instance removes it");
    Expect(!ErrorText(server, "delete_instance", R"({"instance":"game"})").empty(), "the root is not deleted");
}

const engine_core::LuaSource* ScriptNamed(engine_core::DataModel& game, const char* name) {
    return dynamic_cast<const engine_core::LuaSource*>(game.instance(game.find_first_child(game.id(), name)));
}

bool HasProblem(const JsonValue& problems, const std::string& code, int line) {
    for (const JsonValue& problem : problems.items()) {
        if (problem.find("code")->as_string() == code && problem.find("line")->as_number() == line) {
            return true;
        }
    }
    return false;
}

// Scripts are checked, edited in part, searched, and undone. Analysis looks
// only at open scripts, as in the studio, so the tools watch what they ask about.
void TestScriptTools() {
    engine_core::Engine engine;
    engine_core::DataModel& game = engine.datamodel();
    engine.analysis().set_scope(engine_core::AnalysisScope::Open);
    ide::McpServer server;
    ide::add_engine_tools(server, engine, {});
    AddScript(game, "Main", "print('hi')", game.id());
    // The explorer ends each of its edits as a step. So does this test's own.
    game.history().end_gesture();

    const JsonValue broken = Call(server, "write_script", R"({"instance":"Main","source":"local x ="})");
    const JsonValue* problems = broken.find("problems");
    Expect(problems != nullptr && problems->items().size() == 1 && HasProblem(*problems, "Syntax", 1) &&
               problems->items()[0].find("severity")->as_string() == "error",
           "write_script returns a syntax error, lines from 1: " + ide::compact_json(broken));
    const JsonValue clean = Call(server, "write_script", R"j({"instance":"Main","source":"local n = 1\nprint(n)\n"})j");
    Expect(clean.find("problems") != nullptr && clean.find("problems")->items().empty(),
           "a clean script has no problems: " + ide::compact_json(clean));

    const JsonValue edited = Call(
        server, "edit_script",
        R"j({"instance":"Main","edits":[{"old_text":"print(n)","new_text":"print(n + missing)"}]})j");
    Expect(ScriptNamed(game, "Main")->source() == "local n = 1\nprint(n + missing)\n", "edit_script replaces the text");
    Expect(edited.find("replaced")->as_number() == 1, "edit_script counts its replacements");
    Expect(edited.find("problems") != nullptr && HasProblem(*edited.find("problems"), "Lint/UnknownGlobal", 2),
           "edit_script returns what analysis finds: " + ide::compact_json(edited));

    const JsonValue two = Call(server, "write_script",
                               R"j({"instance":"Main","source":"local n = 1\nprint(n + missing)\nlocal x = n"})j");
    const JsonValue* ordered = two.find("problems");
    Expect(ordered != nullptr && ordered->items().size() == 2 &&
               ordered->items()[0].find("line")->as_number() == 2 && ordered->items()[1].find("line")->as_number() == 3,
           "problems come in line order: " + ide::compact_json(two));

    Call(server, "write_script", R"j({"instance":"Main","source":"local a = 1\nlocal b = a\nlocal c = a\n"})j");
    const std::string twice =
        ErrorText(server, "edit_script", R"j({"instance":"Main","edits":[{"old_text":"= a","new_text":"= 2"}]})j");
    Expect(twice.find("2 times, on lines 2, 3") != std::string::npos,
           "old_text that matches twice is refused, naming the lines: " + twice);
    Expect(ErrorText(server, "edit_script", R"j({"instance":"Main","edits":[{"old_text":"nope","new_text":""}]})j")
                   .find("is not in the Source") != std::string::npos,
           "old_text that does not match is refused");
    ErrorText(server, "edit_script",
              R"j({"instance":"Main","edits":[{"old_text":"local b","new_text":"local B"},{"old_text":"nope","new_text":""}]})j");
    Expect(ScriptNamed(game, "Main")->source() == "local a = 1\nlocal b = a\nlocal c = a\n",
           "a failed edit leaves the earlier ones unapplied");
    const JsonValue every = Call(
        server, "edit_script", R"j({"instance":"Main","edits":[{"old_text":"= a","new_text":"= 2","replace_all":true}]})j");
    Expect(every.find("replaced")->as_number() == 2 &&
               ScriptNamed(game, "Main")->source() == "local a = 1\nlocal b = 2\nlocal c = 2\n",
           "replace_all replaces each place");

    Call(server, "write_script", R"j({"instance":"Main","source":"one\ntwo\nthree\n"})j");
    const JsonValue whole = Call(server, "read_script", R"({"instance":"Main"})");
    Expect(whole.find("line_count")->as_number() == 4 && whole.find("source")->as_string() == "one\ntwo\nthree\n",
           "read_script returns the whole Source and its line count");
    const JsonValue middle = Call(server, "read_script", R"({"instance":"Main","first_line":2,"last_line":3})");
    Expect(middle.find("source")->as_string() == "two\nthree" && middle.find("first_line")->as_number() == 2,
           "read_script returns a range of lines");
    const JsonValue tail = Call(server, "read_script", R"({"instance":"Main","first_line":3})");
    Expect(tail.find("source")->as_string() == "three\n" && tail.find("last_line")->as_number() == 4,
           "a range without last_line runs to the end");
    Expect(ErrorText(server, "read_script", R"({"instance":"Main","first_line":9})") == "The Source has 4 lines.",
           "a range past the end is refused");

    const JsonValue lib = Call(server, "create_instance", R"({"class":"Folder","name":"Lib"})");
    engine_core::ModuleScript& module = game.create<engine_core::ModuleScript>();
    game.set_name(module.id(), "Util");
    module.set_source("local Value = 1\nreturn value\n");
    game.set_parent(module.id(), static_cast<engine_core::InstanceId>(lib.find("id")->as_number()));
    game.history().end_gesture();

    const JsonValue found = Call(server, "search_scripts", R"({"pattern":"VALUE"})");
    const JsonValue* scripts = found.find("scripts");
    Expect(scripts->items().size() == 1 && scripts->items()[0].find("path")->as_string() == "Lib.Util" &&
               scripts->items()[0].find("lines")->items().size() == 2 && found.find("matches")->as_number() == 2,
           "search_scripts ignores case and lists matching lines: " + ide::compact_json(found));
    const JsonValue cased = Call(server, "search_scripts", R"({"pattern":"value","match_case":true})");
    Expect(cased.find("matches")->as_number() == 1 &&
               cased.find("scripts")->items()[0].find("lines")->items()[0].find("line")->as_number() == 2,
           "match_case finds only the exact case, on its line");
    const JsonValue pattern = Call(server, "search_scripts", R"({"pattern":"^t\\w+","regex":true})");
    Expect(pattern.find("matches")->as_number() == 2 &&
               pattern.find("scripts")->items()[0].find("path")->as_string() == "Main",
           "a regex matches per line, scripts in explorer order: " + ide::compact_json(pattern));
    const JsonValue scoped = Call(server, "search_scripts", R"({"pattern":"t","instance":"Lib"})");
    Expect(scoped.find("scripts")->items().size() == 1, "instance limits the search to what is under it");
    const JsonValue capped = Call(server, "search_scripts", R"({"pattern":"e","limit":1})");
    Expect(capped.find("matches")->as_number() == 1 && capped.find("truncated") != nullptr,
           "limit caps the matches and says so");
    Expect(ErrorText(server, "search_scripts", R"({"pattern":"(","regex":true})").find("is not a regex") != std::string::npos,
           "a bad regex is refused");

    Call(server, "write_script", R"j({"instance":"Lib.Util","source":"return {"})j");
    const JsonValue all = Call(server, "get_diagnostics", "{}");
    Expect(all.find("checked")->as_number() == 2 && all.find("errors")->as_number() >= 1 &&
               all.find("pending") == nullptr,
           "get_diagnostics checks every script: " + ide::compact_json(all));
    bool util = false;
    for (const JsonValue& entry : all.find("scripts")->items()) {
        util = util || (entry.find("path")->as_string() == "Lib.Util" && HasProblem(*entry.find("problems"), "Syntax", 1));
    }
    Expect(util, "get_diagnostics lists a script's problems under its path");
    const JsonValue one = Call(server, "get_diagnostics", R"({"instance":"Main"})");
    Expect(one.find("checked")->as_number() == 1, "get_diagnostics checks only the scripts asked for");

    Call(server, "write_script", R"j({"instance":"Main","source":"print('v1')"})j");
    Call(server, "write_script", R"j({"instance":"Main","source":"print('v2')"})j");
    const JsonValue undone = Call(server, "undo", "{}");
    Expect(ScriptNamed(game, "Main")->source() == "print('v1')" && undone.find("undone")->items().size() == 1 &&
               undone.find("next_redo")->is_string(),
           "undo puts the Source back: " + ide::compact_json(undone));
    Call(server, "undo", R"({"redo":true})");
    Expect(ScriptNamed(game, "Main")->source() == "print('v2')", "redo applies it again");

    const JsonValue several = Call(server, "get_properties", R"({"instances":["Main","Lib"]})");
    Expect(several.find("instances")->items().size() == 2 &&
               several.find("instances")->items()[1].find("name")->as_string() == "Lib",
           "get_properties reads several instances in order");
}

// playtest run_for waits while the place plays, and returns what it printed.
void TestPlaytestRun() {
    engine_core::Engine engine;
    engine_core::DataModel& game = engine.datamodel();
    AddScript(game, "Hello", "print('from play')", game.id());
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
    for (const JsonValue& line : ran.find("output")->items()) {
        printed = printed || line.find("text")->as_string() == "from play";
    }
    Expect(printed, "run_for returns what the place printed: " + ide::compact_json(ran));
    Expect(ran.find("session")->as_string() == "paused" && ran.find("ran_for")->as_number() >= 0.3 &&
               ran.find("ended_on_error") == nullptr,
           "run_for runs the whole time, then pauses");
    Expect(Call(server, "playtest", R"({"action":"stop"})").find("session")->as_string() == "stopped",
           "stop ends the test");

    Call(server, "write_script", R"j({"instance":"Hello","source":"print('about to fail')\nerror('boom')"})j");
    const JsonValue failed = Call(server, "playtest", R"({"action":"start","run_for":10,"then":"stop"})");
    Expect(failed.find("ended_on_error") != nullptr && failed.find("ran_for")->as_number() < 10 &&
               failed.find("errors")->as_number() >= 1 && failed.find("session")->as_string() == "stopped",
           "an error ends the wait early, and then stop ends the test: " + ide::compact_json(failed));
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
        const JsonValue run = Call(server, "run_lua", R"j({"source":"print(game:FindFirstChild('WhilePaused') ~= nil)"})j");
        Expect(run.find("output")->items().size() == 1 && run.find("output")->items()[0].find("text")->as_string() == "true",
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
        Expect(reply.find("id") != nullptr && reply.find("id")->as_number() == 7, "the reply carries the request id");
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
    game.set_parent(module.id(), game.id());

    const std::uint64_t since = engine.scripts().output_next();
    Call(server, "run_lua", R"j({"source":"local lib = require(game.Lib)\nprint('console')\nlib.say()"})j");
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
    const auto* command = find("local lib = require(game.Lib)\nprint('console')\nlib.say()");
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
    const ide::StudioEntry live{ide::current_pid(), 4321, "Alpha", "/somewhere/Alpha"};
    Expect(ide::write_studio(dir, live, error), "an entry is written: " + error);
    // Above any pid a system hands out.
    const ide::StudioEntry gone{0x7ffffff0, 4322, "Gone", ""};
    Expect(ide::write_studio(dir, gone, error), "a second entry is written: " + error);
    std::ofstream(dir / "junk.json") << "{nope";
    const std::vector<ide::StudioEntry> studios = ide::list_studios(dir);
    Expect(studios.size() == 1 && studios[0].project == "Alpha" && studios[0].port == 4321 &&
               studios[0].root == "/somewhere/Alpha" && studios[0].pid == ide::current_pid(),
           "a running studio is listed with its project and folder");
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
    return !failed && out.find("name") != nullptr ? out.find("name")->as_string() : "(failed)";
}

void TestBridge() {
    const fs::path base = TempDir("bridge");
    const fs::path registry = base / "studios";
    fs::create_directories(base / "Alpha");
    fs::create_directories(base / "Beta" / "src");
    const std::vector<ide::McpTool> catalog = {
        {"whoami", "Names the studio.", ide::json_literal(R"({"type":"object"})"), nullptr}};
    bridge::StudioBridge outside({registry, base, "", ""}, catalog);
    const ide::McpServer& front = outside.server();

    const JsonValue init = Request(front, "initialize", R"({"protocolVersion":"2025-06-18"})");
    const JsonValue* instructions = init.find("result") != nullptr ? init.find("result")->find("instructions") : nullptr;
    Expect(instructions != nullptr && instructions->as_string().find("select_studio") != std::string::npos,
           "the bridge's instructions explain how a studio is picked");
    const JsonValue tools = Request(front, "tools/list");
    std::vector<std::string> names;
    for (const JsonValue& tool : tools.find("result")->find("tools")->items()) {
        names.push_back(tool.find("name")->as_string());
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
    for (const JsonValue& studio : listed.find("studios")->items()) {
        alpha_selected = alpha_selected ||
                         (studio.find("project")->as_string() == "Alpha" && studio.find("selected")->as_bool());
    }
    Expect(alpha_selected && listed.find("studios")->items().size() == 2 &&
               listed.find("selected_by")->as_string() == "select_studio",
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
               content->items()[1].find("type")->as_string() == "image" &&
               content->items()[1].find("data")->as_string() == ide::base64_encode("\x89PNG") &&
               content->items()[1].find("mimeType")->as_string() == "image/png",
           "screenshot sends the PNG as image content: " + ide::compact_json(reply));
    Expect(content != nullptr && content->items()[0].find("text")->as_string() == R"({"height":3,"width":4})",
           "the text holds the rest, without the image");
    Expect(result != nullptr && result->find("structuredContent")->find(ide::kImageMember) == nullptr,
           "structured content leaves the image out");

    // Through the bridge, the image still reaches the client.
    const fs::path base = TempDir("images");
    const fs::path registry = base / "studios";
    fs::create_directories(base / "Pics");
    ide::McpServer pics;
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
    const ide::StudioEntry entry{ide::current_pid(), pics.port(), "Pics", ide::utf8_path(base / "Pics")};
    Expect(ide::write_studio(registry, entry, error), "the picture studio is registered: " + error);
    bridge::StudioBridge bridge({registry, base, "", ""}, {{"picture", "", ide::json_literal(R"({"type":"object"})"), nullptr}});
    const JsonValue forwarded = Request(bridge.server(), "tools/call", R"({"name":"picture","arguments":{}})");
    const JsonValue* items = forwarded.find("result") != nullptr ? forwarded.find("result")->find("content") : nullptr;
    Expect(items != nullptr && items->items().size() == 2 && items->items()[1].find("data")->as_string() == "AAAA" &&
               items->items()[0].find("text")->as_string() == R"({"width":1})",
           "the bridge forwards the image as image content: " + ide::compact_json(forwarded));
    pics.stop();
    ide::remove_studio(registry, entry);
    std::error_code ignored;
    fs::remove_all(base, ignored);
}

}  // namespace

int main() {
    TestProtocol();
    TestEngineTools();
    TestScriptTools();
    TestPlaytestRun();
    TestPrintSource();
    TestThreadedEdits();
    TestHttp();
    TestRegistry();
    TestBridge();
    TestImages();
    if (gFailures == 0) {
        std::printf("mcp tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d mcp tests failed\n", gFailures);
    return 1;
}
