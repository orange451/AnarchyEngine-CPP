#pragma once

#include "Strings.hpp"

#include <string_view>

namespace ide {

// Where a class's cluster sits in the explorer and the Insert list. Lower
// comes first. The Camera leads, then Terrain, then Folders, then scripts and CSS, then the things
// in the world: the physics objects, then plain GameObjects. A class
// not listed comes after every listed one.
inline int class_rank(std::string_view class_name) {
    struct Rank {
        std::string_view name;
        int rank;
    };
    static constexpr Rank kRanks[] = {
        {"Camera", 0},
        {"Terrain", 1},
        {"Folder", 2},
        {"Script", 3},
        {"ModuleScript", 3},
        {"CSS", 3},
        {"PhysicsObject", 4},
        {"Brush", 4},
        {"PlayerController", 4},
        {"GameObject", 5},
        {"Model", 5},
        {"Skybox", 6},
        {"DynamicSky", 6},
        {"AmbientOcclusionEffect", 7},
        {"BloomEffect", 7},
        {"ScreenSpaceReflections", 7},
        {"DirectionalLight", 8},
        {"PointLight", 8},
        {"SpotLight", 8},
        {"Sound", 9},
        {"SoundEmitter", 9},
        {"Attachment", 10},
        {"Bone", 10},
        {"Animator", 10},
        {"ScreenGui", 11},
        {"BillboardGui", 11},
        {"Pane", 12},
        {"HBox", 12},
        {"VBox", 12},
        {"ImagePane", 12},
        {"Label", 13},
        {"Button", 13},
        {"TextField", 13},
        {"Slider", 13},
        {"AssetPicker", 13},
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
