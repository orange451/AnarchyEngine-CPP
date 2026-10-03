#pragma once

#include "InputRecord.hpp"
#include "types.hpp"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct lua_State;

namespace engine_core {

class DataModel;
struct EnumType;

// A value carried between a class's property and the Luau stack.
// The set of kinds stays small. Property names do not live here.
struct LuaSlot {
    // A Vec2 (a Vector2) keeps its x and y in vec, with z 0. An InputObject is
    // only ever an event's value, never a property's.
    enum class Kind { Nil, Bool, Number, String, Instance, Vec3, Color, Matrix4, Signal, Enum, Vec2, InputObject };
    Kind kind = Kind::Nil;
    bool flag = false;
    // A Number's value, or an Enum item's value.
    double number = 0;
    // An Enum's type. Null for every other kind.
    const EnumType* enum_type = nullptr;
    std::string text;
    InstanceId id = 0;
    Vec3 vec{};
    ColorRgb color{};
    Matrix4 transform{};
    // An InputObject's record.
    InputRecord input{};
    // Why a write refused the value, for the user. Empty when it did not say.
    std::string error;
};

using LuaRead = bool (*)(DataModel& world, DataModel& object, LuaSlot& out);
// False when the value was refused; the write may set in.error to say why.
using LuaWrite = bool (*)(DataModel& world, DataModel& object, LuaSlot& in);

// One argument a Signal passes to Connect, in order.
struct LuaParam {
    const char* name = nullptr;
    const char* type_name = nullptr;
};

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
    // GetService: the string argument is a name from register_lua_service.
    bool service_arg = false;
    // GetChildren: the result is a list of `type_name`.
    bool returns_list = false;
    // Connect: the first argument after self is a callback. Its parameters are
    // the receiver signal's `params`.
    bool callback_arg = false;
    // Arguments a Signal passes into that callback. Empty when the field is not a signal.
    const LuaParam* params = nullptr;
    int param_count = 0;
    // Signal phase, or a non-instance property tag (Connection.Connected).
    int tag = -1;
    // An event on each instance of the class (lua_event).
    bool event = false;
    bool blocked = false;
    LuaRead read = nullptr;
    LuaWrite write = nullptr;
    void* call = nullptr;
    // A saved property: a project save writes it when it differs from
    // default_json, a load reads it, and Stop puts it back, all through read
    // and write. See lua_saved_property.
    bool saved = false;
    const char* default_json = nullptr;
    // A number the Properties page shows as a slider from slider_min to
    // slider_max, beside a field that takes any value the write does. See
    // lua_slider. Equal bounds are a plain field.
    double slider_min = 0;
    double slider_max = 0;
    // An enum property: type_name is "EnumItem", and its slots are Kind::Enum
    // of this type. Saved as the item's name. See lua_saved_enum.
    const EnumType* enum_type = nullptr;
    // Properties shows the field only while the sibling property shown_when
    // (an enum property) holds an item whose value's bit is set in
    // shown_when_items. A display rule: the value is still saved, loaded,
    // and scriptable. See lua_shown_when.
    const char* shown_when = nullptr;
    std::uint32_t shown_when_items = 0;

    bool shown_for(int value) const { return value >= 0 && value < 32 && ((shown_when_items >> value) & 1u) != 0; }

