#include "ide/LuauHighlight.hpp"

#include "LuaApi.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

int gFailures = 0;

void expect(bool condition, const char* label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", label);
        ++gFailures;
    }
}

int span_length(const std::vector<ide::LuauSpan>& spans) {
    int total = 0;
    for (const ide::LuauSpan& span : spans) {
        total += span.length;
    }
    return total;
}

int code_points(std::string_view text) {
    int count = 0;
    for (std::size_t index = 0; index < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[index++]);
        ++count;
        if (lead < 0x80) {
            continue;
        }
        int need = lead < 0xE0 ? 1 : lead < 0xF0 ? 2 : 3;
        index += static_cast<std::size_t>(need);
    }
    return count;
}

const char* style_at(const std::vector<ide::LuauSpan>& spans, int index) {
    int cursor = 0;
    for (const ide::LuauSpan& span : spans) {
        if (index < cursor + span.length) {
            return span.style;
        }
        cursor += span.length;
    }
    return "missing";
}

bool same(const char* style, const char* expected) {
    if (style == nullptr || expected == nullptr) {
        return style == expected;
    }
    return std::string_view(style) == expected;
}

void expect_style(const std::string& text, int index, const char* expected, const char* label) {
    const std::vector<ide::LuauSpan> spans = ide::highlight_luau(text);
    expect(span_length(spans) == code_points(text), label);
    expect(same(style_at(spans, index), expected), label);
}

}  // namespace

int RunLuauHighlightTests() {
    expect_style("local script = game", 0, "keyword", "local is a keyword");
    expect_style("local script = game", 6, "builtin", "script is a builtin");
    expect_style("local script = game", 15, "builtin", "game is a builtin");
    expect_style("task.wait(0.2)", 0, "builtin", "task is a builtin");
    expect_style("task.wait(0.2)", 5, nullptr, "a method stays plain");
    expect_style("task.wait(0.2)", 10, "number", "a decimal is a number");

    expect_style("-- local script", 3, "comment", "a comment hides keywords");
    expect_style("--[[ local\nscript ]]", 4, "comment", "a long comment stays a comment");
    expect_style("--[[ local\nscript ]]", 12, "comment", "a newline inside a long comment is a comment");
    expect_style("\"local\"", 1, "string", "a keyword inside a string is a string");
    expect_style("'game'", 1, "string", "a single-quoted string is a string");
    expect_style("[[script]]", 2, "string", "a long string is a string");
    expect_style("[=[game]=]", 3, "string", "a long string with equals is a string");
    expect_style("`hello {script}`", 8, "builtin", "an interpolation is highlighted");
    expect_style("`hello {script}`", 1, "string", "text around an interpolation stays a string");

    expect_style("continue", 0, "keyword", "continue is a keyword");
    expect_style("export type Vec", 0, "keyword", "export is a keyword");
    expect_style("export type Vec", 7, "keyword", "type is a keyword");
    expect_style("0xFF", 0, "number", "a hex literal is a number");
    expect_style(".5", 0, "number", "a leading dot is a number");
    expect_style("Instance.new", 0, "datatype", "Instance is a datatype");
    expect_style("local v = Vector3.new(1, 2, 3)", 10, "datatype", "Vector3 is a datatype");
    expect_style("local v = Vector3.new(1, 2, 3)", 18, nullptr, "a datatype's constructor stays plain");
    expect_style("Enum.Material.Plastic", 0, "datatype", "Enum is a datatype");
    expect_style("local p: Vector3 = v", 9, "datatype", "a type annotation names the datatype");
    expect_style("local Vector3s = {}", 6, nullptr, "a longer name is not the datatype");
    expect_style("-- Vector3", 3, "comment", "a comment hides datatypes");
    expect_style("\"Vector3\"", 1, "string", "a datatype inside a string is a string");

    // Every capitalized global the engine installs is a datatype, so a new one cannot go uncolored.
    std::vector<engine_core::LuaSymbol> globals;
    engine_core::lua_library_globals(globals);
    int capitalized = 0;
    for (const engine_core::LuaSymbol& global : globals) {
        if (global.name.empty() || global.name[0] < 'A' || global.name[0] > 'Z') {
            continue;
        }
        ++capitalized;
        const std::string label = "the global " + global.name + " is highlighted as a datatype";
        expect_style(global.name, 0, "datatype", label.c_str());
    }
    expect(capitalized >= 3, "the engine installs Instance, Vector3, and Enum");
    expect_style("_G", 0, "builtin", "_G is a builtin");

    const std::string utf = "local café = 1";
    const std::vector<ide::LuauSpan> spans = ide::highlight_luau(utf);
    expect(span_length(spans) == code_points(utf), "a multibyte character is one code point");
    expect(same(style_at(spans, 0), "keyword"), "local before a multibyte name is a keyword");

    const std::string sample = "local tri = game:FindFirstChild(\"Tri0\")\n";
    const std::vector<ide::LuauSpan> play = ide::highlight_luau(sample);
    expect(span_length(play) == code_points(sample), "a play script covers every code point");
    expect(same(style_at(play, 0), "keyword"), "a play script keywords local");
    expect(same(style_at(play, 12), "builtin"), "a play script keywords game");
    return gFailures;
}
