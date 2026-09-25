#pragma once

#include "ScriptAnalysis.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ide {

// One diagnostic placed in editor code points. A newline counts as one position,
// matching a JadeFX document built from the same source.
struct ScriptMark {
    int start = 0;
    int end = 0;
    engine_core::Severity severity = engine_core::Severity::Error;
    std::string code;
    std::string message;
    std::uint32_t line = 0;

    bool operator==(const ScriptMark& other) const {
        return start == other.start && end == other.end && severity == other.severity && code == other.code &&
               message == other.message && line == other.line;
    }
};

// The line under the script. Empty text means no banner.
// blocks_compile is true only for a Syntax error. Type errors still run.
struct ScriptProblemSummary {
    bool blocks_compile = false;
    engine_core::Severity severity = engine_core::Severity::Hint;
    std::string text;
};

std::vector<ScriptMark> marks_for(std::string_view source, const std::vector<engine_core::Diagnostic>& diagnostics);

ScriptProblemSummary summarize_problems(const std::vector<engine_core::Diagnostic>& diagnostics);

// Hover detail under the message. Syntax says the script will not compile.
std::string problem_detail(const ScriptMark& mark);

// True when a character or caret index sits on this mark.
bool mark_covers(const ScriptMark& mark, int index);

}  // namespace ide
