#pragma once

#include "ModelImport.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace engine_core {
class DataModel;
}

namespace ide {

// Importing files into a place, in two steps: prepare_assets reads them and
// writes what they need into the resources folder, on any thread, and
// place_assets makes their instances, on the simulation thread. Dropping files
// on the studio and the import_assets MCP tool both run them.

// Whether path, in UTF-8, names a file prepare_assets takes: an image, as
// is_texture_file says, or a model, as is_model_file says.
bool is_importable_file(const std::string& path);

// One file, read and written into a resources folder.
struct PreparedAsset {
    // As given.
    std::string file;
    // A model; otherwise an image.
    bool model = false;
    // Why nothing was prepared. Nothing below is set then.
    std::string error;
    // An image's Texture: its Name, after the file, and Path.
    ImportedTexture texture;
    // A model's meshes, materials, and textures.
    ImportedModel imported;
};

// Prepares each file, in order, into resources: an image as import_texture_file
// does, into textures/, and a model as import_model_file does. A file that is
// neither, or that cannot be read or written, has error set; the rest go on.
// Takes as long as the files do to read, on the calling thread.
std::vector<PreparedAsset> prepare_assets(const std::filesystem::path& resources, const std::vector<std::string>& files);

// What place_assets made of one PreparedAsset.
struct PlacedAsset {
    // The Texture or the Prefab. 0 when error is set.
    engine_core::InstanceId root = 0;
    // Every instance made, root included, in the order made.
    std::vector<engine_core::InstanceId> made;
    // Why nothing was made: the prepare's error, or the place's refusal.
    std::string error;
};

// SimulationThread. Makes the instances for each prepared file: a Texture in
// Assets.Textures for an image, and what build_model_assets makes for a model.
// One entry for each in prepared, in order. The caller holds the undo gesture.
std::vector<PlacedAsset> place_assets(engine_core::DataModel& world, const std::vector<PreparedAsset>& prepared);

}  // namespace ide
