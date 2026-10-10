#include "ide/IdeLayout.hpp"
#include "ide/IdeResources.hpp"
#include "ide/ModelImport.hpp"
#include "runner/TextureCache.hpp"

#include "AssetInstances.hpp"
#include "DataModel.hpp"
#include "Engine.hpp"
#include "LuaApi.hpp"
#include "Project.hpp"
#include "SelectionService.hpp"
#include "amesh.hpp"

#include "jadefx/jadefx.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::string ReadBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteBytes(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

// An uncompressed 32-bit TGA, top row first, from RGBA rows.
std::string Tga(int width, int height, const std::vector<std::uint8_t>& rgba) {
    std::string bytes(18, '\0');
    bytes[2] = 2;
    bytes[12] = static_cast<char>(width & 0xff), bytes[13] = static_cast<char>(width >> 8);
    bytes[14] = static_cast<char>(height & 0xff), bytes[15] = static_cast<char>(height >> 8);
    bytes[16] = 32;
    bytes[17] = 0x28;
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        bytes += static_cast<char>(rgba[i + 2]);
        bytes += static_cast<char>(rgba[i + 1]);
        bytes += static_cast<char>(rgba[i + 0]);
        bytes += static_cast<char>(rgba[i + 3]);
    }
    return bytes;
}

template <typename T>
void Append(std::string& bytes, const T& value) {
    bytes.append(reinterpret_cast<const char*>(&value), sizeof(T));
}

bool Decode(const fs::path& file, runner::TexturePixels& pixels) {
    const std::string bytes = ReadBytes(file);
    std::string error;
    return runner::DecodeTexture(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), pixels, error);
}

