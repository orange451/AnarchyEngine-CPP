#include "UserData.hpp"

#include "Environment.hpp"

#include <cstdlib>

namespace engine_core {

namespace fs = std::filesystem;

fs::path user_data_directory() {
    if (const auto chosen = environment_variable("ANARCHY_USER_DATA"); chosen && !chosen->empty()) {
        return fs::u8path(*chosen);
    }
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

}  // namespace engine_core
