#include "Skeleton.hpp"

#include "amesh.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace engine_core {
namespace {

constexpr std::uint16_t kRoot = 0xFFFF;

// FNV-1a, 64-bit.
void mix(std::uint64_t& hash, const void* bytes, std::size_t size) {
    const auto* at = static_cast<const unsigned char*>(bytes);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= at[i];
        hash *= 1099511628211ull;
    }
}

}  // namespace

int Skeleton::find(const std::string& name) const {
    const auto found = by_name.find(name);
    return found != by_name.end() ? static_cast<int>(found->second) : -1;
}

std::shared_ptr<const Skeleton> make_skeleton(const std::vector<anarchy::amesh::Bone>& table) {
    if (table.empty()) {
        return nullptr;
    }
    auto skeleton = std::make_shared<Skeleton>();
    skeleton->bones.resize(table.size());
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t b = 0; b < table.size(); ++b) {
        const anarchy::amesh::Bone& from = table[b];
        Skeleton::Bone& to = skeleton->bones[b];
        to.name = from.name;
        to.parent = from.parent < table.size() ? from.parent : kRoot;
        to.cull_radius = from.cull_radius;
        to.rest = matrix4_identity();
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                to.rest.m[column * 4 + row] = from.m[row][column];
            }
        }
        to.rest.m[12] = from.t[0], to.rest.m[13] = from.t[1], to.rest.m[14] = from.t[2];
        to.inverse_bind = matrix4_inverse(to.rest);
        skeleton->by_name.emplace(to.name, static_cast<std::uint16_t>(b));
        mix(hash, to.name.data(), to.name.size());
        mix(hash, "\0", 1);
        mix(hash, &to.parent, sizeof(to.parent));
        mix(hash, from.m, sizeof(from.m));
        mix(hash, from.t, sizeof(from.t));
    }
    skeleton->signature = hash;
    for (Skeleton::Bone& each : skeleton->bones) {
        each.rest_local = each.parent == kRoot
                              ? each.rest
                              : matrix4_multiply(skeleton->bones[each.parent].inverse_bind, each.rest);
    }

    // Parents first: each root, then its subtree, depth first.
    std::vector<std::vector<std::uint16_t>> children(table.size());
    std::vector<std::uint16_t> stack;
    for (std::size_t b = table.size(); b-- > 0;) {
        const std::uint16_t parent = skeleton->bones[b].parent;
        if (parent == kRoot) {
            stack.push_back(static_cast<std::uint16_t>(b));
        } else {
            children[parent].push_back(static_cast<std::uint16_t>(b));
        }
    }
    skeleton->order.reserve(table.size());
    while (!stack.empty()) {
        const std::uint16_t b = stack.back();
        stack.pop_back();
        skeleton->order.push_back(b);
        stack.insert(stack.end(), children[b].rbegin(), children[b].rend());
    }
    return skeleton;
}

Pose compute_pose(std::shared_ptr<const Skeleton> skeleton, const std::vector<PoseInput>& inputs,
                  const std::vector<Matrix4>* animated) {
    static std::atomic<std::uint64_t> next_revision{1};
    Pose pose;
    pose.revision = next_revision.fetch_add(1, std::memory_order_relaxed);
    pose.skeleton = std::move(skeleton);
    if (pose.skeleton == nullptr) {
        return pose;
    }
    const std::vector<Skeleton::Bone>& bones = pose.skeleton->bones;
    const std::size_t count = bones.size();
    std::vector<const Matrix4*> offsets(count, nullptr);
    pose.owners.assign(count, 0);
    for (const PoseInput& input : inputs) {
        if (input.bone < count) {
            offsets[input.bone] = &input.offset;
            pose.owners[input.bone] = input.owner;
        }
    }
    pose.locals.resize(count);
    pose.globals.resize(count);
    pose.palette.resize(count * 12);
    for (const std::uint16_t b : pose.skeleton->order) {
        const Skeleton::Bone& bone = bones[b];
        pose.locals[b] = animated != nullptr && animated->size() == count
                             ? matrix4_multiply(bone.rest_local, (*animated)[b])
                             : bone.rest_local;
        const Matrix4 local = offsets[b] != nullptr ? matrix4_multiply(pose.locals[b], *offsets[b]) : pose.locals[b];
        pose.globals[b] = bone.parent == kRoot ? local : matrix4_multiply(pose.globals[bone.parent], local);
        const Matrix4 skin = matrix4_multiply(pose.globals[b], bone.inverse_bind);
        float* rows = pose.palette.data() + std::size_t{b} * 12;
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 4; ++column) {
                rows[row * 4 + column] = skin.m[column * 4 + row];
            }
        }
    }
    for (std::size_t b = 0; b < count; ++b) {
        const Vec3 at = matrix4_position(pose.globals[b]);
        const float r = bones[b].cull_radius;
        const Vec3 low{at.x - r, at.y - r, at.z - r};
        const Vec3 high{at.x + r, at.y + r, at.z + r};
        if (b == 0) {
            pose.low = low;
            pose.high = high;
        } else {
            pose.low = {std::min(pose.low.x, low.x), std::min(pose.low.y, low.y), std::min(pose.low.z, low.z)};
            pose.high = {std::max(pose.high.x, high.x), std::max(pose.high.y, high.y), std::max(pose.high.z, high.z)};
        }
    }
    return pose;
}

}  // namespace engine_core
