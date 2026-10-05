#include "ModelImport.hpp"

#include "AssetInstances.hpp"
#include "CutSet.hpp"
#include "DataModel.hpp"
#include "IdeResources.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "TextureImport.hpp"
#include "amesh.hpp"
#include "runner/TextureCache.hpp"

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include "stb_image_write.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <system_error>
#include <utility>

namespace ide {
namespace {

namespace fs = std::filesystem;
namespace amesh = anarchy::amesh;

// The resources folders an import writes into, as a Path names them.
constexpr const char* kMeshFolder = "meshes";
constexpr const char* kTextureFolder = "textures";

// Past this many names taken, the import gives up rather than search on.
constexpr int kMaxSuffix = 10000;
// How far the search for a missing texture looks under the model's folder.
constexpr int kSearchDepth = 4;
constexpr std::size_t kSearchFiles = 20000;

std::string AsciiLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

float Clamp01(float value) { return std::isfinite(value) ? std::clamp(value, 0.f, 1.f) : 0.f; }

std::uint8_t ToByte(float value) { return static_cast<std::uint8_t>(std::lround(Clamp01(value) * 255.f)); }

// A folder name under meshes/ and textures/ that neither has yet.
std::optional<std::string> FreeFolder(const fs::path& root, const std::string& name) {
    const std::string safe = engine_core::sanitize_file_name(name);
    for (int suffix = 1; suffix <= kMaxSuffix; ++suffix) {
        const std::string folder = suffix == 1 ? safe : safe + "-" + std::to_string(suffix);
        std::error_code failure;
        if (!fs::exists(root / kMeshFolder / path_from_utf8(folder), failure) &&
            !fs::exists(root / kTextureFolder / path_from_utf8(folder), failure)) {
            return folder;
        }
    }
    return std::nullopt;
}

// name, or name-2, name-3, and so on, whichever taken does not hold, in any case.
std::string UniqueName(const std::string& name, std::set<std::string>& taken) {
    std::string unique = name;
    for (int suffix = 2; !taken.insert(AsciiLower(unique)).second; ++suffix) {
        unique = name + "-" + std::to_string(suffix);
    }
    return unique;
}

// The geometry of one material, gathered from every node that draws it.
struct Bucket {
    unsigned material = 0;
    amesh::Data data;
};

// Appends mesh to data, moved by transform into the model's space.
void AppendMesh(amesh::Data& data, const aiMesh& mesh, const aiMatrix4x4& transform) {
    const aiMatrix3x3 linear(transform);
    aiMatrix3x3 normal_matrix = linear;
    normal_matrix.Inverse().Transpose();
    // A mirroring transform turns the faces inside out; their order turns them back.
    const bool mirrored = linear.Determinant() < 0.f;

    const auto base = static_cast<std::uint32_t>(data.vertices.size());
    for (unsigned i = 0; i < mesh.mNumVertices; ++i) {
        amesh::Vertex vertex;
        const aiVector3D position = transform * mesh.mVertices[i];
        vertex.p[0] = position.x, vertex.p[1] = position.y, vertex.p[2] = position.z;
        aiVector3D normal;
        if (mesh.HasNormals()) {
            normal = normal_matrix * mesh.mNormals[i];
            const float length = normal.Length();
            normal = length > 0.f ? normal / length : aiVector3D();
            vertex.n[0] = normal.x, vertex.n[1] = normal.y, vertex.n[2] = normal.z;
        }
        if (mesh.HasTextureCoords(0)) {
            vertex.uv[0] = mesh.mTextureCoords[0][i].x;
            vertex.uv[1] = mesh.mTextureCoords[0][i].y;
        }
        if (mesh.HasTangentsAndBitangents()) {
            aiVector3D tangent = linear * mesh.mTangents[i];
            tangent -= normal * (normal * tangent);
            const float length = tangent.Length();
            if (length > 0.f) {
                tangent /= length;
                const aiVector3D bitangent = linear * mesh.mBitangents[i];
                vertex.t[0] = tangent.x, vertex.t[1] = tangent.y, vertex.t[2] = tangent.z;
                vertex.t[3] = ((normal ^ tangent) * bitangent) < 0.f ? -1.f : 1.f;
            }
        }
        if (mesh.HasVertexColors(0)) {
            const aiColor4D& color = mesh.mColors[0][i];
            vertex.rgba[0] = ToByte(color.r), vertex.rgba[1] = ToByte(color.g);
            vertex.rgba[2] = ToByte(color.b), vertex.rgba[3] = ToByte(color.a);
        }
        data.vertices.push_back(vertex);
    }
    for (unsigned f = 0; f < mesh.mNumFaces; ++f) {
        const aiFace& face = mesh.mFaces[f];
        if (face.mNumIndices != 3) {
            continue;
        }
        data.indices.push_back(base + face.mIndices[0]);
        data.indices.push_back(base + face.mIndices[mirrored ? 2 : 1]);
        data.indices.push_back(base + face.mIndices[mirrored ? 1 : 2]);
    }
}

// Which of an image's channels a texture keeps: all of them, or one as gray.
enum class Channel { All, Green, Blue };

// Finds, copies, and names the textures one model's materials use, each once.
class TextureFinder {
public:
    TextureFinder(const aiScene& scene, fs::path model_folder, fs::path root, std::string folder,
                  ImportedModel& out)
        : scene_(scene), model_folder_(std::move(model_folder)), root_(std::move(root)),
          folder_(std::move(folder)), out_(out) {}

