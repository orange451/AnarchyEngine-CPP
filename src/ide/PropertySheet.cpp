#include "PropertySheet.hpp"

#include "ChangeHistoryService.hpp"
#include "LuaApi.hpp"
#include "PropertyBag.hpp"
#include "PropertyReflection.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace ide {
namespace {

using engine_core::DataModel;
using engine_core::InstanceId;
using engine_core::LuaField;
using engine_core::LuaSlot;

// Instance rows, in the order they are shown. Every other row is Data.
constexpr const char* kInstanceRows[] = {"Name", "Parent", "ClassName"};

int instance_rank(const std::string& name) {
    for (int index = 0; index < 3; ++index) {
        if (name == kInstanceRows[index]) {
            return index;
        }
    }
    return -1;
}

const char* class_of(const DataModel& world, InstanceId id) {
    const DataModel* object = id == world.id() ? &world : world.instance(id);
    if (object == nullptr) {
        return nullptr;
    }
    return object->class_name();
}

DataModel* object_of(DataModel& world, InstanceId id) { return id == world.id() ? &world : world.instance(id); }

// A field the panel can show: a property with a reader, of a type it knows.
bool shown(const LuaField& field, PropertyKind& kind) {
    if (field.method || field.read == nullptr || field.name == nullptr || field.type_name == nullptr) {
        return false;
    }
    // Multi-line source belongs to the script editor.
    if (std::string_view(field.name) == "Source") {
        return false;
    }
    // An EnumItem the registry gives no type for has no items to pick from.
    return property_kind_for(field.type_name, kind) && (kind != PropertyKind::Enum || field.enum_type != nullptr);
}

// False when field has a display rule its object does not meet: the enum
// property it names holds another item, or the object has no such property.
bool rule_met(DataModel& world, DataModel& object, const LuaField& field) {
    if (field.shown_when == nullptr) {
        return true;
    }
    const LuaField* gate = engine_core::lua_class_find(object.class_name(), field.shown_when);
    LuaSlot slot;
    if (gate == nullptr || gate->read == nullptr || !gate->read(world, object, slot)) {
        return false;
    }
    return slot.kind == LuaSlot::Kind::Enum && field.shown_for(static_cast<int>(slot.number));
}

// Degrees to a thousandth, so float noise in
// the matrix reads as the angle it was set to. -0 reads as 0.
float shown_angle(float degrees) {
    const float rounded = static_cast<float>(std::round(static_cast<double>(degrees) * 1000.0) / 1000.0);
    return rounded == 0.f ? 0.f : rounded;
}

float& vec_part(engine_core::Vec3& vec, int axis) { return axis == 0 ? vec.x : axis == 1 ? vec.y : vec.z; }

float vec_part(const engine_core::Vec3& vec, int axis) { return axis == 0 ? vec.x : axis == 1 ? vec.y : vec.z; }

bool read_value(DataModel& world, DataModel& object, const LuaField& field, PropertyKind kind, PropertyValue& out) {
    LuaSlot slot;
    if (!field.read(world, object, slot)) {
        return false;
    }
    switch (kind) {
    case PropertyKind::String:
        out.text = slot.text;
        return slot.kind == LuaSlot::Kind::String;
    case PropertyKind::Bool:
        out.flag = slot.flag;
        return slot.kind == LuaSlot::Kind::Bool;
    case PropertyKind::Number:
        out.number = slot.number;
        return slot.kind == LuaSlot::Kind::Number;
    case PropertyKind::Enum:
        out.number = slot.number;
        return slot.kind == LuaSlot::Kind::Enum && slot.enum_type == field.enum_type;
    case PropertyKind::Vector3:
        out.vec = slot.vec;
        return slot.kind == LuaSlot::Kind::Vec3;
    case PropertyKind::Color3:
        out.color = engine_core::Color3{slot.color.r, slot.color.g, slot.color.b};
        return slot.kind == LuaSlot::Kind::Color;
    case PropertyKind::Transform: {
        if (slot.kind != LuaSlot::Kind::Matrix4) {
            return false;
        }
        out.transform = slot.transform;
        out.vec = engine_core::matrix4_position(slot.transform);
        const engine_core::Vec3 angles = transform_orientation(slot.transform);
        out.orientation = {shown_angle(angles.x), shown_angle(angles.y), shown_angle(angles.z)};
        return true;
    }
    case PropertyKind::Ref:
        if (slot.kind == LuaSlot::Kind::Nil) {
            out.ref = DataModel::kNoParent;
            return true;
        }
        out.ref = slot.id;
        return slot.kind == LuaSlot::Kind::Instance;
    case PropertyKind::ReadOnlyText:
        switch (slot.kind) {
        case LuaSlot::Kind::Color:
            out.text = format_float(slot.color.r) + ", " + format_float(slot.color.g) + ", " +
                       format_float(slot.color.b);
            return true;
        case LuaSlot::Kind::String:
            out.text = slot.text;
            return true;
        case LuaSlot::Kind::Number:
            out.text = format_number(slot.number);
            return true;
        case LuaSlot::Kind::Bool:
            out.text = slot.flag ? "true" : "false";
            return true;
        default:
            return false;
        }
    }
    return false;
}

bool same_component(float a, float b) { return a == b; }

// Folds one more instance's value into the row. first is the first instance.
void merge(PropertyRow& row, const PropertyValue& next) {
    switch (row.kind) {
    case PropertyKind::String:
    case PropertyKind::ReadOnlyText:
        row.mixed = row.mixed || row.value.text != next.text;
        break;
    case PropertyKind::Bool:
        row.mixed = row.mixed || row.value.flag != next.flag;
        break;
    case PropertyKind::Number:
    case PropertyKind::Enum:
        row.mixed = row.mixed || row.value.number != next.number;
        break;
    case PropertyKind::Vector3:
        row.axis_mixed[0] = row.axis_mixed[0] || !same_component(row.value.vec.x, next.vec.x);
        row.axis_mixed[1] = row.axis_mixed[1] || !same_component(row.value.vec.y, next.vec.y);
        row.axis_mixed[2] = row.axis_mixed[2] || !same_component(row.value.vec.z, next.vec.z);
        row.mixed = row.axis_mixed[0] || row.axis_mixed[1] || row.axis_mixed[2];
        break;
    case PropertyKind::Transform:
        // Parts compare as shown: two Transforms that differ only past a
        // thousandth of a degree show one Orientation.
        for (int axis = 0; axis < 3; ++axis) {
            row.axis_mixed[axis] =
                row.axis_mixed[axis] || !same_component(vec_part(row.value.vec, axis), vec_part(next.vec, axis));
            row.axis_mixed[3 + axis] = row.axis_mixed[3 + axis] || !same_component(vec_part(row.value.orientation, axis),
                                                                                   vec_part(next.orientation, axis));
        }
        row.mixed = row.mixed || !engine_core::same_matrix4(row.value.transform, next.transform);
        break;
    case PropertyKind::Color3:
        row.mixed = row.mixed || row.value.color.r != next.color.r || row.value.color.g != next.color.g ||
                    row.value.color.b != next.color.b;
        break;
    case PropertyKind::Ref:
        row.mixed = row.mixed || row.value.ref != next.ref;
        break;
    }
}

// Mixed parts read as empty, so two sheets that differ only in a hidden
// first-instance value compare equal and the widgets do not churn.
void blank_mixed(PropertyRow& row) {
    if (row.kind == PropertyKind::Vector3) {
        float* parts[3] = {&row.value.vec.x, &row.value.vec.y, &row.value.vec.z};
        for (int axis = 0; axis < 3; ++axis) {
            if (row.axis_mixed[axis]) {
                *parts[axis] = 0.f;
            }
        }
        return;
    }
    if (row.kind == PropertyKind::Transform) {
        for (int axis = 0; axis < 3; ++axis) {
            if (row.axis_mixed[axis]) {
                vec_part(row.value.vec, axis) = 0.f;
            }
            if (row.axis_mixed[3 + axis]) {
                vec_part(row.value.orientation, axis) = 0.f;
            }
        }
        if (row.mixed) {
            row.value.transform = engine_core::Matrix4{};
        }
        return;
    }
    if (row.mixed) {
        row.value = PropertyValue{};
    }
}

}  // namespace

