#include "ide/TextWrap.hpp"

namespace ide {
namespace {

// Bytes in the UTF-8 sequence that starts with this byte.
std::size_t SequenceLength(unsigned char lead) {
    if (lead >= 0xf0) {
        return 4;
    }
    if (lead >= 0xe0) {
        return 3;
    }
    if (lead >= 0xc0) {
        return 2;
    }
    return 1;
}

// Moves the widest prefix of `word` that fits onto `out`, one code point at
// least, so a line always advances.
void BreakWord(std::string& word, double max_width, const MeasureText& measure, std::vector<std::string>& out) {
    while (!word.empty() && measure(word) > max_width) {
        std::size_t fit = 0;
        std::size_t next = SequenceLength(static_cast<unsigned char>(word[0]));
        while (next < word.size() && measure(word.substr(0, next)) <= max_width) {
            fit = next;
            next += SequenceLength(static_cast<unsigned char>(word[next]));
        }
        if (fit == 0) {
            fit = SequenceLength(static_cast<unsigned char>(word[0]));
        }
        if (fit >= word.size()) {
            break;
        }
        out.push_back(word.substr(0, fit));
        word.erase(0, fit);
    }
}

void WrapParagraph(std::string_view paragraph, double max_width, const MeasureText& measure, std::vector<std::string>& out) {
    std::string line;
    std::size_t at = 0;
    while (at < paragraph.size()) {
        if (paragraph[at] == ' ') {
            ++at;
            continue;
        }
        std::size_t end = paragraph.find(' ', at);
        if (end == std::string_view::npos) {
            end = paragraph.size();
        }
        std::string word(paragraph.substr(at, end - at));
        at = end;
        if (!line.empty()) {
            std::string joined = line + " " + word;
            if (measure(joined) <= max_width) {
                line = std::move(joined);
                continue;
            }
            out.push_back(std::move(line));
            line.clear();
        }
        BreakWord(word, max_width, measure, out);
        line = std::move(word);
    }
    out.push_back(std::move(line));
}

}  // namespace

std::vector<std::string> WrapText(std::string_view text, double max_width, const MeasureText& measure) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t newline = text.find('\n', start);
        WrapParagraph(text.substr(start, newline == std::string_view::npos ? std::string_view::npos : newline - start),
                      max_width, measure, out);
        if (newline == std::string_view::npos) {
            break;
        }
        start = newline + 1;
    }
    return out;
}

}  // namespace ide
