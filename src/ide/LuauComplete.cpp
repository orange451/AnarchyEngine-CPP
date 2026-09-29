#include "LuauComplete.hpp"

#include "LuauWords.hpp"
#include "Utf8.hpp"

#include "Game.hpp"
#include "ScriptAnalysis.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ide {
namespace {

struct Param {
    std::string name;
    std::string type_name;
};

struct Token {
    enum Kind {
        Name, Number, String, Dot, Colon, Comma, LParen, RParen, LBrack, RBrack, LBrace, RBrace, Eq, Op, Keyword,
        Ellipsis, Semi
    } kind = Name;
    int begin = 0;
    int end = 0;
    std::string text;
};

bool StartsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool IsDigit(char32_t code) { return code >= U'0' && code <= U'9'; }

bool IsNameStart(char32_t code) {
    return (code >= U'A' && code <= U'Z') || (code >= U'a' && code <= U'z') || code == U'_' || code >= 0x80;
}

bool IsNameContinue(char32_t code) { return IsNameStart(code) || IsDigit(code); }

// Completion offers continue and export, but not const or type, which more
// often name a variable.
constexpr LuauContextual kCompletionContextual = LuauContextual::Continue | LuauContextual::Export;

const char* KeywordText(std::string_view word) { return LuauKeyword(word, kCompletionContextual); }

// The keywords completion offers, in alphabetical order.
const std::vector<const char*>& CompletionKeywords() {
    static const std::vector<const char*> words = [] {
        std::vector<const char*> out(std::begin(kLuauReserved), std::end(kLuauReserved));
        for (const LuauContextualWord& word : kLuauContextual) {
            if ((static_cast<unsigned>(kCompletionContextual) & static_cast<unsigned>(word.flag)) != 0) {
                out.push_back(word.text);
            }
        }
        std::sort(out.begin(), out.end(), [](const char* a, const char* b) { return std::strcmp(a, b) < 0; });
        return out;
    }();
    return words;
}

int LongSeparator(const std::u32string& text, int index) {
    if (index < 0 || index >= static_cast<int>(text.size())) {
        return -1;
    }
    const char32_t start = text[static_cast<std::size_t>(index)];
    if (start != U'[' && start != U']') {
        return -1;
    }
    int count = 0;
    int cursor = index + 1;
    while (cursor < static_cast<int>(text.size()) && text[static_cast<std::size_t>(cursor)] == U'=') {
        ++count;
        ++cursor;
    }
    if (cursor < static_cast<int>(text.size()) && text[static_cast<std::size_t>(cursor)] == start) {
        return count;
    }
    return -1;
}

struct Scan {
    std::vector<Token> tokens;
    bool blocked = false;
    // The caret sits in a header `--!` comment. `directive_begin` is the code point after `!`.
    bool directive = false;
    int directive_begin = 0;
    // The caret sits inside a ' or " literal that has not closed yet.
    bool open_string = false;
    int string_begin = 0;
    char32_t quote = 0;
};

Scan Tokenize(const std::u32string& text, int caret) {
    Scan scan;
    auto emit = [&](Token::Kind kind, int begin, int end, std::string value) {
        Token token;
        token.kind = kind;
        token.begin = begin;
        token.end = end;
        token.text = std::move(value);
        scan.tokens.push_back(std::move(token));
    };
    int i = 0;
    while (i < caret && i < static_cast<int>(text.size())) {
        const char32_t code = text[static_cast<std::size_t>(i)];
        if (code == U' ' || code == U'\t' || code == U'\n' || code == U'\r' || code == U'\v' || code == U'\f') {
            ++i;
            continue;
        }
        if (code == U'-' && i + 1 < caret && text[static_cast<std::size_t>(i + 1)] == U'-') {
            i += 2;
            if (i < caret && text[static_cast<std::size_t>(i)] == U'[') {
                const int sep = LongSeparator(text, i);
                if (sep >= 0) {
                    i += sep + 2;
                    bool closed = false;
                    while (i < caret) {
                        if (text[static_cast<std::size_t>(i)] == U']' && LongSeparator(text, i) == sep) {
                            i += sep + 2;
                            closed = true;
                            break;
                        }
                        ++i;
                    }
                    if (!closed) {
                        scan.blocked = true;
                        return scan;
                    }
                    continue;
                }
            }
            const int body = i;
            while (i < caret && text[static_cast<std::size_t>(i)] != U'\n' && text[static_cast<std::size_t>(i)] != U'\r') {
                ++i;
            }
            if (i >= caret) {
                // The caret is inside this line comment. A `--!` before any code is a directive.
                // Luau ignores the same comment once a statement has been written.
                if (scan.tokens.empty() && body < caret && text[static_cast<std::size_t>(body)] == U'!') {
                    scan.directive = true;
                    scan.directive_begin = body + 1;
                }
                scan.blocked = true;
                return scan;
            }
            continue;
        }
        if (code == U'"' || code == U'\'') {
            const int start = i;
            ++i;
            std::u32string body;
            bool closed = false;
            while (i < caret) {
                const char32_t unit = text[static_cast<std::size_t>(i)];
                if (unit == code) {
                    ++i;
                    closed = true;
                    break;
                }
                if (unit == U'\n' || unit == U'\r') {
                    break;
                }
                if (unit == U'\\' && i + 1 < caret) {
                    body.push_back(text[static_cast<std::size_t>(i + 1)]);
                    i += 2;
                    continue;
                }
                body.push_back(unit);
                ++i;
            }
            if (!closed) {
                if (i < caret) {
                    // A raw newline ended the literal before the caret.
                    scan.blocked = true;
                    return scan;
                }
                scan.open_string = true;
                scan.string_begin = start + 1;
                scan.quote = code;
                return scan;
            }
            emit(Token::String, start, i, Utf8(body));
            continue;
        }
        if (code == U'`') {
            ++i;
            while (i < caret && text[static_cast<std::size_t>(i)] != U'`') {
                ++i;
            }
            if (i >= caret) {
                scan.blocked = true;
                return scan;
            }
            ++i;
            emit(Token::String, 0, i, {});
            continue;
        }
        if (IsDigit(code) || (code == U'.' && i + 1 < caret && IsDigit(text[static_cast<std::size_t>(i + 1)]))) {
            const int start = i;
            if (code == U'0' && i + 1 < caret && (text[static_cast<std::size_t>(i + 1)] == U'x' ||
                                                  text[static_cast<std::size_t>(i + 1)] == U'X' ||
                                                  text[static_cast<std::size_t>(i + 1)] == U'b' ||
                                                  text[static_cast<std::size_t>(i + 1)] == U'B')) {
                i += 2;
            }
            while (i < caret && (IsDigit(text[static_cast<std::size_t>(i)]) || text[static_cast<std::size_t>(i)] == U'_' ||
                                 text[static_cast<std::size_t>(i)] == U'.' || text[static_cast<std::size_t>(i)] == U'x' ||
                                 text[static_cast<std::size_t>(i)] == U'e' || text[static_cast<std::size_t>(i)] == U'E')) {
                if (text[static_cast<std::size_t>(i)] == U'.' && (i + 1 >= caret || !IsDigit(text[static_cast<std::size_t>(i + 1)]))) {
                    break;
                }
                ++i;
            }
            emit(Token::Number, start, i, {});
            continue;
        }
        if (IsNameStart(code)) {
            const int start = i;
            ++i;
            while (i < caret && IsNameContinue(text[static_cast<std::size_t>(i)])) {
                ++i;
            }
            const std::string word = Utf8(std::u32string_view(text.data() + start, static_cast<std::size_t>(i - start)));
            emit(KeywordText(word) != nullptr ? Token::Keyword : Token::Name, start, i, word);
            continue;
        }
        const int start = i;
        ++i;
        if (code == U'.' && i < caret && text[static_cast<std::size_t>(i)] == U'.') {
            ++i;
            if (i < caret && text[static_cast<std::size_t>(i)] == U'.') {
                ++i;
                emit(Token::Ellipsis, start, i, "...");
            } else {
                emit(Token::Op, start, i, "..");
            }
            continue;
        }
        if (code == U'=' && i < caret && text[static_cast<std::size_t>(i)] == U'=') {
            ++i;
            emit(Token::Op, start, i, "==");
            continue;
        }
        if ((code == U'~' || code == U'<' || code == U'>') && i < caret && text[static_cast<std::size_t>(i)] == U'=') {
            ++i;
            emit(Token::Op, start, i, code == U'~' ? "~=" : code == U'<' ? "<=" : ">=");
            continue;
        }
        Token::Kind kind = Token::Op;
        std::string value(1, static_cast<char>(code < 128 ? code : '?'));
        if (code == U'.') {
            kind = Token::Dot;
        } else if (code == U':') {
            kind = Token::Colon;
        } else if (code == U',') {
            kind = Token::Comma;
        } else if (code == U'(') {
            kind = Token::LParen;
        } else if (code == U')') {
            kind = Token::RParen;
        } else if (code == U'[') {
            kind = Token::LBrack;
        } else if (code == U']') {
            kind = Token::RBrack;
        } else if (code == U'{') {
            kind = Token::LBrace;
        } else if (code == U'}') {
            kind = Token::RBrace;
        } else if (code == U'=') {
            kind = Token::Eq;
        } else if (code == U';') {
            kind = Token::Semi;
        }
        emit(kind, start, i, std::move(value));
    }
    return scan;
}

const engine_core::LuaNode* FindNode(const std::vector<engine_core::LuaNode>& world, std::uint32_t id) {
    for (const engine_core::LuaNode& node : world) {
        if (node.id == id) {
            return &node;
        }
    }
    return nullptr;
}

bool IsCallPrefix(const Token& token) {
    if (token.kind == Token::Name || token.kind == Token::String || token.kind == Token::RParen ||
        token.kind == Token::RBrack || token.kind == Token::RBrace) {
        return true;
    }
    return token.kind == Token::Keyword && (token.text == "true" || token.text == "false" || token.text == "nil");
}

// `obj:Method(` passes the receiver as self. `obj.Method(` does not, so the
// first string is not the service or child name.
bool IsColonCall(const std::vector<Token>& tokens, int callee_end) {
    const int name = callee_end - 1;
    if (name < 1 || tokens[static_cast<std::size_t>(name)].kind != Token::Name) {
        return false;
    }
    if (tokens[static_cast<std::size_t>(name - 1)].kind != Token::Colon) {
        return false;
    }
    return name < 2 || tokens[static_cast<std::size_t>(name - 2)].kind != Token::Colon;
}

// The caret is the start of an argument. `open` is the call's '('.
struct CallSlot {
    int open = -1;
    int argument = 0;
    bool found = false;
};

CallSlot CallArgumentAt(const std::vector<Token>& tokens, int index) {
    CallSlot slot;
    if (index <= 0 || index > static_cast<int>(tokens.size())) {
        return slot;
    }
    const Token::Kind previous = tokens[static_cast<std::size_t>(index - 1)].kind;
    if (previous != Token::LParen && previous != Token::Comma) {
        return slot;
    }
    int depth = 0;
    int commas = 0;
    for (int cursor = index - 1; cursor >= 0; --cursor) {
        const Token::Kind kind = tokens[static_cast<std::size_t>(cursor)].kind;
        if (kind == Token::RParen || kind == Token::RBrack || kind == Token::RBrace) {
            ++depth;
        } else if (kind == Token::LParen || kind == Token::LBrack || kind == Token::LBrace) {
            if (depth == 0) {
                if (kind != Token::LParen) {
                    return slot;
                }
                slot.open = cursor;
                slot.argument = commas;
                slot.found = true;
                return slot;
            }
            --depth;
        } else if (depth == 0 && kind == Token::Comma) {
            ++commas;
        }
    }
    return slot;
}

bool AnonymousFunctionOpen(const std::vector<Token>& tokens, int open) {
    return open > 0 && tokens[static_cast<std::size_t>(open - 1)].kind == Token::Keyword &&
           tokens[static_cast<std::size_t>(open - 1)].text == "function";
}

// The '(' of `function name(`, `function a.b(`, or `function a:b(`: a parameter
// list being written, not a call.
bool NamedFunctionOpen(const std::vector<Token>& tokens, int open) {
    int cursor = open - 1;
    bool name = false;
    while (cursor >= 0) {
        const Token& token = tokens[static_cast<std::size_t>(cursor)];
        if (token.kind == Token::Name && !name) {
            name = true;
        } else if ((token.kind == Token::Dot || token.kind == Token::Colon) && name) {
            name = false;
        } else {
            break;
        }
        --cursor;
    }
    return name && cursor >= 0 && tokens[static_cast<std::size_t>(cursor)].kind == Token::Keyword &&
           tokens[static_cast<std::size_t>(cursor)].text == "function";
}

// `active` is the parameter being typed. Its byte range in the result goes to
// `bold`. Past the last parameter, `...` is the active one, or nothing is.
std::string FormatParams(const std::vector<Param>& params, bool variadic, int active = -1,
                         std::pair<int, int>* bold = nullptr) {
    std::string out = "(";
    const auto mark = [&](int index, std::size_t begin) {
        if (bold != nullptr && index == active) {
            *bold = {static_cast<int>(begin), static_cast<int>(out.size())};
        }
    };
    for (std::size_t index = 0; index < params.size(); ++index) {
        if (index > 0) {
            out += ", ";
        }
        const std::size_t begin = out.size();
        out += params[index].name;
        if (!params[index].type_name.empty()) {
            out += ": ";
            out += params[index].type_name;
        }
        mark(static_cast<int>(index), begin);
    }
    if (variadic) {
        if (!params.empty()) {
            out += ", ";
        }
        const std::size_t begin = out.size();
        out += "...";
        if (active >= static_cast<int>(params.size())) {
            active = static_cast<int>(params.size());
        }
        mark(static_cast<int>(params.size()), begin);
    }
    out += ")";
    return out;
}

// Puts a parameter list in the popup's header, with `lead` before it, such as
// `function`. The parameter being typed is drawn bold.
void SetSignature(CompletionList& list, const std::string& lead, const std::vector<Param>& params, bool variadic,
                  int active) {
    std::pair<int, int> bold{-1, -1};
    list.signature = lead + FormatParams(params, variadic, active, &bold);
    list.signature_bold_begin = bold.first < 0 ? -1 : bold.first + static_cast<int>(lead.size());
    list.signature_bold_end = bold.second < 0 ? -1 : bold.second + static_cast<int>(lead.size());
}

// "string?" is a string for matching. Unions and function types stay as written.
std::string CoreType(std::string_view type) {
    while (!type.empty() && (type.back() == '?' || type.back() == ' ')) {
        type.remove_suffix(1);
    }
    return std::string(type);
}

bool IsIdent(std::string_view text) {
    if (text.empty() || !IsNameStart(static_cast<char32_t>(static_cast<unsigned char>(text.front())))) {
        return false;
    }
    for (unsigned char unit : text) {
        if (!IsNameContinue(static_cast<char32_t>(unit))) {
            return false;
        }
    }
    return true;
}

// Every class in the tree is a DataModel: Instance and its subclasses, and Game.
bool IsInstanceClass(const std::string& name) {
    return engine_core::lua_class_inherits(name.c_str(), "DataModel");
}

bool AcceptsType(const std::string& expected, const CompletionItem& item) {
    if (expected.empty()) {
        return false;
    }
    if (expected == "boolean" && (item.name == "true" || item.name == "false")) {
        return true;
    }
    if (expected == "function" && (item.call || item.name == "function")) {
        return true;
    }
    if (item.detail == expected) {
        return true;
    }
    if (!IsIdent(expected) || !IsInstanceClass(expected) || !IsInstanceClass(item.detail)) {
        return false;
    }
    // Instance takes what Instance.new makes, not game. DataModel takes both.
    return engine_core::lua_class_inherits(item.detail.c_str(), expected.c_str());
}

// The hover tooltip's text for one shape. `returns` is the short form shown on a completion row.
struct Written {
    bool found = false;
    std::string title;
    std::string detail;
    std::string summary;
    std::string returns;
};

bool IsFunctionParen(const std::vector<Token>& tokens, int paren) {
    int j = paren - 1;
    if (j < 0) {
        return false;
    }
    if (tokens[static_cast<std::size_t>(j)].kind == Token::Keyword && tokens[static_cast<std::size_t>(j)].text == "function") {
        return true;
    }
    if (tokens[static_cast<std::size_t>(j)].kind == Token::Name) {
        --j;
        if (j >= 0 && (tokens[static_cast<std::size_t>(j)].kind == Token::Dot ||
                       tokens[static_cast<std::size_t>(j)].kind == Token::Colon)) {
            --j;
            if (j >= 0 && tokens[static_cast<std::size_t>(j)].kind == Token::Name) {
                --j;
            }
        }
        return j >= 0 && tokens[static_cast<std::size_t>(j)].kind == Token::Keyword &&
               tokens[static_cast<std::size_t>(j)].text == "function";
    }
    return false;
}

bool AnnotationAt(const std::vector<Token>& tokens, int colon) {
    if (colon < 1 || tokens[static_cast<std::size_t>(colon)].kind != Token::Colon) {
        return false;
    }
    int name = colon - 1;
    if (tokens[static_cast<std::size_t>(name)].kind != Token::Name) {
        return false;
    }
    int j = name - 1;
    for (int guard = 0; guard < 32 && j >= 0; ++guard) {
        const Token& token = tokens[static_cast<std::size_t>(j)];
        if ((token.kind == Token::Keyword && (token.text == "local" || token.text == "for")) ||
            (token.kind == Token::LParen && IsFunctionParen(tokens, j))) {
            return true;
        }
        if (token.kind != Token::Comma) {
            return false;
        }
        --j;
        if (j >= 1 && tokens[static_cast<std::size_t>(j)].kind == Token::Name &&
            tokens[static_cast<std::size_t>(j - 1)].kind == Token::Colon) {
            j -= 2;
        }
        if (j < 0 || tokens[static_cast<std::size_t>(j)].kind != Token::Name) {
            return false;
        }
        --j;
    }
    return false;
}

std::string JoinParams(const std::vector<std::pair<std::string, std::string>>& params, bool variadic) {
    std::string out = "(";
    for (std::size_t index = 0; index < params.size(); ++index) {
        if (index > 0) {
            out += ", ";
        }
        out += params[index].first;
        if (!params[index].second.empty()) {
            out += ": ";
            out += params[index].second;
        }
    }
    if (variadic) {
        if (!params.empty()) {
            out += ", ";
        }
        out += "...";
    }
    out += ")";
    return out;
}

void AddTypes(std::string_view prefix, std::vector<CompletionItem>& out) {
    std::vector<std::string> names;
    engine_core::lua_class_names(names);
    const char* primitives[] = {"string", "number", "boolean", "vector", "buffer"};
    for (const char* primitive : primitives) {
        names.emplace_back(primitive);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    for (const std::string& name : names) {
        if (!prefix.empty() && !StartsWith(name, prefix)) {
            continue;
        }
        CompletionItem item;
        item.name = name;
        item.detail = "type";
        const engine_core::LuaDoc doc = engine_core::lua_symbol_doc("", name);
        if (doc.found && !doc.summary.empty()) {
            item.title = name;
            item.summary = doc.summary;
        }
        out.push_back(std::move(item));
    }
}

int StringContentEnd(const std::u32string& text, int content_begin, char32_t quote) {
    int index = content_begin;
    const int size = static_cast<int>(text.size());
    if (index < 0) {
        index = 0;
    }
    while (index < size) {
        const char32_t unit = text[static_cast<std::size_t>(index)];
        if (unit == quote || unit == U'\n' || unit == U'\r') {
            return index;
        }
        if (unit == U'\\' && index + 1 < size) {
            index += 2;
            continue;
        }
        ++index;
    }
    return size;
}

std::string ArgumentPrefix(const std::u32string& text, int begin, int caret) {
    if (begin < 0) {
        begin = 0;
    }
    if (caret < begin) {
        return {};
    }
    if (caret > static_cast<int>(text.size())) {
        caret = static_cast<int>(text.size());
    }
    return Utf8(std::u32string_view(text.data() + begin, static_cast<std::size_t>(caret - begin)));
}

void AddChildren(const std::vector<engine_core::LuaNode>& world, std::uint32_t parent, std::string_view prefix,
                 std::vector<CompletionItem>& out) {
    struct Row {
        std::string name;
        std::string detail;
    };
    std::vector<Row> rows;
    for (const engine_core::LuaNode& node : world) {
        if (node.parent != parent || node.name.empty()) {
            continue;
        }
        if (!prefix.empty() && !StartsWith(node.name, prefix)) {
            continue;
        }
        Row row;
        row.name = node.name;
        row.detail = node.class_name.empty() ? "Instance" : node.class_name;
        rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end(), [](const Row& left, const Row& right) {
        if (left.name != right.name) {
            return left.name < right.name;
        }
        return left.detail < right.detail;
    });
    rows.erase(std::unique(rows.begin(), rows.end(),
                           [](const Row& left, const Row& right) { return left.name == right.name; }),
               rows.end());
    for (Row& row : rows) {
        CompletionItem item;
        item.name = std::move(row.name);
        item.detail = std::move(row.detail);
        out.push_back(std::move(item));
    }
}

void AddNamed(std::string_view prefix, const std::vector<std::string>& names, const char* detail,
              std::vector<CompletionItem>& out) {
    std::vector<std::string> sorted = names;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    for (const std::string& name : sorted) {
        if (!prefix.empty() && !StartsWith(name, prefix)) {
            continue;
        }
        CompletionItem item;
        item.name = name;
        item.detail = detail;
        out.push_back(std::move(item));
    }
}

void AddServices(std::string_view prefix, std::vector<CompletionItem>& out) {
    std::vector<std::string> names;
    engine_core::lua_service_names(names);
    AddNamed(prefix, names, "service", out);
}

void AddCreatable(std::string_view prefix, std::vector<CompletionItem>& out) {
    std::vector<std::string> names;
    engine_core::lua_creatable_names(names);
    AddNamed(prefix, names, "class", out);
}

// A statement can end on this token, so the next line may start another.
bool EndsStatement(const Token& token) {
    switch (token.kind) {
    case Token::Name:
    case Token::Number:
    case Token::String:
    case Token::RParen:
    case Token::RBrack:
    case Token::RBrace:
    case Token::Ellipsis:
    case Token::Semi:
        return true;
    case Token::Keyword:
        return token.text == "end" || token.text == "true" || token.text == "false" || token.text == "nil" ||
               token.text == "break" || token.text == "continue";
    default:
        return false;
    }
}

// Tokens [0, count) leave no function, block, loop header, or bracket open, so
// the next statement belongs to the chunk itself. An `if` that follows `=`, `(`,
// `return`, or an operator is an if-expression, which has no `end`.
bool AtChunkTop(const std::vector<Token>& tokens, int count) {
    // 'b' is a block closed by `end` or `until`. 'h' is a `while` or `for` header
    // waiting for its `do`. '(' is any bracket.
    std::vector<char> open;
    for (int index = 0; index < count; ++index) {
        const Token& token = tokens[static_cast<std::size_t>(index)];
        if (token.kind == Token::LParen || token.kind == Token::LBrack || token.kind == Token::LBrace) {
            open.push_back('(');
        } else if (token.kind == Token::RParen || token.kind == Token::RBrack || token.kind == Token::RBrace) {
            if (!open.empty()) {
                open.pop_back();
            }
        } else if (token.kind != Token::Keyword) {
            continue;
        } else if (token.text == "function" || token.text == "repeat") {
            open.push_back('b');
        } else if (token.text == "if") {
            const Token* before = index > 0 ? &tokens[static_cast<std::size_t>(index - 1)] : nullptr;
            if (before == nullptr || EndsStatement(*before) ||
                (before->kind == Token::Keyword &&
                 (before->text == "then" || before->text == "else" || before->text == "do" || before->text == "repeat"))) {
                open.push_back('b');
            }
        } else if (token.text == "while" || token.text == "for") {
            open.push_back('h');
        } else if (token.text == "do") {
            if (!open.empty() && open.back() == 'h') {
                open.back() = 'b';
            } else {
                open.push_back('b');
            }
        } else if (token.text == "end" || token.text == "until") {
            if (!open.empty()) {
                open.pop_back();
            }
        }
    }
    return open.empty();
}

// Only spaces and tabs come before code point `index` on its line.
bool StartsLine(const std::u32string& text, int index) {
    for (int at = index - 1; at >= 0; --at) {
        const char32_t code = text[static_cast<std::size_t>(at)];
        if (code == U'\n' || code == U'\r') {
            return true;
        }
        if (code != U' ' && code != U'\t') {
            return false;
        }
    }
    return true;
}

bool IsAsciiName(std::string_view text) {
    if (text.empty() || KeywordText(text) != nullptr) {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char unit = text[index];
        const bool letter = (unit >= 'A' && unit <= 'Z') || (unit >= 'a' && unit <= 'z') || unit == '_';
        if (!letter && !(index > 0 && unit >= '0' && unit <= '9')) {
            return false;
        }
    }
    return true;
}

std::string QuoteString(std::string_view text) {
    std::string out = "\"";
    for (const char unit : text) {
        if (unit == '"' || unit == '\\') {
            out.push_back('\\');
            out.push_back(unit);
        } else if (unit == '\n') {
            out += "\\n";
        } else if (unit == '\r') {
            out += "\\r";
        } else {
            out.push_back(unit);
        }
    }
    out.push_back('"');
    return out;
}

// The expression that reaches `id` from game, such as `game.Folder.Config`. A name
// that is not an identifier is indexed as `["My Part"]`. A name a member of its
// parent shadows, such as a child called Name, is found with FindFirstChild.
// Empty when `id` is not under game.
std::string InstancePath(const std::vector<engine_core::LuaNode>& world, std::uint32_t id) {
    std::vector<const engine_core::LuaNode*> chain;
    const engine_core::LuaNode* node = FindNode(world, id);
    while (node != nullptr && node->id != 0) {
        if (chain.size() > world.size()) {
            return {};
        }
        chain.push_back(node);
        node = FindNode(world, node->parent);
    }
    if (node == nullptr || chain.empty()) {
        return {};
    }
    std::string path = "game";
    std::string parent_class = node->class_name.empty() ? "Game" : node->class_name;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const std::string& name = (*it)->name;
        const engine_core::LuaField* member = engine_core::lua_class_find(parent_class.c_str(), name);
        if (member != nullptr && member->name != nullptr) {
            path += ":FindFirstChild(" + QuoteString(name) + ")";
        } else if (IsAsciiName(name)) {
            path += "." + name;
        } else {
            path += "[" + QuoteString(name) + "]";
        }
        parent_class = (*it)->class_name.empty() ? "Instance" : (*it)->class_name;
    }
    return path;
}

// A local named after an instance: its letters, digits, and underscores. It never
// starts with a digit and is never a keyword.
std::string LocalName(std::string_view name) {
    std::string out;
    for (const char unit : name) {
        if ((unit >= 'A' && unit <= 'Z') || (unit >= 'a' && unit <= 'z') || (unit >= '0' && unit <= '9') ||
            unit == '_') {
            out.push_back(unit);
        }
    }
    if (out.empty() || (out.front() >= '0' && out.front() <= '9')) {
        out.insert(out.begin(), '_');
    }
    if (KeywordText(out) != nullptr) {
        out.push_back('_');
    }
    return out;
}

// Every ModuleScript under game but the script being edited, and every registered
// service, whose name starts with `prefix`. Each row writes the local that holds it.
void AddRequires(const std::vector<engine_core::LuaNode>& world, std::uint32_t script_id, std::string_view prefix,
                 std::vector<CompletionItem>& out) {
    const std::size_t first = out.size();
    for (const engine_core::LuaNode& node : world) {
        if (node.class_name != "ModuleScript" || node.id == script_id || node.name.empty()) {
            continue;
        }
        if (!prefix.empty() && !StartsWith(node.name, prefix)) {
            continue;
        }
        const std::string path = InstancePath(world, node.id);
        if (path.empty()) {
            continue;
        }
        CompletionItem item;
        item.name = node.name;
        item.detail = path;
        item.insert = "local " + LocalName(node.name) + " = require(" + path + ")";
        item.title = item.insert;
        out.push_back(std::move(item));
    }
    std::vector<std::string> services;
    engine_core::lua_service_names(services);
    for (const std::string& name : services) {
        if (!prefix.empty() && !StartsWith(name, prefix)) {
            continue;
        }
        CompletionItem item;
        item.name = name;
        item.detail = "service";
        item.insert = "local " + LocalName(name) + " = game:GetService(" + QuoteString(name) + ")";
        item.title = item.insert;
        const engine_core::LuaDoc doc = engine_core::lua_symbol_doc("", name);
        if (doc.found) {
            item.summary = doc.summary;
        }
        out.push_back(std::move(item));
    }
    std::sort(out.begin() + static_cast<std::ptrdiff_t>(first), out.end(),
              [](const CompletionItem& left, const CompletionItem& right) {
                  if (left.name != right.name) {
                      return left.name < right.name;
                  }
                  return left.detail < right.detail;
              });
}

// Header comments Luau reads before the first statement. Order is the order shown.
struct DirectiveRow {
    const char* name;
    const char* detail;
    const char* summary;
};

constexpr DirectiveRow kDirectives[] = {
    {"strict", "mode", "Report type errors as errors."},
    {"nonstrict", "mode", "Report type errors as warnings."},
    {"nocheck", "mode", "Skip type checking."},
    {"nolint", "lint", "Disable lint warnings, or one named rule."},
    {"native", "compile", "Mark the script as a native module."},
    {"optimize", "compile", "Set the optimization level to 0, 1, or 2."},
};

void AddDirectives(std::string_view prefix, std::vector<CompletionItem>& out) {
    for (const DirectiveRow& row : kDirectives) {
        if (!prefix.empty() && !StartsWith(row.name, prefix)) {
            continue;
        }
        CompletionItem item;
        item.name = row.name;
        item.detail = row.detail;
        item.summary = row.summary;
        out.push_back(std::move(item));
    }
}

void AddLintRules(std::string_view prefix, std::vector<CompletionItem>& out) {
    std::vector<std::string> names;
    engine_core::lint_rule_names(names);
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) {
        if (!prefix.empty() && !StartsWith(name, prefix)) {
            continue;
        }
        CompletionItem item;
        item.name = name;
        item.detail = "rule";
        out.push_back(std::move(item));
    }
}

