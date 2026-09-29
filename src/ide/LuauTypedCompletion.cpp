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

std::string Shortened(std::string text) {
    if (text.size() > kMaxDetail) {
        text.resize(kMaxDetail - 3);
        text += "...";
    }
    return text;
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

}  // namespace ide
