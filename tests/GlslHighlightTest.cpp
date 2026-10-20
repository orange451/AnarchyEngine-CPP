#include "ide/GlslHighlight.hpp"
#include "ide/Utf8.hpp"

#include <cstdio>
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

int total(const std::vector<ide::GlslSpan>& spans) {
    int length = 0;
    for (const ide::GlslSpan& span : spans) {
        length += span.length;
    }
    return length;
}

// The style at a code point, "plain" for none.
std::string style_at(const std::vector<ide::GlslSpan>& spans, int index) {
    int cursor = 0;
    for (const ide::GlslSpan& span : spans) {
        if (index < cursor + span.length) {
            return span.style != nullptr ? span.style : "plain";
        }
        cursor += span.length;
    }
    return "missing";
}

int at(const std::string& text, const char* needle) { return ide::CodePointsBefore(text, text.find(needle)); }

void TestTokens() {
    const std::string glsl =
        "shader surface;\nrender_mode two_sided;\n// a comment\nuniform float Progress : range(0, 1) = 0.5;\n"
        "uniform sampler2D Noise;\n#define STRENGTH 4.0\nvoid surface() {\n    float n = texture(Noise, UV).r;\n"
        "    if (n < Progress) { discard; }\n    EMISSIVE += vec3(1.0) * smoothstep(0.0, 1.0, n);\n}\n";
    const auto spans = ide::highlight_glsl(glsl);
    expect(total(spans) == ide::CodePoints(glsl), "the spans cover every code point");
    expect(style_at(spans, at(glsl, "shader")) == "keyword", "shader is a keyword");
    expect(style_at(spans, at(glsl, "render_mode")) == "keyword", "render_mode is a keyword");
    expect(style_at(spans, at(glsl, "a comment")) == "comment", "a // comment");
    expect(style_at(spans, at(glsl, "uniform")) == "keyword", "uniform is a keyword");
    expect(style_at(spans, at(glsl, "float Progress")) == "datatype", "float is a type");
    expect(style_at(spans, at(glsl, "sampler2D")) == "datatype", "sampler2D is a type");
    expect(style_at(spans, at(glsl, "Progress")) == "plain", "a uniform's name is plain");
    expect(style_at(spans, at(glsl, "0.5")) == "number", "a number");
    expect(style_at(spans, at(glsl, "#define")) == "keyword", "a preprocessor line");
    expect(style_at(spans, at(glsl, "STRENGTH")) == "keyword", "the whole preprocessor line");
    expect(style_at(spans, at(glsl, "void")) == "datatype", "void is a type");
    expect(style_at(spans, at(glsl, "texture(")) == "builtin", "texture is a built-in function");
    expect(style_at(spans, at(glsl, "UV)")) == "builtin", "UV is the engine's name");
    expect(style_at(spans, at(glsl, "if (")) == "keyword", "if is a keyword");
    expect(style_at(spans, at(glsl, "discard")) == "keyword", "discard is a keyword");
    expect(style_at(spans, at(glsl, "EMISSIVE")) == "builtin", "EMISSIVE is the engine's name");
    expect(style_at(spans, at(glsl, "smoothstep")) == "builtin", "smoothstep is a built-in function");
    expect(style_at(spans, at(glsl, "vec3(1.0)")) == "datatype", "vec3 is a type");
}

void TestOddText() {
    for (const std::string text : {std::string(), std::string("/* never closed"), std::string("#"),
                                   std::string("1.5e"), std::string("é = 2;\n"), std::string("a/b/*c*/d")}) {
        expect(total(ide::highlight_glsl(text)) == ide::CodePoints(text), "odd text is still covered");
    }
    const std::string block = "a /* x\ny */ b";
    const auto spans = ide::highlight_glsl(block);
    expect(style_at(spans, at(block, "y */")) == "comment", "a block comment spans lines");
    expect(style_at(spans, at(block, " b")) == "plain", "and ends at */");
}

void TestEnterAndBrace() {
    const std::string source = "void surface() {}";
    const int inside = at(source, "}");
    const ide::CssEnter enter = ide::enter_glsl(source, inside, "    ");
    expect(enter.text.find("\n    ") == 0, "Enter between braces indents a level");
    expect(enter.text.find("\n}") != std::string::npos || enter.text.back() == '\n' || enter.text.find('\n', 1) != std::string::npos,
           "and puts the } on its own line");
    const ide::CssBrace brace = ide::brace_glsl("void surface() ", 15);
    expect(brace.insert && brace.text == "{}", "{ brings its }");
}

}  // namespace

int RunGlslHighlightTests() {
    TestTokens();
    TestOddText();
    TestEnterAndBrace();
    if (gFailures == 0) {
        std::printf("GLSL highlight tests passed\n");
    }
    return gFailures;
}
