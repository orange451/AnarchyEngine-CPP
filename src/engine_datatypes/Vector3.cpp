#include "Vector3.hpp"

#include "LuaApi.hpp"

#include "lualib.h"

#include <cmath>
#include <cstring>
#include <type_traits>

namespace engine_core {
namespace {

static_assert(std::is_same<LUA_VECTOR_TYPE, float>::value, "Vector3 is stored as float");

const char* kEnumItemMeta = "AE.EnumItem";

struct EnumItemUd {
    const char* type_name = nullptr;
    const char* name = nullptr;
    int value = 0;
};

// Roblox scales the tolerance by |component| + 1, using the receiver's component.
// An exact match is equal, including infinities.
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

const float* check_vector3(lua_State* state, int index) {
    const float* value = lua_tovector(state, index);
    if (value == nullptr) {
        luaL_typeerrorL(state, index, "Vector3");
    }
    return value;
}

void* matching_udata(lua_State* state, int index, const char* name) {
    void* data = lua_touserdata(state, index);
    if (data == nullptr || !lua_getmetatable(state, index)) {
        return nullptr;
    }
    luaL_getmetatable(state, name);
    const bool match = lua_rawequal(state, -1, -2) != 0;
    lua_pop(state, 2);
    return match ? data : nullptr;
}

const EnumItemUd* enum_item(lua_State* state, int index) {
    return static_cast<const EnumItemUd*>(matching_udata(state, index, kEnumItemMeta));
}

// `stored` is the name on the item ("NormalId"). `expected` is what scripts see ("Enum.NormalId").
const EnumItemUd* check_enum_item(lua_State* state, int index, const char* stored, const char* expected) {
    const EnumItemUd* item = enum_item(state, index);
    if (item == nullptr || item->type_name == nullptr || std::strcmp(item->type_name, stored) != 0) {
        luaL_typeerrorL(state, index, expected);
    }
    return item;
}

int vector3_abs(lua_State* state) {
    const float* value = check_vector3(state, 1);
    lua_pushvector(state, std::fabs(value[0]), std::fabs(value[1]), std::fabs(value[2]));
    return 1;
}

int vector3_ceil(lua_State* state) {
    const float* value = check_vector3(state, 1);
    lua_pushvector(state, std::ceil(value[0]), std::ceil(value[1]), std::ceil(value[2]));
    return 1;
}

int vector3_floor(lua_State* state) {
    const float* value = check_vector3(state, 1);
    lua_pushvector(state, std::floor(value[0]), std::floor(value[1]), std::floor(value[2]));
    return 1;
}

int vector3_sign(lua_State* state) {
    const float* value = check_vector3(state, 1);
    lua_pushvector(state, sign_of(value[0]), sign_of(value[1]), sign_of(value[2]));
    return 1;
}

int vector3_cross(lua_State* state) {
    const float* a = check_vector3(state, 1);
    const float* b = check_vector3(state, 2);
    lua_pushvector(state, a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]);
    return 1;
}

int vector3_dot(lua_State* state) {
    const float* a = check_vector3(state, 1);
    const float* b = check_vector3(state, 2);
    lua_pushnumber(state, double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2]);
    return 1;
}

int vector3_angle(lua_State* state) {
    const float* a = check_vector3(state, 1);
    const float* b = check_vector3(state, 2);
    const float* axis = nullptr;
    if (!lua_isnoneornil(state, 3)) {
        axis = check_vector3(state, 3);
    }
    const float cross_x = a[1] * b[2] - a[2] * b[1];
    const float cross_y = a[2] * b[0] - a[0] * b[2];
    const float cross_z = a[0] * b[1] - a[1] * b[0];
    const double sin_angle =
        std::sqrt(double(cross_x) * cross_x + double(cross_y) * cross_y + double(cross_z) * cross_z);
    const double cos_angle = double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2];
    double angle = std::atan2(sin_angle, cos_angle);
    if (axis != nullptr) {
        const double side = double(cross_x) * axis[0] + double(cross_y) * axis[1] + double(cross_z) * axis[2];
        if (side < 0.0) {
            angle = -angle;
        }
    }
    lua_pushnumber(state, angle);
    return 1;
}

