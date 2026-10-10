#include "AssetLoads.hpp"

#include <mutex>
#include <unordered_set>

namespace engine_core {

namespace {

std::mutex gMutex;
std::unordered_set<std::string> gLoaded;

std::string Key(AssetKind kind, const std::filesystem::path& root, const std::string& path) {
    return std::to_string(static_cast<int>(kind)) + '|' + root.lexically_normal().generic_u8string() + '|' + path;
}

}  // namespace

void set_asset_loaded(AssetKind kind, const std::filesystem::path& root, const std::string& path, bool loaded) {
    if (root.empty() || path.empty()) return;
    std::lock_guard<std::mutex> lock(gMutex);
    if (loaded) {
        gLoaded.insert(Key(kind, root, path));
    } else {
        gLoaded.erase(Key(kind, root, path));
    }
}

bool asset_loaded(AssetKind kind, const std::filesystem::path& root, const std::string& path) {
    if (root.empty() || path.empty()) return false;
    std::lock_guard<std::mutex> lock(gMutex);
    return gLoaded.count(Key(kind, root, path)) != 0;
}

void clear_asset_loads() {
    std::lock_guard<std::mutex> lock(gMutex);
    gLoaded.clear();
}

}  // namespace engine_core
