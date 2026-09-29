#include "LuauTypedCompletion.hpp"

#include "ScriptAnalysis.hpp"

#include <algorithm>
#include <string>

namespace ide {
namespace {

// A long table or intersection type reads badly on one row.
constexpr std::size_t kMaxDetail = 48;
// A hover has room for more than a row.
constexpr std::size_t kMaxHoverType = 120;

bool StartsWithText(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::string Shortened(std::string text, std::size_t most = kMaxDetail) {
    if (text.size() > most) {
        text.resize(most - 3);
        text += "...";
    }
    return text;
}

// The byte offset of each code point, and one past the last.
std::vector<std::size_t> CodePointBytes(std::string_view source) {
    std::vector<std::size_t> bytes;
    std::size_t at = 0;
    while (at < source.size()) {
        bytes.push_back(at);
        ++at;
        while (at < source.size() && (static_cast<unsigned char>(source[at]) & 0xC0) == 0x80) {
            ++at;
        }
    }
    bytes.push_back(source.size());
    return bytes;
}

std::size_t ByteOffset(std::string_view source, int caret) {
    const std::vector<std::size_t> bytes = CodePointBytes(source);
    if (caret <= 0) {
        return 0;
    }
    return bytes[std::min(static_cast<std::size_t>(caret), bytes.size() - 1)];
}

bool NameUnit(char unit) {
    return (unit >= 'a' && unit <= 'z') || (unit >= 'A' && unit <= 'Z') || (unit >= '0' && unit <= '9') ||
           unit == '_';
}

// Types that say nothing a hover could use.
bool Uninformative(const std::string& type) {
    return type.empty() || type == "unknown" || type == "any" || type == "*error-type*" || type == "nil" ||
           type == "never";
}

// The byte offset of the callee's last character, just before its '('.
std::optional<std::size_t> CalleeOffset(const CompletionList& list, std::string_view source) {
    const std::vector<std::size_t> bytes = CodePointBytes(source);
    if (list.call_open <= 0 || static_cast<std::size_t>(list.call_open) >= bytes.size()) {
        return std::nullopt;
    }
    const std::size_t callee = bytes[static_cast<std::size_t>(list.call_open) - 1];
    if (!NameUnit(source[callee]) && source[callee] != ')' && source[callee] != ']') {
        return std::nullopt;
    }
    return callee;
}

}  // namespace

bool wants_luau_members(const CompletionList& list) {
    return list.site == CompleteSite::Member && !list.receiver_known && list.items.empty();
}

bool wants_luau_signature(const CompletionList& list) { return list.signature.empty() && list.call_open > 0; }

bool apply_luau_members(CompletionList& list, const engine_core::LuauCompletion& luau) {
    if (!luau.ran || !wants_luau_members(list)) {
        return false;
    }
    for (const engine_core::LuauSuggestion& suggestion : luau.items) {
        // A method after '.' or a field after ':', a metamethod, a name the
        // typed prefix does not start, or anything that is not a member.
        if (suggestion.wrong_index || suggestion.kind != "property" || StartsWithText(suggestion.name, "__") ||
            !StartsWithText(suggestion.name, list.prefix)) {
            continue;
        }
        CompletionItem item;
        item.name = suggestion.name;
        if (suggestion.function) {
            item.detail = "function";
            item.call = true;
            item.title = "function " + suggestion.name + suggestion.params +
                         (suggestion.returns.empty() ? std::string() : ": " + suggestion.returns);
            item.returns = suggestion.returns.empty() ? "returns nothing" : suggestion.returns;
        } else {
            item.detail = Shortened(suggestion.type);
            item.title = suggestion.name + ": " + item.detail;
        }
        list.items.push_back(std::move(item));
    }
    return !list.items.empty();
}

bool apply_luau_signature(CompletionList& list, const engine_core::LuauTypeAt& luau) {
    if (!wants_luau_signature(list) || !luau.found || !luau.described.function) {
        return false;
    }
    std::vector<SignatureParam> params;
    for (const auto& [name, type] : luau.described.param_list) {
        params.push_back(SignatureParam{name, Shortened(type)});
    }
    if (params.empty() && !luau.described.variadic) {
        return false;
    }
    set_signature(list, params, luau.described.variadic, list.call_argument);
    return true;
}

std::optional<HoverWord> hover_word(std::string_view source, int index) {
    const std::vector<std::size_t> bytes = CodePointBytes(source);
    const int count = static_cast<int>(bytes.size()) - 1;
    if (index < 0 || index >= count || !NameUnit(source[bytes[static_cast<std::size_t>(index)]])) {
        return std::nullopt;
    }
    HoverWord out;
    out.begin = index;
    while (out.begin > 0 && NameUnit(source[bytes[static_cast<std::size_t>(out.begin - 1)]])) {
        --out.begin;
    }
    out.end = index;
    while (out.end < count && NameUnit(source[bytes[static_cast<std::size_t>(out.end)]])) {
        ++out.end;
    }
    const std::size_t first = bytes[static_cast<std::size_t>(out.begin)];
    out.word = std::string(source.substr(first, bytes[static_cast<std::size_t>(out.end)] - first));
    if (out.word.empty() || (out.word[0] >= '0' && out.word[0] <= '9')) {
        return std::nullopt;
    }
    out.offset = bytes[static_cast<std::size_t>(index)];
    return out;
}

bool wants_luau_hover(const HoverInfo& info, const HoverWord& word) {
    return !info.found ||
           (info.summary.empty() && (info.title == word.word || info.title == word.word + ": function"));
}

bool apply_luau_hover(HoverInfo& info, const HoverWord& word, const engine_core::LuauTypeAt& luau) {
    if (!wants_luau_hover(info, word) || !luau.found || luau.name != word.word ||
        Uninformative(luau.described.type)) {
        return false;
    }
    const bool parameter = info.found && info.detail == "parameter";
    info.found = true;
    info.begin = word.begin;
    info.end = word.end;
    info.summary.clear();
    if (luau.described.function) {
        info.title = "function " + word.word + luau.described.params +
                     (luau.described.returns.empty() ? std::string() : ": " + luau.described.returns);
        info.detail = luau.described.returns.empty() ? "returns nothing" : "";
    } else {
        info.title = word.word + ": " + Shortened(luau.described.type, kMaxHoverType);
        info.detail = parameter ? "parameter" : "";
    }
    return true;
}

bool complete_from_luau(CompletionList& list, engine_core::ScriptAnalysis& analysis, std::string_view source,
                        int caret, const std::vector<engine_core::LuaNode>& world, std::uint32_t script_id,
                        std::chrono::milliseconds wait) {
    if (!wants_luau_members(list)) {
        return false;
    }
    const engine_core::LuauCompletion luau =
        analysis.luau_complete(world, script_id, std::string(source), ByteOffset(source, caret), wait);
    return apply_luau_members(list, luau);
}

bool signature_from_luau(CompletionList& list, engine_core::ScriptAnalysis& analysis, std::string_view source,
                         const std::vector<engine_core::LuaNode>& world, std::uint32_t script_id,
                         std::chrono::milliseconds wait) {
    if (!wants_luau_signature(list)) {
        return false;
    }
    const std::optional<std::size_t> callee = CalleeOffset(list, source);
    if (!callee) {
        return false;
    }
    const engine_core::LuauTypeAt luau = analysis.luau_type_at(world, script_id, std::string(source), *callee, wait);
    return apply_luau_signature(list, luau);
}

bool hover_from_luau(HoverInfo& info, engine_core::ScriptAnalysis& analysis, std::string_view source, int index,
                     const std::vector<engine_core::LuaNode>& world, std::uint32_t script_id,
                     std::chrono::milliseconds wait) {
    const std::optional<HoverWord> word = hover_word(source, index);
    if (!word || !wants_luau_hover(info, *word)) {
        return false;
    }
    const engine_core::LuauTypeAt luau =
        analysis.luau_type_at(world, script_id, std::string(source), word->offset, wait);
    return apply_luau_hover(info, *word, luau);
}

std::optional<PendingLuauList> ask_luau_for_list(const CompletionList& list, engine_core::ScriptAnalysis& analysis,
                                                 std::string_view source, int caret,
                                                 const std::vector<engine_core::LuaNode>& world,
                                                 std::uint32_t script_id, bool force) {
    const bool members = wants_luau_members(list);
    const std::optional<std::size_t> callee = wants_luau_signature(list) ? CalleeOffset(list, source) : std::nullopt;
    if (!members && !callee) {
        return std::nullopt;
    }
    PendingLuauList pending;
    pending.source = std::string(source);
    pending.caret = caret;
    pending.force = force;
    pending.list = list;
    if (members) {
        pending.members =
            analysis.luau_complete_later(world, script_id, pending.source, ByteOffset(source, caret), "members");
    }
    if (callee) {
        pending.signature = analysis.luau_type_at_later(world, script_id, pending.source, *callee, "signature");
    }
    if (!pending.members && !pending.signature) {
        return std::nullopt;
    }
    return pending;
}

bool take_luau_answers(PendingLuauList& pending, bool& changed) {
    changed = false;
    const auto arrived = [](const std::shared_ptr<const engine_core::LuauAnswer>& answer) {
        return !answer || answer->ready.load(std::memory_order_acquire);
    };
    if (!arrived(pending.members) || !arrived(pending.signature)) {
        return false;
    }
    if (pending.members) {
        changed = apply_luau_members(pending.list, pending.members->completion) || changed;
    }
    if (pending.signature) {
        changed = apply_luau_signature(pending.list, pending.signature->type) || changed;
    }
    return true;
}

}  // namespace ide
