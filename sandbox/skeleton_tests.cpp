// Skeleton: an AMESH bone table as rest, rest-local, and inverse-bind matrices,
// and the pose a set of additive Offsets makes of it.

#include "support.hpp"

#include "Matrix4.hpp"
#include "Skeleton.hpp"
#include "amesh.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

namespace {

using engine_core::Matrix4;
using engine_core::Vec3;
namespace amesh = anarchy::amesh;

constexpr double kQuarter = 1.5707963267948966;

bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

bool near_matrix(const Matrix4& a, const Matrix4& b) {
    for (int i = 0; i < 16; ++i) {
        if (!near(a.m[i], b.m[i])) {
            return false;
        }
    }
    return true;
}

bool near_vec(Vec3 a, Vec3 b) { return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z); }

// A bone at value, as AMESH stores it: a row-major 3x3 and a translation.
amesh::Bone bone(const char* name, std::uint16_t parent, const Matrix4& value, float cull = 0.f) {
    amesh::Bone out;
    out.name = name;
    out.parent = parent;
    out.cull_radius = cull;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            out.m[row][column] = value.m[column * 4 + row];
        }
    }
    out.t[0] = value.m[12], out.t[1] = value.m[13], out.t[2] = value.m[14];
    return out;
}

Matrix4 spin_z() { return engine_core::matrix4_axis_angle(Vec3{0.f, 0.f, 1.f}, kQuarter); }

// root at the origin, mid 1 unit up, tip 1 unit above mid and turned a quarter about Z.
std::vector<amesh::Bone> chain() {
    return {bone("root", amesh::kNoBone, engine_core::matrix4_identity(), 0.5f),
            bone("mid", 0, engine_core::matrix4_translation(0.f, 1.f, 0.f), 0.5f),
            bone("tip", 1, engine_core::matrix4_multiply(engine_core::matrix4_translation(0.f, 2.f, 0.f), spin_z()),
                 0.25f)};
}

}  // namespace

TEST_CASE("SKEL1 a bone table gives rest, rest-local, and inverse-bind matrices", "[skeleton]") {
    const auto skeleton = engine_core::make_skeleton(chain());
    REQUIRE(skeleton != nullptr);
    REQUIRE(skeleton->bones.size() == 3);
    REQUIRE(near_matrix(skeleton->bones[1].rest_local, engine_core::matrix4_translation(0.f, 1.f, 0.f)));
    REQUIRE(near_matrix(skeleton->bones[2].rest_local,
                        engine_core::matrix4_multiply(engine_core::matrix4_translation(0.f, 1.f, 0.f), spin_z())));
    for (const engine_core::Skeleton::Bone& each : skeleton->bones) {
        REQUIRE(near_matrix(engine_core::matrix4_multiply(each.inverse_bind, each.rest), engine_core::matrix4_identity()));
    }
    REQUIRE(skeleton->find("tip") == 2);
    REQUIRE(skeleton->find("nope") == -1);
    REQUIRE(engine_core::make_skeleton({}) == nullptr);
}

TEST_CASE("SKEL2 with no Offsets the pose is the rest pose", "[skeleton]") {
    const auto skeleton = engine_core::make_skeleton(chain());
    const engine_core::Pose pose = engine_core::compute_pose(skeleton, {});
    REQUIRE(pose.palette.size() == 36);
    for (std::size_t b = 0; b < 3; ++b) {
        REQUIRE(near_matrix(pose.globals[b], skeleton->bones[b].rest));
        const float* rows = pose.palette.data() + b * 12;
        const float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        for (int i = 0; i < 12; ++i) {
            REQUIRE(near(rows[i], identity[i]));
        }
        REQUIRE(pose.owners[b] == 0);
    }
    // Every origin, grown by its cull radius.
    REQUIRE(near_vec(pose.low, Vec3{-0.5f, -0.5f, -0.5f}));
    REQUIRE(near_vec(pose.high, Vec3{0.5f, 2.25f, 0.5f}));
}

