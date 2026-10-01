#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ide {

// One run of code points in a stylesheet. style is null for plain text, or one
// of the script editor's syntax classes: keyword for a selector or an at-rule,
// builtin for a property name, datatype for a hex color, comment, string, or
// number. Lengths are Unicode code points, matching CodeArea.
struct CssSpan {
    const char* style = nullptr;
    int length = 0;
};

// Every code point of source falls in exactly one span, in order. Text it
// cannot make sense of, such as an unclosed comment, is still covered.
std::vector<CssSpan> highlight_css(std::string_view source);

// What Enter does at caret, a code point index with nothing selected: a new
// line with the current line's indent, one level more after a {, and, between
// { and }, the } on a line of its own below the caret. indent is one level.
struct CssEnter {
    int begin = 0;
    int end = 0;
    std::string text;
    int caret = 0;
};
CssEnter enter_css(std::string_view source, int caret, std::string_view indent);

// What typing { does at caret: { and a matching } after it, the caret between
// them, unless the next code point is already a name or a number.
struct CssBrace {
    bool insert = false;
    std::string text;
    int caret = 0;
};
CssBrace brace_css(std::string_view source, int caret);

}  // namespace ide
