#pragma once

#include "DataModel.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ide {

// A drag of instances: their ids in decimal, joined by commas. The Assets
// pane writes it; a reference row in Properties takes it.
inline constexpr const char* kInstanceDragFormat = "application/x-anarchy-instances";

// kInstanceDragFormat's text for ids: each in decimal, joined by commas.
std::string instance_drag_text(const std::vector<engine_core::InstanceId>& ids);
// The ids in that text, in order. Empty when any part is not a plain decimal
// id that fits an InstanceId, so a malformed drag is ignored. Never throws.
std::vector<engine_core::InstanceId> instance_drag_ids(std::string_view text);

// What a Properties row edits. The class registry's type name picks it.
// ReadOnlyText is shown and never written. Transform is a Matrix4, edited as
// its Position and its Orientation.
enum class PropertyKind { String, Bool, Number, Vector3, Color3, Ref, Transform, ReadOnlyText };

// A Transform row's parts: Position's X, Y, and Z, then Orientation's.
inline constexpr int kTransformParts = 6;

// Instance rows come first in a fixed order, then Data rows by name.
enum class PropertyGroup { Instance, Data };

// One value in the panel. Only the fields for its kind mean anything.
// A Ref with ref == DataModel::kNoParent is nil. A Transform keeps the matrix
// in transform, its translation in vec, and its Orientation in orientation.
struct PropertyValue {
    std::string text;
    bool flag = false;
    double number = 0;
    engine_core::Vec3 vec{};
    engine_core::InstanceId ref = engine_core::DataModel::kNoParent;
    engine_core::Color3 color{};
    engine_core::Matrix4 transform{};
    engine_core::Vec3 orientation{};

    bool nil_ref() const { return ref == engine_core::DataModel::kNoParent; }
};

// One property every selected instance has, with the same type.
// mixed is true when the instances do not all hold the same value. For a
// Vector3, axis_mixed says which components differ; value keeps the ones that
// agree. A Transform's axis_mixed covers its six parts, in kTransformParts
// order. For a Ref, label is the Name and path is the Names from the root
// down, both empty when the value is nil or mixed.
struct PropertyRow {
    std::string name;
    std::string type_name;
    PropertyKind kind = PropertyKind::String;
    PropertyGroup group = PropertyGroup::Data;
    bool writable = false;
    bool mixed = false;
    bool axis_mixed[kTransformParts] = {false, false, false, false, false, false};
    PropertyValue value;
    std::string label;
    std::string path;

    bool operator==(const PropertyRow& other) const;
    bool operator!=(const PropertyRow& other) const { return !(*this == other); }
    // Same name, type, and editor: the widget for one can show the other.
    bool same_slot(const PropertyRow& other) const;
};

// The intersection of the selection's properties. ids are the selected
// instances that are still alive, in selection order.
struct PropertySheet {
    std::vector<engine_core::InstanceId> ids;
    std::vector<PropertyRow> rows;

    bool operator==(const PropertySheet& other) const { return ids == other.ids && rows == other.rows; }
    bool operator!=(const PropertySheet& other) const { return !(*this == other); }
    const PropertyRow* find(const std::string& name) const;
};

// The editor for a registry type, or false when the panel does not show it.
bool property_kind_for(const std::string& type_name, PropertyKind& out);

// The caller holds the DataModel lock. Dead ids and the root are left out.
// A property is a row only when every instance has it with the same type.
// Source is left to the script editor.
PropertySheet read_sheet(engine_core::DataModel& world, const std::vector<engine_core::InstanceId>& selection);

// Orientation: the rotation of transform, scale left out, as degrees
// about X, Y, and Z, turned about Y first, then X, then Z.
engine_core::Vec3 transform_orientation(const engine_core::Matrix4& transform);
// transform turned to orientation, in those degrees. Its translation, and the
// length of each axis, stay.
engine_core::Matrix4 transform_with_orientation(const engine_core::Matrix4& transform, engine_core::Vec3 orientation);

// One commit from a row. For a Vector3, axis 0..2 writes only that component
// and keeps each instance's other two. For a Transform, axis 0..2 is one axis
// of value.vec, the Position, and 3..5 one axis of value.orientation; each
// instance keeps the rest of its own Transform. axis -1 writes the whole
// value, for a Transform value.transform.
struct PropertyEdit {
    std::string property;
    PropertyKind kind = PropertyKind::String;
    PropertyValue value;
    int axis = -1;
};

struct EditResult {
    std::size_t written = 0;
    bool rejected = false;
    std::string error;
};

// Writes the edit to every id that has the property, through the class's own
// setters, as one ChangeHistoryService recording named "Set <property>". An
// explicit recording, so a Properties edit during play is undoable too.
// Runs on the simulation thread. A Parent that would put any id under itself
// rejects the whole edit, as does an edit during undo or redo.
EditResult apply_edit(engine_core::DataModel& world, const std::vector<engine_core::InstanceId>& ids,
                      const PropertyEdit& edit);

// The Name of a Ref value, and the Names from the root down. The caller
// holds the DataModel lock.
std::string ref_label(const engine_core::DataModel& world, engine_core::InstanceId id);
std::string ref_path(const engine_core::DataModel& world, engine_core::InstanceId id);

// The shortest text that reads back as the same double, or the same float.
std::string format_number(double value);
std::string format_float(float value);
// Leading and trailing spaces are allowed. False for anything else that is not
// one finite number.
bool parse_number(const std::string& text, double& out);

}  // namespace ide
