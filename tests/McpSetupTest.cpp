#include "ide/IdeResources.hpp"
#include "ide/McpSetup.hpp"
#include "ide/RunProcess.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

// Whether the MCP server runs, finding and driving the claude CLI, and running
// a program for its output. The run_process tests run this program itself as
// the child, through the --child flags main handles first.
namespace {

namespace fs = std::filesystem;

int gFailures = 0;
fs::path gSelf;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message.c_str());
        ++gFailures;
    }
}

void ExpectText(const std::string& got, const std::string& want, const std::string& message) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s: got [%s], want [%s]\n", message.c_str(), got.c_str(), want.c_str());
        ++gFailures;
    }
}

// A fresh folder under the system's temporary folder, removed when the test ends.
struct Scratch {
    fs::path root;
    Scratch() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() / ("anarchy-mcp-setup-test-" + std::to_string(stamp));
        fs::create_directories(root);
    }
    ~Scratch() {
        std::error_code error;
        fs::remove_all(root, error);
    }
};

void Touch(const fs::path& path) {
    std::error_code ignored;
    fs::create_directories(path.parent_path(), ignored);
    std::string error;
    Expect(ide::write_file(path, "", error), "a test file is written: " + error);
}

ide::EnvironmentLookup Env(std::map<std::string, std::string> values) {
    return [values](const char* name) -> std::optional<std::string> {
        const auto found = values.find(name);
        if (found == values.end()) {
            return std::nullopt;
        }
        return found->second;
    };
}

void TestDecide() {
    ide::McpSwitch got = ide::decide_mcp(false, Env({}));
    Expect(!got.on && got.forced_by.empty(), "off by default, by the preference");
    got = ide::decide_mcp(true, Env({}));
    Expect(got.on && got.forced_by.empty(), "the preference turns it on");
    got = ide::decide_mcp(true, Env({{"ANARCHY_MCP", "0"}}));
    Expect(!got.on, "ANARCHY_MCP=0 turns it off over the preference");
    ExpectText(got.forced_by, "ANARCHY_MCP", "and says so");
    got = ide::decide_mcp(false, Env({{"ANARCHY_MCP", "1"}}));
    Expect(got.on && got.forced_by == "ANARCHY_MCP", "ANARCHY_MCP=1 turns it on");
    got = ide::decide_mcp(false, Env({{"ANARCHY_MCP_PORT", "7790"}}));
    Expect(got.on && got.forced_by == "ANARCHY_MCP_PORT", "a pinned port turns it on");
    got = ide::decide_mcp(false, Env({{"ANARCHY_MCP_TOKEN", "secret"}}));
    Expect(got.on && got.forced_by == "ANARCHY_MCP_TOKEN", "a fixed token turns it on");
    got = ide::decide_mcp(false, Env({{"ANARCHY_MCP", "0"}, {"ANARCHY_MCP_PORT", "7790"}}));
    Expect(!got.on && got.forced_by == "ANARCHY_MCP", "ANARCHY_MCP decides before the port");
    got = ide::decide_mcp(false, Env({{"ANARCHY_MCP", ""}, {"ANARCHY_MCP_TOKEN", ""}}));
    Expect(!got.on && got.forced_by.empty(), "empty variables count as unset");
}

void TestFindClaude() {
    Scratch scratch;
    const fs::path home = scratch.root / "home";
    const fs::path appdata = scratch.root / "appdata";
    const fs::path first = scratch.root / "first";
    const fs::path second = scratch.root / "second";
#if defined(_WIN32)
    const char separator = ';';
    const std::string exe = "claude.exe";
#else
    const char separator = ':';
    const std::string exe = "claude";
#endif
    const std::string path_list = ide::utf8_path(first) + separator + ide::utf8_path(second);
    Expect(ide::find_claude_cli(path_list, home, appdata).empty(), "no claude anywhere finds none");

    Touch(home / ".local" / "bin" / exe);
    Expect(ide::find_claude_cli(path_list, home, appdata) == home / ".local" / "bin" / exe,
           "the native installer's folder is a fallback");
    Touch(second / exe);
    Expect(ide::find_claude_cli(path_list, home, appdata) == second / exe, "PATH comes first");
    Touch(first / exe);
    Expect(ide::find_claude_cli(path_list, home, appdata) == first / exe, "in PATH's order");
#if defined(_WIN32)
    Scratch npm;
    const fs::path npm_appdata = npm.root / "appdata";
    Touch(npm_appdata / "npm" / "claude.cmd");
    Expect(ide::find_claude_cli("", npm.root / "home", npm_appdata) == npm_appdata / "npm" / "claude.cmd",
           "npm's claude.cmd is found too");
    Touch(npm.root / "both" / "claude.cmd");
    Touch(npm.root / "both" / "claude.exe");
    Expect(ide::find_claude_cli(ide::utf8_path(npm.root / "both"), {}, {}) == npm.root / "both" / "claude.exe",
           "claude.exe before claude.cmd in one folder");
#endif
}

