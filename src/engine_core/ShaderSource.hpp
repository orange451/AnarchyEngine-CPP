#pragma once

#include "LuaApi.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace engine_core {

// What a shader draws: a Material's surface, or the finished frame.
enum class ShaderKind { Surface = 0, Post = 1 };

// Which of a Shader's two texts: the fragment stage (surface() or post()) or
// the vertex stage (vertex(), surface shaders only).
enum class ShaderStage : std::uint8_t { Fragment = 0, Vertex = 1 };

// A uniform's type as the holder's attribute takes it. Color is a vec3 hinted
// `color`; Sampler is a sampler2D, whose attribute is a Texture.
enum class ShaderParamType { Float, Int, Bool, Vec2, Vec3, Color, Sampler };

// One `uniform` a shader declares, with its hints.
struct ShaderParam {
    std::string name;
    ShaderParamType type = ShaderParamType::Float;
    // From the latest `group Name;` above it; empty for none.
    std::string group;
    // range(min, max) was given.
    bool ranged = false;
    double min = 0;
    double max = 0;
    bool hidden = false;
    // A sampler2D's hints: `color` (sampled as sRGB), `normal`, `black` (an
    // unset texture reads black, not white), `clamp` (not repeat), `nearest`.
    bool srgb = false;
    bool normal = false;
    bool black = false;
    bool clamp = false;
    bool nearest = false;
    // What the shader reads while the holder sets nothing: a Number, Bool,
    // Vec2, Vec3, or Color; Nil for a sampler.
    LuaSlot fallback;
    // 1-based.
    int line = 0;
};

struct ShaderError {
    // 1-based.
    int line = 1;
    std::string message;
};

struct ShaderParse {
    // The kind the text was read under: the header's when there was one.
    ShaderKind kind = ShaderKind::Surface;
    ShaderStage stage = ShaderStage::Fragment;
    // A `shader surface;` or `shader post;` line was present, and said this.
    std::optional<ShaderKind> header_kind;
    // A `render_mode` line was present, with these words (none for `render_mode;`).
    std::optional<std::vector<std::string>> header_modes;
    // Where the header and render_mode statements were in the source, as
    // [begin, end) byte ranges, so a holder can take them out of its text.
    std::vector<std::pair<std::size_t, std::size_t>> header_spans;
    // In declaration order. Only those before the error when there is one.
    std::vector<ShaderParam> params;
    // The first problem, or none.
    std::optional<ShaderError> error;
    // The source as GLSL: the `shader`, `render_mode`, and `group` statements
    // and each uniform's hints and default turned to spaces, newlines kept, so
    // its line numbers are the source's.
    std::string glsl;
    // The code, comments aside, names TIME; and names discard or ALPHA, so it
    // can cut holes.
    bool reads_time = false;
    bool cuts = false;
};

// Reads source's header, render_mode, groups, and uniforms. Comments are
// skipped; preprocessor lines pass through and are not evaluated, so a
// uniform inside `#if 0` still counts. With kind given, the header line is
// optional (it sets header_kind when present); without, the text must start
// with one, as phase 1 read it. A vertex stage needs void vertex() and takes
// no `shader` line or render_mode.
ShaderParse parse_shader(std::string_view source, ShaderStage stage = ShaderStage::Fragment,
                         std::optional<ShaderKind> kind = std::nullopt);

// The two stages' uniforms as one list: the fragment's, then the vertex
// stage's the fragment lacks. A uniform in both must match in type, hints,
// and default; one that does not is an error in each stage at its line.
struct MergedParams {
    std::vector<ShaderParam> params;
    std::optional<ShaderError> fragment_error;
    std::optional<ShaderError> vertex_error;
};
MergedParams merge_shader_params(const ShaderParse& fragment, const ShaderParse* vertex);

// "<line>: <message>".
std::string shader_error_text(const ShaderError& error);

// The engine's own names in a shader, which a uniform cannot take: the
// built-ins (ALBEDO, TIME, ...), anything starting gl_, and u followed by an
// uppercase letter, the engine's uniforms.
bool shader_reserved_name(std::string_view name);

}  // namespace engine_core
