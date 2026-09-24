#include "IdeIcons.hpp"

#include "jadefx/jadefx.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>

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
#else
#include <unistd.h>
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

// ClassName.png when that file exists. These classes have no file of that name.
const char* IconFile(const std::string& class_name) {
    if (class_name == "DataModel") {
        return "World.png";
    }
    if (class_name == "ModuleScript") {
        return "Script.png";
    }
    if (class_name == "TestTriangle") {
        return "Mesh.png";
    }
    return nullptr;
}

fs::path FindIcon(const std::string& filename) {
    const fs::path exeDir = ExecutableDirectory();
    fs::path candidates[4];
    std::size_t count = 0;
    if (!exeDir.empty()) {
        // Mac bundle: the executable is Contents/MacOS, and resources/ from
        // the source tree is copied onto Contents/Resources.
        candidates[count++] = exeDir / ".." / "Resources" / "icons" / filename;
    }
    candidates[count++] = fs::path("resources") / "icons" / filename;
    if (!exeDir.empty()) {
        candidates[count++] = exeDir / "resources" / "icons" / filename;
        candidates[count++] = exeDir / ".." / "resources" / "icons" / filename;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (IsFile(candidates[i])) {
            return candidates[i];
        }
    }
    std::fprintf(stderr, "Could not find icon \"%s\". Looked for:\n", filename.c_str());
    for (std::size_t i = 0; i < count; ++i) {
        std::fprintf(stderr, "  %s\n", candidates[i].string().c_str());
    }
    return {};
}

std::string Utf8(const fs::path& path) {
    const auto utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

std::shared_ptr<jadefx::Image> IconImage(const std::string& filename) {
    static std::unordered_map<std::string, std::shared_ptr<jadefx::Image>> cache;
    const auto found = cache.find(filename);
    if (found != cache.end()) {
        return found->second;
    }
    std::shared_ptr<jadefx::Image> image;
    const fs::path path = FindIcon(filename);
    if (!path.empty()) {
        image = jadefx::Image::load(Utf8(path));
        if (!image) {
            std::fprintf(stderr, "Could not decode icon %s\n", path.string().c_str());
        }
    }
    cache.emplace(filename, image);
    return image;
}

}  // namespace

std::shared_ptr<jadefx::ImageView> icon_view(const std::string& class_name) {
    if (class_name.empty()) {
        return nullptr;
    }
    const char* aliased = IconFile(class_name);
    const std::string filename = aliased != nullptr ? aliased : class_name + ".png";
    const std::shared_ptr<jadefx::Image> image = IconImage(filename);
    if (!image) {
        return nullptr;
    }
    return jadefx::make<jadefx::ImageView>(image);
}

}  // namespace ide
