#include "Enum.hpp"

#include "LuaApi.hpp"
#include "LuaUserdata.hpp"

#include "lualib.h"

#include <cstring>

namespace engine_core {
namespace {

const char* kEnumItemMeta = "AE.EnumItem";

// Points at the static tables below, so an item never outlives its names.
struct EnumItemUd {
    const EnumType* type = nullptr;
    const char* name = nullptr;
    int value = 0;
};

const EnumEntry kNormalIds[] = {
    {"Right", 0}, {"Top", 1}, {"Back", 2}, {"Left", 3}, {"Bottom", 4}, {"Front", 5},
};

const EnumEntry kAxes[] = {
    {"X", 0},
    {"Y", 1},
    {"Z", 2},
};

// Matrix4.fromEulerAngles and ToEulerAngles. XYZ is Rx * Ry * Rz.
const EnumEntry kRotationOrders[] = {
    {"XYZ", 0}, {"XZY", 1}, {"YZX", 2}, {"YXZ", 3}, {"ZXY", 4}, {"ZYX", 5},
};

// Roblox's numbering: printable keys are their lowercase ASCII code.
const EnumEntry kKeyCodes[] = {
    {"Unknown", 0},
    {"Backspace", 8},
    {"Tab", 9},
    {"Return", 13},
    {"Pause", 19},
    {"Escape", 27},
    {"Space", 32},
    {"Quote", 39},
    {"Comma", 44},
    {"Minus", 45},
    {"Period", 46},
    {"Slash", 47},
    {"Zero", 48},
    {"One", 49},
    {"Two", 50},
    {"Three", 51},
    {"Four", 52},
    {"Five", 53},
    {"Six", 54},
    {"Seven", 55},
    {"Eight", 56},
    {"Nine", 57},
    {"Semicolon", 59},
    {"Equals", 61},
    {"LeftBracket", 91},
    {"BackSlash", 92},
    {"RightBracket", 93},
    {"Backquote", 96},
    {"A", 97},
    {"B", 98},
    {"C", 99},
    {"D", 100},
    {"E", 101},
    {"F", 102},
    {"G", 103},
    {"H", 104},
    {"I", 105},
    {"J", 106},
    {"K", 107},
    {"L", 108},
    {"M", 109},
    {"N", 110},
    {"O", 111},
    {"P", 112},
    {"Q", 113},
    {"R", 114},
    {"S", 115},
    {"T", 116},
    {"U", 117},
    {"V", 118},
    {"W", 119},
    {"X", 120},
    {"Y", 121},
    {"Z", 122},
    {"Delete", 127},
    {"KeypadZero", 256},
    {"KeypadOne", 257},
    {"KeypadTwo", 258},
    {"KeypadThree", 259},
    {"KeypadFour", 260},
    {"KeypadFive", 261},
    {"KeypadSix", 262},
    {"KeypadSeven", 263},
    {"KeypadEight", 264},
    {"KeypadNine", 265},
    {"KeypadPeriod", 266},
    {"KeypadDivide", 267},
    {"KeypadMultiply", 268},
    {"KeypadMinus", 269},
    {"KeypadPlus", 270},
    {"KeypadEnter", 271},
    {"KeypadEquals", 272},
    {"Up", 273},
    {"Down", 274},
    {"Right", 275},
    {"Left", 276},
    {"Insert", 277},
    {"Home", 278},
    {"End", 279},
    {"PageUp", 280},
    {"PageDown", 281},
    {"F1", 282},
    {"F2", 283},
    {"F3", 284},
    {"F4", 285},
    {"F5", 286},
    {"F6", 287},
    {"F7", 288},
    {"F8", 289},
    {"F9", 290},
    {"F10", 291},
    {"F11", 292},
    {"F12", 293},
    {"F13", 294},
    {"F14", 295},
    {"F15", 296},
    {"NumLock", 300},
    {"CapsLock", 301},
    {"ScrollLock", 302},
    {"RightShift", 303},
    {"LeftShift", 304},
    {"RightControl", 305},
    {"LeftControl", 306},
    {"RightAlt", 307},
    {"LeftAlt", 308},
    {"LeftSuper", 311},
    {"RightSuper", 312},
    {"Print", 316},
    {"Menu", 319},
};

const EnumEntry kUserInputTypes[] = {
    {"MouseButton1", 0}, {"MouseButton2", 1}, {"MouseButton3", 2}, {"MouseWheel", 3}, {"MouseMovement", 4},
    {"Touch", 7},        {"Keyboard", 8},     {"Focus", 9},        {"None", 22},
};

const EnumEntry kUserInputStates[] = {
    {"Begin", 0}, {"Change", 1}, {"End", 2}, {"Cancel", 3}, {"None", 4},
};

template <std::size_t N>
constexpr int count_of(const EnumEntry (&)[N]) {
    return static_cast<int>(N);
}

const EnumType kNormalIdType{"NormalId", kNormalIds, count_of(kNormalIds)};
const EnumType kAxisType{"Axis", kAxes, count_of(kAxes)};
const EnumType kRotationOrderType{"RotationOrder", kRotationOrders, count_of(kRotationOrders)};
const EnumType kKeyCodeType{"KeyCode", kKeyCodes, count_of(kKeyCodes)};
const EnumType kUserInputTypeType{"UserInputType", kUserInputTypes, count_of(kUserInputTypes)};
const EnumType kUserInputStateType{"UserInputState", kUserInputStates, count_of(kUserInputStates)};

const EnumType* const kTypes[] = {&kNormalIdType, &kAxisType,          &kRotationOrderType,
                                  &kKeyCodeType,  &kUserInputTypeType, &kUserInputStateType};

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
        if (lua_istable(state, -1) && item->type != nullptr) {
            lua_getfield(state, -1, item->type->name);
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
    const char* type_name = item != nullptr && item->type != nullptr ? item->type->name : "";
    const char* name = item != nullptr && item->name != nullptr ? item->name : "";
    lua_pushfstring(state, "Enum.%s.%s", type_name, name);
    return 1;
}

void new_enum_item(lua_State* state, const EnumType& type, const EnumEntry& entry) {
    push_userdata(state, EnumItemUd{&type, entry.name, entry.value}, kEnumItemMeta);
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

}  // namespace

const EnumType& normal_id_enum() { return kNormalIdType; }

const EnumType& axis_enum() { return kAxisType; }

const EnumType& rotation_order_enum() { return kRotationOrderType; }

const EnumType& key_code_enum() { return kKeyCodeType; }

const EnumType& user_input_type_enum() { return kUserInputTypeType; }

const EnumType& user_input_state_enum() { return kUserInputStateType; }

int enum_type_count() { return static_cast<int>(sizeof(kTypes) / sizeof(kTypes[0])); }

const EnumType& enum_type_at(int index) { return *kTypes[index]; }

const char* enum_item_name(const EnumType& type, int value) {
    for (int index = 0; index < type.count; ++index) {
        if (type.items[index].value == value) {
            return type.items[index].name;
        }
    }
    return nullptr;
}

void push_enum_item(lua_State* state, const EnumType& type, int value) {
    const char* name = enum_item_name(type, value);
    if (name == nullptr) {
        lua_pushnil(state);
        return;
    }
    // The installed item, not a new one: == on two userdata compares identity.
    lua_getglobal(state, "Enum");
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        lua_pushnil(state);
        return;
    }
    lua_getfield(state, -1, type.name);
    lua_remove(state, -2);
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        lua_pushnil(state);
        return;
    }
    lua_getfield(state, -1, name);
    lua_remove(state, -2);
}

