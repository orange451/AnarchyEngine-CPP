#include "AssetChoices.hpp"

#include "Containment.hpp"

#include <algorithm>
#include <cctype>

namespace ide {

namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

void collect(const engine_core::DataModel& world, engine_core::InstanceId folder, const std::string& where,
             std::string_view asset_class, std::vector<AssetChoice>& out) {
    for (engine_core::InstanceId child : world.get_children(folder)) {
        const engine_core::DataModel* object = world.instance(child);
        if (object == nullptr) {
            continue;
        }
        const std::string name = world.name(child);
        if (asset_class == object->class_name()) {
            out.push_back(AssetChoice{child, name, where});
        } else {
            collect(world, child, where + "/" + name, asset_class, out);
        }
    }
}

}  // namespace

std::vector<AssetChoice> asset_choices(const engine_core::DataModel& world, std::string_view asset_class) {
    std::vector<AssetChoice> choices;
    const char* category = engine_core::asset_home(asset_class);
    if (category == nullptr) {
        return choices;
    }
    if (const engine_core::InstanceId root = world.service(category)) {
        collect(world, root, category, asset_class, choices);
    }
    std::stable_sort(choices.begin(), choices.end(), [](const AssetChoice& a, const AssetChoice& b) {
        return lower(a.name) < lower(b.name);
    });
    return choices;
}

std::vector<AssetChoice> filter_choices(const std::vector<AssetChoice>& choices, std::string_view query) {
    const std::string needle = lower(query);
    if (needle.empty()) {
        return choices;
    }
    std::vector<AssetChoice> kept;
    for (const AssetChoice& choice : choices) {
        if (lower(choice.name).find(needle) != std::string::npos ||
            lower(choice.where).find(needle) != std::string::npos) {
            kept.push_back(choice);
        }
    }
    return kept;
}

}  // namespace ide