void AddOptimizeLevels(std::string_view prefix, std::vector<CompletionItem>& out) {
    struct Level {
        const char* name;
        const char* summary;
    };
    constexpr Level kLevels[] = {
        {"0", "Leave the script unoptimized."},
        {"1", "Use the baseline optimizations."},
        {"2", "Use the full optimization level."},
    };
    for (const Level& level : kLevels) {
        if (!prefix.empty() && !StartsWith(level.name, prefix)) {
            continue;
        }
        CompletionItem item;
        item.name = level.name;
        item.detail = "level";
        item.summary = level.summary;
        out.push_back(std::move(item));
    }
}

bool IsDirectiveSpace(char32_t code) { return code == U' ' || code == U'\t'; }

int DirectiveLineEnd(const std::u32string& text, int index) {
    const int size = static_cast<int>(text.size());
    while (index < size && text[static_cast<std::size_t>(index)] != U'\n' &&
           text[static_cast<std::size_t>(index)] != U'\r') {
        ++index;
    }
    return index;
}

// `--!` completes the directive word. A space after `nolint` or `optimize`
// completes that directive's argument. A space right after `!` is not a directive.
CompletionList CompleteDirective(const std::u32string& text, int caret, int begin) {
    if (begin < 0 || caret < begin || begin > static_cast<int>(text.size())) {
        return {};
    }
    const int line_end = DirectiveLineEnd(text, begin);
    if (caret > line_end) {
        return {};
    }
    if (begin < line_end && IsDirectiveSpace(text[static_cast<std::size_t>(begin)])) {
        return {};
    }
    int word_end = begin;
    while (word_end < line_end && !IsDirectiveSpace(text[static_cast<std::size_t>(word_end)])) {
        ++word_end;
    }
    CompletionList list;
    list.site = CompleteSite::Directive;
    if (caret <= word_end) {
        list.replace_begin = begin;
        list.replace_end = word_end;
        list.prefix = ArgumentPrefix(text, begin, caret);
        AddDirectives(list.prefix, list.items);
    } else {
        const std::string word = ArgumentPrefix(text, begin, word_end);
        int argument = word_end;
        while (argument < line_end && IsDirectiveSpace(text[static_cast<std::size_t>(argument)])) {
            ++argument;
        }
        if (word != "nolint" && word != "optimize") {
            return {};
        }
        if (caret < argument) {
            if (argument != line_end) {
                return {};
            }
            list.replace_begin = caret;
            list.replace_end = line_end;
            list.prefix.clear();
        } else {
            int argument_end = argument;
            while (argument_end < line_end && !IsDirectiveSpace(text[static_cast<std::size_t>(argument_end)])) {
                ++argument_end;
            }
            if (caret > argument_end) {
                return {};
            }
            list.replace_begin = argument;
            list.replace_end = argument_end;
            list.prefix = ArgumentPrefix(text, argument, caret);
        }
        if (word == "nolint") {
            AddLintRules(list.prefix, list.items);
        } else {
            AddOptimizeLevels(list.prefix, list.items);
        }
    }
    if (list.items.empty()) {
        return {};
    }
    return list;
}

