#include "RemoteValues.hpp"

#include "Color3.hpp"
#include "CurveLua.hpp"
#include "DataModel.hpp"
#include "Enum.hpp"
#include "Matrix4.hpp"
#include "ScriptBindings.hpp"
#include "Vector2.hpp"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <utility>

namespace engine_core {

void push_registered(lua_State* state, ScriptRuntime* runtime, const LuaSlot& slot, InstanceId id, std::uint32_t world);

DataModel* ScriptBindings::instance_arg(lua_State* state, int index) {
    const auto* ud = static_cast<InstanceUd*>(test_userdata(state, index, kInstanceMeta));
    ScriptRuntime* runtime = runtime_from(state);
    return ud == nullptr || runtime == nullptr ? nullptr : runtime->resolve_id(ud->id, ud->world);
}

namespace {

// Reads Lua values into RemoteValues. path holds the tables being read, outer
// first, so a table met again inside itself is a cycle; one reached twice by
// two routes is only copied twice.
struct Reader {
    lua_State* state;
    const char* method;
    std::string& error;
    std::vector<const void*> path;
    std::size_t values = 0;   // read so far, at every depth

    bool refuse(const std::string& what) {
        error = std::string(method) + " " + what;
        return false;
    }

    // Counts more values against kRemoteMaxValues, before anything is made for them.
    bool take(std::size_t count) {
        if (count > kRemoteMaxValues - values) {
            return refuse("message has more than " + std::to_string(kRemoteMaxValues) + " values");
        }
        values += count;
        return true;
    }

    // The value at index, which is absolute. Its own count is already taken.
    bool read(int index, RemoteValue& out) {
        LuaSlot& slot = out.slot;
        switch (lua_type(state, index)) {
        case LUA_TNIL:
        case LUA_TNONE:
            return true;
        case LUA_TBOOLEAN:
            slot.kind = LuaSlot::Kind::Bool;
            slot.flag = lua_toboolean(state, index) != 0;
            return true;
        case LUA_TNUMBER:
            slot.kind = LuaSlot::Kind::Number;
            slot.number = lua_tonumber(state, index);
            return true;
        case LUA_TSTRING: {
            std::size_t length = 0;
            const char* text = lua_tolstring(state, index, &length);
            slot.kind = LuaSlot::Kind::String;
            slot.text.assign(text != nullptr ? text : "", length);
            return true;
        }
        case LUA_TVECTOR: {
            const float* components = lua_tovector(state, index);
            slot.kind = LuaSlot::Kind::Vec3;
            slot.vec = Vec3{components[0], components[1], components[2]};
            return true;
        }
        case LUA_TTABLE:
            return read_table(index, out);
        case LUA_TUSERDATA:
            return read_userdata(index, slot);
        default:
            break;
        }
        return refuse(std::string("cannot send a ") + lua_typename(state, lua_type(state, index)));
    }

    bool read_userdata(int index, LuaSlot& slot) {
        if (test_userdata(state, index, kInstanceMeta) != nullptr) {
            // One that is gone travels as nil, as one destroyed in flight arrives.
            if (DataModel* object = ScriptBindings::instance_arg(state, index)) {
                slot.kind = LuaSlot::Kind::Instance;
                slot.id = object->id();
            }
            return true;
        }
        if (const Vec2* flat = to_vector2(state, index)) {
            slot.kind = LuaSlot::Kind::Vec2;
            slot.vec = Vec3{flat->x, flat->y, 0.f};
            return true;
        }
        if (read_color3(state, index, slot.color)) {
            slot.kind = LuaSlot::Kind::Color;
            return true;
        }
        if (const Matrix4* frame = to_matrix4(state, index)) {
            slot.kind = LuaSlot::Kind::Matrix4;
            slot.transform = *frame;
            return true;
        }
        int value = 0;
        if (const EnumType* type = to_enum_item(state, index, value)) {
            slot.kind = LuaSlot::Kind::Enum;
            slot.enum_type = type;
            slot.number = value;
            return true;
        }
        if (std::shared_ptr<const NumberCurve> curve = to_number_curve(state, index)) {
            slot.kind = LuaSlot::Kind::NumberCurve;
            slot.number_curve = std::move(curve);
            return true;
        }
        if (std::shared_ptr<const ColorCurve> curve = to_color_curve(state, index)) {
            slot.kind = LuaSlot::Kind::ColorCurve;
            slot.color_curve = std::move(curve);
            return true;
        }
        return refuse("cannot send a userdata");
    }

