#pragma once

#include "Attachment.hpp"

namespace engine_core {

// An Attachment that poses one bone of the skinned GameObject it is under: the
// bone its Name names.
//
// Offset       Matrix4  added after the bone's own local transform (its rest
//                       pose, or what an animation made of it), in the bone's
//                       frame, so the bones below it move with it. Identity.
// Transform    Matrix4  where the bone is in the world: the GameObject's
//                       Transform, grown by its Scale, times the bone's posed
//                       model-space transform. Not saved. Writing it solves
//                       for the Offset that puts the bone there.
//
// A Bone whose parent is not a skinned GameObject, whose Name names no bone,
// or that comes after another Bone of the same Name, poses nothing: it is a
// plain Attachment, OffsetSpace and all. A matched Bone ignores OffsetSpace.
// Nothing makes a Bone but a script, Insert Object, or GameObject:AddBone.
class Bone : public Attachment {
public:
    using Attachment::Attachment;

    const char* class_name() const override { return "Bone"; }

    // The bone this Bone poses, into its GameObject's skeleton, or -1 when it poses none.
    int bone_index() const;

    Matrix4 transform() const override;
    std::optional<std::string> set_transform(const Matrix4& transform) override;
};

}  // namespace engine_core
