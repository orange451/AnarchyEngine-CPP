#pragma once

#include "ShaderSource.hpp"
#include "SourceText.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace engine_core {

// What a new Shader holds: a surface shader that tints its Material, with one
// uniform, so the shape is plain without reading the docs.
inline constexpr const char* kShaderStarterSource = R"(shader surface;

// Each uniform is an attribute of the Material that uses this shader.
uniform vec3 Tint : color = vec3(1.0);

void surface() {
    ALBEDO *= Tint;
}
)";

// GLSL a Material or a ShaderEffect draws with, under Assets.Shaders. Its
// Source is a .glsl file beside its .meta.json, opened in the shader editor.
// Each change of Source parses it (parse_shader): Kind says what it draws,
// Error the first problem as "<line>: <message>", and parse() its uniforms.
// Kind and Error are read-only and never saved; they follow Source, Stop and
// undo included.
class Shader : public SourceText {
public:
    Shader(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);
    const char* class_name() const override;
    const char* source_extension() const override { return "glsl"; }

    ShaderKind kind() const { return parse_->kind; }
    // "<line>: <message>": the parse's error, else the driver's (set_compile_error), else "".
    const std::string& error() const { return error_.empty() ? compile_error_ : error_; }
    // The driver's error building source_version version, as the Renderer
    // found it: kept while that version is current, and cleared by the next
    // Source. Not undoable and not saved. Simulation thread.
    void set_compile_error(std::uint64_t version, std::string error);
    // The latest parse of source(), shared and immutable; never null.
    std::shared_ptr<const ShaderParse> parse() const { return parse_; }

protected:
    void on_source_changed() override;
    void on_reuse() override;

private:
    // Parses source_, keeping the result and its error text.
    void reparse();

    std::shared_ptr<const ShaderParse> parse_;
    std::string error_;
    // The driver's, for source_version (set_compile_error).
    std::string compile_error_;
};

}  // namespace engine_core
