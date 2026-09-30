#include "Enum.hpp"
#include "LuaApi.hpp"

#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine_core {
namespace {

bool identifier_char(char character, bool first) {
    const bool letter = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') || character == '_';
    if (first) {
        return letter;
    }
    return letter || (character >= '0' && character <= '9');
}

bool identifier(std::string_view name) {
    if (name.empty() || !identifier_char(name.front(), true)) {
        return false;
    }
    for (char character : name) {
        if (!identifier_char(character, false)) {
            return false;
        }
    }
    return true;
}

bool known_type(std::string_view name) {
    if (name == "any" || name == "unknown" || name == "never" || name == "nil" || name == "string" || name == "number" ||
        name == "boolean" || name == "thread" || name == "buffer" || name == "vector" || name == "Vector3") {
        return true;
    }
    return !name.empty() && lua_class_known(std::string(name).c_str());
}

// Editor docs say "function". Luau's definition grammar wants a function type,
// parenthesized so it can sit inside another function type.
std::string to_luau_type(std::string type) {
    while (!type.empty() && (type.front() == ' ' || type.back() == ' ')) {
        if (type.front() == ' ') {
            type.erase(type.begin());
        } else {
            type.pop_back();
        }
    }
    if (type.empty()) {
        return "any";
    }
    bool optional = false;
    if (type.back() == '?' && type.size() > 1) {
        optional = true;
        type.pop_back();
    }
    std::string core = "any";
    if (type == "function") {
        core = "((...any) -> ...any)";
    } else if (type == "table") {
        core = "{[any]: any}";
    } else if (type == "nil") {
        core = "nil";
        optional = false;
    } else if (type.size() >= 2 && type.front() == '{' && type.back() == '}') {
        const std::string inner = type.substr(1, type.size() - 2);
        core = known_type(inner) || inner == "number" ? type : "{[any]: any}";
    } else if (known_type(type)) {
        core = std::string(type);
    }
    if (!optional) {
        return core;
    }
    if (identifier(core)) {
        return core + "?";
    }
    return "(" + core + ")?";
}

std::string return_syntax(const LuaField* field, const LuaDoc& doc) {
    if (field != nullptr && field->returns_list && field->type_name != nullptr && field->type_name[0] != '\0') {
        return "{" + to_luau_type(field->type_name) + "}";
    }
    if (field != nullptr && field->type_name != nullptr && field->type_name[0] != '\0' &&
        std::strcmp(field->type_name, "nil") != 0) {
        return to_luau_type(field->type_name);
    }
    if (doc.found && doc.returns_nothing) {
        return "()";
    }
    if (doc.found && !doc.return_type.empty()) {
        if (doc.return_type.find(',') != std::string::npos && doc.return_type.front() != '{') {
            std::string pack;
            std::size_t start = 0;
            while (start < doc.return_type.size()) {
                const std::size_t comma = doc.return_type.find(',', start);
                const std::string part = doc.return_type.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                if (!pack.empty()) {
                    pack += ", ";
                }
                pack += to_luau_type(part);
                if (comma == std::string::npos) {
                    break;
                }
                start = comma + 1;
            }
            return "(" + pack + ")";
        }
        return to_luau_type(doc.return_type);
    }
    if (doc.found && doc.return_unknown) {
        return "...any";
    }
    return "()";
}

std::string param_list(const LuaDoc& doc) {
    std::string out;
    if (!doc.found) {
        return out;
    }
    int unnamed = 0;
    for (const LuaDocParam& param : doc.params) {
        if (!out.empty()) {
            out += ", ";
        }
        std::string name = param.name;
        if (!identifier(name)) {
            name = "arg" + std::to_string(++unnamed);
        }
        out += name;
        out += ": ";
        out += to_luau_type(param.type_name.empty() ? "any" : param.type_name);
    }
    if (doc.variadic) {
        if (!out.empty()) {
            out += ", ";
        }
        out += "...any";
    }
    return out;
}

// `self` stays unannotated. The definition grammar requires that, then adds the receiver type itself.
// A declared function spells its vararg `...: any`. `...any` only parses in a function type.
std::string method_params(const LuaDoc& doc) {
    if (!doc.found) {
        return "self, ...: any";
    }
    std::string rest = param_list(doc);
    if (doc.variadic) {
        rest.replace(rest.size() - std::char_traits<char>::length("...any"), std::string::npos, "...: any");
    }
    if (rest.empty()) {
        return "self";
    }
    return "self, " + rest;
}

// An operand as registered: one type, or several joined by " | ".
std::string operand_type(const char* registered) {
    const std::string text = registered != nullptr ? registered : "";
    std::string out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t bar = text.find('|', start);
        const std::string part = text.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        if (!out.empty()) {
            out += " | ";
        }
        out += to_luau_type(part);
        if (bar == std::string::npos) {
            break;
        }
        start = bar + 1;
    }
    return out.empty() ? "any" : out;
}

