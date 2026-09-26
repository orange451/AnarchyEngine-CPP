#include "LuaApi.hpp"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#endif
#include "Luau/Parser.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include "luacode.h"
#include "lualib.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace engine_core {
namespace {

void* reflect_alloc(void*, void* pointer, std::size_t, std::size_t size) {
    if (size == 0) {
        std::free(pointer);
        return nullptr;
    }
    return std::realloc(pointer, size);
}

lua_State* reflect_state() {
    static lua_State* state = nullptr;
    if (state == nullptr) {
        state = lua_newstate(reflect_alloc, nullptr);
        if (state != nullptr) {
            open_host_libraries(state);
        }
    }
    return state;
}

std::string lua_type_name(lua_State* state, int index) {
    switch (lua_type(state, index)) {
    case LUA_TFUNCTION:
        return "function";
    case LUA_TTABLE:
        return "table";
    case LUA_TNUMBER:
        return "number";
    case LUA_TSTRING:
        return "string";
    case LUA_TBOOLEAN:
        return "boolean";
    case LUA_TNIL:
        return "nil";
    default:
        break;
    }
    const char* name = luaL_typename(state, index);
    return name != nullptr ? name : "value";
}

void walk_table(lua_State* state, int index, std::vector<LuaSymbol>& out, bool methods) {
    const int table = lua_absindex(state, index);
    lua_pushnil(state);
    while (lua_next(state, table) != 0) {
        if (lua_type(state, -2) == LUA_TSTRING) {
            const char* key = lua_tostring(state, -2);
            if (key != nullptr && key[0] != '\0') {
                LuaSymbol symbol;
                symbol.name = key;
                symbol.type_name = lua_type_name(state, -1);
                symbol.call = symbol.type_name == "function";
                symbol.method = methods && symbol.call;
                out.push_back(std::move(symbol));
            }
        }
        lua_pop(state, 1);
    }
}

void sort_symbols(std::vector<LuaSymbol>& out) {
    std::sort(out.begin(), out.end(), [](const LuaSymbol& a, const LuaSymbol& b) { return a.name < b.name; });
}

int reflect_steps = 0;

void reflect_interrupt(lua_State* state, int) {
    if (--reflect_steps <= 0) {
        luaL_error(state, "reflection timed out");
    }
}

struct Job {
    const std::vector<LuaNode>* world = nullptr;
    std::unordered_set<std::uint32_t> loading;
    std::unordered_set<std::uint32_t> scanned;
    std::unordered_map<std::uint32_t, int> returns;
    // "moduleId\nname\nline" for each `function obj:name` in a module that was run.
    std::unordered_set<std::string> methods;
};

Job* job = nullptr;

const LuaNode* find_node(std::uint32_t id) {
    if (job == nullptr || job->world == nullptr) {
        return nullptr;
    }
    for (const LuaNode& node : *job->world) {
        if (node.id == id) {
            return &node;
        }
    }
    return nullptr;
}

int push_dummy(lua_State* state, std::uint32_t id);

int dummy_index(lua_State* state) {
    const char* key = luaL_checkstring(state, 2);
    lua_getfield(state, 1, "__id");
    const auto id = static_cast<std::uint32_t>(lua_tointeger(state, -1));
    lua_pop(state, 1);
    const LuaNode* node = find_node(id);
    if (node == nullptr || key == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    if (std::strcmp(key, "Name") == 0) {
        lua_pushlstring(state, node->name.data(), node->name.size());
        return 1;
    }
    if (std::strcmp(key, "ClassName") == 0) {
        lua_pushlstring(state, node->class_name.data(), node->class_name.size());
        return 1;
    }
    if (std::strcmp(key, "Parent") == 0) {
        if (node->parent == 0xffffffffu) {
            lua_pushnil(state);
        } else {
            push_dummy(state, node->parent);
        }
        return 1;
    }
    if (lua_method_resolves_child(key) || std::strcmp(key, "GetChildren") == 0 ||
        std::strcmp(key, "GetService") == 0 || std::strcmp(key, "IsA") == 0) {
        lua_pushstring(state, key);
        lua_pushcclosure(
            state,
            [](lua_State* inner) -> int {
                const char* method = lua_tostring(inner, lua_upvalueindex(1));
                lua_getfield(inner, 1, "__id");
                const auto self = static_cast<std::uint32_t>(lua_tointeger(inner, -1));
                lua_pop(inner, 1);
                if (method != nullptr && lua_method_resolves_child(method)) {
                    const char* name = luaL_checkstring(inner, 2);
                    if (job != nullptr && job->world != nullptr && name != nullptr) {
                        for (const LuaNode& child : *job->world) {
                            if (child.parent == self && child.name == name) {
                                push_dummy(inner, child.id);
                                return 1;
                            }
                        }
                    }
                    lua_pushnil(inner);
                    return 1;
                }
                if (method != nullptr && std::strcmp(method, "GetChildren") == 0) {
                    lua_newtable(inner);
                    int index = 1;
                    if (job != nullptr && job->world != nullptr) {
                        for (const LuaNode& child : *job->world) {
                            if (child.parent == self) {
                                push_dummy(inner, child.id);
                                lua_rawseti(inner, -2, index);
                                ++index;
                            }
                        }
                    }
                    return 1;
                }
                if (method != nullptr && std::strcmp(method, "IsA") == 0) {
                    const char* class_name = luaL_checkstring(inner, 2);
                    const LuaNode* self_node = find_node(self);
                    const bool match = self_node != nullptr && class_name != nullptr &&
                                       (self_node->class_name == class_name || std::strcmp(class_name, "DataModel") == 0 ||
                                        lua_class_inherits(self_node->class_name.c_str(), class_name));
                    lua_pushboolean(inner, match ? 1 : 0);
                    return 1;
                }
                if (method != nullptr && std::strcmp(method, "GetService") == 0) {
                    const char* service = luaL_checkstring(inner, 2);
                    if (service != nullptr && lua_service_known(service)) {
                        lua_newtable(inner);
                        lua_pushstring(inner, service);
                        lua_setfield(inner, -2, "__class");
                        std::vector<LuaField> members;
                        lua_class_members(service, members);
                        for (const LuaField& member : members) {
                            if (member.name == nullptr) {
                                continue;
                            }
                            if (member.method) {
                                lua_pushcfunction(inner, [](lua_State*) -> int { return 0; }, member.name);
                                lua_setfield(inner, -2, member.name);
                                continue;
                            }
                            lua_newtable(inner);
                            lua_pushstring(inner, "Signal");
                            lua_setfield(inner, -2, "__class");
                            lua_setfield(inner, -2, member.name);
                        }
                        return 1;
                    }
                    luaL_error(inner, "unknown service");
                }
                lua_pushnil(inner);
                return 1;
            },
            "method", 1);
        return 1;
    }
    lua_pushnil(state);
    return 1;
}

int push_dummy(lua_State* state, std::uint32_t id) {
    lua_getfield(state, LUA_REGISTRYINDEX, "ae_dummies");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        lua_newtable(state);
        lua_pushvalue(state, -1);
        lua_setfield(state, LUA_REGISTRYINDEX, "ae_dummies");
    }
    lua_rawgeti(state, -1, static_cast<int>(id));
    if (lua_istable(state, -1)) {
        lua_remove(state, -2);
        return 1;
    }
    lua_pop(state, 1);
    const LuaNode* node = find_node(id);
    lua_newtable(state);
    lua_pushinteger(state, static_cast<int>(id));
    lua_setfield(state, -2, "__id");
    if (node != nullptr) {
        lua_pushlstring(state, node->name.data(), node->name.size());
        lua_setfield(state, -2, "Name");
        lua_pushlstring(state, node->class_name.data(), node->class_name.size());
        lua_setfield(state, -2, "ClassName");
        lua_pushlstring(state, node->class_name.data(), node->class_name.size());
        lua_setfield(state, -2, "__class");
    }
    lua_newtable(state);
    lua_pushcfunction(state, dummy_index, "index");
    lua_setfield(state, -2, "__index");
    lua_setmetatable(state, -2);
    lua_pushvalue(state, -1);
    lua_rawseti(state, -3, static_cast<int>(id));
    lua_remove(state, -2);
    return 1;
}

// The reflection chunk is named "=id", matching the ModuleScript that defined the function.
std::string method_key(std::uint32_t module_id, std::string_view name, int line) {
    std::string key = std::to_string(module_id);
    key.push_back('\n');
    key.append(name);
    key.push_back('\n');
    key.append(std::to_string(line));
    return key;
}

struct ColonMethods : Luau::AstVisitor {
    std::uint32_t module_id = 0;
    std::unordered_set<std::string>* keys = nullptr;