    // The texture that reference names, as an index into out.textures; -1,
    // with a note, when it cannot be found or read.
    int find(const std::string& reference, Channel channel) {
        const std::string key = reference + (channel == Channel::Green ? "#g" : channel == Channel::Blue ? "#b" : "");
        if (const auto known = by_reference_.find(key); known != by_reference_.end()) {
            return known->second;
        }
        int index = -1;
        std::string problem;
        if (std::optional<std::string> path = store(reference, channel, problem)) {
            index = add(*path);
        } else {
            out_.notes.push_back(std::move(problem));
        }
        by_reference_[key] = index;
        return index;
    }

private:
    int add(const std::string& path) {
        if (const auto known = by_path_.find(path); known != by_path_.end()) {
            return known->second;
        }
        const fs::path file = path_from_utf8(path);
        out_.textures.push_back({utf8_path(file.stem()), path});
        const int index = static_cast<int>(out_.textures.size()) - 1;
        by_path_[path] = index;
        return index;
    }

    // Writes the texture into the resources folder and returns its Path.
    std::optional<std::string> store(const std::string& reference, Channel channel, std::string& problem) {
        std::string bytes;
        std::string file_name;
        if (const aiTexture* embedded = scene_.GetEmbeddedTexture(reference.c_str())) {
            if (!Embedded(*embedded, bytes, file_name, problem)) {
                return std::nullopt;
            }
        } else {
            const std::optional<fs::path> found = locate(reference);
            if (!found) {
                problem = "Texture not found: " + reference;
                return std::nullopt;
            }
            if (!is_texture_file(utf8_path(*found))) {
                problem = "Not an image file: " + utf8_path(found->filename());
                return std::nullopt;
            }
            if (channel == Channel::All) {
                std::string error;
                std::optional<std::string> path = import_texture_file(root_, utf8_path(*found), error, folder_);
                if (!path) {
                    problem = "Could not import " + utf8_path(found->filename()) + ": " + error;
                }
                return path;
            }
            std::string error;
            if (!read_file(*found, bytes, error)) {
                problem = "Could not read " + utf8_path(found->filename()) + ": " + error;
                return std::nullopt;
            }
            file_name = utf8_path(found->filename());
        }
        if (channel != Channel::All) {
            const std::string stem = utf8_path(path_from_utf8(file_name).stem());
            file_name = stem + (channel == Channel::Green ? "-roughness.png" : "-metalness.png");
            if (!SplitChannel(bytes, channel == Channel::Green ? 1 : 2, problem)) {
                problem = "Could not split " + stem + ": " + problem;
                return std::nullopt;
            }
        }
        std::string error;
        std::optional<std::string> path = store_resource_file(root_, folder_, file_name, bytes, error);
        if (!path) {
            problem = "Could not write " + file_name + ": " + error;
        }
        return path;
    }

