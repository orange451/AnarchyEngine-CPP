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

struct Shape {
    std::string class_name;
    std::string library;
    std::string value_type;
    std::uint32_t instance = kNoInstance;
    bool call = false;
    bool class_from_arg = false;
    bool resolves_child = false;
    bool returns_list = false;
    std::string callee_owner;
    std::string callee_name;
    std::string result_type;
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
                scan.blocked = true;
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

const engine_core::LuaNode* FindChild(const std::vector<engine_core::LuaNode>& world, std::uint32_t parent,
                                      std::string_view name) {
    for (const engine_core::LuaNode& node : world) {
        if (node.parent == parent && node.name == name) {
            return &node;
        }
    }
    return nullptr;
}

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
        if (name.empty() || name == "nil") {
            return none();
        }
        if (name == "string" || name == "number" || name == "boolean" || name == "vector" || name == "buffer" ||
            name == "function" || name == "table" || name == "thread") {
            return value_shape(name, name == "function");
        }
        if (engine_core::lua_class_known(name.c_str())) {
            return class_shape(name, kNoInstance);
        }
        return value_shape(name);
    }

    Shape* adopt(const engine_core::LuaShape& input) {
        Shape* shape = fresh();
        shape->class_name = input.class_name;
        shape->call = input.call;
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
        for (int index = static_cast<int>(bindings_.size()) - 1; index >= 0; --index) {
            const Binding& binding = bindings_[static_cast<std::size_t>(index)];
            if (binding.name == name && binding.depth <= depth_ && binding.visible <= i_) {
                return binding.shape;
            }
        }
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
                return type_shape(field->type_name != nullptr ? field->type_name : "");
            }
            Shape* shape = fresh();
            shape->call = true;
            shape->class_from_arg = field->class_from_arg;
            shape->resolves_child = field->resolves_child;
            shape->returns_list = field->returns_list;
            shape->result_type = field->type_name != nullptr ? field->type_name : "";
            shape->instance = base->instance;
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
                    return class_shape(symbol.type_name, kNoInstance);
                }
                Shape* shape = fresh();
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
                    shape->callee_owner = base->library;
                    shape->callee_name = name;
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
            if (const engine_core::LuaNode* child = FindChild(world_, callee->instance, *literal)) {
                return class_shape(child->class_name.empty() ? "DataModel" : child->class_name, child->id);
            }
            return class_shape("Instance", kNoInstance);
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
        return adopt(exported);
    }

    void bind(std::string name, Shape* shape, int visible, int depth = -1) {
        if (depth < 0) {
            depth = depth_;
        }
        bindings_.push_back(Binding{std::move(name), shape != nullptr ? shape : none(), depth, visible});
    }

    void push_scope() { ++depth_; }

    void pop_scope() {
        if (depth_ > 0) {
            --depth_;
        }
        while (!bindings_.empty() && bindings_.back().depth > depth_) {
            bindings_.pop_back();
        }
    }

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

    void add_names(std::string_view prefix, int at, std::vector<CompletionItem>& out) {
        std::vector<std::string> seen;
        auto take = [&](const std::string& name, const std::string& detail, bool call) {
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
            } else if (shape != nullptr && !shape->value_type.empty()) {
                detail = shape->value_type;
            }
            take(binding.name, detail, shape != nullptr && shape->call);
        }
        std::vector<engine_core::LuaSymbol> globals;
        engine_core::lua_library_globals(globals);
        for (const engine_core::LuaSymbol& symbol : globals) {
            take(symbol.name, symbol.type_name == "table" ? "library" : symbol.type_name, symbol.call);
        }
        take("game", "DataModel", false);
        if (script_global_) {
            std::string script_type = "Script";
            if (script_id_ != 0) {
                if (const engine_core::LuaNode* script = FindNode(world_, script_id_)) {
                    if (!script->class_name.empty()) {
                        script_type = script->class_name;
                    }
                }
            }
            take("script", script_type, false);
        }
        for (const char* keyword : kKeywords) {
            take(keyword, "keyword", false);
        }
    }

    void add_members(Shape* shape, bool colon, std::string_view prefix, std::vector<CompletionItem>& out) {
        if (shape == nullptr) {
            return;
        }
        std::vector<std::string> seen;
        auto take = [&](const std::string& name, const std::string& detail, bool call, bool method) {
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
                take(field.name, field.method ? "function" : type_name, field.method, field.method);
            }
        }
        if (!shape->library.empty()) {
            std::vector<engine_core::LuaSymbol> symbols;
            engine_core::lua_library_members(shape->library, symbols);
            for (const engine_core::LuaSymbol& symbol : symbols) {
                take(symbol.name, symbol.type_name, symbol.call, symbol.method);
            }
        }
        if (shape->value_type == "string" || shape->value_type == "vector") {
            std::vector<engine_core::LuaSymbol> symbols;
            engine_core::lua_value_members(shape->value_type, symbols);
            for (const engine_core::LuaSymbol& symbol : symbols) {
                take(symbol.name, symbol.type_name, symbol.call, symbol.method);
            }
        }
        for (const auto& field : shape->fields) {
            const Shape* child = field.second;
            const bool call = child != nullptr && child->call;
            std::string detail = "field";
            if (child != nullptr && !child->class_name.empty()) {
                detail = child->class_name;
            } else if (child != nullptr && !child->value_type.empty()) {
                detail = child->value_type;
            }
            take(field.first, detail, call, false);
        }
    }

