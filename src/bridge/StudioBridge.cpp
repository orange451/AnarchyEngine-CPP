#include "StudioBridge.hpp"

#include "ide/IdeResources.hpp"

#include "httplib.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace bridge {
namespace {

namespace fs = std::filesystem;
using engine_core::JsonValue;
using ide::StudioEntry;

constexpr const char* kSeveral =
    " Several studios may be open, each with its own place. A tool call goes to the studio list_studios marks "
    "selected: the one picked with select_studio, else the only one open, else the one whose project folder holds "
    "the working directory. When none is selected, a call fails and names the open studios; pick one with "
    "select_studio by project name, folder, or pid. get_studio_info says which studio answers.";

// Resolves what it can, so /tmp and /private/tmp, or a trailing slash, compare equal.
fs::path Normal(const fs::path& path) {
    std::error_code error;
    fs::path out = fs::weakly_canonical(path, error);
    if (error) {
        out = path.lexically_normal();
    }
    if (!out.has_filename() && out.has_relative_path()) {
        out = out.parent_path();
    }
    return out;
}

// How many components deep root is, when dir is root or inside it. -1 otherwise.
int Depth(const fs::path& dir, const fs::path& root) {
    int depth = 0;
    auto at = dir.begin();
    for (const fs::path& part : root) {
        if (at == dir.end() || *at != part) {
            return -1;
        }
        ++at;
        ++depth;
    }
    return depth;
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string Listing(const std::vector<StudioEntry>& studios) {
    std::string out;
    for (const StudioEntry& studio : studios) {
        out += out.empty() ? "" : "; ";
        out += describe_studio(studio);
    }
    return out;
}

bool Same(const StudioEntry& a, const StudioEntry& b) { return a.pid == b.pid && a.port == b.port; }

JsonValue EntryJson(const StudioEntry& studio) {
    JsonValue out = JsonValue::object();
    out.set("project", JsonValue::string(studio.project));
    out.set("root", JsonValue::string(studio.root));
    out.set("pid", JsonValue::number(static_cast<double>(studio.pid)));
    out.set("port", JsonValue::number(studio.port));
    return out;
}

}  // namespace

std::string describe_studio(const StudioEntry& studio) {
    return (studio.project.empty() ? std::string("Untitled") : studio.project) + " (" +
           (studio.root.empty() ? std::string("no folder") : studio.root) + ", pid " + std::to_string(studio.pid) +
           ")";
}

StudioBridge::StudioBridge(BridgeOptions options, const std::vector<ide::McpTool>& catalog)
    : options_(std::move(options)) {
    front_.set_instructions(std::string(ide::default_instructions()) + kSeveral);
    for (const ide::McpTool& tool : catalog) {
        const std::string name = tool.name;
        front_.add_tool({tool.name, tool.description, tool.input_schema,
                         [this, name](const JsonValue& arguments) { return forward(target(), name, arguments); }});
    }
    front_.add_tool({"list_studios",
                     "Lists the open Anarchy Engine studios: each one's project name, folder, pid, and port, and "
                     "which one tool calls go to now (selected) and why (selected_by).",
                     ide::json_literal(R"({"type":"object","properties":{}})"),
                     [this](const JsonValue&) { return list_tool(); }});
    front_.add_tool({"select_studio",
                     "Sends the tool calls that follow to one studio, until it closes. Name it by project name "
                     "(any case), project folder, or pid. An empty string drops the choice.",
                     ide::json_literal(R"({"type":"object","required":["studio"],"properties":{
                         "studio":{"type":["string","number"],"description":"Project name, folder, or pid."}}})"),
                     [this](const JsonValue& arguments) { return select_tool(arguments); }});
}