void BeginStringArgument(CompletionList& list, const Scan& scan, const std::u32string& text, int caret) {
    list.site = CompleteSite::Argument;
    list.close_quote = static_cast<char>(scan.quote);
    list.replace_begin = scan.string_begin;
    if (list.replace_begin < 0) {
        list.replace_begin = 0;
    }
    if (list.replace_begin > caret) {
        list.replace_begin = caret;
    }
    list.replace_end = StringContentEnd(text, list.replace_begin, scan.quote);
    list.prefix = ArgumentPrefix(text, list.replace_begin, caret);
    const bool at_end = list.replace_end >= static_cast<int>(text.size());
    list.unclosed = at_end || text[static_cast<std::size_t>(list.replace_end)] != scan.quote;
}

}  // namespace

// ================================================================ From Luau's answers
//
// The text decides the site; Luau's type checker says what the names are and
// what they hold; the registry adds docs, children, services, and the classes
// Instance.new makes.

namespace {

engine_core::ScriptAnalysis& SharedAnalysis() {
    static engine_core::Game game;
    static engine_core::ScriptAnalysis analysis(game);
    return analysis;
}

// A type worth showing. Luau writes unknown, any, or a generic such as `a`
// for code that does not say.
bool ShowType(const std::string& type) {
    if (type.empty() || type == "unknown" || type == "any" || type == "*error-type*" || type == "never" ||
        type == "*blocked*" || type.find("*blocked") != std::string::npos || type.rfind("...", 0) == 0 ||
        type.rfind("(...", 0) == 0) {
        return false;
    }
    // A lone generic: one letter, or one letter and digits.
    if (type.size() <= 2 && ((type[0] >= 'a' && type[0] <= 'z') || (type[0] >= 'A' && type[0] <= 'Z'))) {
        return type.size() == 1 || (type[1] >= '0' && type[1] <= '9');
    }
    return true;
}

// The short type a row's detail shows: a class or primitive name. Empty for a
// table, a function, or anything longer.
std::string RowType(const std::string& type) {
    if (!ShowType(type) || type == "nil") {
        return {};
    }
    for (char unit : type) {
        const bool name = (unit >= 'a' && unit <= 'z') || (unit >= 'A' && unit <= 'Z') || (unit >= '0' && unit <= '9') ||
                          unit == '_' || unit == '?';
        if (!name) {
            return type.front() == '{' ? "table" : std::string();
        }
    }
    return type;
}

// What a hover shows for a value's type.
std::string HoverType(const engine_core::LuauTypeAt& luau) {
    if (!luau.class_name.empty()) {
        return luau.class_name;
    }
    const std::string& type = luau.described.type;
    if (type == "vector") {
        return "Vector3";
    }
    return ShowType(type) && type != "nil" ? type : std::string();
}

std::vector<std::pair<std::string, std::string>> ShownParams(const engine_core::LuauSuggestion& fn) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& [name, type] : fn.param_list) {
        out.emplace_back(name.empty() ? RowType(type) : name, name.empty() || !ShowType(type) ? std::string() : type);
    }
    return out;
}

