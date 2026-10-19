// AnarchyServer: serves one project, or the game packed into this program,
// to AnarchyPlayer clients until Ctrl+C. See README.md beside this file.

#include "ServerHost.hpp"
#include "runner/CommandLine.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <csignal>
#else
#include <csignal>
#endif

namespace {

namespace fs = std::filesystem;

// Set by Ctrl+C, SIGTERM, or the console closing; the main loop then stops the server.
std::atomic<bool> g_stop{false};
// Ctrl+C and the like, counted: the first stops the server cleanly, a second
// quits at once, for a server that is slow to stop.
std::atomic<int> g_signals{0};
// Set once the server has stopped, so a closing console's handler can return.
std::atomic<bool> g_finished{false};

// This program's own file, which holds a packed game when the studio exported one.
fs::path ExecutablePath() {
#if defined(_WIN32)
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            return fs::path(std::wstring(buffer.data(), length));
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    std::error_code failure;
    const fs::path resolved = fs::canonical(buffer.data(), failure);
    return failure ? fs::path(buffer.data()) : resolved;
#else
    std::error_code failure;
    const fs::path self = fs::read_symlink("/proc/self/exe", failure);
    return failure ? fs::path() : self;
#endif
}

#if defined(_WIN32)
// Ctrl+C and Ctrl+Break stop the server; a second one quits at once. Closing
// the console ends the process when this returns, so for that it waits (a few
// seconds at most) until the clients have been told.
BOOL WINAPI OnConsoleEvent(DWORD event) {
    if ((event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) && g_signals.fetch_add(1) >= 1) {
        ExitProcess(130);
    }
    g_stop.store(true);
    if (event == CTRL_CLOSE_EVENT || event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4500);
        while (!g_finished.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    return TRUE;
}
#else
// The first signal stops the server; a second quits at once.
void OnSignal(int) {
    if (g_signals.fetch_add(1) >= 1) {
        std::_Exit(130);
    }
    g_stop.store(true);
}
#endif

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    // Paths and script prints are UTF-8; the console shows them as such, and
    // gets its own code page back on every way out of main, Ctrl+C included.
    // A second Ctrl+C quits at once and leaves it as UTF-8.
    struct ConsoleCodePage {
        UINT previous = GetConsoleOutputCP();
        ~ConsoleCodePage() {
            if (previous != 0) {
                SetConsoleOutputCP(previous);
            }
        }
    } console_code_page;
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::vector<std::string> args = runner::utf8_arguments(argc, argv);
    if (!args.empty()) {
        args.erase(args.begin());
    }
    std::string error;
    std::optional<server::ServerOptions> options = server::parse_server_args(args, error);
    if (!options) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    if (options->project.empty()) {
        options->pack_file = ExecutablePath();
    }
    std::unique_ptr<server::ServerHost> host = server::ServerHost::open(*options, error);
    if (!host) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::printf("Serving %s on port %u\n", host->project_name().c_str(), static_cast<unsigned>(host->port()));
    std::fflush(stdout);
#if defined(_WIN32)
    SetConsoleCtrlHandler(OnConsoleEvent, TRUE);
#else
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
#endif
    host->start();
    while (!g_stop.load()) {
        host->print_output(stdout, stderr);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    host->stop();
    host->print_output(stdout, stderr);
    g_finished.store(true);
    return 0;
}
