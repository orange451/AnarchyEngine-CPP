#include "Script.hpp"

#include "Contract.hpp"
#include "LuaApi.hpp"
#include "ScriptAnalysis.hpp"

#include <cstdint>
#include <cstring>

namespace engine_core {

void LuaSource::context_actions(std::vector<ContextAction>& out) const {
    out.push_back(ContextAction{"Edit", true});
    DataModel::context_actions(out);
}

void LuaSource::set_source(std::string source) {
    if (!on_gameplay_thread()) {
        contract_fail("set_source runs on SimulationThread");
    }
    if (source_ == source) {
        return;
    }
    const std::string previous = source_;
    source_ = std::move(source);
    record_string(id(), Field::Source, previous, source_);
    emit_own(Field::Source);
    if (ScriptAnalysis* analysis = script_analysis()) {
        analysis->invalidate(id());
    }
}

void LuaSource::set_enabled(bool enabled) {
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
    if (auto* script = dynamic_cast<Script*>(this)) {
        if (ScriptHost* host = script_host()) {
            host->on_script_enabled(*script, enabled);
        }
    }
}

std::uint32_t LuaSource::bump_start_generation() {
    if (start_generation_ == 0xffffffffu) {
        start_generation_ = 1;
    } else {
        ++start_generation_;
    }
    return start_generation_;
}

void LuaSource::reset_source_fields() {
    source_.clear();
    enabled_ = true;
    start_generation_ = 0;
}

void LuaSource::on_release() {
    if (ScriptAnalysis* analysis = script_analysis()) {
        analysis->remove(id());
    }
    reset_source_fields();
}

void LuaSource::on_reuse() { reset_source_fields(); }

void LuaSource::write_place(std::vector<std::byte>& out) const {
    out.push_back(std::byte{enabled_ ? std::uint8_t{1} : std::uint8_t{0}});
    const std::uint32_t length = static_cast<std::uint32_t>(source_.size());
    const auto* bytes = reinterpret_cast<const std::byte*>(&length);
    out.insert(out.end(), bytes, bytes + sizeof(length));
    const auto* text = reinterpret_cast<const std::byte*>(source_.data());
    out.insert(out.end(), text, text + source_.size());
}

void LuaSource::read_place(const std::byte* data, std::size_t size) {
    const std::string previous = source_;
    reset_source_fields();
    if (data != nullptr && size >= 1 + sizeof(std::uint32_t)) {
        enabled_ = static_cast<unsigned char>(data[0]) != 0;
        std::uint32_t length = 0;
        std::memcpy(&length, data + 1, sizeof(length));
        if (sizeof(std::uint32_t) + 1 + static_cast<std::size_t>(length) > size) {
            enabled_ = true;
        } else {
            source_.assign(reinterpret_cast<const char*>(data + 1 + sizeof(std::uint32_t)), length);
        }
    }
    // Stop restores authored source through here, not through set_source.
    if (source_ != previous) {
        if (ScriptAnalysis* analysis = script_analysis()) {
            analysis->invalidate(id());
        }
    }
}

void Script::on_release() {
    if (ScriptHost* host = script_host()) {
        host->on_script_destroyed(*this);
    }
    LuaSource::on_release();
}

void Script::on_parent_changed(InstanceId previous, InstanceId next) {
    if (ScriptHost* host = script_host()) {
        host->on_script_parent(*this, previous, next);
    }
}

namespace {
constexpr char kModuleScriptSource[] = "local module = {}\n\nreturn module\n";
}

ModuleScript::ModuleScript(DataModel::ChildTag tag, DataModel::State& state, InstanceId id)
    : LuaSource(tag, state, id) {
    source_ = kModuleScriptSource;
}

void ModuleScript::on_reuse() {
    LuaSource::on_reuse();
    source_ = kModuleScriptSource;
}

namespace {

bool read_lua_source(DataModel&, DataModel& object, LuaSlot& out) {
    auto* source = dynamic_cast<LuaSource*>(&object);
    if (source == nullptr) {
        return false;
    }
    out.kind = LuaSlot::Kind::String;
    out.text = source->source();
    return true;
}

bool write_lua_source(DataModel&, DataModel& object, LuaSlot& in) {
    auto* source = dynamic_cast<LuaSource*>(&object);
    if (source == nullptr) {
        return false;
    }
    source->set_source(in.text);
    return true;
}

bool read_lua_enabled(DataModel&, DataModel& object, LuaSlot& out) {
    auto* source = dynamic_cast<LuaSource*>(&object);
    if (source == nullptr) {
        return false;
    }
    out.kind = LuaSlot::Kind::Bool;
    out.flag = source->enabled();
    return true;
}

bool write_lua_enabled(DataModel&, DataModel& object, LuaSlot& in) {
    auto* source = dynamic_cast<LuaSource*>(&object);
    if (source == nullptr) {
        return false;
    }
    source->set_enabled(in.flag);
    return true;
}

ANARCHY_LUA_REGISTER(register_script_lua) {
    const LuaField fields[] = {
        lua_property("Source", "string", true, read_lua_source, write_lua_source),
        lua_property("Enabled", "boolean", true, read_lua_enabled, write_lua_enabled),
    };
    register_lua_class("Script", "DataModel", fields, 2);
    register_lua_class("ModuleScript", "DataModel", fields, 2);
}

}  // namespace

}  // namespace engine_core
