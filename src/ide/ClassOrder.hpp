#pragma once

#include "Strings.hpp"

#include <string_view>

namespace ide {

// Where a class's cluster sits in the explorer and the Insert list. Lower
// comes first. The Camera leads, then Folders, then scripts and CSS, then the things
// in the world: Terrain, the other physics objects, then plain GameObjects. A class
// not listed comes after every listed one.
inline int class_rank(std::string_view class_name) {
    struct Rank {
        std::string_view name;
        int rank;
    };
    static constexpr Rank kRanks[] = {
        {"Camera", 0},
        {"Folder", 1},
        {"Script", 2},
        {"ModuleScript", 2},
        {"CSS", 2},
        {"Terrain", 3},
        {"PhysicsObject", 4},
        {"Brush", 4},
        {"PlayerController", 4},
        {"GameObject", 5},
        {"Model", 5},
        {"Skybox", 6},
        {"DynamicSky", 6},
        {"AmbientOcclusionEffect", 6},
        {"BloomEffect", 6},
        {"ScreenSpaceReflections", 6},
        {"DirectionalLight", 7},
        {"PointLight", 7},
        {"SpotLight", 7},
        {"Sound", 8},
        {"SoundEmitter", 8},
        {"Attachment", 9},
        {"Bone", 9},
        {"Animator", 9},
        {"ScreenGui", 10},
        {"BillboardGui", 10},
        {"Pane", 11},
        {"HBox", 11},
        {"VBox", 11},
        {"ImagePane", 11},
        {"Label", 12},
        {"Button", 12},
        {"TextField", 12},
        {"Slider", 12},
        {"AssetPicker", 12},
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
