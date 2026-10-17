#include "Connections.hpp"

#include "Containment.hpp"
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

ANARCHY_LUA_REGISTER(register_connections_lua) {
    const LuaField fields[] = {
        lua_property("LocalConnection", "Connection?", false, read_local_connection, nullptr),
    };
    register_lua_class(kConnectionsClass, "Service", fields, static_cast<int>(std::size(fields)));
    register_lua_service(kConnectionsClass);
}

}  // namespace

}  // namespace engine_core
