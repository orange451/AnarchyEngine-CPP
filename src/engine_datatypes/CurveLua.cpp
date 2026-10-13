#include "Color3.hpp"
#include "CurveLua.hpp"
#include "LuaApi.hpp"

#include "lualib.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace engine_core {
namespace {

const char* kNumberCurveMeta = "AE.NumberCurve";
const char* kColorCurveMeta = "AE.ColorCurve";

struct NumberCurveUd {
    std::shared_ptr<const NumberCurve> curve;
};

struct ColorCurveUd {
    std::shared_ptr<const ColorCurve> curve;
};

const char* number_mode_name(NumberCurveMode mode) {
    switch (mode) {
    case NumberCurveMode::Constant:
        return "Constant";
    case NumberCurveMode::Range:
        return "Range";
    case NumberCurveMode::Curve:
        return "Curve";
    case NumberCurveMode::CurveRange:
        return "CurveRange";
    }
    return "Constant";
}

const char* color_mode_name(ColorCurveMode mode) {
    switch (mode) {
    case ColorCurveMode::Constant:
        return "Constant";
    case ColorCurveMode::RandomBetween:
        return "RandomBetween";
    case ColorCurveMode::Gradient:
        return "Gradient";
    case ColorCurveMode::GradientRange:
        return "GradientRange";
    }
    return "Constant";
}

const NumberCurve& check_number_curve(lua_State* state, int index) {
    auto* ud = static_cast<NumberCurveUd*>(luaL_checkudata(state, index, kNumberCurveMeta));
    return *ud->curve;
}

const ColorCurve& check_color_curve(lua_State* state, int index) {
    auto* ud = static_cast<ColorCurveUd*>(luaL_checkudata(state, index, kColorCurveMeta));
    return *ud->curve;
}

// Rows of {time, value, interpolation?}. Raises naming what is wrong.
std::vector<NumberKey> read_number_keys(lua_State* state, int index) {
    luaL_checktype(state, index, LUA_TTABLE);
    std::vector<NumberKey> keys;
    const int count = lua_objlen(state, index);
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(state, index, i);
        if (!lua_istable(state, -1)) {
            luaL_error(state, "key %d must be a table {time, value, interpolation?}", i);
        }
        lua_rawgeti(state, -1, 1);
        lua_rawgeti(state, -2, 2);
        lua_rawgeti(state, -3, 3);
        if (!lua_isnumber(state, -3) || !lua_isnumber(state, -2)) {
            luaL_error(state, "key %d needs a number time and value", i);
        }
        NumberKey key;
        key.time = static_cast<float>(lua_tonumber(state, -3));
        key.value = static_cast<float>(lua_tonumber(state, -2));
        if (!lua_isnil(state, -1)) {
            const char* name = lua_tostring(state, -1);
            const auto interp = name != nullptr ? curve_interp_from_name(name) : std::nullopt;
            if (!interp) {
                luaL_error(state, "key %d has an unknown interpolation (Linear, Smooth, or Step)", i);
            }
            key.interp = *interp;
        }
        lua_pop(state, 4);
        keys.push_back(key);
    }
    return keys;
}

// Rows of {time, Color3, transparency?}.
std::vector<ColorKey> read_color_keys(lua_State* state, int index) {
    luaL_checktype(state, index, LUA_TTABLE);
    std::vector<ColorKey> keys;
    const int count = lua_objlen(state, index);
    for (int i = 1; i <= count; ++i) {
        lua_rawgeti(state, index, i);
        if (!lua_istable(state, -1)) {
            luaL_error(state, "key %d must be a table {time, Color3, transparency?}", i);
        }
        lua_rawgeti(state, -1, 1);
        lua_rawgeti(state, -2, 2);
        lua_rawgeti(state, -3, 3);
        const Color3* color = to_color3(state, -2);
        if (!lua_isnumber(state, -3) || color == nullptr) {
            luaL_error(state, "key %d needs a number time and a Color3", i);
        }
        if (!lua_isnil(state, -1) && !lua_isnumber(state, -1)) {
            luaL_error(state, "key %d's transparency must be a number", i);
        }
        ColorKey key;
        key.time = static_cast<float>(lua_tonumber(state, -3));
        key.color = ColorRgb{color->r, color->g, color->b, 1.f};
        key.transparency = lua_isnil(state, -1) ? 0.f : static_cast<float>(lua_tonumber(state, -1));
        lua_pop(state, 4);
        keys.push_back(key);
    }
    return keys;
}

ColorRgb check_rgb(lua_State* state, int index) {
    const Color3* color = to_color3(state, index);
    if (color == nullptr) {
        luaL_typeerrorL(state, index, "Color3");
    }
    return ColorRgb{color->r, color->g, color->b, 1.f};
}