    bool visit(Luau::AstExprFunction* function) override {
        if (keys != nullptr && function->self != nullptr && function->debugname.value != nullptr) {
            keys->insert(method_key(module_id, function->debugname.value,
                                    static_cast<int>(function->location.begin.line) + 1));
        }
        return true;
    }
};

void collect_colon_methods(std::uint32_t module_id, std::string_view source) {
    if (job == nullptr || !job->scanned.insert(module_id).second) {
        return;
    }
    if (source.empty()) {
        return;
    }
    try {
        Luau::Allocator allocator;
        Luau::AstNameTable names(allocator);
        const Luau::ParseResult parsed = Luau::Parser::parse(source.data(), source.size(), names, allocator);
        if (parsed.root == nullptr) {
            return;
        }
        ColonMethods visitor;
        visitor.module_id = module_id;
        visitor.keys = &job->methods;
        parsed.root->visit(&visitor);
    } catch (...) {
        // A parse failure leaves the function as a plain field.
    }
}

bool chunk_module_id(const char* source, std::uint32_t& id) {
    if (source == nullptr || source[0] != '=') {
        return false;
    }
    char* end = nullptr;
    const unsigned long value = std::strtoul(source + 1, &end, 10);
    if (end == source + 1 || *end != '\0' || value > 0xfffffffful) {
        return false;
    }
    id = static_cast<std::uint32_t>(value);
    return true;
}

// True when this closure is a `function obj:name` from the module that defined it.
bool function_is_method(lua_State* state, int index) {
    if (job == nullptr || job->methods.empty()) {
        return false;
    }
    index = lua_absindex(state, index);
    lua_pushvalue(state, index);
    lua_Debug debug{};
    const bool ok = lua_getinfo(state, -1, "sn", &debug) != 0;
    lua_pop(state, 1);
    if (!ok || debug.name == nullptr || debug.linedefined <= 0) {
        return false;
    }
    std::uint32_t id = 0;
    if (!chunk_module_id(debug.source, id)) {
        return false;
    }
    return job->methods.find(method_key(id, debug.name, debug.linedefined)) != job->methods.end();
}

void snapshot_value(lua_State* state, int index, LuaShape& out, int depth, std::unordered_set<const void*>& seen) {
    index = lua_absindex(state, index);
    if (depth > 8) {
        out.type_name = "table";
        return;
    }
    const int type = lua_type(state, index);
    if (type == LUA_TFUNCTION) {
        out.type_name = "function";
        out.call = true;
        out.method = function_is_method(state, index);
        return;
    }
    if (type == LUA_TNUMBER) {
        out.type_name = "number";
        return;
    }
    if (type == LUA_TSTRING) {
        out.type_name = "string";
        return;
    }
    if (type == LUA_TBOOLEAN) {
        out.type_name = "boolean";
        return;
    }
    if (type != LUA_TTABLE) {
        out.type_name = lua_type_name(state, index);
        return;
    }
    const void* pointer = lua_topointer(state, index);
    if (pointer != nullptr && !seen.insert(pointer).second) {
        out.type_name = "table";
        return;
    }
    lua_getfield(state, index, "__class");
    if (lua_isstring(state, -1)) {
        const char* class_name = lua_tostring(state, -1);
        if (class_name != nullptr && lua_class_known(class_name)) {
            out.class_name = class_name;
        }
    }
    lua_pop(state, 1);
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        if (lua_type(state, -2) == LUA_TSTRING) {
            const char* key = lua_tostring(state, -2);
            if (key != nullptr && key[0] != '\0' && key[0] != '_') {
                LuaShape child;
                snapshot_value(state, -1, child, depth + 1, seen);
                out.fields.emplace_back(key, std::move(child));
            }
        }
        lua_pop(state, 1);
    }
    if (out.class_name.empty()) {
        out.type_name = "table";
    }
}