StudioBridge::Pick StudioBridge::pick(const std::vector<StudioEntry>& studios) const {
    Pick out;
    {
        std::lock_guard<std::mutex> guard(mu_);
        if (chosen_) {
            for (const StudioEntry& studio : studios) {
                if (Same(studio, *chosen_)) {
                    out.studio = studio;
                    out.why = "select_studio";
                    return out;
                }
            }
            out.why = "The studio picked with select_studio, " + describe_studio(*chosen_) + ", has closed." +
                      (studios.empty() ? std::string() : " Open studios: " + Listing(studios) + ".") +
                      " Call select_studio to pick another, or with \"\" to go back to the usual order.";
            return out;
        }
    }
    if (studios.empty()) {
        out.why = "No Anarchy Engine studio is open. Open one, then try again.";
        return out;
    }
    if (!options_.pinned.empty()) {
        const std::vector<StudioEntry> matches = matching(studios, options_.pinned);
        if (matches.size() == 1) {
            out.studio = matches.front();
            out.why = "--project";
        } else if (matches.empty()) {
            out.why = "No open studio matches --project " + options_.pinned + ". Open studios: " + Listing(studios) +
                      ". Call select_studio to pick one.";
        } else {
            out.why = "--project " + options_.pinned + " matches several studios: " + Listing(matches) +
                      ". Call select_studio with one's pid.";
        }
        return out;
    }
    if (studios.size() == 1) {
        out.studio = studios.front();
        out.why = "only one open";
        return out;
    }
    const fs::path cwd = Normal(options_.cwd);
    int best = -1;
    std::vector<StudioEntry> deepest;
    for (const StudioEntry& studio : studios) {
        if (studio.root.empty() || options_.cwd.empty()) {
            continue;
        }
        const int depth = Depth(cwd, Normal(ide::path_from_utf8(studio.root)));
        if (depth < 0 || depth < best) {
            continue;
        }
        if (depth > best) {
            best = depth;
            deepest.clear();
        }
        deepest.push_back(studio);
    }
    if (deepest.size() == 1) {
        out.studio = deepest.front();
        out.why = "working directory";
        return out;
    }
    if (deepest.size() > 1) {
        out.why = "The working directory is in a project open in several studios: " + Listing(deepest) +
                  ". Call select_studio with one's pid.";
        return out;
    }
    out.why = std::to_string(studios.size()) + " studios are open: " + Listing(studios) +
              ". Call select_studio with a project name, folder, or pid.";
    return out;
}

StudioEntry StudioBridge::target() const {
    const Pick picked = pick(ide::list_studios(options_.registry));
    if (!picked.studio) {
        throw std::runtime_error(picked.why);
    }
    return *picked.studio;
}

JsonValue StudioBridge::forward(const StudioEntry& studio, const std::string& tool, const JsonValue& arguments) const {
    httplib::Client client("127.0.0.1", studio.port);
    client.set_connection_timeout(2);
    client.set_write_timeout(10);
    // A tool may wait on the studio: a play step, a UI round trip, a long chunk of Luau.
    client.set_read_timeout(120);
    JsonValue params = JsonValue::object();
    params.set("name", JsonValue::string(tool));
    params.set("arguments", arguments);
    JsonValue request = JsonValue::object();
    request.set("jsonrpc", JsonValue::string("2.0"));
    request.set("id", JsonValue::number(1));
    request.set("method", JsonValue::string("tools/call"));
    request.set("params", std::move(params));
    httplib::Headers headers;
    if (!options_.token.empty()) {
        headers.emplace("Authorization", "Bearer " + options_.token);
    }
    const httplib::Result reply = client.Post("/mcp", headers, ide::compact_json(request), "application/json");
    if (!reply) {
        throw std::runtime_error("Could not reach " + describe_studio(studio) + " on port " +
                                 std::to_string(studio.port) + ": " + httplib::to_string(reply.error()) + ".");
    }
    if (reply->status == 401) {
        throw std::runtime_error(describe_studio(studio) +
                                 " refused the call: its ANARCHY_MCP_TOKEN and the bridge's do not match.");
    }
    if (reply->status != 200) {
        throw std::runtime_error(describe_studio(studio) + " answered HTTP " + std::to_string(reply->status) + ".");
    }
    JsonValue answer;
    std::string error;
    if (!engine_core::parse_json(reply->body, answer, error)) {
        throw std::runtime_error(describe_studio(studio) + " sent a reply that is not JSON: " + error);
    }
    if (const JsonValue* failure = answer.find("error")) {
        const JsonValue* message = failure->find("message");
        throw std::runtime_error(message != nullptr ? message->as_string() : ide::compact_json(*failure));
    }
    const JsonValue* result = answer.find("result");
    if (result == nullptr) {
        throw std::runtime_error(describe_studio(studio) + " sent no result.");
    }
    const JsonValue* content = result->find("content");
    const std::string text = content != nullptr && !content->items().empty() && content->items()[0].find("text")
                                 ? content->items()[0].find("text")->as_string()
                                 : std::string();
    const JsonValue* failed = result->find("isError");
    if (failed != nullptr && failed->as_bool()) {
        throw std::runtime_error(text);
    }
    JsonValue value;
    if (const JsonValue* structured = result->find("structuredContent")) {
        value = *structured;
    } else if (!engine_core::parse_json(text, value, error)) {
        return JsonValue::string(text);
    }
    // An image rides beside the text. Put it back where this side's server looks for it.
    if (content == nullptr || !value.is_object()) {
        return value;
    }
    for (const JsonValue& item : content->items()) {
        const JsonValue* type = item.find("type");
        const JsonValue* data = item.find("data");
        const JsonValue* mime = item.find("mimeType");
        if (type != nullptr && type->is_string() && type->as_string() == "image" && data != nullptr &&
            mime != nullptr) {
            JsonValue image = JsonValue::object();
            image.set("data", *data);
            image.set("mimeType", *mime);
            value.set(ide::kImageMember, std::move(image));
            break;
        }
    }
    return value;
}

