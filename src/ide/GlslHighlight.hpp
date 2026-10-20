#pragma once

#include "CssHighlight.hpp"

#include <string_view>
#include <vector>

namespace ide {

// One run of code points in a shader. style is null for plain text, or one of
// the script editor's syntax classes: keyword (control words, qualifiers, the
// shader header's words, and whole preprocessor lines), datatype (GLSL types),
// builtin (the engine's names and GLSL's functions), number, or comment.
// Lengths are Unicode code points, matching CodeArea.
struct GlslSpan {
    const char* style = nullptr;
    int length = 0;
};

// Every code point of source falls in exactly one span, in order. Text it
// cannot make sense of, such as an unclosed comment, is still covered.
std::vector<GlslSpan> highlight_glsl(std::string_view source);

// Enter and { in a shader, as the CSS editor has them: GLSL is a brace
// language, so the same rules hold.
CssEnter enter_glsl(std::string_view source, int caret, std::string_view indent);
CssBrace brace_glsl(std::string_view source, int caret);

}  // namespace ide