int reflect_require(lua_State* state);
int reflect_instance_new(lua_State* state);

int eval_module(lua_State* state, const LuaNode& node) {
    if (job == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    const auto cached = job->returns.find(node.id);
    if (cached != job->returns.end()) {
        lua_getref(state, cached->second);
        return 1;
    }
    if (!job->loading.insert(node.id).second) {
        lua_pushnil(state);
        return 1;
    }
    collect_colon_methods(node.id, node.source);
    lua_CompileOptions options{};
    options.optimizationLevel = 1;
    options.debugLevel = 1;
    std::size_t bytecode_size = 0;
    std::unique_ptr<char, void (*)(void*)> bytecode(
        luau_compile(node.source.data(), node.source.size(), &options, &bytecode_size), std::free);
    if (bytecode == nullptr || bytecode_size == 0) {
        job->loading.erase(node.id);
        lua_pushnil(state);
        return 1;
    }
    lua_State* thread = lua_newthread(state);
    luaL_sandboxthread(thread);
    lua_pushcfunction(thread, reflect_require, "require");
    lua_setglobal(thread, "require");
    lua_newtable(thread);
    lua_pushcfunction(thread, reflect_instance_new, "new");
    lua_setfield(thread, -2, "new");
    lua_setglobal(thread, "Instance");
    push_dummy(thread, 0);
    lua_setglobal(thread, "game");
    push_dummy(thread, node.id);
    lua_setglobal(thread, "script");

    // "=id" is the function's debug source, so a method can be matched back to this module.
    const std::string chunk = "=" + std::to_string(node.id);
    if (luau_load(thread, chunk.c_str(), bytecode.get(), bytecode_size, 0) != 0) {
        job->loading.erase(node.id);
        lua_pop(state, 1);
        lua_pushnil(state);
        return 1;
    }
    const int status = lua_pcall(thread, 0, 1, 0);
    job->loading.erase(node.id);
    if (status != 0) {
        lua_pop(state, 1);
        lua_pushnil(state);
        return 1;
    }
    lua_xmove(thread, state, 1);
    lua_remove(state, -2);
    lua_pushvalue(state, -1);
    job->returns[node.id] = lua_ref(state, -1);
    return 1;
}

int reflect_require(lua_State* state) {
    if (!lua_istable(state, 1)) {
        luaL_error(state, "require expects a ModuleScript");
    }
    lua_getfield(state, 1, "__id");
    if (!lua_isnumber(state, -1)) {
        luaL_error(state, "require expects a ModuleScript");
    }
    const auto id = static_cast<std::uint32_t>(lua_tointeger(state, -1));
    lua_pop(state, 1);
    const LuaNode* node = find_node(id);
    if (node == nullptr || node->class_name != "ModuleScript") {
        luaL_error(state, "require expects a ModuleScript");
    }
    return eval_module(state, *node);
}

int reflect_instance_new(lua_State* state) {
    const char* name = luaL_checkstring(state, 1);
    lua_newtable(state);
    if (name != nullptr) {
        lua_pushstring(state, name);
        lua_setfield(state, -2, "__class");
        lua_pushstring(state, name);
        lua_setfield(state, -2, "ClassName");
    }
    lua_newtable(state);
    lua_pushcfunction(state, dummy_index, "index");
    lua_setfield(state, -2, "__index");
    lua_setmetatable(state, -2);
    return 1;
}

std::uint64_t hash_world(const std::vector<LuaNode>& world) {
    std::uint64_t hash = 14695981039346656037ull;
    auto mix = [&](unsigned char byte) { hash = (hash ^ byte) * 1099511628211ull; };
    for (const LuaNode& node : world) {
        for (int shift = 0; shift < 32; shift += 8) {
            mix(static_cast<unsigned char>(node.id >> shift));
            mix(static_cast<unsigned char>(node.parent >> shift));
        }
        for (unsigned char byte : node.name) {
            mix(byte);
        }
        for (unsigned char byte : node.class_name) {
            mix(byte);
        }
        for (unsigned char byte : node.source) {
            mix(byte);
        }
        mix(0);
    }
    return hash;
}

struct ExportCache {
    std::uint64_t hash = 0;
    std::unordered_map<std::uint32_t, LuaShape> shapes;
};

ExportCache& cache() {
    static ExportCache stored;
    return stored;
}

}  // namespace

