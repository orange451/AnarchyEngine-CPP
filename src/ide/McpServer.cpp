#include "McpServer.hpp"

#include "httplib.h"

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ide {
namespace {

using engine_core::JsonValue;

// The protocol versions this server speaks, newest first. A client asking
// for one of these gets it back. Any other request gets the newest.
constexpr const char* kVersions[] = {"2025-11-25", "2025-06-18", "2025-03-26"};

constexpr const char* kServerName = "anarchy-engine";
constexpr const char* kServerVersion = "0.1.0";

constexpr const char* kDefaultInstructions =
    "Anarchy Engine studio. The place is a tree of instances under the root, `game`. "
    "Name an instance by its id, or by its path of Names from the root such as \"Folder.Part\". "
    "Edits go through the studio's undo history, as if made by hand. "
    "run_lua runs Luau against the live place, like the studio's command line.";

// JSON-RPC error codes.
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;

void AppendString(std::string& out, const std::string& text) {
    out.push_back('"');
    for (const char unit : text) {
        const auto byte = static_cast<unsigned char>(unit);
        switch (unit) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (byte < 0x20) {
                    char escaped[8];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", byte);
                    out += escaped;
                } else {
                    out.push_back(unit);
                }
        }
    }
    out.push_back('"');
}

void AppendJson(std::string& out, const JsonValue& value) {
    switch (value.kind()) {
        case JsonValue::Kind::Null:
            out += "null";
            break;
        case JsonValue::Kind::Bool:
            out += value.as_bool() ? "true" : "false";
            break;
        case JsonValue::Kind::Number:
            out += engine_core::format_json_number(value.as_number());
            break;
        case JsonValue::Kind::String:
            AppendString(out, value.as_string());
            break;
        case JsonValue::Kind::Array: {
            out.push_back('[');
            bool first = true;
            for (const JsonValue& item : value.items()) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                AppendJson(out, item);
            }
            out.push_back(']');
            break;
        }
        case JsonValue::Kind::Object: {
            out.push_back('{');
            bool first = true;
            for (const JsonValue::Member& member : value.members()) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                AppendString(out, member.first);
                out.push_back(':');
                AppendJson(out, member.second);
            }
            out.push_back('}');
            break;
        }
    }
}

JsonValue ErrorReply(const JsonValue& id, int code, const std::string& message) {
    JsonValue error = JsonValue::object();
    error.set("code", JsonValue::number(code));
    error.set("message", JsonValue::string(message));
    JsonValue reply = JsonValue::object();
    reply.set("jsonrpc", JsonValue::string("2.0"));
    reply.set("id", id);
    reply.set("error", std::move(error));
    return reply;
}

JsonValue ResultReply(const JsonValue& id, JsonValue result) {
    JsonValue reply = JsonValue::object();
    reply.set("jsonrpc", JsonValue::string("2.0"));
    reply.set("id", id);
    reply.set("result", std::move(result));
    return reply;
}

JsonValue TextContent(std::string text) {
    JsonValue item = JsonValue::object();
    item.set("type", JsonValue::string("text"));
    item.set("text", JsonValue::string(std::move(text)));
    return JsonValue::array({std::move(item)});
}

std::string PickVersion(const JsonValue& params) {
    if (const JsonValue* asked = params.find("protocolVersion")) {
        for (const char* version : kVersions) {
            if (asked->as_string() == version) {
                return version;
            }
        }
    }
    return kVersions[0];
}

// A page on this machine, or no Origin at all, which is what non-browser clients send.
bool LocalOrigin(const std::string& origin) {
    if (origin.empty() || origin == "null") {
        return origin.empty();
    }
    for (const char* prefix : {"http://localhost", "http://127.0.0.1", "http://[::1]", "https://localhost",
                               "https://127.0.0.1", "https://[::1]"}) {
        const std::string_view head(prefix);
        if (origin.compare(0, head.size(), head) != 0) {
            continue;
        }
        // Only a port, or nothing, may follow: http://localhost.example.com is not local.
        if (origin.size() == head.size() || origin[head.size()] == ':') {
            return true;
        }
    }
    return false;
}

}  // namespace