// A signal's own type, such as Signal_RunService_Heartbeat: a Signal whose
// Connect takes a callback of the parameters it passes, and whose Wait returns
// them. Declared after the classes, as Luau reads the file whole.
struct SignalType {
    std::string name;
    std::string params;
    std::string values;
};

std::vector<SignalType>& signal_types() {
    static std::vector<SignalType> types;
    return types;
}

std::string signal_type(const std::string& owner, const LuaField& field) {
    SignalType type;
    type.name = "Signal_" + owner + "_" + field.name;
    for (int index = 0; index < field.param_count; ++index) {
        const LuaParam& param = field.params[index];
        const std::string luau = to_luau_type(param.type_name != nullptr && param.type_name[0] != '\0' ? param.type_name : "any");
        type.params += (index > 0 ? ", " : "") + std::string(param.name != nullptr && identifier(param.name) ? std::string(param.name) + ": " : "") + luau;
        type.values += (index > 0 ? ", " : "") + luau;
    }
    signal_types().push_back(type);
    return type.name;
}

void emit_signal_types(std::ostringstream& out) {
    if (!lua_class_known("Signal") || !lua_class_known("Connection")) {
        return;
    }
    for (const SignalType& type : signal_types()) {
        out << "declare extern type " << type.name << " extends Signal with\n";
        out << "    function Connect(self, callback: (" << type.params << ") -> ()): Connection\n";
        out << "    function Wait(self): (" << type.values << ")\n";
        out << "end\n\n";
    }
    signal_types().clear();
}

void emit_class(std::ostringstream& out, const std::string& name) {
    if (name == "Vector3" || !identifier(name)) {
        return;
    }
    const char* base = lua_class_base(name.c_str());
    const bool has_base = base != nullptr && base[0] != '\0' && std::strcmp(base, name.c_str()) != 0 && identifier(base) &&
                          lua_class_known(base);
    out << "declare extern type " << name;
    if (has_base) {
        out << " extends " << base;
    }
    out << " with\n";

    std::vector<LuaField> fields;
    lua_class_own_members(name.c_str(), fields);
    for (const LuaField& field : fields) {
        if (field.blocked || field.name == nullptr || !identifier(field.name)) {
            continue;
        }
        const LuaDoc doc = lua_symbol_doc(name, field.name);
        if (field.method) {
            out << "    function " << field.name << "(" << method_params(doc) << "): " << return_syntax(&field, doc) << "\n";
            continue;
        }
        const char* type_name = field.type_name != nullptr && field.type_name[0] != '\0' ? field.type_name : "any";
        out << "    ";
        if (!field.writable) {
            out << "read ";
        }
        // A signal that passes values has its own type, so a callback's
        // parameters are typed where it is written.
        const bool signal = field.params != nullptr && field.param_count > 0 && std::strcmp(type_name, "Signal") == 0;
        out << field.name << ": " << (signal ? signal_type(name, field) : to_luau_type(type_name)) << "\n";
    }
    // Declared as properties, not methods: a method's first argument is always
    // this class, and `2 * v` passes the number first. Luau moves __ names into
    // the metatable. Overloads of one metamethod, such as Matrix4 * Matrix4 and
    // Matrix4 * Vector3, are one intersection of function types.
    std::vector<LuaOperator> operators;
    lua_class_operators(name.c_str(), operators);
    std::vector<std::string> metamethods;
    std::unordered_map<std::string, std::vector<std::string>> signatures;
    for (const LuaOperator& op : operators) {
        if (op.metamethod == nullptr || !identifier(op.metamethod)) {
            continue;
        }
        std::string signature = "(" + operand_type(op.left);
        if (op.right != nullptr) {
            signature += ", " + operand_type(op.right);
        }
        signature += ") -> " + operand_type(op.result);
        std::vector<std::string>& overloads = signatures[op.metamethod];
        if (overloads.empty()) {
            metamethods.emplace_back(op.metamethod);
        }
        overloads.push_back(std::move(signature));
    }
    for (const std::string& metamethod : metamethods) {
        const std::vector<std::string>& overloads = signatures[metamethod];
        out << "    " << metamethod << ": ";
        if (overloads.size() == 1) {
            out << overloads.front() << "\n";
            continue;
        }
        for (std::size_t index = 0; index < overloads.size(); ++index) {
            out << (index > 0 ? " & " : "") << "(" << overloads[index] << ")";
        }
        out << "\n";
    }
    out << "end\n\n";
}

