#include "Brush.hpp"

#include "Containment.hpp"
#include "LuaApi.hpp"
#include "PropertyBag.hpp"

#include <atomic>
#include <cmath>
#include <iterator>

namespace engine_core {

using namespace physics_detail;

namespace brush {

std::uint64_t next_revision() {
    static std::atomic<std::uint64_t> counter{0};
    return ++counter;
}

namespace {

JsonValue numbers(std::initializer_list<double> values) {
    std::vector<JsonValue> items;
    items.reserve(values.size());
    for (double value : values) {
        items.push_back(JsonValue::number(value));
    }
    return JsonValue::array(std::move(items));
}

bool read_numbers(const JsonValue* value, double* out, std::size_t count) {
    if (value == nullptr || !value->is_array() || value->items().size() != count) {
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const JsonValue& item = value->items()[i];
        if (!item.is_number() || !std::isfinite(item.as_number())) {
            return false;
        }
        out[i] = item.as_number();
    }
    return true;
}

}  // namespace

std::string faces_to_json(const std::vector<Face>& faces) {
    std::vector<JsonValue> items;
    items.reserve(faces.size());
    for (const Face& face : faces) {
        JsonValue object = JsonValue::object();
        object.set("p", numbers({face.p1.x, face.p1.y, face.p1.z, face.p2.x, face.p2.y, face.p2.z, face.p3.x,
                                 face.p3.y, face.p3.z}));
        if (!face.material.empty()) {
            object.set("m", JsonValue::string(face.material));
        }
        // Axes are written only when they are not what the face would get anyway.
        DVec3 u;
        DVec3 v;
        if (const std::optional<Plane> plane = plane_of(face)) {
            default_axes(plane->normal, u, v);
        }
        if (face.u_axis != u || face.v_axis != v) {
            object.set("u", numbers({face.u_axis.x, face.u_axis.y, face.u_axis.z}));
            object.set("v", numbers({face.v_axis.x, face.v_axis.y, face.v_axis.z}));
        }
        if (face.offset_u != 0.0 || face.offset_v != 0.0) {
            object.set("o", numbers({face.offset_u, face.offset_v}));
        }
        if (face.scale_u != 1.0 || face.scale_v != 1.0) {
            object.set("s", numbers({face.scale_u, face.scale_v}));
        }
        if (face.rotation != 0.0) {
            object.set("r", JsonValue::number(face.rotation));
        }
        items.push_back(std::move(object));
    }
    return compact_json(JsonValue::array(std::move(items)));
}

std::optional<std::vector<Face>> faces_from_json(const std::string& text, std::string& error) {
    JsonValue root;
    if (!parse_json(text, root, error)) {
        return std::nullopt;
    }
    if (!root.is_array()) {
        error = "Faces must be a list";
        return std::nullopt;
    }
    std::vector<Face> faces;
    faces.reserve(root.items().size());
    for (std::size_t index = 0; index < root.items().size(); ++index) {
        const JsonValue& item = root.items()[index];
        double p[9];
        if (!item.is_object() || !read_numbers(item.find("p"), p, 9)) {
            error = "face " + std::to_string(index + 1) + " needs nine numbers in p";
            return std::nullopt;
        }
        Face face;
        face.p1 = {p[0], p[1], p[2]};
        face.p2 = {p[3], p[4], p[5]};
        face.p3 = {p[6], p[7], p[8]};
        if (const std::optional<Plane> plane = plane_of(face)) {
            default_axes(plane->normal, face.u_axis, face.v_axis);
        }
        if (const JsonValue* material = item.find("m"); material != nullptr && material->is_string()) {
            face.material = material->as_string();
        }
        double axis[3];
        if (read_numbers(item.find("u"), axis, 3)) {
            face.u_axis = {axis[0], axis[1], axis[2]};
        }
        if (read_numbers(item.find("v"), axis, 3)) {
            face.v_axis = {axis[0], axis[1], axis[2]};
        }
        double pair[2];
        if (read_numbers(item.find("o"), pair, 2)) {
            face.offset_u = pair[0];
            face.offset_v = pair[1];
        }
        if (read_numbers(item.find("s"), pair, 2)) {
            face.scale_u = pair[0];
            face.scale_v = pair[1];
        }
        if (const JsonValue* rotation = item.find("r"); rotation != nullptr && rotation->is_number()) {
            face.rotation = rotation->as_number();
        }
        faces.push_back(std::move(face));
    }
    return faces;
}

}  // namespace brush

namespace {

LuaSlot string_slot(const std::string& text) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::String;
    slot.text = text;
    return slot;
}

LuaSlot color_slot(ColorRgb color) {
    LuaSlot slot;
    slot.kind = LuaSlot::Kind::Color;
    slot.color = color;
    return slot;
}

bool same_points(const std::vector<brush::Face>& a, const std::vector<brush::Face>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].p1 != b[i].p1 || a[i].p2 != b[i].p2 || a[i].p3 != b[i].p3) {
            return false;
        }
    }
    return true;
}

}  // namespace

