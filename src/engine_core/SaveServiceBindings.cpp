// SaveService and the SaveStore datatype: SaveStores' Lua face
// (docs/superpowers/specs/2026-11-23-tweens-tags-gamepad-saves-design.md).
// A SaveStore names its store in the world's SaveStores, with the generation
// it was opened in. When the folder changes (another project opened), the
// stores are forgotten, and the handle raises "SaveStore is gone".

#include "ScriptBindings.hpp"

#include "Color3.hpp"
#include "DataModel.hpp"
#include "LuaApi.hpp"
#include "Matrix4.hpp"
#include "PropertyBag.hpp"
#include "SaveStore.hpp"
#include "Vector2.hpp"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine_core {

using namespace script_internal;

namespace {

constexpr const char* kSaveStoreMeta = "AE.SaveStore";
// How deep tables may nest in a saved value; the value itself is the first.
constexpr std::size_t kMaxDepth = 32;
constexpr std::size_t kMaxKeyBytes = 100;

struct SaveStoreUd {
    SaveStore* store = nullptr;
    std::uint64_t generation = 0;
};

SaveStores& saves_of(lua_State* state) {
    DataModel* game = ScriptBindings::game_of(state);
    if (game == nullptr) {
        luaL_error(state, "SaveService has no place");
    }
    return game->saves();
}

// The store self names, or raises.
SaveStore& store_arg(lua_State* state) {
    const auto* ud = static_cast<SaveStoreUd*>(luaL_checkudata(state, 1, kSaveStoreMeta));
    if (saves_of(state).generation() != ud->generation) {
        luaL_error(state, "SaveStore is gone");
    }
    return *ud->store;
}

std::string key_arg(lua_State* state) {
    std::size_t length = 0;
    const char* text = luaL_checklstring(state, 2, &length);
    if (length == 0 || length > kMaxKeyBytes) {
        luaL_error(state, "a save key is 1 to 100 bytes");
    }
    return std::string(text, length);
}

double finite(lua_State* state, double value) {
    if (!std::isfinite(value)) {
        luaL_error(state, "a saved number must be finite");
    }
    return value;
}

// {"$type": type, "value": [values...]}.
JsonValue typed(lua_State* state, const char* type, const float* values, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        finite(state, values[i]);
    }
    JsonValue out = JsonValue::object();
    out.set("$type", JsonValue::string(type));
    out.set("value", json_floats(values, count));
    return out;
}

JsonValue to_json(lua_State* state, int index, std::vector<const void*>& path);

// The table at index, an array when its keys are exactly 1 to n (an empty one
// too), else an object when every key is a string.
JsonValue table_to_json(lua_State* state, int index, std::vector<const void*>& path) {
    luaL_checkstack(state, 4, "a saved table nests too deep");
    int count = 0;
    bool strings = true;
    bool numbers = true;
    double largest = 0;
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        lua_pop(state, 1);
        ++count;
        if (lua_type(state, -1) == LUA_TSTRING) {
            numbers = false;
        } else if (lua_type(state, -1) == LUA_TNUMBER) {
            strings = false;
            const double key = lua_tonumber(state, -1);
            if (key < 1 || key != std::floor(key)) {
                numbers = false;
            }
            largest = std::max(largest, key);
        } else {
            strings = false;
            numbers = false;
        }
    }
    if (numbers && largest == count) {
        std::vector<JsonValue> items;
        items.reserve(static_cast<std::size_t>(count));
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(state, index, i);
            items.push_back(to_json(state, lua_gettop(state), path));
            lua_pop(state, 1);
        }
        return JsonValue::array(std::move(items));
    }
    if (!strings) {
        luaL_error(state, "a saved table's keys must be 1 to n (an array) or all strings");
    }
    JsonValue out = JsonValue::object();
    lua_pushnil(state);
    while (lua_next(state, index) != 0) {
        std::size_t length = 0;
        const char* text = lua_tolstring(state, -2, &length);
        std::string key(text, length);
        if (key == "$type") {
            luaL_error(state, "a saved table cannot have the key $type");
        }
        out.set(std::move(key), to_json(state, lua_gettop(state), path));
        lua_pop(state, 1);
    }
    return out;
}

