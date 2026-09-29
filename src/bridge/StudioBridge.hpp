#pragma once

#include "ide/McpServer.hpp"
#include "ide/McpTools.hpp"
#include "ide/StudioRegistry.hpp"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace bridge {

struct BridgeOptions {
    // The studio registry folder, where each open studio names its port and project.
    std::filesystem::path registry;
    // Where the client started the bridge. A studio whose project folder holds it is the default.
    std::filesystem::path cwd;
    // --project: a name, folder, or pid that picks the studio instead.
    std::string pinned;
    // Sent as "Authorization: Bearer <token>" when not empty.
    std::string token;
};

// One MCP server in front of every open studio. The client registers this
// once; each tool call goes to one studio, picked in this order:
//   1. the studio select_studio picked, until it closes;
//   2. the studio --project names;
//   3. the only studio open;
//   4. the studio whose project folder holds the working directory, the deepest one.
// Otherwise the call fails and names the open studios. Once the picked studio
// closes, every call fails until select_studio picks again, so a retry never
// edits a place other than the one picked.
//
// The tools are the studio's own, from catalog, plus list_studios and select_studio.
class StudioBridge {
public:
    StudioBridge(BridgeOptions options, const std::vector<ide::McpToolSpec>& catalog);

    StudioBridge(const StudioBridge&) = delete;
    StudioBridge& operator=(const StudioBridge&) = delete;

    // Handles the protocol; only its handle is used, never start.
    const ide::McpServer& server() const { return front_; }

private:
    struct Pick {
        std::optional<ide::StudioEntry> studio;
        // How it was picked, or why none was.
        std::string why;
    };

    Pick pick(const std::vector<ide::StudioEntry>& studios) const;
    // The studio a call goes to. Throws with the reason when there is none.
    ide::StudioEntry target() const;
    engine_core::JsonValue forward(const ide::StudioEntry& studio, const std::string& tool,
                                   const engine_core::JsonValue& arguments) const;
    std::vector<ide::StudioEntry> matching(const std::vector<ide::StudioEntry>& studios, const std::string& query) const;
    engine_core::JsonValue list_tool();
    engine_core::JsonValue select_tool(const engine_core::JsonValue& arguments);

    BridgeOptions options_;
    ide::McpServer front_;
    mutable std::mutex mu_;
    std::optional<ide::StudioEntry> chosen_;
};

// "Alpha (/path/to/Alpha, pid 123)", or "Untitled (no folder, pid 123)".
std::string describe_studio(const ide::StudioEntry& studio);

}  // namespace bridge
