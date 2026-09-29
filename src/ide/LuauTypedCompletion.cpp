#include "LuauTypedCompletion.hpp"

#include "ScriptAnalysis.hpp"

#include <algorithm>
#include <string>

namespace ide {
namespace {

// A long table or intersection type reads badly on one row.
constexpr std::size_t kMaxDetail = 48;

bool StartsWithText(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

// A hover has room for more than a row.
constexpr std::size_t kMaxHoverType = 120;

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

bool NameUnit(char unit) {
    return (unit >= 'a' && unit <= 'z') || (unit >= 'A' && unit <= 'Z') || (unit >= '0' && unit <= '9') ||
           unit == '_';
}

// Types that say nothing a hover could use.
bool Uninformative(const std::string& type) {
    return type.empty() || type == "unknown" || type == "any" || type == "*error-type*" || type == "nil" ||
           type == "never";
}

}  // namespace

bool complete_from_luau(CompletionList& list, engine_core::ScriptAnalysis& analysis, std::string_view source,
                        int caret, const std::vector<engine_core::LuaNode>& world, std::uint32_t script_id,
                        std::chrono::milliseconds wait) {
    if (list.site != CompleteSite::Member || list.receiver_known || !list.items.empty()) {
        return false;
    }
    // The caret counts code points. Luau counts bytes.
    std::size_t offset = 0;
    for (int seen = 0; offset < source.size() && seen < caret; ++seen) {
        ++offset;
        while (offset < source.size() && (static_cast<unsigned char>(source[offset]) & 0xC0) == 0x80) {
            ++offset;
        }
    }
    const engine_core::LuauCompletion luau =
        analysis.luau_complete(world, script_id, std::string(source), offset, wait);
    if (!luau.ran) {
        return false;
    }
    for (const engine_core::LuauSuggestion& suggestion : luau.items) {
        // A method after '.' or a field after ':', a metamethod, or a name the
        // typed prefix does not start.
        if (suggestion.wrong_index || StartsWithText(suggestion.name, "__") ||
            !StartsWithText(suggestion.name, list.prefix)) {
            continue;
        }
        if (suggestion.kind != "property") {
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

bool signature_from_luau(CompletionList& list, engine_core::ScriptAnalysis& analysis, std::string_view source,
                         const std::vector<engine_core::LuaNode>& world, std::uint32_t script_id,
                         std::chrono::milliseconds wait) {
    if (!list.signature.empty() || list.call_open <= 0) {
        return false;
    }
    const std::vector<std::size_t> bytes = CodePointBytes(source);
    if (static_cast<std::size_t>(list.call_open) >= bytes.size()) {
        return false;
    }
    // The last character of the callee, just before its '('.
    const std::size_t callee = bytes[static_cast<std::size_t>(list.call_open) - 1];
    if (!NameUnit(source[callee]) && source[callee] != ')' && source[callee] != ']') {
        return false;
    }
    const engine_core::LuauTypeAt luau = analysis.luau_type_at(world, script_id, std::string(source), callee, wait);
    if (!luau.found || !luau.described.function) {
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

bool hover_from_luau(HoverInfo& info, engine_core::ScriptAnalysis& analysis, std::string_view source, int index,
                     const std::vector<engine_core::LuaNode>& world, std::uint32_t script_id,
                     std::chrono::milliseconds wait) {
    const std::vector<std::size_t> bytes = CodePointBytes(source);
    const int count = static_cast<int>(bytes.size()) - 1;
    if (index < 0 || index >= count || !NameUnit(source[bytes[static_cast<std::size_t>(index)]])) {
        return false;
    }
    int begin = index;
    while (begin > 0 && NameUnit(source[bytes[static_cast<std::size_t>(begin - 1)]])) {
        --begin;
    }
    int end = index;
    while (end < count && NameUnit(source[bytes[static_cast<std::size_t>(end)]])) {
        ++end;
    }
    const std::string word(source.substr(bytes[static_cast<std::size_t>(begin)],
                                         bytes[static_cast<std::size_t>(end)] - bytes[static_cast<std::size_t>(begin)]));
    if (word.empty() || (word[0] >= '0' && word[0] <= '9')) {
        return false;
    }
    // Only a hover that says no more than the name, or that it is a function.
    const bool bare = !info.found || (info.summary.empty() && (info.title == word || info.title == word + ": function"));
    if (!bare) {
        return false;
    }
    const engine_core::LuauTypeAt luau =
        analysis.luau_type_at(world, script_id, std::string(source), bytes[static_cast<std::size_t>(index)], wait);
    if (!luau.found || luau.name != word || Uninformative(luau.described.type)) {
        return false;
    }
    const bool parameter = info.found && info.detail == "parameter";
    info.found = true;
    info.begin = begin;
    info.end = end;
    info.summary.clear();
    if (luau.described.function) {
        info.title = "function " + word + luau.described.params +
                     (luau.described.returns.empty() ? std::string() : ": " + luau.described.returns);
        info.detail = luau.described.returns.empty() ? "returns nothing" : "";
    } else {
        info.title = word + ": " + Shortened(luau.described.type, kMaxHoverType);
        info.detail = parameter ? "parameter" : "";
    }
    return true;
}

}  // namespace ide
