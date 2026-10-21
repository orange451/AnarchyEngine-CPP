#include "ShaderSource.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace engine_core {
namespace {

constexpr int kMaxUniforms = 64;
constexpr int kMaxSurfaceSamplers = 3;
constexpr int kMaxPostSamplers = 4;

constexpr std::string_view kHeaderMessage = "a shader starts with `shader surface;` or `shader post;`";
constexpr std::string_view kTypeMessage = "a uniform is float, int, bool, vec2, vec3, or sampler2D";
constexpr std::string_view kRangeMessage = "range takes two numbers, the first below the second";

constexpr std::string_view kReserved[] = {
    "ALBEDO",          "ALPHA",          "NORMAL",          "METALNESS",         "ROUGHNESS",         "REFLECTIVITY",
    "EMISSIVE",        "UV",             "COLOR",           "VIEW_POSITION",     "WORLD_POSITION",    "VIEW_NORMAL",
    "WORLD_NORMAL",    "SCREEN_UV",      "SCREEN_SIZE",     "TEXEL",             "FRONT_FACING",      "TIME",
    "CAMERA_POSITION", "VIEW",           "PROJECTION",      "INV_VIEW",          "INV_PROJECTION",    "SCREEN",
    "DEPTH",           "NORMALS",        "MATERIAL",        "TEXTURE_DIFFUSE",   "TEXTURE_NORMAL",    "TEXTURE_ROUGHNESS",
    "TEXTURE_METALNESS", "TEXTURE_EMISSIVE", "VERTEX",
};

bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }
bool word_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool word_char(char c) { return word_start(c) || (c >= '0' && c <= '9'); }