int check_enum_arg(lua_State* state, int index, const EnumType& type) {
    if (const auto* item = static_cast<const EnumItemUd*>(test_userdata(state, index, kEnumItemMeta))) {
        if (item->type == &type) {
            return item->value;
        }
    } else if (lua_type(state, index) == LUA_TSTRING) {
        const char* name = lua_tostring(state, index);
        for (int entry = 0; entry < type.count; ++entry) {
            if (name != nullptr && std::strcmp(type.items[entry].name, name) == 0) {
                return type.items[entry].value;
            }
        }
    } else if (lua_type(state, index) == LUA_TNUMBER) {
        const int value = static_cast<int>(lua_tointeger(state, index));
        if (enum_item_name(type, value) != nullptr) {
            return value;
        }
    }
    const char* expected = lua_pushfstring(state, "Enum.%s", type.name);
    luaL_typeerrorL(state, index, expected);
}

void open_enum(lua_State* state) {
    if (state == nullptr) {
        return;
    }
    install_enum_items(state);
    lua_newtable(state);
    for (const EnumType* type : kTypes) {
        lua_newtable(state);
        for (int index = 0; index < type->count; ++index) {
            new_enum_item(state, *type, type->items[index]);
            lua_setfield(state, -2, type->items[index].name);
        }
        lua_setreadonly(state, -1, 1);
        lua_setfield(state, -2, type->name);
    }
    lua_setreadonly(state, -1, 1);
    lua_setglobal(state, "Enum");
}

namespace {

ANARCHY_LUA_REGISTER(register_enum_item_lua) {
    const LuaField enum_item_fields[] = {
        lua_property("Name", "string", false, nullptr, nullptr),
        lua_property("Value", "number", false, nullptr, nullptr),
        lua_property("EnumType", "table", false, nullptr, nullptr),
    };
    register_lua_class("EnumItem", nullptr, enum_item_fields,
                       static_cast<int>(sizeof(enum_item_fields) / sizeof(enum_item_fields[0])));
}

}  // namespace

}  // namespace engine_core
