#pragma once

#include "LuaApi.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine_core {

// What a shader draws: a Material's surface, or the finished frame.
enum class ShaderKind { Surface = 0, Post = 1 };

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
    ShaderKind kind = ShaderKind::Surface;
    // The render_mode words, in order.
    std::vector<std::string> modes;
    // In declaration order. Only those before the error when there is one.
    std::vector<ShaderParam> params;
    // The first problem, or none.
    std::optional<ShaderError> error;
    // The source as GLSL: the `shader`, `render_mode`, and `group` statements
    // and each uniform's hints and default turned to spaces, newlines kept, so
    // its line numbers are the source's.
    std::string glsl;
};

// Reads source's header, render_mode, groups, and uniforms. Comments are
// skipped; preprocessor lines pass through and are not evaluated, so a
// uniform inside `#if 0` still counts.
ShaderParse parse_shader(std::string_view source);

// "<line>: <message>".
std::string shader_error_text(const ShaderError& error);

// The engine's own names in a shader, which a uniform cannot take: the
// built-ins (ALBEDO, TIME, ...), anything starting gl_, and u followed by an
// uppercase letter, the engine's uniforms.
bool shader_reserved_name(std::string_view name);

}  // namespace engine_core