std::string instance_drag_text(const std::vector<InstanceId>& ids) {
    std::string out;
    for (InstanceId id : ids) {
        if (!out.empty()) {
            out += ',';
        }
        out += std::to_string(id);
    }
    return out;
}

std::vector<InstanceId> instance_drag_ids(std::string_view text) {
    std::vector<InstanceId> out;
    if (text.empty()) {
        return out;
    }
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::string_view part = text.substr(start, comma - start);
        if (part.empty()) {
            return {};
        }
        std::uint64_t value = 0;
        for (char digit : part) {
            if (digit < '0' || digit > '9') {
                return {};
            }
            value = value * 10 + static_cast<std::uint64_t>(digit - '0');
            if (value > std::numeric_limits<InstanceId>::max()) {
                return {};
            }
        }
        out.push_back(static_cast<InstanceId>(value));
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return out;
}

bool PropertyRow::operator==(const PropertyRow& other) const {
    if (!same_slot(other) || writable != other.writable || mixed != other.mixed || label != other.label ||
        path != other.path) {
        return false;
    }
    for (int axis = 0; axis < kTransformParts; ++axis) {
        if (axis_mixed[axis] != other.axis_mixed[axis]) {
            return false;
        }
    }
    return value.text == other.value.text && value.flag == other.value.flag && value.number == other.value.number &&
           value.vec.x == other.value.vec.x && value.vec.y == other.value.vec.y && value.vec.z == other.value.vec.z &&
           value.color.r == other.value.color.r && value.color.g == other.value.color.g &&
           value.color.b == other.value.color.b && value.ref == other.value.ref &&
           engine_core::same_matrix4(value.transform, other.value.transform) &&
           value.orientation.x == other.value.orientation.x && value.orientation.y == other.value.orientation.y &&
           value.orientation.z == other.value.orientation.z;
}

