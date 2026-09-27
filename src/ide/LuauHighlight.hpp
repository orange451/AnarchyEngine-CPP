#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ide {

// One run of code points. style is null for plain text, or keyword, builtin,
// datatype, comment, string, or number. A datatype is a global such as Vector3 or Enum. Lengths are Unicode code points, matching CodeArea.
struct LuauSpan {
    const char* style = nullptr;
    int length = 0;
};

// Luau tokens plus the names this engine exposes (script, game, task, and the rest).
std::vector<LuauSpan> highlight_luau(std::string_view source);

}  // namespace ide
