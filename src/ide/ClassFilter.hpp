#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace ide {

inline char AsciiLower(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return static_cast<char>(c);
}

inline std::string_view TrimAscii(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

inline bool EqualsAt(std::string_view text, std::string_view query, std::size_t offset) {
    if (offset + query.size() > text.size()) {
        return false;
    }
    for (std::size_t i = 0; i < query.size(); ++i) {
        const char left = AsciiLower(static_cast<unsigned char>(text[offset + i]));
        const char right = AsciiLower(static_cast<unsigned char>(query[i]));
        if (left != right) {
            return false;
        }
    }
    return true;
}

// Empty query keeps every name. A name that starts with the query is listed
// before one that only contains it. Each group is A to Z, ignoring case.
inline void filter_class_names(const std::vector<std::string>& names, std::string_view query,
                               std::vector<std::string>& out) {
    query = TrimAscii(query);
    std::vector<std::string> prefix;
    std::vector<std::string> rest;
    for (const std::string& name : names) {
        if (query.empty() || EqualsAt(name, query, 0)) {
            prefix.push_back(name);
            continue;
        }
        bool found = false;
        for (std::size_t i = 1; i < name.size() && !found; ++i) {
            found = EqualsAt(name, query, i);
        }
        if (found) {
            rest.push_back(name);
        }
    }
    const auto by_name = [](const std::string& left, const std::string& right) {
        const std::size_t count = std::min(left.size(), right.size());
        for (std::size_t i = 0; i < count; ++i) {
            const char a = AsciiLower(static_cast<unsigned char>(left[i]));
            const char b = AsciiLower(static_cast<unsigned char>(right[i]));
            if (a != b) {
                return a < b;
            }
        }
        return left.size() < right.size();
    };
    std::sort(prefix.begin(), prefix.end(), by_name);
    std::sort(rest.begin(), rest.end(), by_name);
    out.clear();
    out.reserve(prefix.size() + rest.size());
    out.insert(out.end(), prefix.begin(), prefix.end());
    out.insert(out.end(), rest.begin(), rest.end());
}

}  // namespace ide