bool PropertyRow::same_slot(const PropertyRow& other) const {
    return name == other.name && type_name == other.type_name && kind == other.kind && group == other.group &&
           slider_min == other.slider_min && slider_max == other.slider_max && enum_type == other.enum_type;
}

const PropertyRow* PropertySheet::find(const std::string& name) const {
    for (const PropertyRow& row : rows) {
        if (row.name == name) {
            return &row;
        }
    }
    return nullptr;
}

bool property_kind_for(const std::string& type_name, PropertyKind& out) {
    if (type_name == "string") {
        out = PropertyKind::String;
    } else if (type_name == "boolean") {
        out = PropertyKind::Bool;
    } else if (type_name == "number") {
        out = PropertyKind::Number;
    } else if (type_name == "Vector3") {
        out = PropertyKind::Vector3;
    } else if (type_name == "Instance" || type_name == "Instance?" || type_name == "DataModel" ||
               type_name == "DataModel?" || !engine_core::reference_class(type_name).empty()) {
        out = PropertyKind::Ref;
    } else if (type_name == "Color3") {
        out = PropertyKind::Color3;
    } else if (type_name == "Matrix4") {
        out = PropertyKind::Transform;
    } else if (type_name == "EnumItem") {
        out = PropertyKind::Enum;
    } else {
        return false;
    }
    return true;
}