Brush::Brush(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : PhysicsBase(tag, state, id) {
    default_anchored(true);
    reset_faces();
}

void Brush::reset_faces() {
    const double size = kDefaultSize;
    brush::Built built = brush::build(brush::make_box({size, size, size}));
    faces_ = std::move(built.faces);
    shape_ = std::move(built.shape);
    faces_json_ = brush::faces_to_json(faces_);
    mesh_.reset();
    revision_ = brush::next_revision();
    shape_revision_ = revision_;
}

std::shared_ptr<const brush::Mesh> Brush::mesh() const {
    if (mesh_ == nullptr) {
        mesh_ = std::make_shared<const brush::Mesh>(brush::build_mesh(faces_, shape_));
    }
    return mesh_;
}

void Brush::keep(brush::Built built, std::string json) {
    const bool reshaped = !same_points(faces_, built.faces);
    const std::string previous = std::move(faces_json_);
    faces_ = std::move(built.faces);
    shape_ = std::move(built.shape);
    faces_json_ = std::move(json);
    mesh_.reset();
    revision_ = brush::next_revision();
    if (reshaped) {
        shape_revision_ = revision_;
        mark_dirty(kDirtyShape | kDirtyMass);
    }
    note_property_change("Faces", string_slot(previous), string_slot(faces_json_));
}

std::optional<std::string> Brush::apply(brush::Built built) {
    require_gameplay_thread(*this);
    if (!built.ok()) {
        return std::move(built.error);
    }
    std::string json = brush::faces_to_json(built.faces);
    if (json == faces_json_) {
        return std::nullopt;
    }
    keep(std::move(built), std::move(json));
    return std::nullopt;
}

std::optional<std::string> Brush::set_faces(std::vector<brush::Face> faces) {
    return apply(brush::build(std::move(faces)));
}

std::optional<std::string> Brush::set_faces_json(const std::string& json) {
    require_gameplay_thread(*this);
    if (json == faces_json_) {
        return std::nullopt;
    }
    std::string error;
    std::optional<std::vector<brush::Face>> faces = brush::faces_from_json(json, error);
    if (!faces) {
        return "Faces: " + error;
    }
    return set_faces(std::move(*faces));
}

std::optional<std::string> Brush::set_angular_velocity(Vec3 velocity) {
    return set_vec("AngularVelocity", angular_velocity_, velocity, kDirtyVelocity);
}

std::optional<std::string> Brush::set_friction(double friction) {
    return set_number("Friction", friction_, std::isfinite(friction) ? std::max(friction, 0.0) : friction,
                      kDirtyMaterial);
}

std::optional<std::string> Brush::set_bounciness(double bounciness) {
    return set_number("Bounciness", bounciness_, std::isfinite(bounciness) ? std::max(bounciness, 0.0) : bounciness,
                      kDirtyMaterial);
}

std::optional<std::string> Brush::set_angular_damping(double damping) {
    return set_number("AngularDamping", angular_damping_, std::isfinite(damping) ? std::max(damping, 0.0) : damping,
                      kDirtyDamping);
}

std::optional<std::string> Brush::set_can_collide(bool can_collide) {
    require_gameplay_thread(*this);
    if (can_collide == can_collide_) {
        return std::nullopt;
    }
    can_collide_ = can_collide;
    mark_dirty(kDirtyShape);
    note_property_change("CanCollide", bool_slot(!can_collide), bool_slot(can_collide));
    return std::nullopt;
}

std::optional<std::string> Brush::set_color(ColorRgb color) {
    require_gameplay_thread(*this);
    if (!std::isfinite(color.r) || !std::isfinite(color.g) || !std::isfinite(color.b)) {
        return std::string("Color must be finite");
    }
    color.a = 1.f;
    if (same_color(color_, color)) {
        return std::nullopt;
    }
    const ColorRgb previous = color_;
    color_ = color;
    revision_ = brush::next_revision();
    note_property_change("Color", color_slot(previous), color_slot(color));
    return std::nullopt;
}

std::optional<std::string> Brush::set_transparency(double transparency) {
    require_gameplay_thread(*this);
    if (!std::isfinite(transparency)) {
        return std::string("Transparency must be a finite number");
    }
    if (transparency == transparency_) {
        return std::nullopt;
    }
    const double previous = transparency_;
    transparency_ = transparency;
    revision_ = brush::next_revision();
    note_property_change("Transparency", number_slot(previous), number_slot(transparency));
    return std::nullopt;
}

void Brush::on_reuse() {
    PhysicsBase::on_reuse();
    default_anchored(true);
    angular_velocity_ = {};
    friction_ = kDefaultFriction;
    bounciness_ = 0.0;
    angular_damping_ = 0.0;
    can_collide_ = true;
    color_ = {};
    transparency_ = 0.0;
    reset_faces();
}

namespace {

bool read_can_collide(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* body = dynamic_cast<const Brush*>(&object);
    if (body == nullptr) {
        return false;
    }
    out = bool_slot(body->can_collide());
    return true;
}

bool write_can_collide(DataModel&, DataModel& object, LuaSlot& in) {
    auto* body = dynamic_cast<Brush*>(&object);
    return body != nullptr && refuse(in, body->set_can_collide(in.flag));
}

bool read_anchored(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* body = dynamic_cast<const Brush*>(&object);
    if (body == nullptr) {
        return false;
    }
    out = bool_slot(body->anchored());
    return true;
}

bool write_anchored(DataModel&, DataModel& object, LuaSlot& in) {
    auto* body = dynamic_cast<Brush*>(&object);
    if (body == nullptr) {
        return false;
    }
    body->set_anchored(in.flag);
    return true;
}

bool read_color(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* body = dynamic_cast<const Brush*>(&object);
    if (body == nullptr) {
        return false;
    }
    out = color_slot(body->color());
    return true;
}

bool write_color(DataModel&, DataModel& object, LuaSlot& in) {
    auto* body = dynamic_cast<Brush*>(&object);
    return body != nullptr && refuse(in, body->set_color(in.color));
}

bool read_faces(DataModel&, DataModel& object, LuaSlot& out) {
    const auto* body = dynamic_cast<const Brush*>(&object);
    if (body == nullptr) {
        return false;
    }
    out = string_slot(body->faces_json());
    return true;
}

// Load, Stop, undo, and paste write here; scripts cannot (writable is false).
bool write_faces(DataModel&, DataModel& object, LuaSlot& in) {
    auto* body = dynamic_cast<Brush*>(&object);
    return body != nullptr && refuse(in, body->set_faces_json(in.text));
}

std::string number_json(double value) { return write_json(JsonValue::number(value)); }

ANARCHY_LUA_REGISTER(register_brush_lua) {
    static const std::string friction = number_json(Brush::kDefaultFriction);
    static const std::string faces = [] {
        const float size = Brush::kDefaultSize;
        return write_json(JsonValue::string(brush::faces_to_json(brush::build(brush::make_box({size, size, size})).faces)));
    }();
    LuaField faces_field = lua_hidden(lua_saved_property("Faces", "string", read_faces, write_faces, faces.c_str()));
    faces_field.writable = false;
    const LuaField fields[] = {
        lua_saved_property("AngularVelocity", "Vector3", read_vec<Brush, &Brush::angular_velocity>,
                           write_vec<Brush, &Brush::set_angular_velocity>, "[0,0,0]"),
        lua_slider(lua_saved_property("Friction", "number", read_number<Brush, &Brush::friction>,
                                      write_number<Brush, &Brush::set_friction>, friction.c_str()),
                   0.0, 1.0),
        lua_slider(lua_saved_property("Bounciness", "number", read_number<Brush, &Brush::bounciness>,
                                      write_number<Brush, &Brush::set_bounciness>, "0"),
                   0.0, 1.0),
        lua_slider(lua_saved_property("AngularDamping", "number", read_number<Brush, &Brush::angular_damping>,
                                      write_number<Brush, &Brush::set_angular_damping>, "0"),
                   0.0, 1.0),
        // A Brush starts anchored, so its saved default is true where PhysicsBase's is false.
        lua_saved_property("Anchored", "boolean", read_anchored, write_anchored, "true"),
        lua_saved_property("CanCollide", "boolean", read_can_collide, write_can_collide, "true"),
        lua_saved_property("Color", "Color3", read_color, write_color, "[1,1,1]"),
        lua_slider(lua_saved_property("Transparency", "number", read_number<Brush, &Brush::transparency>,
                                      write_number<Brush, &Brush::set_transparency>, "0"),
                   0.0, 1.0),
        faces_field,
    };
    register_lua_class("Brush", "PhysicsBase", fields, static_cast<int>(std::size(fields)));
    register_suited_parents("Brush", {"Workspace", "PVInstance"});
}

}  // namespace

}  // namespace engine_core