TEST_CASE("SKEL3 an Offset turns its bone and carries the bones below it", "[skeleton]") {
    const auto skeleton = engine_core::make_skeleton(chain());
    const engine_core::Pose pose = engine_core::compute_pose(skeleton, {{1, spin_z(), 77}});
    REQUIRE(near_vec(engine_core::matrix4_position(pose.globals[1]), Vec3{0.f, 1.f, 0.f}));
    REQUIRE(near_vec(engine_core::matrix4_position(pose.globals[2]), Vec3{-1.f, 1.f, 0.f}));
    REQUIRE(pose.owners[1] == 77);
    REQUIRE(pose.owners[2] == 0);
    // The skinning matrix carries a rest-pose point on tip to where tip is now.
    const Matrix4 skin = engine_core::matrix4_multiply(pose.globals[2], skeleton->bones[2].inverse_bind);
    REQUIRE(near_vec(engine_core::matrix4_point(skin, Vec3{0.f, 2.f, 0.f}), Vec3{-1.f, 1.f, 0.f}));
    REQUIRE(near(pose.palette[2 * 12 + 3], skin.m[12]));
    REQUIRE(near(pose.palette[2 * 12 + 4], skin.m[1]));
}

TEST_CASE("SKEL4 an Offset is added after the bone's own local transform", "[skeleton]") {
    const auto skeleton = engine_core::make_skeleton(chain());
    const engine_core::Pose pose =
        engine_core::compute_pose(skeleton, {{1, engine_core::matrix4_translation(1.f, 0.f, 0.f), 5}});
    REQUIRE(near_vec(engine_core::matrix4_position(pose.globals[1]), Vec3{1.f, 1.f, 0.f}));
    REQUIRE(near_matrix(pose.locals[1], skeleton->bones[1].rest_local));
}

TEST_CASE("SKEL5 a table that lists children before parents still poses", "[skeleton]") {
    std::vector<amesh::Bone> reversed = chain();
    // tip, mid, root
    std::swap(reversed[0], reversed[2]);
    reversed[0].parent = 1;
    reversed[1].parent = 2;
    reversed[2].parent = amesh::kNoBone;
    const auto skeleton = engine_core::make_skeleton(reversed);
    REQUIRE(near_matrix(skeleton->bones[0].rest_local,
                        engine_core::matrix4_multiply(engine_core::matrix4_translation(0.f, 1.f, 0.f), spin_z())));
    const engine_core::Pose pose = engine_core::compute_pose(skeleton, {{2, spin_z(), 1}});
    REQUIRE(near_vec(engine_core::matrix4_position(pose.globals[0]), Vec3{-2.f, 0.f, 0.f}));
}

TEST_CASE("SKEL6 equal tables share a signature; a renamed bone does not", "[skeleton]") {
    const auto a = engine_core::make_skeleton(chain());
    const auto b = engine_core::make_skeleton(chain());
    std::vector<amesh::Bone> renamed = chain();
    renamed[2].name = "toe";
    const auto c = engine_core::make_skeleton(renamed);
    REQUIRE(a->signature == b->signature);
    REQUIRE(a->signature != c->signature);
}

// Mesh::skeleton and the Prefab's skeleton, read from AMESH files under a resources folder.

#include "AssetInstances.hpp"
#include "GameObject.hpp"
#include "Skinning.hpp"

#include <chrono>
#include <fstream>
#include <thread>

