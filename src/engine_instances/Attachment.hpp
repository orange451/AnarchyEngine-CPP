#pragma once

#include "PVInstance.hpp"
#include "Matrix4.hpp"

#include <optional>
#include <string>

namespace engine_core {

// A point held at an offset from its parent PVInstance, as a Roblox
// Attachment is. It draws nothing.
//
// Offset     Matrix4  from the parent's Transform to this one's. Identity.
// Transform  Matrix4  where it is in the world: the parent's Transform times
//                     Offset, or Offset alone when the parent is not a
//                     PVInstance. Not saved.
//
// Offset is all it stores, so a new parent, or a parent that moves, carries
// the Attachment with it. Writing Transform solves for the Offset that puts it
// there, under the parent it has now; a parent whose Transform has no inverse
// refuses it. Offset is a saved registry property, so DataModel saves, loads,
// undoes, and restores it at Stop, and a Transform write is undone as the
// Offset write it made.
class Attachment : public PVInstance {
public:
    Attachment(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : PVInstance(tag, state, id) {}

    const char* class_name() const override { return "Attachment"; }

    Matrix4 offset() const { return offset_; }
    // The parent's Transform, or identity when the parent is not a PVInstance.
    Matrix4 parent_transform() const;
    // A dead instance's is a zero matrix, as every PVInstance's is.
    Matrix4 transform() const override;

    // SimulationThread. Each returns why it refused the value, changing nothing.
    std::optional<std::string> set_offset(const Matrix4& offset);
    std::optional<std::string> set_transform(const Matrix4& transform);
    std::optional<std::string> set_pv_transform(const Matrix4& transform) override { return set_transform(transform); }

protected:
    void on_reuse() override;
    // Transform moves with the parent, so it fires Changed as Offset would.
    void on_parent_changed(InstanceId previous, InstanceId next) override;

private:
    Matrix4 offset_ = matrix4_identity();
};

}  // namespace engine_core
