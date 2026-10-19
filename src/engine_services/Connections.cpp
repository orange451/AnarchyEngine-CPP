#include "Connections.hpp"

#include "Containment.hpp"
#include "Enum.hpp"
#include "LuaApi.hpp"

namespace engine_core {

const char* Connections::class_name() const { return kConnectionsClass; }

bool Connections::hidden_in_explorer() const { return first_child(id()) == 0; }

InstanceId Connections::local_connection() const {
    // A destroyed Connection reads nil: its id no longer names a live instance.
    return instance(local_connection_) != nullptr ? local_connection_ : 0;
}

void Connections::set_local_connection(InstanceId id) {
    local_connection_ = id;
    emit_property("LocalConnection");
}

JoinState Connections::join_state() const { return join_state_; }

void Connections::set_join_state(JoinState state) {
    if (state == join_state_) {
        return;
    }
    join_state_ = state;
    emit_property("JoinState");
}

void Connections::note_joined() {
    set_join_state(JoinState::Joined);
    on_joined.emit();
}

void Connections::note_left(const std::string& reason) {
    set_join_state(JoinState::Idle);
    on_left.emit(reason);
}

namespace {

bool read_local_connection(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* service = dynamic_cast<const Connections*>(&object);
    if (service == nullptr) {
        return false;
    }
    out.id = service->local_connection();
    out.kind = out.id != 0 ? LuaSlot::Kind::Instance : LuaSlot::Kind::Nil;
    return true;
}

bool read_join_state(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* service = dynamic_cast<const Connections*>(&object);
    if (service == nullptr) {
        return false;
    }
    out.kind = LuaSlot::Kind::Enum;
    out.enum_type = &join_state_enum();
    out.number = static_cast<int>(service->join_state());
    return true;
}

ANARCHY_LUA_REGISTER(register_connections_lua) {
    static const LuaParam kLeftArgs[] = {{"reason", "string"}};
    LuaField join_state = lua_property("JoinState", "EnumItem", false, read_join_state, nullptr);
    join_state.enum_type = &join_state_enum();
    LuaField left = lua_host_signal("Left", HostSignal::Left);
    left.params = kLeftArgs;
    left.param_count = 1;
    // ScriptBindings.cpp adds Join and Leave.
    const LuaField fields[] = {
        lua_property("LocalConnection", "Connection?", false, read_local_connection, nullptr),
        join_state,
        lua_host_signal("Joined", HostSignal::Joined),
        left,
    };
    register_lua_class(kConnectionsClass, "Service", fields, static_cast<int>(std::size(fields)));
    register_lua_service(kConnectionsClass);
}

}  // namespace

}  // namespace engine_core
