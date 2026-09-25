#include "LuauComplete.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ide {
namespace {

constexpr std::uint32_t kNoInstance = 0xffffffffu;

struct Param {
    std::string name;
    std::string type_name;
    // Token index of the parameter name. -1 when the parameter was not written here.
    int decl = -1;
};

struct Shape {
    std::string class_name;
    std::string library;
    std::string value_type;
    std::uint32_t instance = kNoInstance;
    bool call = false;
    bool class_from_arg = false;
    bool resolves_child = false;
    bool service_arg = false;
    bool returns_list = false;
    // Connect: the callback's parameters are `signal_params` from the receiver.
    bool callback_arg = false;
    // The value may be nil. Members still come from the class.
    bool optional = false;
    std::vector<Param> signal_params;
    // Parameters of a function written in this source. Empty for host functions.
    std::vector<Param> params;
    bool variadic = false;
    std::string callee_owner;
    std::string callee_name;
    std::string result_type;
    // A function written in this source, as opposed to a host function.
    bool user_function = false;
    // `function obj:name` takes self as its first parameter.
    bool method = false;
    std::string self_type;
    bool annotated_return = false;
    bool return_known = false;
    bool return_conflict = false;
    bool body_closed = false;
    // The first return statement. A later return does not replace it.
    bool saw_return = false;
    bool first_none = false;
    std::string first_type;
    std::vector<std::pair<std::string, Shape*>> fields;
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

std::u32string Utf32(std::string_view text) {
    std::u32string out;
    out.reserve(text.size());
    for (std::size_t index = 0; index < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[index++]);
        if (lead < 0x80) {
            out.push_back(lead);
            continue;
        }
        int need = 0;
        char32_t value = 0;
        if ((lead & 0xE0) == 0xC0 && lead >= 0xC2) {
            need = 1;
            value = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            need = 2;
            value = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0 && lead <= 0xF4) {
            need = 3;
            value = lead & 0x07;
        } else {
            out.push_back(0xFFFD);
            continue;
        }
        bool ok = true;
        for (int i = 0; i < need; ++i) {
            if (index >= text.size()) {
                ok = false;
                break;
            }
            const unsigned char next = static_cast<unsigned char>(text[index]);
            if ((next & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            value = (value << 6) | (next & 0x3F);
            ++index;
        }
        if (!ok || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
            out.push_back(0xFFFD);
            continue;
        }
        out.push_back(value);
    }
    return out;
}

std::string Utf8(std::u32string_view text) {
    std::string out;
    for (char32_t code : text) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return out;
}

const char* KeywordText(std::string_view word) {
    static const char* kWords[] = {"and",     "break", "continue", "do",   "else",   "elseif", "end",  "export", "false",
                                   "for",     "function", "if",    "in",   "local",  "nil",    "not",  "or",     "repeat",
                                   "return",  "then",  "true",     "until", "while"};
    for (const char* candidate : kWords) {
        if (word == candidate) {
            return candidate;
        }
    }
    return nullptr;
}

const char* kKeywords[] = {"and",  "break",    "continue", "do",    "else", "elseif", "end",  "export", "false", "for",
                           "function", "if",    "in",       "local", "nil",  "not",    "or",   "repeat", "return", "then",
                           "true", "until", "while"};

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
            while (i < caret && text[static_cast<std::size_t>(i)] != U'\n' && text[static_cast<std::size_t>(i)] != U'\r') {
                ++i;
            }
            if (i >= caret) {
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

std::string FormatParams(const std::vector<Param>& params, bool variadic) {
    std::string out = "(";
    for (std::size_t index = 0; index < params.size(); ++index) {
        if (index > 0) {
            out += ", ";
        }
        out += params[index].name;
        if (!params[index].type_name.empty()) {
            out += ": ";
            out += params[index].type_name;
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

// Instance is registered as an alias of DataModel, so any DataModel class fits it.
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
    if (expected == "Instance" || expected == "DataModel") {
        return true;
    }
    return engine_core::lua_class_inherits(item.detail.c_str(), expected.c_str());
}

void AppendSignalParams(Shape* shape, const engine_core::LuaField& field) {
    if (shape == nullptr || field.params == nullptr || field.param_count <= 0) {
        return;
    }
    for (int index = 0; index < field.param_count; ++index) {
        const engine_core::LuaParam& param = field.params[index];
        if (param.name == nullptr || param.name[0] == '\0') {
            continue;
        }
        Param row;
        row.name = param.name;
        row.type_name = param.type_name != nullptr ? param.type_name : "";
        shape->signal_params.push_back(std::move(row));
    }
}

const engine_core::LuaNode* FindChild(const std::vector<engine_core::LuaNode>& world, std::uint32_t parent,
                                      std::string_view name) {
    for (const engine_core::LuaNode& node : world) {
        if (node.parent == parent && node.name == name) {
            return &node;
        }
    }
    return nullptr;
}

std::string DescribeType(const Shape* shape) {
    if (shape == nullptr) {
        return {};
    }
    if (!shape->class_name.empty()) {
        return shape->class_name;
    }
    if (!shape->library.empty()) {
        return "table";
    }
    if (!shape->value_type.empty() && shape->value_type != "function") {
        return shape->value_type;
    }
    if (shape->call || shape->user_function || shape->value_type == "function") {
        return "function";
    }
    return {};
}

// Hover shows Vector3 for the built-in vector. Completion still says vector.
std::string ShownType(const Shape* shape) {
    std::string type = DescribeType(shape);
    if (type == "vector") {
        type = "Vector3";
    }
    if (shape != nullptr && shape->optional && !type.empty() && type.back() != '?') {
        type.push_back('?');
    }
    return type;
}

bool ShapeUseful(const Shape* shape) {
    if (shape == nullptr) {
        return false;
    }
    if (!shape->class_name.empty() || !shape->library.empty() || !shape->value_type.empty()) {
        return true;
    }
    return shape->call || shape->user_function || !shape->params.empty() || !shape->result_type.empty() ||
           !shape->fields.empty();
}

// The hover tooltip's text for one shape. `returns` is the short form shown on a completion row.
struct Written {
    bool found = false;
    std::string title;
    std::string detail;
    std::string summary;
    std::string returns;
};

Written DescribeSymbol(const Shape* shape, const std::string& name, bool bound, bool parameter);
void AttachDocs(CompletionItem& item, const Shape* shape);

class Resolver {
public:
    Resolver(const std::vector<Token>& tokens, const std::vector<engine_core::LuaNode>& world, std::uint32_t script_id,
             bool script_global)
        : tokens_(tokens), world_(world), script_id_(script_id), script_global_(script_global) {}

    Shape* fresh() {
        arena_.push_back(std::make_unique<Shape>());
        return arena_.back().get();
    }

    Shape* none() { return fresh(); }

    Shape* class_shape(std::string name, std::uint32_t instance) {
        Shape* shape = fresh();
        shape->class_name = std::move(name);
        shape->instance = instance;
        return shape;
    }

    Shape* value_shape(std::string type, bool call = false) {
        Shape* shape = fresh();
        shape->value_type = std::move(type);
        shape->call = call;
        return shape;
    }

    Shape* type_shape(const std::string& name) {
        std::string type = name;
        bool optional = false;
        if (!type.empty() && type.back() == '?') {
            optional = true;
            type.pop_back();
        }
        Shape* shape = nullptr;
        if (type.empty() || type == "nil") {
            shape = none();
        } else if (type == "string" || type == "number" || type == "boolean" || type == "vector" || type == "buffer" ||
                   type == "function" || type == "table" || type == "thread") {
            shape = value_shape(type, type == "function");
        } else if (engine_core::lua_class_known(type.c_str())) {
            shape = class_shape(std::move(type), kNoInstance);
        } else {
            shape = value_shape(std::move(type));
        }
        if (shape != nullptr) {
            shape->optional = optional;
        }
        return shape;
    }

    Shape* adopt(const engine_core::LuaShape& input) {
        Shape* shape = fresh();
        shape->class_name = input.class_name;
        shape->call = input.call;
        shape->method = input.method;
        if (input.class_name.empty()) {
            if (input.type_name == "string" || input.type_name == "number" || input.type_name == "boolean" ||
                input.type_name == "vector" || input.type_name == "function" || input.type_name == "table") {
                shape->value_type = input.type_name;
            } else if (engine_core::lua_class_known(input.type_name.c_str())) {
                shape->class_name = input.type_name;
            }
            if (input.type_name == "function") {
                shape->call = true;
            }
        }
        for (const auto& field : input.fields) {
            shape->fields.emplace_back(field.first, adopt(field.second));
        }
        return shape;
    }

    Shape* lookup_global(const std::string& name) {
        if (name == "game") {
            const engine_core::LuaNode* root = FindNode(world_, 0);
            return class_shape(root != nullptr && !root->class_name.empty() ? root->class_name : "DataModel", 0);
        }
        if (name == "script") {
            if (!script_global_) {
                return none();
            }
            const engine_core::LuaNode* script = FindNode(world_, script_id_);
            if (script != nullptr) {
                return class_shape(script->class_name.empty() ? "Script" : script->class_name, script->id);
            }
            return class_shape("Script", script_id_);
        }
        std::vector<engine_core::LuaSymbol> globals;
        engine_core::lua_library_globals(globals);
        for (const engine_core::LuaSymbol& symbol : globals) {
            if (symbol.name != name) {
                continue;
            }
            if (symbol.type_name == "table") {
                Shape* shape = fresh();
                shape->library = name;
                return shape;
            }
            Shape* shape = value_shape(symbol.type_name, symbol.call);
            if (symbol.call) {
                shape->callee_owner = "";
                shape->callee_name = name;
            }
            return shape;
        }
        return none();
    }

    Shape* lookup(const std::string& name) {
        if (hover_at_ >= 0) {
            bool bound = false;
            bool parameter = false;
            return lookup_binding(name, hover_at_, bound, parameter);
        }
        for (int index = static_cast<int>(bindings_.size()) - 1; index >= 0; --index) {
            const Binding& binding = bindings_[static_cast<std::size_t>(index)];
            if (binding.name == name && binding.depth <= depth_ && binding.visible <= i_ &&
                (binding.end < 0 || i_ < binding.end)) {
                return binding.shape;
            }
        }
        return lookup_global(name);
    }

    // The binding in scope at `token_index`, including the name's own declaration.
    Shape* lookup_binding(const std::string& name, int token_index, bool& bound, bool& parameter) {
        for (int index = static_cast<int>(bindings_.size()) - 1; index >= 0; --index) {
            const Binding& binding = bindings_[static_cast<std::size_t>(index)];
            if (binding.name != name) {
                continue;
            }
            const bool declaration = binding.decl == token_index;
            const bool span = binding.visible <= token_index && (binding.end < 0 || token_index < binding.end);
            if (!declaration && !span) {
                continue;
            }
            bound = true;
            parameter = binding.parameter;
            return binding.shape != nullptr ? binding.shape : none();
        }
        bound = false;
        parameter = false;
        return lookup_global(name);
    }

    Shape* member_of(Shape* base, const std::string& name) {
        if (base == nullptr) {
            return none();
        }
        for (const auto& field : base->fields) {
            if (field.first == name) {
                return field.second != nullptr ? field.second : none();
            }
        }
        if (!base->class_name.empty()) {
            const engine_core::LuaField* field = engine_core::lua_class_find(base->class_name.c_str(), name);
            if (field == nullptr || field->name == nullptr) {
                return none();
            }
            if (!field->method) {
                if (std::strcmp(field->name, "Parent") == 0 && base->instance != kNoInstance) {
                    if (const engine_core::LuaNode* node = FindNode(world_, base->instance)) {
                        if (node->parent == kNoInstance) {
                            return none();
                        }
                        const engine_core::LuaNode* parent = FindNode(world_, node->parent);
                        std::string class_name = "Instance";
                        if (parent != nullptr && !parent->class_name.empty()) {
                            class_name = parent->class_name;
                        }
                        Shape* parent_shape = class_shape(std::move(class_name), node->parent);
                        parent_shape->callee_owner = base->class_name;
                        parent_shape->callee_name = "Parent";
                        return parent_shape;
                    }
                }
                Shape* shape = type_shape(field->type_name != nullptr ? field->type_name : "");
                AppendSignalParams(shape, *field);
                shape->callee_owner = base->class_name;
                shape->callee_name = field->name;
                return shape;
            }
            Shape* shape = fresh();
            shape->call = true;
            shape->method = true;
            shape->class_from_arg = field->class_from_arg;
            shape->resolves_child = field->resolves_child;
            shape->service_arg = field->service_arg;
            shape->returns_list = field->returns_list;
            shape->callback_arg = field->callback_arg;
            if (field->callback_arg) {
                shape->signal_params = base->signal_params;
            }
            shape->result_type = field->type_name != nullptr ? field->type_name : "";
            shape->callee_owner = base->class_name;
            shape->callee_name = field->name;
            shape->instance = base->instance;
            // Wait returns the signal's arguments, not nil.
            if (std::strcmp(field->name, "Wait") == 0 && !base->signal_params.empty()) {
                std::string type;
                for (std::size_t param = 0; param < base->signal_params.size(); ++param) {
                    if (param > 0) {
                        type += ", ";
                    }
                    type += base->signal_params[param].type_name.empty() ? "any" : base->signal_params[param].type_name;
                }
                shape->result_type = std::move(type);
                shape->return_known = true;
            }
            return shape;
        }
        if (!base->library.empty()) {
            std::vector<engine_core::LuaSymbol> symbols;
            if (!engine_core::lua_library_members(base->library, symbols)) {
                return none();
            }
            for (const engine_core::LuaSymbol& symbol : symbols) {
                if (symbol.name != name) {
                    continue;
                }
                if (!symbol.call && engine_core::lua_class_known(symbol.type_name.c_str())) {
                    Shape* shape = class_shape(symbol.type_name, kNoInstance);
                    shape->callee_owner = base->library;
                    shape->callee_name = name;
                    return shape;
                }
                Shape* shape = fresh();
                shape->callee_owner = base->library;
                shape->callee_name = name;
                if (symbol.type_name == "table") {
                    shape->library = name;
                    return shape;
                }
                if (symbol.type_name == "vector" || symbol.type_name == "string") {
                    shape->value_type = symbol.type_name;
                    return shape;
                }
                shape->value_type = symbol.type_name;
                shape->call = symbol.call;
                if (symbol.call) {
                    const engine_core::LuaResult result = engine_core::lua_function_result(base->library, name);
                    if (result.known) {
                        shape->class_from_arg = result.class_from_arg;
                        shape->result_type = result.type_name;
                    }
                }
                return shape;
            }
            return none();
        }
        if (base->value_type == "string" || base->value_type == "vector") {
            std::vector<engine_core::LuaSymbol> symbols;
            engine_core::lua_value_members(base->value_type, symbols);
            for (const engine_core::LuaSymbol& symbol : symbols) {
                if (symbol.name != name) {
                    continue;
                }
                Shape* shape = value_shape(symbol.type_name.empty() ? "function" : symbol.type_name, symbol.call);
                shape->callee_owner = base->value_type;
                shape->callee_name = name;
                shape->method = symbol.method;
                return shape;
            }
        }
        return none();
    }

    Shape* call_shape(Shape* callee, const std::string* literal) {
        if (callee == nullptr) {
            return none();
        }
        if (callee->callee_name == "require" && callee->instance != kNoInstance) {
            // The receiver of require is the argument, stored on the call below.
        }
        if (callee->resolves_child && literal != nullptr && callee->instance != kNoInstance) {
            // The child may be missing at runtime, so the result stays optional.
            if (const engine_core::LuaNode* child = FindChild(world_, callee->instance, *literal)) {
                Shape* shape = class_shape(child->class_name.empty() ? "DataModel" : child->class_name, child->id);
                shape->optional = true;
                return shape;
            }
            Shape* shape = class_shape("Instance", kNoInstance);
            shape->optional = true;
            return shape;
        }
        if (callee->class_from_arg && literal != nullptr && engine_core::lua_class_known(literal->c_str())) {
            return class_shape(*literal, kNoInstance);
        }
        if (callee->returns_list) {
            Shape* shape = type_shape(callee->result_type);
            Shape* list = fresh();
            list->returns_list = true;
            list->result_type = callee->result_type;
            list->class_name = shape->class_name;
            return list;
        }
        if (!callee->result_type.empty() && !callee->class_from_arg) {
            return type_shape(callee->result_type);
        }
        if (callee->callee_name == "require") {
            return none();
        }
        return none();
    }

    Shape* require_shape(Shape* argument) {
        if (argument == nullptr || argument->instance == kNoInstance) {
            return none();
        }
        const engine_core::LuaNode* node = FindNode(world_, argument->instance);
        if (node == nullptr || node->class_name != "ModuleScript") {
            return none();
        }
        engine_core::LuaShape exported;
        engine_core::lua_module_exports(node->source, node->id, world_, exported);
        Shape* adopted = adopt(exported);
        // The running module only records that a field is a function. The return
        // type is the first value of the first return written in its source.
        learn_returns(adopted, *node);
        return adopted;
    }

    // Reads `module` and copies each exported function's parameters and first
    // return onto the table `require` actually produced.
    void learn_returns(Shape* adopted, const engine_core::LuaNode& node) {
        if (adopted == nullptr || node.source.empty() || requiring_->size() >= 8) {
            return;
        }
        if (std::find(requiring_->begin(), requiring_->end(), node.id) != requiring_->end()) {
            return;
        }
        requiring_->push_back(node.id);
        const std::u32string text = Utf32(node.source);
        const Scan scan = Tokenize(text, static_cast<int>(text.size()));
        Resolver nested(scan.tokens, world_, node.id, true);
        nested.signing_ = true;
        nested.requiring_ = requiring_;
        nested.parse_until(static_cast<int>(scan.tokens.size()));
        nested.stamp_onto(adopted);
        requiring_->pop_back();
    }

    void stamp_onto(Shape* runtime) { stamp_shape(runtime, chunk_return_); }

    void stamp_shape(Shape* runtime, const Shape* source) {
        if (runtime == nullptr || source == nullptr) {
            return;
        }
        if (runtime->call && source->user_function) {
            copy_signature(runtime, source);
        }
        for (auto& field : runtime->fields) {
            const Shape* match = nullptr;
            for (const auto& candidate : source->fields) {
                if (candidate.first == field.first) {
                    match = candidate.second;
                    break;
                }
            }
            if (match != nullptr) {
                stamp_shape(field.second, match);
            }
        }
    }

    void copy_signature(Shape* runtime, const Shape* source) {
        runtime->user_function = true;
        runtime->params = source->params;
        for (Param& param : runtime->params) {
            param.decl = -1;
        }
        runtime->variadic = source->variadic;
        if (runtime->method) {
            runtime->self_type = source->self_type.empty() ? "table" : source->self_type;
        }
        if (!source->callee_name.empty()) {
            runtime->callee_owner = source->callee_owner;
            runtime->callee_name = source->callee_name;
        }
        auto set_type = [&](const std::string& type, bool annotated) {
            runtime->annotated_return = annotated;
            runtime->return_known = true;
            runtime->return_conflict = false;
            runtime->result_type = type;
        };
        auto set_nothing = [&]() {
            runtime->annotated_return = false;
            runtime->return_known = true;
            runtime->return_conflict = false;
            runtime->result_type.clear();
            runtime->body_closed = true;
        };
        if (source->saw_return && !source->first_type.empty()) {
            set_type(source->first_type, false);
            return;
        }
        if (source->saw_return && source->first_none) {
            set_nothing();
            return;
        }
        if (source->saw_return) {
            runtime->return_conflict = true;
            return;
        }
        if (source->return_known && !source->return_conflict) {
            if (source->result_type.empty()) {
                set_nothing();
            } else {
                set_type(source->result_type, source->annotated_return);
            }
            return;
        }
        if (source->annotated_return && !source->result_type.empty()) {
            set_type(source->result_type, true);
            return;
        }
        if (source->body_closed && !source->return_conflict) {
            set_nothing();
            return;
        }
        runtime->return_conflict = true;
    }

    void bind(std::string name, Shape* shape, int visible, int depth = -1, int decl = -1, bool parameter = false) {
        if (depth < 0) {
            depth = depth_;
        }
        Binding binding;
        binding.name = std::move(name);
        binding.shape = shape != nullptr ? shape : none();
        binding.depth = depth;
        binding.visible = visible;
        binding.decl = decl;
        binding.parameter = parameter;
        bindings_.push_back(std::move(binding));
    }

    void push_scope() { ++depth_; }

    void pop_scope() {
        if (depth_ > 0) {
            --depth_;
        }
        if (retain_) {
            for (Binding& binding : bindings_) {
                if (binding.depth > depth_ && binding.end < 0) {
                    binding.end = i_;
                }
            }
            return;
        }
        while (!bindings_.empty() && bindings_.back().depth > depth_) {
            bindings_.pop_back();
        }
    }

    HoverInfo describe(int code_index);

    void parse_until(int end) {
        limit_ = end;
        i_ = 0;
        int spins = 0;
        while (i_ < limit_ && spins < end + 8) {
            const int before = i_;
            if (is(Token::Semi)) {
                advance();
            } else {
                parse_stmt();
            }
            if (i_ == before) {
                advance();
            }
            ++spins;
        }
    }

    Shape* receiver(int expr_end) {
        const int start = primary_start(expr_end);
        if (start < 0 || start >= expr_end) {
            return none();
        }
        const int saved_i = i_;
        const int saved_limit = limit_;
        i_ = start;
        limit_ = expr_end;
        Shape* shape = parse_expr();
        i_ = saved_i;
        limit_ = saved_limit;
        return shape != nullptr ? shape : none();
    }

    // Parameters of the callback at `argument` of the call whose '(' is `open`.
    // A colon call's callback is argument 0. A dot call passes self first.
    std::vector<Param> callback_params(int open, int argument) {
        if (open < 0) {
            return {};
        }
        Shape* callee = receiver(open);
        if (callee == nullptr || !callee->callback_arg || callee->signal_params.empty()) {
            return {};
        }
        const int expected = IsColonCall(tokens_, open) ? 0 : 1;
        if (argument != expected) {
            return {};
        }
        return callee->signal_params;
    }

    void add_names(std::string_view prefix, int at, std::vector<CompletionItem>& out) {
        std::vector<std::string> seen;
        auto take = [&](const std::string& name, const std::string& detail, bool call, const Shape* shape) {
            if (!prefix.empty() && !StartsWith(name, prefix)) {
                return;
            }
            if (std::find(seen.begin(), seen.end(), name) != seen.end()) {
                return;
            }
            seen.push_back(name);
            CompletionItem item;
            item.name = name;
            item.detail = detail;
            item.call = call;
            AttachDocs(item, shape);
            out.push_back(std::move(item));
        };
        for (int index = static_cast<int>(bindings_.size()) - 1; index >= 0; --index) {
            const Binding& binding = bindings_[static_cast<std::size_t>(index)];
            if (binding.depth > depth_ || binding.visible > at) {
                continue;
            }
            const Shape* shape = binding.shape;
            std::string detail = "local";
            if (shape != nullptr && !shape->class_name.empty()) {
                detail = shape->class_name;
            } else if (shape != nullptr && shape->value_type == "function" &&
                       (!shape->params.empty() || shape->variadic)) {
                detail = FormatParams(shape->params, shape->variadic);
            } else if (shape != nullptr && !shape->value_type.empty()) {
                detail = shape->value_type;
            }
            take(binding.name, detail, shape != nullptr && shape->call, shape);
        }
        std::vector<engine_core::LuaSymbol> globals;
        engine_core::lua_library_globals(globals);
        for (const engine_core::LuaSymbol& symbol : globals) {
            if (!prefix.empty() && !StartsWith(symbol.name, prefix)) {
                continue;
            }
            Shape* documented = nullptr;
            if (symbol.type_name == "table") {
                documented = fresh();
                documented->library = symbol.name;
            } else {
                documented = value_shape(symbol.type_name, symbol.call);
                if (symbol.call) {
                    documented->callee_name = symbol.name;
                }
            }
            take(symbol.name, symbol.type_name == "table" ? "library" : symbol.type_name, symbol.call, documented);
        }
        take("game", "DataModel", false, lookup_global("game"));
        if (script_global_) {
            std::string script_type = "Script";
            if (script_id_ != 0) {
                if (const engine_core::LuaNode* script = FindNode(world_, script_id_)) {
                    if (!script->class_name.empty()) {
                        script_type = script->class_name;
                    }
                }
            }
            take("script", script_type, false, lookup_global("script"));
        }
        for (const char* keyword : kKeywords) {
            take(keyword, "keyword", false, nullptr);
        }
    }

    // The shape hover would describe for this member. A root's Parent has no
    // instance to point at, so the field's own type is used for the explanation.
    Shape* documented_member(Shape* owner, const std::string& name, const std::string& type_name) {
        Shape* documented = member_of(owner, name);
        if (documented != nullptr && ShapeUseful(documented)) {
            return documented;
        }
        documented = type_shape(type_name);
        if (owner != nullptr) {
            if (!owner->class_name.empty()) {
                documented->callee_owner = owner->class_name;
            } else if (!owner->library.empty()) {
                documented->callee_owner = owner->library;
            } else {
                documented->callee_owner = owner->value_type;
            }
        }
        documented->callee_name = name;
        return documented;
    }

    void add_members(Shape* shape, bool colon, std::string_view prefix, std::vector<CompletionItem>& out) {
        if (shape == nullptr) {
            return;
        }
        std::vector<std::string> seen;
        auto take = [&](const std::string& name, const std::string& detail, bool call, bool method, const Shape* documented) {
            if (colon && !method) {
                return;
            }
            if (!prefix.empty() && !StartsWith(name, prefix)) {
                return;
            }
            if (std::find(seen.begin(), seen.end(), name) != seen.end()) {
                return;
            }
            seen.push_back(name);
            CompletionItem item;
            item.name = name;
            item.detail = detail;
            item.call = call;
            AttachDocs(item, documented);
            out.push_back(std::move(item));
        };
        if (!shape->class_name.empty()) {
            std::vector<engine_core::LuaField> fields;
            engine_core::lua_class_members(shape->class_name.c_str(), fields);
            for (const engine_core::LuaField& field : fields) {
                if (field.name == nullptr) {
                    continue;
                }
                const char* type_name = field.type_name != nullptr ? field.type_name : "";
                take(field.name, field.method ? "function" : type_name, field.method, field.method,
                     documented_member(shape, field.name, type_name));
            }
        }
        if (!shape->library.empty()) {
            std::vector<engine_core::LuaSymbol> symbols;
            engine_core::lua_library_members(shape->library, symbols);
            for (const engine_core::LuaSymbol& symbol : symbols) {
                take(symbol.name, symbol.type_name, symbol.call, symbol.method,
                     documented_member(shape, symbol.name, symbol.type_name));
            }
        }
        if (shape->value_type == "string" || shape->value_type == "vector") {
            std::vector<engine_core::LuaSymbol> symbols;
            engine_core::lua_value_members(shape->value_type, symbols);
            for (const engine_core::LuaSymbol& symbol : symbols) {
                take(symbol.name, symbol.type_name, symbol.call, symbol.method,
                     documented_member(shape, symbol.name, symbol.type_name));
            }
        }
        for (const auto& field : shape->fields) {
            const Shape* child = field.second;
            const bool call = child != nullptr && child->call;
            const bool method = child != nullptr && child->method;
            // `function obj:name` is offered after ':'. A '.' completion leaves it out.
            if (!colon && method) {
                continue;
            }
            std::string detail = "field";
            if (child != nullptr && !child->class_name.empty()) {
                detail = child->class_name;
            } else if (child != nullptr && !child->value_type.empty()) {
                detail = child->value_type;
            }
            take(field.first, detail, call, method, child);
        }
    }

    // `function` in a Connect argument becomes `function(dt)`, and the parameter
    // list of that function offers the same names.
    void complete_callback(int index, CompletionList& list) {
        const CallSlot slot = CallArgumentAt(tokens_, index);
        if (!slot.found) {
            return;
        }
        if (AnonymousFunctionOpen(tokens_, slot.open)) {
            const CallSlot outer = CallArgumentAt(tokens_, slot.open - 1);
            if (!outer.found) {
                return;
            }
            const std::vector<Param> params = callback_params(outer.open, outer.argument);
            if (params.empty()) {
                return;
            }
            std::vector<std::string> used;
            for (int cursor = slot.open + 1; cursor < index && cursor < static_cast<int>(tokens_.size()); ++cursor) {
                if (tokens_[static_cast<std::size_t>(cursor)].kind == Token::Name) {
                    used.push_back(tokens_[static_cast<std::size_t>(cursor)].text);
                }
            }
            std::vector<CompletionItem> extra;
            for (const Param& param : params) {
                if (std::find(used.begin(), used.end(), param.name) != used.end()) {
                    continue;
                }
                if (!list.prefix.empty() && !StartsWith(param.name, list.prefix)) {
                    continue;
                }
                bool seen = false;
                for (const CompletionItem& item : list.items) {
                    if (item.name == param.name) {
                        seen = true;
                        break;
                    }
                }
                if (seen) {
                    continue;
                }
                CompletionItem item;
                item.name = param.name;
                item.detail = param.type_name.empty() ? "parameter" : param.type_name;
                extra.push_back(std::move(item));
            }
            if (!extra.empty()) {
                list.items.insert(list.items.begin(), extra.begin(), extra.end());
            }
            return;
        }
        const std::vector<Param> params = callback_params(slot.open, slot.argument);
        if (params.empty()) {
            return;
        }
        std::string detail;
        std::string snippet = "function(";
        for (std::size_t param_index = 0; param_index < params.size(); ++param_index) {
            if (param_index > 0) {
                snippet += ", ";
                detail += ", ";
            }
            snippet += params[param_index].name;
            detail += params[param_index].type_name.empty() ? "parameter" : params[param_index].type_name;
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
        for (const CompletionItem& item : list.items) {
            if (item.name == snippet) {
                return;
            }
        }
        CompletionItem item;
        item.name = std::move(snippet);
        item.detail = std::move(detail);
        item.snippet = true;
        list.items.insert(list.items.begin(), std::move(item));
    }

    // A call of a function written in this source lists that function's parameters.
    // Names that fit the active parameter move to the front.
    void complete_signature(int index, CompletionList& list) {
        const CallSlot slot = CallArgumentAt(tokens_, index);
        if (!slot.found || AnonymousFunctionOpen(tokens_, slot.open)) {
            return;
        }
        Shape* callee = receiver(slot.open);
        if (callee == nullptr || (callee->params.empty() && !callee->variadic)) {
            return;
        }
        list.signature = FormatParams(callee->params, callee->variadic);
        if (list.items.empty() || slot.argument < 0 || slot.argument >= static_cast<int>(callee->params.size())) {
            return;
        }
        const std::string expected = CoreType(callee->params[static_cast<std::size_t>(slot.argument)].type_name);
        if (expected.empty()) {
            return;
        }
        std::stable_partition(list.items.begin(), list.items.end(), [&](const CompletionItem& item) {
            return AcceptsType(expected, item);
        });
    }

private:
    struct Binding {
        std::string name;
        Shape* shape = nullptr;
        int depth = 0;
        int visible = 0;
        // Token index where the binding leaves scope. -1 while it is still open.
        int end = -1;
        // Token index of the name that introduces the binding. -1 when there is none.
        int decl = -1;
        bool parameter = false;
    };

    bool at_end() const { return i_ >= limit_; }
    const Token& cur() const { return tokens_[static_cast<std::size_t>(i_)]; }
    void advance() {
        if (i_ < limit_) {
            ++i_;
        }
    }
    bool is(Token::Kind kind) const { return !at_end() && cur().kind == kind; }
    bool is_kw(const char* word) const { return is(Token::Keyword) && cur().text == word; }
    bool is_name() const { return is(Token::Name); }
    bool next_colon() const { return i_ + 1 < limit_ && tokens_[static_cast<std::size_t>(i_ + 1)].kind == Token::Colon; }
    bool consume(Token::Kind kind) {
        if (!is(kind)) {
            return false;
        }
        advance();
        return true;
    }
    std::string take_name() {
        std::string name = cur().text;
        advance();
        return name;
    }

    // The type written after `:`, up to the next parameter or the closing ')'.
    std::string read_type() {
        std::string text;
        int angle = 0;
        int paren = 0;
        int brace = 0;
        int brack = 0;
        while (!at_end()) {
            // A return annotation is followed by the function body. A name there is
            // a statement, unless the type is still open (`string |`, `Foo.`, `->`).
            const bool type_continues = text.empty() || (text.size() >= 2 && text.compare(text.size() - 2, 2, "->") == 0) ||
                                        text.back() == '.' || text.back() == ':' || text.back() == '<' || text.back() == '|' ||
                                        text.back() == '&' || text.back() == '(' || text.back() == ',';
            if (angle == 0 && paren == 0 && brace == 0 && brack == 0 &&
                (is(Token::Comma) || is(Token::RParen) || is(Token::Eq) ||
                 ((is(Token::Name) || is(Token::Keyword)) && !type_continues))) {
                break;
            }
            const Token& token = cur();
            if (token.kind == Token::Op && token.text == "<") {
                ++angle;
            } else if (token.kind == Token::Op && token.text == ">") {
                if (angle > 0) {
                    --angle;
                }
            } else if (token.kind == Token::LParen) {
                ++paren;
            } else if (token.kind == Token::RParen) {
                if (paren == 0) {
                    break;
                }
                --paren;
            } else if (token.kind == Token::LBrace) {
                ++brace;
            } else if (token.kind == Token::RBrace) {
                if (brace > 0) {
                    --brace;
                }
            } else if (token.kind == Token::LBrack) {
                ++brack;
            } else if (token.kind == Token::RBrack) {
                if (brack > 0) {
                    --brack;
                }
            }
            const bool tight = token.text == "?" || token.text == "," || token.text == ">" || token.text == ")" ||
                               token.text == "]" || token.text == "}";
            if (!text.empty()) {
                const char last = text.back();
                const bool after_open =
                    last == '<' || last == '(' || last == '[' || last == '{' || last == '.' || last == ':';
                if (!after_open && !tight) {
                    text.push_back(' ');
                }
            }
            text += token.text;
            advance();
        }
        return text;
    }

    Shape* annotation_shape(const std::string& type_text) {
        if (type_text.empty()) {
            return none();
        }
        const std::string core = CoreType(type_text);
        if (IsIdent(core)) {
            return type_shape(core);
        }
        return value_shape(type_text);
    }

    bool block_end() const { return is_kw("end") || is_kw("else") || is_kw("elseif") || is_kw("until"); }

    void parse_block() {
        while (!at_end() && !block_end()) {
            const int before = i_;
            if (is(Token::Semi)) {
                advance();
                continue;
            }
            parse_stmt();
            if (i_ == before) {
                advance();
            }
        }
    }

    void parse_stmt() {
        if (is_kw("local")) {
            parse_local();
            return;
        }
        if (is_kw("function")) {
            advance();
            parse_function(false, false);
            return;
        }
        if (is_kw("do")) {
            advance();
            push_scope();
            parse_block();
            if (at_end()) {
                return;
            }
            pop_scope();
            if (is_kw("end")) {
                advance();
            }
            return;
        }
        if (is_kw("if")) {
            advance();
            parse_expr();
            if (at_end()) {
                return;
            }
            if (is_kw("then")) {
                advance();
            }
            push_scope();
            parse_block();
            if (at_end()) {
                return;
            }
            pop_scope();
            while (is_kw("elseif")) {
                advance();
                parse_expr();
                if (is_kw("then")) {
                    advance();
                }
                push_scope();
                parse_block();
                if (at_end()) {
                    return;
                }
                pop_scope();
            }
            if (is_kw("else")) {
                advance();
                push_scope();
                parse_block();
                if (at_end()) {
                    return;
                }
                pop_scope();
            }
            if (is_kw("end")) {
                advance();
            }
            return;
        }
        if (is_kw("while")) {
            advance();
            parse_expr();
            if (is_kw("do")) {
                advance();
            }
            if (at_end()) {
                return;
            }
            push_scope();
            parse_block();
            if (at_end()) {
                return;
            }
            pop_scope();
            if (is_kw("end")) {
                advance();
            }
            return;
        }
        if (is_kw("for")) {
            advance();
            struct LoopName {
                std::string name;
                int decl = -1;
            };
            std::vector<LoopName> names;
            if (is_name()) {
                LoopName item;
                item.decl = i_;
                item.name = take_name();
                names.push_back(std::move(item));
                while (consume(Token::Comma) && is_name()) {
                    LoopName next;
                    next.decl = i_;
                    next.name = take_name();
                    names.push_back(std::move(next));
                }
            }
            bool numeric = false;
            if (consume(Token::Eq)) {
                numeric = true;
                parse_expr_list();
            } else if (is_kw("in")) {
                advance();
                parse_expr_list();
            }
            if (at_end()) {
                return;
            }
            if (is_kw("do")) {
                advance();
            }
            push_scope();
            for (const LoopName& name : names) {
                bind(name.name, numeric ? value_shape("number") : none(), i_, -1, name.decl, false);
            }
            parse_block();
            if (at_end()) {
                return;
            }
            pop_scope();
            if (is_kw("end")) {
                advance();
            }
            return;
        }
        if (is_kw("repeat")) {
            advance();
            push_scope();
            parse_block();
            if (is_kw("until")) {
                advance();
                const bool cut_before = cut_;
                if (at_end()) {
                    cut_ = true;
                } else {
                    parse_expr();
                }
                // Body locals stay visible while the condition is still being typed.
                if (cut_ && !cut_before) {
                    return;
                }
                pop_scope();
                return;
            }
            if (!at_end()) {
                pop_scope();
            }
            return;
        }
        if (is_kw("return")) {
            advance();
            std::vector<Shape*> values;
            if (!at_end() && !block_end()) {
                values = parse_expr_list();
            }
            note_return(values);
            if (signing_ && functions_.empty() && !saw_chunk_return_) {
                saw_chunk_return_ = true;
                chunk_return_ = values.empty() || values[0] == nullptr ? none() : values[0];
            }
            return;
        }
        if (is_kw("break") || is_kw("continue")) {
            advance();
            return;
        }
        parse_assign();
    }

    void parse_local() {
        advance();
        if (is_kw("function")) {
            advance();
            parse_function(false, true);
            return;
        }
        const bool cut_before = cut_;
        struct Slot {
            std::string name;
            std::string type_name;
            int decl = -1;
        };
        std::vector<Slot> slots;
        while (is_name()) {
            Slot slot;
            slot.decl = i_;
            slot.name = take_name();
            if (is(Token::Colon) && !next_colon()) {
                advance();
                if (at_end()) {
                    cut_ = true;
                } else {
                    slot.type_name = read_type();
                }
            }
            slots.push_back(std::move(slot));
            if (!consume(Token::Comma)) {
                break;
            }
            if (at_end()) {
                cut_ = true;
                break;
            }
        }
        std::vector<Shape*> inits;
        if (consume(Token::Eq)) {
            if (at_end()) {
                cut_ = true;
            } else {
                inits = parse_expr_list();
            }
        }
        // A local is not visible in its own initializer, and not visible at all
        // until the statement can stand on its own.
        if (cut_ && !cut_before) {
            return;
        }
        const int visible = i_;
        for (std::size_t index = 0; index < slots.size(); ++index) {
            Shape* shape = index < inits.size() && inits[index] != nullptr ? inits[index] : nullptr;
            if (shape == nullptr) {
                shape = annotation_shape(slots[index].type_name);
            }
            bind(slots[index].name, shape, visible, -1, slots[index].decl, false);
        }
    }

    void attach_function(Shape* owner, const std::string& name, Shape* fn) {
        if (owner == nullptr || fn == nullptr || name.empty() || !ShapeUseful(owner)) {
            return;
        }
        for (auto& field : owner->fields) {
            if (field.first == name) {
                field.second = fn;
                return;
            }
        }
        owner->fields.emplace_back(name, fn);
    }

    void note_return(const std::vector<Shape*>& values) {
        if (functions_.empty()) {
            return;
        }
        Shape* fn = functions_.back();
        if (fn == nullptr) {
            return;
        }
        if (!fn->saw_return) {
            fn->saw_return = true;
            if (values.empty()) {
                fn->first_none = true;
            } else {
                fn->first_type = DescribeType(values[0]);
            }
        }
        if (fn->annotated_return) {
            return;
        }
        std::string type;
        for (std::size_t index = 0; index < values.size(); ++index) {
            const std::string part = DescribeType(values[index]);
            if (part.empty()) {
                fn->return_known = true;
                fn->return_conflict = true;
                fn->result_type.clear();
                return;
            }
            if (index > 0) {
                type += ", ";
            }
            type += part;
        }
        if (fn->return_conflict) {
            return;
        }
        if (!fn->return_known) {
            fn->return_known = true;
            fn->result_type = std::move(type);
            return;
        }
        if (fn->result_type != type) {
            fn->return_conflict = true;
            fn->result_type.clear();
        }
    }

    Shape* parse_function(bool expression, bool local_name) {
        Shape* fn = value_shape("function", true);
        fn->user_function = true;
        if (!expression && is_name()) {
            const int name_at = i_;
            std::string first = take_name();
            if (consume(Token::Dot)) {
                if (is_name()) {
                    const int method_at = i_;
                    std::string method = take_name();
                    Shape* owner = lookup(first);
                    attach_function(owner, method, fn);
                    fn->callee_owner = std::move(first);
                    fn->callee_name = method;
                    // Visible only on the declaration. A later `owner.name` sees the field.
                    bind(std::move(method), fn, limit_ + 1, 0, method_at, false);
                }
            } else if (is(Token::Colon) && !next_colon()) {
                advance();
                if (is_name()) {
                    const int method_at = i_;
                    std::string method = take_name();
                    Shape* owner = lookup(first);
                    fn->method = true;
                    fn->self_type = DescribeType(owner);
                    attach_function(owner, method, fn);
                    fn->callee_owner = std::move(first);
                    fn->callee_name = method;
                    bind(method, fn, limit_ + 1, 0, method_at, false);
                }
            } else if (local_name) {
                bind(std::move(first), fn, i_, -1, name_at, false);
            } else {
                // `function name` assigns a global, including when it is nested in another block.
                bind(std::move(first), fn, i_, 0, name_at, false);
            }
        } else if (expression && is_name()) {
            advance();
        }
        push_scope();
        bool closed = false;
        if (consume(Token::LParen)) {
            while (!at_end() && !is(Token::RParen)) {
                if (is(Token::Ellipsis)) {
                    fn->variadic = true;
                    advance();
                    break;
                }
                if (!is_name()) {
                    break;
                }
                Param param;
                param.decl = i_;
                param.name = take_name();
                if (is(Token::Colon) && !next_colon()) {
                    advance();
                    if (at_end()) {
                        cut_ = true;
                    } else {
                        param.type_name = read_type();
                    }
                }
                fn->params.push_back(std::move(param));
                if (!consume(Token::Comma)) {
                    break;
                }
                if (at_end()) {
                    break;
                }
            }
            closed = consume(Token::RParen);
        }
        if (closed && is(Token::Colon) && !next_colon()) {
            advance();
            if (at_end()) {
                cut_ = true;
            } else {
                fn->result_type = read_type();
                fn->annotated_return = true;
                fn->return_known = true;
            }
        }
        // Parameters belong to the body. They are not in scope while the
        // parameter list itself is still being typed.
        const int visible = closed ? i_ : limit_ + 1;
        for (const Param& param : fn->params) {
            bind(param.name, annotation_shape(param.type_name), visible, -1, param.decl, true);
        }
        if (!closed && at_end()) {
            cut_ = true;
        }
        functions_.push_back(fn);
        parse_block();
        if (!functions_.empty() && functions_.back() == fn) {
            functions_.pop_back();
        }
        if (is_kw("end")) {
            fn->body_closed = true;
            advance();
            pop_scope();
        } else if (at_end()) {
            cut_ = true;
        } else {
            pop_scope();
        }
        return fn;
    }

    void parse_assign() {
        const int start = i_;
        Shape* first = parse_expr();
        if (!is(Token::Comma) && !is(Token::Eq)) {
            (void)first;
            return;
        }
        int eq = -1;
        int depth = 0;
        for (int index = i_; index < limit_; ++index) {
            const Token::Kind kind = tokens_[static_cast<std::size_t>(index)].kind;
            if (kind == Token::LParen || kind == Token::LBrack || kind == Token::LBrace) {
                ++depth;
            } else if (kind == Token::RParen || kind == Token::RBrack || kind == Token::RBrace) {
                if (depth > 0) {
                    --depth;
                }
            } else if (depth == 0 && kind == Token::Eq) {
                eq = index;
                break;
            } else if (depth == 0 && (kind == Token::Keyword || kind == Token::Semi)) {
                break;
            }
        }
        if (eq < 0) {
            return;
        }
        struct Assigned {
            std::string name;
            int decl = -1;
        };
        std::vector<Assigned> names;
        int cursor = start;
        while (cursor < eq) {
            const Token& token = tokens_[static_cast<std::size_t>(cursor)];
            if (token.kind == Token::Name &&
                (cursor + 1 == eq || tokens_[static_cast<std::size_t>(cursor + 1)].kind == Token::Comma)) {
                Assigned item;
                item.name = token.text;
                item.decl = cursor;
                names.push_back(std::move(item));
                cursor += cursor + 1 < eq && tokens_[static_cast<std::size_t>(cursor + 1)].kind == Token::Comma ? 2 : 1;
                continue;
            }
            break;
        }
        const bool cut_before = cut_;
        i_ = eq + 1;
        if (at_end()) {
            cut_ = true;
            return;
        }
        std::vector<Shape*> values = parse_expr_list();
        if (cut_ && !cut_before) {
            return;
        }
        if (signing_ && names.empty() && !values.empty()) {
            assign_field(start, eq, values[0]);
        }
        for (std::size_t index = 0; index < names.size(); ++index) {
            Shape* shape = index < values.size() ? values[index] : none();
            bool updated = false;
            for (int slot = static_cast<int>(bindings_.size()) - 1; slot >= 0; --slot) {
                Binding& binding = bindings_[static_cast<std::size_t>(slot)];
                if (binding.name != names[index].name || binding.depth > depth_ || binding.visible > i_ || binding.end >= 0) {
                    continue;
                }
                if (retain_) {
                    // A later assignment must not change the type of earlier uses.
                    const int depth = binding.depth;
                    binding.end = i_;
                    bind(names[index].name, shape, i_, depth, names[index].decl, false);
                } else {
                    binding.shape = shape != nullptr ? shape : none();
                }
                updated = true;
                break;
            }
            if (!updated) {
                bind(names[index].name, shape, i_, 0, names[index].decl, false);
            }
        }
    }

    std::vector<Shape*> parse_expr_list() {
        std::vector<Shape*> values;
        if (at_end()) {
            cut_ = true;
            return values;
        }
        values.push_back(parse_expr());
        while (consume(Token::Comma)) {
            if (at_end()) {
                cut_ = true;
                break;
            }
            values.push_back(parse_expr());
        }
        return values;
    }

    Shape* parse_expr() {
        if (at_end()) {
            return none();
        }
        return parse_binary(1);
    }

    int precedence() const {
        if (at_end()) {
            return 0;
        }
        if (is_kw("or")) {
            return 1;
        }
        if (is_kw("and")) {
            return 2;
        }
        if (!is(Token::Op)) {
            return 0;
        }
        const std::string& op = cur().text;
        if (op == "==" || op == "~=" || op == "<" || op == ">" || op == "<=" || op == ">=") {
            return 3;
        }
        if (op == "..") {
            return 4;
        }
        if (op == "+" || op == "-") {
            return 5;
        }
        if (op == "*" || op == "/" || op == "%") {
            return 6;
        }
        return 0;
    }

    Shape* parse_binary(int minimum) {
        Shape* left = parse_unary();
        while (!at_end()) {
            const int level = precedence();
            if (level < minimum) {
                break;
            }
            const std::string op = cur().text;
            advance();
            if (at_end()) {
                cut_ = true;
                break;
            }
            Shape* right = parse_binary(op == ".." ? level : level + 1);
            if (op == "..") {
                left = value_shape("string");
            } else if (op == "and" || op == "or") {
                left = right != nullptr ? right : left;
            } else if (level == 3) {
                left = value_shape("boolean");
            } else {
                left = value_shape("number");
            }
        }
        return left;
    }

    Shape* parse_unary() {
        if (is_kw("not")) {
            advance();
            if (at_end()) {
                cut_ = true;
                return value_shape("boolean");
            }
            parse_unary();
            return value_shape("boolean");
        }
        if (is(Token::Op) && (cur().text == "-" || cur().text == "#")) {
            advance();
            if (at_end()) {
                cut_ = true;
                return value_shape("number");
            }
            parse_unary();
            return value_shape("number");
        }
        return parse_simple();
    }

    Shape* parse_simple() {
        Shape* shape = parse_primary();
        while (!at_end()) {
            if (is(Token::Colon) && next_colon()) {
                advance();
                advance();
                if (is_name()) {
                    const std::string name = take_name();
                    shape = type_shape(name);
                }
                continue;
            }
            if (is(Token::Dot) || is(Token::Colon)) {
                advance();
                if (!is_name()) {
                    if (at_end()) {
                        cut_ = true;
                    }
                    break;
                }
                shape = member_of(shape, take_name());
                continue;
            }
            if (is(Token::LParen)) {
                const int before = i_;
                shape = parse_call(shape);
                if (i_ == before) {
                    break;
                }
                continue;
            }
            if (is(Token::LBrack)) {
                advance();
                if (!at_end() && !is(Token::RBrack)) {
                    parse_expr();
                }
                if (!consume(Token::RBrack) && at_end()) {
                    cut_ = true;
                }
                if (shape != nullptr && shape->returns_list) {
                    shape = type_shape(shape->result_type);
                } else {
                    shape = none();
                }
                continue;
            }
            break;
        }
        return shape;
    }

    Shape* parse_primary() {
        if (is_name()) {
            const std::string name = take_name();
            return lookup(name);
        }
        if (is_kw("true") || is_kw("false")) {
            advance();
            return value_shape("boolean");
        }
        if (is_kw("nil")) {
            advance();
            return none();
        }
        if (is(Token::Number)) {
            advance();
            return value_shape("number");
        }
        if (is(Token::String)) {
            advance();
            return value_shape("string");
        }
        if (is_kw("function")) {
            advance();
            return parse_function(true, false);
        }
        if (is(Token::LParen)) {
            advance();
            Shape* inner = at_end() || is(Token::RParen) ? none() : parse_expr();
            if (!consume(Token::RParen) && at_end()) {
                cut_ = true;
            }
            return inner;
        }
        if (is(Token::LBrace)) {
            if (signing_) {
                return parse_table();
            }
            int depth = 0;
            do {
                if (is(Token::LBrace)) {
                    ++depth;
                } else if (is(Token::RBrace)) {
                    --depth;
                }
                advance();
            } while (!at_end() && depth > 0);
            if (depth > 0) {
                cut_ = true;
            }
            return value_shape("table");
        }
        return none();
    }

    Shape* parse_call(Shape* callee) {
        if (callee != nullptr && callee->callee_name == "require") {
            // Filled after the argument is parsed.
        }
        std::string literal;
        bool have_literal = false;
        Shape* first = nullptr;
        if (!consume(Token::LParen)) {
            return callee;
        }
        if (!is(Token::RParen) && !at_end()) {
            bool leading = true;
            for (;;) {
                if (leading && is(Token::String)) {
                    literal = cur().text;
                    have_literal = true;
                }
                Shape* argument = parse_expr();
                if (leading) {
                    first = argument;
                }
                leading = false;
                if (!consume(Token::Comma)) {
                    break;
                }
                if (at_end()) {
                    cut_ = true;
                    break;
                }
            }
        }
        if (!consume(Token::RParen) && at_end()) {
            cut_ = true;
        }
        if (callee != nullptr && callee->callee_name == "require") {
            return require_shape(first);
        }
        return call_shape(callee, have_literal ? &literal : nullptr);
    }

    bool callee_token(const Token& token) const { return IsCallPrefix(token); }

    int match_open(int close) const {
        Token::Kind open_kind = Token::LParen;
        const Token::Kind close_kind = tokens_[static_cast<std::size_t>(close)].kind;
        if (close_kind == Token::RBrack) {
            open_kind = Token::LBrack;
        } else if (close_kind == Token::RBrace) {
            open_kind = Token::LBrace;
        }
        int depth = 0;
        for (int index = close; index >= 0; --index) {
            const Token::Kind kind = tokens_[static_cast<std::size_t>(index)].kind;
            if (kind == close_kind) {
                ++depth;
            } else if (kind == open_kind) {
                --depth;
                if (depth == 0) {
                    return index;
                }
            }
        }
        return -1;
    }

    bool suffix_before(int name_index) const {
        if (name_index <= 0) {
            return false;
        }
        const Token& dot = tokens_[static_cast<std::size_t>(name_index - 1)];
        if (dot.kind == Token::Dot) {
            return true;
        }
        if (dot.kind != Token::Colon) {
            return false;
        }
        return name_index < 2 || tokens_[static_cast<std::size_t>(name_index - 2)].kind != Token::Colon;
    }

    int primary_start(int end) const {
        int cursor = end - 1;
        while (cursor >= 0) {
            const Token& token = tokens_[static_cast<std::size_t>(cursor)];
            if (token.kind == Token::RParen || token.kind == Token::RBrack || token.kind == Token::RBrace) {
                const int open = match_open(cursor);
                if (open < 0) {
                    break;
                }
                if (open > 0 && callee_token(tokens_[static_cast<std::size_t>(open - 1)])) {
                    cursor = open - 1;
                    continue;
                }
                return open;
            }
            if (token.kind == Token::Name && suffix_before(cursor)) {
                cursor -= 2;
                continue;
            }
            if (token.kind == Token::Name || token.kind == Token::String || token.kind == Token::Number ||
                token.kind == Token::LParen || token.kind == Token::LBrace ||
                (token.kind == Token::Keyword &&
                 (token.text == "true" || token.text == "false" || token.text == "nil" || token.text == "function"))) {
                return cursor;
            }
            break;
        }
        return cursor + 1;
    }

    const std::vector<Token>& tokens_;
    const std::vector<engine_core::LuaNode>& world_;
    std::uint32_t script_id_ = 0;
    bool script_global_ = true;
    std::vector<std::unique_ptr<Shape>> arena_;
    std::vector<Binding> bindings_;
    int i_ = 0;
    int limit_ = 0;
    int depth_ = 0;
    // The caret cut a declaration or expression off before it was complete.
    bool cut_ = false;
    // Hover keeps bindings after their scope ends so a name can be resolved later.
    bool retain_ = false;
    // When set, lookup resolves names as they are at this token.
    int hover_at_ = -1;
    // Module source keeps table fields so a required function's return can be read.
    bool signing_ = false;
    bool saw_chunk_return_ = false;
    Shape* chunk_return_ = nullptr;
    std::vector<std::uint32_t> requiring_storage_;
    std::vector<std::uint32_t>* requiring_ = &requiring_storage_;
    std::vector<Shape*> functions_;

    void assign_field(int start, int eq, Shape* value) {
        if (start < 0 || eq <= start) {
            return;
        }
        for (int index = start; index < eq; ++index) {
            if (tokens_[static_cast<std::size_t>(index)].kind == Token::Comma) {
                return;
            }
        }
        int cursor = start;
        if (tokens_[static_cast<std::size_t>(cursor)].kind != Token::Name) {
            return;
        }
        Shape* owner = lookup(tokens_[static_cast<std::size_t>(cursor)].text);
        ++cursor;
        if (cursor >= eq || tokens_[static_cast<std::size_t>(cursor)].kind != Token::Dot) {
            return;
        }
        while (cursor < eq) {
            if (tokens_[static_cast<std::size_t>(cursor)].kind != Token::Dot) {
                return;
            }
            ++cursor;
            if (cursor >= eq || tokens_[static_cast<std::size_t>(cursor)].kind != Token::Name) {
                return;
            }
            const std::string field = tokens_[static_cast<std::size_t>(cursor)].text;
            ++cursor;
            if (cursor >= eq) {
                attach_function(owner, field, value);
                return;
            }
            owner = member_of(owner, field);
        }
    }

    Shape* parse_table() {
        advance();
        Shape* table = value_shape("table");
        while (!at_end() && !is(Token::RBrace)) {
            if (is(Token::LBrack)) {
                advance();
                if (!at_end() && !is(Token::RBrack)) {
                    parse_expr();
                }
                consume(Token::RBrack);
                if (consume(Token::Eq) && !at_end() && !is(Token::RBrace) && !is(Token::Comma) && !is(Token::Semi)) {
                    parse_expr();
                }
            } else if (is_name() && i_ + 1 < limit_ && tokens_[static_cast<std::size_t>(i_ + 1)].kind == Token::Eq) {
                const std::string name = take_name();
                advance();
                Shape* value = at_end() ? none() : parse_expr();
                attach_function(table, name, value);
            } else {
                const int before = i_;
                parse_expr();
                if (i_ == before) {
                    advance();
                }
            }
            if (is(Token::Comma) || is(Token::Semi)) {
                advance();
                continue;
            }
            break;
        }
        if (!consume(Token::RBrace) && at_end()) {
            cut_ = true;
        }
        return table;
    }
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

std::string CallbackType(const Shape* shape) {
    if (shape == nullptr || shape->signal_params.empty()) {
        return "function";
    }
    std::string out = "(";
    for (std::size_t index = 0; index < shape->signal_params.size(); ++index) {
        if (index > 0) {
            out += ", ";
        }
        out += shape->signal_params[index].name;
        if (!shape->signal_params[index].type_name.empty()) {
            out += ": ";
            out += shape->signal_params[index].type_name;
        }
    }
    out += ") -> ()";
    return out;
}

std::string QualifiedName(const Shape* shape, const std::string& token) {
    if (shape == nullptr) {
        return token;
    }
    if (!shape->callee_owner.empty() && !shape->callee_name.empty() && (shape->call || shape->user_function || shape->method)) {
        return shape->callee_owner + (shape->method ? ":" : ".") + shape->callee_name;
    }
    if (shape->user_function) {
        return token;
    }
    if (!shape->callee_name.empty() && shape->call) {
        return shape->callee_name;
    }
    return token;
}

Written DescribeSymbol(const Shape* shape, const std::string& name, bool bound, bool parameter) {
    Written info;
    if (shape == nullptr) {
        return info;
    }
    engine_core::LuaDoc doc;
    if (!shape->user_function && !shape->callee_name.empty()) {
        doc = engine_core::lua_symbol_doc(shape->callee_owner, shape->callee_name);
    }
    if (!doc.found && !shape->library.empty()) {
        doc = engine_core::lua_symbol_doc("", shape->library);
    }

    if (!shape->library.empty() && !shape->call && !shape->user_function) {
        info.found = true;
        info.title = name;
        info.detail = "library";
        if (doc.found) {
            info.summary = doc.summary;
        }
        return info;
    }

    if (shape->call || shape->user_function) {
        std::vector<std::pair<std::string, std::string>> params;
        bool variadic = false;
        if (shape->user_function) {
            if (shape->method) {
                params.emplace_back("self", shape->self_type);
            }
            for (const Param& param : shape->params) {
                params.emplace_back(param.name, param.type_name);
            }
            variadic = shape->variadic;
        } else if (doc.found) {
            std::size_t first = 0;
            if (shape->method && shape->callee_owner == "string" && !doc.params.empty() &&
                doc.params[0].type_name == "string") {
                first = 1;
            }
            for (std::size_t index = first; index < doc.params.size(); ++index) {
                std::string type_name = doc.params[index].type_name;
                if (shape->callback_arg && doc.params[index].name == "callback") {
                    type_name = CallbackType(shape);
                }
                params.emplace_back(doc.params[index].name, std::move(type_name));
            }
            variadic = doc.variadic;
        }
        std::string ret;
        bool known = false;
        bool nothing = false;
        if (shape->user_function && shape->annotated_return) {
            if (!shape->result_type.empty()) {
                known = true;
                ret = shape->result_type;
            }
        } else if (shape->user_function && shape->return_known && !shape->return_conflict) {
            known = true;
            ret = shape->result_type;
            nothing = ret.empty();
        } else if (shape->user_function && shape->body_closed && !shape->return_conflict) {
            known = true;
            nothing = true;
        } else if (shape->return_known && !shape->result_type.empty()) {
            known = true;
            ret = shape->result_type;
        } else if (doc.found && doc.return_unknown) {
            known = false;
        } else if (doc.found && doc.returns_nothing) {
            known = true;
            nothing = true;
        } else if (doc.found && !doc.return_type.empty()) {
            known = true;
            ret = doc.return_type;
        } else if (shape->returns_list) {
            known = true;
            ret = shape->result_type.empty() ? "table" : "{" + shape->result_type + "}";
        } else if (!shape->result_type.empty() && shape->result_type != "nil") {
            known = true;
            ret = shape->result_type;
        } else if (shape->result_type == "nil") {
            known = true;
            nothing = true;
        }
        info.found = true;
        info.title = "function " + QualifiedName(shape, name) + JoinParams(params, variadic);
        if (known && !ret.empty()) {
            info.title += ": ";
            info.title += ret;
            info.returns = std::move(ret);
        }
        if (nothing) {
            info.detail = "returns nothing";
            info.returns = "returns nothing";
        }
        if (doc.found && !shape->user_function) {
            info.summary = doc.summary;
        }
        return info;
    }

    if (!bound && !ShapeUseful(shape)) {
        return info;
    }
    info.found = true;
    const std::string type_name = ShownType(shape);
    if (!type_name.empty() && type_name != "function") {
        info.title = name + ": " + type_name;
    } else if (type_name == "function") {
        info.title = name + ": function";
    } else {
        info.title = name;
        info.detail = parameter ? "parameter" : "local";
    }
    if (doc.found) {
        info.summary = doc.summary;
    }
    return info;
}

void AttachDocs(CompletionItem& item, const Shape* shape) {
    if (shape == nullptr) {
        return;
    }
    const Written written = DescribeSymbol(shape, item.name, true, false);
    if (written.summary.empty() && written.returns.empty()) {
        return;
    }
    item.title = written.title;
    item.summary = written.summary;
    item.returns = written.returns;
}

HoverInfo Resolver::describe(int code_index) {
    int token_index = -1;
    for (int index = 0; index < static_cast<int>(tokens_.size()); ++index) {
        const Token& token = tokens_[static_cast<std::size_t>(index)];
        if (code_index >= token.begin && code_index < token.end) {
            token_index = index;
            break;
        }
    }
    if (token_index < 0) {
        return {};
    }
    const Token& token = tokens_[static_cast<std::size_t>(token_index)];
    HoverInfo info;
    info.begin = token.begin;
    info.end = token.end;
    if (token.kind == Token::Keyword) {
        if (token.text == "true" || token.text == "false") {
            info.found = true;
            info.title = token.text + ": boolean";
            return info;
        }
        if (token.text == "nil") {
            info.found = true;
            info.title = "nil";
            return info;
        }
        return {};
    }
    if (token.kind != Token::Name) {
        return {};
    }

    const bool cast = token_index >= 2 && tokens_[static_cast<std::size_t>(token_index - 1)].kind == Token::Colon &&
                      tokens_[static_cast<std::size_t>(token_index - 2)].kind == Token::Colon;
    bool annotation = cast;
    if (!annotation && token_index > 0 && tokens_[static_cast<std::size_t>(token_index - 1)].kind == Token::Colon) {
        const int colon = token_index - 1;
        if (AnnotationAt(tokens_, colon)) {
            annotation = true;
        } else if (colon > 0 && tokens_[static_cast<std::size_t>(colon - 1)].kind == Token::RParen) {
            const int open = match_open(colon - 1);
            annotation = open >= 0 && IsFunctionParen(tokens_, open);
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
        return info;
    }

    retain_ = true;
    parse_until(static_cast<int>(tokens_.size()));
    retain_ = false;
    hover_at_ = token_index;
    bool bound = false;
    bool parameter = false;
    Shape* shape = nullptr;
    if (suffix_before(token_index)) {
        // receiver() parses up to the '.' or ':' and leaves the member name out.
        shape = member_of(receiver(token_index - 1), token.text);
    } else {
        shape = lookup_binding(token.text, token_index, bound, parameter);
    }
    hover_at_ = -1;
    if (shape == nullptr) {
        return {};
    }

    const Written written = DescribeSymbol(shape, token.text, bound, parameter);
    if (!written.found) {
        return {};
    }
    info.found = true;
    info.title = written.title;
    info.detail = written.detail;
    info.summary = written.summary;
    return info;
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

// The caret is inside a string argument. The first string of a colon call offers
// services or children. The first string of Instance.new offers classes that
// call can create. Any call of a function written in this source shows that
// function's parameter list.
CompletionList CompleteString(const Scan& scan, const std::u32string& text, int caret,
                              const std::vector<engine_core::LuaNode>& world, std::uint32_t script_id,
                              bool script_global) {
    if (scan.tokens.empty() || scan.quote == 0) {
        return {};
    }
    const std::vector<Token>& tokens = scan.tokens;
    const int index = static_cast<int>(tokens.size());
    const CallSlot slot = CallArgumentAt(tokens, index);
    int callee_end = -1;
    const Token& last = tokens.back();
    if (last.kind == Token::LParen) {
        callee_end = index - 1;
    } else if (IsCallPrefix(last)) {
        callee_end = index;
    }
    const bool first_arg = !slot.found || slot.argument == 0;
    const bool colon = callee_end >= 0 && first_arg && IsColonCall(tokens, callee_end);
    const int expr_end = slot.found ? slot.open : callee_end;
    if (expr_end < 0) {
        return {};
    }
    Resolver resolver(tokens, world, script_id, script_global);
    resolver.parse_until(expr_end);
    CompletionList list;
    Shape* callee = resolver.receiver(expr_end);
    const bool child = colon && callee != nullptr && callee->resolves_child && callee->instance != kNoInstance;
    const bool service = colon && callee != nullptr && callee->service_arg;
    // Instance.new is a dot call. GetService also records class_from_arg, and a
    // colon call already offers services, so a service argument is not a class.
    const bool created = !colon && first_arg && callee != nullptr && callee->class_from_arg && !callee->service_arg;
    if (child || service || created) {
        BeginStringArgument(list, scan, text, caret);
        if (child) {
            AddChildren(world, callee->instance, list.prefix, list.items);
        } else if (service) {
            AddServices(list.prefix, list.items);
        } else {
            AddCreatable(list.prefix, list.items);
        }
    }
    resolver.complete_signature(index, list);
    if (list.site == CompleteSite::None && list.signature.empty()) {
        return {};
    }
    if (list.site == CompleteSite::None) {
        list.site = CompleteSite::Argument;
    }
    return list;
}

}  // namespace

CompletionList complete_luau(std::string_view source, int caret, const std::vector<engine_core::LuaNode>& world,
                             std::uint32_t script_id, bool script_global) {
    const std::u32string text = Utf32(source);
    if (caret < 0) {
        caret = 0;
    }
    if (caret > static_cast<int>(text.size())) {
        caret = static_cast<int>(text.size());
    }
    const Scan scan = Tokenize(text, caret);
    CompletionList list;
    if (scan.blocked) {
        return list;
    }
    if (scan.open_string) {
        return CompleteString(scan, text, caret, world, script_id, script_global);
    }
    const std::vector<Token>& tokens = scan.tokens;
    int index = static_cast<int>(tokens.size());
    if (!tokens.empty() && tokens.back().end == caret &&
        (tokens.back().kind == Token::Name || tokens.back().kind == Token::Keyword)) {
        list.prefix = tokens.back().text;
        --index;
    }
    list.replace_end = caret;
    list.replace_begin = list.prefix.empty() ? caret : tokens[static_cast<std::size_t>(index)].begin;
    if (!list.prefix.empty()) {
        int end = caret;
        while (end < static_cast<int>(text.size()) && IsNameContinue(text[static_cast<std::size_t>(end)])) {
            ++end;
        }
        list.replace_end = end;
    }

    const bool cast = index >= 2 && tokens[static_cast<std::size_t>(index - 1)].kind == Token::Colon &&
                      tokens[static_cast<std::size_t>(index - 2)].kind == Token::Colon;
    const bool annotation = !cast && index > 0 && tokens[static_cast<std::size_t>(index - 1)].kind == Token::Colon &&
                            AnnotationAt(tokens, index - 1);
    const bool member = !cast && !annotation && index > 0 &&
                        (tokens[static_cast<std::size_t>(index - 1)].kind == Token::Dot ||
                         tokens[static_cast<std::size_t>(index - 1)].kind == Token::Colon);
    Resolver resolver(tokens, world, script_id, script_global);
    if (cast || annotation) {
        list.site = CompleteSite::Type;
        AddTypes(list.prefix, list.items);
        return list;
    }
    if (member) {
        list.site = CompleteSite::Member;
        const bool colon = tokens[static_cast<std::size_t>(index - 1)].kind == Token::Colon;
        const int expr_end = index - 1;
        resolver.parse_until(expr_end);
        Shape* shape = resolver.receiver(expr_end);
        resolver.add_members(shape, colon, list.prefix, list.items);
        return list;
    }
    list.site = CompleteSite::Name;
    resolver.parse_until(index);
    resolver.add_names(list.prefix, index, list.items);
    resolver.complete_callback(index, list);
    resolver.complete_signature(index, list);
    return list;
}

HoverInfo hover_luau(std::string_view source, int index, const std::vector<engine_core::LuaNode>& world,
                     std::uint32_t script_id, bool script_global) {
    const std::u32string text = Utf32(source);
    if (index < 0) {
        index = 0;
    }
    if (index > static_cast<int>(text.size())) {
        index = static_cast<int>(text.size());
    }
    // A trailing comment marks the scan blocked and still keeps every token before it.
    const Scan scan = Tokenize(text, static_cast<int>(text.size()));
    if (scan.tokens.empty()) {
        return {};
    }
    Resolver resolver(scan.tokens, world, script_id, script_global);
    return resolver.describe(index);
}

}  // namespace ide