std::string_view trim(std::string_view text) {
    while (!text.empty() && space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// source with comments and preprocessor lines turned to spaces, newlines
// kept, so offsets and lines match the source and no rule reads them.
std::string mask(std::string_view source) {
    std::string out(source);
    bool line_start = true;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const char c = out[i];
        if (c == '/' && i + 1 < out.size() && out[i + 1] == '/') {
            for (; i < out.size() && out[i] != '\n'; ++i) {
                out[i] = ' ';
            }
            line_start = true;
            continue;
        }
        if (c == '/' && i + 1 < out.size() && out[i + 1] == '*') {
            out[i] = ' ';
            out[i + 1] = ' ';
            for (i += 2; i < out.size(); ++i) {
                if (out[i] == '*' && i + 1 < out.size() && out[i + 1] == '/') {
                    out[i] = ' ';
                    out[i + 1] = ' ';
                    ++i;
                    break;
                }
                if (out[i] != '\n') {
                    out[i] = ' ';
                }
            }
            // A comment before a directive leaves the line a directive's.
            continue;
        }
        if (c == '#' && line_start) {
            // To the line's end, and on through each line a trailing \ continues.
            for (;;) {
                char last = ' ';
                for (; i < out.size() && out[i] != '\n'; ++i) {
                    if (!space(out[i])) {
                        last = out[i];
                    }
                    out[i] = ' ';
                }
                if (last != '\\' || i >= out.size()) {
                    break;
                }
                ++i;
            }
            line_start = true;
            continue;
        }
        if (c == '\n') {
            line_start = true;
        } else if (!space(c)) {
            line_start = false;
        }
    }
    return out;
}

// A top-level statement: [begin, end), end being where its `;` or `{` is.
struct Statement {
    std::size_t begin = 0;
    std::size_t end = 0;
    char terminator = '\0';
};

std::vector<Statement> statements(const std::string& masked) {
    std::vector<Statement> out;
    constexpr std::size_t kNone = std::string::npos;
    std::size_t begin = kNone;
    int depth = 0;
    for (std::size_t i = 0; i < masked.size(); ++i) {
        const char c = masked[i];
        if (depth > 0) {
            depth += c == '{' ? 1 : c == '}' ? -1 : 0;
            continue;
        }
        if (c == ';') {
            if (begin != kNone) {
                out.push_back({begin, i, ';'});
            }
            begin = kNone;
        } else if (c == '{') {
            out.push_back({begin != kNone ? begin : i, i, '{'});
            begin = kNone;
            depth = 1;
        } else if (begin == kNone && !space(c) && c != '}') {
            begin = i;
        }
    }
    if (begin != kNone) {
        out.push_back({begin, masked.size(), '\0'});
    }
    return out;
}

// The word starting at pos after any spaces, moving pos past it; empty for none.
std::string_view read_word(std::string_view text, std::size_t& pos) {
    while (pos < text.size() && space(text[pos])) {
        ++pos;
    }
    const std::size_t start = pos;
    if (pos < text.size() && word_start(text[pos])) {
        while (pos < text.size() && word_char(text[pos])) {
            ++pos;
        }
    }
    return text.substr(start, pos - start);
}

// text split on commas outside parentheses, each part trimmed.
std::vector<std::string_view> split_commas(std::string_view text) {
    std::vector<std::string_view> out;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        const char c = i < text.size() ? text[i] : ',';
        depth += c == '(' ? 1 : c == ')' ? -1 : 0;
        if (c == ',' && depth <= 0) {
            out.push_back(trim(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    return out;
}

bool parse_number(std::string_view text, double& out) {
    text = trim(text);
    if (!text.empty() && (text.back() == 'f' || text.back() == 'F')) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return false;
    }
    const std::string copy(text);
    char* end = nullptr;
    out = std::strtod(copy.c_str(), &end);
    return end == copy.c_str() + copy.size() && std::isfinite(out);
}

// "vecN(a, b)" or "vecN(s)" into count floats.
bool parse_vector(std::string_view text, int count, float* out) {
    text = trim(text);
    const std::string head = "vec" + std::to_string(count);
    if (text.substr(0, head.size()) != head) {
        return false;
    }
    text = trim(text.substr(head.size()));
    if (text.size() < 2 || text.front() != '(' || text.back() != ')') {
        return false;
    }
    const std::vector<std::string_view> args = split_commas(text.substr(1, text.size() - 2));
    if (args.size() != 1 && args.size() != static_cast<std::size_t>(count)) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        double value = 0;
        if (!parse_number(args[args.size() == 1 ? 0 : static_cast<std::size_t>(i)], value)) {
            return false;
        }
        out[i] = static_cast<float>(value);
    }
    return true;
}

const char* type_word(ShaderParamType type) {
    switch (type) {
    case ShaderParamType::Float:
        return "float";
    case ShaderParamType::Int:
        return "int";
    case ShaderParamType::Bool:
        return "bool";
    case ShaderParamType::Vec2:
        return "vec2";
    case ShaderParamType::Vec3:
    case ShaderParamType::Color:
        return "vec3";
    case ShaderParamType::Sampler:
        return "sampler2D";
    }
    return "float";
}

class Parser {
public:
    Parser(std::string_view source, ShaderStage stage, std::optional<ShaderKind> kind)
        : source_(source), masked_(mask(source)), header_required_(!kind.has_value()) {
        out_.stage = stage;
        out_.kind = kind.value_or(ShaderKind::Surface);
        out_.glsl.assign(source.data(), source.size());
        for (std::size_t i = 0; i < source.size(); ++i) {
            if (source[i] == '\n') {
                line_starts_.push_back(i + 1);
            }
        }
    }

    ShaderParse run() {
        const std::vector<Statement> all = statements(masked_);
        const bool headed = !all.empty() && header(all.front());
        if (headed && out_.stage == ShaderStage::Vertex) {
            return fail(line_of(all.front().begin), "the shader line belongs in the fragment stage");
        }
        if (header_required_ && !headed) {
            return fail(all.empty() ? 1 : line_of(all.front().begin), std::string(kHeaderMessage));
        }
        for (std::size_t index = headed ? 1 : 0; index < all.size(); ++index) {
            if (!statement(all[index])) {
                return std::move(out_);
            }
        }
        if (out_.stage == ShaderStage::Vertex) {
            if (!has_function("vertex")) {
                return fail(last_line(), "a vertex stage needs void vertex()");
            }
            return std::move(out_);
        }
        const char* function = out_.kind == ShaderKind::Surface ? "surface" : "post";
        if (!has_function(function)) {
            return fail(last_line(), std::string("a ") + (out_.kind == ShaderKind::Surface ? "surface" : "post") +
                                         " shader needs void " + function + "()");
        }
        return std::move(out_);
    }

private:
    int line_of(std::size_t offset) const {
        return static_cast<int>(std::upper_bound(line_starts_.begin(), line_starts_.end(), offset) - line_starts_.begin()) + 1;
    }

    int last_line() const {
        const int newlines = static_cast<int>(line_starts_.size());
        return !source_.empty() && source_.back() == '\n' ? std::max(1, newlines) : newlines + 1;
    }

    ShaderParse fail(int line, std::string message) {
        out_.error = ShaderError{line, std::move(message)};
        return std::move(out_);
    }

    bool fail_at(const Statement& at, std::string message) {
        out_.error = ShaderError{line_of(at.begin), std::move(message)};
        return false;
    }

    // Turns [begin, end) of the GLSL to spaces, keeping newlines.
    void blank(std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end && i < out_.glsl.size(); ++i) {
            if (out_.glsl[i] != '\n') {
                out_.glsl[i] = ' ';
            }
        }
    }

    std::string_view text(const Statement& at) const {
        return std::string_view(masked_).substr(at.begin, at.end - at.begin);
    }

    bool header(const Statement& at) {
        if (at.terminator != ';') {
            return false;
        }
        const std::string_view body = text(at);
        std::size_t pos = 0;
        if (read_word(body, pos) != "shader") {
            return false;
        }
        const std::string_view kind = read_word(body, pos);
        if (!trim(body.substr(pos)).empty() || (kind != "surface" && kind != "post")) {
            return false;
        }
        out_.kind = kind == "surface" ? ShaderKind::Surface : ShaderKind::Post;
        out_.header_kind = out_.kind;
        out_.header_spans.emplace_back(at.begin, at.end + 1);
        blank(at.begin, at.end + 1);
        return true;
    }

    bool statement(const Statement& at) {
        const std::string_view body = text(at);
        std::size_t pos = 0;
        const std::string_view first = read_word(body, pos);
        if (first == "render_mode" && at.terminator == ';') {
            return render_mode(at, body.substr(pos));
        }
        if (first == "group" && at.terminator == ';') {
            const std::string_view name = trim(body.substr(pos));
            group_.assign(name.data(), name.size());
            blank(at.begin, at.end + 1);
            return true;
        }
        if (first == "uniform") {
            return uniform(at, pos);
        }
        return true;
    }

    bool render_mode(const Statement& at, std::string_view rest) {
        if (out_.stage == ShaderStage::Vertex) {
            return fail_at(at, "render_mode belongs in the fragment stage");
        }
        const bool surface = out_.kind == ShaderKind::Surface;
        std::vector<std::string> modes;
        for (const std::string_view mode : split_commas(rest)) {
            if (trim(mode).empty()) {
                continue;
            }
            const bool known = surface ? (mode == "transparent" || mode == "two_sided" || mode == "no_shadow")
                                       : mode == "hdr";
            if (!known) {
                return fail_at(at, "unknown render_mode " + std::string(mode) +
                                       (surface ? "; a surface shader has transparent, two_sided, no_shadow"
                                                : "; a post shader has hdr"));
            }
            modes.emplace_back(mode);
        }
        out_.header_modes = std::move(modes);
        out_.header_spans.emplace_back(at.begin, at.end + 1);
        blank(at.begin, at.end + 1);
        return true;
    }

    bool uniform(const Statement& at, std::size_t pos) {
        if (at.terminator == '{') {
            return fail_at(at, "uniform blocks are not supported");
        }
        const std::string_view body = text(at);
        const std::string_view type = read_word(body, pos);
        ShaderParam param;
        if (type == "float") {
            param.type = ShaderParamType::Float;
        } else if (type == "int") {
            param.type = ShaderParamType::Int;
        } else if (type == "bool") {
            param.type = ShaderParamType::Bool;
        } else if (type == "vec2") {
            param.type = ShaderParamType::Vec2;
        } else if (type == "vec3") {
            param.type = ShaderParamType::Vec3;
        } else if (type == "sampler2D") {
            param.type = ShaderParamType::Sampler;
        } else {
            return fail_at(at, std::string(kTypeMessage));
        }
        const std::string_view name = read_word(body, pos);
        if (name.empty()) {
            return fail_at(at, "a uniform needs a name");
        }
        param.name.assign(name.data(), name.size());
        param.line = line_of(at.begin);
        param.group = group_;
        const std::string_view rest = body.substr(pos);
        if (trim(rest).substr(0, 1) == "[") {
            return fail_at(at, "a uniform cannot be an array");
        }
        if (shader_reserved_name(name)) {
            return fail_at(at, param.name + " is the engine's name");
        }
        for (const ShaderParam& before : out_.params) {
            if (before.name == param.name) {
                return fail_at(at, param.name + " is declared twice");
            }
        }
        // `: hints` up to `=`, then the default.
        const std::size_t colon = rest.find(':');
        const std::size_t equals = rest.find('=');
        std::string_view hints;
        std::string_view value;
        if (colon != std::string_view::npos && (equals == std::string_view::npos || colon < equals)) {
            hints = rest.substr(colon + 1, equals == std::string_view::npos ? std::string_view::npos : equals - colon - 1);
        } else if (colon != std::string_view::npos || (!trim(rest).empty() && equals == std::string_view::npos)) {
            return fail_at(at, "a uniform is `uniform <type> <Name> : hints = default;`");
        }
        if (equals != std::string_view::npos) {
            value = rest.substr(equals + 1);
        }
        if (!apply_hints(at, param, hints) || !apply_default(at, param, value)) {
            return false;
        }
        if (static_cast<int>(out_.params.size()) >= kMaxUniforms) {
            return fail_at(at, "a shader has at most 64 uniforms");
        }
        if (param.type == ShaderParamType::Sampler) {
            const bool surface = out_.kind == ShaderKind::Surface;
            const int most = surface ? kMaxSurfaceSamplers : kMaxPostSamplers;
            if (++samplers_ > most) {
                return fail_at(at, std::string(surface ? "a surface" : "a post") + " shader has at most " +
                                       std::to_string(most) + " sampler2D");
            }
        }
        // The GLSL keeps `uniform <type> <Name>` and the `;`.
        const std::size_t first = std::min(colon, equals);
        if (first != std::string_view::npos) {
            blank(at.begin + pos + first, at.end);
        }
        out_.params.push_back(std::move(param));
        return true;
    }

    bool apply_hints(const Statement& at, ShaderParam& param, std::string_view hints) {
        if (trim(hints).empty()) {
            return true;
        }
        const bool sampler = param.type == ShaderParamType::Sampler;
        for (const std::string_view hint : split_commas(hints)) {
            std::size_t pos = 0;
            const std::string_view word = read_word(hint, pos);
            const std::string_view args = trim(hint.substr(pos));
            if (word == "range") {
                if (param.type != ShaderParamType::Float && param.type != ShaderParamType::Int) {
                    return fail_at(at, "range is for float and int");
                }
                const std::vector<std::string_view> bounds =
                    args.size() >= 2 && args.front() == '(' && args.back() == ')'
                        ? split_commas(args.substr(1, args.size() - 2))
                        : std::vector<std::string_view>{};
                if (bounds.size() != 2 || !parse_number(bounds[0], param.min) || !parse_number(bounds[1], param.max) ||
                    !(param.max > param.min)) {
                    return fail_at(at, std::string(kRangeMessage));
                }
                param.ranged = true;
                continue;
            }
            if (!args.empty()) {
                return fail_at(at, "unknown hint " + std::string(trim(hint)));
            }
            if (word == "hidden") {
                param.hidden = true;
            } else if (word == "color") {
                if (param.type == ShaderParamType::Vec3) {
                    param.type = ShaderParamType::Color;
                } else if (sampler) {
                    param.srgb = true;
                } else if (param.type != ShaderParamType::Color) {
                    return fail_at(at, "color is for vec3 and sampler2D");
                }
            } else if (word == "repeat" || word == "clamp" || word == "white" || word == "black" || word == "normal" ||
                       word == "nearest") {
                if (!sampler) {
                    return fail_at(at, std::string(word) + " is for sampler2D");
                }
                param.clamp = word == "clamp" ? true : word == "repeat" ? false : param.clamp;
                param.black = word == "black" ? true : word == "white" ? false : param.black;
                param.normal = param.normal || word == "normal";
                param.nearest = param.nearest || word == "nearest";
            } else {
                return fail_at(at, "unknown hint " + std::string(word.empty() ? trim(hint) : word));
            }
        }
        return true;
    }

    bool apply_default(const Statement& at, ShaderParam& param, std::string_view value) {
        LuaSlot& slot = param.fallback;
        const bool given = !trim(value).empty();
        const std::string wrong = std::string("the default must be a ") + type_word(param.type);
        switch (param.type) {
        case ShaderParamType::Float:
        case ShaderParamType::Int: {
            double number = 0;
            if (given && !parse_number(value, number)) {
                return fail_at(at, wrong);
            }
            slot.kind = LuaSlot::Kind::Number;
            slot.number = param.type == ShaderParamType::Int ? std::trunc(number) : number;
            return true;
        }
        case ShaderParamType::Bool: {
            const std::string_view word = trim(value);
            if (given && word != "true" && word != "false") {
                return fail_at(at, wrong);
            }
            slot.kind = LuaSlot::Kind::Bool;
            slot.flag = word == "true";
            return true;
        }
        case ShaderParamType::Vec2: {
            float axes[2] = {0.f, 0.f};
            if (given && !parse_vector(value, 2, axes)) {
                return fail_at(at, wrong);
            }
            slot.kind = LuaSlot::Kind::Vec2;
            slot.vec = Vec3{axes[0], axes[1], 0.f};
            return true;
        }
        case ShaderParamType::Vec3:
        case ShaderParamType::Color: {
            const bool color = param.type == ShaderParamType::Color;
            float axes[3] = {color ? 1.f : 0.f, color ? 1.f : 0.f, color ? 1.f : 0.f};
            if (given && !parse_vector(value, 3, axes)) {
                return fail_at(at, wrong);
            }
            if (color) {
                slot.kind = LuaSlot::Kind::Color;
                slot.color = ColorRgb{axes[0], axes[1], axes[2], 1.f};
            } else {
                slot.kind = LuaSlot::Kind::Vec3;
                slot.vec = Vec3{axes[0], axes[1], axes[2]};
            }
            return true;
        }
        case ShaderParamType::Sampler:
            if (given) {
                return fail_at(at, "a sampler2D has no default; use white or black");
            }
            return true;
        }
        return true;
    }

    // `void <name>(` outside comments.
    bool has_function(std::string_view name) const {
        const std::string_view text(masked_);
        for (std::size_t at = text.find("void"); at != std::string_view::npos; at = text.find("void", at + 4)) {
            if ((at > 0 && word_char(text[at - 1])) || (at + 4 < text.size() && word_char(text[at + 4]))) {
                continue;
            }
            std::size_t pos = at + 4;
            if (read_word(text, pos) != name) {
                continue;
            }
            while (pos < text.size() && space(text[pos])) {
                ++pos;
            }
            if (pos < text.size() && text[pos] == '(') {
                return true;
            }
        }
        return false;
    }

    std::string_view source_;
    std::string masked_;
    std::vector<std::size_t> line_starts_;
    bool header_required_ = true;
    ShaderParse out_;
    std::string group_;
    int samplers_ = 0;
};

bool same_param(const ShaderParam& a, const ShaderParam& b) {
    const LuaSlot& x = a.fallback;
    const LuaSlot& y = b.fallback;
    return a.type == b.type && a.group == b.group && a.ranged == b.ranged && a.min == b.min && a.max == b.max &&
           a.hidden == b.hidden && a.srgb == b.srgb && a.normal == b.normal && a.black == b.black &&
           a.clamp == b.clamp && a.nearest == b.nearest && x.kind == y.kind && x.number == y.number &&
           x.flag == y.flag && x.vec.x == y.vec.x && x.vec.y == y.vec.y && x.vec.z == y.vec.z &&
           x.color.r == y.color.r && x.color.g == y.color.g && x.color.b == y.color.b;
}

}  // namespace