PropertySheet read_sheet(DataModel& world, const std::vector<InstanceId>& selection, bool with_hidden) {
    PropertySheet sheet;
    for (InstanceId id : selection) {
        if (id != 0 && world.alive(id) && std::find(sheet.ids.begin(), sheet.ids.end(), id) == sheet.ids.end()) {
            sheet.ids.push_back(id);
        }
    }
    if (sheet.ids.empty()) {
        return sheet;
    }

    // Members per class, read once per sheet.
    std::unordered_map<std::string, std::vector<LuaField>> members;
    auto fields_of = [&members](const char* class_name) -> const std::vector<LuaField>& {
        const std::string key = class_name != nullptr ? class_name : "";
        auto found = members.find(key);
        if (found == members.end()) {
            std::vector<LuaField> fields;
            if (!key.empty()) {
                engine_core::lua_class_members(key.c_str(), fields);
            }
            found = members.emplace(key, std::move(fields)).first;
        }
        return found->second;
    };
    auto field_named = [](const std::vector<LuaField>& fields, const std::string& name) -> const LuaField* {
        for (const LuaField& field : fields) {
            if (field.name != nullptr && name == field.name) {
                return &field;
            }
        }
        return nullptr;
    };

    // Candidates are the first instance's fields. Each later instance must
    // have the same name with the same type, or the row is dropped.
    const std::vector<LuaField>& first = fields_of(class_of(world, sheet.ids.front()));
    for (const LuaField& field : first) {
        PropertyKind kind{};
        if (!shown(field, kind)) {
            continue;
        }
        PropertyRow row;
        row.name = field.name;
        row.type_name = field.type_name;
        row.kind = kind;
        row.group = instance_rank(row.name) >= 0 ? PropertyGroup::Instance : PropertyGroup::Data;
        row.writable = field.writable && field.write != nullptr && kind != PropertyKind::ReadOnlyText;
        if (kind == PropertyKind::Number && field.slider()) {
            row.slider_min = field.slider_min;
            row.slider_max = field.slider_max;
        }
        row.enum_type = field.enum_type;
        bool keep = true;
        bool have_first = false;
        for (InstanceId id : sheet.ids) {
            DataModel* object = object_of(world, id);
            const LuaField* own = object != nullptr ? field_named(fields_of(object->class_name()), row.name) : nullptr;
            if (own == nullptr || own->method || own->read == nullptr || own->type_name == nullptr ||
                row.type_name != own->type_name || own->enum_type != row.enum_type ||
                (!with_hidden && !rule_met(world, *object, *own))) {
                keep = false;
                break;
            }
            row.writable = row.writable && own->writable && own->write != nullptr;
            // Classes that disagree on the range get a plain field.
            if (own->slider_min != row.slider_min || own->slider_max != row.slider_max) {
                row.slider_min = 0;
                row.slider_max = 0;
            }
            // A service keeps its name and its place.
            if (object->is_service() && (row.name == "Name" || row.name == "Parent")) {
                row.writable = false;
            }
            PropertyValue value;
            if (!read_value(world, *object, *own, kind, value)) {
                keep = false;
                break;
            }
            if (!have_first) {
                row.value = std::move(value);
                have_first = true;
            } else {
                merge(row, value);
            }
        }
        if (!keep) {
            continue;
        }
        blank_mixed(row);
        if (row.kind == PropertyKind::Ref && !row.mixed && !row.value.nil_ref()) {
            row.label = ref_label(world, row.value.ref);
            row.path = ref_path(world, row.value.ref);
        }
        sheet.rows.push_back(std::move(row));
    }

    std::stable_sort(sheet.rows.begin(), sheet.rows.end(), [](const PropertyRow& a, const PropertyRow& b) {
        if (a.group != b.group) {
            return a.group == PropertyGroup::Instance;
        }
        if (a.group == PropertyGroup::Instance) {
            return instance_rank(a.name) < instance_rank(b.name);
        }
        return a.name < b.name;
    });
    return sheet;
}

