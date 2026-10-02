#include "IdeResources.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <cerrno>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

extern char** environ;
#endif

namespace ide {
namespace {

namespace fs = std::filesystem;

fs::path ExecutableDirectory() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return fs::path(buffer).parent_path();
#elif defined(__APPLE__)
    std::string buffer(256, '\0');
    uint32_t size = static_cast<uint32_t>(buffer.size());
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        buffer.assign(size, '\0');
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
            return {};
        }
    }
    const std::size_t terminator = buffer.find('\0');
    if (terminator == std::string::npos) {
        return {};
    }
    buffer.resize(terminator);
    std::error_code error;
    const fs::path canonical = fs::weakly_canonical(buffer, error);
    return (error ? fs::path(buffer) : canonical).parent_path();
#else
    std::string buffer(256, '\0');
    for (;;) {
        const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            return {};
        }
        if (static_cast<std::size_t>(length) < buffer.size()) {
            buffer.resize(static_cast<std::size_t>(length));
            break;
        }
        buffer.resize(buffer.size() * 2);
    }
    return fs::path(buffer).parent_path();
#endif
}

bool IsFile(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error);
}

}  // namespace

fs::path executable_directory() { return ExecutableDirectory(); }

fs::path find_resource(const std::string& relative) {
    const fs::path exeDir = ExecutableDirectory();
    const fs::path tail = fs::path(relative);
    fs::path candidates[4];
    std::size_t count = 0;
    if (!exeDir.empty()) {
        // Mac bundle: the executable is Contents/MacOS, and resources/ from
        // the source tree is copied onto Contents/Resources.
        candidates[count++] = exeDir / ".." / "Resources" / tail;
    }
    candidates[count++] = fs::path("resources") / tail;
    if (!exeDir.empty()) {
        candidates[count++] = exeDir / "resources" / tail;
        candidates[count++] = exeDir / ".." / "resources" / tail;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (IsFile(candidates[i])) {
            return candidates[i];
        }
    }
    std::fprintf(stderr, "Could not find resource \"%s\". Looked for:\n", relative.c_str());
    for (std::size_t i = 0; i < count; ++i) {
        std::fprintf(stderr, "  %s\n", candidates[i].string().c_str());
    }
    return {};
}

fs::path path_from_utf8(const std::string& text) { return fs::u8path(text); }

fs::path config_directory() {
#if defined(_WIN32)
    wchar_t* appdata = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&appdata, &length, L"APPDATA") != 0 || appdata == nullptr) {
        return {};
    }
    const fs::path base(appdata);
    std::free(appdata);
    return base.empty() ? fs::path() : base / "AnarchyEngine";
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') {
        return {};
    }
    return fs::path(home) / "Library" / "Application Support" / "AnarchyEngine";
#else
    // XDG says a relative XDG_CONFIG_HOME is to be ignored.
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] == '/') {
        return fs::path(xdg) / "anarchy-engine";
    }
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') {
        return {};
    }
    return fs::path(home) / ".config" / "anarchy-engine";
#endif
}

bool reveal_folder(const fs::path& folder) {
#if defined(_WIN32)
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    // ShellExecute reports success as a value above 32.
    return reinterpret_cast<INT_PTR>(result) > 32;
#else
#if defined(__APPLE__)
    const char* tool = "open";
#else
    const char* tool = "xdg-open";
#endif
    std::string target = folder.string();
    char* argv[] = {const_cast<char*>(tool), target.data(), nullptr};
    pid_t pid = 0;
    if (posix_spawnp(&pid, tool, nullptr, nullptr, argv, environ) != 0) {
        return false;
    }
    // Reaped off the UI thread, so a slow opener never holds up a frame.
    std::thread([pid] {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }).detach();
    return true;
#endif
}

}  // namespace ide