void emit_static(std::ostringstream& out, const std::string& owner) {
    if (!identifier(owner)) {
        return;
    }
    std::vector<std::string> names;
    lua_doc_names(owner, names);
    std::vector<std::string> lines;
    for (const std::string& name : names) {
        if (!identifier(name) || lua_class_find(owner.c_str(), name) != nullptr) {
            continue;
        }
        const LuaDoc doc = lua_symbol_doc(owner, name);
        const LuaResult noted = lua_function_result(owner, name);
        if (!doc.found && !noted.known) {
            continue;
        }
        const bool callable = noted.known || !doc.found || !doc.params.empty() || doc.variadic || doc.returns_nothing || doc.return_unknown;
        if (callable) {
            const std::string params = doc.found ? param_list(doc) : "...any";
            const std::string result = doc.found ? return_syntax(nullptr, doc) : to_luau_type(noted.type_name.empty() ? "any" : noted.type_name);
            lines.push_back(name + ": (" + params + ") -> " + result);
            continue;
        }
        if (!doc.return_type.empty()) {
            lines.push_back(name + ": " + to_luau_type(doc.return_type));
        }
    }
    if (lines.empty()) {
        return;
    }
    out << "declare " << owner << ": {\n";
    for (const std::string& line : lines) {
        out << "    " << line << ",\n";
    }
    out << "}\n\n";
}

// Enum.<Type>.<Name>. Every item is an EnumItem. Each type is its own
// declared type, EnumKeyCode and so on, so a misspelled item is an unknown
// member of a type with a short name. A misspelled type is an unknown key.
void emit_enum(std::ostringstream& out) {
    if (!lua_class_known("EnumItem")) {
        return;
    }
    for (int index = 0; index < enum_type_count(); ++index) {
        const EnumType& type = enum_type_at(index);
        out << "declare extern type Enum" << type.name << " with\n";
        for (int item = 0; item < type.count; ++item) {
            out << "    read " << type.items[item].name << ": EnumItem\n";
        }
        out << "end\n\n";
    }
    out << "declare Enum: {\n";
    for (int index = 0; index < enum_type_count(); ++index) {
        const EnumType& type = enum_type_at(index);
        out << "    " << type.name << ": Enum" << type.name << ",\n";
    }
    out << "}\n\n";
}

}  // namespace

std::string lua_analysis_definitions() {
    std::ostringstream out;
    out << "-- Built from the registered classes and their documentation.\n";
    // Redeclaring `vector` would replace Luau's builtin and drop its operators.
    // Vector3 is that same value. Members are added onto it after this loads.
    if (lua_class_known("Vector3")) {
        out << "export type Vector3 = vector\n\n";
    }

    std::vector<std::string> names;
    lua_class_names(names);
    std::unordered_set<std::string> emitted;
    emitted.insert("Vector3");
    bool progress = true;
    while (progress) {
        progress = false;
        for (const std::string& name : names) {
            if (emitted.count(name) != 0) {
                continue;
            }
            const char* base = lua_class_base(name.c_str());
            if (base != nullptr && base[0] != '\0' && std::strcmp(base, name.c_str()) != 0 && lua_class_known(base) &&
                emitted.count(base) == 0) {
                continue;
            }
            emit_class(out, name);
            emitted.insert(name);
            progress = true;
        }
    }
    for (const std::string& name : names) {
        if (emitted.count(name) == 0) {
            emit_class(out, name);
        }
    }

    emit_signal_types(out);
    if (lua_class_known("Game")) {
        out << "declare game: Game\n";
    }
    if (lua_class_known("Workspace")) {
        out << "declare workspace: Workspace\n";
    }
    if (lua_class_known("Script") && lua_class_known("ModuleScript")) {
        out << "declare script: Script | ModuleScript\n";
    }
    out << "\n";

    for (const std::string& name : names) {
        emit_static(out, name);
    }
    emit_enum(out);
    std::vector<std::string> libraries;
    lua_host_library_names(libraries);
    for (const std::string& name : libraries) {
        if (name == "Enum" || lua_class_known(name.c_str())) {
            continue;
        }
        emit_static(out, name);
    }
    return out.str();
}

}  // namespace engine_core
