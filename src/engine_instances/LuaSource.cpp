#include "LuaSource.hpp"

#include "Contract.hpp"
#include "LuaApi.hpp"
#include "ScriptAnalysis.hpp"

#include <cstdint>
#include <cstring>

namespace engine_core {

void LuaSource::context_actions(std::vector<ContextAction>& out) const {
    out.push_back(ContextAction{InstanceAction::Edit, true});
    DataModel::context_actions(out);
}

bool LuaSource::load_property(const std::string& key, const JsonValue& value, std::string& error) {
    if (key == "Source") {
        error = "Source belongs in the .luau file, not in json";
        return true;
    }
    return DataModel::load_property(key, value, error);
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

void LuaSource::on_release() {
    if (ScriptAnalysis* analysis = script_analysis()) {
        analysis->remove(id());
    }
    source_.clear();
}

void LuaSource::on_reuse() { source_.clear(); }

void LuaSource::write_place(std::vector<std::byte>& out) const {
    const std::uint32_t length = static_cast<std::uint32_t>(source_.size());
    const auto* bytes = reinterpret_cast<const std::byte*>(&length);
    out.insert(out.end(), bytes, bytes + sizeof(length));
    const auto* text = reinterpret_cast<const std::byte*>(source_.data());
    out.insert(out.end(), text, text + source_.size());
}

void LuaSource::read_place(const std::byte* data, std::size_t size) {
    const std::string previous = source_;
    source_.clear();
    if (data != nullptr && size >= sizeof(std::uint32_t)) {
        std::uint32_t length = 0;
        std::memcpy(&length, data, sizeof(length));
        if (sizeof(std::uint32_t) + static_cast<std::size_t>(length) <= size) {
            source_.assign(reinterpret_cast<const char*>(data + sizeof(std::uint32_t)), length);
        }
    }
    // Stop restores authored source through here, not through set_source.
    if (source_ != previous) {
        if (ScriptAnalysis* analysis = script_analysis()) {
            analysis->invalidate(id());
        }
    }
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

ANARCHY_LUA_REGISTER(register_lua_source_lua) {
    const LuaField fields[] = {
        lua_property("Source", "string", true, read_lua_source, write_lua_source),
    };
    register_lua_class("LuaSource", "Instance", fields, 1);
}

}  // namespace

}  // namespace engine_core
