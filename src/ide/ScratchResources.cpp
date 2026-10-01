#include "ScratchResources.hpp"

#include "IdeResources.hpp"

#include <chrono>
#include <random>
#include <system_error>
#include <vector>

namespace ide {
namespace {

namespace fs = std::filesystem;

// The names new_scratch_resources puts under the temporary folder.
constexpr const char* kStudioFolder = "AnarchyEngine";
constexpr const char* kUntitledPrefix = "untitled-";
constexpr const char* kResourcesFolder = "resources";

}  // namespace

fs::path new_scratch_resources() {
    std::error_code failure;
    const fs::path temp = fs::temp_directory_path(failure);
    if (failure || temp.empty()) {
        return {};
    }
    // The time keeps names in order; the random part keeps two studios started together apart.
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    std::random_device random;
    const std::string name = kUntitledPrefix + std::to_string(stamp) + "-" + std::to_string(random() % 1000000u);
    return temp / kStudioFolder / name / kResourcesFolder;
}

bool is_scratch_resources(const fs::path& path) {
    const fs::path untitled = path.parent_path();
    return path.filename() == kResourcesFolder && utf8_path(untitled.filename()).rfind(kUntitledPrefix, 0) == 0 &&
           untitled.parent_path().filename() == kStudioFolder;
}

bool move_scratch_resources(const fs::path& scratch, const fs::path& resources, std::string& error) {
    std::error_code failure;
    // Listed first, since moving files out while iterating can skip some.
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(scratch, failure), end; !failure && it != end; it.increment(failure)) {
        if (it->is_regular_file(failure)) {
            files.push_back(it->path());
        }
    }
    for (const fs::path& file : files) {
        const fs::path target = resources / file.lexically_relative(scratch);
        if (fs::exists(target, failure)) {
            continue;
        }
        fs::create_directories(target.parent_path(), failure);
        failure.clear();
        // A rename is instant on one volume; across volumes it fails, and a copy does instead.
        fs::rename(file, target, failure);
        if (failure) {
            failure.clear();
            fs::copy_file(file, target, failure);
        }
        if (failure) {
            error = "could not move " + utf8_path(file.lexically_relative(scratch)) + ": " + failure.message();
            return false;
        }
    }
    remove_scratch_resources(scratch);
    return true;
}

void remove_scratch_resources(const fs::path& scratch) {
    if (scratch.empty() || !is_scratch_resources(scratch)) {
        return;
    }
    std::error_code failure;
    fs::remove_all(scratch.parent_path(), failure);
}

}  // namespace ide