    // An embedded texture's file: its bytes as they are when compressed, a
    // PNG when they are raw texels.
    bool Embedded(const aiTexture& texture, std::string& bytes, std::string& file_name, std::string& problem) {
        std::string stem = utf8_path(path_from_utf8(texture.mFilename.C_Str()).stem());
        if (stem.empty()) {
            stem = out_.name + "-texture-" + std::to_string(++unnamed_);
        }
        if (texture.mHeight == 0) {
            bytes.assign(reinterpret_cast<const char*>(texture.pcData), texture.mWidth);
            const std::string hint = AsciiLower(texture.achFormatHint);
            file_name = stem + "." + (hint.empty() ? std::string("png") : hint);
            return true;
        }
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(texture.mWidth) * texture.mHeight * 4);
        for (std::size_t i = 0; i < static_cast<std::size_t>(texture.mWidth) * texture.mHeight; ++i) {
            const aiTexel& texel = texture.pcData[i];
            rgba[i * 4 + 0] = texel.r, rgba[i * 4 + 1] = texel.g, rgba[i * 4 + 2] = texel.b, rgba[i * 4 + 3] = texel.a;
        }
        if (!EncodePng(static_cast<int>(texture.mWidth), static_cast<int>(texture.mHeight), 4, rgba.data(), bytes)) {
            problem = "Could not write embedded texture " + stem;
            return false;
        }
        file_name = stem + ".png";
        return true;
    }

    // Replaces bytes, an image, with a gray PNG of one of its channels.
    static bool SplitChannel(std::string& bytes, int channel, std::string& problem) {
        runner::TexturePixels pixels;
        if (!runner::DecodeTexture(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), pixels,
                                   problem)) {
            return false;
        }
        // Decoded bottom row first; a PNG starts at the top.
        std::vector<std::uint8_t> gray(static_cast<std::size_t>(pixels.width) * pixels.height);
        for (int y = 0; y < pixels.height; ++y) {
            const std::size_t from = static_cast<std::size_t>(pixels.height - 1 - y) * pixels.width;
            const std::size_t to = static_cast<std::size_t>(y) * pixels.width;
            for (int x = 0; x < pixels.width; ++x) {
                gray[to + x] = pixels.rgba[(from + x) * 4 + channel];
            }
        }
        if (!EncodePng(pixels.width, pixels.height, 1, gray.data(), bytes)) {
            problem = "it could not be encoded";
            return false;
        }
        return true;
    }

    static bool EncodePng(int width, int height, int components, const std::uint8_t* data, std::string& out) {
        out.clear();
        return stbi_write_png_to_func(
                   [](void* context, void* chunk, int size) {
                       static_cast<std::string*>(context)->append(static_cast<const char*>(chunk),
                                                                  static_cast<std::size_t>(size));
                   },
                   &out, width, height, components, data, width * components) != 0;
    }

    // The file reference names: where it says, beside the model, or anywhere
    // under the model's folder by its name, in any case.
    std::optional<fs::path> locate(std::string reference) {
        std::replace(reference.begin(), reference.end(), '\\', '/');
        if (reference.rfind("file://", 0) == 0) {
            reference.erase(0, 7);
        }
        const fs::path given = path_from_utf8(reference);
        std::error_code failure;
        for (const fs::path& candidate : {given.is_absolute() ? given : model_folder_ / given,
                                          model_folder_ / given.filename()}) {
            if (fs::is_regular_file(candidate, failure)) {
                return candidate;
            }
        }
        const std::string wanted = AsciiLower(utf8_path(given.filename()));
        if (wanted.empty()) {
            return std::nullopt;
        }
        if (!listed_) {
            listed_ = true;
            List(model_folder_, kSearchDepth);
            // Beside the model's folder, as in model/ and textures/ side by side.
            for (fs::directory_iterator it(model_folder_.parent_path(), failure), end; !failure && it != end;
                 it.increment(failure)) {
                if (it->is_directory(failure) && it->path() != model_folder_) {
                    List(it->path(), 1);
                }
            }
        }
        for (const fs::path& file : files_) {
            if (AsciiLower(utf8_path(file.filename())) == wanted) {
                return file;
            }
        }
        return std::nullopt;
    }