namespace {

// Whether text names token as a whole word outside // and /* */ comments.
bool names_token(std::string_view text, std::string_view token) {
    const auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; };
    std::size_t i = 0;
    while (i < text.size()) {
        if (text.compare(i, 2, "//") == 0) {
            const std::size_t end = text.find('\n', i);
            i = end == std::string_view::npos ? text.size() : end;
            continue;
        }
        if (text.compare(i, 2, "/*") == 0) {
            const std::size_t end = text.find("*/", i + 2);
            i = end == std::string_view::npos ? text.size() : end + 2;
            continue;
        }
        if (!word(text[i])) {
            ++i;
            continue;
        }
        std::size_t end = i;
        while (end < text.size() && word(text[end])) {
            ++end;
        }
        if (text.substr(i, end - i) == token) {
            return true;
        }
        i = end;
    }
    return false;
}

}  // namespace

ShaderParse parse_shader(std::string_view source, ShaderStage stage, std::optional<ShaderKind> kind) {
    ShaderParse out = Parser(source, stage, kind).run();
    out.reads_time = names_token(out.glsl, "TIME");
    out.cuts = stage == ShaderStage::Fragment && (names_token(out.glsl, "discard") || names_token(out.glsl, "ALPHA"));
    return out;
}

MergedParams merge_shader_params(const ShaderParse& fragment, const ShaderParse* vertex) {
    MergedParams out;
    out.params = fragment.params;
    if (vertex == nullptr) {
        return out;
    }
    for (const ShaderParam& param : vertex->params) {
        const auto found = std::find_if(out.params.begin(), out.params.end(),
                                        [&](const ShaderParam& have) { return have.name == param.name; });
        if (found == out.params.end()) {
            out.params.push_back(param);
            continue;
        }
        if (!same_param(*found, param)) {
            if (!out.fragment_error) {
                out.fragment_error = ShaderError{found->line, param.name + " is declared differently in the vertex stage"};
            }
            if (!out.vertex_error) {
                out.vertex_error = ShaderError{param.line, param.name + " is declared differently in the fragment stage"};
            }
        }
    }
    return out;
}

std::string shader_error_text(const ShaderError& error) { return std::to_string(error.line) + ": " + error.message; }

bool shader_reserved_name(std::string_view name) {
    if (name.substr(0, 3) == "gl_") {
        return true;
    }
    if (name.size() >= 2 && name[0] == 'u' && name[1] >= 'A' && name[1] <= 'Z') {
        return true;
    }
    return std::find(std::begin(kReserved), std::end(kReserved), name) != std::end(kReserved);
}

}  // namespace engine_core
