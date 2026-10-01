#include "AssetImport.hpp"

#include "AssetInstances.hpp"
#include "CutSet.hpp"
#include "DataModel.hpp"
#include "IdeResources.hpp"
#include "TextureImport.hpp"

namespace ide {

bool is_importable_file(const std::string& path) { return is_texture_file(path) || is_model_file(path); }

std::vector<PreparedAsset> prepare_assets(const std::filesystem::path& resources, const std::vector<std::string>& files) {
    std::vector<PreparedAsset> prepared;
    for (const std::string& file : files) {
        PreparedAsset asset;
        asset.file = file;
        asset.model = is_model_file(file);
        if (asset.model) {
            if (std::optional<ImportedModel> model = import_model_file(resources, file, asset.error)) {
                asset.imported = std::move(*model);
            }
        } else if (is_texture_file(file)) {
            if (std::optional<std::string> path = import_texture_file(resources, file, asset.error)) {
                asset.texture = {utf8_path(path_from_utf8(file).stem()), std::move(*path)};
            }
        } else {
            asset.error = "it is not an image or a model file";
        }
        prepared.push_back(std::move(asset));
    }
    return prepared;
}

std::vector<PlacedAsset> place_assets(engine_core::DataModel& world, const std::vector<PreparedAsset>& prepared) {
    std::vector<PlacedAsset> placed;
    for (const PreparedAsset& asset : prepared) {
        PlacedAsset out;
        if (!asset.error.empty()) {
            out.error = asset.error;
        } else if (asset.model) {
            out.root = build_model_assets(world, asset.imported, out.error, &out.made);
        } else if (const engine_core::InstanceId folder = world.service("Textures"); folder == 0) {
            out.error = "this place has no Assets.Textures to import into";
        } else if (const engine_core::InstanceId id = insert_instance(world, "Texture", folder, out.error); id != 0) {
            world.set_name(id, asset.texture.name);
            if (std::optional<std::string> refused =
                    static_cast<engine_core::Texture*>(world.instance(id))->set_path(asset.texture.path)) {
                world.destroy(id);
                out.error = std::move(*refused);
            } else {
                out.root = id;
                out.made.push_back(id);
            }
        }
        placed.push_back(std::move(out));
    }
    return placed;
}

}  // namespace ide
