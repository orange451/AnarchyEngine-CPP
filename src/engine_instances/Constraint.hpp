#pragma once

#include "DataModel.hpp"
#include "InstanceRef.hpp"

#include <optional>
#include <string>

namespace engine_core {

// Joins two PVInstances into one assembly (Assemblies), so they move as one
// rigid thing. Abstract: the Lua class Constraint cannot be made;
// WeldConstraint and RigidConstraint can. It has no Transform of its own and
// acts only while it and both its ends are in Workspace.
//
// PVInstance1  PVInstance?  nil. Saved, by GUID. For a RigidConstraint, the parent.
// PVInstance2  PVInstance?  nil. Saved, by GUID. For a RigidConstraint, the sub,
//                           whose pivot is held on the parent's.
// Enabled      boolean      true. Saved. Off disconnects it without deleting it.
// Active       boolean      read-only, not saved: whether it holds anything now.
//
// An end that names a dead instance reads nil and keeps its GUID, so an undo
// that brings the instance back heals it, as every reference does.
class Constraint : public DataModel {
public:
    Constraint(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : DataModel(tag, state, id) {}

    bool constraint() const override { return true; }
    // True for a RigidConstraint, whose sub is snapped onto its parent.
    virtual bool rigid() const = 0;

    LuaSlot pv_instance1() const;
    LuaSlot pv_instance2() const;
    // The live PVInstance an end names, or 0. Copies no string, so it
    // allocates nothing.
    InstanceId pv_instance1_id() const;
    InstanceId pv_instance2_id() const;
    const InstanceRef& pv_instance1_ref() const { return ref1_; }
    const InstanceRef& pv_instance2_ref() const { return ref2_; }
    bool enabled() const { return enabled_; }

    // SimulationThread. Each returns why it refused the value, changing nothing.
    std::optional<std::string> set_pv_instance1(const LuaSlot& value);
    std::optional<std::string> set_pv_instance2(const LuaSlot& value);
    std::optional<std::string> set_enabled(bool enabled);

protected:
    void on_reuse() override;

private:
    InstanceId end_id(const InstanceRef& ref) const;

    InstanceRef ref1_;
    InstanceRef ref2_;
    bool enabled_ = true;
};

// Holds two PVInstances where they were placed: in play, the offset between
// them when it became active is kept.
class WeldConstraint final : public Constraint {
public:
    WeldConstraint(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : Constraint(tag, state, id) {}
    const char* class_name() const override { return "WeldConstraint"; }
    bool rigid() const override { return false; }
};

// Holds PVInstance2's pivot on PVInstance1's: live in the editor, and as a
// weld with no offset in play.
class RigidConstraint final : public Constraint {
public:
    RigidConstraint(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : Constraint(tag, state, id) {}
    const char* class_name() const override { return "RigidConstraint"; }
    bool rigid() const override { return true; }
};

}  // namespace engine_core
