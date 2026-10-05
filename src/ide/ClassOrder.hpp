#pragma once

#include "Strings.hpp"

#include <string_view>

namespace ide {

// Where a class's cluster sits in the explorer and the Insert list. Lower
// comes first. Folders lead, then scripts and CSS, then the things in the world. A
// class not listed comes after every listed one.
inline int class_rank(std::string_view class_name) {
    struct Rank {
        std::string_view name;
        int rank;
    };
    static constexpr Rank kRanks[] = {
        {"Folder", 0},
        {"Script", 1},
        {"ModuleScript", 1},
        {"CSS", 1},
        {"Camera", 2},
        {"GameObject", 3},
        {"PhysicsObject", 3},
        {"Model", 3},
        {"Skybox", 4},
        {"AmbientOcclusionEffect", 4},
        {"BloomEffect", 4},
        {"ScreenSpaceReflections", 4},
        {"DirectionalLight", 5},
        {"PointLight", 5},
        {"SpotLight", 5},
        {"Sound", 6},
        {"SoundEmitter", 6},
        {"Attachment", 7},
        {"ScreenGui", 8},
        {"BillboardGui", 8},
        {"Pane", 9},
        {"HBox", 9},
        {"VBox", 9},
        {"ImagePane", 9},
        {"Label", 10},
        {"Button", 10},
        {"TextField", 10},
    };
    for (const Rank& entry : kRanks) {
        if (entry.name == class_name) {
            return entry.rank;
        }
    }
    return 100;
}

// A to Z, ignoring case. Names equal but for case are not ordered.
inline bool name_before(std::string_view left, std::string_view right) {
    const std::size_t count = left.size() < right.size() ? left.size() : right.size();
    for (std::size_t i = 0; i < count; ++i) {
        const char a = AsciiLower(static_cast<unsigned char>(left[i]));
        const char b = AsciiLower(static_cast<unsigned char>(right[i]));
        if (a != b) {
            return a < b;
        }
    }
    return left.size() < right.size();
}

// The lower cluster first, then A to Z by name within it.
inline bool cluster_before(int left_rank, std::string_view left_name, int right_rank, std::string_view right_name) {
    if (left_rank != right_rank) {
        return left_rank < right_rank;
    }
    return name_before(left_name, right_name);
}

}  // namespace ide
