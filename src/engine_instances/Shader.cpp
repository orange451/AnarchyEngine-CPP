#include "Shader.hpp"

#include "Enum.hpp"
#include "LuaApi.hpp"

#include <cmath>
#include <cstring>
#include <iterator>

namespace engine_core {

namespace {

LuaSlot bool_slot(bool flag) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Bool;
    slot.flag = flag;
    return slot;
}

LuaSlot number_slot(double number) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Number;
    slot.number = number;
    return slot;
}

LuaSlot kind_slot(ShaderKind kind) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Enum;
    slot.enum_type = &shader_kind_enum();
    slot.number = static_cast<int>(kind);
    return slot;
}

// text with [begin, end) of each span turned to spaces, newlines kept.
std::string without_spans(std::string text, const std::vector<std::pair<std::size_t, std::size_t>>& spans) {
    for (const auto& [begin, end] : spans) {
        for (std::size_t i = begin; i < end && i < text.size(); ++i) {
            if (text[i] != '\n') {
                text[i] = ' ';
            }
        }
    }
    return text;
}

}  // namespace

Shader::Shader(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : SourceText(tag, state, id, 2) {
    source_ = kShaderStarterSource;
    reparse(ShaderStage::Fragment);
    reparse(ShaderStage::Vertex);
    rebuild();
}

const char* Shader::class_name() const { return "Shader"; }

void Shader::reparse(ShaderStage stage) {
    const std::string& text = source(stage == ShaderStage::Vertex ? kVertexStage : kFragmentStage);
    auto parse = std::make_shared<ShaderParse>(parse_shader(text, stage, kind_));
    if (stage == ShaderStage::Vertex) {
        vertex_parse_ = std::move(parse);
    } else {
        fragment_parse_ = std::move(parse);
    }
}

void Shader::rebuild() {
    const std::string was_error = error_;
    const std::string was_vertex = vertex_error_;
    const std::string was_fragment = fragment_error_;
    // The vertex stage counts only in a surface Shader, while it has text.
    const ShaderParse* vertex = kind_ == ShaderKind::Surface && has_vertex_stage() ? vertex_parse_.get() : nullptr;
    const MergedParams merged = merge_shader_params(*fragment_parse_, vertex);
    auto whole = std::make_shared<ShaderParse>(*fragment_parse_);
    whole->params = merged.params;
    if (!whole->error && merged.fragment_error) {
        whole->error = merged.fragment_error;
    }
    if (!whole->error && vertex != nullptr && vertex->error) {
        whole->error = vertex->error;
    }
    if (!whole->error && vertex != nullptr && merged.vertex_error) {
        whole->error = merged.vertex_error;
    }
    merged_ = std::move(whole);
    const auto text_of = [](const std::optional<ShaderError>& parse_error, const std::optional<ShaderError>& merge_error,
                            const std::string& compile_error) {
        if (parse_error) {
            return shader_error_text(*parse_error);
        }
        if (merge_error) {
            return shader_error_text(*merge_error);
        }
        return compile_error;
    };
    fragment_error_ = text_of(fragment_parse_->error, merged.fragment_error, fragment_compile_error_);
    vertex_error_ = vertex != nullptr ? text_of(vertex->error, merged.vertex_error, vertex_compile_error_) : std::string();
    error_ = !vertex_error_.empty()     ? "vertex " + vertex_error_
             : !fragment_error_.empty() ? "fragment " + fragment_error_
                                        : std::string();
    if (error_ != was_error) {
        emit_property("Error");
    }
    if (vertex_error_ != was_vertex) {
        emit_property("VertexError");
    }
    if (fragment_error_ != was_fragment) {
        emit_property("FragmentError");
    }
}

template <class T>
std::optional<std::string> Shader::set_mode(const char* property, T& field, T value, bool rebuilds) {
    if (!on_gameplay_thread()) {
        contract_fail("a Shader's properties are set on SimulationThread");
    }
    if (field == value) {
        return std::nullopt;
    }
    const T previous = field;
    field = value;
    if constexpr (std::is_same_v<T, bool>) {
        note_property_change(property, bool_slot(previous), bool_slot(value));
    } else if constexpr (std::is_same_v<T, double>) {
        note_property_change(property, number_slot(previous), number_slot(value));
    } else {
        note_property_change(property, kind_slot(previous), kind_slot(value));
    }
    if (rebuilds) {
        // Programs build from the kind and the modes too. The driver's errors
        // were for the old program, so they go with its version.
        ++source_version_;
        if (!fragment_compile_error_.empty() || !vertex_compile_error_.empty()) {
            fragment_compile_error_.clear();
            vertex_compile_error_.clear();
            rebuild();
        }
    }
    return std::nullopt;
}

std::optional<std::string> Shader::set_kind(ShaderKind kind) {
    if (kind == kind_) {
        return std::nullopt;
    }
    set_mode("Kind", kind_, kind, true);
    reparse(ShaderStage::Fragment);
    reparse(ShaderStage::Vertex);
    rebuild();
    return std::nullopt;
}

std::optional<std::string> Shader::set_transparent(bool on) { return set_mode("Transparent", transparent_, on, true); }

// The stage, not the text: the programs stay as built.
std::optional<std::string> Shader::set_hdr(bool on) { return set_mode("Hdr", hdr_, on, false); }

std::optional<std::string> Shader::set_two_sided(bool on) { return set_mode("TwoSided", two_sided_, on, true); }

std::optional<std::string> Shader::set_casts_shadow(bool on) { return set_mode("CastsShadow", casts_shadow_, on, true); }

std::optional<std::string> Shader::set_displacement_margin(double units) {
    if (!std::isfinite(units) || units < 0.0) {
        return std::string("DisplacementMargin must not be negative");
    }
    return set_mode("DisplacementMargin", displacement_margin_, units, false);
}

void Shader::set_compile_error(std::uint64_t version, ShaderStage stage, std::string error) {
    if (version != source_version()) {
        return;
    }
    std::string& held = stage == ShaderStage::Vertex ? vertex_compile_error_ : fragment_compile_error_;
    if (held == error) {
        return;
    }
    held = std::move(error);
    rebuild();
}

std::string Shader::prepare_source(int stage, std::string text) {
    // Phase 1's `shader X;` and `render_mode` lines: read, applied, and taken
    // out. Only the fragment stage's: in the vertex stage they are its error.
    if (stage != kFragmentStage) {
        return text;
    }
    const ShaderParse peek = parse_shader(text, ShaderStage::Fragment, kind_);
    if (peek.header_spans.empty()) {
        return text;
    }
    bool kind_changed = false;
    if (peek.header_kind && *peek.header_kind != kind_) {
        // Kind alone: the text about to be stored is parsed under it next.
        set_mode("Kind", kind_, *peek.header_kind, true);
        kind_changed = true;
    }
    if (peek.header_modes && kind_ == ShaderKind::Post) {
        bool hdr = false;
        for (const std::string& mode : *peek.header_modes) {
            hdr = hdr || mode == "hdr";
        }
        set_hdr(hdr);
    } else if (peek.header_modes) {
        bool transparent = false;
        bool two_sided = false;
        bool casts_shadow = true;
        for (const std::string& mode : *peek.header_modes) {
            transparent = transparent || mode == "transparent";
            two_sided = two_sided || mode == "two_sided";
            casts_shadow = casts_shadow && mode != "no_shadow";
        }
        set_transparent(transparent);
        set_two_sided(two_sided);
        set_casts_shadow(casts_shadow);
    }
    std::string stripped = without_spans(std::move(text), peek.header_spans);
    if (kind_changed && stripped == source(stage)) {
        // The text will not change, so nothing else parses it again.
        reparse(ShaderStage::Fragment);
        reparse(ShaderStage::Vertex);
        rebuild();
    }
    return stripped;
}

void Shader::on_source_changed(int stage) {
    if (stage == kVertexStage) {
        vertex_compile_error_.clear();
        reparse(ShaderStage::Vertex);
    } else {
        fragment_compile_error_.clear();
        reparse(ShaderStage::Fragment);
        // A Kind a header line just set reads the vertex stage anew too.
        reparse(ShaderStage::Vertex);
    }
    rebuild();
}

void Shader::write_place(std::vector<std::byte>& out) const {
    const std::uint8_t kind = static_cast<std::uint8_t>(kind_);
    const std::uint8_t flags = static_cast<std::uint8_t>((transparent_ ? 1 : 0) | (two_sided_ ? 2 : 0) |
                                                         (casts_shadow_ ? 4 : 0) | (hdr_ ? 8 : 0));
    out.push_back(static_cast<std::byte>(kind));
    out.push_back(static_cast<std::byte>(flags));
    const auto* margin = reinterpret_cast<const std::byte*>(&displacement_margin_);
    out.insert(out.end(), margin, margin + sizeof(displacement_margin_));
    SourceText::write_place(out);
}

void Shader::read_place(const std::byte* data, std::size_t size) {
    constexpr std::size_t kHead = 2 + sizeof(double);
    if (data != nullptr && size >= kHead) {
        // As captured, with no record: a Stop puts back, it does not edit.
        kind_ = static_cast<std::uint8_t>(data[0]) == 1 ? ShaderKind::Post : ShaderKind::Surface;
        const auto flags = static_cast<std::uint8_t>(data[1]);
        transparent_ = (flags & 1) != 0;
        two_sided_ = (flags & 2) != 0;
        casts_shadow_ = (flags & 4) != 0;
        hdr_ = (flags & 8) != 0;
        std::memcpy(&displacement_margin_, data + 2, sizeof(displacement_margin_));
        SourceText::read_place(data + kHead, size - kHead);
    } else {
        SourceText::read_place(nullptr, 0);
    }
    fragment_compile_error_.clear();
    vertex_compile_error_.clear();
    ++source_version_;
    reparse(ShaderStage::Fragment);
    reparse(ShaderStage::Vertex);
    rebuild();
}

void Shader::on_reuse() {
    source_ = kShaderStarterSource;
    for (std::string& text : more_stages_) {
        text.clear();
    }
    kind_ = ShaderKind::Surface;
    transparent_ = false;
    two_sided_ = false;
    casts_shadow_ = true;
    hdr_ = false;
    displacement_margin_ = 1.0;
    fragment_compile_error_.clear();
    vertex_compile_error_.clear();
    reparse(ShaderStage::Fragment);
    reparse(ShaderStage::Vertex);
    rebuild();
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

bool read_vertex_source(DataModel&, DataModel& object, LuaSlot& out) {
    const Shader* shader = shader_of(object);
    if (shader == nullptr) {
        return false;
    }
    out.kind = LuaSlot::Kind::String;
    out.text = shader->vertex_source();
    return true;
}

bool write_vertex_source(DataModel&, DataModel& object, LuaSlot& in) {
    Shader* shader = shader_of(object);
    if (shader == nullptr) {
        return false;
    }
    shader->set_vertex_source(in.text);
    return true;
}

bool read_kind(DataModel&, DataModel& object, LuaSlot& out) {
    const Shader* shader = shader_of(object);
    if (shader == nullptr) {
        return false;
    }
    out = kind_slot(shader->kind());
    return true;
}

bool write_kind(DataModel&, DataModel& object, LuaSlot& in) {
    Shader* shader = shader_of(object);
    if (shader == nullptr) {
        return false;
    }
    if (in.kind != LuaSlot::Kind::Enum || in.enum_type != &shader_kind_enum()) {
        in.error = "Kind must be an Enum.ShaderKind";
        return false;
    }
    shader->set_kind(static_cast<ShaderKind>(static_cast<int>(in.number)));
    return true;
}

template <bool (Shader::*Get)() const, std::optional<std::string> (Shader::*Set)(bool)>
bool read_flag(DataModel&, DataModel& object, LuaSlot& out) {
    const Shader* shader = shader_of(object);
    if (shader == nullptr) {
        return false;
    }
    out = bool_slot((shader->*Get)());
    return true;
}

template <bool (Shader::*Get)() const, std::optional<std::string> (Shader::*Set)(bool)>
bool write_flag(DataModel&, DataModel& object, LuaSlot& in) {
    Shader* shader = shader_of(object);
    if (shader == nullptr) {
        return false;
    }
    (shader->*Set)(in.flag);
    return true;
}

bool read_margin(DataModel&, DataModel& object, LuaSlot& out) {
    const Shader* shader = shader_of(object);
    if (shader == nullptr) {
        return false;
    }
    out = number_slot(shader->displacement_margin());
    return true;
}

bool write_margin(DataModel&, DataModel& object, LuaSlot& in) {
    Shader* shader = shader_of(object);
    if (shader == nullptr) {
        return false;
    }
    if (const auto refused = shader->set_displacement_margin(in.number)) {
        in.error = *refused;
        return false;
    }
    return true;
}

template <const std::string& (Shader::*Get)() const>
bool read_text(DataModel&, DataModel& object, LuaSlot& out) {
    const Shader* shader = shader_of(object);
    if (shader == nullptr) {
        return false;
    }
    out.kind = LuaSlot::Kind::String;
    out.text = (shader->*Get)();
    return true;
}

ANARCHY_LUA_REGISTER(register_shader_lua) {
    const LuaField fields[] = {
        lua_property("Source", "string", true, read_source, write_source),
        lua_property("FragmentSource", "string", true, read_source, write_source),
        lua_property("VertexSource", "string", true, read_vertex_source, write_vertex_source),
        lua_saved_enum("Kind", shader_kind_enum(), read_kind, write_kind, "\"Surface\""),
        lua_saved_property("Transparent", "boolean", read_flag<&Shader::transparent, &Shader::set_transparent>,
                           write_flag<&Shader::transparent, &Shader::set_transparent>, "false"),
        lua_saved_property("TwoSided", "boolean", read_flag<&Shader::two_sided, &Shader::set_two_sided>,
                           write_flag<&Shader::two_sided, &Shader::set_two_sided>, "false"),
        lua_saved_property("CastsShadow", "boolean", read_flag<&Shader::casts_shadow, &Shader::set_casts_shadow>,
                           write_flag<&Shader::casts_shadow, &Shader::set_casts_shadow>, "true"),
        lua_saved_property("Hdr", "boolean", read_flag<&Shader::hdr, &Shader::set_hdr>,
                           write_flag<&Shader::hdr, &Shader::set_hdr>, "false"),
        lua_saved_property("DisplacementMargin", "number", read_margin, write_margin, "1"),
        lua_property("Error", "string", false, read_text<&Shader::error>, nullptr),
        lua_property("VertexError", "string", false, read_text<&Shader::vertex_error>, nullptr),
        lua_property("FragmentError", "string", false, read_text<&Shader::fragment_error>, nullptr),
    };
    register_lua_class("Shader", "Instance", fields, static_cast<int>(std::size(fields)));
}

}  // namespace

}  // namespace engine_core
