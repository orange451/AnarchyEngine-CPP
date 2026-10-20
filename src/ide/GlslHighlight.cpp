#include "GlslHighlight.hpp"

#include "Utf8.hpp"

#include "ShaderSource.hpp"

#include <algorithm>
#include <string>

namespace ide {
namespace {

constexpr const char* kKeyword = "keyword";
constexpr const char* kType = "datatype";
constexpr const char* kBuiltin = "builtin";
constexpr const char* kNumber = "number";
constexpr const char* kComment = "comment";

constexpr std::string_view kKeywords[] = {
    "if",      "else",  "for",     "while", "do",          "return", "break", "continue", "discard",
    "const",   "in",    "out",     "inout", "uniform",     "struct", "true",  "false",    "shader",
    "render_mode", "group", "flat", "smooth", "highp", "mediump", "lowp", "precision",
};

constexpr std::string_view kTypes[] = {
    "void",  "bool",  "int",   "uint",  "float", "vec2",  "vec3",  "vec4",      "ivec2", "ivec3",
    "ivec4", "uvec2", "uvec3", "uvec4", "bvec2", "bvec3", "bvec4", "mat2",      "mat3",  "mat4",
    "sampler2D",
};

constexpr std::string_view kFunctions[] = {
    "texture", "textureLod", "texelFetch", "textureSize", "mix",     "clamp",  "smoothstep", "step",
    "min",     "max",        "abs",        "sign",        "floor",   "ceil",   "fract",      "mod",
    "pow",     "exp",        "exp2",       "log",         "log2",    "sqrt",   "inversesqrt", "length",
    "distance", "normalize", "dot",        "cross",       "reflect", "refract", "sin",       "cos",
    "tan",     "asin",       "acos",       "atan",        "radians", "degrees", "dFdx",      "dFdy",
    "fwidth",  "transpose",  "inverse",    "lessThan",    "greaterThan",
};

bool IsSpace(char32_t c) { return c == U' ' || c == U'\t' || c == U'\n' || c == U'\r' || c == U'\f'; }
bool IsDigit(char32_t c) { return c >= U'0' && c <= U'9'; }
bool IsWordStart(char32_t c) { return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || c == U'_'; }
bool IsWordChar(char32_t c) { return IsWordStart(c) || IsDigit(c); }

template <std::size_t N>
bool Contains(const std::string_view (&words)[N], std::string_view word) {
    return std::find(std::begin(words), std::end(words), word) != std::end(words);
}

class Spans {
public:
    void add(const char* style, std::size_t length) {
        if (length == 0) {
            return;
        }
        if (!out_.empty() && out_.back().style == style) {
            out_.back().length += static_cast<int>(length);
        } else {
            out_.push_back(GlslSpan{style, static_cast<int>(length)});
        }
    }
    std::vector<GlslSpan> take() { return std::move(out_); }

private:
    std::vector<GlslSpan> out_;
};

const char* WordStyle(const std::u32string& word32) {
    std::string word;
    for (const char32_t c : word32) {
        word.push_back(static_cast<char>(c));
    }
    if (Contains(kKeywords, word)) {
        return kKeyword;
    }
    if (Contains(kTypes, word)) {
        return kType;
    }
    if (Contains(kFunctions, word) || engine_core::shader_reserved_name(word)) {
        return kBuiltin;
    }
    return nullptr;
}

}  // namespace

std::vector<GlslSpan> highlight_glsl(std::string_view source) {
    const std::u32string text = Utf32(source);
    Spans spans;
    bool line_start = true;
    std::size_t at = 0;
    while (at < text.size()) {
        const char32_t c = text[at];
        const std::size_t start = at;
        if (c == U'/' && at + 1 < text.size() && text[at + 1] == U'/') {
            while (at < text.size() && text[at] != U'\n') {
                ++at;
            }
            spans.add(kComment, at - start);
            continue;
        }
        if (c == U'/' && at + 1 < text.size() && text[at + 1] == U'*') {
            const std::size_t close = text.find(U"*/", at + 2);
            at = close == std::u32string::npos ? text.size() : close + 2;
            spans.add(kComment, at - start);
            line_start = false;
            continue;
        }
        if (c == U'#' && line_start) {
            while (at < text.size() && text[at] != U'\n') {
                ++at;
            }
            spans.add(kKeyword, at - start);
            continue;
        }
        if (IsDigit(c) || (c == U'.' && at + 1 < text.size() && IsDigit(text[at + 1]))) {
            while (at < text.size()) {
                const char32_t n = text[at];
                const bool exponent_sign = (n == U'+' || n == U'-') && at > start &&
                                           (text[at - 1] == U'e' || text[at - 1] == U'E');
                if (!(IsWordChar(n) || n == U'.' || exponent_sign)) {
                    break;
                }
                ++at;
            }
            spans.add(kNumber, at - start);
            line_start = false;
            continue;
        }
        if (IsWordStart(c)) {
            while (at < text.size() && IsWordChar(text[at])) {
                ++at;
            }
            spans.add(WordStyle(text.substr(start, at - start)), at - start);
            line_start = false;
            continue;
        }
        if (c == U'\n') {
            line_start = true;
        } else if (!IsSpace(c)) {
            line_start = false;
        }
        spans.add(nullptr, 1);
        ++at;
    }
    return spans.take();
}

CssEnter enter_glsl(std::string_view source, int caret, std::string_view indent) {
    return enter_css(source, caret, indent);
}

CssBrace brace_glsl(std::string_view source, int caret) { return brace_css(source, caret); }

}  // namespace ide
