#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace ide {

struct ProcessResult {
    // False when it could not be started; error says why.
    bool started = false;
    // True when it ran past the timeout and was ended.
    bool timed_out = false;
    int exit_code = -1;
    // What it wrote to stdout and stderr, interleaved as it wrote them.
    std::string output;
    std::string error;
};

// Runs program with args and waits for it, up to timeout, with stdin empty and
// no console window. A Windows .cmd or .bat runs through cmd.exe, so an
// argument holding a character cmd.exe would read, such as % or &, is refused.
// What it starts is ended with it on a timeout. Blocks: call it off the UI thread.
ProcessResult run_process(const std::filesystem::path& program, const std::vector<std::string>& args,
                          std::chrono::milliseconds timeout);

// The command line Windows passes for program and args, each quoted as
// CommandLineToArgvW reads it back.
std::wstring windows_command_line(const std::filesystem::path& program, const std::vector<std::string>& args);

}  // namespace ide