std::string compact_json(const JsonValue& value) {
    std::string out;
    AppendJson(out, value);
    return out;
}

JsonValue json_literal(const char* text) {
    JsonValue value;
    std::string error;
    if (!engine_core::parse_json(text != nullptr ? text : "", value, error)) {
        throw std::invalid_argument("json_literal: " + error);
    }
    return value;
}

const char* default_instructions() { return kDefaultInstructions; }

McpServer::McpServer() : instructions_(kDefaultInstructions) {}

McpServer::~McpServer() { stop(); }

void McpServer::add_tool(McpTool tool) { tools_.push_back(std::move(tool)); }

void McpServer::set_token(std::string token) { token_ = std::move(token); }

void McpServer::set_instructions(std::string instructions) { instructions_ = std::move(instructions); }

bool McpServer::start(int port, std::string& error) {
    stop();
    auto http = std::make_unique<httplib::Server>();
    http->Post("/mcp", [this](const httplib::Request& request, httplib::Response& response) {
        if (!LocalOrigin(request.get_header_value("Origin"))) {
            response.status = 403;
            response.set_content("Forbidden origin", "text/plain");
            return;
        }
        if (!token_.empty() && request.get_header_value("Authorization") != "Bearer " + token_) {
            response.status = 401;
            response.set_content("Missing or wrong token", "text/plain");
            return;
        }
        int status = 200;
        std::string body = handle(request.body, status);
        response.status = status;
        if (!body.empty()) {
            response.set_content(std::move(body), "application/json");
        }
    });
    // No event streams: a GET that asks for one is told this server has none.
    http->Get("/mcp", [](const httplib::Request&, httplib::Response& response) { response.status = 405; });
    http->Delete("/mcp", [](const httplib::Request&, httplib::Response& response) { response.status = 405; });
    // httplib's default is SO_REUSEPORT, which lets a second studio bind this
    // port too and take some of the first one's requests. One port, one studio.
    http->set_socket_options([](socket_t sock) {
#ifdef _WIN32
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
    });
    if (port == 0) {
        port = http->bind_to_any_port("127.0.0.1");
        if (port <= 0) {
            error = "could not listen on any port of 127.0.0.1.";
            return false;
        }
    } else if (!http->bind_to_port("127.0.0.1", port)) {
        error = "could not listen on 127.0.0.1:" + std::to_string(port) + ". Is another program using that port?";
        return false;
    }
    http_ = std::move(http);
    port_ = port;
    running_ = true;
    thread_ = std::thread([this] {
        http_->listen_after_bind();
        running_ = false;
    });
    return true;
}