    bool read_table(int index, RemoteValue& out) {
        const void* table = lua_topointer(state, index);
        if (std::find(path.begin(), path.end(), table) != path.end()) {
            return refuse("cannot send a table that contains itself");
        }
        if (path.size() >= kRemoteMaxDepth) {
            return refuse("tables nest deeper than " + std::to_string(kRemoteMaxDepth));
        }
        // Each level holds a key, a value, and an element at most.
        if (!lua_checkstack(state, 4)) {
            return refuse("tables nest deeper than " + std::to_string(kRemoteMaxDepth));
        }
        path.push_back(table);
        auto result = std::make_shared<RemoteTable>();
        const int length = lua_objlen(state, index);
        if (!take(static_cast<std::size_t>(std::max(length, 0)))) {
            return false;
        }
        result->array.resize(static_cast<std::size_t>(std::max(length, 0)));
        for (int element = 1; element <= length; ++element) {
            lua_rawgeti(state, index, element);
            const bool ok = read(lua_gettop(state), result->array[static_cast<std::size_t>(element - 1)]);
            lua_pop(state, 1);
            if (!ok) {
                return false;
            }
        }
        // String keys travel; the array part is already read, and other keys drop.
        lua_pushnil(state);
        while (lua_next(state, index) != 0) {
            if (lua_type(state, -2) == LUA_TSTRING) {
                if (!take(1)) {
                    lua_pop(state, 2);
                    return false;
                }
                std::size_t size = 0;
                const char* key = lua_tolstring(state, -2, &size);
                result->fields.emplace_back(std::string(key != nullptr ? key : "", size), RemoteValue{});
                if (!read(lua_gettop(state), result->fields.back().second)) {
                    lua_pop(state, 2);
                    return false;
                }
            }
            lua_pop(state, 1);
        }
        path.pop_back();
        out.table = std::move(result);
        return true;
    }
};

// Pushes RemoteValues onto a script's stack.
struct Writer {
    lua_State* state;
    const DataModel& world;
    ScriptRuntime* runtime;

    void push(const RemoteValue& value) {
        if (value.table != nullptr) {
            push_table(*value.table);
            return;
        }
        const LuaSlot& slot = value.slot;
        if (slot.kind == LuaSlot::Kind::Instance) {
            if (runtime == nullptr || world.instance(slot.id) == nullptr) {
                lua_pushnil(state);
                return;
            }
        } else if (slot.kind == LuaSlot::Kind::Values || slot.kind == LuaSlot::Kind::Signal) {
            // Never a remote value; nothing sends one.
            lua_pushnil(state);
            return;
        }
        push_registered(state, runtime, slot, 0, 0);
    }

    // The table, a key and a value a level: remote_stack_slots made room.
    void push_table(const RemoteTable& table) {
        lua_createtable(state, static_cast<int>(table.array.size()), static_cast<int>(table.fields.size()));
        int element = 0;
        for (const RemoteValue& value : table.array) {
            ++element;
            push(value);
            lua_rawseti(state, -2, element);
        }
        for (const auto& [key, value] : table.fields) {
            lua_pushlstring(state, key.data(), key.size());
            push(value);
            lua_rawset(state, -3);
        }
    }
};

}  // namespace

bool remote_values_from_lua(lua_State* state, int first, const char* method, RemoteValues& out, std::string& error) {
    out.values.clear();
    const int top = lua_gettop(state);
    if (first > top) {
        return true;
    }
    Reader reader{state, method, error, {}};
    if (static_cast<std::size_t>(top - first + 1) > kRemoteMaxArgs) {
        return reader.refuse("sends more than " + std::to_string(kRemoteMaxArgs) + " arguments");
    }
    if (!reader.take(static_cast<std::size_t>(top - first + 1))) {
        return false;
    }
    out.values.resize(static_cast<std::size_t>(top - first + 1));
    for (int index = first; index <= top; ++index) {
        if (!reader.read(index, out.values[static_cast<std::size_t>(index - first)])) {
            lua_settop(state, top);
            out.values.clear();
            return false;
        }
    }
    return true;
}

int remote_stack_slots(const RemoteValues& values) {
    return static_cast<int>(values.values.size()) + 3 * (static_cast<int>(kRemoteMaxDepth) + 1);
}

int push_remote_values(lua_State* state, const DataModel& world, const RemoteValues& values) {
    const int count = static_cast<int>(values.values.size());
    Writer writer{state, world, runtime_from(state)};
    for (const RemoteValue& value : values.values) {
        writer.push(value);
    }
    return count;
}

}  // namespace engine_core
