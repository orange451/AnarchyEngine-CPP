#include "TextSearch.hpp"

#include <regex>
#include <utility>

namespace ide {
namespace {

bool continuation(char unit) { return (static_cast<unsigned char>(unit) & 0xC0u) == 0x80u; }

// Letters, digits, and _ in ASCII, and every byte of a non-ASCII code point.
bool word_unit(char unit) {
    const unsigned char c = static_cast<unsigned char>(unit);
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}

// [begin, end) of line neither starts inside a word nor stops inside one. A match
// whose own first or last character is a separator counts as bounded on that side.
bool word_bounded(std::string_view line, std::size_t begin, std::size_t end) {
    const bool left = begin == 0 || !word_unit(line[begin - 1]) || (begin < line.size() && !word_unit(line[begin]));
    const bool right = end >= line.size() || !word_unit(line[end]) || (end > begin && !word_unit(line[end - 1]));
    return left && right;
}

std::string lower_ascii(std::string_view text) {
    std::string out(text);
    for (char& unit : out) {
        if (unit >= 'A' && unit <= 'Z') {
            unit = static_cast<char>(unit - 'A' + 'a');
        }
    }
    return out;
}

// The byte after the code point at index.
std::size_t next_unit(std::string_view text, std::size_t index) {
    std::size_t next = index + 1;
    while (next < text.size() && continuation(text[next])) {
        ++next;
    }
    return next;
}

// \n, \t, and \\ in a regex replacement, as VS Code reads them. $ forms are left to std::regex.
std::string unescape(std::string_view replacement) {
    std::string out;
    out.reserve(replacement.size());
    for (std::size_t i = 0; i < replacement.size(); ++i) {
        const char unit = replacement[i];
        if (unit != '\\' || i + 1 >= replacement.size()) {
            out.push_back(unit);
            continue;
        }
        const char next = replacement[i + 1];
        if (next == 'n') {
            out.push_back('\n');
        } else if (next == 't') {
            out.push_back('\t');
        } else if (next == '\\') {
            out.push_back('\\');
        } else {
            out.push_back(unit);
            continue;
        }
        ++i;
    }
    return out;
}

// Flags for a regex search that starts at byte at of line. match_prev_avail would
// let \b see the byte before, but libc++ then matches ^ there too. So a search
// that starts inside the line is not at its start, and is not at the start of
// a word when the byte before is part of one. Only a word that ends right where
// the search starts is lost to \b.
std::regex_constants::match_flag_type resume_flags(std::string_view line, std::size_t at) {
    auto flags = std::regex_constants::match_default;
    if (at > 0) {
        flags |= std::regex_constants::match_not_bol;
        if (word_unit(line[at - 1])) {
            flags |= std::regex_constants::match_not_bow;
        }
    }
    return flags;
}

}  // namespace

struct TextSearch::Compiled {
    std::regex pattern;
};

int code_points(std::string_view text) {
    int count = 0;
    for (const char unit : text) {
        if (!continuation(unit)) {
            ++count;
        }
    }
    return count;
}

TextSearch::TextSearch(SearchQuery query) : query_(std::move(query)) {
    if (query_.pattern.empty()) {
        return;
    }
    if (!query_.regex) {
        needle_ = query_.match_case ? query_.pattern : lower_ascii(query_.pattern);
        return;
    }
    auto flags = std::regex::ECMAScript;
    if (!query_.match_case) {
        flags |= std::regex::icase;
    }
    try {
        regex_ = std::make_unique<Compiled>(Compiled{std::regex(query_.pattern, flags)});
    } catch (const std::regex_error& error) {
        error_ = error.what();
        if (error_.empty()) {
            error_ = "Invalid regular expression";
        }
    }
}

TextSearch::~TextSearch() = default;
TextSearch::TextSearch(TextSearch&&) noexcept = default;
TextSearch& TextSearch::operator=(TextSearch&&) noexcept = default;

bool TextSearch::ready() const {
    if (query_.pattern.empty()) {
        return false;
    }
    return query_.regex ? regex_ != nullptr : !needle_.empty();
}

std::vector<TextMatch> TextSearch::find_all(std::string_view text, std::size_t limit) const {
    std::vector<TextMatch> out;
    if (!ready() || limit == 0) {
        return out;
    }
    std::size_t line_byte = 0;
    int line = 0;
    int line_start = 0;
    for (;;) {
        std::size_t line_end = text.find('\n', line_byte);
        if (line_end == std::string_view::npos) {
            line_end = text.size();
        }
        const std::string_view row = text.substr(line_byte, line_end - line_byte);
        // Byte offsets in row become code points by counting forward from the last one.
        std::size_t counted_byte = 0;
        int counted = 0;
        auto column_of = [&](std::size_t byte) {
            counted += code_points(row.substr(counted_byte, byte - counted_byte));
            counted_byte = byte;
            return counted;
        };
        auto emit = [&](std::size_t begin, std::size_t end) {
            TextMatch match;
            match.line = line;
            match.column = column_of(begin);
            match.start = line_start + match.column;
            match.end = line_start + column_of(end);
            match.byte_start = line_byte + begin;
            match.byte_end = line_byte + end;
            match.line_byte = line_byte;
            out.push_back(match);
            return out.size() < limit;
        };
        bool more = true;
        if (!query_.regex) {
            const std::string folded = query_.match_case ? std::string() : lower_ascii(row);
            const std::string_view hay = query_.match_case ? row : std::string_view(folded);
            std::size_t at = 0;
            while (more && (at = hay.find(needle_, at)) != std::string_view::npos) {
                const std::size_t end = at + needle_.size();
                if (query_.whole_word && !word_bounded(row, at, end)) {
                    at = next_unit(row, at);
                    continue;
                }
                more = emit(at, end);
                at = end;
            }
        } else {
            const char* const first = row.data();
            const char* const last = row.data() + row.size();
            std::size_t at = 0;
            while (more && at <= row.size()) {
                std::cmatch found;
                bool hit = false;
                try {
                    hit = std::regex_search(first + at, last, found, regex_->pattern, resume_flags(row, at));
                } catch (const std::regex_error&) {
                    // Too complex for this line. Its other matches are skipped.
                    hit = false;
                }
                if (!hit) {
                    break;
                }
                const std::size_t begin = static_cast<std::size_t>(found[0].first - first);
                const std::size_t end = static_cast<std::size_t>(found[0].second - first);
                if (query_.whole_word && !word_bounded(row, begin, end)) {
                    if (begin >= row.size()) {
                        break;
                    }
                    at = next_unit(row, begin);
                    continue;
                }
                more = emit(begin, end);
                if (end > begin) {
                    at = end;
                } else if (begin >= row.size()) {
                    break;
                } else {
                    at = next_unit(row, begin);
                }
            }
        }
        if (!more || line_end >= text.size()) {
            break;
        }
        line_start += code_points(row) + 1;
        line_byte = line_end + 1;
        ++line;
    }
    return out;
}

std::string TextSearch::expand(std::string_view text, const TextMatch& match, std::string_view replacement) const {
    if (!query_.regex || !regex_) {
        return std::string(replacement);
    }
    std::size_t line_end = text.find('\n', match.byte_start);
    if (line_end == std::string_view::npos) {
        line_end = text.size();
    }
    const std::string_view row = text.substr(match.line_byte, line_end - match.line_byte);
    const std::size_t at = match.byte_start - match.line_byte;
    const auto flags = resume_flags(row, at) | std::regex_constants::match_continuous;
    std::cmatch found;
    try {
        if (std::regex_search(row.data() + at, row.data() + row.size(), found, regex_->pattern, flags)) {
            return found.format(unescape(replacement));
        }
    } catch (const std::regex_error&) {
    }
    return unescape(replacement);
}

std::string TextSearch::replace_span(std::string_view text, const std::vector<TextMatch>& matches,
                                     std::string_view replacement) const {
    std::string out;
    if (matches.empty()) {
        return out;
    }
    std::size_t last = matches.front().byte_start;
    for (const TextMatch& match : matches) {
        out.append(text.substr(last, match.byte_start - last));
        out += expand(text, match, replacement);
        last = match.byte_end;
    }
    return out;
}

std::string TextSearch::replace_all(std::string_view text, std::string_view replacement, int line, int* count) const {
    std::vector<TextMatch> matches = find_all(text);
    if (line >= 0) {
        std::vector<TextMatch> kept;
        for (const TextMatch& match : matches) {
            if (match.line == line) {
                kept.push_back(match);
            }
        }
        matches.swap(kept);
    }
    if (count != nullptr) {
        *count = static_cast<int>(matches.size());
    }
    if (matches.empty()) {
        return std::string(text);
    }
    std::string out(text.substr(0, matches.front().byte_start));
    out += replace_span(text, matches, replacement);
    out.append(text.substr(matches.back().byte_end));
    return out;
}

}  // namespace ide