// A function's title and returns. `qualified` is the name after `function`,
// empty for an anonymous one.
Written DescribeFunction(const std::string& qualified, const std::vector<std::pair<std::string, std::string>>& params,
                         bool variadic, const std::string& returns, bool nothing) {
    Written out;
    out.found = true;
    out.title = (qualified.empty() ? "function" : "function " + qualified) + JoinParams(params, variadic);
    if (!returns.empty()) {
        out.title += ": " + returns;
        out.returns = returns;
    }
    if (nothing) {
        out.detail = "returns nothing";
        out.returns = "returns nothing";
    }
    return out;
}

// A host function from the registry's docs, such as task.wait or FindFirstChild.
// A string method's first parameter is the string it is called on.
Written DescribeHost(const std::string& owner, const std::string& qualified, bool method,
                     const engine_core::LuaDoc& doc, const std::string& callback_type,
                     const std::string& luau_returns = {}) {
    std::vector<std::pair<std::string, std::string>> params;
    const std::size_t first = method && owner == "string" && !doc.params.empty() && doc.params[0].type_name == "string" ? 1 : 0;
    for (std::size_t index = first; index < doc.params.size(); ++index) {
        std::string type = doc.params[index].type_name;
        if (doc.params[index].name == "callback" && !callback_type.empty()) {
            type = callback_type;
        }
        params.emplace_back(doc.params[index].name, std::move(type));
    }
    std::string returns;
    bool nothing = false;
    if (!doc.return_unknown && doc.returns_nothing) {
        nothing = true;
    } else if (!doc.return_unknown) {
        returns = doc.return_type;
    } else if (ShowType(luau_returns) && luau_returns != "()") {
        returns = luau_returns;
    }
    Written out = DescribeFunction(qualified, params, doc.variadic, returns, nothing);
    out.summary = doc.summary;
    return out;
}

// A function Luau typed from the source, named as its definition wrote it
// when it has one. A method's hover lists self. Returns that disagree are
// not claimed.
Written DescribeLuauFunction(const std::string& qualified, const engine_core::LuauSuggestion& fn) {
    const std::string& name = fn.defined_as.empty() ? qualified : fn.defined_as;
    const bool nothing = !fn.returns_disagree && (fn.returns.empty() || fn.returns == "()" || fn.returns_none);
    const std::string returns =
        !fn.returns_disagree && !nothing && ShowType(fn.returns) ? fn.returns : std::string();
    return DescribeFunction(name, ShownParams(fn), fn.variadic, returns, nothing);
}

// The row Luau gave for a name, if any.
const engine_core::LuauSuggestion* LuauRow(const engine_core::LuauCompletion& luau, const std::string& name) {
    for (const engine_core::LuauSuggestion& row : luau.items) {
        if (row.name == name) {
            return &row;
        }
    }
    return nullptr;
}

// A row shows a title only when it has a summary or a return to show.
void AttachWritten(CompletionItem& item, const Written& written) {
    if (written.summary.empty() && written.returns.empty()) {
        return;
    }
    item.title = written.title;
    item.summary = written.summary;
    item.returns = written.returns;
}