    void List(const fs::path& folder, int depth) {
        std::error_code failure;
        fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, failure);
        for (const fs::recursive_directory_iterator end{}; !failure && it != end && files_.size() < kSearchFiles;
             it.increment(failure)) {
            if (it->is_directory(failure)) {
                if (it.depth() + 1 >= depth) {
                    it.disable_recursion_pending();
                }
            } else if (it->is_regular_file(failure)) {
                files_.push_back(it->path());
            }
        }
    }

    const aiScene& scene_;
    const fs::path model_folder_;
    const fs::path root_;
    const std::string folder_;
    ImportedModel& out_;
    std::map<std::string, int> by_reference_;
    std::map<std::string, int> by_path_;
    std::vector<fs::path> files_;
    bool listed_ = false;
    int unnamed_ = 0;
};

std::optional<std::string> TextureReference(const aiMaterial& material, aiTextureType type) {
    aiString path;
    if (material.GetTextureCount(type) == 0 || material.GetTexture(type, 0, &path) != AI_SUCCESS ||
        path.length == 0) {
        return std::nullopt;
    }
    return std::string(path.C_Str());
}

std::optional<std::string> FirstTexture(const aiMaterial& material, std::initializer_list<aiTextureType> types) {
    for (aiTextureType type : types) {
        if (std::optional<std::string> reference = TextureReference(material, type)) {
            return reference;
        }
    }
    return std::nullopt;
}

ImportedMaterial ConvertMaterial(const aiMaterial& material, const std::string& fallback_name, bool obj,
                                 TextureFinder& textures) {
    ImportedMaterial out;
    aiString name;
    out.name = material.Get(AI_MATKEY_NAME, name) == AI_SUCCESS && name.length > 0 &&
                       std::string(name.C_Str()) != AI_DEFAULT_MATERIAL_NAME
                   ? std::string(name.C_Str())
                   : fallback_name;

    if (std::optional<std::string> diffuse = FirstTexture(material, {aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE})) {
        out.diffuse = textures.find(*diffuse, Channel::All);
    }
    // OBJ has no normal map; exporters such as Blender's write one as its bump map.
    std::optional<std::string> normal = FirstTexture(material, {aiTextureType_NORMALS, aiTextureType_NORMAL_CAMERA});
    if (!normal && obj) {
        normal = TextureReference(material, aiTextureType_HEIGHT);
    }
    if (normal) {
        out.normal = textures.find(*normal, Channel::All);
    }
    const std::optional<std::string> roughness = TextureReference(material, aiTextureType_DIFFUSE_ROUGHNESS);
    const std::optional<std::string> metalness = TextureReference(material, aiTextureType_METALNESS);
    if (roughness && metalness && *roughness == *metalness) {
        // glTF packs roughness in green and metalness in blue; a Material reads each from red.
        out.roughness_map = textures.find(*roughness, Channel::Green);
        out.metalness_map = textures.find(*metalness, Channel::Blue);
    } else {
        if (roughness) {
            out.roughness_map = textures.find(*roughness, Channel::All);
        }
        if (metalness) {
            out.metalness_map = textures.find(*metalness, Channel::All);
        }
    }
    if (std::optional<std::string> emissive =
            FirstTexture(material, {aiTextureType_EMISSION_COLOR, aiTextureType_EMISSIVE})) {
        out.emissive_map = textures.find(*emissive, Channel::All);
    }

    aiColor4D color;
    if (material.Get(AI_MATKEY_BASE_COLOR, color) == AI_SUCCESS) {
        out.color = {Clamp01(color.r), Clamp01(color.g), Clamp01(color.b), 1.f};
    } else if (out.diffuse < 0 && material.Get(AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS) {
        // A diffuse color beside a diffuse map is, in most exporters, a leftover the map replaces.
        out.color = {Clamp01(color.r), Clamp01(color.g), Clamp01(color.b), 1.f};
    }
    if (material.Get(AI_MATKEY_COLOR_EMISSIVE, color) == AI_SUCCESS) {
        float intensity = 1.f;
        material.Get(AI_MATKEY_EMISSIVE_INTENSITY, intensity);
        out.emissive = {Clamp01(color.r * intensity), Clamp01(color.g * intensity), Clamp01(color.b * intensity), 1.f};
    } else if (out.emissive_map >= 0) {
        // Emissive scales the map, so with no color the map has the say.
        out.emissive = {1.f, 1.f, 1.f, 1.f};
    }
    // An opacity of 0 is, in practice, an exporter that never set it.
    float opacity = 1.f;
    if (material.Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS && opacity > 0.f && opacity < 1.f) {
        out.transparency = 1.0 - opacity;
    }
    // A map's factor scales it, so with no factor the map has the say.
    float factor = 0.f;
    if (material.Get(AI_MATKEY_METALLIC_FACTOR, factor) == AI_SUCCESS) {
        out.metalness = Clamp01(factor);
    } else {
        out.metalness = out.metalness_map >= 0 ? 1.0 : 0.0;
    }
    float shininess = 0.f;
    if (material.Get(AI_MATKEY_ROUGHNESS_FACTOR, factor) == AI_SUCCESS) {
        out.roughness = Clamp01(factor);
    } else if (out.roughness_map >= 0) {
        out.roughness = 1.0;
    } else if (material.Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS && shininess > 0.f) {
        // A Phong exponent as a GGX roughness.
        out.roughness = Clamp01(std::sqrt(2.f / (shininess + 2.f)));
    }
    return out;
}

void Note(ImportedModel& model, std::string note) {
    if (std::find(model.notes.begin(), model.notes.end(), note) == model.notes.end()) {
        model.notes.push_back(std::move(note));
    }
}

}  // namespace