namespace {

// Turns history off for a scope, so writes in it record nothing, and puts it back as it was.
class HistoryOff {
public:
    explicit HistoryOff(engine_core::ChangeHistoryService& history) : history_(history), was_(history.enabled()) {
        history_.set_enabled(false);
    }
    ~HistoryOff() { history_.set_enabled(was_); }
    HistoryOff(const HistoryOff&) = delete;
    HistoryOff& operator=(const HistoryOff&) = delete;

private:
    engine_core::ChangeHistoryService& history_;
    bool was_;
};

// apply_edit's work. record false writes with history off, as a live drag
// does; live, when given and still empty, takes each target's value first.
EditResult write_edit(DataModel& world, const std::vector<InstanceId>& ids, const PropertyEdit& edit, bool record,
                      LiveEdit* live);

}  // namespace

EditResult apply_edit(DataModel& world, const std::vector<InstanceId>& ids, const PropertyEdit& edit) {
    return write_edit(world, ids, edit, true, nullptr);
}

EditResult preview_edit(DataModel& world, const std::vector<InstanceId>& ids, const PropertyEdit& edit,
                        LiveEdit& live) {
    return write_edit(world, ids, edit, false, &live);
}

EditResult finish_live_edit(DataModel& world, const std::vector<InstanceId>& ids, const PropertyEdit& edit,
                            LiveEdit& live, bool commit) {
    {
        // Back to where the drag began, so the commit below records from there.
        HistoryOff off(world.history());
        for (auto& [id, slot] : live.before) {
            DataModel* object = world.alive(id) ? object_of(world, id) : nullptr;
            const LuaField* field =
                object != nullptr ? engine_core::lua_class_find(object->class_name(), live.property) : nullptr;
            if (field != nullptr && field->write != nullptr) {
                LuaSlot copy = slot;
                field->write(world, *object, copy);
            }
        }
    }
    live.before.clear();
    live.property.clear();
    return commit ? apply_edit(world, ids, edit) : EditResult{};
}

