#include "ide/ColorLiterals.hpp"

#include <cmath>
#include <cstdio>
#include <string>

// Finding Color3 literals in a script and writing them back.
namespace {

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

bool Near(float a, float b) { return std::fabs(a - b) < 0.002f; }

}  // namespace

int RunColorLiteralsTests() {
    gFailures = 0;
    const std::string source =
        "local a = Color3.new(1, 0.5)\n"
        "local b = Color3.fromRGB( 255 , 128, 0 )\n"
        "-- Color3.new(1, 1, 1) in a comment\n"
        "local c = \"Color3.new(0, 0, 0)\"\n"
        "local d = Color3.fromHex('#1A73E8')\n"
        "local e = Color3.fromHSV(0.5, 1, 1)\n"
        "local f = Color3.new(x, 0, 0)\n"
        "local g = Color3.fromHSV(0.5, 1)\n"
        "local h = Color3.fromRGB(10, 20, 30, 40)\n"
        "local \xC3\xA9 = Color3.new(0, 0, 1)\n";
    const std::vector<ide::Color3Literal> found = ide::find_color3_literals(source);
    Expect(found.size() == 5, "five literals, none from comments, strings, variables, or wrong argument counts");
    if (found.size() != 5) {
        return gFailures;
    }
    const ide::Color3Literal& a = found[0];
    Expect(a.form == ide::Color3Literal::Form::New && Near(a.color.r, 1) && Near(a.color.g, 0.5f) && Near(a.color.b, 0),
           "Color3.new fills an omitted channel with 0");
    Expect(a.start == 10 && a.end == 28, "the literal spans Color3 to its closing parenthesis");
    const ide::Color3Literal& b = found[1];
    Expect(b.form == ide::Color3Literal::Form::FromRGB && Near(b.color.g, 128 / 255.f), "fromRGB takes 0 to 255");
    const ide::Color3Literal& d = found[2];
    Expect(d.form == ide::Color3Literal::Form::FromHex && d.quote == '\'' && d.hash && d.upper &&
               Near(d.color.r, 26 / 255.f),
           "fromHex records its quote, #, and capitals");
    Expect(found[3].form == ide::Color3Literal::Form::FromHSV && Near(found[3].color.g, 1) && Near(found[3].color.r, 0),
           "fromHSV converts");
    const ide::Color3Literal& last = found[4];
    Expect(last.start == 324 && last.end - last.start == 19,
           "positions count code points, not bytes, after an accented name");

    const engine_core::Color3 orange{1.f, 0.5f, 0.f};
    Expect(ide::format_color3_literal(a, orange) == "Color3.new(1, 0.5, 0)", "new writes up to three decimals");
    Expect(ide::format_color3_literal(b, orange) == "Color3.fromRGB(255, 128, 0)", "fromRGB writes whole numbers");
    Expect(ide::format_color3_literal(d, orange) == "Color3.fromHex('#FF8000')", "fromHex keeps its quote, #, and case");
    Expect(ide::format_color3_literal(found[3], orange) == "Color3.fromHSV(0.083, 1, 1)", "fromHSV writes hue, saturation, value");

    if (gFailures == 0) {
        std::printf("color literal tests passed\n");
    }
    return gFailures;
}
