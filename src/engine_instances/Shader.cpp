#include "Shader.hpp"

#include "Enum.hpp"
#include "LuaApi.hpp"

#include <iterator>

namespace engine_core {

Shader::Shader(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : SourceText(tag, state, id) {
    source_ = kShaderStarterSource;
    reparse();
}

const char* Shader::class_name() const { return "Shader"; }

void Shader::reparse() {
    parse_ = std::make_shared<const ShaderParse>(parse_shader(source_));
    error_ = parse_->error ? shader_error_text(*parse_->error) : std::string();
}

void Shader::on_source_changed() {
    const ShaderKind kind = parse_->kind;
    const std::string error = error_;
    reparse();
    if (parse_->kind != kind) {
        emit_property("Kind");
    }
    if (error_ != error) {
        emit_property("Error");
    }
}

void Shader::on_reuse() {
    source_ = kShaderStarterSource;
    reparse();
}

namespace {

Shader* shader_of(DataModel& object) { return dynamic_cast<Shader*>(&object); }

bool read_source(DataModel&, DataModel& object, LuaSlot& out) {
    const Shader* shader = shader_of(object);
    if (shader == nullptr) {
        return false;
    }
    out.kind = LuaSlot::Kind::String;
    out.text = shader->source();
    return true;
}

bool write_source(DataModel&, DataModel& object, LuaSlot& in) {
    Shader* shader = shader_of(object);
    if (shader == nullptr) {
        return false;
    }
    shader->set_source(in.text);
    return true;
}

bool read_kind(DataModel&, DataModel& object, LuaSlot& out) {
    const Shader* shader = shader_of(object);
    if (shader == nullptr) {
        return false;
    }
    out.kind = LuaSlot::Kind::Enum;
    out.enum_type = &shader_kind_enum();
    out.number = static_cast<int>(shader->kind());
    return true;
}

bool read_error(DataModel&, DataModel& object, LuaSlot& out) {
    const Shader* shader = shader_of(object);
    if (shader == nullptr) {
        return false;
    }
    out.kind = LuaSlot::Kind::String;
    out.text = shader->error();
    return true;
}

ANARCHY_LUA_REGISTER(register_shader_lua) {
    LuaField kind = lua_property("Kind", "EnumItem", false, read_kind, nullptr);
    kind.enum_type = &shader_kind_enum();
    const LuaField fields[] = {
        lua_property("Source", "string", true, read_source, write_source),
        kind,
        lua_property("Error", "string", false, read_error, nullptr),
    };
    register_lua_class("Shader", "Instance", fields, static_cast<int>(std::size(fields)));
}

}  // namespace

}  // namespace engine_core
