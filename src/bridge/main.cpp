// anarchy-mcp: the MCP server an LLM client runs over stdio. It forwards each
// tool call to one open studio, found through the studio registry.
//
//   claude mcp add anarchy -s user -- /path/to/anarchy-mcp [--project <name|folder|pid>]

#include "Environment.hpp"
#include "StudioBridge.hpp"
#include "ide/McpTools.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {

constexpr const char* kUsage =
    "usage: anarchy-mcp [--project <name|folder|pid>]\n"
    "Speaks MCP on stdin and stdout, and sends each tool call to one open Anarchy Engine studio:\n"
    "the one select_studio picked, else the one --project names, else the only one open,\n"
    "else the one whose project folder holds the working directory.\n"
    "Each studio's token comes from its registry entry. ANARCHY_MCP_TOKEN is sent\n"
    "to a studio whose entry has none.\n";

}  // namespace

int main(int argc, char** argv) {
    bridge::BridgeOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--project" && index + 1 < argc) {
            options.pinned = argv[++index];
        } else if (arg.rfind("--project=", 0) == 0) {
            options.pinned = arg.substr(10);
        } else if (arg == "--help" || arg == "-h") {
            std::fputs(kUsage, stdout);
            return 0;
        } else {
            std::fprintf(stderr, "anarchy-mcp: unknown argument %s\n%s", arg.c_str(), kUsage);
            return 2;
        }
    }
#if defined(_WIN32)
    // One JSON-RPC message per line, ended by \n alone.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    options.registry = ide::studio_registry_dir();
    if (options.registry.empty()) {
        std::fputs("anarchy-mcp: no home folder, so no studio registry to read.\n", stderr);
    }
    std::error_code ignored;
    options.cwd = std::filesystem::current_path(ignored);
    if (std::optional<std::string> token = engine_core::environment_variable("ANARCHY_MCP_TOKEN")) {
        options.token = std::move(*token);
    }

    // The studio's own tool definitions, so the list is whole even before a studio opens.
    bridge::StudioBridge bridge(options, ide::engine_tool_specs());

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.find_first_not_of(" \t") == std::string::npos) {
            continue;
        }
        int status = 0;
        const std::string reply = bridge.server().handle(line, status);
        if (!reply.empty()) {
            std::cout << reply << '\n' << std::flush;
        }
    }
    return 0;
}