std::string CallbackTypeOf(const std::vector<Param>& params) {
    if (params.empty()) {
        return {};
    }
    std::string out = "(";
    for (std::size_t index = 0; index < params.size(); ++index) {
        out += index > 0 ? ", " : "";
        out += params[index].name;
        if (!params[index].type_name.empty()) {
            out += ": " + params[index].type_name;
        }
    }
    return out + ") -> ()";
}

// The registry field a signal is: from its own declared type, such as
// Signal_RunService_Heartbeat; else as a member of its owner; else by name on
// the class of the object it is read from.
const engine_core::LuaField* SignalField(const engine_core::LuauTypeAt* signal, const engine_core::LuauTypeAt* object,
                                         const std::string& name) {
    if (signal != nullptr && signal->found) {
        const std::string prefix = "Signal_";
        if (signal->raw_class.rfind(prefix, 0) == 0) {
            const std::string rest = signal->raw_class.substr(prefix.size());
            const std::size_t split = rest.find('_');
            if (split != std::string::npos) {
                if (const engine_core::LuaField* field =
                        engine_core::lua_class_find(rest.substr(0, split).c_str(), rest.substr(split + 1))) {
                    return field;
                }
            }
        }
        if (signal->kind == "member" && !signal->described.owner.empty()) {
            if (const engine_core::LuaField* field =
                    engine_core::lua_class_find(signal->described.owner.c_str(), signal->name)) {
                return field;
            }
        }
    }
    if (object != nullptr && object->found && !object->class_name.empty() && !name.empty()) {
        return engine_core::lua_class_find(object->class_name.c_str(), name);
    }
    return nullptr;
}

// What a signal passes to Connect.
std::vector<Param> SignalParams(const engine_core::LuaField* field) {
    std::vector<Param> out;
    if (field == nullptr || field->params == nullptr) {
        return out;
    }
    for (int index = 0; index < field->param_count; ++index) {
        Param param;
        param.name = field->params[index].name != nullptr ? field->params[index].name : "";
        param.type_name = field->params[index].type_name != nullptr ? field->params[index].type_name : "";
        out.push_back(std::move(param));
    }
    return out;
}

// The registry field a callee is, when it is a registered class's member.
const engine_core::LuaField* CalleeField(const engine_core::LuauTypeAt& callee) {
    if (!callee.found || callee.kind != "member" || callee.described.owner.empty()) {
        return nullptr;
    }
    return engine_core::lua_class_find(callee.described.owner.c_str(), callee.name);
}

std::size_t ByteOf(const std::string& source, int point) { return CodePointByte(source, point); }

// The source with whatever is still open closed at its end: `end` for a
// function, if, or do, `until true` for a repeat, and the brackets. Luau drops
// the body of a block it cannot close, and with it the locals being typed
// inside it. Offsets before the end stay where they were.
std::string ClosedForLuau(const std::string& source) {
    const std::u32string text = Utf32(source);
    const Scan scan = Tokenize(text, static_cast<int>(text.size()));
    std::vector<std::string> open;
    for (const Token& token : scan.tokens) {
        if (token.kind == Token::Keyword) {
            if (token.text == "function" || token.text == "if" || token.text == "do") {
                open.push_back("end");
            } else if (token.text == "repeat") {
                open.push_back("until true");
            } else if ((token.text == "end" || token.text == "until") && !open.empty()) {
                open.pop_back();
            }
        } else if (token.kind == Token::LParen) {
            open.push_back(")");
        } else if (token.kind == Token::LBrace) {
            open.push_back("}");
        } else if (token.kind == Token::LBrack) {
            open.push_back("]");
        } else if ((token.kind == Token::RParen || token.kind == Token::RBrace || token.kind == Token::RBrack) &&
                   !open.empty()) {
            open.pop_back();
        }
    }
    if (open.empty()) {
        return source;
    }
    std::string closed = source;
    for (auto closer = open.rbegin(); closer != open.rend(); ++closer) {
        closed += *closer == ")" || *closer == "}" || *closer == "]" ? *closer : "\n" + *closer;
    }
    return closed;
}

enum class Step { Done, Member, Name, String };

}  // namespace

struct CompletionPlanState {
    Step step = Step::Done;
    // What Luau reads: the source with open blocks closed at its end.
    std::string luau_source;
    std::string source;
    std::u32string text;
    int caret = 0;
    std::vector<Token> tokens;
    int index = 0;
    bool colon = false;
    // The receiver is a string literal, as `"hi":`.
    bool string_receiver = false;
    std::vector<engine_core::LuaNode> world;
    std::uint32_t script_id = 0;
    bool script_global = true;
    // The call the caret is inside, and the callback whose parameters it is
    // naming, when there is one. Token indices.
    CallSlot slot;
    bool in_callback = false;
    CallSlot outer;
    // The string argument's quote and its callee's end token, for Step::String.
    Scan scan;
    int callee_end = -1;
    bool first_arg = true;
    // Which of the facts' types is which. -1 when not asked. The names are the
    // callee's and the signal's as written, for what Luau cannot type.
    int callee_type = -1;
    int signal_type = -1;
    int object_type = -1;
    std::string callee_name;
    std::string signal_name;
};

namespace {

// The last token of the expression before a call's '(' or a member's '.' or ':',
// as a byte offset inside that token.
std::size_t TokenByte(const CompletionPlanState& plan, int token) {
    const Token& at = plan.tokens[static_cast<std::size_t>(token)];
    return ByteOf(plan.source, std::max(at.begin, at.end - 1));
}

// Asks for the callee of the call at `open` and, when it is a member of
// something with a name, for that something too, such as the signal Connect
// is called on.
void AskCallee(CompletionPlan& plan, CompletionPlanState& state, int open) {
    if (open <= 0) {
        return;
    }
    const auto kind = [&](int token) { return state.tokens[static_cast<std::size_t>(token)].kind; };
    state.callee_type = static_cast<int>(plan.offsets.size());
    plan.offsets.push_back(TokenByte(state, open - 1));
    state.callee_name = kind(open - 1) == Token::Name ? state.tokens[static_cast<std::size_t>(open - 1)].text : "";
    // `a.b(` or `a:b(`: the object before the member.
    if (open >= 3 && kind(open - 1) == Token::Name && (kind(open - 2) == Token::Dot || kind(open - 2) == Token::Colon)) {
        state.signal_type = static_cast<int>(plan.offsets.size());
        plan.offsets.push_back(TokenByte(state, open - 3));
        state.signal_name = kind(open - 3) == Token::Name ? state.tokens[static_cast<std::size_t>(open - 3)].text : "";
        // `x.Signal:Connect(`: what the signal is read from, for a signal
        // Luau has no type for.
        if (open >= 5 && kind(open - 3) == Token::Name && (kind(open - 4) == Token::Dot || kind(open - 4) == Token::Colon)) {
            state.object_type = static_cast<int>(plan.offsets.size());
            plan.offsets.push_back(TokenByte(state, open - 5));
        }
    }
}

const engine_core::LuauTypeAt* FactType(const engine_core::LuauFacts& facts, int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= facts.types.size()) {
        return nullptr;
    }
    return &facts.types[static_cast<std::size_t>(index)];
}

void FinishMember(const CompletionPlanState& state, const engine_core::LuauCompletion& luau, CompletionList& list) {
    // `script` is nil on the command line.
    if (!state.script_global && luau.receiver_global == "script") {
        return;
    }
    std::vector<std::string> seen;
    const auto take = [&](CompletionItem item, bool method) {
        if ((state.colon && !method) || (!list.prefix.empty() && !StartsWith(item.name, list.prefix)) ||
            std::find(seen.begin(), seen.end(), item.name) != seen.end()) {
            return;
        }
        seen.push_back(item.name);
        list.items.push_back(std::move(item));
    };
    const auto documented = [&luau](CompletionItem& item, const std::string& owner, bool method, bool call) {
        const engine_core::LuaDoc doc = engine_core::lua_symbol_doc(owner, item.name);
        if (!doc.found) {
            return;
        }
        if (call) {
            const engine_core::LuauSuggestion* row = LuauRow(luau, item.name);
            AttachWritten(item, DescribeHost(owner, owner + (method ? ":" : ".") + item.name, method, doc, {},
                                             row != nullptr ? row->returns : std::string()));
        } else if (!doc.summary.empty()) {
            item.title = item.name + (item.detail.empty() ? std::string() : ": " + item.detail);
            item.summary = doc.summary;
        }
    };
    // A registered class: its members as the registry has them, then an
    // instance's children by name after '.'.
    if (!luau.receiver_class.empty() && engine_core::lua_class_known(luau.receiver_class.c_str())) {
        std::vector<engine_core::LuaField> fields;
        engine_core::lua_class_members(luau.receiver_class.c_str(), fields);
        for (const engine_core::LuaField& field : fields) {
            if (field.name == nullptr) {
                continue;
            }
            CompletionItem item;
            item.name = field.name;
            item.detail = field.method ? "function" : (field.type_name != nullptr ? field.type_name : "");
            item.call = field.method;
            documented(item, luau.receiver_class, field.method, field.method);
            take(std::move(item), field.method);
        }
        if (!state.colon && luau.receiver_instance_known) {
            for (const engine_core::LuaNode& node : state.world) {
                if (node.parent == luau.receiver_instance && node.id != luau.receiver_instance && IsIdent(node.name) &&
                    KeywordText(node.name) == nullptr) {
                    CompletionItem item;
                    item.name = node.name;
                    item.detail = node.class_name.empty() ? "Instance" : node.class_name;
                    take(std::move(item), false);
                }
            }
        }
        return;
    }
    // A library, or a string: what the runtime has, as before.
    std::vector<engine_core::LuaSymbol> symbols;
    std::string owner;
    if (!luau.receiver_global.empty() && engine_core::lua_library_members(luau.receiver_global, symbols)) {
        owner = luau.receiver_global;
    } else if ((state.string_receiver || luau.receiver_type == "string") &&
               engine_core::lua_value_members("string", symbols)) {
        owner = "string";
    }
    if (!owner.empty()) {
        for (const engine_core::LuaSymbol& symbol : symbols) {
            const engine_core::LuauSuggestion* row = LuauRow(luau, symbol.name);
            if (state.colon && row != nullptr && row->wrong_index) {
                continue;
            }
            CompletionItem item;
            item.name = symbol.name;
            item.detail = symbol.type_name;
            item.call = symbol.call;
            documented(item, owner, symbol.method, symbol.call);
            take(std::move(item), symbol.method);
        }
        return;
    }
    // Anything else, as Luau types it: a table's fields after '.', and its
    // `function obj:name` methods after ':'.
    for (const engine_core::LuauSuggestion& row : luau.items) {
        if (row.kind != "property" || !IsIdent(row.name) || KeywordText(row.name) != nullptr ||
            StartsWith(row.name, "__")) {
            continue;
        }
        const bool method = row.function && row.method;
        if (!state.colon && method) {
            continue;
        }
        CompletionItem item;
        item.name = row.name;
        item.call = row.function;
        if (row.function) {
            item.detail = "function";
            AttachWritten(item, DescribeLuauFunction(row.name, row));
        } else if (!row.class_name.empty()) {
            item.detail = row.class_name;
        } else {
            const std::string shown = RowType(row.type);
            item.detail = shown.empty() ? "field" : shown;
        }
        take(std::move(item), method);
    }
}

