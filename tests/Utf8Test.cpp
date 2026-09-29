#include "ide/ColorLiterals.hpp"
#include "ide/ScriptMarks.hpp"
#include "ide/TextSearch.hpp"
#include "ide/TextWrap.hpp"
#include "ide/Utf8.hpp"

#include <cstdio>
#include <string>
#include <vector>

// The IDE's UTF-8 walkers count code points the way JadeFX's text controls
// decode them, malformed bytes included, so a column found in a script is the
// caret position the editor shows.
namespace {

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

// A byte from each class a decoder tells apart: ASCII, continuation bytes, the
// leads C0 and C1 that are never valid, 2-, 3-, and 4-byte leads, and leads past F4.
const unsigned char kAlphabet[] = {0x00, 0x41, 0x7F, 0x80, 0x9F, 0xA0, 0xBF, 0xC0, 0xC1, 0xC2,
                                   0xDF, 0xE0, 0xED, 0xEF, 0xF0, 0xF4, 0xF5, 0xF8, 0xFF};

// Calls check with every string of 1 to max_length bytes drawn from kAlphabet.
template <typename Check>
void EveryString(std::string& text, std::size_t max_length, Check& check) {
    for (const unsigned char byte : kAlphabet) {
        text.push_back(static_cast<char>(byte));
        check(text);
        if (text.size() < max_length) {
            EveryString(text, max_length, check);
        }
        text.pop_back();
    }
}

void TestStepsMatchTheDecoder() {
    int mismatches = 0;
    auto check = [&mismatches](const std::string& text) {
        std::size_t steps = 0;
        for (std::size_t at = 0; at < text.size(); at += ide::Utf8Step(text, at)) {
            ++steps;
        }
        if (steps != ide::Utf32(text).size() || ide::CodePoints(text) != static_cast<int>(steps)) {
            ++mismatches;
        }
    };
    std::string text;
    EveryString(text, 5, check);
    Expect(mismatches == 0, "Utf8Step and CodePoints count what Utf32 decodes, malformed bytes included");
}

void TestBytesAndCodePoints() {
    // "\xE2\x82" is one malformed code point, then A, then a two-byte e with an acute accent.
    const std::string text = "\xE2\x82"
                             "A"
                             "\xC3\xA9";
    Expect(ide::CodePoints(text) == 3, "a cut-off sequence is one code point");
    Expect(ide::CodePointByte(text, 0) == 0, "code point 0 starts at byte 0");
    Expect(ide::CodePointByte(text, 1) == 2, "code point 1 starts after the cut-off sequence");
    Expect(ide::CodePointByte(text, 2) == 3, "code point 2 starts at the accented e");
    Expect(ide::CodePointByte(text, 3) == 5, "the code point past the last starts at the end");
    Expect(ide::CodePointByte(text, 9) == 5, "a code point past the end is the end");
    Expect(ide::CodePointByte(text, -1) == 0, "a negative code point is the start");
    Expect(ide::CodePointsBefore(text, 2) == 1, "one code point ends by byte 2");
    Expect(ide::CodePointsBefore(text, 4) == 2, "a byte inside a code point does not count it");
    Expect(ide::CodePointsBefore(text, 99) == 3, "every code point ends by a byte past the end");
}

void TestWalkersAgreeOnMalformedText() {
    const std::vector<std::string> lines =
        ide::WrapText("\xFF"
                      "abc",
                      2, [](const std::string& text) { return static_cast<double>(text.size()); });
    Expect(lines == std::vector<std::string>{"\xFF"
                                             "a",
                                             "bc"},
           "a stray byte wraps as one code point");

    // A stray continuation byte in a string on the line before is one code point.
    const std::vector<ide::Color3Literal> colors = ide::find_color3_literals("local s = \""
                                                                             "\x80"
                                                                             "\"\n"
                                                                             "Color3.new(1, 0, 0)");
    Expect(colors.size() == 1 && colors[0].start == 14, "a color literal after a stray byte starts where the editor shows it");

    ide::SearchQuery query;
    query.pattern = "x";
    const std::vector<ide::TextMatch> matches = ide::TextSearch(query).find_all("\x80"
                                                                               "x");
    Expect(matches.size() == 1 && matches[0].start == 1 && matches[0].end == 2,
           "a match after a stray byte starts at code point 1");

    // A byte regex can stop inside a character. The search then resumes after
    // that character, so the upside-down A (E2 88 80) gets no match on its last byte.
    ide::SearchQuery bytes;
    bytes.pattern = "[\xE2\x82\xAC]*";
    bytes.match_case = true;
    bytes.regex = true;
    const std::vector<ide::TextMatch> cut = ide::TextSearch(bytes).find_all("\xE2\x88\x80");
    Expect(cut.size() == 3, "an empty match inside a character resumes after the character");

    engine_core::Diagnostic problem;
    problem.severity = engine_core::Severity::Error;
    problem.code = "Syntax";
    problem.message = "x";
    problem.range.start.line = 0;
    problem.range.start.character = 4;
    problem.range.end.line = 0;
    problem.range.end.character = 5;
    const std::vector<ide::ScriptMark> marks = ide::marks_for("\xE2\x82"
                                                              "A x",
                                                              {problem});
    Expect(marks.size() == 1 && marks[0].start == 3, "a mark after a cut-off sequence starts where the editor shows it");
}

}  // namespace

int RunUtf8Tests() {
    gFailures = 0;
    TestStepsMatchTheDecoder();
    TestBytesAndCodePoints();
    TestWalkersAgreeOnMalformedText();
    if (gFailures == 0) {
        std::printf("utf-8 tests passed\n");
    }
    return gFailures;
}
