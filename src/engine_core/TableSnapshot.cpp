#include "TableSnapshot.hpp"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <cstdio>
#include <string_view>
#include <utility>

namespace engine_core {
namespace {

// A print of a large table still returns quickly. The console opens one level at a time,
// so this caps the copy, not what is on screen.
constexpr std::size_t kMaxFields = 5000;
constexpr std::size_t kMaxDepth = 16;
constexpr std::size_t kMaxStringBytes = 200;

bool is_name(std::string_view text) {
    static constexpr std::string_view kKeywords[] = {
        "and",   "break", "continue", "do",   "else", "elseif", "end",  "false", "for",   "function",
        "if",    "in",    "local",    "nil",  "not",  "or",     "repeat", "return", "then", "true",
        "until", "while",
    };
    if (text.empty()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        const bool digit = c >= '0' && c <= '9';
        if (!alpha && !(digit && i > 0)) {
            return false;
        }
    }
    return std::find(std::begin(kKeywords), std::end(kKeywords), text) == std::end(kKeywords);
}

// Cuts at a code point boundary.
std::size_t fit_utf8(std::string_view text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return text.size();
    }
    std::size_t cut = max_bytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) {
        --cut;
    }
    return cut;
}

// A Luau string literal, so a string reads apart from a number or a name.
std::string quote(std::string_view text) {
    const std::size_t keep = fit_utf8(text, kMaxStringBytes);
    std::string out = "\"";
    for (std::size_t i = 0; i < keep; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20 || c == 0x7F) {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\%u", static_cast<unsigned>(c));
                out += escaped;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
    if (keep < text.size()) {
        out += "...";
    }
    return out;
}

struct Sorted {
    int rank = 0;
    double number = 0;
    TableField field;
};

class Copier {
public:
    explicit Copier(lua_State* state) : state_(state) {}

    std::shared_ptr<const TableSnapshot> copy(int index) {
        index = lua_absindex(state_, index);
        luaL_checkstack(state_, 4, "print");
        open_.push_back(lua_topointer(state_, index));
        auto table = std::make_shared<TableSnapshot>();
        std::vector<Sorted> rows;
        lua_pushnil(state_);
        while (lua_next(state_, index) != 0) {
            if (budget_ == 0) {
                ++table->omitted;
                lua_pop(state_, 1);
                continue;
            }
            --budget_;
            Sorted row;
            describe_key(row);
            if (lua_type(state_, -1) == LUA_TSTRING) {
                std::size_t length = 0;
                const char* text = lua_tolstring(state_, -1, &length);
                row.field.value = quote(std::string_view(text, length));
            } else {
                row.field.value = display(-1);
            }
            if (lua_type(state_, -1) == LUA_TTABLE) {
                const void* pointer = lua_topointer(state_, -1);
                if (std::find(open_.begin(), open_.end(), pointer) != open_.end()) {
                    row.field.value += "  (cycle)";
                } else if (open_.size() >= kMaxDepth) {
                    row.field.value += "  (too deep)";
                } else {
                    row.field.table = copy(-1);
                }
            }
            rows.push_back(std::move(row));
            lua_pop(state_, 1);
        }
        std::stable_sort(rows.begin(), rows.end(), [](const Sorted& a, const Sorted& b) {
            if (a.rank != b.rank) {
                return a.rank < b.rank;
            }
            if (a.rank == 0) {
                return a.number < b.number;
            }
            return a.field.key < b.field.key;
        });
        table->fields.reserve(rows.size());
        for (Sorted& row : rows) {
            table->fields.push_back(std::move(row.field));
        }
        open_.pop_back();
        return table;
    }

private:
    // tostring of the value at index, leaving the stack as it was.
    std::string display(int index) {
        std::size_t length = 0;
        const char* text = luaL_tolstring(state_, index, &length);
        std::string out(text != nullptr ? text : "", text != nullptr ? length : 0);
        lua_pop(state_, 1);
        return out;
    }

    // The key sits under the value. lua_next needs it unchanged, so only copies are converted.
    void describe_key(Sorted& row) {
        switch (lua_type(state_, -2)) {
        case LUA_TNUMBER:
            row.rank = 0;
            row.number = lua_tonumber(state_, -2);
            row.field.key = "[" + display(-2) + "]";
            break;
        case LUA_TSTRING: {
            row.rank = 1;
            std::size_t length = 0;
            const char* text = lua_tolstring(state_, -2, &length);
            const std::string_view name(text, length);
            row.field.key = is_name(name) ? std::string(name) : "[" + quote(name) + "]";
            break;
        }
        case LUA_TBOOLEAN:
            row.rank = 2;
            row.field.key = "[" + display(-2) + "]";
            break;
        default:
            row.rank = 3;
            row.field.key = "[" + display(-2) + "]";
            break;
        }
    }

    lua_State* state_;
    std::size_t budget_ = kMaxFields;
    std::vector<const void*> open_;
};

}  // namespace

std::shared_ptr<const TableSnapshot> snapshot_table(lua_State* state, int index) {
    Copier copier(state);
    return copier.copy(index);
}

}  // namespace engine_core
