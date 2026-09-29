#include "ColorLiterals.hpp"

#include "LuauHighlight.hpp"
#include "Strings.hpp"
#include "Utf8.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ide {
namespace {

// The source as single bytes with a code point index for each, so a match found in
// bytes reports the positions the editor uses.
struct Cursor {
    std::string_view text;
    std::vector<int> code_point;  // byte index -> code point index
    std::size_t at = 0;

    explicit Cursor(std::string_view source) : text(source) {
        code_point.reserve(source.size() + 1);
        // A byte inside a code point maps to the code point after it.
        int count = 0;
        for (std::size_t index = 0; index < source.size();) {
            const std::size_t step = Utf8Step(source, index);
            code_point.push_back(count);
            ++count;
            for (std::size_t i = 1; i < step; ++i) {
                code_point.push_back(count);
            }
            index += step;
        }
        code_point.push_back(count);
    }

    bool done() const { return at >= text.size(); }
    char peek() const { return done() ? '\0' : text[at]; }
    void skip_space() {
        while (!done() && std::isspace(static_cast<unsigned char>(text[at])) != 0) {
            ++at;
        }
    }
    bool take(char unit) {
        skip_space();
        if (peek() != unit) {
            return false;
        }
        ++at;
        return true;
    }
    std::string_view name() {
        skip_space();
        const std::size_t begin = at;
        while (!done() && (std::isalnum(static_cast<unsigned char>(text[at])) != 0 || text[at] == '_')) {
            ++at;
        }
        return text.substr(begin, at - begin);
    }
    // A decimal number, with an optional - in front. Hex and exponents are left to real code.
    bool number(double& value) {
        skip_space();
        const std::size_t begin = at;
        if (peek() == '-') {
            ++at;
        }
        bool digits = false;
        while (!done() && (std::isdigit(static_cast<unsigned char>(text[at])) != 0 || text[at] == '.')) {
            digits = digits || text[at] != '.';
            ++at;
        }
        if (!digits) {
            at = begin;
            return false;
        }
        const std::string literal(text.substr(begin, at - begin));
        char* stop = nullptr;
        value = std::strtod(literal.c_str(), &stop);
        return stop != nullptr && *stop == '\0';
    }
    bool string(std::string& value, char& quote) {
        skip_space();
        quote = peek();
        if (quote != '"' && quote != '\'') {
            return false;
        }
        const std::size_t close = text.find(quote, at + 1);
        if (close == std::string_view::npos || text.substr(at + 1, close - at - 1).find('\n') != std::string_view::npos) {
            return false;
        }
        value = std::string(text.substr(at + 1, close - at - 1));
        at = close + 1;
        return true;
    }
};

// The byte where each datatype token named Color3 starts. The highlighter already
// knows which code is live, so comments and strings drop out here.
std::vector<std::size_t> color3_tokens(std::string_view source, const Cursor& cursor) {
    std::vector<std::size_t> starts;
    int code_point = 0;
    std::size_t byte = 0;
    for (const LuauSpan& span : highlight_luau(source)) {
        const std::size_t begin = byte;
        const int end_point = code_point + span.length;
        while (byte < source.size() && cursor.code_point[byte] < end_point) {
            ++byte;
        }
        if (span.style != nullptr && std::strcmp(span.style, "datatype") == 0 && source.substr(begin, byte - begin) == "Color3") {
            starts.push_back(begin);
        }
        code_point = end_point;
    }
    return starts;
}

// Parses the call after Color3 at the cursor. Fills the form and color.
bool parse_call(Cursor& cursor, Color3Literal& literal) {
    if (!cursor.take('.')) {
        return false;
    }
    const std::string_view name = cursor.name();
    if (name == "new") {
        literal.form = Color3Literal::Form::New;
    } else if (name == "fromRGB") {
        literal.form = Color3Literal::Form::FromRGB;
    } else if (name == "fromHSV") {
        literal.form = Color3Literal::Form::FromHSV;
    } else if (name == "fromHex") {
        literal.form = Color3Literal::Form::FromHex;
    } else {
        return false;
    }
    if (!cursor.take('(')) {
        return false;
    }
    if (literal.form == Color3Literal::Form::FromHex) {
        std::string hex;
        if (!cursor.string(hex, literal.quote) || !cursor.take(')')) {
            return false;
        }
        literal.hash = !hex.empty() && hex[0] == '#';
        literal.upper = std::any_of(hex.begin(), hex.end(), [](char unit) { return unit >= 'A' && unit <= 'F'; });
        return engine_core::color3_from_hex(hex, literal.color);
    }
    double values[3] = {0, 0, 0};
    int count = 0;
    cursor.skip_space();
    if (cursor.peek() != ')') {
        do {
            if (count == 3 || !cursor.number(values[count])) {
                return false;
            }
            ++count;
        } while (cursor.take(','));
    }
    if (!cursor.take(')')) {
        return false;
    }
    switch (literal.form) {
        case Color3Literal::Form::FromRGB:
            literal.color = {static_cast<float>(values[0] / 255.0), static_cast<float>(values[1] / 255.0),
                             static_cast<float>(values[2] / 255.0)};
            return true;
        case Color3Literal::Form::FromHSV:
            if (count != 3) {
                return false;
            }
            literal.color = engine_core::color3_from_hsv(values[0], values[1], values[2]);
            return true;
        default:
            literal.color = {static_cast<float>(values[0]), static_cast<float>(values[1]), static_cast<float>(values[2])};
            return true;
    }
}

// Up to three decimals with the trailing zeros dropped: 1, 0.5, 0.102.
std::string decimal(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", value);
    std::string text = buffer;
    while (!text.empty() && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text == "-0" ? "0" : text;
}

}  // namespace

std::vector<Color3Literal> find_color3_literals(std::string_view source) {
    std::vector<Color3Literal> literals;
    Cursor cursor(source);
    for (const std::size_t begin : color3_tokens(source, cursor)) {
        cursor.at = begin + 6;
        Color3Literal literal;
        if (parse_call(cursor, literal)) {
            literal.start = cursor.code_point[begin];
            literal.end = cursor.code_point[cursor.at];
            literals.push_back(literal);
        }
    }
    return literals;
}

std::string format_color3_literal(const Color3Literal& literal, const engine_core::Color3& color) {
    switch (literal.form) {
        case Color3Literal::Form::FromRGB: {
            auto byte = [](float channel) { return std::to_string(std::lround(std::clamp(channel, 0.f, 1.f) * 255.f)); };
            return "Color3.fromRGB(" + byte(color.r) + ", " + byte(color.g) + ", " + byte(color.b) + ")";
        }
        case Color3Literal::Form::FromHSV: {
            double hue = 0;
            double saturation = 0;
            double value = 0;
            engine_core::color3_to_hsv(color, hue, saturation, value);
            return "Color3.fromHSV(" + decimal(hue) + ", " + decimal(saturation) + ", " + decimal(value) + ")";
        }
        case Color3Literal::Form::FromHex: {
            std::string hex = engine_core::color3_to_hex(color);
            if (!literal.upper) {
                hex = AsciiLower(hex);
            }
            const std::string quote(1, literal.quote);
            return "Color3.fromHex(" + quote + (literal.hash ? "#" : "") + hex + quote + ")";
        }
        default:
            return "Color3.new(" + decimal(color.r) + ", " + decimal(color.g) + ", " + decimal(color.b) + ")";
    }
}

}  // namespace ide
