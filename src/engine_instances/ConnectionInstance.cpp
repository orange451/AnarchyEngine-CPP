#include "ConnectionInstance.hpp"

#include "LuaApi.hpp"

#include <cstdint>
#include <cstdio>
#include <iterator>
#include <random>

namespace engine_core {

const char* ConnectionInstance::class_name() const { return "Connection"; }

void ConnectionInstance::set_connection_id(std::string uuid) {
    connection_id_ = std::move(uuid);
    emit_property("Id");
}

void ConnectionInstance::on_release() {
    connection_id_.clear();
    DataModel::on_release();
}

void ConnectionInstance::on_reuse() {
    connection_id_.clear();
    DataModel::on_reuse();
}

std::string make_connection_uuid() {
    // One generator per thread, seeded from the device; ids never repeat in practice.
    thread_local std::mt19937_64 engine{std::random_device{}() ^ (static_cast<std::uint64_t>(std::random_device{}()) << 32)};
    const std::uint64_t high = engine();
    const std::uint64_t low = engine();
    char text[37];
    std::snprintf(text, sizeof(text), "%08x-%04x-%04x-%04x-%012llx",
                  static_cast<unsigned>(high >> 32), static_cast<unsigned>((high >> 16) & 0xffff),
                  static_cast<unsigned>((high & 0x0fff) | 0x4000), static_cast<unsigned>(((low >> 48) & 0x3fff) | 0x8000),
                  static_cast<unsigned long long>(low & 0xffffffffffffull));
    return std::string(text, 36);
}

namespace {

bool read_id(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* connection = dynamic_cast<const ConnectionInstance*>(&object);
    if (connection == nullptr) {
        return false;
    }
    out.kind = LuaSlot::Kind::String;
    out.text = connection->connection_id();
    return true;
}

DataModel& create_connection(DataModel& world) { return world.create<ConnectionInstance>(); }

ANARCHY_LUA_REGISTER(register_connection_lua) {
    const LuaField fields[] = {
        lua_property("Id", "string", false, read_id, nullptr),
    };
    register_lua_class("Connection", "Instance", fields, static_cast<int>(std::size(fields)));
    // Made by replication (lua_create_instance), never by Instance.new.
    register_lua_creatable("Connection", create_connection, false);
}

}  // namespace

}  // namespace engine_core
