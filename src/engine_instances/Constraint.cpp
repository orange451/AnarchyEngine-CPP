#include "Constraint.hpp"

#include "Assemblies.hpp"
#include "Containment.hpp"
#include "Contract.hpp"
#include "LuaApi.hpp"

#include <iterator>
#include <utility>

namespace engine_core {
namespace {

LuaSlot bool_slot(bool value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Bool;
    slot.flag = value;
    return slot;
}

void require_thread(const DataModel& object) {
    if (!object.on_gameplay_thread()) {
        contract_fail("Constraint setters run on SimulationThread");
    }
}

}  // namespace

LuaSlot Constraint::pv_instance1() const { return instance_reference_slot(ref1_, "PVInstance"); }

LuaSlot Constraint::pv_instance2() const { return instance_reference_slot(ref2_, "PVInstance"); }

InstanceId Constraint::end_id(const InstanceRef& ref) const {
    const InstanceId target = ref.resolve(*this);
    if (target == 0) {
        return 0;
    }
    const DataModel* object = instance(target);
    return object != nullptr && lua_class_inherits(object->class_name(), "PVInstance") ? target : 0;
}

InstanceId Constraint::pv_instance1_id() const { return end_id(ref1_); }

InstanceId Constraint::pv_instance2_id() const { return end_id(ref2_); }

std::optional<std::string> Constraint::set_pv_instance1(const LuaSlot& value) {
    require_thread(*this);
    std::optional<std::string> refused = set_instance_reference("PVInstance1", "PVInstance", ref1_, value);
    if (!refused) {
        assemblies().constraint_changed(*this, id());
        link_changed();
    }
    return refused;
}

std::optional<std::string> Constraint::set_pv_instance2(const LuaSlot& value) {
    require_thread(*this);
    std::optional<std::string> refused = set_instance_reference("PVInstance2", "PVInstance", ref2_, value);
    if (!refused) {
        assemblies().constraint_changed(*this, id());
        link_changed();
    }
    return refused;
}

std::optional<std::string> Constraint::set_enabled(bool enabled) {
    require_thread(*this);
    if (enabled == enabled_) {
        return std::nullopt;
    }
    enabled_ = enabled;
    note_property_change("Enabled", bool_slot(!enabled), bool_slot(enabled));
    assemblies().constraint_changed(*this, id());
    link_changed();
    return std::nullopt;
}

void Constraint::on_reuse() {
    ref1_.set_guid(std::string());
    ref2_.set_guid(std::string());
    enabled_ = true;
}

namespace {

bool refuse(LuaSlot& in, std::optional<std::string> error) {
    if (error) {
        in.error = std::move(*error);
        return false;
    }
    return true;
}

Constraint* constraint_of(DataModel& object) { return dynamic_cast<Constraint*>(&object); }

bool read_pv_instance1(DataModel&, DataModel& object, LuaSlot& out) {
    const Constraint* constraint = constraint_of(object);
    if (constraint == nullptr) {
        return false;
    }
    out = constraint->pv_instance1();
    return true;
}

bool write_pv_instance1(DataModel&, DataModel& object, LuaSlot& in) {
    Constraint* constraint = constraint_of(object);
    return constraint != nullptr && refuse(in, constraint->set_pv_instance1(in));
}

bool read_pv_instance2(DataModel&, DataModel& object, LuaSlot& out) {
    const Constraint* constraint = constraint_of(object);
    if (constraint == nullptr) {
        return false;
    }
    out = constraint->pv_instance2();
    return true;
}

bool write_pv_instance2(DataModel&, DataModel& object, LuaSlot& in) {
    Constraint* constraint = constraint_of(object);
    return constraint != nullptr && refuse(in, constraint->set_pv_instance2(in));
}

bool read_enabled(DataModel&, DataModel& object, LuaSlot& out) {
    const Constraint* constraint = constraint_of(object);
    if (constraint == nullptr) {
        return false;
    }
    out = bool_slot(constraint->enabled());
    return true;
}

bool write_enabled(DataModel&, DataModel& object, LuaSlot& in) {
    Constraint* constraint = constraint_of(object);
    return constraint != nullptr && refuse(in, constraint->set_enabled(in.flag));
}

bool read_active(DataModel&, DataModel& object, LuaSlot& out) {
    if (constraint_of(object) == nullptr) {
        return false;
    }
    Assemblies& assemblies = object.assemblies();
    assemblies.refresh(object);
    out = bool_slot(assemblies.active(object.id()));
    return true;
}

ANARCHY_LUA_REGISTER(register_constraint_lua) {
    const LuaField fields[] = {
        lua_group("Constraint"),
        lua_saved_property("PVInstance1", "PVInstance?", read_pv_instance1, write_pv_instance1, "null"),
        lua_saved_property("PVInstance2", "PVInstance?", read_pv_instance2, write_pv_instance2, "null"),
        lua_saved_property("Enabled", "boolean", read_enabled, write_enabled, "true"),
        lua_property("Active", "boolean", false, read_active, nullptr),
    };
    // Abstract: no factory registers it.
    register_lua_class("Constraint", "Instance", fields, static_cast<int>(std::size(fields)));
    register_lua_class("WeldConstraint", "Constraint", nullptr, 0);
    register_lua_class("RigidConstraint", "Constraint", nullptr, 0);
    register_lua_class("NoCollisionConstraint", "Constraint", nullptr, 0);
    register_suited_parents("WeldConstraint", {"Workspace", "PVInstance"});
    register_suited_parents("RigidConstraint", {"Workspace", "PVInstance"});
    register_suited_parents("NoCollisionConstraint", {"Workspace", "PVInstance"});
}

}  // namespace

}  // namespace engine_core