bool is_model_file(const std::string& path) {
    const std::size_t dot = path.rfind('.');
    if (dot == std::string::npos) {
        return false;
    }
    const std::vector<std::string>& known = model_file_extensions();
    return std::find(known.begin(), known.end(), AsciiLower(path.substr(dot + 1))) != known.end();
}

const std::vector<std::string>& model_file_extensions() {
    static const std::vector<std::string> extensions = {"obj", "fbx", "gltf", "glb", "dae", "3ds", "ply", "stl"};
    return extensions;
}

std::optional<ImportedModel> import_model_file(const fs::path& resources_root, const std::string& source,
                                               std::string& error) {
    const fs::path from = path_from_utf8(source);
    Assimp::Importer importer;
    // Points and lines have nothing to draw them; a degenerate triangle goes with them.
    importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_POINT | aiPrimitiveType_LINE);
    importer.SetPropertyBool(AI_CONFIG_PP_FD_REMOVE, true);
    // Assimp's UVs already start at the image's bottom row, as AMESH's do.
    constexpr unsigned kSteps = aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_GenSmoothNormals |
                                aiProcess_CalcTangentSpace | aiProcess_SortByPType | aiProcess_FindDegenerates |
                                aiProcess_FindInvalidData | aiProcess_GenUVCoords | aiProcess_TransformUVCoords |
                                aiProcess_SplitLargeMeshes | aiProcess_ImproveCacheLocality |
                                aiProcess_RemoveRedundantMaterials | aiProcess_ValidateDataStructure;
    const aiScene* scene = importer.ReadFile(utf8_path(from), kSteps);
    if (scene == nullptr || scene->mRootNode == nullptr || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0) {
        error = importer.GetErrorString();
        if (error.empty()) {
            error = "it holds no scene";
        }
        return std::nullopt;
    }

    ImportedModel model;
    model.name = utf8_path(from.stem());
    if (model.name.empty()) {
        model.name = "Model";
    }

    // Each material's triangles, in the order the nodes first draw it, with
    // every node's transform baked in. A material too big for one AMESH goes on in another.
    std::vector<Bucket> buckets;
    std::map<unsigned, std::size_t> open;
    std::vector<std::pair<const aiNode*, aiMatrix4x4>> stack{{scene->mRootNode, scene->mRootNode->mTransformation}};
    while (!stack.empty()) {
        const auto [node, transform] = stack.back();
        stack.pop_back();
        for (unsigned i = node->mNumChildren; i-- > 0;) {
            stack.emplace_back(node->mChildren[i], transform * node->mChildren[i]->mTransformation);
        }
        for (unsigned i = 0; i < node->mNumMeshes; ++i) {
            const aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
            if (mesh == nullptr || (mesh->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) == 0 || mesh->mNumFaces == 0) {
                continue;
            }
            if (mesh->HasBones()) {
                Note(model, "Skinned meshes came in static, in their bind pose: bones are not imported yet");
            }
            auto at = open.find(mesh->mMaterialIndex);
            if (at == open.end() ||
                buckets[at->second].data.vertices.size() + mesh->mNumVertices > amesh::kMaxVertices ||
                buckets[at->second].data.indices.size() / 3 + mesh->mNumFaces > amesh::kMaxTriangles) {
                buckets.push_back({mesh->mMaterialIndex, {}});
                at = open.insert_or_assign(mesh->mMaterialIndex, buckets.size() - 1).first;
            }
            AppendMesh(buckets[at->second].data, *mesh, transform);
        }
    }
    buckets.erase(std::remove_if(buckets.begin(), buckets.end(),
                                 [](const Bucket& bucket) { return bucket.data.indices.empty(); }),
                  buckets.end());
    if (buckets.empty()) {
        error = "it holds no triangles";
        return std::nullopt;
    }
    if (scene->mNumAnimations > 0) {
        Note(model, "Animations are not imported yet");
    }

    const std::optional<std::string> folder = FreeFolder(resources_root, model.name);
    if (!folder) {
        error = "every folder name for " + model.name + " is taken";
        return std::nullopt;
    }
    const std::string mesh_folder = std::string(kMeshFolder) + "/" + *folder;
    TextureFinder textures(*scene, from.parent_path(), resources_root, std::string(kTextureFolder) + "/" + *folder,
                           model);
    const bool obj = AsciiLower(utf8_path(from.extension())) == ".obj";

    std::map<unsigned, int> materials;
    std::set<std::string> material_names;
    std::set<std::string> mesh_names;
    for (Bucket& bucket : buckets) {
        auto material = materials.find(bucket.material);
        if (material == materials.end() && bucket.material < scene->mNumMaterials) {
            ImportedMaterial made = ConvertMaterial(*scene->mMaterials[bucket.material], model.name, obj, textures);
            made.name = UniqueName(made.name, material_names);
            model.materials.push_back(std::move(made));
            material = materials.emplace(bucket.material, static_cast<int>(model.materials.size()) - 1).first;
        }
        const int material_index = material != materials.end() ? material->second : -1;
        const std::string base = material_index >= 0 ? model.materials[material_index].name : model.name;
        const std::string name = UniqueName(base, mesh_names);

        amesh::compute_aabb(bucket.data);
        std::string bytes;
        try {
            const std::vector<std::byte> written = amesh::write(bucket.data);
            bytes.assign(reinterpret_cast<const char*>(written.data()), written.size());
        } catch (const std::exception& failure) {
            error = name + " does not fit in an AMESH file: " + failure.what();
            return std::nullopt;
        }
        std::optional<std::string> path = store_resource_file(resources_root, mesh_folder, name + ".amesh", bytes, error);
        if (!path) {
            return std::nullopt;
        }
        model.meshes.push_back({name, std::move(*path), material_index});
    }
    return model;
}

