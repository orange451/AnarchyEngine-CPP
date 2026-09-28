#include "StudioRegistry.hpp"

#include "IdeResources.hpp"
#include "PropertyBag.hpp"

#include <algorithm>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <signal.h>
#include <unistd.h>
#endif

namespace ide {
namespace {

namespace fs = std::filesystem;
using engine_core::JsonValue;

fs::path EntryPath(const fs::path& dir, const StudioEntry& entry) {
    return dir / (std::to_string(entry.pid) + "-" + std::to_string(entry.port) + ".json");
}

bool ReadEntry(const fs::path& path, StudioEntry& out) {
    std::string text;
    std::string error;
    JsonValue value;
    if (!read_file(path, text, error) || !engine_core::parse_json(text, value, error) || !value.is_object()) {
        return false;
    }
    const JsonValue* pid = value.find("pid");
    const JsonValue* port = value.find("port");
    if (pid == nullptr || port == nullptr || !pid->is_number() || !port->is_number()) {
        return false;
    }
    out.pid = static_cast<long long>(pid->as_number());
    out.port = static_cast<int>(port->as_number());
    if (const JsonValue* project = value.find("project")) {
        out.project = project->as_string();
    }
    if (const JsonValue* root = value.find("root")) {
        out.root = root->as_string();
    }
    return out.pid > 0 && out.port > 0 && out.port < 65536;
}

}  // namespace

fs::path studio_registry_dir() {
    const fs::path config = config_directory();
    return config.empty() ? fs::path() : config / "studios";
}

long long current_pid() {
#if defined(_WIN32)
    return static_cast<long long>(GetCurrentProcessId());
#else
    return static_cast<long long>(getpid());
#endif
}

bool process_alive(long long pid) {
    if (pid <= 0) {
        return false;
    }
#if defined(_WIN32)
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr) {
        return false;
    }
    DWORD code = 0;
    const bool running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    CloseHandle(process);
    return running;
#else
    if (pid > static_cast<long long>(static_cast<pid_t>(~0u >> 1))) {
        return false;
    }
    // EPERM: the process is there, run by someone else.
    return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

bool write_studio(const fs::path& dir, const StudioEntry& entry, std::string& error) {
    JsonValue value = JsonValue::object();
    value.set("pid", JsonValue::number(static_cast<double>(entry.pid)));
    value.set("port", JsonValue::number(entry.port));
    value.set("project", JsonValue::string(entry.project));
    value.set("root", JsonValue::string(entry.root));
    return write_file(EntryPath(dir, entry), engine_core::write_json(value), error);
}

void remove_studio(const fs::path& dir, const StudioEntry& entry) {
    std::error_code ignored;
    fs::remove(EntryPath(dir, entry), ignored);
}

std::vector<StudioEntry> list_studios(const fs::path& dir) {
    std::vector<StudioEntry> studios;
    std::error_code error;
    fs::directory_iterator it(dir, error);
    if (error) {
        return studios;
    }
    for (const fs::directory_entry& file : it) {
        if (file.path().extension() != ".json") {
            continue;
        }
        StudioEntry entry;
        if (!ReadEntry(file.path(), entry)) {
            continue;
        }
        if (!process_alive(entry.pid)) {
            std::error_code ignored;
            fs::remove(file.path(), ignored);
            continue;
        }
        studios.push_back(std::move(entry));
    }
    std::sort(studios.begin(), studios.end(), [](const StudioEntry& a, const StudioEntry& b) {
        return a.pid != b.pid ? a.pid < b.pid : a.port < b.port;
    });
    return studios;
}

}  // namespace ide