    bool slider() const { return slider_max > slider_min; }
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

// A property the class keeps no other record of: the registry is the whole
// list. DataModel saves, loads, and restores it at Stop from read and write,
// by its type: number, boolean, string, Color3, or Vector3. default_json is
// the value a new instance has, as the JSON a save would write. The write
// validates and calls DataModel::note_property_change, which gives undo and
// Changed.
inline LuaField lua_saved_property(const char* name, const char* type_name, LuaRead read, LuaWrite write,
                                   const char* default_json) {
    LuaField field = lua_property(name, type_name, true, read, write);
    field.saved = true;
    field.default_json = default_json;
    return field;
}

// field, shown in Properties as a slider from min to max. The slider is only
// the UI's range: the write still decides what the property takes.
inline LuaField lua_slider(LuaField field, double min, double max) {
    field.slider_min = min;
    field.slider_max = max;
    return field;
}

// A saved property holding an item of `type`. A script may write the
// EnumItem, its name, or its value; default_json is the item's name as JSON,
// such as "\"Box\"".
inline LuaField lua_saved_enum(const char* name, const EnumType& type, LuaRead read, LuaWrite write,
                               const char* default_json) {
    LuaField field = lua_saved_property(name, "EnumItem", read, write, default_json);
    field.enum_type = &type;
    return field;
}

// field, shown in Properties only while the enum property `property` holds
// one of the items whose values are `values`, each from 0 to 31.
inline LuaField lua_shown_when(LuaField field, const char* property, std::initializer_list<int> values) {
    field.shown_when = property;
    field.shown_when_items = 0;
    for (int value : values) {
        field.shown_when_items |= 1u << value;
    }
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

// Every RunService signal passes its step's delta as `dt`.
inline const LuaParam kPhaseSignalArgs[] = {{"dt", "number"}};

inline LuaField lua_signal_member(const char* name, int phase, bool blocked) {
    LuaField field;
    field.name = name;
    field.type_name = "Signal";
    field.tag = phase;
    field.blocked = blocked;
    field.params = kPhaseSignalArgs;
    field.param_count = 1;
    return field;
}

// A signal on each instance of the class, such as a Button's Action. The
// engine fires it with DataModel::fire_event; its callbacks get no arguments.
inline LuaField lua_event(const char* name) {
    LuaField field;
    field.name = name;
    field.type_name = "Signal";
    field.event = true;
    return field;
}

// An event whose callbacks get these values, in order. fire_event must pass
// exactly these: one LuaSlot per param, of the kind its type_name names.
inline LuaField lua_event(const char* name, const LuaParam* params, int count) {
    LuaField field = lua_event(name);
    field.params = params;
    field.param_count = count;
    return field;
}

// Adds `fields` onto a class. `base` is another class name, or null.
// Safe during static initialization and safe if the base is registered later.
// Calling again adds more fields, so two libraries can each register part of
// one class (UserInputService's signals in engine_services, its methods in
// ScriptRuntime). A field whose name the class already has replaces the
// earlier one, and static initialization decides which ran first, so those
// parts must not share a name.
void register_lua_class(const char* class_name, const char* base, const LuaField* fields, int count);
// Moves on every registration below. Script analysis reloads its definitions
// when it moved, so a class registered after it started is known too.
std::uint64_t lua_registry_revision();

// A small number for a property name, the same for every class that has a
// property of that name, so an event or an undo step can carry it. Every
// registered property has one; 0 is no property. Any thread.
std::uint32_t lua_property_id(std::string_view name);
// The name for an id, or "" for one no property has.
const char* lua_property_name(std::uint32_t id);

// One operator a value's metatable implements, as script analysis types it.
// Each operand is a registered class name or number, or several joined by
// " | ". `right` is null for a unary operator. Operators are not members, so
// completion does not list them.
struct LuaOperator {
    const char* metamethod = nullptr;
    const char* left = nullptr;
    const char* right = nullptr;
    const char* result = nullptr;
};

// Adds operators onto a class. A later row for the same metamethod and
// operands replaces the earlier one; other operands add an overload. Safe
// during static initialization, like register_lua_class.
void register_lua_operators(const char* class_name, const LuaOperator* operators, int count);
// This class's operators only, in registration order.
void lua_class_operators(const char* class_name, std::vector<LuaOperator>& out);

// Base members first. A derived field with the same name replaces the base one.
void lua_class_members(const char* class_name, std::vector<LuaField>& out);
// The saved properties (lua_saved_property) of a class and its bases, in
// lua_class_members order. Kept per class until the registry moves, since a
// place capture asks for every instance. Any thread.
std::vector<LuaField> lua_saved_fields(const char* class_name);
// Fields registered on this class only. Inherited fields are not included.
void lua_class_own_members(const char* class_name, std::vector<LuaField>& out);
const char* lua_class_base(const char* class_name);
const LuaField* lua_class_find(const char* class_name, std::string_view name);
bool lua_class_known(const char* class_name);
// True when `class_name` is `ancestor` or registers `ancestor` as a base.
bool lua_class_inherits(const char* class_name, const char* ancestor);
void lua_class_names(std::vector<std::string>& out);
// True when some class registers a method with this name as resolves_child.
// Static analysis sees a call's method name before it knows the receiver class.
bool lua_method_resolves_child(std::string_view name);

// Names GetService accepts. The service name is also its class name.
void register_lua_service(const char* name);
bool lua_service_known(const char* name);
void lua_service_names(std::vector<std::string>& out);

// Classes Instance.new can construct. The factory builds one in `world`.
// Completion and the explorer insert list use these same names. A null result
// from lua_create_instance is an unknown class.
using LuaCreate = DataModel& (*)(DataModel& world);
void register_lua_creatable(const char* class_name, LuaCreate create);
bool lua_creatable_known(const char* class_name);
void lua_creatable_names(std::vector<std::string>& out);
DataModel* lua_create_instance(DataModel& world, const char* class_name);

// Runs at load so a class is registered even when no instance has been created.
// The object file that contains the class is what pulls the registrar in.
// MSVC has no constructor attribute. A pointer placed in .CRT$XCU is dropped
// when it has internal linkage, as it does in an anonymous namespace, so a
// dynamic initializer calls the registrar instead.
#if defined(_MSC_VER)
#define ANARCHY_LUA_REGISTER(fn)                            \
    static void fn();                                       \
    [[maybe_unused]] static const bool fn##_done = (fn(), true); \
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

// A global table script analysis should declare, such as task. Its members and
// their parameter types come from lua_symbol_doc. Note it next to the install.
void lua_note_host_library(const char* name);
void lua_host_library_names(std::vector<std::string>& out);

void lua_library_globals(std::vector<LuaSymbol>& out);
bool lua_library_members(std::string_view global_name, std::vector<LuaSymbol>& out);
bool lua_value_members(std::string_view lua_type, std::vector<LuaSymbol>& out);
LuaResult lua_function_result(std::string_view owner, std::string_view name);

// One parameter in a host function's documentation.
struct LuaDocParam {
    std::string name;
    std::string type_name;
};

// What the editor shows for a host library, function, or property.
// `found` is false when nothing was recorded for that name.
// `returns_nothing` means the function returns no value.
// `return_unknown` means it returns a value whose type is not a single named type.
struct LuaDoc {
    bool found = false;
    std::string summary;
    std::string return_type;
    bool returns_nothing = false;
    bool return_unknown = false;
    bool variadic = false;
    std::vector<LuaDocParam> params;
};

// `owner` is empty for a global such as `task` or `print`. A member is looked up
// as owner "task" and name "wait". A class member walks base classes, so a
// method recorded on DataModel is found on a Script.
LuaDoc lua_symbol_doc(std::string_view owner, std::string_view name);
// Member names documented for `owner`, in sorted order.
void lua_doc_names(std::string_view owner, std::vector<std::string>& out);

// Definition source for the script checker, built from the class registry,
// noted host libraries, and lua_symbol_doc. There is no separate definition file.
std::string lua_analysis_definitions();

// One instance the editor can see. parent is kNoParent for the root.
struct LuaNode {
    std::uint32_t id = 0;
    std::uint32_t parent = 0xffffffffu;
    std::string name;
    std::string class_name;
    std::string source;
};

}  // namespace engine_core
