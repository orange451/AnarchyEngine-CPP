#pragma once

// What the skeleton, Bone, and skinned render tests share: matrix comparisons,
// AMESH bone tables, and a game whose resources folder holds a skinned mesh.

#include "support.hpp"

#include "AssetInstances.hpp"
#include "GameObject.hpp"
#include "Matrix4.hpp"
#include "amesh.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <vector>

namespace skinning_test {

using engine_core::Matrix4;
using engine_core::Vec3;
namespace amesh = anarchy::amesh;

inline constexpr double kQuarter = 1.5707963267948966;

inline bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

inline bool near_matrix(const Matrix4& a, const Matrix4& b) {
    for (int i = 0; i < 16; ++i) {
        if (!near(a.m[i], b.m[i])) {
            return false;
        }
    }
    return true;
}

inline bool near_vec(Vec3 a, Vec3 b) { return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z); }

inline Matrix4 spin_z() { return engine_core::matrix4_axis_angle(Vec3{0.f, 0.f, 1.f}, kQuarter); }

// A bone at value, as AMESH stores it: a row-major 3x3 and a translation.
inline amesh::Bone bone(const char* name, std::uint16_t parent, const Matrix4& value, float cull = 0.f) {
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

inline engine_core::LuaSlot id_slot(engine_core::InstanceId id) {
    engine_core::LuaSlot slot;
    slot.kind = engine_core::LuaSlot::Kind::Instance;
    slot.id = id;
    return slot;
}

// A triangle whose vertices follow count bones (none for a static mesh): a
// chain up Y, 2 units apart, named Root, Hand, Finger.
inline amesh::Data arm(int count) {
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

inline void write_mesh(const std::filesystem::path& file, const amesh::Data& data) {
    std::filesystem::create_directories(file.parent_path());
    const std::vector<std::byte> bytes = amesh::write(data);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// A game whose resources folder holds meshes/arm.amesh (Root and Hand) and
// meshes/box.amesh (no bones).
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

    // A Prefab with a Model on each mesh, in order.
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

    // A GameObject in Workspace drawing the arm.
    engine_core::GameObject& arm_object() {
        engine_core::Prefab& made = prefab({mesh("meshes/arm.amesh").id()});
        engine_core::GameObject& object = create_part(game);
        REQUIRE_FALSE(object.set_prefab(id_slot(made.id())));
        return object;
    }
};

}  // namespace skinning_test
