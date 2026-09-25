#pragma once

#include "types.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct lua_State;

namespace engine_core {

class DataModel;

// A value carried between a class's property and the Luau stack.
// The set of kinds stays small. Property names do not live here.
struct LuaSlot {
    enum class Kind { Nil, Bool, Number, String, Instance, Vec3, Color, Transform, Signal };
    Kind kind = Kind::Nil;
    bool flag = false;
    double number = 0;
    std::string text;
    InstanceId id = 0;
    Vec3 vec{};
    ColorRgb color{};
    Transform transform{};
};

using LuaRead = bool (*)(DataModel& world, DataModel& object, LuaSlot& out);
using LuaWrite = bool (*)(DataModel& world, DataModel& object, LuaSlot& in);

// One field or method a class exposes to Luau. `call` is the C function for a
// method, stored without including lua.h here. Null for properties.
struct LuaField {
    const char* name = nullptr;
    // Completion type. A registered class name, or string/number/boolean/nil.
    const char* type_name = nullptr;
    bool method = false;
    bool writable = false;
    // Instance.new / GetService: the first string argument names the result class.
    bool class_from_arg = false;
    // FindFirstChild: the string argument is a child of the receiver.
    bool resolves_child = false;
    // GetChildren: the result is a list of `type_name`.
    bool returns_list = false;
    // Signal phase, or a non-instance property tag (Connection.Connected).
    int tag = -1;
    bool blocked = false;
    LuaRead read = nullptr;
    LuaWrite write = nullptr;
    void* call = nullptr;
};

inline LuaField lua_property(const char* name, const char* type_name, bool writable, LuaRead read, LuaWrite write) {
    LuaField field;
    field.name = name;
    field.type_name = type_name;
    field.writable = writable;
    field.read = read;
    field.write = write;
    return field;
}

inline LuaField lua_method(const char* name, const char* type_name, void* call, bool class_from_arg = false,
                           bool resolves_child = false, bool returns_list = false) {
    LuaField field;
    field.name = name;
    field.type_name = type_name;
    field.method = true;
    field.class_from_arg = class_from_arg;
    field.resolves_child = resolves_child;
    field.returns_list = returns_list;
    field.call = call;
    return field;
}

inline LuaField lua_signal_member(const char* name, int phase, bool blocked) {
    LuaField field;
    field.name = name;
    field.type_name = "Signal";
    field.tag = phase;
    field.blocked = blocked;
    return field;
}

// Adds `fields` onto a class. `base` is another class name, or null.
// Safe during static initialization and safe if the base is registered later.
// Calling again appends. It does not replace fields already added.
void register_lua_class(const char* class_name, const char* base, const LuaField* fields, int count);

// Base members first. A derived field with the same name replaces the base one.
void lua_class_members(const char* class_name, std::vector<LuaField>& out);
const LuaField* lua_class_find(const char* class_name, std::string_view name);
bool lua_class_known(const char* class_name);
void lua_class_names(std::vector<std::string>& out);

// Runs at load so a class is registered even when no instance has been created.
// The object file that contains the class is what pulls the registrar in.
#if defined(_MSC_VER)
#pragma section(".CRT$XCU", read)
#define ANARCHY_LUA_REGISTER(fn)                                   \
    static void fn();                                              \
    __declspec(allocate(".CRT$XCU")) void (*fn##_ptr)() = fn;      \
    static void fn()
#else
#define ANARCHY_LUA_REGISTER(fn)                   \
    static void fn() __attribute__((constructor)); \
    static void fn()
#endif

// One name in a loaded library, or on a Luau value such as string or vector.
struct LuaSymbol {
    std::string name;
    std::string type_name;
    bool call = false;
    // True when the name is called with ':'. String methods are. task.wait is not.
    bool method = false;
};

// What a library function returns, recorded next to the function when it is
// installed. `class_from_arg` means the first string names a registered class.
struct LuaResult {
    std::string type_name;
    bool class_from_arg = false;
    bool known = false;
};

// The same library install the play VM uses. Reflection walks that state,
// so a library added there shows up in completion without a second list.
void open_host_libraries(lua_State* state);
void lua_note_result(const char* owner, const char* name, const char* type_name, bool class_from_arg);

void lua_library_globals(std::vector<LuaSymbol>& out);
bool lua_library_members(std::string_view global_name, std::vector<LuaSymbol>& out);
bool lua_value_members(std::string_view lua_type, std::vector<LuaSymbol>& out);
LuaResult lua_function_result(std::string_view owner, std::string_view name);

// One instance the editor can see. parent is kNoParent for the root.
struct LuaNode {
    std::uint32_t id = 0;
    std::uint32_t parent = 0xffffffffu;
    std::string name;
    std::string class_name;
    std::string source;
};

// Nested table or class produced by running a ModuleScript.
// `class_name` is set when the value is an instance of a registered class.
// `fields` are the keys a returned table actually has.
struct LuaShape {
    std::string type_name;
    std::string class_name;
    bool call = false;
    std::vector<std::pair<std::string, LuaShape>> fields;
};

// Runs `source` in the host environment with `world` behind game, script,
// FindFirstChild, and require. `module_id` is the ModuleScript being run.
// A chunk that errors leaves `out` empty and returns false.
bool lua_module_exports(std::string_view source, std::uint32_t module_id, const std::vector<LuaNode>& world,
                        LuaShape& out);

}  // namespace engine_core
