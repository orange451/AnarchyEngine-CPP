#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace ide {

// Code points the way JadeFX's text controls count them, so an index from a
// scan here is a caret position there. A malformed sequence is one U+FFFD.
inline std::u32string Utf32(std::string_view text) {
    std::u32string out;
    out.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[index++]);
        if (lead < 0x80) {
            out.push_back(lead);
            continue;
        }
        int need = 0;
        char32_t value = 0;
        if ((lead & 0xE0) == 0xC0 && lead >= 0xC2) {
            need = 1;
            value = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            need = 2;
            value = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0 && lead <= 0xF4) {
            need = 3;
            value = lead & 0x07;
        } else {
            out.push_back(0xFFFD);
            continue;
        }
        bool ok = true;
        for (int i = 0; i < need; ++i) {
            if (index >= text.size()) {
                ok = false;
                break;
            }
            const unsigned char next = static_cast<unsigned char>(text[index]);
            if ((next & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            value = (value << 6) | (next & 0x3F);
            ++index;
        }
        if (!ok || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
            out.push_back(0xFFFD);
            continue;
        }
        out.push_back(value);
    }
    return out;
}

// Bytes the code point at index spans, as Utf32 decodes it: a malformed
// sequence is one code point over its lead byte and the continuation bytes
// right after it. 1 at or past the end.
inline std::size_t Utf8Step(std::string_view text, std::size_t index) {
    if (index >= text.size()) {
        return 1;
    }
    const unsigned char lead = static_cast<unsigned char>(text[index]);
    int need = 0;
    if ((lead & 0xE0) == 0xC0 && lead >= 0xC2) {
        need = 1;
    } else if ((lead & 0xF0) == 0xE0) {
        need = 2;
    } else if ((lead & 0xF8) == 0xF0 && lead <= 0xF4) {
        need = 3;
    }
    std::size_t end = index + 1;
    for (int i = 0; i < need && end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80; ++i) {
        ++end;
    }
    return end - index;
}

// Code points in text, as Utf32 counts them.
inline int CodePoints(std::string_view text) {
    int count = 0;
    for (std::size_t index = 0; index < text.size(); index += Utf8Step(text, index)) {
        ++count;
    }
    return count;
}

// The byte where code point point starts. 0 for a negative point, text.size()
// at or past the end.
inline std::size_t CodePointByte(std::string_view text, int point) {
    std::size_t index = 0;
    for (int count = 0; count < point && index < text.size(); ++count) {
        index += Utf8Step(text, index);
    }
    return index;
}

// Code points that end at or before byte.
inline int CodePointsBefore(std::string_view text, std::size_t byte) {
    const std::size_t end = byte < text.size() ? byte : text.size();
    int count = 0;
    for (std::size_t index = 0; index < end;) {
        index += Utf8Step(text, index);
        if (index > end) {
            break;
        }
        ++count;
    }
    return count;
}

// The code point at index, decoded as above. 0 outside the text.
inline char32_t CodePointAt(std::string_view text, int index) {
    if (index < 0) {
        return 0;
    }
    const std::size_t at = CodePointByte(text, index);
    if (at >= text.size()) {
        return 0;
    }
    return Utf32(text.substr(at, Utf8Step(text, at))).front();
}

inline std::string Utf8(std::u32string_view text) {
    std::string out;
    for (char32_t code : text) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return out;
}

}  // namespace ide
