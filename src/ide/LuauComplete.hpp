#pragma once

#include "LuaApi.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ide {

// Where the caret is asking for a completion.
enum class CompleteSite {
    None,
    Member,    // After '.' or ':'.
    Name,      // A local, global, or keyword.
    Type,      // A type name after ':' or '::'.
    Argument,  // A string argument of GetService or FindFirstChild.
};

struct CompletionItem {
    std::string name;
    std::string detail;
    // True when accepting the name can insert a call's parentheses.
    bool call = false;
};

struct CompletionList {
    CompleteSite site = CompleteSite::None;
    // Code-point range the accepted name replaces. Same unit as CodeArea.
    int replace_begin = 0;
    int replace_end = 0;
    std::string prefix;
    std::vector<CompletionItem> items;
    // Wrapping quote of an argument completion. 0 for every other site.
    char close_quote = 0;
    // The closing quote is not in the buffer yet. Accepting can type it.
    bool unclosed = false;
};

// `world` is the live instance tree. `script_id` is the script being edited.
// Members come from the class registry and from the libraries the play VM loads.
// require of a ModuleScript runs that source and completes whatever it returns.
// GetService("...") completes registered services. FindFirstChild("...") completes
// the receiver's children. `script_global` is false on the command line, where `script` is nil.
CompletionList complete_luau(std::string_view source, int caret, const std::vector<engine_core::LuaNode>& world = {},
                             std::uint32_t script_id = 0, bool script_global = true);

}  // namespace ide