engine_core::InstanceId build_model_assets(engine_core::DataModel& world, const ImportedModel& model,
                                           std::string& error, std::vector<engine_core::InstanceId>* made_out) {
    const engine_core::InstanceId textures = world.service("Textures");
    const engine_core::InstanceId materials = world.service("Materials");
    const engine_core::InstanceId meshes = world.service("Meshes");
    const engine_core::InstanceId prefabs = world.service("Prefabs");
    if (textures == 0 || materials == 0 || meshes == 0 || prefabs == 0) {
        error = "This place has no Assets to import into";
        return 0;
    }
    // Every instance, its three folders, and a Prefab, so none is left half made.
    const std::size_t needed = model.textures.size() + model.materials.size() + model.meshes.size() * 2 + 4;
    if (world.room_left() < needed) {
        error = engine_core::InstanceCapacityError().what();
        return 0;
    }

    std::vector<engine_core::InstanceId> made;
    auto make = [&](const char* class_name, engine_core::InstanceId parent, const std::string& name) {
        const engine_core::InstanceId id = insert_instance(world, class_name, parent, error);
        if (id != 0) {
            world.set_name(id, name);
            made.push_back(id);
        }
        return id;
    };
    auto fail = [&] {
        for (auto it = made.rbegin(); it != made.rend(); ++it) {
            world.destroy(*it);
        }
        return engine_core::InstanceId{0};
    };
    auto reference = [](engine_core::ReferenceAsset& asset, std::size_t index, engine_core::InstanceId target) {
        if (target == 0) {
            return;
        }
        engine_core::LuaSlot slot;
        slot.kind = engine_core::LuaSlot::Kind::Instance;
        slot.id = target;
        (void)asset.set_reference(index, slot);
    };

    std::vector<engine_core::InstanceId> texture_ids;
    if (!model.textures.empty()) {
        const engine_core::InstanceId folder = make("Folder", textures, model.name);
        if (folder == 0) {
            return fail();
        }
        for (const ImportedTexture& texture : model.textures) {
            const engine_core::InstanceId id = make("Texture", folder, texture.name);
            if (id == 0) {
                return fail();
            }
            (void)static_cast<engine_core::Texture*>(world.instance(id))->set_path(texture.path);
            texture_ids.push_back(id);
        }
    }
    auto texture_id = [&texture_ids](int index) {
        return index >= 0 && static_cast<std::size_t>(index) < texture_ids.size() ? texture_ids[index]
                                                                                  : engine_core::InstanceId{0};
    };

    std::vector<engine_core::InstanceId> material_ids;
    if (!model.materials.empty()) {
        const engine_core::InstanceId folder = make("Folder", materials, model.name);
        if (folder == 0) {
            return fail();
        }
        for (const ImportedMaterial& imported : model.materials) {
            const engine_core::InstanceId id = make("Material", folder, imported.name);
            if (id == 0) {
                return fail();
            }
            auto& material = *static_cast<engine_core::Material*>(world.instance(id));
            (void)material.set_color(imported.color);
            (void)material.set_emissive(imported.emissive);
            (void)material.set_metalness(imported.metalness);
            (void)material.set_roughness(imported.roughness);
            (void)material.set_transparency(imported.transparency);
            reference(material, engine_core::Material::kDiffuseTextureReference, texture_id(imported.diffuse));
            reference(material, engine_core::Material::kNormalTextureReference, texture_id(imported.normal));
            reference(material, engine_core::Material::kRoughnessTextureReference, texture_id(imported.roughness_map));
            reference(material, engine_core::Material::kMetalnessTextureReference, texture_id(imported.metalness_map));
            reference(material, engine_core::Material::kEmissiveTextureReference, texture_id(imported.emissive_map));
            material_ids.push_back(id);
        }
    }

    const engine_core::InstanceId mesh_folder = make("Folder", meshes, model.name);
    const engine_core::InstanceId prefab = mesh_folder != 0 ? make("Prefab", prefabs, model.name) : 0;
    if (prefab == 0) {
        return fail();
    }
    for (const ImportedMesh& imported : model.meshes) {
        const engine_core::InstanceId mesh = make("Mesh", mesh_folder, imported.name);
        const engine_core::InstanceId model_id = mesh != 0 ? make("Model", prefab, imported.name) : 0;
        if (model_id == 0) {
            return fail();
        }
        (void)static_cast<engine_core::Mesh*>(world.instance(mesh))->set_path(imported.path);
        auto& joined = *static_cast<engine_core::Model*>(world.instance(model_id));
        reference(joined, engine_core::Model::kMeshReference, mesh);
        if (imported.material >= 0 && static_cast<std::size_t>(imported.material) < material_ids.size()) {
            reference(joined, engine_core::Model::kMaterialReference, material_ids[imported.material]);
        }
    }
    if (made_out != nullptr) {
        *made_out = std::move(made);
    }
    return prefab;
}

}  // namespace ide