namespace {

EditResult write_edit(DataModel& world, const std::vector<InstanceId>& ids, const PropertyEdit& edit, bool record,
                      LiveEdit* live) {
    EditResult result;
    engine_core::ChangeHistoryService& history = world.history();
    if (history.applying_undo_redo()) {
        result.rejected = true;
        result.error = "Undo is running";
        return result;
    }
    if (edit.kind == PropertyKind::ReadOnlyText) {
        result.rejected = true;
        result.error = edit.property + " is read-only";
        return result;
    }

    // The instances that still have this property, writable, with a type that
    // takes this edit.
    struct Target {
        DataModel* object = nullptr;
        LuaField field;
    };
    std::vector<Target> targets;
    for (InstanceId id : ids) {
        if (id == 0 || !world.alive(id)) {
            continue;
        }
        DataModel* object = object_of(world, id);
        if (object == nullptr) {
            continue;
        }
        const LuaField* field = engine_core::lua_class_find(object->class_name(), edit.property);
        PropertyKind kind{};
        if (field == nullptr || field->method || !field->writable || field->write == nullptr ||
            field->type_name == nullptr || !property_kind_for(field->type_name, kind) || kind != edit.kind ||
            (kind == PropertyKind::Enum && field->enum_type == nullptr)) {
            continue;
        }
        targets.push_back({object, *field});
    }
    if (targets.empty()) {
        return result;
    }

    if (edit.kind == PropertyKind::Ref && !edit.value.nil_ref()) {
        const InstanceId parent = edit.value.ref;
        if (parent != 0 && !world.alive(parent)) {
            result.rejected = true;
            result.error = "That instance no longer exists";
            return result;
        }
    }
    // Checked for every target before any is written, so a refusal changes nothing.
    for (const Target& target : targets) {
        std::optional<std::string> error;
        if (edit.property == "Parent" && edit.kind == PropertyKind::Ref) {
            const InstanceId parent = edit.value.nil_ref() ? DataModel::kNoParent : edit.value.ref;
            error = world.parent_error(target.object->id(), parent);
        } else if (edit.property == "Name" && edit.kind == PropertyKind::String) {
            error = world.rename_error(target.object->id(), edit.value.text);
        } else if (edit.kind == PropertyKind::Ref && !edit.value.nil_ref()) {
            const std::string klass = engine_core::reference_class(target.field.type_name);
            const DataModel* picked = world.instance(edit.value.ref);
            if (!klass.empty() && (picked == nullptr ||
                                   !engine_core::lua_class_inherits(picked->class_name(), klass.c_str()))) {
                error = edit.property + " must be a " + klass;
            }
        }
        if (error) {
            result.rejected = true;
            result.error = std::move(*error);
            return result;
        }
    }

    // A drag's first write keeps what each instance held, so its end can put it back.
    if (live != nullptr && live->before.empty()) {
        live->property = edit.property;
        for (const Target& target : targets) {
            LuaSlot current;
            if (target.field.read != nullptr && target.field.read(world, *target.object, current)) {
                live->before.emplace_back(target.object->id(), std::move(current));
            }
        }
    }

    // A gesture left open by an earlier edit is its own waypoint, not part of this one.
    std::optional<std::string> recording;
    std::optional<HistoryOff> off;
    if (record) {
        history.end_gesture();
        recording = history.try_begin_recording("Set " + edit.property);
    } else {
        off.emplace(history);
    }
    for (Target& target : targets) {
        LuaSlot slot;
        switch (edit.kind) {
        case PropertyKind::String:
            slot.kind = LuaSlot::Kind::String;
            slot.text = edit.value.text;
            break;
        case PropertyKind::Bool:
            slot.kind = LuaSlot::Kind::Bool;
            slot.flag = edit.value.flag;
            break;
        case PropertyKind::Number:
            slot.kind = LuaSlot::Kind::Number;
            slot.number = edit.value.number;
            break;
        case PropertyKind::Enum:
            // The write refuses a value its type has no item for.
            slot.kind = LuaSlot::Kind::Enum;
            slot.enum_type = target.field.enum_type;
            slot.number = edit.value.number;
            break;
        case PropertyKind::Vector3: {
            slot.kind = LuaSlot::Kind::Vec3;
            slot.vec = edit.value.vec;
            if (edit.axis >= 0 && edit.axis < 3) {
                // One component: the other two stay what this instance has.
                LuaSlot current;
                if (target.field.read == nullptr || !target.field.read(world, *target.object, current) ||
                    current.kind != LuaSlot::Kind::Vec3) {
                    continue;
                }
                slot.vec = current.vec;
                const float component = edit.axis == 0 ? edit.value.vec.x : edit.axis == 1 ? edit.value.vec.y
                                                                                            : edit.value.vec.z;
                (edit.axis == 0 ? slot.vec.x : edit.axis == 1 ? slot.vec.y : slot.vec.z) = component;
            }
            break;
        }
        case PropertyKind::Transform: {
            slot.kind = LuaSlot::Kind::Matrix4;
            slot.transform = edit.value.transform;
            if (edit.axis >= 0 && edit.axis < kTransformParts) {
                // One part: the rest of this instance's Transform stays.
                LuaSlot current;
                if (target.field.read == nullptr || !target.field.read(world, *target.object, current) ||
                    current.kind != LuaSlot::Kind::Matrix4) {
                    continue;
                }
                slot.transform = current.transform;
                if (edit.axis < 3) {
                    slot.transform.m[12 + edit.axis] = vec_part(edit.value.vec, edit.axis);
                } else {
                    // The other two angles as held, not as shown, so they do not drift.
                    engine_core::Vec3 angles = transform_orientation(current.transform);
                    vec_part(angles, edit.axis - 3) = vec_part(edit.value.orientation, edit.axis - 3);
                    slot.transform = transform_with_orientation(current.transform, angles);
                }
            }
            break;
        }
        case PropertyKind::Color3:
            // A Color3 has no alpha, so the color is opaque, as a script's write makes it.
            slot.kind = LuaSlot::Kind::Color;
            slot.color = engine_core::ColorRgb{edit.value.color.r, edit.value.color.g, edit.value.color.b, 1.f};
            break;
        case PropertyKind::Ref:
            if (edit.value.nil_ref()) {
                slot.kind = LuaSlot::Kind::Nil;
            } else {
                slot.kind = LuaSlot::Kind::Instance;
                slot.id = edit.value.ref;
            }
            break;
        case PropertyKind::ReadOnlyText:
            continue;
        }
        if (target.field.write(world, *target.object, slot)) {
            ++result.written;
        }
    }
    if (recording) {
        history.finish_recording(*recording, engine_core::FinishRecordingOperation::Commit);
    }
    return result;
}

}  // namespace

