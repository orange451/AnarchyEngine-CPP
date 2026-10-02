#include "McpSetup.hpp"

#include "Environment.hpp"
#include "IdeResources.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <system_error>

namespace ide {
namespace {

namespace fs = std::filesystem;

#if defined(_WIN32)
constexpr char kPathSeparator = ';';
#else
constexpr char kPathSeparator = ':';
#endif

bool IsFile(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error);
}

// The CLI in folder, by the names it goes by there.
fs::path ClaudeIn(const fs::path& folder) {
    if (folder.empty()) {
        return {};
    }
#if defined(_WIN32)
    for (const char* name : {"claude.exe", "claude.cmd"}) {
#else
    for (const char* name : {"claude"}) {
#endif
        const fs::path candidate = folder / name;
        if (IsFile(candidate)) {
            return candidate;
        }
    }
    return {};
}

std::string Trim(std::string text) {
    const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!text.empty() && space(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    std::size_t start = 0;
    while (start < text.size() && space(static_cast<unsigned char>(text[start]))) {
        ++start;
    }
    return text.substr(start);
}

// A path as claude prints it and as the studio finds it, compared alike:
// forward slashes, and on Windows, any case.
std::string Comparable(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
#if defined(_WIN32)
    std::transform(path.begin(), path.end(), path.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return path;
}

// An environment variable as UTF-8, or empty.
std::string Variable(const char* name) {
#if defined(_WIN32)
    const std::wstring wide(name, name + std::char_traits<char>::length(name));
    wchar_t* value = nullptr;
    std::size_t size = 0;
    if (_wdupenv_s(&value, &size, wide.c_str()) != 0 || value == nullptr) {
        return {};
    }
    const std::string text = utf8_path(fs::path(value));
    std::free(value);
    return text;
#else
    return engine_core::environment_variable(name).value_or(std::string());
#endif
}

}  // namespace

McpSwitch decide_mcp(bool preferred, const EnvironmentLookup& env) {
    const auto set = [&env](const char* name) {
        const std::optional<std::string> value = env(name);
        return value && !value->empty() ? value : std::nullopt;
    };
    if (const std::optional<std::string> value = set("ANARCHY_MCP")) {
        return {*value != "0", "ANARCHY_MCP"};
    }
    for (const char* name : {"ANARCHY_MCP_PORT", "ANARCHY_MCP_TOKEN"}) {
        if (set(name)) {
            return {true, name};
        }
    }
    return {preferred, {}};
}

fs::path find_claude_cli(const std::string& path_list, const fs::path& home, const fs::path& appdata) {
    std::stringstream folders(path_list);
    std::string folder;
    while (std::getline(folders, folder, kPathSeparator)) {
        // Windows allows a PATH entry in quotes.
        if (folder.size() >= 2 && folder.front() == '"' && folder.back() == '"') {
            folder = folder.substr(1, folder.size() - 2);
        }
        if (folder.empty()) {
            continue;
        }
        if (fs::path found = ClaudeIn(path_from_utf8(folder)); !found.empty()) {
            return found;
        }
    }
    if (!home.empty()) {
        if (fs::path found = ClaudeIn(home / ".local" / "bin"); !found.empty()) {
            return found;
        }
    }
#if defined(_WIN32)
    if (!appdata.empty()) {
        if (fs::path found = ClaudeIn(appdata / "npm"); !found.empty()) {
            return found;
        }
    }
#else
    (void)appdata;
#endif
    return {};
}

fs::path find_claude_cli() {
    std::string path_list = Variable("PATH");
#if defined(_WIN32)
    const std::string home = Variable("USERPROFILE");
#else
    const std::string home = Variable("HOME");
    // An app opened from the Finder or a desktop menu gets a short PATH, without
    // the folders package managers put the CLI in.
    for (const char* folder : {"/opt/homebrew/bin", "/usr/local/bin"}) {
        path_list += std::string(1, kPathSeparator) + folder;
    }
#endif
    return find_claude_cli(path_list, home.empty() ? fs::path() : path_from_utf8(home),
                           path_from_utf8(Variable("APPDATA")));
}

fs::path find_bridge(const fs::path& folder) {
#if defined(_WIN32)
    const fs::path bridge = folder / "anarchy-mcp.exe";
#else
    const fs::path bridge = folder / "anarchy-mcp";
#endif
    return !folder.empty() && IsFile(bridge) ? bridge : fs::path();
}

std::vector<std::string> claude_add_args(const fs::path& bridge) {
    return {"mcp", "add", kClaudeServerName, "--scope", "user", "--", utf8_path(bridge)};
}

std::vector<std::string> claude_remove_args() { return {"mcp", "remove", kClaudeServerName, "--scope", "user"}; }

std::vector<std::string> claude_get_args() { return {"mcp", "get", kClaudeServerName}; }

std::string claude_add_command(const fs::path& bridge) {
    std::string command = "claude";
    for (const std::string& arg : claude_add_args(bridge)) {
        const bool plain = !arg.empty() && std::all_of(arg.begin(), arg.end(), [](unsigned char c) {
            return std::isalnum(c) != 0 || c == '/' || c == '\\' || c == ':' || c == '.' || c == '_' || c == '-';
        });
        command += ' ';
        command += plain ? arg : "\"" + arg + "\"";
    }
    return command;
}

ClaudeStatus parse_claude_get(int exit_code, const std::string& output, const fs::path& bridge) {
    ClaudeStatus status;
    if (exit_code != 0) {
        status.registration = output.find("No MCP server named") != std::string::npos ? ClaudeRegistration::NotRegistered
                                                                                     : ClaudeRegistration::Unknown;
        return status;
    }
    std::optional<std::string> command;
    std::string args;
    std::stringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        line = Trim(line);
        if (line.rfind("Command:", 0) == 0) {
            command = Trim(line.substr(8));
        } else if (line.rfind("Args:", 0) == 0) {
            args = Trim(line.substr(5));
        }
    }
    if (!command || command->empty()) {
        return status;
    }
    status.command = args.empty() ? *command : *command + " " + args;
    status.registration = args.empty() && Comparable(*command) == Comparable(utf8_path(bridge))
                              ? ClaudeRegistration::Connected
                              : ClaudeRegistration::Elsewhere;
    return status;
}

}  // namespace ide