namespace {

engine_core::LuaSlot id_slot(engine_core::InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

// A triangle whose vertices follow bones: count bones in a chain up Y, 2 units apart.
amesh::Data arm(int count) {
    amesh::Data data;
    for (int v = 0; v < 3; ++v) {
        amesh::Vertex vertex;
        vertex.p[0] = static_cast<float>(v);
        vertex.p[1] = static_cast<float>(v);
        if (count > 0) {
            vertex.bone[0] = static_cast<std::uint16_t>(v % count);
            vertex.weight[0] = 1.f;
        }
        data.vertices.push_back(vertex);
    }
    data.indices = {0, 1, 2};
    const char* names[] = {"Root", "Hand", "Finger"};
    for (int b = 0; b < count; ++b) {
        data.bones.push_back(bone(names[b], b == 0 ? amesh::kNoBone : static_cast<std::uint16_t>(b - 1),
                                  engine_core::matrix4_translation(0.f, 2.f * static_cast<float>(b), 0.f), 1.f));
    }
    amesh::compute_aabb(data);
    return data;
}

void write_mesh(const std::filesystem::path& file, const amesh::Data& data) {
    std::filesystem::create_directories(file.parent_path());
    const std::vector<std::byte> bytes = amesh::write(data);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// A game whose resources folder holds meshes/arm.amesh (two bones) and meshes/box.amesh (none).
struct Rig {
    SimRole role;
    TempDir dir;
    engine_core::Game game;

    Rig() {
        write_mesh(dir / "meshes" / "arm.amesh", arm(2));
        write_mesh(dir / "meshes" / "box.amesh", arm(0));
        game.set_resources_root(dir.path);
    }

    engine_core::Mesh& mesh(const char* path) {
        engine_core::Mesh& made = game.create<engine_core::Mesh>();
        game.set_parent(made.id(), game.service("Meshes"));
        REQUIRE_FALSE(made.set_path(path));
        return made;
    }

    engine_core::Prefab& prefab(std::initializer_list<engine_core::InstanceId> meshes) {
        engine_core::Prefab& made = game.create<engine_core::Prefab>();
        game.set_parent(made.id(), game.service("Prefabs"));
        for (const engine_core::InstanceId mesh : meshes) {
            engine_core::Model& model = game.create<engine_core::Model>();
            game.set_parent(model.id(), made.id());
            REQUIRE_FALSE(model.set_reference(engine_core::Model::kMeshReference, id_slot(mesh)));
        }
        return made;
    }
};

}  // namespace

TEST_CASE("SKEL7 a Mesh's skeleton comes from its AMESH file", "[skeleton]") {
    Rig rig;
    const auto skeleton = rig.mesh("meshes/arm.amesh").skeleton();
    REQUIRE(skeleton != nullptr);
    REQUIRE(skeleton->bones.size() == 2);
    REQUIRE(skeleton->bones[1].name == "Hand");
    REQUIRE(rig.mesh("meshes/box.amesh").skeleton() == nullptr);
    REQUIRE(rig.mesh("meshes/missing.amesh").skeleton() == nullptr);
}

TEST_CASE("SKEL8 a Mesh's skeleton is read once, and again when its file changes", "[skeleton]") {
    Rig rig;
    engine_core::Mesh& mesh = rig.mesh("meshes/arm.amesh");
    const auto first = mesh.skeleton();
    REQUIRE(mesh.skeleton() == first);
    // A file time that moves on, however coarse the file system's clock.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    write_mesh(rig.dir / "meshes" / "arm.amesh", arm(3));
    std::filesystem::last_write_time(rig.dir / "meshes" / "arm.amesh",
                                     std::filesystem::file_time_type::clock::now() + std::chrono::seconds(5));
    const auto second = mesh.skeleton();
    REQUIRE(second != nullptr);
    REQUIRE(second->bones.size() == 3);
}

TEST_CASE("SKEL9 a Prefab poses with its first Model whose Mesh has bones", "[skeleton]") {
    Rig rig;
    engine_core::Mesh& box = rig.mesh("meshes/box.amesh");
    engine_core::Mesh& arm_mesh = rig.mesh("meshes/arm.amesh");
    engine_core::Prefab& prefab = rig.prefab({box.id(), arm_mesh.id()});
    const auto skeleton = engine_core::prefab_skeleton(rig.game, rig.game.guid(prefab.id()));
    REQUIRE(skeleton != nullptr);
    REQUIRE(skeleton->bones.size() == 2);
    REQUIRE(engine_core::mesh_poses_with(arm_mesh, *skeleton));
    REQUIRE_FALSE(engine_core::mesh_poses_with(box, *skeleton));
    REQUIRE(engine_core::prefab_skeleton(rig.game, rig.game.guid(rig.prefab({}).id())) == nullptr);
    REQUIRE(engine_core::prefab_skeleton(rig.game, "") == nullptr);
}
