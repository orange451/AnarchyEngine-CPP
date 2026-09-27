#include "Vector2.hpp"

#include "LuaApi.hpp"

#include "lualib.h"

#include <cmath>
#include <cstring>

namespace engine_core {
namespace {

const char* kVector2Meta = "AE.Vector2";

// Roblox scales the tolerance by |component| + 1, as Vector3 does.
bool fuzzy_component(float left, float right, double epsilon) {
    const double a = left;
    const double b = right;
    return a == b || std::fabs(a - b) <= (std::fabs(a) + 1.0) * epsilon;
}

float sign_of(float value) {
    if (value > 0.f) {
        return 1.f;
    }
    if (value < 0.f) {
        return -1.f;
    }
    return 0.f;
}

float lerp_component(float from, float to, double alpha) {
    if (alpha == 1.0) {
        return to;
    }
    return static_cast<float>(from + (to - from) * alpha);
}

double magnitude_of(const Vec2& value) { return std::sqrt(double(value.x) * value.x + double(value.y) * value.y); }

const Vec2& check_vector2(lua_State* state, int index) {
    const Vec2* value = to_vector2(state, index);
    if (value == nullptr) {
        luaL_typeerrorL(state, index, "Vector2");
    }
    return *value;
}

void push(lua_State* state, float x, float y) { push_vector2(state, Vec2{x, y}); }

int vector2_abs(lua_State* state) {
    const Vec2& value = check_vector2(state, 1);
    push(state, std::fabs(value.x), std::fabs(value.y));
    return 1;
}

int vector2_ceil(lua_State* state) {
    const Vec2& value = check_vector2(state, 1);
    push(state, std::ceil(value.x), std::ceil(value.y));
    return 1;
}

int vector2_floor(lua_State* state) {
    const Vec2& value = check_vector2(state, 1);
    push(state, std::floor(value.x), std::floor(value.y));
    return 1;
}

int vector2_sign(lua_State* state) {
    const Vec2& value = check_vector2(state, 1);
    push(state, sign_of(value.x), sign_of(value.y));
    return 1;
}

// The z of the 3D cross product, a number as in Roblox.
int vector2_cross(lua_State* state) {
    const Vec2& a = check_vector2(state, 1);
    const Vec2& b = check_vector2(state, 2);
    lua_pushnumber(state, double(a.x) * b.y - double(a.y) * b.x);
    return 1;
}

int vector2_dot(lua_State* state) {
    const Vec2& a = check_vector2(state, 1);
    const Vec2& b = check_vector2(state, 2);
    lua_pushnumber(state, double(a.x) * b.x + double(a.y) * b.y);
    return 1;
}

// Unsigned unless isSigned is true. Signed is positive counterclockwise.
int vector2_angle(lua_State* state) {
    const Vec2& a = check_vector2(state, 1);
    const Vec2& b = check_vector2(state, 2);
    const bool is_signed = lua_toboolean(state, 3) != 0;
    const double cross = double(a.x) * b.y - double(a.y) * b.x;
    const double dot = double(a.x) * b.x + double(a.y) * b.y;
    lua_pushnumber(state, std::atan2(is_signed ? cross : std::fabs(cross), dot));
    return 1;
}

int vector2_fuzzy_eq(lua_State* state) {
    const Vec2& a = check_vector2(state, 1);
    const Vec2& b = check_vector2(state, 2);
    const double epsilon = luaL_optnumber(state, 3, 1e-5);
    lua_pushboolean(state, fuzzy_component(a.x, b.x, epsilon) && fuzzy_component(a.y, b.y, epsilon) ? 1 : 0);
    return 1;
}

int vector2_lerp(lua_State* state) {
    const Vec2& from = check_vector2(state, 1);
    const Vec2& goal = check_vector2(state, 2);
    const double alpha = luaL_checknumber(state, 3);
    push(state, lerp_component(from.x, goal.x, alpha), lerp_component(from.y, goal.y, alpha));
    return 1;
}

// Max and Min take any number of vectors after the receiver.
int vector2_extreme(lua_State* state, bool max) {
    Vec2 result = check_vector2(state, 1);
    const int count = lua_gettop(state);
    if (count < 2) {
        check_vector2(state, 2);
    }
    for (int index = 2; index <= count; ++index) {
        const Vec2& other = check_vector2(state, index);
        result.x = max ? (other.x > result.x ? other.x : result.x) : (other.x < result.x ? other.x : result.x);
        result.y = max ? (other.y > result.y ? other.y : result.y) : (other.y < result.y ? other.y : result.y);
    }
    push_vector2(state, result);
    return 1;
}

int vector2_max(lua_State* state) { return vector2_extreme(state, true); }

int vector2_min(lua_State* state) { return vector2_extreme(state, false); }

int vector2_index(lua_State* state) {
    const Vec2& value = check_vector2(state, 1);
    const char* name = luaL_checkstring(state, 2);
    if (std::strcmp(name, "X") == 0) {
        lua_pushnumber(state, value.x);
        return 1;
    }
    if (std::strcmp(name, "Y") == 0) {
        lua_pushnumber(state, value.y);
        return 1;
    }
    if (std::strcmp(name, "Magnitude") == 0) {
        lua_pushnumber(state, magnitude_of(value));
        return 1;
    }
    if (std::strcmp(name, "Unit") == 0) {
        // A zero vector's unit is NaN on both components, as with Vector3.
        const double magnitude = magnitude_of(value);
        push(state, static_cast<float>(value.x / magnitude), static_cast<float>(value.y / magnitude));
        return 1;
    }
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_pushvalue(state, 2);
    lua_rawget(state, -2);
    if (lua_isfunction(state, -1)) {
        return 1;
    }
    luaL_error(state, "%s is not a valid member of Vector2", name);
}

int vector2_newindex(lua_State* state) {
    luaL_error(state, "%s cannot be assigned to", luaL_checkstring(state, 2));
}

int vector2_tostring(lua_State* state) {
    const Vec2& value = check_vector2(state, 1);
    lua_pushnumber(state, value.x);
    luaL_tolstring(state, -1, nullptr);
    lua_pushliteral(state, ", ");
    lua_pushnumber(state, value.y);
    luaL_tolstring(state, -1, nullptr);
    lua_remove(state, -2);
    lua_concat(state, 3);
    return 1;
}

int vector2_eq(lua_State* state) {
    const Vec2* a = to_vector2(state, 1);
    const Vec2* b = to_vector2(state, 2);
    lua_pushboolean(state, a != nullptr && b != nullptr && a->x == b->x && a->y == b->y ? 1 : 0);
    return 1;
}

int vector2_add(lua_State* state) {
    const Vec2& a = check_vector2(state, 1);
    const Vec2& b = check_vector2(state, 2);
    push(state, a.x + b.x, a.y + b.y);
    return 1;
}

int vector2_sub(lua_State* state) {
    const Vec2& a = check_vector2(state, 1);
    const Vec2& b = check_vector2(state, 2);
    push(state, a.x - b.x, a.y - b.y);
    return 1;
}

int vector2_unm(lua_State* state) {
    const Vec2& value = check_vector2(state, 1);
    push(state, -value.x, -value.y);
    return 1;
}

// A Vector2 or a number on either side. A number stands for (n, n).
Vec2 scaling_operand(lua_State* state, int index) {
    if (lua_type(state, index) == LUA_TNUMBER) {
        const float value = static_cast<float>(lua_tonumber(state, index));
        return Vec2{value, value};
    }
    return check_vector2(state, index);
}

int vector2_mul(lua_State* state) {
    const Vec2 a = scaling_operand(state, 1);
    const Vec2 b = scaling_operand(state, 2);
    push(state, a.x * b.x, a.y * b.y);
    return 1;
}

int vector2_div(lua_State* state) {
    const Vec2 a = scaling_operand(state, 1);
    const Vec2 b = scaling_operand(state, 2);
    push(state, a.x / b.x, a.y / b.y);
    return 1;
}

int vector2_idiv(lua_State* state) {
    const Vec2 a = scaling_operand(state, 1);
    const Vec2 b = scaling_operand(state, 2);
    push(state, std::floor(a.x / b.x), std::floor(a.y / b.y));
    return 1;
}

// Omitted components are 0. An explicit nil is not a number.
float new_component(lua_State* state, int index) {
    if (lua_isnone(state, index)) {
        return 0.f;
    }
    return static_cast<float>(luaL_checknumber(state, index));
}

int vector2_new(lua_State* state) {
    push(state, new_component(state, 1), new_component(state, 2));
    return 1;
}

void install_vector2_metatable(lua_State* state) {
    luaL_newmetatable(state, kVector2Meta);

    lua_newtable(state);
    const luaL_Reg methods[] = {
        {"Abs", vector2_abs},     {"Ceil", vector2_ceil}, {"Floor", vector2_floor}, {"Sign", vector2_sign},
        {"Cross", vector2_cross}, {"Dot", vector2_dot},   {"Angle", vector2_angle}, {"FuzzyEq", vector2_fuzzy_eq},
        {"Lerp", vector2_lerp},   {"Max", vector2_max},   {"Min", vector2_min},     {nullptr, nullptr},
    };
    for (const luaL_Reg* method = methods; method->func != nullptr; ++method) {
        lua_pushcfunction(state, method->func, method->name);
        lua_setfield(state, -2, method->name);
    }
    lua_setreadonly(state, -1, 1);
    lua_pushcclosure(state, vector2_index, "index", 1);
    lua_setfield(state, -2, "__index");

    const luaL_Reg metamethods[] = {
        {"__newindex", vector2_newindex}, {"__tostring", vector2_tostring}, {"__eq", vector2_eq},
        {"__add", vector2_add},           {"__sub", vector2_sub},           {"__unm", vector2_unm},
        {"__mul", vector2_mul},           {"__div", vector2_div},           {"__idiv", vector2_idiv},
        {nullptr, nullptr},
    };
    for (const luaL_Reg* method = metamethods; method->func != nullptr; ++method) {
        lua_pushcfunction(state, method->func, method->name);
        lua_setfield(state, -2, method->name);
    }
    lua_pushliteral(state, "Vector2");
    lua_setfield(state, -2, "__type");
    lua_setreadonly(state, -1, 1);
    lua_pop(state, 1);
}

void install_vector2_library(lua_State* state) {
    lua_newtable(state);
    lua_pushcfunction(state, vector2_new, "new");
    lua_setfield(state, -2, "new");
    push(state, 0.f, 0.f);
    lua_setfield(state, -2, "zero");
    push(state, 1.f, 1.f);
    lua_setfield(state, -2, "one");
    push(state, 1.f, 0.f);
    lua_setfield(state, -2, "xAxis");
    push(state, 0.f, 1.f);
    lua_setfield(state, -2, "yAxis");
    lua_setreadonly(state, -1, 1);
    lua_setglobal(state, "Vector2");
}

// Built here, not at namespace scope, for the reason Vector3.cpp gives.
ANARCHY_LUA_REGISTER(register_vector2_lua) {
    const LuaField vector_fields[] = {
        lua_property("X", "number", false, nullptr, nullptr),
        lua_property("Y", "number", false, nullptr, nullptr),
        lua_property("Magnitude", "number", false, nullptr, nullptr),
        lua_property("Unit", "Vector2", false, nullptr, nullptr),
        lua_method("Abs", "Vector2", nullptr),
        lua_method("Ceil", "Vector2", nullptr),
        lua_method("Floor", "Vector2", nullptr),
        lua_method("Sign", "Vector2", nullptr),
        lua_method("Cross", "number", nullptr),
        lua_method("Angle", "number", nullptr),
        lua_method("Dot", "number", nullptr),
        lua_method("FuzzyEq", "boolean", nullptr),
        lua_method("Lerp", "Vector2", nullptr),
        lua_method("Max", "Vector2", nullptr),
        lua_method("Min", "Vector2", nullptr),
    };
    register_lua_class("Vector2", nullptr, vector_fields,
                       static_cast<int>(sizeof(vector_fields) / sizeof(vector_fields[0])));
    lua_note_result("Vector2", "new", "Vector2", false);
}

}  // namespace

void push_vector2(lua_State* state, Vec2 value) {
    auto* data = static_cast<Vec2*>(lua_newuserdata(state, sizeof(Vec2)));
    *data = value;
    luaL_getmetatable(state, kVector2Meta);
    lua_setmetatable(state, -2);
}

const Vec2* to_vector2(lua_State* state, int index) {
    void* data = lua_touserdata(state, index);
    if (data == nullptr || !lua_getmetatable(state, index)) {
        return nullptr;
    }
    luaL_getmetatable(state, kVector2Meta);
    const bool match = lua_rawequal(state, -1, -2) != 0;
    lua_pop(state, 2);
    return match ? static_cast<const Vec2*>(data) : nullptr;
}

void open_vector2(lua_State* state) {
    if (state == nullptr) {
        return;
    }
    install_vector2_metatable(state);
    install_vector2_library(state);
}

}  // namespace engine_core
