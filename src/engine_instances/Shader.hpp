#pragma once

#include "ShaderSource.hpp"
#include "SourceText.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace engine_core {

// What a new Shader holds: a surface shader that tints its Material, with one
// uniform, so the shape is plain without reading the docs.
inline constexpr const char* kShaderStarterSource = R"(// Each uniform is an attribute of the Material that uses this shader.
uniform vec3 Tint : color = vec3(1.0);

void surface() {
    ALBEDO *= Tint;
}
)";

// What a new vertex stage holds: a vertex() that leaves the vertex alone.
inline constexpr const char* kShaderVertexStarterSource = R"(// Runs once per vertex, before the fragment stage. VERTEX and NORMAL are
// in world space and may be changed; UV, COLOR, TIME, and the uniforms are
// read-only. For example, waves:
//     VERTEX.y += 0.5 * sin(VERTEX.x + TIME);

void vertex() {
}
)";

// GLSL a Material or a ShaderEffect draws with, under Assets.Shaders. It has
// two texts: the fragment stage, Source, a .glsl file beside its .meta.json
// (surface() or post()), and the vertex stage, VertexSource, a .vert.glsl
// file written while it has text (vertex(); surface shaders only). Each
// change of a stage parses it (parse_shader) under Kind; the uniforms of both
// are one list (parse()). Kind, Transparent, TwoSided, CastsShadow, Hdr, and
// DisplacementMargin are saved properties; a text written with phase 1's
// `shader X;` or `render_mode` lines sets them and loses the lines. Error is
// the first problem of either stage, "<stage> <line>: <message>".
class Shader : public SourceText {
public:
    // Its two texts: Source (surface() or post()) and VertexSource (vertex()).
    static constexpr int kFragmentStage = 0;
    static constexpr int kVertexStage = 1;

    Shader(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);
    const char* class_name() const override;
    const char* source_extension(int stage = 0) const override { return stage == kVertexStage ? "vert.glsl" : "glsl"; }
    const char* source_alias(int stage) const override { return stage == kFragmentStage ? "FragmentSource" : nullptr; }
    const std::string& vertex_source() const { return source(kVertexStage); }
    void set_vertex_source(std::string text) { set_source(kVertexStage, std::move(text)); }
    // The vertex text is not empty.
    bool has_vertex_stage() const { return !vertex_source().empty(); }

    // The saved properties. Each setter records undo and fires Changed; a
    // Kind or mode change is a new source_version, so programs build again.
    ShaderKind kind() const { return kind_; }
    std::optional<std::string> set_kind(ShaderKind kind);
    bool transparent() const { return transparent_; }
    std::optional<std::string> set_transparent(bool on);
    bool two_sided() const { return two_sided_; }
    std::optional<std::string> set_two_sided(bool on);
    bool casts_shadow() const { return casts_shadow_; }
    std::optional<std::string> set_casts_shadow(bool on);
    // A post Shader's stage: before bloom and the tone map, on linear light,
    // instead of after the tone map. Kept, and ignored, on a surface Shader.
    bool hdr() const { return hdr_; }
    std::optional<std::string> set_hdr(bool on);
    // How far the vertex stage may move a vertex, in units: culling grows a
    // draw's box by it.
    double displacement_margin() const { return displacement_margin_; }
    std::optional<std::string> set_displacement_margin(double units);

    // The first problem of either stage, "<stage> <line>: <message>", or "":
    // a parse error, else a mismatched uniform, else the driver's.
    const std::string& error() const { return error_; }
    const std::string& vertex_error() const { return vertex_error_; }
    const std::string& fragment_error() const { return fragment_error_; }
    // The driver's error building stage at source_version version, as the
    // Renderer found it: kept while that version is current, cleared by the
    // next text of that stage or a Kind or mode change. Not undoable and not saved. Simulation thread.
    void set_compile_error(std::uint64_t version, ShaderStage stage, std::string error);

    // The Shader as a whole: the fragment stage's parse with both stages'
    // uniforms, shared and immutable; never null. Its error is the first of
    // either stage, so a broken stage draws nothing through it.
    std::shared_ptr<const ShaderParse> parse() const { return merged_; }
    std::shared_ptr<const ShaderParse> stage_parse(ShaderStage stage) const {
        return stage == ShaderStage::Vertex ? vertex_parse_ : fragment_parse_;
    }

protected:
    // Phase 1's header lines set Kind and the modes, and leave the text.
    std::string prepare_source(int stage, std::string text) override;
    void on_source_changed(int stage) override;
    void on_reuse() override;
    // Kind, the modes, and the margin, then the two texts: what Stop puts back.
    void write_place(std::vector<std::byte>& out) const override;
    void read_place(const std::byte* data, std::size_t size) override;

private:
    void reparse(ShaderStage stage);
    // The merged parse and the three error texts, from the stage parses and
    // compile errors; emits Error, VertexError, and FragmentError as they move.
    void rebuild();
    template <class T>
    std::optional<std::string> set_mode(const char* property, T& field, T value, bool rebuilds);

    ShaderKind kind_ = ShaderKind::Surface;
    bool transparent_ = false;
    bool two_sided_ = false;
    bool casts_shadow_ = true;
    bool hdr_ = false;
    double displacement_margin_ = 1.0;
    std::shared_ptr<const ShaderParse> fragment_parse_;
    std::shared_ptr<const ShaderParse> vertex_parse_;
    std::shared_ptr<const ShaderParse> merged_;
    std::string fragment_compile_error_;
    std::string vertex_compile_error_;
    std::string error_;
    std::string vertex_error_;
    std::string fragment_error_;
};

}  // namespace engine_core
