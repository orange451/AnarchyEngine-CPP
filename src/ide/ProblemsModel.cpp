#include "ProblemsModel.hpp"

#include "Strings.hpp"
#include "Utf8.hpp"

#include <algorithm>
#include <cctype>

namespace ide {
namespace {

int rank(engine_core::Severity severity) {
    switch (severity) {
    case engine_core::Severity::Error:
        return 0;
    case engine_core::Severity::Warning:
        return 1;
    case engine_core::Severity::Information:
        return 2;
    case engine_core::Severity::Hint:
        return 3;
    }
    return 3;
}

// The bytes of 0-based line `line` in source, without its line break.
std::string_view line_of(std::string_view source, std::uint32_t line) {
    std::size_t start = 0;
    for (std::uint32_t at = 0; at < line; ++at) {
        const std::size_t stop = source.find('\n', start);
        if (stop == std::string_view::npos) {
            return {};
        }
        start = stop + 1;
    }
    std::size_t stop = source.find('\n', start);
    if (stop == std::string_view::npos) {
        stop = source.size();
    }
    if (stop > start && source[stop - 1] == '\r') {
        --stop;
    }
    return source.substr(start, stop - start);
}

// Code points before byte column `byte` on the line, clamped to the line.
int column_of(std::string_view line, std::uint32_t byte) {
    return CodePointsBefore(line, std::min<std::size_t>(byte, line.size()));
}

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& unit : out) {
        unit = static_cast<char>(std::tolower(static_cast<unsigned char>(unit)));
    }
    return out;
}

bool contains(const std::string& haystack, const std::string& lowered_needle) {
    return lower(haystack).find(lowered_needle) != std::string::npos;
}

void count(ProblemCounts& counts, engine_core::Severity severity) {
    switch (severity) {
    case engine_core::Severity::Error:
        ++counts.errors;
        break;
    case engine_core::Severity::Warning:
        ++counts.warnings;
        break;
    case engine_core::Severity::Information:
        ++counts.info;
        break;
    case engine_core::Severity::Hint:
        break;
    }
}

bool shown_by(const ProblemFilter& filter, engine_core::Severity severity) {
    switch (severity) {
    case engine_core::Severity::Error:
        return filter.errors;
    case engine_core::Severity::Warning:
        return filter.warnings;
    case engine_core::Severity::Information:
        return filter.info;
    case engine_core::Severity::Hint:
        return false;
    }
    return false;
}

}  // namespace

std::vector<Problem> problems_from(std::string_view source, const std::vector<engine_core::Diagnostic>& diagnostics) {
    std::vector<Problem> out;
    for (const engine_core::Diagnostic& diagnostic : diagnostics) {
        if (diagnostic.severity == engine_core::Severity::Hint) {
            continue;
        }
        const std::string_view line = line_of(source, diagnostic.range.start.line);
        Problem problem;
        problem.line = static_cast<int>(diagnostic.range.start.line) + 1;
        problem.column = column_of(line, diagnostic.range.start.character);
        problem.column_end = diagnostic.range.end.line == diagnostic.range.start.line
                                 ? column_of(line, diagnostic.range.end.character)
                                 : CodePoints(line);
        problem.column_end = std::max(problem.column_end, problem.column);
        problem.severity = diagnostic.severity;
        problem.code = diagnostic.code;
        problem.message = diagnostic.message;
        out.push_back(std::move(problem));
    }
    return out;
}

ProblemList build_problems(std::vector<ProblemSource> sources, const ProblemFilter& filter) {
    ProblemList list;
    const std::string needle = lower(filter.text);
    for (ProblemSource& source : sources) {
        if (source.problems.empty()) {
            continue;
        }
        ++list.total.scripts;
        const bool script_matches =
            needle.empty() || contains(source.name, needle) || contains(source.path, needle);
        ScriptProblems row;
        row.id = source.id;
        row.name = std::move(source.name);
        row.class_name = std::move(source.class_name);
        row.path = std::move(source.path);
        bool matched_any = false;
        for (Problem& problem : source.problems) {
            count(list.total, problem.severity);
            if (problem.severity == engine_core::Severity::Error) {
                row.has_error = true;
            }
            const bool matches =
                script_matches || contains(problem.message, needle) || contains(problem.code, needle);
            if (!matches) {
                continue;
            }
            matched_any = true;
            count(list.matching, problem.severity);
            if (!shown_by(filter, problem.severity)) {
                continue;
            }
            count(list.shown, problem.severity);
            if (problem.severity == engine_core::Severity::Error) {
                ++row.errors;
            } else if (problem.severity == engine_core::Severity::Warning) {
                ++row.warnings;
            }
            row.problems.push_back(std::move(problem));
        }
        if (matched_any) {
            ++list.matching.scripts;
        }
        if (row.problems.empty()) {
            continue;
        }
        std::stable_sort(row.problems.begin(), row.problems.end(), [](const Problem& a, const Problem& b) {
            if (rank(a.severity) != rank(b.severity)) {
                return rank(a.severity) < rank(b.severity);
            }
            if (a.line != b.line) {
                return a.line < b.line;
            }
            return a.column < b.column;
        });
        ++list.shown.scripts;
        list.scripts.push_back(std::move(row));
    }
    std::stable_sort(list.scripts.begin(), list.scripts.end(), [](const ScriptProblems& a, const ScriptProblems& b) {
        if (a.has_error != b.has_error) {
            return a.has_error;
        }
        if (a.path != b.path) {
            return a.path < b.path;
        }
        return a.name < b.name;
    });
    return list;
}

std::string problems_summary(const ProblemList& list) {
    if (list.total.scripts == 0) {
        return "No problems";
    }
    if (list.shown.scripts == 0) {
        return "No problems match";
    }
    std::string text;
    const auto add = [&text](int n, const char* one, const char* many) {
        if (n == 0) {
            return;
        }
        if (!text.empty()) {
            text += ", ";
        }
        text += counted(static_cast<std::size_t>(n), one, many);
    };
    add(list.shown.errors, "error", "errors");
    add(list.shown.warnings, "warning", "warnings");
    add(list.shown.info, "info", "info");
    return text + " in " + counted(static_cast<std::size_t>(list.shown.scripts), "script", "scripts");
}

std::string problems_title(const ProblemList& list) {
    const int count = list.total.errors + list.total.warnings;
    return count == 0 ? std::string("Problems") : "Problems (" + std::to_string(count) + ")";
}

}  // namespace ide