void McpServer::stop() {
    if (http_) {
        http_->stop();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    http_.reset();
    running_ = false;
}

std::string McpServer::handle(const std::string& body, int& status) const {
    status = 200;
    JsonValue message;
    std::string error;
    if (!engine_core::parse_json(body, message, error)) {
        return compact_json(ErrorReply(JsonValue(), kParseError, "Parse error: " + error));
    }
    // A batch is an array of messages. Only the requests in it get replies.
    if (message.is_array()) {
        if (message.items().empty()) {
            return compact_json(ErrorReply(JsonValue(), kInvalidRequest, "Empty batch"));
        }
        JsonValue replies = JsonValue::array();
        for (const JsonValue& item : message.items()) {
            bool reply = false;
            JsonValue answer = dispatch(item, reply);
            if (reply) {
                replies.items().push_back(std::move(answer));
            }
        }
        if (replies.items().empty()) {
            status = 202;
            return {};
        }
        return compact_json(replies);
    }
    bool reply = false;
    JsonValue answer = dispatch(message, reply);
    if (!reply) {
        status = 202;
        return {};
    }
    return compact_json(answer);
}

JsonValue McpServer::dispatch(const JsonValue& message, bool& reply) const {
    reply = true;
    if (!message.is_object()) {
        return ErrorReply(JsonValue(), kInvalidRequest, "Invalid request");
    }
    const JsonValue* id = message.find("id");
    const JsonValue* method = message.find("method");
    // A response from the client, or a notification: nothing to send back.
    if (method == nullptr || id == nullptr) {
        reply = false;
        return {};
    }
    if (!method->is_string()) {
        return ErrorReply(*id, kInvalidRequest, "Invalid request");
    }
    static const JsonValue kNoParams = JsonValue::object();
    const JsonValue* params = message.find("params");
    if (params == nullptr || !params->is_object()) {
        params = &kNoParams;
    }
    const std::string& name = method->as_string();
    if (name == "initialize") {
        JsonValue tools = JsonValue::object();
        tools.set("listChanged", JsonValue::boolean(false));
        JsonValue capabilities = JsonValue::object();
        capabilities.set("tools", std::move(tools));
        JsonValue info = JsonValue::object();
        info.set("name", JsonValue::string(kServerName));
        info.set("version", JsonValue::string(kServerVersion));
        JsonValue result = JsonValue::object();
        result.set("protocolVersion", JsonValue::string(PickVersion(*params)));
        result.set("capabilities", std::move(capabilities));
        result.set("serverInfo", std::move(info));
        result.set("instructions", JsonValue::string(instructions_));
        return ResultReply(*id, std::move(result));
    }
    if (name == "ping") {
        return ResultReply(*id, JsonValue::object());
    }
    if (name == "tools/list") {
        JsonValue list = JsonValue::array();
        for (const McpTool& tool : tools_) {
            JsonValue entry = JsonValue::object();
            entry.set("name", JsonValue::string(tool.name));
            entry.set("description", JsonValue::string(tool.description));
            entry.set("inputSchema", tool.input_schema.is_object() ? tool.input_schema : json_literal(R"({"type":"object"})"));
            list.items().push_back(std::move(entry));
        }
        JsonValue result = JsonValue::object();
        result.set("tools", std::move(list));
        return ResultReply(*id, std::move(result));
    }
    if (name == "tools/call") {
        bool found = false;
        JsonValue result = call_tool(*params, found);
        if (!found) {
            const JsonValue* tool = params->find("name");
            return ErrorReply(*id, kInvalidParams, "Unknown tool: " + (tool != nullptr ? tool->as_string() : ""));
        }
        return ResultReply(*id, std::move(result));
    }
    return ErrorReply(*id, kMethodNotFound, "Method not found: " + name);
}

JsonValue McpServer::call_tool(const JsonValue& params, bool& found) const {
    found = false;
    const JsonValue* name = params.find("name");
    if (name == nullptr) {
        return {};
    }
    const McpTool* tool = nullptr;
    for (const McpTool& candidate : tools_) {
        if (candidate.name == name->as_string()) {
            tool = &candidate;
        }
    }
    if (tool == nullptr || !tool->run) {
        return {};
    }
    found = true;
    static const JsonValue kNoArguments = JsonValue::object();
    const JsonValue* arguments = params.find("arguments");
    if (arguments == nullptr || !arguments->is_object()) {
        arguments = &kNoArguments;
    }
    JsonValue result = JsonValue::object();
    try {
        JsonValue value = tool->run(*arguments);
        result.set("content", TextContent(compact_json(value)));
        if (value.is_object()) {
            result.set("structuredContent", std::move(value));
        }
        result.set("isError", JsonValue::boolean(false));
    } catch (const std::exception& ex) {
        result.set("content", TextContent(ex.what()));
        result.set("isError", JsonValue::boolean(true));
    }
    return result;
}

}  // namespace ide
