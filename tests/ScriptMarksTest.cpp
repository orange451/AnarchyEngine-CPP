#include "ide/ScriptMarks.hpp"

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

engine_core::Diagnostic problem(engine_core::Severity severity, const char* code, const char* message, std::uint32_t line,
                                std::uint32_t character, std::uint32_t end_line, std::uint32_t end_character) {
    engine_core::Diagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.code = code;
    diagnostic.message = message;
    diagnostic.range.start.line = line;
    diagnostic.range.start.character = character;
    diagnostic.range.end.line = end_line;
    diagnostic.range.end.character = end_character;
    return diagnostic;
}

void testOffsets() {
    const std::string broken = "local x =";
    const std::vector<ide::ScriptMark> marks =
        ide::marks_for(broken, {problem(engine_core::Severity::Error, "Syntax", "Expected identifier", 0, 8, 0, 8)});
    expect(marks.size() == 1, "a syntax range becomes one mark");
    expect(marks[0].start == 8 && marks[0].end == 8, "the mark sits on the equals sign");
    expect(marks[0].code == "Syntax", "the mark keeps the syntax code");
    expect(ide::mark_covers(marks[0], 8), "the character under the caret is covered");
    expect(ide::problem_detail(marks[0]) == "line 1 · Will not compile", "syntax hover says it will not compile");

    const std::string lines = "local x =\nreturn 1\n";
    const std::vector<ide::ScriptMark> second = ide::marks_for(
        lines, {problem(engine_core::Severity::Warning, "Lint/UnknownGlobal", "Unknown global 'wait'", 1, 0, 1, 6)});
    expect(second.size() == 1 && second[0].start == 10 && second[0].end == 16, "the second line keeps the newline");

    const std::string emoji = "a\xF0\x9F\x98\x80" "b";
    const std::vector<ide::ScriptMark> glyph =
        ide::marks_for(emoji, {problem(engine_core::Severity::Warning, "Type", "mismatch", 0, 1, 0, 5)});
    expect(glyph.size() == 1 && glyph[0].start == 1 && glyph[0].end == 2, "a byte column lands on the code point");

    const std::vector<ide::ScriptMark> crlf =
        ide::marks_for("a\r\nb", {problem(engine_core::Severity::Error, "Syntax", "bad", 1, 0, 1, 1)});
    expect(crlf.size() == 1 && crlf[0].start == 2 && crlf[0].end == 3, "CR LF is one newline");
}

void testSummary() {
    const ide::ScriptProblemSummary syntax = ide::summarize_problems(
        {problem(engine_core::Severity::Error, "Syntax", "Expected identifier", 0, 8, 0, 8),
         problem(engine_core::Severity::Warning, "Lint/UnknownGlobal", "Unknown global 'wait'", 1, 0, 1, 4)});
    expect(syntax.blocks_compile, "a syntax error blocks compile");
    expect(syntax.text.find("Will not compile") == 0, "the banner leads with will not compile");
    expect(syntax.text.find("Expected identifier") != std::string::npos, "the banner includes the syntax message");
    expect(syntax.text.find("1 more") != std::string::npos, "another problem is counted");

    const ide::ScriptProblemSummary typed = ide::summarize_problems(
        {problem(engine_core::Severity::Error, "Type", "expected number, got string", 1, 6, 1, 9)});
    expect(!typed.blocks_compile, "a type error still compiles");
    expect(typed.text.find("Will not compile") == std::string::npos, "a type error does not say it will not compile");
    expect(typed.text.find("Error —") == 0, "a strict type error is labeled Error");

    const ide::ScriptProblemSummary waiting = ide::summarize_problems(
        {problem(engine_core::Severity::Warning, "Lint/UnknownGlobal", "Unknown global 'wait'", 0, 0, 0, 4)});
    expect(!waiting.blocks_compile, "an unknown global still compiles");
    expect(waiting.text.find("Warning —") == 0, "an unknown global is a warning");
    expect(waiting.text.find("Will not compile") == std::string::npos, "a warning does not say it will not compile");

    const ide::ScriptProblemSummary hint = ide::summarize_problems(
        {problem(engine_core::Severity::Hint, "Lint/LocalUnused", "x is never used", 0, 6, 0, 7)});
    expect(hint.text.empty(), "a hint does not take the banner");
    expect(!hint.blocks_compile, "a hint does not block compile");

    const ide::ScriptProblemSummary analysis = ide::summarize_problems(
        {problem(engine_core::Severity::Error, "Analysis", "script analysis definitions failed to load", 0, 0, 0, 0)});
    expect(analysis.text.empty(), "an analysis failure is not a squiggle or a compile banner");
    expect(ide::marks_for("return 1\n", {problem(engine_core::Severity::Error, "Analysis", "failed", 0, 0, 0, 0)}).empty(),
           "an analysis failure has no underline");
}

// A long message is cut at a whole character, so the banner never shows half of one.
void testLongMessage() {
    std::string message;
    for (int index = 0; index < 120; ++index) {
        message += "\xC3\xA9";  // e with an acute accent, two bytes
    }
    const std::vector<ide::ScriptMark> marks =
        ide::marks_for("return 1\n", {problem(engine_core::Severity::Error, "Type", message.c_str(), 0, 0, 0, 6)});
    expect(marks.size() == 1, "a long message still marks");
    if (marks.empty()) {
        return;
    }
    const std::string& shown = marks[0].message;
    expect(shown.size() <= 180, "a long message is cut to fit");
    expect(shown.size() >= 3 && shown.compare(shown.size() - 3, 3, "...") == 0, "a cut message ends in ...");
    const std::string kept = shown.substr(0, shown.size() - 3);
    expect(kept.size() % 2 == 0, "a cut message keeps whole characters");
    expect(engine_core::one_line("a\tb\nc") == "a b c", "tabs and line breaks become spaces");
}

}  // namespace

int RunScriptMarksTests() {
    gFailures = 0;
    testOffsets();
    testSummary();
    testLongMessage();
    return gFailures;
}
