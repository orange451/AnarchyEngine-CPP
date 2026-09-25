#include "LuaApi.hpp"

#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>

namespace engine_core {
namespace {

struct ClassRecord {
    const char* name = nullptr;
    const char* base = nullptr;
    std::vector<LuaField> fields;
};

std::vector<ClassRecord>& classes() {
    static std::vector<ClassRecord> records;
    return records;
}

ClassRecord* find_class(const char* name) {
    if (name == nullptr) {
        return nullptr;
    }
    for (ClassRecord& record : classes()) {
        if (std::strcmp(record.name, name) == 0) {
            return &record;
        }
    }
    return nullptr;
}

const ClassRecord* find_class_const(const char* name) {
    return find_class(name);
}

struct ResultNote {
    std::string type_name;
    bool class_from_arg = false;
};

std::unordered_map<std::string, ResultNote>& results() {
    static std::unordered_map<std::string, ResultNote> notes;
    return notes;
}

std::string result_key(std::string_view owner, std::string_view name) {
    std::string key;
    key.reserve(owner.size() + name.size() + 1);
    key.append(owner);
    key.push_back('\n');
    key.append(name);
    return key;
}

void append_unique(std::vector<LuaField>& out, const LuaField& field) {
    for (LuaField& existing : out) {
        if (existing.name != nullptr && field.name != nullptr && std::strcmp(existing.name, field.name) == 0) {
            existing = field;
            return;
        }
    }
    out.push_back(field);
}

void collect(const char* class_name, std::vector<LuaField>& out, int depth) {
    if (class_name == nullptr || depth > 32) {
        return;
    }
    const ClassRecord* record = find_class_const(class_name);
    if (record == nullptr) {
        return;
    }
    collect(record->base, out, depth + 1);
    for (const LuaField& field : record->fields) {
        append_unique(out, field);
    }
}

}  // namespace

void register_lua_class(const char* class_name, const char* base, const LuaField* fields, int count) {
    if (class_name == nullptr) {
        return;
    }
    ClassRecord* record = find_class(class_name);
    if (record == nullptr) {
        classes().push_back(ClassRecord{});
        record = &classes().back();
        record->name = class_name;
    }
    if (base != nullptr && record->base == nullptr) {
        record->base = base;
    }
    if (fields == nullptr || count <= 0) {
        return;
    }
    for (int index = 0; index < count; ++index) {
        append_unique(record->fields, fields[index]);
    }
}

void lua_class_members(const char* class_name, std::vector<LuaField>& out) {
    out.clear();
    collect(class_name, out, 0);
}

const LuaField* lua_class_find(const char* class_name, std::string_view name) {
    const ClassRecord* record = find_class_const(class_name);
    int depth = 0;
    while (record != nullptr && depth < 32) {
        for (const LuaField& field : record->fields) {
            if (field.name != nullptr && name == field.name) {
                return &field;
            }
        }
        record = find_class_const(record->base);
        ++depth;
    }
    return nullptr;
}

bool lua_class_known(const char* class_name) { return find_class_const(class_name) != nullptr; }

void lua_class_names(std::vector<std::string>& out) {
    out.clear();
    for (const ClassRecord& record : classes()) {
        if (record.name != nullptr) {
            out.emplace_back(record.name);
        }
    }
}

namespace {

std::vector<const char*>& service_names() {
    static std::vector<const char*> names;
    return names;
}

}  // namespace

void register_lua_service(const char* name) {
    if (name == nullptr) {
        return;
    }
    for (const char* existing : service_names()) {
        if (std::strcmp(existing, name) == 0) {
            return;
        }
    }
    service_names().push_back(name);
}

bool lua_service_known(const char* name) {
    if (name == nullptr) {
        return false;
    }
    for (const char* existing : service_names()) {
        if (std::strcmp(existing, name) == 0) {
            return true;
        }
    }
    return false;
}

void lua_service_names(std::vector<std::string>& out) {
    out.clear();
    for (const char* name : service_names()) {
        if (name != nullptr) {
            out.emplace_back(name);
        }
    }
}

void lua_note_result(const char* owner, const char* name, const char* type_name, bool class_from_arg) {
    if (owner == nullptr || name == nullptr) {
        return;
    }
    ResultNote note;
    note.type_name = type_name != nullptr ? type_name : "";
    note.class_from_arg = class_from_arg;
    results()[result_key(owner, name)] = std::move(note);
}

namespace {

LuaField component(const char* name) { return lua_property(name, "number", true, nullptr, nullptr); }

ANARCHY_LUA_REGISTER(register_value_classes) {
    const LuaField color[] = {component("r"), component("g"), component("b"), component("a")};
    register_lua_class("Color", nullptr, color, 4);
    register_lua_class("Transform", nullptr, nullptr, 0);
    register_lua_class("Instance", "DataModel", nullptr, 0);
}

}  // namespace

LuaResult lua_function_result(std::string_view owner, std::string_view name) {
    const auto found = results().find(result_key(owner, name));
    LuaResult result;
    if (found == results().end()) {
        return result;
    }
    result.known = true;
    result.type_name = found->second.type_name;
    result.class_from_arg = found->second.class_from_arg;
    return result;
}

}  // namespace engine_core
