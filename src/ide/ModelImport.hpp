#pragma once

#include "Color.hpp"
#include "types.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace engine_core {
class DataModel;
}

namespace ide {

// Whether path, in UTF-8, names a model file Assimp reads, by its extension:
// OBJ, FBX, glTF, GLB, DAE, 3DS, PLY, or STL, in any case.
bool is_model_file(const std::string& path);
// The extensions is_model_file takes, without the dot, for a file dialog.
const std::vector<std::string>& model_file_extensions();

// A Texture import_model_file put in the resources folder: its Name and Path.
struct ImportedTexture {
    std::string name;
    std::string path;
};

// A Material to make. Each texture is an index into ImportedModel::textures,
// or -1 for none.
struct ImportedMaterial {
    std::string name;
    engine_core::ColorRgb color{1.f, 1.f, 1.f, 1.f};
    engine_core::ColorRgb emissive{0.f, 0.f, 0.f, 1.f};
    double metalness = 0.0;
    double roughness = 0.4;
    double transparency = 0.0;
    int diffuse = -1;
    int normal = -1;
    int roughness_map = -1;
    int metalness_map = -1;
    int emissive_map = -1;
};

// A Mesh whose AMESH file import_model_file wrote, and the Model that joins it
// to materials[material], or to no Material when it is -1.
struct ImportedMesh {
    std::string name;
    std::string path;
    int material = -1;
};

// An Animation whose AANIM file import_model_file wrote: its Name and Path.
struct ImportedAnimation {
    std::string name;
    std::string path;
};

// What one model file became on disk, for build_model_assets to make instances of.
struct ImportedModel {
    // The file's name without its extension: the Prefab's, and its folders'.
    std::string name;
    std::vector<ImportedTexture> textures;
    std::vector<ImportedMaterial> materials;
    std::vector<ImportedMesh> meshes;
    std::vector<ImportedAnimation> animations;
    // What the file had that was left out, such as a texture not found, one
    // line each.
    std::vector<std::string> notes;
};

// Reads source, a UTF-8 path to a model file, with Assimp, and writes what the
// place needs under resources_root: one AMESH for each of its materials in
// meshes/<Name>/, and the textures its materials use in textures/<Name>/,
// where <Name> is the file's name or name-2, name-3, and so on when another
// import has it. Every node's transform is baked into the vertices, so the
// Models need none. A skinned file keeps its skeleton: every AMESH it writes
// holds the same bone table, each bone resting where the file binds it, and
// each vertex its four heaviest bones; a mesh under a bone follows that bone
// whole. Each clip becomes an AANIM file in animations/<Name>/, its keys
// sampled at every key time it has, each a change from the bone's rest. A
// texture the file names is looked for where it says, then by file name beside
// the model and in the folders under it; one embedded in the file is written
// out. A glTF metallic-roughness map is split in two, since a Material reads
// each from the red channel. Takes as long as the file does to read, on the
// calling thread. Empty, with error set, when the file cannot be read or holds
// neither triangles nor clips.
std::optional<ImportedModel> import_model_file(const std::filesystem::path& resources_root, const std::string& source,
                                               std::string& error);

// SimulationThread. Makes the instances for model: a Folder named after it in
// Assets.Textures, Assets.Materials, Assets.Meshes, and Assets.Animations,
// holding its Textures, Materials, Meshes, and Animations, and a Prefab in
// Assets.Prefabs holding a Model for each Mesh. A Folder that would be empty
// is not made. Returns the Prefab, or for a model of clips alone its
// Animations Folder, and, when made is set, every instance made, in the order
// made. 0, with error set and nothing made, when the place has no room or no
// Assets.
engine_core::InstanceId build_model_assets(engine_core::DataModel& world, const ImportedModel& model,
                                           std::string& error, std::vector<engine_core::InstanceId>* made = nullptr);

}  // namespace ide
