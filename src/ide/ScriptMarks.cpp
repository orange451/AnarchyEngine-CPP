#include "ScriptMarks.hpp"

#include "Utf8.hpp"

#include <cstddef>

namespace ide {
namespace {

// Code-point offset of a Luau position. Columns are bytes on that line.
// A newline is one position. CR LF is one newline, matching the styled document.
int document_offset(std::string_view source, std::uint32_t line, std::uint32_t byte_column) {
    std::size_t index = 0;
    std::uint32_t current = 0;
    int offset = 0;
    while (index < source.size() && current < line) {
        if (source[index] == '\r') {
            ++offset;
            ++index;
            if (index < source.size() && source[index] == '\n') {
                ++index;
            }
            ++current;
            continue;
        }
        if (source[index] == '\n') {
            ++offset;
            ++index;
            ++current;
            continue;
        }
        index += Utf8Step(source, index);
        ++offset;
    }
    std::uint32_t bytes = 0;
    while (index < source.size() && bytes < byte_column && source[index] != '\n' && source[index] != '\r') {
        const std::size_t span = Utf8Step(source, index);
        if (bytes + static_cast<std::uint32_t>(span) > byte_column) {
            break;
        }
        index += span;
        bytes += static_cast<std::uint32_t>(span);
        ++offset;
    }
    return offset;
}

std::string one_line(std::string message) {
    for (char& unit : message) {
        if (unit == '\n' || unit == '\r' || unit == '\t') {
            unit = ' ';
        }
    }
    if (message.size() > 180) {
        message.resize(177);
        message += "...";
    }
    return message;
}

int problem_rank(const engine_core::Diagnostic& diagnostic) {
    if (diagnostic.code == "Syntax" && diagnostic.severity == engine_core::Severity::Error) {
        return 0;
    }
    switch (diagnostic.severity) {
    case engine_core::Severity::Error:
        return 1;
    case engine_core::Severity::Warning:
        return 2;
    case engine_core::Severity::Information:
        return 3;
    case engine_core::Severity::Hint:
        return 4;
    }
    return 4;
}

const char* severity_word(engine_core::Severity severity) {
    switch (severity) {
    case engine_core::Severity::Error:
        return "Error";
    case engine_core::Severity::Warning:
        return "Warning";
    case engine_core::Severity::Information:
        return "Information";
    case engine_core::Severity::Hint:
        return "Hint";
    }
    return "Error";
}

bool earlier(const engine_core::Diagnostic& left, const engine_core::Diagnostic& right) {
    if (left.range.start.line != right.range.start.line) {
        return left.range.start.line < right.range.start.line;
    }
    return left.range.start.character < right.range.start.character;
}

}  // namespace

std::vector<ScriptMark> marks_for(std::string_view source, const std::vector<engine_core::Diagnostic>& diagnostics) {
    std::vector<ScriptMark> marks;
    marks.reserve(diagnostics.size());
    for (const engine_core::Diagnostic& diagnostic : diagnostics) {
        if (diagnostic.code == "Analysis") {
            continue;
        }
        ScriptMark mark;
        mark.start = document_offset(source, diagnostic.range.start.line, diagnostic.range.start.character);
        mark.end = document_offset(source, diagnostic.range.end.line, diagnostic.range.end.character);
        if (mark.end < mark.start) {
            mark.end = mark.start;
        }
        mark.severity = diagnostic.severity;
        mark.code = diagnostic.code;
        mark.message = one_line(diagnostic.message);
        mark.line = diagnostic.range.start.line;
        marks.push_back(std::move(mark));
    }
    return marks;
}

ScriptProblemSummary summarize_problems(const std::vector<engine_core::Diagnostic>& diagnostics) {
    ScriptProblemSummary summary;
    const engine_core::Diagnostic* best = nullptr;
    int best_rank = 100;
    int counted = 0;
    for (const engine_core::Diagnostic& diagnostic : diagnostics) {
        if (diagnostic.code == "Analysis") {
            continue;
        }
        ++counted;
        if (diagnostic.code == "Syntax" && diagnostic.severity == engine_core::Severity::Error) {
            summary.blocks_compile = true;
        }
        const int rank = problem_rank(diagnostic);
        if (best == nullptr || rank < best_rank || (rank == best_rank && earlier(diagnostic, *best))) {
            best = &diagnostic;
            best_rank = rank;
        }
    }
    if (best == nullptr || best_rank >= 3) {
        return summary;
    }
    summary.severity = best->severity;
    const std::string message = one_line(best->message);
    const std::string where = "(line " + std::to_string(best->range.start.line + 1) + ")";
    if (summary.blocks_compile) {
        summary.text = "Will not compile — " + message + " " + where;
    } else {
        summary.text = std::string(severity_word(best->severity)) + " — " + message + " " + where;
    }
    if (counted > 1) {
        summary.text += " · " + std::to_string(counted - 1) + " more";
    }
    return summary;
}

std::string problem_detail(const ScriptMark& mark) {
    const std::string line = "line " + std::to_string(mark.line + 1);
    if (mark.code == "Syntax") {
        return line + " · Will not compile";
    }
    if (mark.code.empty()) {
        return line;
    }
    return line + " · " + mark.code;
}

bool mark_covers(const ScriptMark& mark, int index) {
    if (index < 0) {
        return false;
    }
    if (mark.end > mark.start) {
        return index >= mark.start && index < mark.end;
    }
    return index == mark.start || index + 1 == mark.start;
}

}  // namespace ide