private:
    struct Binding {
        std::string name;
        Shape* shape = nullptr;
        int depth = 0;
        int visible = 0;
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
            std::vector<std::string> names;
            if (is_name()) {
                names.push_back(take_name());
                while (consume(Token::Comma) && is_name()) {
                    names.push_back(take_name());
                }
            }
            if (consume(Token::Eq) || is_kw("in")) {
                if (is_kw("in")) {
                    advance();
                }
                parse_expr_list();
            }
            if (at_end()) {
                return;
            }
            if (is_kw("do")) {
                advance();
            }
            push_scope();
            for (const std::string& name : names) {
                bind(name, none(), i_);
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
            if (!at_end() && !block_end()) {
                parse_expr_list();
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
        };
        std::vector<Slot> slots;
        while (is_name()) {
            slots.push_back(Slot{take_name()});
            if (is(Token::Colon) && !next_colon()) {
                advance();
                if (is_name()) {
                    advance();
                } else if (at_end()) {
                    cut_ = true;
                }
            }
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
            Shape* shape = index < inits.size() && inits[index] != nullptr ? inits[index] : none();
            bind(slots[index].name, shape, visible);
        }
    }

    void parse_function(bool expression, bool local_name) {
        if (!expression && is_name()) {
            std::string first = take_name();
            if (consume(Token::Dot)) {
                if (is_name()) {
                    advance();
                }
            } else if (is(Token::Colon) && !next_colon()) {
                advance();
                if (is_name()) {
                    advance();
                }
            } else if (local_name) {
                bind(std::move(first), value_shape("function", true), i_);
            } else {
                // `function name` assigns a global, including when it is nested in another block.
                bind(std::move(first), value_shape("function", true), i_, 0);
            }
        } else if (expression && is_name()) {
            advance();
        }
        push_scope();
        std::vector<std::string> params;
        bool closed = false;
        if (consume(Token::LParen)) {
            while (!at_end() && !is(Token::RParen)) {
                if (is(Token::Ellipsis)) {
                    advance();
                    break;
                }
                if (!is_name()) {
                    break;
                }
                std::string name = take_name();
                if (is(Token::Colon) && !next_colon()) {
                    advance();
                    if (is_name()) {
                        advance();
                    }
                }
                params.push_back(std::move(name));
                if (!consume(Token::Comma)) {
                    break;
                }
                if (at_end()) {
                    break;
                }
            }
            closed = consume(Token::RParen);
        }
        // Parameters belong to the body. They are not in scope while the
        // parameter list itself is still being typed.
        const int visible = closed ? i_ : limit_ + 1;
        for (const std::string& name : params) {
            bind(name, none(), visible);
        }
        if (!closed && at_end()) {
            cut_ = true;
        }
        parse_block();
        if (is_kw("end")) {
            advance();
            pop_scope();
        } else if (at_end()) {
            cut_ = true;
        } else {
            pop_scope();
        }
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
        std::vector<std::string> names;
        int cursor = start;
        while (cursor < eq) {
            const Token& token = tokens_[static_cast<std::size_t>(cursor)];
            if (token.kind == Token::Name &&
                (cursor + 1 == eq || tokens_[static_cast<std::size_t>(cursor + 1)].kind == Token::Comma)) {
                names.push_back(token.text);
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
        for (std::size_t index = 0; index < names.size(); ++index) {
            Shape* shape = index < values.size() ? values[index] : none();
            bool updated = false;
            for (int slot = static_cast<int>(bindings_.size()) - 1; slot >= 0; --slot) {
                Binding& binding = bindings_[static_cast<std::size_t>(slot)];
                if (binding.name == names[index] && binding.depth <= depth_ && binding.visible <= i_) {
                    binding.shape = shape != nullptr ? shape : none();
                    updated = true;
                    break;
                }
            }
            if (!updated) {
                bind(names[index], shape, i_, 0);
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
            parse_function(true, false);
            return value_shape("function", true);
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

    bool callee_token(const Token& token) const {
        if (token.kind == Token::Name || token.kind == Token::String || token.kind == Token::RParen ||
            token.kind == Token::RBrack || token.kind == Token::RBrace) {
            return true;
        }
        return token.kind == Token::Keyword && (token.text == "true" || token.text == "false" || token.text == "nil");
    }

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
        out.push_back(std::move(item));
    }
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
    return list;
}

}  // namespace ide
