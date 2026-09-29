#include "ide/Pty.hpp"
#include "ide/TerminalScreen.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// Runs the user's shell as the terminal page would, types a command into it,
// and prints the screen it leaves: a check by hand that a real program draws,
// without opening the studio.
//   terminal-probe <seconds> <command...>
int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 5;
    std::string command;
    for (int i = 2; i < argc; ++i) {
        command += (command.empty() ? "" : " ") + std::string(argv[i]);
    }

    ide::TerminalScreen screen(30, 100);
    std::mutex mutex;
    std::string pending;
    bool exited = false;
    int exit_code = 0;

    ide::PtyOptions options = ide::DefaultShell();
    options.cols = screen.cols();
    options.rows = screen.rows();
    options.on_output = [&](std::string_view bytes) {
        std::lock_guard<std::mutex> lock(mutex);
        pending.append(bytes.data(), bytes.size());
    };
    options.on_exit = [&](int code) {
        std::lock_guard<std::mutex> lock(mutex);
        exited = true;
        exit_code = code;
    };
    std::string error;
    std::unique_ptr<ide::Pty> pty = ide::Pty::spawn(options, &error);
    if (!pty) {
        std::fprintf(stderr, "can't start %s: %s\n", options.program.c_str(), error.c_str());
        return 1;
    }
    screen.set_on_reply([&](std::string_view bytes) { pty->write(bytes); });

    const auto start = std::chrono::steady_clock::now();
    bool typed = command.empty();
    for (;;) {
        std::string bytes;
        bool done = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            bytes.swap(pending);
            done = exited;
        }
        screen.write(bytes);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        if (!typed && elapsed > std::chrono::milliseconds(1500)) {
            pty->write(command + "\r");
            typed = true;
        }
        if (done || elapsed > std::chrono::seconds(seconds)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::printf("---- %s, %d history rows, cursor %d,%d%s\n", options.program.c_str(), screen.scrollback_rows(),
                screen.cursor().row, screen.cursor().col, screen.alt_screen() ? ", alternate screen" : "");
    for (int row = -screen.scrollback_rows(); row < screen.rows(); ++row) {
        std::printf("%4d|%s\n", row, screen.row_text(row).c_str());
    }
    if (exited) {
        std::printf("---- exited with %d\n", exit_code);
    }
    // ANARCHY_PROBE_ROW=n also lists that row's cells and their colors.
    if (const char* at = std::getenv("ANARCHY_PROBE_ROW")) {
        const int row = std::atoi(at);
        for (int col = 0; col < screen.cols(); ++col) {
            const ide::TerminalCell cell = screen.cell(row, col);
            if (cell.chars.empty() && cell.bg.is_default) {
                continue;
            }
            std::printf("  col %d U+%04X fg %s#%02x%02x%02x bg %s#%02x%02x%02x%s%s\n", col,
                        cell.chars.empty() ? 0u : static_cast<unsigned>(cell.chars[0]), cell.fg.is_default ? "default " : "",
                        cell.fg.r, cell.fg.g, cell.fg.b, cell.bg.is_default ? "default " : "", cell.bg.r, cell.bg.g,
                        cell.bg.b, cell.bold ? " bold" : "", cell.reverse ? " reverse" : "");
        }
    }
    return 0;
}
