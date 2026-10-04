#pragma once

#include "PropertyBag.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace httplib {
class Server;
}  // namespace httplib

namespace ide {

// One tool an MCP client can call. run gets the call's arguments, an object,
// and returns the result. A std::exception thrown from run is reported to the
// client as a failed call with its message, not as a protocol error.
// run is called on the server's threads, several at once.
struct McpTool {
    std::string name;
    std::string description;
    // A JSON Schema object describing the arguments.
    engine_core::JsonValue input_schema;
    std::function<engine_core::JsonValue(const engine_core::JsonValue& arguments)> run;
};

// What clients have asked of a server since it started, for the studio to show.
struct McpActivity {
    // clientInfo's name from the latest initialize, such as "claude-code". Empty before one.
    std::string client;
    // The latest tool called, and how many tool calls there have been.
    std::string last_tool;
    std::uint64_t calls = 0;
    // When the latest request of any kind, and the latest tool call, came in.
    // The clock's epoch before the first.
    std::chrono::steady_clock::time_point last_request{};
    std::chrono::steady_clock::time_point last_call{};
};

// A Model Context Protocol server over Streamable HTTP. One endpoint, /mcp,
// takes JSON-RPC over POST and answers with application/json; it opens no
// event streams, so GET is refused. It keeps no sessions.
//
// It listens on the loopback address only. A request whose Origin header is
// not a localhost page is refused, so a web page cannot reach it through the
// browser. With a token set, every request needs "Authorization: Bearer <token>".
//
// Tools are added before start and not changed after.
class McpServer {
public:
    McpServer();
    ~McpServer();

    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    void add_tool(McpTool tool);
    const std::vector<McpTool>& tools() const { return tools_; }
    void set_token(std::string token);
    // What initialize tells the client about the server. default_instructions() until set.
    void set_instructions(std::string instructions);

    // Listens on 127.0.0.1:port from its own thread. Port 0 takes any free
    // port, which port() then reports. False, with error set, when the port
    // cannot be bound, including when another server already listens there.
    bool start(int port, std::string& error);
    // Stops listening and waits for the thread. Safe to call twice.
    void stop();
    bool running() const { return running_.load(); }
    int port() const { return port_; }

    // One POST body in, the reply body out. status is the HTTP status. An
    // empty reply with 202 means the body held only notifications.
    std::string handle(const std::string& body, int& status) const;

    // A copy of what clients have asked so far. Safe from any thread.
    McpActivity activity() const;

private:
    engine_core::JsonValue dispatch(const engine_core::JsonValue& message, bool& reply) const;
    engine_core::JsonValue call_tool(const engine_core::JsonValue& params, bool& found) const;

    std::vector<McpTool> tools_;
    std::string token_;
    std::string instructions_;
    std::unique_ptr<httplib::Server> http_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    int port_ = 0;
    // Written by handle, on the server's threads, several at once.
    mutable std::mutex activity_mutex_;
    mutable McpActivity activity_;
};

// A tool result holds an image under this member: an object with "data", the
// bytes in base64, and "mimeType". The server sends it as image content after
// the JSON text, not inside it. The bridge puts it back here when it forwards.
inline constexpr const char* kImageMember = "image_content";

// What a studio's server tells the client: the place, paths, undo, run_lua.
const char* default_instructions();
// Standard base64 with padding, as MCP image content carries bytes.
std::string base64_encode(std::string_view bytes);
// What goes on the wire and into a tool result's text.
using engine_core::compact_json;
// Parses text written in the source, such as a schema. Throws on bad JSON.
engine_core::JsonValue json_literal(const char* text);

}  // namespace ide
