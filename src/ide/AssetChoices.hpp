#pragma once

#include "DataModel.hpp"
#include "MaterialPreviews.hpp"

#include <filesystem>
#include <optional>

#include <string>
#include <string_view>
#include <vector>

namespace ide {

// What an asset picker lists, without widgets.

// An asset a picker can pick. where is its folder from its category down,
// such as "Meshes/Props". path is a file asset's Path, such as
// "Textures/Grass/Diffuse.png", empty for others. file is a Texture's file
// under the resources folder, and look a Material's, for the picker's icons.
struct AssetChoice {
    engine_core::InstanceId id = 0;
    std::string name;
    std::string where;
    std::string path;
    std::filesystem::path file;
    std::optional<MaterialLook> look;
};

// Every asset of asset_class, such as "Mesh", under its Assets category, by
// name, ignoring case. Empty for a class that is not an asset. The caller
// holds the world's read lock.
std::vector<AssetChoice> asset_choices(const engine_core::DataModel& world, std::string_view asset_class);
// id as a choice, with no where, or nothing when it is not live. The caller holds the world's read lock.
std::optional<AssetChoice> asset_choice(const engine_core::DataModel& world, engine_core::InstanceId id);
// The choices whose name, folder, or file Path contains query, ignoring case. An empty query keeps all.
std::vector<AssetChoice> filter_choices(const std::vector<AssetChoice>& choices, std::string_view query);

}  // namespace ide