void TestBridge() {
    Scratch scratch;
    Expect(ide::find_bridge(scratch.root).empty(), "no bridge in an empty folder");
#if defined(_WIN32)
    const fs::path bridge = scratch.root / "anarchy-mcp.exe";
#else
    const fs::path bridge = scratch.root / "anarchy-mcp";
#endif
    Touch(bridge);
    Expect(ide::find_bridge(scratch.root) == bridge, "the bridge beside the studio is found");
}

std::string Joined(const std::vector<std::string>& args) {
    std::string out;
    for (const std::string& arg : args) {
        out += (out.empty() ? "" : "|") + arg;
    }
    return out;
}

void TestCommands() {
    const fs::path bridge = ide::path_from_utf8("C:/Program Files/Anarchy/anarchy-mcp.exe");
    ExpectText(Joined(ide::claude_add_args(bridge)),
               "mcp|add|anarchy|--scope|user|--|" + ide::utf8_path(bridge), "add registers for every folder");
    ExpectText(Joined(ide::claude_remove_args()), "mcp|remove|anarchy|--scope|user", "remove takes out the user's");
    ExpectText(Joined(ide::claude_get_args()), "mcp|get|anarchy", "get describes it");
    ExpectText(ide::claude_add_command(bridge),
               "claude mcp add anarchy --scope user -- \"" + ide::utf8_path(bridge) + "\"",
               "the typed command quotes a path with spaces");
    ExpectText(ide::claude_add_command(ide::path_from_utf8("/opt/anarchy/anarchy-mcp")),
               "claude mcp add anarchy --scope user -- " + ide::utf8_path(ide::path_from_utf8("/opt/anarchy/anarchy-mcp")),
               "and leaves a plain one bare");
}

void TestParseGet() {
    const fs::path bridge = ide::path_from_utf8("C:/Games/Anarchy/anarchy-mcp.exe");
    ide::ClaudeStatus status = ide::parse_claude_get(
        1, "No MCP server named \"anarchy\". Configured servers: Roblox_Studio\n", bridge);
    Expect(status.registration == ide::ClaudeRegistration::NotRegistered, "no server named anarchy is not registered");

    const std::string registered =
        "anarchy:\n"
        "  Scope: User config (available in all your projects)\n"
        "  Status: \xE2\x9C\x94 Connected\n"
        "  Type: stdio\n"
        "  Command: C:\\Games\\Anarchy\\anarchy-mcp.exe\n"
        "  Args: \n"
        "  Environment:\n"
        "\n"
        "To remove this server, run: claude mcp remove anarchy -s user\n";
    status = ide::parse_claude_get(0, registered, bridge);
    Expect(status.registration == ide::ClaudeRegistration::Connected, "the same bridge, either slash, is connected");
    ExpectText(status.command, "C:\\Games\\Anarchy\\anarchy-mcp.exe", "and its command is kept");

#if defined(_WIN32)
    status = ide::parse_claude_get(0, "anarchy:\n  Command: c:/games/anarchy/ANARCHY-MCP.exe\r\n  Args: \r\n", bridge);
    Expect(status.registration == ide::ClaudeRegistration::Connected, "case does not matter on Windows");
#endif
    status = ide::parse_claude_get(0, "anarchy:\n  Command: C:/Old/build/anarchy-mcp.exe\n  Args: \n", bridge);
    Expect(status.registration == ide::ClaudeRegistration::Elsewhere, "another bridge is elsewhere");
    ExpectText(status.command, "C:/Old/build/anarchy-mcp.exe", "naming it");
    status = ide::parse_claude_get(0, "anarchy:\n  Command: C:/Games/Anarchy/anarchy-mcp.exe\n  Args: --project Foo\n", bridge);
    Expect(status.registration == ide::ClaudeRegistration::Elsewhere, "the bridge with arguments is not ours as is");
    ExpectText(status.command, "C:/Games/Anarchy/anarchy-mcp.exe --project Foo", "its arguments are shown");
    status = ide::parse_claude_get(2, "error: unknown command 'mcp'\n", bridge);
    Expect(status.registration == ide::ClaudeRegistration::Unknown, "another failure is unknown");
}

