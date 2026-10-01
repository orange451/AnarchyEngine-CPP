#include "ide/CssHighlight.hpp"
#include "ide/Utf8.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

void expect(bool condition, const char* label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", label);
        ++gFailures;
    }
}

int total(const std::vector<ide::CssSpan>& spans) {
    int length = 0;
    for (const ide::CssSpan& span : spans) {
        length += span.length;
    }
    return length;
}

// The style at a code point, "plain" for none.
std::string style_at(const std::vector<ide::CssSpan>& spans, int index) {
    int cursor = 0;
    for (const ide::CssSpan& span : spans) {
        if (index < cursor + span.length) {
            return span.style != nullptr ? span.style : "plain";
        }
        cursor += span.length;
    }
    return "missing";
}

int at(const std::string& text, const char* needle) {
    return ide::CodePointsBefore(text, text.find(needle));
}

void TestTokens() {
    const std::string css =
        "/* top */\n#Toolbar label, .big:hover {\n  color: #ffcc00;\n  padding: 10px 2em;\n"
        "  font-family: \"Open Sans\";\n  width: calc(100% - 48px);\n}\n";
    const auto spans = ide::highlight_css(css);
    expect(total(spans) == ide::CodePoints(css), "the spans cover every code point");
    expect(style_at(spans, at(css, "top")) == "comment", "a comment");
    expect(style_at(spans, at(css, "#Toolbar")) == "keyword", "an id selector");
    expect(style_at(spans, at(css, "label")) == "keyword", "a type selector");
    expect(style_at(spans, at(css, ".big:hover")) == "keyword", "a class with a pseudo-class");
    expect(style_at(spans, at(css, ", .big")) == "plain", "the comma between selectors");
    expect(style_at(spans, at(css, "color")) == "builtin", "a property name");
    expect(style_at(spans, at(css, "#ffcc00")) == "datatype", "a hex color");
    expect(style_at(spans, at(css, "10px")) == "number", "a length");
    expect(style_at(spans, at(css, "px 2em")) == "number", "with its unit");
    expect(style_at(spans, at(css, "2em")) == "number", "a second length");
    expect(style_at(spans, at(css, "\"Open")) == "string", "a string");
    expect(style_at(spans, at(css, "calc")) == "plain", "a function name");
    expect(style_at(spans, at(css, "100%")) == "number", "a percentage");
    expect(style_at(spans, at(css, "48px")) == "number", "a length inside calc");
}

void TestOddText() {
    const std::string open = "label { color: red; /* never closed";
    auto spans = ide::highlight_css(open);
    expect(total(spans) == ide::CodePoints(open), "an unclosed comment is still covered");
    expect(style_at(spans, at(open, "never")) == "comment", "and runs to the end");
    const std::string wide = "label::before { content: \"\xE2\x86\x92\"; }";
    spans = ide::highlight_css(wide);
    expect(total(spans) == ide::CodePoints(wide), "lengths count code points, not bytes");
    expect(ide::highlight_css("").empty(), "nothing has no spans");
}

void TestEnter() {
    const std::string css = "label {}";
    const int inside = ide::CodePoints("label {");
    const ide::CssEnter split = ide::enter_css(css, inside, "  ");
    expect(split.text == "\n  \n", "Enter between braces indents a line and puts } below it");
    expect(split.caret == inside + 3, "the caret is on the indented line");

    const std::string body = "label {\n  color: red;";
    const ide::CssEnter keep = ide::enter_css(body, ide::CodePoints(body), "  ");
    expect(keep.text == "\n  ", "Enter keeps the line's indent");

    const std::string open = "label {";
    const ide::CssEnter after = ide::enter_css(open, ide::CodePoints(open), "\t");
    expect(after.text == "\n\t", "after { one level more, with no } to move");
}

void TestBrace() {
    const ide::CssBrace pair = ide::brace_css("label ", 6);
    expect(pair.insert && pair.text == "{}" && pair.caret == 7, "{ brings its }");
    expect(!ide::brace_css("label x", 6).insert, "but not before a name");
}

}  // namespace

int RunCssHighlightTests() {
    TestTokens();
    TestOddText();
    TestEnter();
    TestBrace();
    if (gFailures == 0) {
        std::printf("CSS highlight tests passed\n");
    }
    return gFailures;
}