float opt_float(lua_State* state, int index) {
    return lua_isnoneornil(state, index) ? 0.f : static_cast<float>(luaL_checknumber(state, index));
}

int number_curve_new(lua_State* state) {
    push_number_curve(state, std::make_shared<const NumberCurve>(NumberCurve::constant(
                                 static_cast<float>(luaL_checknumber(state, 1)))));
    return 1;
}

int number_curve_range(lua_State* state) {
    push_number_curve(state, std::make_shared<const NumberCurve>(NumberCurve::range(
                                 static_cast<float>(luaL_checknumber(state, 1)),
                                 static_cast<float>(luaL_checknumber(state, 2)))));
    return 1;
}

int number_curve_keys(lua_State* state) {
    std::string error;
    auto curve = NumberCurve::keys(read_number_keys(state, 1), error);
    if (!curve) {
        luaL_error(state, "%s", error.c_str());
    }
    push_number_curve(state, std::make_shared<const NumberCurve>(std::move(*curve)));
    return 1;
}

int number_curve_keys_range(lua_State* state) {
    std::string error;
    auto curve = NumberCurve::keys_range(read_number_keys(state, 1), read_number_keys(state, 2), error);
    if (!curve) {
        luaL_error(state, "%s", error.c_str());
    }
    push_number_curve(state, std::make_shared<const NumberCurve>(std::move(*curve)));
    return 1;
}

int number_curve_evaluate(lua_State* state) {
    const NumberCurve& curve = check_number_curve(state, 1);
    lua_pushnumber(state, curve.evaluate(static_cast<float>(luaL_checknumber(state, 2)), opt_float(state, 3)));
    return 1;
}

void push_number_keys(lua_State* state, const std::vector<NumberKey>& keys) {
    lua_createtable(state, static_cast<int>(keys.size()), 0);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        lua_createtable(state, 0, 3);
        lua_pushnumber(state, keys[i].time);
        lua_setfield(state, -2, "Time");
        lua_pushnumber(state, keys[i].value);
        lua_setfield(state, -2, "Value");
        lua_pushstring(state, curve_interp_name(keys[i].interp));
        lua_setfield(state, -2, "Interpolation");
        lua_rawseti(state, -2, static_cast<int>(i + 1));
    }
}

int number_curve_index(lua_State* state) {
    const NumberCurve& curve = check_number_curve(state, 1);
    const char* name = luaL_checkstring(state, 2);
    if (std::strcmp(name, "Mode") == 0) {
        lua_pushstring(state, number_mode_name(curve.mode()));
        return 1;
    }
    if (std::strcmp(name, "Min") == 0) {
        lua_pushnumber(state, curve.min());
        return 1;
    }
    if (std::strcmp(name, "Max") == 0) {
        lua_pushnumber(state, curve.max());
        return 1;
    }
    if (std::strcmp(name, "Keys") == 0) {
        push_number_keys(state, curve.keys_a());
        return 1;
    }
    if (std::strcmp(name, "Keys2") == 0) {
        push_number_keys(state, curve.keys_b());
        return 1;
    }
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_pushvalue(state, 2);
    lua_rawget(state, -2);
    if (lua_isfunction(state, -1)) {
        return 1;
    }
    luaL_error(state, "%s is not a valid member of NumberCurve", name);
}

int curve_newindex(lua_State* state) { luaL_error(state, "%s cannot be assigned to", luaL_checkstring(state, 2)); }

int number_curve_tostring(lua_State* state) {
    const NumberCurve& curve = check_number_curve(state, 1);
    switch (curve.mode()) {
    case NumberCurveMode::Constant:
        lua_pushfstring(state, "NumberCurve(%g)", static_cast<double>(curve.min()));
        break;
    case NumberCurveMode::Range:
        lua_pushfstring(state, "NumberCurve(%g..%g)", static_cast<double>(curve.min()),
                        static_cast<double>(curve.max()));
        break;
    default:
        lua_pushfstring(state, "NumberCurve(%s, %d keys)", number_mode_name(curve.mode()),
                        static_cast<int>(curve.keys_a().size()));
        break;
    }
    return 1;
}

int number_curve_eq(lua_State* state) {
    const auto a = to_number_curve(state, 1);
    const auto b = to_number_curve(state, 2);
    lua_pushboolean(state, a && b && *a == *b ? 1 : 0);
    return 1;
}

int color_curve_new(lua_State* state) {
    push_color_curve(state, std::make_shared<const ColorCurve>(ColorCurve::constant(check_rgb(state, 1), opt_float(state, 2))));
    return 1;
}

int color_curve_between(lua_State* state) {
    const ColorRgb a = check_rgb(state, 1);
    const float ta = static_cast<float>(luaL_checknumber(state, 2));
    const ColorRgb b = check_rgb(state, 3);
    const float tb = static_cast<float>(luaL_checknumber(state, 4));
    push_color_curve(state, std::make_shared<const ColorCurve>(ColorCurve::between(a, ta, b, tb)));
    return 1;
}

