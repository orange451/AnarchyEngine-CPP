#pragma once

#include "Matrix4.hpp"
#include "types.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace anarchy::amesh {
struct Bone;
}

namespace engine_core {

// A skinned Mesh's bones, as its AMESH bone table holds them, ready to pose.
// Immutable once made, so a Mesh, a GameObject's Pose, and a render snapshot
// can share one.
struct Skeleton {
    struct Bone {
        std::string name;
        // Into bones, or 0xFFFF for a root.
        std::uint16_t parent = 0xFFFF;
        // Model space: where the bone is when nothing poses it.
        Matrix4 rest;
        // The parent's rest inverse times rest: rest for a root.
        Matrix4 rest_local;
        // rest's inverse, which takes a rest-pose vertex into the bone's frame.
        Matrix4 inverse_bind;
        // How far, in units, the vertices it moves reach from its origin.
        float cull_radius = 0.f;
    };
    std::vector<Bone> bones;
    // Every bone once, each after its parent.
    std::vector<std::uint16_t> order;
    // Of the names, parents, and rest transforms: two equal tables have the
    // same one, so meshes cut from one skeleton can share a pose.
    std::uint64_t signature = 0;

    // The first bone with each name.
    std::unordered_map<std::string, std::uint16_t> by_name;

    // The first bone with this name, or -1.
    int find(const std::string& name) const;
};

// The skeleton of an AMESH bone table, whose m and t are each bone's rest
// transform in model space. Null for an empty table. The reader has already
// refused a table whose parents form a cycle.
std::shared_ptr<const Skeleton> make_skeleton(const std::vector<anarchy::amesh::Bone>& bones);

// One bone's Offset, added after its local transform, and the Bone instance
// that set it.
struct PoseInput {
    std::uint16_t bone = 0;
    Matrix4 offset = matrix4_identity();
    InstanceId owner = 0;
};

// A skeleton posed: one entry per bone in each list.
struct Pose {
    std::shared_ptr<const Skeleton> skeleton;
    // Each bone's local transform before its Offset: rest_local, times the
    // animation layer's change when there is one.
    std::vector<Matrix4> locals;
    // Model space, with every Offset applied.
    std::vector<Matrix4> globals;
    // 12 floats a bone, globals times inverse_bind as its first three rows, as
    // the vertex shaders read them.
    std::vector<float> palette;
    // The Bone instance whose Offset posed each bone, or 0.
    std::vector<InstanceId> owners;
    // Model space: every bone's origin, grown by its cull radius.
    Vec3 low{};
    Vec3 high{};
    // Unique to this pose among every one compute_pose made, so a shadow map
    // cached from it can tell when it is posed again. Never 0.
    std::uint64_t revision = 0;
};

// skeleton posed by inputs, each bone's Offset after its local transform, and,
// when animated holds one matrix per bone, each bone's animated change from
// rest between the two: local = rest_local * animated * Offset. A layer of
// another size is ignored. A bone named twice takes the last input; one out
// of range is ignored.
Pose compute_pose(std::shared_ptr<const Skeleton> skeleton, const std::vector<PoseInput>& inputs,
                  const std::vector<Matrix4>* animated = nullptr);

}  // namespace engine_core
