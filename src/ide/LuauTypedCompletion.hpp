#pragma once

// Completion from Luau's own type checker, for what the completion resolver
// cannot follow: metatable classes, loop variables, generics, and values that
// flow through calls and assignments.

#include "LuauComplete.hpp"

#include <chrono>
#include <cstdint>
#include <string_view>
#include <vector>

namespace engine_core {
class ScriptAnalysis;
}

namespace ide {

// Fills `list` from Luau's autocomplete when it is a member list the resolver
// left empty because it did not know the receiver. Every other list is left
// as it is, so nothing the resolver offers is lost. Asks the analysis worker
// and waits up to `wait`. Returns true when it added rows.
bool complete_from_luau(CompletionList& list, engine_core::ScriptAnalysis& analysis, std::string_view source,
                        int caret, const std::vector<engine_core::LuaNode>& world, std::uint32_t script_id,
                        std::chrono::milliseconds wait);

// Writes the signature of the call the caret is inside from Luau's type of the
// callee, when the resolver wrote none. Returns true when it wrote one.
bool signature_from_luau(CompletionList& list, engine_core::ScriptAnalysis& analysis, std::string_view source,
                         const std::vector<engine_core::LuaNode>& world, std::uint32_t script_id,
                         std::chrono::milliseconds wait);

// Replaces a hover the resolver could not type, one that shows no more than
// the bare name, with Luau's type for it. A hover with a type or with docs is
// left as it is. Returns true when it replaced the hover.
bool hover_from_luau(HoverInfo& info, engine_core::ScriptAnalysis& analysis, std::string_view source, int index,
                     const std::vector<engine_core::LuaNode>& world, std::uint32_t script_id,
                     std::chrono::milliseconds wait);

}  // namespace ide