engine_core::Vec3 transform_orientation(const engine_core::Matrix4& transform) {
    constexpr double kDegrees = 180.0 / 3.14159265358979323846;
    double radians[3] = {0, 0, 0};
    engine_core::matrix4_to_euler(transform, engine_core::RotationOrder::YXZ, radians);
    return {static_cast<float>(radians[0] * kDegrees), static_cast<float>(radians[1] * kDegrees),
            static_cast<float>(radians[2] * kDegrees)};
}

engine_core::Matrix4 transform_with_orientation(const engine_core::Matrix4& transform, engine_core::Vec3 orientation) {
    constexpr double kRadians = 3.14159265358979323846 / 180.0;
    engine_core::Matrix4 out =
        engine_core::matrix4_from_euler(orientation.x * kRadians, orientation.y * kRadians, orientation.z * kRadians,
                                        engine_core::RotationOrder::YXZ);
    for (int column = 0; column < 3; ++column) {
        const float* axis = transform.m + column * 4;
        const float length = static_cast<float>(std::sqrt(static_cast<double>(axis[0]) * axis[0] +
                                                          static_cast<double>(axis[1]) * axis[1] +
                                                          static_cast<double>(axis[2]) * axis[2]));
        for (int row = 0; row < 3; ++row) {
            out.m[column * 4 + row] *= length;
        }
    }
    for (int row = 0; row < 4; ++row) {
        out.m[12 + row] = transform.m[12 + row];
    }
    return out;
}

std::string ref_label(const DataModel& world, InstanceId id) {
    if (id == DataModel::kNoParent || (id != 0 && !world.alive(id))) {
        return {};
    }
    return world.name(id);
}

std::string ref_path(const DataModel& world, InstanceId id) {
    if (id == DataModel::kNoParent || (id != 0 && !world.alive(id))) {
        return {};
    }
    std::vector<std::string> names;
    InstanceId cursor = id;
    for (std::size_t guard = 0; guard <= DataModel::kMaxInstances; ++guard) {
        names.push_back(world.name(cursor));
        if (cursor == 0) {
            break;
        }
        cursor = world.parent(cursor);
        if (cursor == DataModel::kNoParent) {
            break;
        }
    }
    std::string out;
    for (auto it = names.rbegin(); it != names.rend(); ++it) {
        if (!out.empty()) {
            out += '.';
        }
        out += *it;
    }
    return out;
}

std::string format_number(double value) {
    if (!std::isfinite(value)) {
        return value != value ? "nan" : value > 0 ? "inf" : "-inf";
    }
    return engine_core::format_json_number(value);
}

std::string format_float(float value) {
    return format_number(engine_core::JsonValue::number_from_float(value).as_number());
}

bool parse_number(const std::string& text, double& out) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    if (begin == end) {
        return false;
    }
    const std::string trimmed = text.substr(begin, end - begin);
    char* stop = nullptr;
    const double value = std::strtod(trimmed.c_str(), &stop);
    if (stop == nullptr || *stop != '\0' || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

}  // namespace ide
