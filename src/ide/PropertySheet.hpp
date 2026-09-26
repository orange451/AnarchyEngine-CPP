#pragma once

#include "DataModel.hpp"

#include <string>
#include <vector>

namespace ide {

// What a Properties row edits. The class registry's type name picks it.
// ReadOnlyText is shown and never written.
enum class PropertyKind { String, Bool, Number, Vector3, Ref, ReadOnlyText };

// Instance rows come first in a fixed order, then Data rows by name.
enum class PropertyGroup { Instance, Data };

// One value in the panel. Only the fields for its kind mean anything.
// A Ref with ref == DataModel::kNoParent is nil.
struct PropertyValue {
    std::string text;
    bool flag = false;
    double number = 0;
    engine_core::Vec3 vec{};
    engine_core::InstanceId ref = engine_core::DataModel::kNoParent;

    bool nil_ref() const { return ref == engine_core::DataModel::kNoParent; }
};

// One property every selected instance has, with the same type.
// mixed is true when the instances do not all hold the same value. For a
// Vector3, axis_mixed says which components differ; value keeps the ones that
// agree. For a Ref, label is "Name (Class)" and path is the Names from the root
// down, both empty when the value is nil or mixed.
struct PropertyRow {
    std::string name;
    std::string type_name;
    PropertyKind kind = PropertyKind::String;
    PropertyGroup group = PropertyGroup::Data;
    bool writable = false;
    bool mixed = false;
    bool axis_mixed[3] = {false, false, false};
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

// One commit from a row. For a Vector3, axis 0..2 writes only that component
// and keeps each instance's other two. axis -1 writes the whole value.
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

// "Name (Class)" for a Ref value, and the Names from the root down. The caller
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
