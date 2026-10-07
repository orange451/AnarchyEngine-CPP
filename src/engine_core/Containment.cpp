#include "Containment.hpp"

#include "LuaApi.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

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

struct SuitedParents {
    const char* class_name;
    std::vector<const char*> parents;
};

std::vector<SuitedParents>& suited_parents() {
    static std::vector<SuitedParents> records;
    return records;
}

bool is_a(const std::string& class_name, const char* ancestor) {
    return class_name == ancestor || lua_class_inherits(class_name.c_str(), ancestor);
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
        if (find_service(child_class) != nullptr || child_class == kCoreClass) {
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
    if (child_class == "TerrainMaterial" && holder_class != "Terrain") {
        return std::string("A TerrainMaterial must be in a Terrain");
    }
    // Every other asset is a leaf.
    if (is_asset_class(holder_class)) {
        return "A " + std::string(holder_class) + " holds nothing";
    }
    // These affect the whole place, so they live where the place's lighting does.
    if ((child_class == "Skybox" || child_class == "DynamicSky" || child_class == "BloomEffect" ||
         child_class == "ScreenSpaceReflections" || child_class == "AmbientOcclusionEffect") &&
        holder_class != "Lighting") {
        const bool vowel = child_class.front() == 'A';
        return std::string(vowel ? "An " : "A ") + std::string(child_class) + " must be in Lighting";
    }
    if (const char* home = asset_home(child_class)) {
        const std::string where = find_service(home) != nullptr ? service_path(home) : std::string("a ") + home;
        return "A " + std::string(child_class) + " must be in " + where;
    }
    return std::nullopt;
}

void register_suited_parents(const char* class_name, std::initializer_list<const char*> parents) {
    if (class_name == nullptr) {
        return;
    }
    std::vector<SuitedParents>& records = suited_parents();
    auto record = std::find_if(records.begin(), records.end(), [&](const SuitedParents& row) {
        return std::string_view(row.class_name) == class_name;
    });
    if (record == records.end()) {
        record = records.insert(records.end(), SuitedParents{class_name, {}});
    }
    for (const char* parent : parents) {
        if (parent != nullptr &&
            std::find_if(record->parents.begin(), record->parents.end(), [&](const char* known) {
                return std::string_view(known) == parent;
            }) == record->parents.end()) {
            record->parents.push_back(parent);
        }
    }
}

bool parent_suits(std::string_view holder_class, std::string_view child_class) {
    const std::string holder(holder_class);
    const std::string child(child_class);
    bool named = false;
    for (const SuitedParents& record : suited_parents()) {
        for (const char* parent : record.parents) {
            if (!is_a(holder, parent)) {
                continue;
            }
            if (is_a(child, record.class_name)) {
                return true;
            }
            named = true;
        }
    }
    return !named;
}

}  // namespace engine_core
