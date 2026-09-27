#include "ide/McpServer.hpp"
#include "ide/McpTools.hpp"

#include "Engine.hpp"
#include "LuaSource.hpp"
#include "Script.hpp"
#include "httplib.h"

#include <cstdio>
#include <stdexcept>
#include <string>
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
    bool failed = false;
    const JsonValue reply =
        Request(server, "tools/call", R"({"name":")" + tool + R"(","arguments":)" + arguments + "}");
    const JsonValue* result = reply.find("result");
    if (result == nullptr || result->find("isError") == nullptr || !result->find("isError")->as_bool()) {
        return {};
    }
    (void)failed;
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
    server.stop();
    Expect(!server.running(), "stop ends the listener");
    server.stop();
}

}  // namespace

int main() {
    TestProtocol();
    TestEngineTools();
    TestThreadedEdits();
    TestHttp();
    if (gFailures == 0) {
        std::printf("mcp tests passed\n");
        return 0;
    }
    std::fprintf(stderr, "%d mcp tests failed\n", gFailures);
    return 1;
}
