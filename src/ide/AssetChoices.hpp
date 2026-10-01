#pragma once

#include "DataModel.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ide {

// What an asset picker lists, without widgets.

// An asset a picker can pick. where is its folder from its category down,
// such as "Meshes/Props".
struct AssetChoice {
    engine_core::InstanceId id = 0;
    std::string name;
    std::string where;
};

// Every asset of asset_class, such as "Mesh", under its Assets category, by
// name, ignoring case. Empty for a class that is not an asset. The caller
// holds the world's read lock.
std::vector<AssetChoice> asset_choices(const engine_core::DataModel& world, std::string_view asset_class);
// The choices whose name or folder contains query, ignoring case. An empty query keeps all.
std::vector<AssetChoice> filter_choices(const std::vector<AssetChoice>& choices, std::string_view query);

}  // namespace ide
