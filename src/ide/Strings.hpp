#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace ide {

// A to Z only. Other bytes, including every byte of a UTF-8 sequence, pass through.
inline char AsciiLower(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return static_cast<char>(c);
}

inline std::string AsciiLower(std::string_view text) {
    std::string out(text);
    for (char& unit : out) {
        unit = AsciiLower(static_cast<unsigned char>(unit));
    }
    return out;
}

// "1 result", "3 results".
inline std::string counted(std::size_t count, const char* one, const char* many) {
    return std::to_string(count) + " " + (count == 1 ? one : many);
}

}  // namespace ide
