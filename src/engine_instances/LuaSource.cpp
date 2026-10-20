#include "LuaSource.hpp"

#include "LuaApi.hpp"
#include "ScriptAnalysis.hpp"

namespace engine_core {

void LuaSource::on_source_changed() {
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