void TestCommandLine() {
#if defined(_WIN32)
    // Backslashes are literal except before a quote, so only those are doubled.
    const std::wstring line = ide::windows_command_line(ide::path_from_utf8("C:/Program Files/x.exe"),
                                                        {"plain", "has space", "", "quote\"d", "trail\\", "a\\b c\\"});
    Expect(line == L"\"C:/Program Files/x.exe\" plain \"has space\" \"\" \"quote\\\"d\" trail\\ \"a\\b c\\\\\"",
           "arguments are quoted as CommandLineToArgvW reads them");
#endif
}

void TestRunProcess() {
    ide::ProcessResult result =
        ide::run_process(gSelf, {"--child-echo", "two words", "", "q\"uote"}, std::chrono::seconds(20));
    Expect(result.started, "the child starts: " + result.error);
    ExpectText(std::to_string(result.exit_code), "7", "its exit code comes back");
#if defined(_WIN32)
    const std::string newline = "\r\n";
#else
    const std::string newline = "\n";
#endif
    ExpectText(result.output, "[two words][][q\"uote]err" + newline, "arguments arrive whole, stdout and stderr both");

    const auto begin = std::chrono::steady_clock::now();
    result = ide::run_process(gSelf, {"--child-sleep"}, std::chrono::milliseconds(300));
    const auto took = std::chrono::steady_clock::now() - begin;
    Expect(result.started && result.timed_out, "a child past its timeout is ended");
    Expect(took < std::chrono::seconds(5), "without waiting it out");

    result = ide::run_process(gSelf.parent_path() / "no-such-program", {}, std::chrono::seconds(5));
    Expect(!result.started && !result.error.empty(), "a missing program does not start, and says why");

#if defined(_WIN32)
    Scratch scratch;
    const fs::path script = scratch.root / "echo args.cmd";
    std::string error;
    Expect(ide::write_file(script, "@echo [%~1][%~2]\r\n@exit /b 3\r\n", error), "a .cmd is written");
    result = ide::run_process(script, {"C:/Program Files/a b", "x"}, std::chrono::seconds(20));
    Expect(result.started, "a .cmd runs through cmd.exe: " + result.error);
    ExpectText(std::to_string(result.exit_code), "3", "with its exit code");
    ExpectText(result.output, "[C:/Program Files/a b][x]\r\n", "and its quoted arguments");
    result = ide::run_process(script, {"%PATH%"}, std::chrono::seconds(20));
    Expect(!result.started && !result.error.empty(), "an argument cmd.exe would expand is refused");
#endif
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--child-echo") == 0) {
        for (int index = 2; index < argc; ++index) {
            std::printf("[%s]", argv[index]);
        }
        std::fflush(stdout);
        std::fprintf(stderr, "err\n");
        return 7;
    }
    if (argc >= 2 && std::strcmp(argv[1], "--child-sleep") == 0) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
    std::error_code ignored;
    gSelf = fs::absolute(argv[0], ignored);
#if defined(_WIN32)
    if (gSelf.extension() != ".exe") {
        gSelf += ".exe";
    }
#endif
    TestDecide();
    TestFindClaude();
    TestBridge();
    TestCommands();
    TestParseGet();
    TestCommandLine();
    TestRunProcess();
    if (gFailures == 0) {
        std::printf("mcp-setup-tests: all passed\n");
    }
    return gFailures == 0 ? 0 : 1;
}
