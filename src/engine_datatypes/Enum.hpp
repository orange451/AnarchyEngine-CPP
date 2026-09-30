#pragma once

struct lua_State;

namespace engine_core {

// One item of an enum, as scripts see Enum.<Type>.<Name>.
struct EnumEntry {
    const char* name;
    int value;
};

// One enum under the Enum global. Values are Roblox's, so a script that
// compares .Value reads the same numbers it would there.
struct EnumType {
    const char* name;
    const EnumEntry* items;
    int count;
};

const EnumType& normal_id_enum();
const EnumType& axis_enum();
const EnumType& rotation_order_enum();
// Keys by their Roblox value. Letters are lowercase ASCII (A is 97), and
// Unknown is 0.
const EnumType& key_code_enum();
const EnumType& user_input_type_enum();
const EnumType& user_input_state_enum();

// Every type the Enum global holds, for script analysis to declare.
int enum_type_count();
const EnumType& enum_type_at(int index);

// Null when no item of the type has this value.
const char* enum_item_name(const EnumType& type, int value);

// Pushes Enum.<Type>.<Name>: the same userdata every time, so == holds and the
// item works as a table key. Pushes nil when the value has no item.
void push_enum_item(lua_State* state, const EnumType& type, int value);

// An argument that names an item of this type: the EnumItem itself, its name,
// or its value. Anything else is an argument error. Returns the item's value.
int check_enum_arg(lua_State* state, int index, const EnumType& type);

// Installs the EnumItem metatable and the Enum global with every type above.
void open_enum(lua_State* state);

}  // namespace engine_core
