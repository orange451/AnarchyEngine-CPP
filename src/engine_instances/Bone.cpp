#include "Bone.hpp"

#include "Containment.hpp"
#include "GameObject.hpp"
#include "LuaApi.hpp"
#include "Skeleton.hpp"

#include <cmath>

namespace engine_core {
namespace {

// The GameObject's Transform, grown by its Scale about its origin, as it draws.
Matrix4 drawn_frame(const GameObject& object) {
    Matrix4 frame = object.transform();
    const float scale = static_cast<float>(object.scale());
    for (int column = 0; column < 3; ++column) {
        for (int axis = 0; axis < 3; ++axis) {
            frame.m[column * 4 + axis] *= scale;
        }
    }
    return frame;
}

bool finite(const Matrix4& value) {
    for (float component : value.m) {
        if (!std::isfinite(component)) {
            return false;
        }
    }
    return true;
}

// The skinned GameObject a Bone is under, its pose, and the bone the Bone poses.
struct Posed {
    const GameObject* object = nullptr;
    std::shared_ptr<const Pose> pose;
    int bone = -1;
};

Posed posed_of(const Bone& bone) {
    Posed out;
    out.object = dynamic_cast<const GameObject*>(bone.instance(bone.parent(bone.id())));
    if (out.object == nullptr) {
        return out;
    }
    out.pose = out.object->pose();
    if (out.pose == nullptr) {
        return out;
    }
    for (std::size_t b = 0; b < out.pose->owners.size(); ++b) {
        if (out.pose->owners[b] == bone.id()) {
            out.bone = static_cast<int>(b);
            break;
        }
    }
    return out;
}

}  // namespace

int Bone::bone_index() const {
    if (!alive(id())) {
        return -1;
    }
    return posed_of(*this).bone;
}

Matrix4 Bone::transform() const {
    if (!alive(id())) {
        return Matrix4{};
    }
    const Posed posed = posed_of(*this);
    if (posed.bone < 0) {
        return Attachment::transform();
    }
    return matrix4_multiply(drawn_frame(*posed.object), posed.pose->globals[static_cast<std::size_t>(posed.bone)]);
}

std::optional<std::string> Bone::set_transform(const Matrix4& transform) {
    const Posed posed = posed_of(*this);
    if (posed.bone < 0) {
        return Attachment::set_transform(transform);
    }
    if (!finite(transform)) {
        return std::string("Transform must be finite");
    }
    // global = parent's global * local * Offset, and Transform = frame * global.
    const auto bone = static_cast<std::size_t>(posed.bone);
    const std::uint16_t parent = posed.pose->skeleton->bones[bone].parent;
    Matrix4 above = drawn_frame(*posed.object);
    if (parent < posed.pose->globals.size()) {
        above = matrix4_multiply(above, posed.pose->globals[parent]);
    }
    above = matrix4_multiply(above, posed.pose->locals[bone]);
    const Matrix4 offset = matrix4_multiply(matrix4_inverse(above), transform);
    if (!finite(offset)) {
        return std::string("Transform cannot be set while the GameObject's Transform has no inverse");
    }
    return set_offset(offset);
}

namespace {

ANARCHY_LUA_REGISTER(register_bone_lua) {
    // Offset, OffsetSpace, and Transform are Attachment's.
    register_lua_class("Bone", "Attachment", nullptr, 0);
    register_suited_parents("Bone", {"GameObject"});
}

}  // namespace

}  // namespace engine_core
