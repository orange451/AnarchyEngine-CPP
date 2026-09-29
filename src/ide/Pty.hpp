#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ide {

struct PtyOptions {
    // Found on PATH when it has no folder in it.
    std::string program;
    // After the program's own name.
    std::vector<std::string> args;
    // Empty starts in this process's folder.
    std::string cwd;
    int cols = 80;
    int rows = 24;
    // Added to this process's environment, replacing a variable of the same name.
    std::vector<std::pair<std::string, std::string>> env;
    // Both run on the pty's reader thread. on_exit comes after the last output.
    std::function<void(std::string_view)> on_output;
    std::function<void(int exit_code)> on_exit;
};

// A program running on a pseudo-terminal: ConPTY on Windows and forkpty
// elsewhere, so it sees a terminal and not a pipe. Destroying it ends the
// program and joins the reader thread; on_exit is not called then.
class Pty {
public:
    // Null with *error set when the program could not start. On macOS and
    // Linux a missing program is found after the fork, and exits with 127.
    static std::unique_ptr<Pty> spawn(PtyOptions options, std::string* error);

    virtual ~Pty() = default;
    // Bytes the program reads, as typed.
    virtual void write(std::string_view bytes) = 0;
    virtual void resize(int cols, int rows) = 0;
};

// The user's shell, started as a person at a terminal would start it: a login
// shell on macOS and Linux, and PowerShell (or cmd) on Windows. TERM and
// COLORTERM say what the terminal page can show.
PtyOptions DefaultShell();

}  // namespace ide