int vector3_fuzzy_eq(lua_State* state) {
    const float* a = check_vector3(state, 1);
    const float* b = check_vector3(state, 2);
    const double epsilon = luaL_optnumber(state, 3, 1e-5);
    const bool equal = fuzzy_component(a[0], b[0], epsilon) && fuzzy_component(a[1], b[1], epsilon) &&
                       fuzzy_component(a[2], b[2], epsilon);
    lua_pushboolean(state, equal ? 1 : 0);
    return 1;
}

int vector3_lerp(lua_State* state) {
    const float* from = check_vector3(state, 1);
    const float* goal = check_vector3(state, 2);
    const double alpha = luaL_checknumber(state, 3);
    lua_pushvector(state, lerp_component(from[0], goal[0], alpha), lerp_component(from[1], goal[1], alpha),
                   lerp_component(from[2], goal[2], alpha));
    return 1;
}

int vector3_max(lua_State* state) {
    const float* a = check_vector3(state, 1);
    const float* b = check_vector3(state, 2);
    lua_pushvector(state, b[0] > a[0] ? b[0] : a[0], b[1] > a[1] ? b[1] : a[1], b[2] > a[2] ? b[2] : a[2]);
    return 1;
}

int vector3_min(lua_State* state) {
    const float* a = check_vector3(state, 1);
    const float* b = check_vector3(state, 2);
    lua_pushvector(state, b[0] < a[0] ? b[0] : a[0], b[1] < a[1] ? b[1] : a[1], b[2] < a[2] ? b[2] : a[2]);
    return 1;
}

// X/Y/Z are also answered by the VM before this runs. Magnitude, Unit, and the
// methods are not, so they live here. The method table is the closure upvalue.
int vector3_index(lua_State* state) {
    const float* value = check_vector3(state, 1);
    std::size_t length = 0;
    const char* name = luaL_checklstring(state, 2, &length);
    if (length == 1) {
        const int component = (name[0] | ' ') - 'x';
        if (component >= 0 && component < 3) {
            lua_pushnumber(state, value[component]);
            return 1;
        }
    }
    if (std::strcmp(name, "Magnitude") == 0) {
        const double magnitude =
            std::sqrt(double(value[0]) * value[0] + double(value[1]) * value[1] + double(value[2]) * value[2]);
        lua_pushnumber(state, magnitude);
        return 1;
    }
    if (std::strcmp(name, "Unit") == 0) {
        // A zero vector's unit is NaN on every component. 0/0 produces that.
        const double magnitude =
            std::sqrt(double(value[0]) * value[0] + double(value[1]) * value[1] + double(value[2]) * value[2]);
        lua_pushvector(state, static_cast<float>(value[0] / magnitude), static_cast<float>(value[1] / magnitude),
                       static_cast<float>(value[2] / magnitude));
        return 1;
    }
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_pushvalue(state, 2);
    lua_rawget(state, -2);
    if (lua_isfunction(state, -1)) {
        return 1;
    }
    luaL_error(state, "attempt to index %s with '%s'", luaL_typename(state, 1), name);
}

// Omitted components are 0. An explicit nil is not a number.
double new_component(lua_State* state, int index) {
    if (lua_isnone(state, index)) {
        return 0.0;
    }
    return luaL_checknumber(state, index);
}