void FinishNames(const CompletionPlanState& state, const engine_core::LuauCompletion& luau, CompletionList& list) {
    std::vector<std::string> seen;
    const auto take = [&](CompletionItem item) {
        if ((!list.prefix.empty() && !StartsWith(item.name, list.prefix)) ||
            std::find(seen.begin(), seen.end(), item.name) != seen.end()) {
            return;
        }
        seen.push_back(item.name);
        list.items.push_back(std::move(item));
    };
    // Locals, the nearest first.
    std::vector<const engine_core::LuauSuggestion*> locals;
    for (const engine_core::LuauSuggestion& row : luau.items) {
        if (row.local) {
            locals.push_back(&row);
        }
    }
    std::stable_sort(locals.begin(), locals.end(), [](const engine_core::LuauSuggestion* a,
                                                      const engine_core::LuauSuggestion* b) {
        return a->declared > b->declared;
    });
    // Then globals this script defines, such as `function take() end`.
    for (const engine_core::LuauSuggestion& row : luau.items) {
        if (row.defined_here) {
            locals.push_back(&row);
        }
    }
    for (const engine_core::LuauSuggestion* row : locals) {
        CompletionItem item;
        item.name = row->name;
        item.call = row->function;
        if (!row->class_name.empty()) {
            item.detail = row->class_name;
        } else if (row->function) {
            const auto params = ShownParams(*row);
            item.detail = params.empty() && !row->variadic ? "function" : JoinParams(params, row->variadic);
            AttachWritten(item, DescribeLuauFunction(row->name, *row));
        } else {
            std::string shown = RowType(row->type);
            if (!shown.empty() && shown.back() == '?') {
                shown.pop_back();
            }
            item.detail = shown.empty() ? "local" : shown;
        }
        take(std::move(item));
    }
    // The runtime's globals and libraries, then game, script, and keywords.
    std::vector<engine_core::LuaSymbol> globals;
    engine_core::lua_library_globals(globals);
    for (const engine_core::LuaSymbol& symbol : globals) {
        CompletionItem item;
        item.name = symbol.name;
        item.detail = symbol.type_name == "table" ? "library" : symbol.type_name;
        item.call = symbol.call;
        const engine_core::LuaDoc doc = engine_core::lua_symbol_doc("", symbol.name);
        if (doc.found && symbol.call) {
            AttachWritten(item, DescribeHost("", symbol.name, false, doc, {}));
        } else if (doc.found && !doc.summary.empty() && symbol.type_name == "table") {
            item.title = symbol.name;
            item.summary = doc.summary;
        }
        take(std::move(item));
    }
    CompletionItem game;
    game.name = "game";
    game.detail = "Game";
    take(std::move(game));
    if (state.script_global) {
        CompletionItem script;
        script.name = "script";
        script.detail = "Script";
        if (const engine_core::LuaNode* node = FindNode(state.world, state.script_id)) {
            if (!node->class_name.empty()) {
                script.detail = node->class_name;
            }
        }
        take(std::move(script));
    }
    for (const char* keyword : CompletionKeywords()) {
        CompletionItem item;
        item.name = keyword;
        item.detail = "keyword";
        take(std::move(item));
    }
}

// Connect( offers `function(dt)`, and inside that function the signal's
// parameters.
void FinishCallback(const CompletionPlanState& state, const engine_core::LuauFacts& facts, CompletionList& list) {
    const engine_core::LuauTypeAt* callee = FactType(facts, state.callee_type);
    const engine_core::LuaField* field = callee != nullptr ? CalleeField(*callee) : nullptr;
    if (field == nullptr) {
        // A callee Luau could not type, such as Connect on PreRender.
        field = engine_core::lua_class_find("Signal", state.callee_name);
    }
    if (field == nullptr || !field->callback_arg) {
        return;
    }
    const std::vector<Param> params = SignalParams(
        SignalField(FactType(facts, state.signal_type), FactType(facts, state.object_type), state.signal_name));
    if (params.empty()) {
        return;
    }
    const CallSlot& call = state.in_callback ? state.outer : state.slot;
    const int expected = IsColonCall(state.tokens, call.open) ? 0 : 1;
    if (call.argument != expected) {
        return;
    }
    if (state.in_callback) {
        SetSignature(list, "function", params, false, state.slot.argument);
        std::vector<std::string> used;
        for (int cursor = state.slot.open + 1; cursor < state.index && cursor < static_cast<int>(state.tokens.size());
             ++cursor) {
            if (state.tokens[static_cast<std::size_t>(cursor)].kind == Token::Name) {
                used.push_back(state.tokens[static_cast<std::size_t>(cursor)].text);
            }
        }
        std::vector<CompletionItem> extra;
        for (const Param& param : params) {
            if (std::find(used.begin(), used.end(), param.name) != used.end() ||
                (!list.prefix.empty() && !StartsWith(param.name, list.prefix))) {
                continue;
            }
            const bool listed = std::any_of(list.items.begin(), list.items.end(),
                                            [&](const CompletionItem& item) { return item.name == param.name; });
            if (listed) {
                continue;
            }
            CompletionItem item;
            item.name = param.name;
            item.detail = param.type_name.empty() ? "parameter" : param.type_name;
            extra.push_back(std::move(item));
        }
        list.items.insert(list.items.begin(), extra.begin(), extra.end());
        return;
    }
    Param callback;
    callback.name = "callback";
    callback.type_name = CallbackTypeOf(params);
    SetSignature(list, "", {std::move(callback)}, false, 0);
    std::string detail;
    std::string snippet = "function(";
    for (std::size_t index = 0; index < params.size(); ++index) {
        snippet += index > 0 ? ", " : "";
        detail += index > 0 ? ", " : "";
        snippet += params[index].name;
        detail += params[index].type_name.empty() ? "parameter" : params[index].type_name;
    }
    snippet += ")";
    if (!list.prefix.empty() && !StartsWith(snippet, list.prefix)) {
        return;
    }
    list.items.erase(std::remove_if(list.items.begin(), list.items.end(),
                                    [](const CompletionItem& item) {
                                        return item.name == "function" && item.detail == "keyword";
                                    }),
                     list.items.end());
    if (std::any_of(list.items.begin(), list.items.end(), [&](const CompletionItem& item) { return item.name == snippet; })) {
        return;
    }
    CompletionItem item;
    item.name = std::move(snippet);
    item.detail = std::move(detail);
    item.snippet = true;
    list.items.insert(list.items.begin(), std::move(item));
}

// The signature of the call being written, with the parameter being typed in bold.
void FinishSignature(const CompletionPlanState& state, const engine_core::LuauFacts& facts, CompletionList& list) {
    if (!list.signature.empty() || !state.slot.found || state.in_callback ||
        NamedFunctionOpen(state.tokens, state.slot.open)) {
        return;
    }
    const engine_core::LuauTypeAt* callee = FactType(facts, state.callee_type);
    if (callee == nullptr || !callee->found || !callee->described.function) {
        return;
    }
    std::vector<Param> params;
    bool variadic = callee->described.variadic;
    const std::string owner = callee->kind == "member" ? callee->described.owner : std::string();
    const engine_core::LuaDoc doc = engine_core::lua_symbol_doc(owner, callee->name);
    if (doc.found && (!owner.empty() || callee->kind == "global")) {
        const bool method = callee->kind == "member" && state.slot.open >= 2 &&
                            state.tokens[static_cast<std::size_t>(state.slot.open - 2)].kind == Token::Colon;
        const std::size_t first = method && owner == "string" && !doc.params.empty() && doc.params[0].type_name == "string" ? 1 : 0;
        for (std::size_t index = first; index < doc.params.size(); ++index) {
            Param param;
            param.name = doc.params[index].name;
            param.type_name = doc.params[index].type_name;
            params.push_back(std::move(param));
        }
        variadic = doc.variadic;
    } else {
        const bool colon = callee->kind == "member" && state.slot.open >= 2 &&
                           state.tokens[static_cast<std::size_t>(state.slot.open - 2)].kind == Token::Colon;
        const auto shown = ShownParams(callee->described);
        for (std::size_t index = colon && callee->described.method ? 1 : 0; index < shown.size(); ++index) {
            Param param;
            param.name = shown[index].first;
            param.type_name = shown[index].second;
            params.push_back(std::move(param));
        }
    }
    if (params.empty()) {
        return;
    }
    SetSignature(list, "", params, variadic, state.slot.argument);
    if (state.slot.argument < 0 || state.slot.argument >= static_cast<int>(params.size())) {
        return;
    }
    const std::string expected = CoreType(params[static_cast<std::size_t>(state.slot.argument)].type_name);
    if (!expected.empty()) {
        std::stable_partition(list.items.begin(), list.items.end(),
                              [&](const CompletionItem& item) { return AcceptsType(expected, item); });
    }
}