// The value at index as SaveStore keeps it, or raises naming what it is.
// path is the tables it is inside.
JsonValue to_json(lua_State* state, int index, std::vector<const void*>& path) {
    switch (lua_type(state, index)) {
    case LUA_TBOOLEAN:
        return JsonValue::boolean(lua_toboolean(state, index) != 0);
    case LUA_TNUMBER:
        return JsonValue::number(finite(state, lua_tonumber(state, index)));
    case LUA_TSTRING: {
        std::size_t length = 0;
        const char* text = lua_tolstring(state, index, &length);
        return JsonValue::string(std::string(text, length));
    }
    case LUA_TVECTOR:
        return typed(state, "Vector3", lua_tovector(state, index), 3);
    case LUA_TTABLE: {
        const void* table = lua_topointer(state, index);
        if (std::find(path.begin(), path.end(), table) != path.end()) {
            luaL_error(state, "a saved table cannot contain itself");
        }
        if (path.size() >= kMaxDepth) {
            luaL_error(state, "a saved table cannot nest more than 32 deep");
        }
        path.push_back(table);
        JsonValue out = table_to_json(state, index, path);
        path.pop_back();
        return out;
    }
    default:
        break;
    }
    if (const Vec2* flat = to_vector2(state, index)) {
        const float axes[2] = {flat->x, flat->y};
        return typed(state, "Vector2", axes, 2);
    }
    if (const Color3* rgb = to_color3(state, index)) {
        const float channels[3] = {rgb->r, rgb->g, rgb->b};
        return typed(state, "Color3", channels, 3);
    }
    if (const Matrix4* frame = to_matrix4(state, index)) {
        return typed(state, "Matrix4", frame->m, 16);
    }
    if (test_userdata(state, index, kInstanceMeta) != nullptr) {
        luaL_error(state, "an Instance cannot be saved");
    }
    luaL_error(state, "a %s cannot be saved", luaL_typename(state, index));
}

// A typed value, pushed. False when value is not one, so it reads as a table.
bool push_typed(lua_State* state, const JsonValue& value) {
    const JsonValue* type = value.find("$type");
    const JsonValue* data = value.find("value");
    if (type == nullptr || data == nullptr || !type->is_string()) {
        return false;
    }
    const std::string& name = type->as_string();
    const std::size_t count = name == "Vector2"                      ? 2
                              : name == "Vector3" || name == "Color3" ? 3
                              : name == "Matrix4"                     ? 16
                                                                      : 0;
    std::vector<float> floats;
    if (count == 0 || !read_json_floats(*data, count, count, floats)) {
        return false;
    }
    if (name == "Vector2") {
        push_vector2(state, Vec2{floats[0], floats[1]});
    } else if (name == "Vector3") {
        lua_pushvector(state, floats[0], floats[1], floats[2]);
    } else if (name == "Color3") {
        push_color3(state, Color3{floats[0], floats[1], floats[2]});
    } else {
        Matrix4 frame;
        std::copy(floats.begin(), floats.end(), frame.m);
        push_matrix4(state, frame);
    }
    return true;
}

// A fresh Lua copy of value.
void push_json(lua_State* state, const JsonValue& value) {
    luaL_checkstack(state, 3, "a saved value nests too deep");
    switch (value.kind()) {
    case JsonValue::Kind::Bool:
        lua_pushboolean(state, value.as_bool() ? 1 : 0);
        return;
    case JsonValue::Kind::Number:
        lua_pushnumber(state, value.as_number());
        return;
    case JsonValue::Kind::String:
        lua_pushlstring(state, value.as_string().data(), value.as_string().size());
        return;
    case JsonValue::Kind::Array: {
        lua_createtable(state, static_cast<int>(value.items().size()), 0);
        int index = 1;
        for (const JsonValue& item : value.items()) {
            push_json(state, item);
            lua_rawseti(state, -2, index++);
        }
        return;
    }
    case JsonValue::Kind::Object:
        if (push_typed(state, value)) {
            return;
        }
        lua_createtable(state, 0, static_cast<int>(value.members().size()));
        for (const JsonValue::Member& member : value.members()) {
            lua_pushlstring(state, member.first.data(), member.first.size());
            push_json(state, member.second);
            lua_rawset(state, -3);
        }
        return;
    case JsonValue::Kind::Null:
        break;
    }
    lua_pushnil(state);
}