int color_curve_gradient(lua_State* state) {
    std::string error;
    auto curve = ColorCurve::gradient(read_color_keys(state, 1), error);
    if (!curve) {
        luaL_error(state, "%s", error.c_str());
    }
    push_color_curve(state, std::make_shared<const ColorCurve>(std::move(*curve)));
    return 1;
}

int color_curve_gradient_range(lua_State* state) {
    std::string error;
    auto curve = ColorCurve::gradient_range(read_color_keys(state, 1), read_color_keys(state, 2), error);
    if (!curve) {
        luaL_error(state, "%s", error.c_str());
    }
    push_color_curve(state, std::make_shared<const ColorCurve>(std::move(*curve)));
    return 1;
}

// A Color3 and the transparency.
int color_curve_evaluate(lua_State* state) {
    const ColorCurve& curve = check_color_curve(state, 1);
    const ColorRgb c = curve.evaluate(static_cast<float>(luaL_checknumber(state, 2)), opt_float(state, 3));
    push_color3(state, Color3{c.r, c.g, c.b});
    lua_pushnumber(state, 1.f - c.a);
    return 2;
}

void push_color_keys(lua_State* state, const std::vector<ColorKey>& keys) {
    lua_createtable(state, static_cast<int>(keys.size()), 0);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        lua_createtable(state, 0, 3);
        lua_pushnumber(state, keys[i].time);
        lua_setfield(state, -2, "Time");
        push_color3(state, Color3{keys[i].color.r, keys[i].color.g, keys[i].color.b});
        lua_setfield(state, -2, "Color");
        lua_pushnumber(state, keys[i].transparency);
        lua_setfield(state, -2, "Transparency");
        lua_rawseti(state, -2, static_cast<int>(i + 1));
    }
}

int color_curve_index(lua_State* state) {
    const ColorCurve& curve = check_color_curve(state, 1);
    const char* name = luaL_checkstring(state, 2);
    if (std::strcmp(name, "Mode") == 0) {
        lua_pushstring(state, color_mode_name(curve.mode()));
        return 1;
    }
    if (std::strcmp(name, "Keys") == 0) {
        push_color_keys(state, curve.keys_a());
        return 1;
    }
    if (std::strcmp(name, "Keys2") == 0) {
        push_color_keys(state, curve.keys_b());
        return 1;
    }
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_pushvalue(state, 2);
    lua_rawget(state, -2);
    if (lua_isfunction(state, -1)) {
        return 1;
    }
    luaL_error(state, "%s is not a valid member of ColorCurve", name);
}

int color_curve_tostring(lua_State* state) {
    const ColorCurve& curve = check_color_curve(state, 1);
    lua_pushfstring(state, "ColorCurve(%s, %d keys)", color_mode_name(curve.mode()),
                    static_cast<int>(curve.keys_a().size()));
    return 1;
}

int color_curve_eq(lua_State* state) {
    const auto a = to_color_curve(state, 1);
    const auto b = to_color_curve(state, 2);
    lua_pushboolean(state, a && b && *a == *b ? 1 : 0);
    return 1;
}

void install_metatable(lua_State* state, const char* meta, const char* type_name, lua_CFunction index,
                       lua_CFunction evaluate, lua_CFunction tostring, lua_CFunction eq) {
    luaL_newmetatable(state, meta);
    lua_newtable(state);
    lua_pushcfunction(state, evaluate, "Evaluate");
    lua_setfield(state, -2, "Evaluate");
    lua_setreadonly(state, -1, 1);
    lua_pushcclosure(state, index, "index", 1);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, curve_newindex, "__newindex");
    lua_setfield(state, -2, "__newindex");
    lua_pushcfunction(state, tostring, "__tostring");
    lua_setfield(state, -2, "__tostring");
    lua_pushcfunction(state, eq, "__eq");
    lua_setfield(state, -2, "__eq");
    lua_pushstring(state, type_name);
    lua_setfield(state, -2, "__type");
    lua_setreadonly(state, -1, 1);
    lua_pop(state, 1);
}

void install_library(lua_State* state, const char* global, const luaL_Reg* constructors) {
    lua_newtable(state);
    for (const luaL_Reg* entry = constructors; entry->func != nullptr; ++entry) {
        lua_pushcfunction(state, entry->func, entry->name);
        lua_setfield(state, -2, entry->name);
    }
    lua_setreadonly(state, -1, 1);
    lua_setglobal(state, global);
}

