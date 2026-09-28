#include "Script.hpp"

#include "Contract.hpp"
#include "LuaApi.hpp"

#include <cstdint>

namespace engine_core {

void Script::save_properties(PropertyBag& out) const {
    LuaSource::save_properties(out);
    if (!enabled_) {
        bag_set(out, "Enabled", JsonValue::boolean(false));
    }
}

void Script::default_properties(PropertyBag& out) const {
    LuaSource::default_properties(out);
    bag_set(out, "Enabled", JsonValue::boolean(true));
}

bool Script::load_property(const std::string& key, const JsonValue& value, std::string& error) {
    if (key == "Enabled") {
        if (!value.is_bool()) {
            error = "Enabled must be true or false";
            return true;
        }
        set_enabled(value.as_bool());
        return true;
    }
    return LuaSource::load_property(key, value, error);
}

void Script::set_enabled(bool enabled) {
    if (!on_gameplay_thread()) {
        contract_fail("set_enabled runs on SimulationThread");
    }
    if (enabled_ == enabled) {
        return;
    }
    const bool previous = enabled_;
    enabled_ = enabled;
    record_bool(id(), Field::Enabled, previous, enabled_);
    emit_own(Field::Enabled);
    if (ScriptHost* host = script_host()) {
        host->on_script_enabled(*this, enabled);
    }
}

std::uint32_t Script::bump_start_generation() {
    if (start_generation_ == 0xffffffffu) {
        start_generation_ = 1;
    } else {
        ++start_generation_;
    }
    return start_generation_;
}

void Script::on_release() {
    if (ScriptHost* host = script_host()) {
        host->on_script_destroyed(*this);
    }
    LuaSource::on_release();
    enabled_ = true;
    start_generation_ = 0;
}

void Script::on_reuse() {
    LuaSource::on_reuse();
    enabled_ = true;
    start_generation_ = 0;
}

void Script::on_parent_changed(InstanceId previous, InstanceId next) {
    if (ScriptHost* host = script_host()) {
        host->on_script_parent(*this, previous, next);
    }
}

void Script::write_place(std::vector<std::byte>& out) const {
    out.push_back(std::byte{enabled_ ? std::uint8_t{1} : std::uint8_t{0}});
    LuaSource::write_place(out);
}

void Script::read_place(const std::byte* data, std::size_t size) {
    enabled_ = data == nullptr || size < 1 || static_cast<unsigned char>(data[0]) != 0;
    start_generation_ = 0;
    if (data == nullptr || size < 1) {
        LuaSource::read_place(nullptr, 0);
        return;
    }
    LuaSource::read_place(data + 1, size - 1);
}

namespace {

bool read_lua_enabled(DataModel&, DataModel& object, LuaSlot& out) {
    auto* script = dynamic_cast<Script*>(&object);
    if (script == nullptr) {
        return false;
    }
    out.kind = LuaSlot::Kind::Bool;
    out.flag = script->enabled();
    return true;
}

bool write_lua_enabled(DataModel&, DataModel& object, LuaSlot& in) {
    auto* script = dynamic_cast<Script*>(&object);
    if (script == nullptr) {
        return false;
    }
    script->set_enabled(in.flag);
    return true;
}

ANARCHY_LUA_REGISTER(register_script_lua) {
    const LuaField fields[] = {
        lua_property("Enabled", "boolean", true, read_lua_enabled, write_lua_enabled),
    };
    register_lua_class("Script", "LuaSource", fields, 1);
}

}  // namespace

}  // namespace engine_core