void lua_library_globals(std::vector<LuaSymbol>& out) {
    out.clear();
    lua_State* state = reflect_state();
    if (state == nullptr) {
        return;
    }
    walk_table(state, LUA_GLOBALSINDEX, out, false);
    sort_symbols(out);
}

bool lua_library_members(std::string_view global_name, std::vector<LuaSymbol>& out) {
    out.clear();
    lua_State* state = reflect_state();
    if (state == nullptr) {
        return false;
    }
    lua_pushlstring(state, global_name.data(), global_name.size());
    lua_rawget(state, LUA_GLOBALSINDEX);
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return false;
    }
    walk_table(state, -1, out, false);
    lua_pop(state, 1);
    sort_symbols(out);
    return true;
}

bool lua_value_members(std::string_view value_type, std::vector<LuaSymbol>& out) {
    out.clear();
    lua_State* state = reflect_state();
    if (state == nullptr) {
        return false;
    }
    if (value_type == "string") {
        lua_pushliteral(state, "");
        if (!lua_getmetatable(state, -1)) {
            lua_pop(state, 1);
            return false;
        }
        lua_getfield(state, -1, "__index");
        if (!lua_istable(state, -1)) {
            lua_pop(state, 3);
            return false;
        }
        walk_table(state, -1, out, true);
        lua_pop(state, 3);
        sort_symbols(out);
        return true;
    }
    if (value_type == "vector") {
        lua_pushvector(state, 0.0f, 0.0f, 0.0f);
        if (!lua_getmetatable(state, -1)) {
            lua_pop(state, 1);
            return false;
        }
        lua_getfield(state, -1, "__index");
        if (!lua_isfunction(state, -1)) {
            lua_pop(state, 3);
            return false;
        }
        const char* names[] = {"x", "y", "z", "w"};
        for (const char* name : names) {
            lua_pushvalue(state, -1);
            lua_pushvalue(state, -4);
            lua_pushstring(state, name);
            if (lua_pcall(state, 2, 1, 0) == 0) {
                LuaSymbol symbol;
                symbol.name = name;
                symbol.type_name = "number";
                out.push_back(std::move(symbol));
                lua_pop(state, 1);
            } else {
                lua_pop(state, 1);
            }
        }
        lua_pop(state, 3);
        return !out.empty();
    }
    if (lua_class_known(std::string(value_type).c_str())) {
        std::vector<LuaField> fields;
        lua_class_members(std::string(value_type).c_str(), fields);
        for (const LuaField& field : fields) {
            if (field.name == nullptr) {
                continue;
            }
            LuaSymbol symbol;
            symbol.name = field.name;
            symbol.type_name = field.type_name != nullptr ? field.type_name : "";
            symbol.call = field.method;
            symbol.method = field.method;
            out.push_back(std::move(symbol));
        }
        return true;
    }
    return false;
}

