#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ide {

// Whether the studio's MCP server runs, and what decided it.
struct McpSwitch {
    bool on = false;
    // The environment variable that decided, such as "ANARCHY_MCP". Empty when
    // the preference did, which Preferences' AI tab sets.
    std::string forced_by;
};

using EnvironmentLookup = std::function<std::optional<std::string>(const char* name)>;

// ANARCHY_MCP=0 turns the server off and any other value turns it on. Without
// it, ANARCHY_MCP_PORT or ANARCHY_MCP_TOKEN turns it on, since whoever set them
// means to connect. Otherwise the preference decides. An empty variable counts as unset.
McpSwitch decide_mcp(bool preferred, const EnvironmentLookup& env);

// The name the studio registers itself under in Claude Code.
inline constexpr const char* kClaudeServerName = "anarchy";

// The claude CLI: claude.exe, then claude.cmd, in each PATH folder on Windows,
// and claude elsewhere; then home/.local/bin, where the native installer puts
// it, and on Windows npm's folder under appdata. Empty when none is there.
// path_list is PATH as the environment holds it.
std::filesystem::path find_claude_cli(const std::string& path_list, const std::filesystem::path& home,
                                      const std::filesystem::path& appdata);
// The same, from this process's environment.
std::filesystem::path find_claude_cli();

// anarchy-mcp (anarchy-mcp.exe on Windows) in folder. Empty when it is not there.
std::filesystem::path find_bridge(const std::filesystem::path& folder);

// Arguments to the claude CLI that register the bridge for every folder the
// user works in, take it out again, and describe what is registered.
std::vector<std::string> claude_add_args(const std::filesystem::path& bridge);
std::vector<std::string> claude_remove_args();
std::vector<std::string> claude_get_args();
// What a person types to register the bridge, with paths quoted as a shell needs.
std::string claude_add_command(const std::filesystem::path& bridge);

enum class ClaudeRegistration {
    // Registered, running this bridge.
    Connected,
    // Registered, running some other program, such as an older build's bridge.
    Elsewhere,
    NotRegistered,
    // claude mcp get failed for another reason, such as an error before it looked.
    Unknown,
};

struct ClaudeStatus {
    ClaudeRegistration registration = ClaudeRegistration::Unknown;
    // The command it runs, as claude printed it. Empty unless registered.
    std::string command;
};

// What claude mcp get anarchy's exit code and output say of the bridge.
ClaudeStatus parse_claude_get(int exit_code, const std::string& output, const std::filesystem::path& bridge);

}  // namespace ide