int vector3_new(lua_State* state) {
    const double x = new_component(state, 1);
    const double y = new_component(state, 2);
    const double z = new_component(state, 3);
    lua_pushvector(state, static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
    return 1;
}

struct Normal {
    const char* name;
    int value;
    float x;
    float y;
    float z;
};

// Front points down -Z. Back is +Z.
const Normal kNormals[] = {
    {"Right", 0, 1.f, 0.f, 0.f},  {"Top", 1, 0.f, 1.f, 0.f},    {"Back", 2, 0.f, 0.f, 1.f},
    {"Left", 3, -1.f, 0.f, 0.f},  {"Bottom", 4, 0.f, -1.f, 0.f}, {"Front", 5, 0.f, 0.f, -1.f},
};

struct Axis {
    const char* name;
    int value;
    float x;
    float y;
    float z;
};

const Axis kAxes[] = {
    {"X", 0, 1.f, 0.f, 0.f},
    {"Y", 1, 0.f, 1.f, 0.f},
    {"Z", 2, 0.f, 0.f, 1.f},
};

int vector3_from_normal(lua_State* state) {
    const EnumItemUd* item = check_enum_item(state, 1, "NormalId", "Enum.NormalId");
    for (const Normal& normal : kNormals) {
        if (item->name != nullptr && std::strcmp(item->name, normal.name) == 0) {
            lua_pushvector(state, normal.x, normal.y, normal.z);
            return 1;
        }
    }
    luaL_argerror(state, 1, "Enum.NormalId expected");
}

int vector3_from_axis(lua_State* state) {
    const EnumItemUd* item = check_enum_item(state, 1, "Axis", "Enum.Axis");
    for (const Axis& axis : kAxes) {
        if (item->name != nullptr && std::strcmp(item->name, axis.name) == 0) {
            lua_pushvector(state, axis.x, axis.y, axis.z);
            return 1;
        }
    }
    luaL_argerror(state, 1, "Enum.Axis expected");
}

int enum_item_index(lua_State* state) {
    auto* item = static_cast<EnumItemUd*>(luaL_checkudata(state, 1, kEnumItemMeta));
    const char* key = luaL_checkstring(state, 2);
    if (key == nullptr || item == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    if (std::strcmp(key, "Name") == 0) {
        lua_pushstring(state, item->name != nullptr ? item->name : "");
        return 1;
    }
    if (std::strcmp(key, "Value") == 0) {
        lua_pushinteger(state, item->value);
        return 1;
    }
    if (std::strcmp(key, "EnumType") == 0) {
        lua_getglobal(state, "Enum");
        if (lua_istable(state, -1) && item->type_name != nullptr) {
            lua_getfield(state, -1, item->type_name);
            lua_remove(state, -2);
            return 1;
        }
        lua_pop(state, 1);
        lua_pushnil(state);
        return 1;
    }
    luaL_error(state, "attempt to index EnumItem with '%s'", key);
}

int enum_item_tostring(lua_State* state) {
    auto* item = static_cast<EnumItemUd*>(luaL_checkudata(state, 1, kEnumItemMeta));
    const char* type_name = item != nullptr && item->type_name != nullptr ? item->type_name : "";
    const char* name = item != nullptr && item->name != nullptr ? item->name : "";
    lua_pushfstring(state, "Enum.%s.%s", type_name, name);
    return 1;
}

void push_enum_item(lua_State* state, const char* type_name, const char* name, int value) {
    auto* item = static_cast<EnumItemUd*>(lua_newuserdata(state, sizeof(EnumItemUd)));
    item->type_name = type_name;
    item->name = name;
    item->value = value;
    luaL_getmetatable(state, kEnumItemMeta);
    lua_setmetatable(state, -2);
}

void install_enum_items(lua_State* state) {
    luaL_newmetatable(state, kEnumItemMeta);
    lua_pushcfunction(state, enum_item_index, "index");
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, enum_item_tostring, "tostring");
    lua_setfield(state, -2, "__tostring");
    lua_pushliteral(state, "EnumItem");
    lua_setfield(state, -2, "__type");
    lua_setreadonly(state, -1, 1);
    lua_pop(state, 1);
}

void install_enum(lua_State* state) {
    install_enum_items(state);
    lua_newtable(state);
    lua_newtable(state);
    for (const Normal& normal : kNormals) {
        push_enum_item(state, "NormalId", normal.name, normal.value);
        lua_setfield(state, -2, normal.name);
    }
    lua_setreadonly(state, -1, 1);
    lua_setfield(state, -2, "NormalId");

    lua_newtable(state);
    for (const Axis& axis : kAxes) {
        push_enum_item(state, "Axis", axis.name, axis.value);
        lua_setfield(state, -2, axis.name);
    }
    lua_setreadonly(state, -1, 1);
    lua_setfield(state, -2, "Axis");
    lua_setreadonly(state, -1, 1);
    lua_setglobal(state, "Enum");
}

void install_vector_metatable(lua_State* state) {
    lua_pushvector(state, 0.f, 0.f, 0.f);
    if (lua_getmetatable(state, -1) == 0) {
        lua_pop(state, 1);
        return;
    }
    lua_setreadonly(state, -1, 0);

    lua_newtable(state);
    const luaL_Reg methods[] = {
        {"Abs", vector3_abs},     {"Ceil", vector3_ceil}, {"Floor", vector3_floor}, {"Sign", vector3_sign},
        {"Cross", vector3_cross}, {"Dot", vector3_dot},   {"Angle", vector3_angle}, {"FuzzyEq", vector3_fuzzy_eq},
        {"Lerp", vector3_lerp},   {"Max", vector3_max},   {"Min", vector3_min},     {nullptr, nullptr},
    };
    for (const luaL_Reg* method = methods; method->func != nullptr; ++method) {
        lua_pushcfunction(state, method->func, method->name);
        lua_setfield(state, -2, method->name);
    }
    lua_setreadonly(state, -1, 1);
    lua_pushcclosure(state, vector3_index, "index", 1);
    lua_setfield(state, -2, "__index");
    // Every vector in this state, including vector.create, reports as Vector3.
    lua_pushliteral(state, "Vector3");
    lua_setfield(state, -2, "__type");
    lua_setreadonly(state, -1, 1);
    lua_pop(state, 2);
}

void install_vector3_library(lua_State* state) {
    lua_newtable(state);
    lua_pushcfunction(state, vector3_new, "new");
    lua_setfield(state, -2, "new");
    lua_pushcfunction(state, vector3_from_normal, "FromNormalId");
    lua_setfield(state, -2, "FromNormalId");
    lua_pushcfunction(state, vector3_from_axis, "FromAxis");
    lua_setfield(state, -2, "FromAxis");
    lua_pushvector(state, 0.f, 0.f, 0.f);
    lua_setfield(state, -2, "zero");
    lua_pushvector(state, 1.f, 1.f, 1.f);
    lua_setfield(state, -2, "one");
    lua_pushvector(state, 1.f, 0.f, 0.f);
    lua_setfield(state, -2, "xAxis");
    lua_pushvector(state, 0.f, 1.f, 0.f);
    lua_setfield(state, -2, "yAxis");
    lua_pushvector(state, 0.f, 0.f, 1.f);
    lua_setfield(state, -2, "zAxis");
    lua_setreadonly(state, -1, 1);
    lua_setglobal(state, "Vector3");
}

// The rows are built here, not at namespace scope. A constructor function can run
// before this file's dynamic initializers, and would copy rows with no names.
ANARCHY_LUA_REGISTER(register_vector3_lua) {
    const LuaField vector_fields[] = {
        lua_property("X", "number", false, nullptr, nullptr),
        lua_property("Y", "number", false, nullptr, nullptr),
        lua_property("Z", "number", false, nullptr, nullptr),
        lua_property("Magnitude", "number", false, nullptr, nullptr),
        lua_property("Unit", "Vector3", false, nullptr, nullptr),
        lua_method("Abs", "Vector3", nullptr),
        lua_method("Ceil", "Vector3", nullptr),
        lua_method("Floor", "Vector3", nullptr),
        lua_method("Sign", "Vector3", nullptr),
        lua_method("Cross", "Vector3", nullptr),
        lua_method("Angle", "number", nullptr),
        lua_method("Dot", "number", nullptr),
        lua_method("FuzzyEq", "boolean", nullptr),
        lua_method("Lerp", "Vector3", nullptr),
        lua_method("Max", "Vector3", nullptr),
        lua_method("Min", "Vector3", nullptr),
    };

    const LuaField enum_item_fields[] = {
        lua_property("Name", "string", false, nullptr, nullptr),
        lua_property("Value", "number", false, nullptr, nullptr),
        lua_property("EnumType", "table", false, nullptr, nullptr),
    };
    register_lua_class("Vector3", nullptr, vector_fields,
                       static_cast<int>(sizeof(vector_fields) / sizeof(vector_fields[0])));
    register_lua_class("EnumItem", nullptr, enum_item_fields,
                       static_cast<int>(sizeof(enum_item_fields) / sizeof(enum_item_fields[0])));
    lua_note_result("Vector3", "new", "Vector3", false);
    lua_note_result("Vector3", "FromNormalId", "Vector3", false);
    lua_note_result("Vector3", "FromAxis", "Vector3", false);
}

}  // namespace

void open_vector3(lua_State* state) {
    if (state == nullptr) {
        return;
    }
    install_enum(state);
    install_vector_metatable(state);
    install_vector3_library(state);
}

}  // namespace engine_core
