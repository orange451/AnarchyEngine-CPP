// Skeleton: an AMESH bone table as rest, rest-local, and inverse-bind matrices,
// and the pose a set of additive Offsets makes of it. Mesh::skeleton and the
// Prefab's skeleton, read from AMESH files under a resources folder.

#include "skinning_support.hpp"

#include "Skeleton.hpp"
#include "Skinning.hpp"

#include <chrono>
#include <thread>

using namespace skinning_test;

namespace {

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