bool lua_module_exports(std::string_view source, std::uint32_t module_id, const std::vector<LuaNode>& world, LuaShape& out) {
    out = LuaShape{};
    const std::uint64_t hash = hash_world(world);
    ExportCache& stored = cache();
    if (stored.hash == hash) {
        const auto found = stored.shapes.find(module_id);
        if (found != stored.shapes.end()) {
            out = found->second;
            return !out.fields.empty() || !out.class_name.empty() || !out.type_name.empty();
        }
    } else {
        stored.hash = hash;
        stored.shapes.clear();
    }
    lua_State* state = reflect_state();
    if (state == nullptr) {
        return false;
    }
    const LuaNode* module = nullptr;
    for (const LuaNode& node : world) {
        if (node.id == module_id) {
            module = &node;
            break;
        }
    }
    LuaNode local;
    if (module == nullptr) {
        local.id = module_id;
        local.class_name = "ModuleScript";
        local.source = std::string(source);
        module = &local;
    }
    std::vector<LuaNode> owned;
    const std::vector<LuaNode>* view = &world;
    if (module == &local) {
        owned = world;
        owned.push_back(local);
        view = &owned;
    }
    Job active;
    active.world = view;
    Job* previous = job;
    job = &active;
    lua_Callbacks* callbacks = lua_callbacks(state);
    const auto previous_interrupt = callbacks->interrupt;
    callbacks->interrupt = reflect_interrupt;
    reflect_steps = 100000;
    const bool ok = eval_module(state, *module) == 1 && !lua_isnil(state, -1);
    callbacks->interrupt = previous_interrupt;
    if (ok) {
        std::unordered_set<const void*> seen;
        snapshot_value(state, -1, out, 0, seen);
        lua_pop(state, 1);
        stored.shapes[module_id] = out;
    } else if (lua_gettop(state) > 0) {
        lua_pop(state, 1);
    }
    for (const auto& entry : active.returns) {
        lua_unref(state, entry.second);
    }
    lua_pushnil(state);
    lua_setfield(state, LUA_REGISTRYINDEX, "ae_dummies");
    job = previous;
    return ok;
}

}  // namespace engine_core
