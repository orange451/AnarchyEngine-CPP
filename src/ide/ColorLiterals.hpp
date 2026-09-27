#pragma once

#include "Color3.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ide {

// A Color3 written out in a script with constant arguments, such as
// Color3.fromRGB(255, 128, 0) or Color3.fromHex("#1a73e8"). The editor shows a
// swatch before each one and rewrites it from the color picker.
struct Color3Literal {
    enum class Form { New, FromRGB, FromHSV, FromHex };

    // Code points, as the editor counts: Color3 starts at start and ) ends before end.
    int start = 0;
    int end = 0;
    Form form = Form::New;
    engine_core::Color3 color;
    // For fromHex: the quote the string used, whether it had a #, and whether its letters were capitals.
    char quote = '"';
    bool hash = true;
    bool upper = false;
};

// Every Color3.new, fromRGB, fromHSV, and fromHex call whose arguments are all
// literals: numbers (a leading - allowed) or, for fromHex, one string. Calls in
// comments and strings do not count, nor ones with expressions for arguments.
std::vector<Color3Literal> find_color3_literals(std::string_view source);

// The call that writes color in the literal's own form, keeping a hex code's
// quote, #, and case. new and fromHSV use up to three decimals and fromRGB whole numbers.
std::string format_color3_literal(const Color3Literal& literal, const engine_core::Color3& color);

}  // namespace ide
