#include "AssetImport.hpp"

#include "AssetInstances.hpp"
#include "CutSet.hpp"
#include "DataModel.hpp"
#include "IdeResources.hpp"
#include "TextureImport.hpp"

#include <algorithm>
#include <cctype>

namespace ide {

const std::vector<std::string>& sound_file_extensions() {
    static const std::vector<std::string> extensions = {"wav", "mp3", "flac", "ogg"};
    return extensions;
}

bool is_sound_file(const std::string& path) {
    const std::size_t dot = path.rfind('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string extension = path.substr(dot + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::vector<std::string>& known = sound_file_extensions();
    return std::find(known.begin(), known.end(), extension) != known.end();
}

bool is_importable_file(const std::string& path) {
    return is_texture_file(path) || is_model_file(path) || is_sound_file(path);
}

std::vector<PreparedAsset> prepare_assets(const std::filesystem::path& resources, const std::vector<std::string>& files) {
    std::vector<PreparedAsset> prepared;
    for (const std::string& file : files) {
        PreparedAsset asset;
        asset.file = file;
        asset.model = is_model_file(file);
        asset.sound = !asset.model && is_sound_file(file);
        if (asset.model) {
            if (std::optional<ImportedModel> model = import_model_file(resources, file, asset.error)) {
                asset.imported = std::move(*model);
            }
        } else if (asset.sound || is_texture_file(file)) {
            // A sound is copied as an image is, into a folder of its own.
            const char* folder = asset.sound ? "sounds" : "textures";
            if (std::optional<std::string> path = import_texture_file(resources, file, asset.error, folder)) {
                asset.texture = {utf8_path(path_from_utf8(file).stem()), std::move(*path)};
            }
        } else {
            asset.error = "it is not an image, a model, or a sound file";
        }
        prepared.push_back(std::move(asset));
    }
    return prepared;
}

namespace {

// Makes a FileAsset of klass named and pathed as asset says, in folder.
void place_file(engine_core::DataModel& world, const PreparedAsset& asset, const char* klass,
                engine_core::InstanceId folder, PlacedAsset& out) {
    const engine_core::InstanceId id = insert_instance(world, klass, folder, out.error);
    if (id == 0) {
        return;
    }
    world.set_name(id, asset.texture.name);
    if (std::optional<std::string> refused =
            static_cast<engine_core::FileAsset*>(world.instance(id))->set_path(asset.texture.path)) {
        world.destroy(id);
        out.error = std::move(*refused);
        return;
    }
    out.root = id;
    out.made.push_back(id);
}

// folder when it is category or under it, otherwise category.
engine_core::InstanceId FolderIn(const engine_core::DataModel& world, engine_core::InstanceId folder,
                                 engine_core::InstanceId category) {
    if (folder == 0 || !world.alive(folder)) {
        return category;
    }
    for (engine_core::InstanceId at = folder; at != 0 && at != engine_core::DataModel::kNoParent;
         at = world.parent(at)) {
        if (at == category) {
            return folder;
        }
    }
    return category;
}

}  // namespace

std::vector<PlacedAsset> place_assets(engine_core::DataModel& world, const std::vector<PreparedAsset>& prepared,
                                      engine_core::InstanceId folder) {
    std::vector<PlacedAsset> placed;
    for (const PreparedAsset& asset : prepared) {
        PlacedAsset out;
        if (!asset.error.empty()) {
            out.error = asset.error;
        } else if (asset.model) {
            out.root = build_model_assets(world, asset.imported, out.error, &out.made);
            const engine_core::InstanceId prefabs = world.service("Prefabs");
            const bool prefab = dynamic_cast<const engine_core::Prefab*>(world.instance(out.root)) != nullptr;
            if (const engine_core::InstanceId into = FolderIn(world, folder, prefabs);
                prefab && into != prefabs) {
                world.set_parent(out.root, into);
            }
        } else if (const engine_core::InstanceId audio = world.service("Audio"); asset.sound && audio == 0) {
            out.error = "this place has no Assets.Audio to import into";
        } else if (asset.sound) {
            place_file(world, asset, "Sound", FolderIn(world, folder, audio), out);
        } else if (const engine_core::InstanceId textures = world.service("Textures"); textures == 0) {
            out.error = "this place has no Assets.Textures to import into";
        } else {
            place_file(world, asset, "Texture", FolderIn(world, folder, textures), out);
        }
        placed.push_back(std::move(out));
    }
    return placed;
}

}  // namespace ide
