#include "Containment.hpp"

#include <cctype>

namespace engine_core {
namespace {

// A category, the asset class it holds, and that class's plural for messages.
struct Category {
    const char* service;
    const char* asset;
    const char* plural;
};

constexpr Category kCategories[] = {
    {"Materials", "Material", "Materials"}, {"Prefabs", "Prefab", "Prefabs"}, {"Meshes", "Mesh", "Meshes"},
    {"Textures", "Texture", "Textures"},    {"Audio", "Sound", "Sounds"},
};

const Category* category_named(std::string_view service) {
    for (const Category& category : kCategories) {
        if (service == category.service) {
            return &category;
        }
    }
    return nullptr;
}

// "Assets.Textures" for a service under Assets; the class itself otherwise.
std::string service_path(std::string_view class_name) {
    const ServiceSpec* spec = find_service(class_name);
    if (spec == nullptr || spec->parent_class == nullptr) {
        return std::string(class_name);
    }
    return std::string(spec->parent_class) + "." + std::string(class_name);
}

}  // namespace

const ServiceSpec* find_service(std::string_view class_name) {
    for (const ServiceSpec& spec : kServices) {
        if (class_name == spec.class_name) {
            return &spec;
        }
    }
    return nullptr;
}

std::string service_guid(std::string_view class_name) {
    std::string guid(class_name);
    for (char& c : guid) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return guid;
}

const char* asset_home(std::string_view class_name) {
    if (class_name == "Model") {
        return "Prefab";
    }
    for (const Category& category : kCategories) {
        if (class_name == category.asset) {
            return category.service;
        }
    }
    return nullptr;
}

bool is_asset_class(std::string_view class_name) { return asset_home(class_name) != nullptr; }

const char* asset_plural(std::string_view class_name) {
    for (const Category& category : kCategories) {
        if (class_name == category.asset) {
            return category.plural;
        }
    }
    return nullptr;
}

bool passes_rule_up(std::string_view class_name) { return class_name == "Folder"; }

std::optional<std::string> placement_error(std::string_view holder_class, std::string_view child_class,
                                           std::string_view child_name) {
    if (holder_class == "Game") {
        if (find_service(child_class) != nullptr) {
            return std::nullopt;
        }
        return "Only scene services can be children of game; put " + std::string(child_name) + " in Workspace";
    }
    if (holder_class == "Assets") {
        const ServiceSpec* spec = find_service(child_class);
        if (spec != nullptr && spec->parent_class != nullptr && std::string_view(spec->parent_class) == "Assets") {
            return std::nullopt;
        }
        return std::string("Assets holds only Materials, Prefabs, Meshes, Textures, and Audio");
    }
    if (const Category* category = category_named(holder_class)) {
        if (child_class == category->asset || child_class == "Folder") {
            return std::nullopt;
        }
        return std::string(category->service) + " holds " + category->plural + " and Folders";
    }
    if (holder_class == "Prefab") {
        if (child_class == "Model") {
            return std::nullopt;
        }
        return std::string("A Prefab holds only Models");
    }
    // Every other asset is a leaf.
    if (is_asset_class(holder_class)) {
        return "A " + std::string(holder_class) + " holds nothing";
    }
    if (const char* home = asset_home(child_class)) {
        const std::string where = find_service(home) != nullptr ? service_path(home) : std::string("a ") + home;
        return "A " + std::string(child_class) + " must be in " + where;
    }
    return std::nullopt;
}

}  // namespace engine_core
