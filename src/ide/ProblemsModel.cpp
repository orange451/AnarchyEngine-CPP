#include "ProblemsModel.hpp"

#include "Strings.hpp"
#include "Utf8.hpp"

#include <algorithm>

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

// Where each line of source starts, so a script with many problems is scanned once.
std::vector<std::size_t> line_starts(std::string_view source) {
    std::vector<std::size_t> starts{0};
    for (std::size_t at = source.find('\n'); at != std::string_view::npos; at = source.find('\n', at + 1)) {
        starts.push_back(at + 1);
    }
    return starts;
}

// The bytes of 0-based line `line` in source, without its line break.
std::string_view line_of(std::string_view source, const std::vector<std::size_t>& starts, std::uint32_t line) {
    if (line >= starts.size()) {
        return {};
    }
    const std::size_t start = starts[line];
    std::size_t stop = line + 1 < starts.size() ? starts[line + 1] - 1 : source.size();
    if (stop > start && source[stop - 1] == '\r') {
        --stop;
    }
    return source.substr(start, stop - start);
}

// Code points before byte column `byte` on the line, clamped to the line.
int column_of(std::string_view line, std::uint32_t byte) {
    return CodePointsBefore(line, std::min<std::size_t>(byte, line.size()));
}

bool contains(const std::string& haystack, const std::string& lowered_needle) {
    return AsciiLower(haystack).find(lowered_needle) != std::string::npos;
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
    std::vector<std::size_t> starts;
    for (const engine_core::Diagnostic& diagnostic : diagnostics) {
        if (diagnostic.severity == engine_core::Severity::Hint) {
            continue;
        }
        if (starts.empty()) {
            starts = line_starts(source);
        }
        const std::string_view line = line_of(source, starts, diagnostic.range.start.line);
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
    const std::string needle = AsciiLower(filter.text);
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
        // Only a shown error sorts the script first: one the toggles or the
        // filter hide would put a row of warnings above the rest for no reason.
        row.has_error = row.errors > 0;
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
