// Writes the Ocean sample project: a sea mesh drawn with samples/ocean's
// Shader, whose vertex stage moves it every frame. Run by hand:
//     OCEAN_DEMO=<empty folder> sandbox "[.ocean-demo]"

#include "support.hpp"

#include "AssetInstances.hpp"
#include "Brush.hpp"
#include "Camera.hpp"
#include "DynamicSky.hpp"
#include "GameObject.hpp"
#include "MeshShapes.hpp"
#include "Project.hpp"
#include "Shader.hpp"
#include "amesh.hpp"
#include "brush/BrushGeometry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace {

using engine_core::InstanceId;

std::string read_sample(const char* name) {
    const std::filesystem::path file = std::filesystem::path(__FILE__).parent_path().parent_path() / "samples" / "ocean" / name;
    std::ifstream in(file, std::ios::binary);
    REQUIRE(in);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

engine_core::LuaSlot id_of(engine_core::InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

// A sea 6000 units across whose grid is fine in the middle, about 2 units
// out to 60, and wide toward the edge, where the swells have died down.
anarchy::amesh::Data sea() {
    anarchy::amesh::Data data;
    engine_core::add_plane(data, 2.f, 2.f, engine_core::Vec3{}, 256);
    const auto warp = [](float t) { return 200.f * t + 2800.f * t * t * t * t * t; };
    for (auto& vertex : data.vertices) {
        vertex.p[0] = warp(vertex.p[0]);
        vertex.p[2] = warp(vertex.p[2]);
    }
    return data;
}

}  // namespace

TEST_CASE("the Ocean sample project", "[.ocean-demo]") {
    const char* where = std::getenv("OCEAN_DEMO");
    REQUIRE(where != nullptr);
    SimRole role;
    engine_core::Game game;
    engine_core::Project project = engine_core::Project::create(std::filesystem::path(where), game);

    // The new project's sky and camera: a sun setting in the west, and the
    // camera low over the water looking into it, for the glitter.
    engine_core::DynamicSky* sky = nullptr;
    for (InstanceId child : game.get_children(game.scene_service("Lighting"))) {
        sky = sky != nullptr ? sky : dynamic_cast<engine_core::DynamicSky*>(game.instance(child));
    }
    engine_core::Camera* camera = nullptr;
    for (InstanceId child : game.get_children(game.scene_service("Workspace"))) {
        camera = camera != nullptr ? camera : dynamic_cast<engine_core::Camera*>(game.instance(child));
    }
    REQUIRE(sky != nullptr);
    REQUIRE(camera != nullptr);
    REQUIRE_FALSE(sky->set_time_of_day(17.8));
    camera->set_transform(engine_core::matrix4_look_at(engine_core::Vec3{0.f, 4.f, 0.f},
                                                       engine_core::Vec3{-60.f, 2.f, 0.f},
                                                       engine_core::Vec3{0.f, 1.f, 0.f}));

    engine_core::Shader& shader = game.create<engine_core::Shader>();
    game.set_name(shader.id(), "Ocean");
    game.set_parent(shader.id(), game.service("Shaders"));
    shader.set_source(read_sample("Ocean.glsl"));
    shader.set_vertex_source(read_sample("Ocean.vert.glsl"));
    REQUIRE_FALSE(shader.set_casts_shadow(false));
    REQUIRE_FALSE(shader.set_displacement_margin(3.0));
    REQUIRE(shader.error().empty());

    engine_core::Material& water = game.create<engine_core::Material>();
    game.set_name(water.id(), "Ocean");
    game.set_parent(water.id(), game.service("Materials"));
    REQUIRE_FALSE(water.set_reference(engine_core::Material::kShaderReference, id_of(shader.id())));

    const std::filesystem::path mesh_file = project.resources_root() / "meshes" / "Ocean.amesh";
    std::filesystem::create_directories(mesh_file.parent_path());
    {
        const std::vector<std::byte> bytes = anarchy::amesh::write(sea());
        std::ofstream out(mesh_file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    engine_core::Mesh& mesh = game.create<engine_core::Mesh>();
    game.set_name(mesh.id(), "Ocean");
    game.set_parent(mesh.id(), game.service("Meshes"));
    REQUIRE_FALSE(mesh.set_path("meshes/Ocean.amesh"));

    engine_core::Prefab& prefab = game.create<engine_core::Prefab>();
    game.set_name(prefab.id(), "Ocean");
    game.set_parent(prefab.id(), game.service("Prefabs"));
    engine_core::Model& model = game.create<engine_core::Model>();
    game.set_name(model.id(), "Sea");
    game.set_parent(model.id(), prefab.id());
    REQUIRE_FALSE(model.set_reference(engine_core::Model::kMeshReference, id_of(mesh.id())));
    REQUIRE_FALSE(model.set_reference(engine_core::Model::kMaterialReference, id_of(water.id())));
    engine_core::GameObject& ocean = game.create_game_object();
    game.set_name(ocean.id(), "Ocean");
    REQUIRE_FALSE(ocean.set_prefab(id_of(prefab.id())));
    game.set_parent(ocean.id(), game.scene_service("Workspace"));

    // Three sea stacks for the waves to wash around.
    engine_core::Material& stone = game.create<engine_core::Material>();
    game.set_name(stone.id(), "Rock");
    game.set_parent(stone.id(), game.service("Materials"));
    REQUIRE_FALSE(stone.set_color({0.36f, 0.33f, 0.3f, 1.f}));
    // Each a hull of points on a ball w by h by d, pushed in and out by seed,
    // narrowing toward the top; its foot 3 units under the sea.
    const auto rock = [&](const char* name, float x, float z, double w, double h, double d, int seed) {
        std::vector<engine_core::brush::DVec3> points;
        for (int ring = 0; ring <= 6; ++ring) {
            const double up = -1.0 + ring / 3.0;
            for (int step = 0; step < 9; ++step) {
                const double hash = std::fmod(std::sin((seed * 97 + ring * 13 + step * 7) * 12.9898) * 43758.5453, 1.0);
                const double jitter = 0.75 + 0.25 * std::abs(hash);
                const double around = (step + 0.5 * (ring % 2)) * 6.2831853 / 9.0;
                const double width = std::sqrt(std::max(0.05, 1.0 - up * up)) * (1.0 - 0.35 * (up + 1.0) * 0.5);
                points.push_back({std::cos(around) * width * jitter * w * 0.5, up * h * 0.5,
                                  std::sin(around) * width * jitter * d * 0.5});
            }
        }
        std::optional<std::vector<engine_core::brush::Face>> faces = engine_core::brush::hull_faces(points);
        REQUIRE(faces.has_value());
        for (engine_core::brush::Face& face : *faces) {
            face.material = game.guid(stone.id());
        }
        engine_core::Brush& made = game.create<engine_core::Brush>();
        game.set_name(made.id(), name);
        REQUIRE_FALSE(made.set_faces(std::move(*faces)));
        REQUIRE_FALSE(made.set_transform(engine_core::matrix4_translation(x, static_cast<float>(h) * 0.5f - 3.f, z)));
        game.set_parent(made.id(), game.scene_service("Workspace"));
    };
    rock("Stack", -70.f, -24.f, 10.0, 20.0, 8.0, 1);
    rock("StackSmall", -60.f, -14.f, 6.0, 9.0, 6.0, 2);
    rock("Reef", -120.f, 34.f, 18.0, 7.0, 10.0, 3);
    project.save();
}