void FinishString(const CompletionPlanState& state, const engine_core::LuauFacts& facts, CompletionList& list) {
    const engine_core::LuauTypeAt* callee = FactType(facts, state.callee_type);
    bool host = false;
    if (callee != nullptr && callee->found) {
        const bool member = callee->kind == "member";
        const engine_core::LuaField* field = member ? CalleeField(*callee) : nullptr;
        const bool colon = state.callee_end >= 0 && state.first_arg && IsColonCall(state.tokens, state.callee_end);
        // FindFirstChild's name is an argument to complete even when the tree
        // does not say which instance it is asked of; the names need the tree.
        const bool child = colon && field != nullptr && field->resolves_child;
        const bool service = colon && field != nullptr && field->service_arg;
        // Instance.new is a dot call on the Instance global, or an alias of it.
        const std::string owner = !callee->function_owner.empty() ? callee->function_owner : callee->described.owner;
        const std::string name = !callee->function_name.empty() ? callee->function_name : callee->name;
        const bool created = !colon && state.first_arg && (member || !callee->function_name.empty()) &&
                             engine_core::lua_function_result(owner, name).class_from_arg &&
                             !(field != nullptr && field->service_arg);
        host = field != nullptr || !callee->function_name.empty() ||
               engine_core::lua_symbol_doc(member ? callee->described.owner : std::string(), callee->name).found;
        if (child || service || created) {
            BeginStringArgument(list, state.scan, state.text, state.caret);
            if (child) {
                if (callee->object_instance_known) {
                    AddChildren(state.world, callee->object_instance, list.prefix, list.items);
                }
            } else if (service) {
                AddServices(list.prefix, list.items);
            } else {
                AddCreatable(list.prefix, list.items);
            }
        }
    }
    // Inside a string, only a function written in the script shows its parameters.
    if (!host) {
        FinishSignature(state, facts, list);
    }
    if (list.site == CompleteSite::None && !list.signature.empty()) {
        list.site = CompleteSite::Argument;
    }
}

}  // namespace

CompletionPlan plan_completion(std::string_view source, int caret, const std::vector<engine_core::LuaNode>& world,
                               std::uint32_t script_id, bool script_global) {
    CompletionPlan plan;
    auto state = std::make_shared<CompletionPlanState>();
    state->source = std::string(source);
    state->luau_source = ClosedForLuau(state->source);
    plan.luau_source = state->luau_source;
    state->text = Utf32(source);
    state->world = world;
    state->script_id = script_id;
    state->script_global = script_global;
    caret = std::max(0, std::min(caret, static_cast<int>(state->text.size())));
    state->caret = caret;
    const std::u32string& text = state->text;
    Scan scan = Tokenize(text, caret);
    if (scan.directive) {
        plan.list = CompleteDirective(text, caret, scan.directive_begin);
        return plan;
    }
    if (scan.blocked) {
        return plan;
    }
    state->tokens = scan.tokens;
    const std::vector<Token>& tokens = state->tokens;
    if (scan.open_string) {
        if (tokens.empty() || scan.quote == 0) {
            return plan;
        }
        const int index = static_cast<int>(tokens.size());
        state->slot = CallArgumentAt(tokens, index);
        const Token& last = tokens.back();
        if (last.kind == Token::LParen) {
            state->callee_end = index - 1;
        } else if (IsCallPrefix(last)) {
            state->callee_end = index;
        }
        state->first_arg = !state->slot.found || state->slot.argument == 0;
        const int open = state->slot.found ? state->slot.open : (state->callee_end >= 0 ? state->callee_end : -1);
        if (open <= 0) {
            return plan;
        }
        state->index = index;
        BeginStringArgument(plan.frame, scan, text, caret);
        state->scan = std::move(scan);
        state->step = Step::String;
        AskCallee(plan, *state, open);
        plan.needs_luau = true;
        plan.state = state;
        return plan;
    }
    CompletionList& list = plan.list;
    int index = static_cast<int>(tokens.size());
    if (!tokens.empty() && tokens.back().end == caret &&
        (tokens.back().kind == Token::Name || tokens.back().kind == Token::Keyword)) {
        list.prefix = tokens.back().text;
        --index;
    }
    state->index = index;
    list.replace_end = caret;
    list.replace_begin = list.prefix.empty() ? caret : tokens[static_cast<std::size_t>(index)].begin;
    if (!list.prefix.empty()) {
        int end = caret;
        while (end < static_cast<int>(text.size()) && IsNameContinue(text[static_cast<std::size_t>(end)])) {
            ++end;
        }
        list.replace_end = end;
    }
    if (script_global && index > 0 && tokens[static_cast<std::size_t>(index - 1)].kind == Token::Dot) {
        const Token& dot = tokens[static_cast<std::size_t>(index - 1)];
        if (StartsLine(text, dot.begin) && (index == 1 || EndsStatement(tokens[static_cast<std::size_t>(index - 2)])) &&
            AtChunkTop(tokens, index - 1)) {
            list.site = CompleteSite::Require;
            list.replace_begin = dot.begin;
            AddRequires(world, script_id, list.prefix, list.items);
            return plan;
        }
    }
    const bool cast = index >= 2 && tokens[static_cast<std::size_t>(index - 1)].kind == Token::Colon &&
                      tokens[static_cast<std::size_t>(index - 2)].kind == Token::Colon;
    const bool annotation = !cast && index > 0 && tokens[static_cast<std::size_t>(index - 1)].kind == Token::Colon &&
                            AnnotationAt(tokens, index - 1);
    if (cast || annotation) {
        list.site = CompleteSite::Type;
        AddTypes(list.prefix, list.items);
        return plan;
    }
    const bool member = index > 0 && (tokens[static_cast<std::size_t>(index - 1)].kind == Token::Dot ||
                                      tokens[static_cast<std::size_t>(index - 1)].kind == Token::Colon);
    plan.caret_offset = ByteOf(state->source, caret);
    plan.needs_luau = true;
    if (member) {
        list.site = CompleteSite::Member;
        state->colon = tokens[static_cast<std::size_t>(index - 1)].kind == Token::Colon;
        state->string_receiver = index >= 2 && tokens[static_cast<std::size_t>(index - 2)].kind == Token::String;
        state->step = Step::Member;
        plan.frame = list;
        plan.state = state;
        return plan;
    }
    list.site = CompleteSite::Name;
    plan.frame = list;
    state->step = Step::Name;
    state->slot = CallArgumentAt(tokens, index);
    if (state->slot.found && AnonymousFunctionOpen(tokens, state->slot.open)) {
        state->outer = CallArgumentAt(tokens, state->slot.open - 1);
        state->in_callback = state->outer.found;
        if (state->in_callback) {
            AskCallee(plan, *state, state->outer.open);
        }
    } else if (state->slot.found) {
        AskCallee(plan, *state, state->slot.open);
        if (!NamedFunctionOpen(tokens, state->slot.open)) {
        }
    }
    plan.state = state;
    return plan;
}

CompletionList finish_completion(const CompletionPlan& plan, const engine_core::LuauFacts& facts) {
    CompletionList list = plan.list;
    if (!plan.state) {
        return list;
    }
    const CompletionPlanState& state = *plan.state;
    // Luau gave no answer, as when its check failed: names still list the
    // globals, game, script, and the keywords.
    if (!facts.ran) {
        if (state.step == Step::Name) {
            FinishNames(state, engine_core::LuauCompletion{}, list);
        }
        return list;
    }
    switch (state.step) {
    case Step::Member:
        FinishMember(state, facts.completion, list);
        break;
    case Step::Name:
        FinishNames(state, facts.completion, list);
        FinishCallback(state, facts, list);
        FinishSignature(state, facts, list);
        break;
    case Step::String:
        FinishString(state, facts, list);
        break;
    case Step::Done:
        break;
    }
    return list;
}

struct HoverPlanState {
    std::string source;
    std::string luau_source;
    std::string word;
    int token_index = -1;
    std::vector<Token> tokens;
    // For `function` in a Connect argument: the call it is passed to.
    CompletionPlanState call;
    bool anonymous = false;
    bool callback = false;
};