bool ReadMesh(const fs::path& file, anarchy::amesh::Data& data) {
    const std::string bytes = ReadBytes(file);
    try {
        data = anarchy::amesh::read(
            anarchy::amesh::ByteSpan(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool Near(double a, double b) { return std::fabs(a - b) < 1e-3; }

// What a Material made by an import holds.
struct MaterialView {
    std::string name;
    engine_core::ColorRgb color;
    double roughness = 0;
    engine_core::InstanceId diffuse = 0;
    engine_core::InstanceId normal = 0;
};

engine_core::InstanceId Target(const engine_core::ReferenceAsset& asset, std::size_t index) {
    const engine_core::LuaSlot slot = asset.reference(index);
    return slot.kind == engine_core::LuaSlot::Kind::Instance ? slot.id : 0;
}

}  // namespace

// Reads model files with Assimp into AMESH files and textures under a
// resources folder, and, dropped on the studio, into a Prefab with its
// Meshes, Materials, and Textures. Replaces the open place.
int RunModelImportTests(ide::IdeLayout& layout, jadefx::Scene& scene) {
    int failures = 0;
    auto expect = [&failures](bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL %s\n", message);
            ++failures;
        }
    };

    expect(ide::is_model_file("a/Crate.OBJ") && ide::is_model_file("b.fbx") && ide::is_model_file("c.glb") &&
               ide::is_model_file("d.gltf") && ide::is_model_file("e.dae"),
           "OBJ, FBX, GLB, glTF, and DAE files are models in any case");
    expect(!ide::is_model_file("Brick.png") && !ide::is_model_file("obj") && !ide::is_model_file("notes.txt"),
           "a file without a model extension is not");

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path folder = fs::temp_directory_path() / ("anarchy-model-test-" + std::to_string(stamp));

    // A glTF triangle moved by its node, whose metallic-roughness map packs
    // roughness in green and metalness in blue.
    {
        const fs::path source = folder / "Gltf";
        const fs::path resources = folder / "GltfResources";
        const std::vector<std::uint8_t> packed = {10, 20, 30, 255, 40, 50, 60, 255};
        WriteBytes(source / "mr.tga", Tga(1, 2, packed));
        std::string buffer;
        for (float value : {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f}) {
            Append(buffer, value);
        }
        for (float value : {0.f, 0.f, 1.f, 0.f, 0.f, 1.f}) {
            Append(buffer, value);
        }
        for (std::uint16_t index : {0, 1, 2, 0}) {
            Append(buffer, index);
        }
        WriteBytes(source / "tri.bin", buffer);
        WriteBytes(source / "Tri.gltf", R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [{"mesh": 0, "translation": [10, 0, 0]}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1}, "indices": 2, "material": 0}]}],
  "materials": [{"name": "Metal", "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1],
    "metallicFactor": 1.0, "roughnessFactor": 0.5, "metallicRoughnessTexture": {"index": 0}},
    "emissiveFactor": [1, 0.5, 0], "emissiveTexture": {"index": 0}}],
  "textures": [{"source": 0}],
  "images": [{"uri": "mr.tga"}],
  "buffers": [{"byteLength": 68, "uri": "tri.bin"}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962},
                  {"buffer": 0, "byteOffset": 36, "byteLength": 24, "target": 34962},
                  {"buffer": 0, "byteOffset": 60, "byteLength": 6, "target": 34963}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
                {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2"},
                {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"}]
})");
        std::string error;
        const std::optional<ide::ImportedModel> model =
            ide::import_model_file(resources, ide::utf8_path(source / "Tri.gltf"), error);
        expect(model.has_value(), "a glTF file imports");
        if (!model) {
            std::fprintf(stderr, "  %s\n", error.c_str());
        }
        if (model && model->meshes.size() == 1 && model->materials.size() == 1) {
            expect(model->name == "Tri", "named after its file");
            const ide::ImportedMaterial& metal = model->materials[0];
            expect(metal.name == "Metal" && Near(metal.metalness, 1.0) && Near(metal.roughness, 0.5),
                   "its material keeps its name and factors");
            expect(metal.emissive_map >= 0 && Near(metal.emissive.r, 1.0) && Near(metal.emissive.g, 0.5) &&
                       Near(metal.emissive.b, 0.0),
                   "its emissive map is kept, with its factor as Emissive");
            expect(model->meshes[0].path == "meshes/Tri/Metal.amesh" && model->meshes[0].material == 0,
                   "its mesh is written under meshes/<model>, named after its material");
            anarchy::amesh::Data data;
            expect(ReadMesh(resources / "meshes" / "Tri" / "Metal.amesh", data), "as an AMESH file");
            expect(data.indices.size() == 3 && Near(data.bbox_min[0], 10) && Near(data.bbox_max[0], 11),
                   "with the node's translation baked into its vertices");
            expect(metal.roughness_map >= 0 && metal.metalness_map >= 0 && metal.roughness_map != metal.metalness_map,
                   "the packed map becomes two textures");
            if (metal.roughness_map >= 0 && metal.metalness_map >= 0) {
                const ide::ImportedTexture& roughness = model->textures[metal.roughness_map];
                const ide::ImportedTexture& metalness = model->textures[metal.metalness_map];
                expect(roughness.path == "textures/Tri/mr-roughness.png" &&
                           metalness.path == "textures/Tri/mr-metalness.png",
                       "named for the channel each keeps");
                runner::TexturePixels original;
                runner::TexturePixels rough;
                runner::TexturePixels metallic;
                expect(Decode(source / "mr.tga", original) && Decode(resources / ide::path_from_utf8(roughness.path), rough) &&
                           Decode(resources / ide::path_from_utf8(metalness.path), metallic),
                       "each split map decodes");
                if (original.rgba.size() == 8 && rough.rgba.size() == 8 && metallic.rgba.size() == 8) {
                    expect(rough.rgba[0] == original.rgba[1] && rough.rgba[4] == original.rgba[5],
                           "the roughness map holds green, row for row");
                    expect(metallic.rgba[0] == original.rgba[2] && metallic.rgba[4] == original.rgba[6],
                           "the metalness map holds blue, row for row");
                }
            }
        } else if (model) {
            expect(false, "one mesh and one material");
        }

        error.clear();
        WriteBytes(source / "Broken.obj", "this is not a model\n");
        expect(!ide::import_model_file(resources, ide::utf8_path(source / "Broken.obj"), error) && !error.empty(),
               "a file with no triangles is refused, saying why");
    }

    // A glTF arm skinned to two joints, Root and Hand (2 units up), under a
    // node 5 units along Z, with one triangle in each of two materials.
    {
        const fs::path source = folder / "Rigged";
        const fs::path resources = folder / "RiggedResources";
        std::string buffer;
        for (float value : {0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f}) {
            Append(buffer, value);
        }
        for (float value : {0.f, 2.f, 0.f, 1.f, 2.f, 0.f, 0.f, 3.f, 0.f}) {
            Append(buffer, value);
        }
        for (std::uint8_t joint : {0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0}) {
            Append(buffer, joint);
        }
        for (float weight : {0.75f, 0.25f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f}) {
            Append(buffer, weight);
        }
        for (std::uint16_t index : {0, 1, 2, 0}) {
            Append(buffer, index);
        }
        // Inverse bind matrices, column-major: Root's identity, Hand's 2 units down.
        for (float value : {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f,
                            1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, -2.f, 0.f, 1.f}) {
            Append(buffer, value);
        }
        WriteBytes(source / "arm.bin", buffer);
        WriteBytes(source / "Arm.gltf", R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0]}],
  "nodes": [{"name": "Arm", "translation": [0, 0, 5], "children": [1, 3]},
            {"name": "Root", "children": [2]},
            {"name": "Hand", "translation": [0, 2, 0]},
            {"name": "Skin", "mesh": 0, "skin": 0}],
  "skins": [{"joints": [1, 2], "inverseBindMatrices": 5}],
  "meshes": [{"primitives": [
    {"attributes": {"POSITION": 0, "JOINTS_0": 2, "WEIGHTS_0": 3}, "indices": 4, "material": 0},
    {"attributes": {"POSITION": 1, "JOINTS_0": 2, "WEIGHTS_0": 3}, "indices": 4, "material": 1}]}],
  "materials": [{"name": "Upper", "pbrMetallicRoughness": {"baseColorFactor": [1, 0, 0, 1]}},
                {"name": "Lower", "pbrMetallicRoughness": {"baseColorFactor": [0, 0, 1, 1]}}],
  "buffers": [{"byteLength": 268, "uri": "arm.bin"}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36},
                  {"buffer": 0, "byteOffset": 36, "byteLength": 36},
                  {"buffer": 0, "byteOffset": 72, "byteLength": 12},
                  {"buffer": 0, "byteOffset": 84, "byteLength": 48},
                  {"buffer": 0, "byteOffset": 132, "byteLength": 6},
                  {"buffer": 0, "byteOffset": 140, "byteLength": 128}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
                {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 2, 0], "max": [1, 3, 0]},
                {"bufferView": 2, "componentType": 5121, "count": 3, "type": "VEC4"},
                {"bufferView": 3, "componentType": 5126, "count": 3, "type": "VEC4"},
                {"bufferView": 4, "componentType": 5123, "count": 3, "type": "SCALAR"},
                {"bufferView": 5, "componentType": 5126, "count": 2, "type": "MAT4"}]
})");
        std::string error;
        const std::optional<ide::ImportedModel> model =
            ide::import_model_file(resources, ide::utf8_path(source / "Arm.gltf"), error);
        expect(model.has_value(), "a skinned glTF file imports");
        if (!model) {
            std::fprintf(stderr, "  %s\n", error.c_str());
        }
        if (model && model->meshes.size() == 2) {
            bool skins_fine = true;
            for (const std::string& note : model->notes) {
                expect(note.find("came in static") == std::string::npos, "a skinned mesh no longer comes in static");
            }
            std::vector<anarchy::amesh::Data> meshes(2);
            for (std::size_t m = 0; m < 2; ++m) {
                expect(ReadMesh(resources / ide::path_from_utf8(model->meshes[m].path), meshes[m]),
                       "each skinned mesh is an AMESH file");
            }
            const anarchy::amesh::Data& upper = meshes[0];
            expect(upper.bones.size() == 2 && upper.bones[0].name == "Root" && upper.bones[1].name == "Hand" &&
                       upper.bones[1].parent == 0 && upper.bones[0].parent == anarchy::amesh::kNoBone,
                   "it keeps the skeleton's joints, parents first");
            expect(meshes[1].bones.size() == upper.bones.size(), "every mesh of the file carries the same bones");
            for (std::size_t b = 0; b < upper.bones.size() && b < meshes[1].bones.size(); ++b) {
                const anarchy::amesh::Bone& a = upper.bones[b];
                const anarchy::amesh::Bone& c = meshes[1].bones[b];
                skins_fine = skins_fine && a.name == c.name && a.parent == c.parent &&
                             std::memcmp(a.m, c.m, sizeof(a.m)) == 0 && std::memcmp(a.t, c.t, sizeof(a.t)) == 0;
            }
            expect(skins_fine, "the same bones, the same way, in each");
            if (upper.bones.size() == 2) {
                const anarchy::amesh::Bone& hand = upper.bones[1];
                expect(Near(hand.t[0], 0) && Near(hand.t[1], 2) && Near(hand.t[2], 5) && Near(hand.m[0][0], 1) &&
                           Near(hand.m[1][1], 1) && Near(hand.m[2][2], 1),
                       "a joint rests where the file binds it, in the model's space");
                expect(hand.cull_radius > 0.9f, "a joint's cull radius reaches the vertices it moves");
            }
            bool placed = true;
            bool weighed = true;
            bool blended = false;
            for (const anarchy::amesh::Data& data : meshes) {
                for (const anarchy::amesh::Vertex& vertex : data.vertices) {
                    placed = placed && Near(vertex.p[2], 5);
                    float total = 0.f;
                    for (int k = 0; k < 4; ++k) {
                        total += vertex.weight[k];
                        blended = blended || (vertex.bone[k] == 1 && std::fabs(vertex.weight[k] - 0.25f) < 0.01f);
                    }
                    weighed = weighed && std::fabs(total - 1.f) < 0.01f;
                }
            }
            expect(placed, "skinned vertices are in the model's space, where they rest");
            expect(weighed, "every skinned vertex's weights add to 1");
            expect(blended, "a vertex shared by two joints keeps both weights");
        } else if (model) {
            expect(false, "one mesh for each of its two materials");
        }
    }

    // An OBJ with three materials, one drawn by two objects, dropped on the studio.
    const fs::path root = folder / "ModelPlace";
    const fs::path outside = folder / "Downloads" / "crate";
    WriteBytes(outside / "Crate.obj", R"(mtllib Crate.mtl
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
v 2 0 0
v 3 0 0
v 2 1 0
vt 0 0
vt 1 0
vt 1 1
vt 0 1
o Box
usemtl Wood
f 1/1 2/2 3/3 4/4
o Tri
usemtl Paint
f 5/1 6/2 7/3
o Lid
usemtl Wood
f 5/1 6/2 7/3
o Trim
usemtl Stain
f 1/1 2/2 3/3
)");
    // Wood names a folder on the artist's machine; the file is under tex/, in other case.
    WriteBytes(outside / "Crate.mtl", R"(newmtl Wood
Kd 0.8 0.8 0.8
Ns 10
map_Kd C:\artist\maps\Wood.png
map_Bump missing_normal.png

newmtl Paint
Kd 1 0 0
Ns 100

newmtl Stain
Kd 0.5 0.5 0.5
Ns 20
map_Kd tex/WOOD.png
)");
    WriteBytes(outside / "tex" / "WOOD.png", "wood bytes");
    layout.simulation().on_simulation([&](engine_core::DataModel&) {
        engine_core::Project project = engine_core::Project::create(root);
        project.save();
    });
    layout.open_project_at(root);

    auto button = [&scene](const char* id) { return dynamic_cast<jadefx::Button*>(scene.getElementById(id)); };
    auto click = [&button](const char* id) {
        if (jadefx::Button* found = button(id)) {
            found->fire();
        }
    };
    auto drop = [&scene](const std::vector<fs::path>& files) {
        std::vector<std::string> paths;
        for (const fs::path& file : files) {
            paths.push_back(ide::utf8_path(file));
        }
        return scene.noteFileDrop(640, 400, std::move(paths));
    };
    auto prefabs = [&layout] {
        std::vector<engine_core::InstanceId> found;
        layout.simulation().on_simulation(
            [&found](engine_core::DataModel& game) { found = game.get_children(game.service("Prefabs")); });
        return found;
    };

    expect(drop({outside / "Crate.obj"}), "a drop with a model is taken");
    expect(button("import-files-import") != nullptr, "and asks before importing");
    click("import-files-cancel");
    expect(prefabs().empty(), "Cancel makes no Prefab");
    expect(!fs::exists(root / "resources" / "meshes" / "Crate"), "and writes no mesh");

    drop({outside / "Crate.obj"});
    click("import-files-import");
    const std::vector<engine_core::InstanceId> made = prefabs();
    expect(made.size() == 1, "Import makes one Prefab");
    layout.simulation().on_simulation([&](engine_core::DataModel& game) {
        if (made.size() != 1) {
            return;
        }
        const engine_core::InstanceId prefab = made[0];
        expect(game.name(prefab) == "Crate", "named after the file");
        expect(game.selection().get() == std::vector<engine_core::InstanceId>{prefab}, "and selected");

        auto folder_in = [&game](const char* category) {
            const std::vector<engine_core::InstanceId> children = game.get_children(game.service(category));
            return children.size() == 1 && game.name(children[0]) == "Crate" ? children[0] : 0;
        };
        const engine_core::InstanceId textures = folder_in("Textures");
        const engine_core::InstanceId materials = folder_in("Materials");
        const engine_core::InstanceId meshes = folder_in("Meshes");
        expect(textures != 0 && materials != 0 && meshes != 0,
               "a Crate folder holds its assets in Textures, Materials, and Meshes");
        if (textures == 0 || materials == 0 || meshes == 0) {
            return;
        }

        const std::vector<engine_core::InstanceId> texture_ids = game.get_children(textures);
        expect(texture_ids.size() == 1, "one Texture for the file both materials use");
        engine_core::InstanceId wood = 0;
        if (texture_ids.size() == 1) {
            wood = texture_ids[0];
            const auto* texture = dynamic_cast<const engine_core::Texture*>(game.instance(wood));
            expect(texture != nullptr && texture->path() == "textures/Crate/WOOD.png" && game.name(wood) == "WOOD",
                   "found by name though the file named another folder, and copied under textures/<model>");
        }

        std::vector<MaterialView> views;
        for (engine_core::InstanceId id : game.get_children(materials)) {
            if (const auto* material = dynamic_cast<const engine_core::Material*>(game.instance(id))) {
                views.push_back({game.name(id), material->color(), material->roughness(),
                                 Target(*material, engine_core::Material::kDiffuseTextureReference),
                                 Target(*material, engine_core::Material::kNormalTextureReference)});
            }
        }
        expect(views.size() == 3, "a Material for each material the meshes use");
        if (views.size() == 3) {
            expect(views[0].name == "Wood" && views[1].name == "Paint" && views[2].name == "Stain",
                   "named as the file names them, in the order drawn");
            expect(views[0].diffuse == wood && views[2].diffuse == wood, "both point at the one Texture");
            expect(views[0].normal == 0, "a normal map not found is left unset");
            expect(same_color(views[0].color, engine_core::ColorRgb{1.f, 1.f, 1.f, 1.f}),
                   "a material with a diffuse map is white, so the map shows as it is");
            expect(Near(views[1].color.r, 1) && Near(views[1].color.g, 0) && Near(views[1].color.b, 0),
                   "one without keeps its diffuse color");
            expect(Near(views[1].roughness, std::sqrt(2.0 / 102.0)), "shininess becomes roughness");
        }

        const std::vector<engine_core::InstanceId> mesh_ids = game.get_children(meshes);
        const std::vector<engine_core::InstanceId> model_ids = game.get_children(prefab);
        expect(mesh_ids.size() == 3 && model_ids.size() == 3, "a Mesh and a Model for each material");
        if (mesh_ids.size() == 3 && model_ids.size() == 3) {
            const auto* mesh = dynamic_cast<const engine_core::Mesh*>(game.instance(mesh_ids[0]));
            expect(mesh != nullptr && mesh->path() == "meshes/Crate/Wood.amesh", "its Path at its AMESH file");
            const auto* model = dynamic_cast<const engine_core::Model*>(game.instance(model_ids[0]));
            const std::vector<engine_core::InstanceId> material_ids = game.get_children(materials);
            expect(model != nullptr && game.name(model_ids[0]) == "Wood" &&
                       Target(*model, engine_core::Model::kMeshReference) == mesh_ids[0] &&
                       Target(*model, engine_core::Model::kMaterialReference) == material_ids[0],
                   "each Model joins its Mesh and Material");
        }
    });

    anarchy::amesh::Data wood_mesh;
    expect(ReadMesh(root / "resources" / "meshes" / "Crate" / "Wood.amesh", wood_mesh) &&
               wood_mesh.indices.size() == 9,
           "the Wood mesh holds both objects that draw it: three triangles");
    expect(ReadBytes(root / "resources" / "textures" / "Crate" / "WOOD.png") == "wood bytes",
           "the texture is copied into the project");

    // Again, beside the first: its files go in a folder of their own.
    drop({outside / "Crate.obj"});
    click("import-files-import");
    expect(prefabs().size() == 2, "a second import adds a second Prefab");
    expect(fs::exists(root / "resources" / "meshes" / "Crate-2" / "Wood.amesh"),
           "whose meshes go in meshes/Crate-2, leaving the first import's alone");

    std::error_code error;
    fs::remove_all(folder, error);
    return failures;
}
