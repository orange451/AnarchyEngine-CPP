#pragma once

// Completion, call signatures, and hover from Luau's own type checker, for what
// the completion resolver cannot follow: metatable classes, loop variables,
// generics, and values that flow through calls and assignments. The resolver
// always answers first. Luau only fills what it left empty.

#include "LuauComplete.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine_core {
class ScriptAnalysis;
struct LuauAnswer;
struct LuauCompletion;
struct LuauTypeAt;
}  // namespace engine_core

namespace ide {

// A member list the resolver left empty because it did not know the receiver.
bool wants_luau_members(const CompletionList& list);
// A call the caret is inside that the resolver wrote no signature for.
bool wants_luau_signature(const CompletionList& list);

// Adds Luau's members to a list wants_luau_members accepted. True when it added rows.
bool apply_luau_members(CompletionList& list, const engine_core::LuauCompletion& luau);
// Writes the signature from Luau's type of the callee. True when it wrote one.
bool apply_luau_signature(CompletionList& list, const engine_core::LuauTypeAt& luau);

// The name the pointer rests on, as a hover asks Luau about it.
struct HoverWord {
    int begin = 0;
    int end = 0;
    std::string word;
    std::size_t offset = 0;
};
std::optional<HoverWord> hover_word(std::string_view source, int index);
// A hover that says no more than the name, or that it is a function.
bool wants_luau_hover(const HoverInfo& info, const HoverWord& word);
// Replaces such a hover with Luau's type. True when it did.
bool apply_luau_hover(HoverInfo& info, const HoverWord& word, const engine_core::LuauTypeAt& luau);

// Asks and waits up to `wait`, for callers that can block, such as tests.
bool complete_from_luau(CompletionList& list, engine_core::ScriptAnalysis& analysis, std::string_view source,
                        int caret, const std::vector<engine_core::LuaNode>& world, std::uint32_t script_id,
                        std::chrono::milliseconds wait);
bool signature_from_luau(CompletionList& list, engine_core::ScriptAnalysis& analysis, std::string_view source,
                         const std::vector<engine_core::LuaNode>& world, std::uint32_t script_id,
                         std::chrono::milliseconds wait);
bool hover_from_luau(HoverInfo& info, engine_core::ScriptAnalysis& analysis, std::string_view source, int index,
                     const std::vector<engine_core::LuaNode>& world, std::uint32_t script_id,
                     std::chrono::milliseconds wait);

// Luau's answers for one completion list, on their way. The UI shows the
// resolver's list at once and applies these on a later frame, when the text
// and caret are still what they were asked about.
struct PendingLuauList {
    std::string source;
    int caret = 0;
    bool force = false;
    // Whether the popup was open once the resolver's list was shown. The
    // answers apply only while it still is, so a popup closed since stays closed.
    bool shown = false;
    CompletionList list;
    std::shared_ptr<const engine_core::LuauAnswer> members;
    std::shared_ptr<const engine_core::LuauAnswer> signature;
};

// Asks for whatever `list` lacks. Nothing when it lacks nothing.
std::optional<PendingLuauList> ask_luau_for_list(const CompletionList& list, engine_core::ScriptAnalysis& analysis,
                                                 std::string_view source, int caret,
                                                 const std::vector<engine_core::LuaNode>& world,
                                                 std::uint32_t script_id, bool force);
// Applies the answers once all have arrived. False while any is still on its
// way. `changed` says whether the list gained rows or a signature.
bool take_luau_answers(PendingLuauList& pending, bool& changed);

}  // namespace ide