int save_service_open(lua_State* state) {
    return lua_guard(state, [&] {
        luaL_checkudata(state, 1, kServiceMeta);
        std::size_t length = 0;
        const char* name = luaL_checklstring(state, 2, &length);
        SaveStores& saves = saves_of(state);
        std::string error;
        SaveStore* store = saves.open(std::string_view(name, length), error);
        if (store == nullptr) {
            luaL_error(state, "%s", error.c_str());
        }
        auto* ud = static_cast<SaveStoreUd*>(lua_newuserdata(state, sizeof(SaveStoreUd)));
        *ud = SaveStoreUd{store, saves.generation()};
        luaL_getmetatable(state, kSaveStoreMeta);
        lua_setmetatable(state, -2);
        return 1;
    });
}

int save_store_get(lua_State* state) {
    return lua_guard(state, [&] {
        SaveStore& store = store_arg(state);
        if (const JsonValue* value = store.get(key_arg(state))) {
            push_json(state, *value);
        } else if (lua_gettop(state) >= 3) {
            lua_pushvalue(state, 3);
        } else {
            lua_pushnil(state);
        }
        return 1;
    });
}

int save_store_set(lua_State* state) {
    return lua_guard(state, [&] {
        SaveStore& store = store_arg(state);
        std::string key = key_arg(state);
        if (lua_isnoneornil(state, 3)) {
            store.remove(key);
            return 0;
        }
        std::vector<const void*> path;
        JsonValue value = to_json(state, 3, path);
        store.set(std::move(key), std::move(value));
        return 0;
    });
}

int save_store_remove(lua_State* state) {
    return lua_guard(state, [&] {
        SaveStore& store = store_arg(state);
        store.remove(key_arg(state));
        return 0;
    });
}

int save_store_keys(lua_State* state) {
    return lua_guard(state, [&] {
        const std::vector<std::string> keys = store_arg(state).keys();
        lua_createtable(state, static_cast<int>(keys.size()), 0);
        int index = 1;
        for (const std::string& key : keys) {
            lua_pushlstring(state, key.data(), key.size());
            lua_rawseti(state, -2, index++);
        }
        return 1;
    });
}

int save_store_clear(lua_State* state) {
    return lua_guard(state, [&] {
        store_arg(state).clear();
        return 0;
    });
}

int save_store_tostring(lua_State* state) {
    lua_pushliteral(state, "SaveStore");
    return 1;
}

ANARCHY_LUA_REGISTER(register_save_service_lua) {
    const LuaField methods[] = {
        lua_method("Open", "SaveStore", reinterpret_cast<void*>(&save_service_open)),
    };
    register_lua_class("SaveService", nullptr, methods, static_cast<int>(sizeof(methods) / sizeof(methods[0])));
    register_lua_service("SaveService");

    // The datatype, for completion and the type checker; the metatable does the work.
    const LuaField store_fields[] = {
        lua_method("Get", "any", nullptr),
        lua_method("Set", "nil", nullptr),
        lua_method("Remove", "nil", nullptr),
        lua_method("Keys", "string", nullptr, false, false, true),
        lua_method("Clear", "nil", nullptr),
    };
    register_lua_class("SaveStore", nullptr, store_fields,
                       static_cast<int>(sizeof(store_fields) / sizeof(store_fields[0])));
}

}  // namespace

void ScriptBindings::link_save_service_methods() {}

void open_save_store(lua_State* state) {
    static const luaL_Reg kMethods[] = {
        {"Get", &save_store_get},       {"Set", &save_store_set},     {"Remove", &save_store_remove},
        {"Keys", &save_store_keys},     {"Clear", &save_store_clear}, {nullptr, nullptr},
    };
    luaL_newmetatable(state, kSaveStoreMeta);
    lua_createtable(state, 0, 5);
    for (const luaL_Reg* entry = kMethods; entry->name != nullptr; ++entry) {
        lua_pushcfunction(state, entry->func, entry->name);
        lua_setfield(state, -2, entry->name);
    }
    lua_setreadonly(state, -1, true);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, save_store_tostring, "tostring");
    lua_setfield(state, -2, "__tostring");
    lua_pushliteral(state, "SaveStore");
    lua_setfield(state, -2, "__type");
    lua_setreadonly(state, -1, true);
    lua_pop(state, 1);
}

}  // namespace engine_core