std::vector<StudioEntry> StudioBridge::matching(const std::vector<StudioEntry>& studios, const std::string& query) const {
    std::vector<StudioEntry> out;
    if (query.empty()) {
        return out;
    }
    const bool digits = std::all_of(query.begin(), query.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    fs::path folder = ide::path_from_utf8(query);
    if (folder.is_relative() && !options_.cwd.empty()) {
        folder = options_.cwd / folder;
    }
    folder = Normal(folder);
    const std::string name = Lower(query);
    for (const StudioEntry& studio : studios) {
        const bool by_pid = digits && std::to_string(studio.pid) == query;
        const bool by_name = Lower(studio.project) == name;
        const bool by_folder = !studio.root.empty() && Normal(ide::path_from_utf8(studio.root)) == folder;
        if (by_pid || by_name || by_folder) {
            out.push_back(studio);
        }
    }
    return out;
}

JsonValue StudioBridge::list_tool() {
    const std::vector<StudioEntry> studios = ide::list_studios(options_.registry);
    const Pick picked = pick(studios);
    JsonValue list = JsonValue::array();
    for (const StudioEntry& studio : studios) {
        JsonValue entry = EntryJson(studio);
        entry.set("selected", JsonValue::boolean(picked.studio && Same(*picked.studio, studio)));
        list.items().push_back(std::move(entry));
    }
    JsonValue out = JsonValue::object();
    out.set("studios", std::move(list));
    if (picked.studio) {
        out.set("selected_by", JsonValue::string(picked.why));
    } else {
        out.set("selected_by", JsonValue());
        out.set("problem", JsonValue::string(picked.why));
    }
    return out;
}

JsonValue StudioBridge::select_tool(const JsonValue& arguments) {
    const JsonValue* studio = arguments.find("studio");
    if (studio == nullptr || !(studio->is_string() || studio->is_number())) {
        throw std::runtime_error("studio must be a project name, folder, or pid.");
    }
    const std::string query = studio->is_number() ? std::to_string(static_cast<long long>(studio->as_number()))
                                                  : studio->as_string();
    if (query.empty()) {
        {
            std::lock_guard<std::mutex> guard(mu_);
            chosen_.reset();
        }
        return list_tool();
    }
    const std::vector<StudioEntry> studios = ide::list_studios(options_.registry);
    const std::vector<StudioEntry> matches = matching(studios, query);
    if (matches.empty()) {
        throw std::runtime_error("No open studio matches " + query + "." +
                                 (studios.empty() ? std::string(" No studio is open.")
                                                  : " Open studios: " + Listing(studios) + "."));
    }
    if (matches.size() > 1) {
        throw std::runtime_error(query + " matches several studios: " + Listing(matches) + ". Pick one by pid.");
    }
    {
        std::lock_guard<std::mutex> guard(mu_);
        chosen_ = matches.front();
    }
    JsonValue out = JsonValue::object();
    out.set("selected", EntryJson(matches.front()));
    return out;
}

}  // namespace bridge