HoverPlan plan_hover(std::string_view source, int index, const std::vector<engine_core::LuaNode>& world,
                     std::uint32_t script_id, bool script_global) {
    HoverPlan plan;
    const std::u32string text = Utf32(source);
    index = std::max(0, std::min(index, static_cast<int>(text.size())));
    const Scan scan = Tokenize(text, static_cast<int>(text.size()));
    int token_index = -1;
    for (int at = 0; at < static_cast<int>(scan.tokens.size()); ++at) {
        const Token& token = scan.tokens[static_cast<std::size_t>(at)];
        if (index >= token.begin && index < token.end) {
            token_index = at;
            break;
        }
    }
    if (token_index < 0) {
        return plan;
    }
    const Token& token = scan.tokens[static_cast<std::size_t>(token_index)];
    HoverInfo& info = plan.info;
    info.begin = token.begin;
    info.end = token.end;
    auto state = std::make_shared<HoverPlanState>();
    state->source = std::string(source);
    state->luau_source = ClosedForLuau(state->source);
    plan.luau_source = state->luau_source;
    state->word = token.text;
    state->token_index = token_index;
    state->tokens = scan.tokens;
    if (token.kind == Token::Keyword) {
        if (token.text == "true" || token.text == "false") {
            info.found = true;
            info.title = token.text + ": boolean";
            return plan;
        }
        if (token.text == "nil") {
            info.found = true;
            info.title = "nil";
            return plan;
        }
        if (token.text != "function") {
            return plan;
        }
        if (token_index + 1 < static_cast<int>(scan.tokens.size()) &&
            scan.tokens[static_cast<std::size_t>(token_index + 1)].kind == Token::Name) {
            return plan;
        }
        // An anonymous function, and whether it is a Connect callback.
        state->anonymous = true;
        plan.offsets.push_back(ByteOf(state->source, token.begin));
        const CallSlot outer = CallArgumentAt(scan.tokens, token_index);
        if (outer.found) {
            state->call.tokens = scan.tokens;
            state->call.source = state->source;
            state->call.slot = outer;
            CompletionPlan call_plan;
            call_plan.offsets = plan.offsets;
            AskCallee(call_plan, state->call, outer.open);
            plan.offsets = call_plan.offsets;
            state->callback = true;
        }
        plan.needs_luau = true;
        plan.state = state;
        return plan;
    }
    if (token.kind != Token::Name) {
        return plan;
    }
    const auto& tokens = scan.tokens;
    const bool cast = token_index >= 2 && tokens[static_cast<std::size_t>(token_index - 1)].kind == Token::Colon &&
                      tokens[static_cast<std::size_t>(token_index - 2)].kind == Token::Colon;
    bool annotation = cast;
    if (!annotation && token_index > 0 && tokens[static_cast<std::size_t>(token_index - 1)].kind == Token::Colon) {
        const int colon = token_index - 1;
        annotation = AnnotationAt(tokens, colon);
        if (!annotation && colon > 0 && tokens[static_cast<std::size_t>(colon - 1)].kind == Token::RParen) {
            int depth = 0;
            for (int cursor = colon - 1; cursor >= 0; --cursor) {
                const Token::Kind kind = tokens[static_cast<std::size_t>(cursor)].kind;
                depth += kind == Token::RParen ? 1 : kind == Token::LParen ? -1 : 0;
                if (depth == 0) {
                    annotation = IsFunctionParen(tokens, cursor);
                    break;
                }
            }
        }
    }
    if (annotation) {
        info.found = true;
        info.title = token.text;
        info.detail = "type";
        const engine_core::LuaDoc doc = engine_core::lua_symbol_doc("", token.text);
        if (doc.found) {
            info.summary = doc.summary;
        }
        return plan;
    }
    (void)world;
    (void)script_id;
    (void)script_global;
    plan.offsets.push_back(ByteOf(state->source, index));
    plan.needs_luau = true;
    plan.state = state;
    return plan;
}

HoverInfo finish_hover(const HoverPlan& plan, const engine_core::LuauFacts& facts) {
    HoverInfo info = plan.info;
    if (!plan.state || !facts.ran || facts.types.empty()) {
        return plan.state ? HoverInfo{} : info;
    }
    const HoverPlanState& state = *plan.state;
    const engine_core::LuauTypeAt& luau = facts.types[0];
    if (state.anonymous) {
        if (!luau.found || !luau.described.function) {
            return {};
        }
        const Written written = DescribeLuauFunction("", luau.described);
        info.found = true;
        info.title = written.title;
        const engine_core::LuauTypeAt* callee = FactType(facts, state.call.callee_type);
        const engine_core::LuauTypeAt* signal = FactType(facts, state.call.signal_type);
        const engine_core::LuauTypeAt* object = FactType(facts, state.call.object_type);
        const engine_core::LuaField* field = callee != nullptr ? CalleeField(*callee) : nullptr;
        if (field == nullptr) {
            field = engine_core::lua_class_find("Signal", state.call.callee_name);
        }
        const engine_core::LuaField* signal_field = SignalField(signal, object, state.call.signal_name);
        // The signal's owner: from its declared type, its member, or the object.
        std::string owner;
        if (signal != nullptr && signal->raw_class.rfind("Signal_", 0) == 0) {
            const std::string rest = signal->raw_class.substr(7);
            owner = rest.substr(0, rest.find('_'));
        } else if (signal != nullptr && signal->kind == "member") {
            owner = signal->described.owner;
        } else if (object != nullptr) {
            owner = object->class_name;
        }
        if (state.callback && field != nullptr && field->callback_arg && signal_field != nullptr && !owner.empty()) {
            const std::string name = owner + "." + signal_field->name;
            info.detail = "callback, runs each time " + name + " fires";
            const engine_core::LuaDoc doc = engine_core::lua_symbol_doc(owner, signal_field->name);
            if (doc.found) {
                info.summary = doc.summary;
            }
        } else {
            info.detail = written.detail.empty() ? "anonymous function" : "anonymous function, " + written.detail;
        }
        return info;
    }
    if (!luau.found || luau.name != state.word) {
        return {};
    }
    info.found = true;
    const bool member = luau.kind == "member";
    const std::string owner = member ? luau.described.owner : std::string();
    // A library global, such as task.
    std::vector<engine_core::LuaSymbol> globals;
    if (luau.kind == "global") {
        engine_core::lua_library_globals(globals);
        for (const engine_core::LuaSymbol& symbol : globals) {
            if (symbol.name == luau.name && symbol.type_name == "table") {
                info.title = luau.name;
                info.detail = "library";
                const engine_core::LuaDoc doc = engine_core::lua_symbol_doc("", luau.name);
                if (doc.found) {
                    info.summary = doc.summary;
                }
                return info;
            }
        }
    }
    if (luau.described.function) {
        const bool method = member && state.token_index >= 1 &&
                            state.tokens[static_cast<std::size_t>(state.token_index - 1)].kind == Token::Colon;
        const std::string doc_owner = member ? owner : std::string();
        const engine_core::LuaDoc doc = engine_core::lua_symbol_doc(doc_owner, luau.name);
        const bool host = doc.found && (member ? !owner.empty() : luau.kind == "global");
        Written written;
        if (host) {
            const std::string qualified =
                member ? (owner.empty() ? luau.name : owner + (method ? ":" : ".") + luau.name) : luau.name;
            written = DescribeHost(owner, qualified, method, doc, {}, luau.described.returns);
        } else {
            // Named as its definition wrote it; a function value such as
            // `new = function()` reads by its own name.
            written = DescribeLuauFunction(luau.name, luau.described);
        }
        info.title = written.title;
        info.detail = written.detail;
        info.summary = written.summary;
        return info;
    }
    const std::string type = HoverType(luau);
    if (type.empty() && luau.kind == "global") {
        return {};
    }
    if (!type.empty()) {
        info.title = luau.name + ": " + type;
    } else {
        info.title = luau.name;
        info.detail = luau.kind == "parameter" ? "parameter" : (luau.kind == "local" ? "local" : "");
    }
    if (member) {
        const engine_core::LuaDoc doc = engine_core::lua_symbol_doc(owner, luau.name);
        if (doc.found) {
            info.summary = doc.summary;
        }
    }
    return info;
}

std::optional<PendingCompletion> ask_completion(CompletionList& now, engine_core::ScriptAnalysis& analysis,
                                                std::string_view source, int caret,
                                                const std::vector<engine_core::LuaNode>& world,
                                                std::uint32_t script_id, bool script_global, bool force,
                                                const char* lane) {
    PendingCompletion pending;
    pending.plan = plan_completion(source, caret, world, script_id, script_global);
    now = pending.plan.list;
    if (!pending.plan.needs_luau) {
        return std::nullopt;
    }
    pending.answer = analysis.luau_facts_later(world, script_id, pending.plan.luau_source, pending.plan.caret_offset,
                                               pending.plan.offsets, lane);
    if (!pending.answer) {
        return std::nullopt;
    }
    pending.source = std::string(source);
    pending.caret = caret;
    pending.force = force;
    return pending;
}

std::optional<CompletionList> take_completion(const PendingCompletion& pending) {
    if (!pending.answer || !pending.answer->ready.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    return finish_completion(pending.plan, pending.answer->facts);
}

std::optional<PendingHover> ask_hover(HoverInfo& now, engine_core::ScriptAnalysis& analysis, std::string_view source,
                                      int index, const std::vector<engine_core::LuaNode>& world,
                                      std::uint32_t script_id) {
    PendingHover pending;
    pending.plan = plan_hover(source, index, world, script_id, true);
    now = pending.plan.info;
    if (!pending.plan.needs_luau) {
        return std::nullopt;
    }
    now = HoverInfo{};
    pending.answer = analysis.luau_facts_later(world, script_id, pending.plan.luau_source, std::string::npos,
                                               pending.plan.offsets, "hover");
    if (!pending.answer) {
        return std::nullopt;
    }
    pending.source = std::string(source);
    return pending;
}

std::optional<HoverInfo> take_hover(const PendingHover& pending) {
    if (!pending.answer || !pending.answer->ready.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    return finish_hover(pending.plan, pending.answer->facts);
}

CompletionList complete_luau(std::string_view source, int caret, const std::vector<engine_core::LuaNode>& world,
                             std::uint32_t script_id, bool script_global, engine_core::ScriptAnalysis* analysis,
                             std::chrono::milliseconds wait) {
    const CompletionPlan plan = plan_completion(source, caret, world, script_id, script_global);
    if (!plan.needs_luau) {
        return plan.list;
    }
    engine_core::ScriptAnalysis& asked = analysis != nullptr ? *analysis : SharedAnalysis();
    const engine_core::LuauFacts facts =
        asked.luau_facts(world, script_id, plan.luau_source, plan.caret_offset, plan.offsets, wait);
    return finish_completion(plan, facts);
}

HoverInfo hover_luau(std::string_view source, int index, const std::vector<engine_core::LuaNode>& world,
                     std::uint32_t script_id, bool script_global, engine_core::ScriptAnalysis* analysis,
                     std::chrono::milliseconds wait) {
    const HoverPlan plan = plan_hover(source, index, world, script_id, script_global);
    if (!plan.needs_luau) {
        return plan.info;
    }
    engine_core::ScriptAnalysis& asked = analysis != nullptr ? *analysis : SharedAnalysis();
    const engine_core::LuauFacts facts =
        asked.luau_facts(world, script_id, plan.luau_source, std::string::npos, plan.offsets, wait);
    return finish_hover(plan, facts);
}

}  // namespace ide
