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
// What a new vertex stage holds: a vertex() that leaves the vertex alone.
inline constexpr const char* kShaderVertexStarterSource = R"(// Runs once per vertex, before the fragment stage. VERTEX and NORMAL are
// in world space and may be changed; UV, COLOR, TIME, and the uniforms are
// read-only. For example, waves:
//     VERTEX.y += 0.5 * sin(VERTEX.x + TIME);

void vertex() {
}
)";

class Shader : public SourceText {
public:
    // Its two texts: Source (surface() or post()) and VertexSource (vertex()).
    static constexpr int kFragmentStage = 0;
    static constexpr int kVertexStage = 1;

    Shader(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);
    const char* class_name() const override;
    int stage_count() const override { return 2; }
    const char* source_extension(int stage = 0) const override { return stage == kVertexStage ? "vert.glsl" : "glsl"; }
    const std::string& vertex_source() const { return source(kVertexStage); }
    void set_vertex_source(std::string text) { set_source(kVertexStage, std::move(text)); }
    // The vertex text is not empty.
    bool has_vertex_stage() const { return !vertex_source().empty(); }

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
    void on_source_changed(int stage) override;
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
