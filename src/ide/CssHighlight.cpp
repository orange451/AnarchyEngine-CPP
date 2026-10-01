#include "CssHighlight.hpp"

#include "Utf8.hpp"

#include <algorithm>

namespace ide {
namespace {

constexpr const char* kSelector = "keyword";
constexpr const char* kProperty = "builtin";
constexpr const char* kColor = "datatype";
constexpr const char* kComment = "comment";
constexpr const char* kString = "string";
constexpr const char* kNumber = "number";

bool IsSpace(char32_t c) { return c == U' ' || c == U'\t' || c == U'\n' || c == U'\r' || c == U'\f'; }

bool IsDigit(char32_t c) { return c >= U'0' && c <= U'9'; }

bool IsNameChar(char32_t c) {
    return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || IsDigit(c) || c == U'-' || c == U'_' || c > 0x7f;
}

bool IsHex(char32_t c) { return IsDigit(c) || (c >= U'a' && c <= U'f') || (c >= U'A' && c <= U'F'); }

class Spans {
public:
    void add(const char* style, int length) {
        if (length <= 0) {
            return;
        }
        if (!out_.empty() && out_.back().style == style) {
            out_.back().length += length;
        } else {
            out_.push_back(CssSpan{style, length});
        }
    }
    std::vector<CssSpan> take() { return std::move(out_); }

private:
    std::vector<CssSpan> out_;
};

// Where the quoted string starting at at ends, past its closing quote, or at
// the end of its line when it has none.
std::size_t StringEnd(const std::u32string& text, std::size_t at) {
    const char32_t quote = text[at];
    std::size_t end = at + 1;
    while (end < text.size() && text[end] != quote && text[end] != U'\n') {
        end += text[end] == U'\\' && end + 1 < text.size() ? 2 : 1;
    }
    return std::min(text.size(), end < text.size() && text[end] == quote ? end + 1 : end);
}

}  // namespace

std::vector<CssSpan> highlight_css(std::string_view source) {
    const std::u32string text = Utf32(source);
    Spans spans;
    // Inside a rule's braces, and there after a property's colon.
    int depth = 0;
    bool value = false;
    std::size_t at = 0;
    while (at < text.size()) {
        const char32_t c = text[at];
        const std::size_t start = at;
        if (c == U'/' && at + 1 < text.size() && text[at + 1] == U'*') {
            const std::size_t close = text.find(U"*/", at + 2);
            at = close == std::u32string::npos ? text.size() : close + 2;
            spans.add(kComment, static_cast<int>(at - start));
            continue;
        }
        if (c == U'"' || c == U'\'') {
            at = StringEnd(text, at);
            spans.add(kString, static_cast<int>(at - start));
            continue;
        }
        if (c == U'{') {
            ++depth;
            value = false;
            spans.add(nullptr, 1);
            ++at;
            continue;
        }
        if (c == U'}') {
            depth = std::max(0, depth - 1);
            value = false;
            spans.add(nullptr, 1);
            ++at;
            continue;
        }
        if (depth == 0) {
            // A selector, or an at-rule, up to the next brace, comma, or space.
            if (IsSpace(c) || c == U',') {
                spans.add(nullptr, 1);
                ++at;
                continue;
            }
            while (at < text.size() && !IsSpace(text[at]) && text[at] != U',' && text[at] != U'{' &&
                   text[at] != U'}' && !(text[at] == U'/' && at + 1 < text.size() && text[at + 1] == U'*')) {
                ++at;
            }
            spans.add(kSelector, static_cast<int>(at - start));
            continue;
        }
        if (!value) {
            if (c == U':') {
                value = true;
                spans.add(nullptr, 1);
                ++at;
                continue;
            }
            if (IsNameChar(c)) {
                while (at < text.size() && IsNameChar(text[at])) {
                    ++at;
                }
                spans.add(kProperty, static_cast<int>(at - start));
                continue;
            }
            spans.add(nullptr, 1);
            ++at;
            continue;
        }
        // A value, up to the declaration's end.
        if (c == U';') {
            value = false;
            spans.add(nullptr, 1);
            ++at;
            continue;
        }
        if (c == U'#' && at + 1 < text.size() && IsHex(text[at + 1])) {
            ++at;
            while (at < text.size() && IsNameChar(text[at])) {
                ++at;
            }
            spans.add(kColor, static_cast<int>(at - start));
            continue;
        }
        const bool signed_number =
            (c == U'-' || c == U'+' || c == U'.') && at + 1 < text.size() && IsDigit(text[at + 1]);
        if (IsDigit(c) || signed_number) {
            ++at;
            while (at < text.size() && (IsDigit(text[at]) || text[at] == U'.')) {
                ++at;
            }
            // Its unit: px, em, %, and the rest.
            while (at < text.size() && (IsNameChar(text[at]) || text[at] == U'%') && !IsDigit(text[at]) &&
                   text[at] != U'-') {
                ++at;
            }
            spans.add(kNumber, static_cast<int>(at - start));
            continue;
        }
        if (IsNameChar(c)) {
            // A keyword or a function name: plain, as names in a value are.
            while (at < text.size() && IsNameChar(text[at])) {
                ++at;
            }
            spans.add(nullptr, static_cast<int>(at - start));
            continue;
        }
        spans.add(nullptr, 1);
        ++at;
    }
    return spans.take();
}

CssEnter enter_css(std::string_view source, int caret, std::string_view indent) {
    const std::u32string text = Utf32(source);
    const std::size_t at = std::min(static_cast<std::size_t>(std::max(caret, 0)), text.size());
    std::size_t line = at;
    while (line > 0 && text[line - 1] != U'\n') {
        --line;
    }
    std::size_t lead = line;
    while (lead < at && (text[lead] == U' ' || text[lead] == U'\t')) {
        ++lead;
    }
    const std::string own = Utf8(text.substr(line, lead - line));
    // The last code point before the caret that is not a space, on this line.
    std::size_t before = at;
    while (before > line && (text[before - 1] == U' ' || text[before - 1] == U'\t')) {
        --before;
    }
    const bool opens = before > line && text[before - 1] == U'{';
    const bool closes = at < text.size() && text[at] == U'}';

    CssEnter out;
    out.begin = static_cast<int>(at);
    out.end = static_cast<int>(at);
    out.text = "\n" + own;
    if (opens) {
        out.text += std::string(indent);
    }
    out.caret = static_cast<int>(at + Utf32(out.text).size());
    if (opens && closes) {
        out.text += "\n" + own;
    }
    return out;
}

CssBrace brace_css(std::string_view source, int caret) {
    const std::u32string text = Utf32(source);
    const std::size_t at = std::min(static_cast<std::size_t>(std::max(caret, 0)), text.size());
    CssBrace out;
    if (at < text.size() && IsNameChar(text[at])) {
        return out;
    }
    out.insert = true;
    out.text = "{}";
    out.caret = static_cast<int>(at) + 1;
    return out;
}

}  // namespace ide