void register_curve_class(const char* name, const char* const* constructors, bool number) {
    const LuaField number_fields[] = {
        lua_property("Mode", "string", false, nullptr, nullptr), lua_property("Min", "number", false, nullptr, nullptr),
        lua_property("Max", "number", false, nullptr, nullptr),  lua_property("Keys", "{any}", false, nullptr, nullptr),
        lua_property("Keys2", "{any}", false, nullptr, nullptr), lua_method("Evaluate", "number", nullptr),
    };
    const LuaField color_fields[] = {
        lua_property("Mode", "string", false, nullptr, nullptr),
        lua_property("Keys", "{any}", false, nullptr, nullptr),
        lua_property("Keys2", "{any}", false, nullptr, nullptr),
        lua_method("Evaluate", nullptr, nullptr),
    };
    if (number) {
        register_lua_class(name, nullptr, number_fields, static_cast<int>(sizeof(number_fields) / sizeof(number_fields[0])));
    } else {
        register_lua_class(name, nullptr, color_fields, static_cast<int>(sizeof(color_fields) / sizeof(color_fields[0])));
    }
    for (const char* const* constructor = constructors; *constructor != nullptr; ++constructor) {
        lua_note_result(name, *constructor, name, false);
    }
    LuaOperator equality;
    equality.metamethod = "__eq";
    equality.left = name;
    equality.right = name;
    equality.result = "boolean";
    register_lua_operators(name, &equality, 1);
}

// Built here, not at namespace scope, for the reason Vector3.cpp gives.
ANARCHY_LUA_REGISTER(register_curve_lua) {
    const char* const number_constructors[] = {"new", "range", "keys", "keysRange", nullptr};
    const char* const color_constructors[] = {"new", "between", "gradient", "gradientRange", nullptr};
    register_curve_class("NumberCurve", number_constructors, true);
    register_curve_class("ColorCurve", color_constructors, false);
}

void destroy_number_curve(lua_State*, void* data) { static_cast<NumberCurveUd*>(data)->~NumberCurveUd(); }

void destroy_color_curve(lua_State*, void* data) { static_cast<ColorCurveUd*>(data)->~ColorCurveUd(); }

}  // namespace

void push_number_curve(lua_State* state, std::shared_ptr<const NumberCurve> curve) {
    void* data = lua_newuserdatadtor(state, sizeof(NumberCurveUd), destroy_number_curve);
    new (data) NumberCurveUd{std::move(curve)};
    luaL_getmetatable(state, kNumberCurveMeta);
    lua_setmetatable(state, -2);
}

std::shared_ptr<const NumberCurve> to_number_curve(lua_State* state, int index) {
    auto* ud = static_cast<NumberCurveUd*>(lua_touserdata(state, index));
    if (ud == nullptr || !lua_getmetatable(state, index)) {
        return nullptr;
    }
    luaL_getmetatable(state, kNumberCurveMeta);
    const bool same = lua_rawequal(state, -1, -2) != 0;
    lua_pop(state, 2);
    return same ? ud->curve : nullptr;
}

void push_color_curve(lua_State* state, std::shared_ptr<const ColorCurve> curve) {
    void* data = lua_newuserdatadtor(state, sizeof(ColorCurveUd), destroy_color_curve);
    new (data) ColorCurveUd{std::move(curve)};
    luaL_getmetatable(state, kColorCurveMeta);
    lua_setmetatable(state, -2);
}

std::shared_ptr<const ColorCurve> to_color_curve(lua_State* state, int index) {
    auto* ud = static_cast<ColorCurveUd*>(lua_touserdata(state, index));
    if (ud == nullptr || !lua_getmetatable(state, index)) {
        return nullptr;
    }
    luaL_getmetatable(state, kColorCurveMeta);
    const bool same = lua_rawequal(state, -1, -2) != 0;
    lua_pop(state, 2);
    return same ? ud->curve : nullptr;
}

void open_number_curve(lua_State* state) {
    install_metatable(state, kNumberCurveMeta, "NumberCurve", number_curve_index, number_curve_evaluate,
                      number_curve_tostring, number_curve_eq);
    const luaL_Reg constructors[] = {{"new", number_curve_new},
                                     {"range", number_curve_range},
                                     {"keys", number_curve_keys},
                                     {"keysRange", number_curve_keys_range},
                                     {nullptr, nullptr}};
    install_library(state, "NumberCurve", constructors);
}

void open_color_curve(lua_State* state) {
    install_metatable(state, kColorCurveMeta, "ColorCurve", color_curve_index, color_curve_evaluate,
                      color_curve_tostring, color_curve_eq);
    const luaL_Reg constructors[] = {{"new", color_curve_new},
                                     {"between", color_curve_between},
                                     {"gradient", color_curve_gradient},
                                     {"gradientRange", color_curve_gradient_range},
                                     {nullptr, nullptr}};
    install_library(state, "ColorCurve", constructors);
}

}  // namespace engine_core
