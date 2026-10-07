#pragma once

#include <string_view>

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
const EnumType& mouse_behavior_enum();
// Cancel 0, Commit 1: Roblox's values. ScriptBindings maps them to
// engine_core::FinishRecordingOperation case by case, not by number.
const EnumType& finish_recording_operation_enum();
// Box 0, Sphere 1, Capsule 2, Hull 3, Custom 4, Cylinder 5, Cone 6, Wedge 7.
const EnumType& physics_shape_enum();
// Inverse 0, Linear 1, Exponential 2, None 3.
const EnumType& roll_off_mode_enum();
// Where a GuiBase puts its children: TopLeft 0, TopCenter 1, TopRight 2,
// CenterLeft 3, Center 4, CenterRight 5, BottomLeft 6, BottomCenter 7, BottomRight 8.
const EnumType& gui_alignment_enum();
// World 0, Local 1. Dragger.Space and Attachment.OffsetSpace.
const EnumType& transform_space_enum();
// transform_space_enum's items, by value.
enum class TransformSpace { World = 0, Local = 1 };
// Exclude 0, Include 1. RaycastParams.FilterType.
const EnumType& raycast_filter_type_enum();
// raycast_filter_type_enum's items, by value.
enum class RaycastFilterType { Exclude = 0, Include = 1 };
// X 0, Y 1, Z 2, XY 3, YZ 4, XZ 5.
const EnumType& dragger_handle_enum();
// How the 3D scene's edges are smoothed: None 0, FXAA 1. Lighting.Antialiasing.
const EnumType& antialiasing_mode_enum();
// antialiasing_mode_enum's items, by value.
enum class AntialiasingMode { None = 0, FXAA = 1 };
// How much an effect spends for how good it looks: Low 0, Medium 1, High 2.
// AmbientOcclusionEffect.Quality; later effects share it.
const EnumType& effect_quality_enum();
// effect_quality_enum's items, by value.
enum class EffectQuality { Low = 0, Medium = 1, High = 2 };

// Every type the Enum global holds, for script analysis to declare.
int enum_type_count();
const EnumType& enum_type_at(int index);

// Null when no item of the type has this value.
const char* enum_item_name(const EnumType& type, int value);
// The value of the item with this name, or -1 when the type has none.
int enum_item_value(const EnumType& type, std::string_view name);

// Pushes Enum.<Type>.<Name>: the same userdata every time, so == holds and the
// item works as a table key. Pushes nil when the value has no item.
void push_enum_item(lua_State* state, const EnumType& type, int value);

// An argument that names an item of this type: the EnumItem itself, its name,
// or its value. Anything else is an argument error. Returns the item's value.
int check_enum_arg(lua_State* state, int index, const EnumType& type);

// Installs the EnumItem metatable and the Enum global with every type above.
void open_enum(lua_State* state);

}  // namespace engine_core
