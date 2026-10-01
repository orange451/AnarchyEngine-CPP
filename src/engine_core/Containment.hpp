#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace engine_core {

// What may go where in a place, by class name, so a project read can check its
// plan before it makes any instance. DataModel::parent_error asks the same rules.

// A service: a child of game, or of another service, that is made with the
// world and cannot be moved, renamed, or destroyed. parent_class is null for a
// child of game.
struct ServiceSpec {
    const char* class_name;
    const char* parent_class;
};

// Every service, parents first, in the order their parents hold them.
inline constexpr ServiceSpec kServices[] = {
    {"Workspace", nullptr}, {"Lighting", nullptr},  {"Storage", nullptr},  {"Scripts", nullptr},
    {"Assets", nullptr},    {"Materials", "Assets"}, {"Prefabs", "Assets"}, {"Meshes", "Assets"},
    {"Textures", "Assets"}, {"Audio", "Assets"},
};

// Null for a class that is not a service.
const ServiceSpec* find_service(std::string_view class_name);
// A service's GUID is its class name in lowercase, the same in every place, so
// a place whose files lack one gets the same service on every read.
std::string service_guid(std::string_view class_name);

// The class an asset must be under: its category, or Prefab for a Model. Null
// for a class that is not an asset.
const char* asset_home(std::string_view class_name);
bool is_asset_class(std::string_view class_name);
// An asset class's plural, such as "Meshes". Null for a class that is not an asset.
const char* asset_plural(std::string_view class_name);

// A Folder has no rule of its own: what goes in it is decided by the first
// ancestor that is not a Folder.
bool passes_rule_up(std::string_view class_name);

// Why holder_class, as the instance whose rule decides, refuses a child of
// child_class named child_name. Empty when it takes it. "Game" is the root.
std::optional<std::string> placement_error(std::string_view holder_class, std::string_view child_class,
                                           std::string_view child_name);

}  // namespace engine_core
