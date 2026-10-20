#include "RemoteEvent.hpp"

#include "Contract.hpp"
#include "LuaApi.hpp"

#include <iterator>

namespace engine_core {
namespace {

LuaSlot bool_slot(bool value) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Bool;
    slot.flag = value;
    return slot;
}

}  // namespace

const char* RemoteEvent::class_name() const { return "RemoteEvent"; }

void RemoteEvent::set_unreliable(bool unreliable) {
    if (!on_gameplay_thread()) {
        contract_fail("RemoteEvent setters run on SimulationThread");
    }
    if (unreliable == unreliable_) {
        return;
    }
    unreliable_ = unreliable;
    note_property_change("Unreliable", bool_slot(!unreliable), bool_slot(unreliable));
}

namespace {

bool read_unreliable(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* remote = dynamic_cast<const RemoteEvent*>(&object);
    if (remote == nullptr) {
        return false;
    }
    out = bool_slot(remote->unreliable());
    return true;
}

bool write_unreliable(DataModel&, DataModel& object, LuaSlot& in) {
    auto* remote = dynamic_cast<RemoteEvent*>(&object);
    if (remote == nullptr) {
        return false;
    }
    remote->set_unreliable(in.flag);
    return true;
}

// The sending Connection, then whatever was sent: one Values slot. A
// standalone world's connection is nil.
const LuaParam kServerEventParams[] = {{"connection", "Connection"}, {"...", "any"}};
const LuaParam kClientEventParams[] = {{"...", "any"}};

ANARCHY_LUA_REGISTER(register_remote_event_lua) {
    const LuaField fields[] = {
        lua_saved_property("Unreliable", "boolean", read_unreliable, write_unreliable, "false"),
        lua_event("OnServerEvent", kServerEventParams, static_cast<int>(std::size(kServerEventParams))),
        lua_event("OnClientEvent", kClientEventParams, static_cast<int>(std::size(kClientEventParams))),
    };
    register_lua_class("RemoteEvent", "Instance", fields, static_cast<int>(std::size(fields)));
}

}  // namespace

}  // namespace engine_core
