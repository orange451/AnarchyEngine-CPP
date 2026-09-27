#include "ide/TextWrap.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

// Every byte is one unit wide, so widths are easy to read in the cases.
double Bytes(const std::string& text) { return static_cast<double>(text.size()); }

void ExpectLines(const std::vector<std::string>& got, const std::vector<std::string>& want, const char* label) {
    if (got == want) {
        return;
    }
    std::fprintf(stderr, "FAIL %s: got", label);
    for (const std::string& line : got) {
        std::fprintf(stderr, " [%s]", line.c_str());
    }
    std::fprintf(stderr, "\n");
    ++gFailures;
}

}  // namespace

int RunTextWrapTests() {
    ExpectLines(ide::WrapText("short", 20, Bytes), {"short"}, "text that fits is one line");
    ExpectLines(ide::WrapText("", 20, Bytes), {""}, "empty text is one empty line");
    ExpectLines(ide::WrapText("Named constants. NormalId and Axis", 16, Bytes),
                {"Named constants.", "NormalId and", "Axis"}, "wraps at spaces");
    ExpectLines(ide::WrapText("one  two   three", 9, Bytes), {"one two", "three"}, "runs of spaces become one");
    ExpectLines(ide::WrapText("first\nsecond line here", 11, Bytes), {"first", "second line", "here"},
                "a newline starts a new line");
    ExpectLines(ide::WrapText("a\n\nb", 5, Bytes), {"a", "", "b"}, "a blank line is kept");
    ExpectLines(ide::WrapText("go Vector3.FromNormalId now", 8, Bytes), {"go", "Vector3.", "FromNorm", "alId now"},
                "a word wider than the line is split");
    // Three two-byte code points: a split never lands inside one.
    ExpectLines(ide::WrapText("\xc3\xa9\xc3\xa9\xc3\xa9", 3, Bytes), {"\xc3\xa9", "\xc3\xa9", "\xc3\xa9"},
                "a split keeps code points whole");
    ExpectLines(ide::WrapText("abc", 0, Bytes), {"a", "b", "c"}, "a line too narrow for anything still advances");
    return gFailures;
}
