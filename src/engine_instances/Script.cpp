#include "Script.hpp"

#include "Contract.hpp"
#include "Enum.hpp"
#include "Containment.hpp"
#include "LuaApi.hpp"

#include <cstdint>
#include <cstring>
#include <iterator>

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

bool Script::read_property_json(std::string_view name, JsonValue& out) const {
    if (name == "Enabled") {
        out = JsonValue::boolean(enabled_);
        return true;
    }
    return LuaSource::read_property_json(name, out);
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

namespace {

LuaSlot run_context_slot(RunContext context) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Enum;
    slot.enum_type = &run_context_enum();
    slot.number = static_cast<int>(context);
    return slot;
}

}  // namespace

bool context_runs(RunContext context, WorldRole role) {
    switch (role) {
        case WorldRole::Standalone: return true;
        case WorldRole::Server: return context != RunContext::Client;
        case WorldRole::Client: return context != RunContext::Server;
    }
    return true;
}

std::optional<std::string> Script::set_run_context(int value) {
    if (!on_gameplay_thread()) {
        contract_fail("set_run_context runs on SimulationThread");
    }
    if (enum_item_name(run_context_enum(), value) == nullptr) {
        return std::string("RunContext must be an Enum.RunContext");
    }
    const RunContext next = static_cast<RunContext>(value);
    if (next == run_context_) {
        return std::nullopt;
    }
    const RunContext previous = run_context_;
    run_context_ = next;
    note_property_change("RunContext", run_context_slot(previous), run_context_slot(next));
    if (ScriptHost* host = script_host()) {
        host->on_script_context_changed(*this);
    }
    return std::nullopt;
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
    run_context_ = RunContext::Server;
    start_generation_ = 0;
}

void Script::on_reuse() {
    LuaSource::on_reuse();
    enabled_ = true;
    run_context_ = RunContext::Server;
    start_generation_ = 0;
}

void Script::write_place(std::vector<std::byte>& out) const {
    // The base's saved registry properties, such as RunContext, length-prefixed.
    std::vector<std::byte> base;
    DataModel::write_place(base);
    const std::uint32_t length = static_cast<std::uint32_t>(base.size());
    const auto* l = reinterpret_cast<const std::byte*>(&length);
    out.insert(out.end(), l, l + sizeof(length));
    out.insert(out.end(), base.begin(), base.end());
    out.push_back(std::byte{enabled_ ? std::uint8_t{1} : std::uint8_t{0}});
    LuaSource::write_place(out);
}

void Script::read_place(const std::byte* data, std::size_t size) {
    start_generation_ = 0;
    std::uint32_t length = 0;
    if (data == nullptr || size < sizeof(length)) {
        DataModel::read_place(nullptr, 0);
        enabled_ = true;
        LuaSource::read_place(nullptr, 0);
        return;
    }
    std::memcpy(&length, data, sizeof(length));
    const std::size_t base_end = sizeof(length) + length;
    if (base_end > size) {
        DataModel::read_place(nullptr, 0);
        enabled_ = true;
        LuaSource::read_place(nullptr, 0);
        return;
    }
    DataModel::read_place(data + sizeof(length), length);
    if (base_end >= size) {
        enabled_ = true;
        LuaSource::read_place(nullptr, 0);
        return;
    }
    enabled_ = static_cast<unsigned char>(data[base_end]) != 0;
    LuaSource::read_place(data + base_end + 1, size - base_end - 1);
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

bool read_run_context(DataModel&, DataModel& object, LuaSlot& out) {
    auto* script = dynamic_cast<Script*>(&object);
    if (script == nullptr) {
        return false;
    }
    out = run_context_slot(script->run_context());
    return true;
}

bool write_run_context(DataModel&, DataModel& object, LuaSlot& in) {
    auto* script = dynamic_cast<Script*>(&object);
    if (script == nullptr) {
        return false;
    }
    if (in.kind != LuaSlot::Kind::Enum || in.enum_type != &run_context_enum()) {
        in.error = "RunContext must be an Enum.RunContext";
        return false;
    }
    if (const std::optional<std::string> error = script->set_run_context(static_cast<int>(in.number))) {
        in.error = *error;
        return false;
    }
    return true;
}

ANARCHY_LUA_REGISTER(register_script_lua) {
    const LuaField fields[] = {
        lua_property("Enabled", "boolean", true, read_lua_enabled, write_lua_enabled),
        lua_saved_enum("RunContext", run_context_enum(), read_run_context, write_run_context, "\"Server\""),
    };
    register_lua_class("Script", "LuaSource", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("Script", {"Scripts", "Workspace", "Gui", "PVInstance", "GuiBase"});
}

}  // namespace

}  // namespace engine_core
