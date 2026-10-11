#pragma once

#include "Color.hpp"
#include "PhysicsBase.hpp"
#include "brush/BrushGeometry.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine_core {

// One convex solid with a Material and texture alignment on each face, built
// and edited exactly (docs/superpowers/specs/2026-10-07-brush-core-design.md).
// A PhysicsBase (which see for Transform, Velocity, Anchored, Mass,
// LinearDamping, and GameObject) with these too:
//
// AngularVelocity  Vector3   as PhysicsObject. (0, 0, 0).
// Friction         number    as PhysicsObject: 0.6, not below 0.
// Bounciness       number    as PhysicsObject: 0, not below 0.
// AngularDamping   number    as PhysicsObject: 0, not below 0.
// CanCollide       boolean   true. False: no shapes, rays pass through.
// Color            Color3    white. Tints every face, over its Material.
// Transparency     number    0. Fades every face, over its Material.
// Faces            string    hidden; the faces as compact JSON. Scripts read
//                            and write faces through the methods
//                            (BrushBindings.cpp); assigning it raises.
//
// Anchored starts true. A new Brush is a 4-unit box.
//
// Every face edit goes through brush::build, so the faces are always a valid
// solid; a refused edit changes nothing. An edit is one SetProperty mutation
// of Faces, so undo, Changed("Faces"), Stop, and saving are the ordinary ones.
class Brush : public PhysicsBase {
public:
    static constexpr double kDefaultFriction = 0.6;
    static constexpr float kDefaultSize = 4.f;

    Brush(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);

    const char* class_name() const override { return "Brush"; }

    Vec3 angular_velocity() const { return angular_velocity_; }
    double friction() const { return friction_; }
    double bounciness() const { return bounciness_; }
    double angular_damping() const { return angular_damping_; }
    bool can_collide() const { return can_collide_; }
    ColorRgb color() const { return color_; }
    double transparency() const { return transparency_; }

    const std::vector<brush::Face>& faces() const { return faces_; }
    const brush::Shape& shape() const { return shape_; }
    // The render mesh, built on first ask after a change; shared and immutable.
    std::shared_ptr<const brush::Mesh> mesh() const;
    // Changes whenever faces or anything drawn changes; unique across Brushes.
    std::uint64_t revision() const { return revision_; }
    // Changes only when the solid's shape changes, not its texturing.
    std::uint64_t shape_revision() const { return shape_revision_; }
    const std::string& faces_json() const { return faces_json_; }

    // SimulationThread. Each returns why it refused, changing nothing.
    std::optional<std::string> set_angular_velocity(Vec3 velocity);
    std::optional<std::string> set_friction(double friction);
    std::optional<std::string> set_bounciness(double bounciness);
    std::optional<std::string> set_angular_damping(double damping);
    std::optional<std::string> set_can_collide(bool can_collide);
    std::optional<std::string> set_color(ColorRgb color);
    std::optional<std::string> set_transparency(double transparency);
    // Builds, then keeps the built faces (redundant ones dropped).
    std::optional<std::string> set_faces(std::vector<brush::Face> faces);
    // Keeps a built result; the caller ran brush::build. The solid is recentred
    // on the origin and the Transform moved to match, so nothing moves in the
    // world; moved, when given, gets the old local position of the new origin.
    std::optional<std::string> apply(brush::Built built, brush::DVec3* moved = nullptr);
    // Takes the faces exactly, not recentred: loading and undo.
    std::optional<std::string> set_faces_json(const std::string& json);

    void store_angular_velocity(Vec3 velocity) { angular_velocity_ = velocity; }

protected:
    void on_reuse() override;

private:
    void reset_faces();
    std::optional<std::string> keep_as_is(brush::Built built);
    void keep(brush::Built built, std::string json);

    Vec3 angular_velocity_{};
    double friction_ = kDefaultFriction;
    double bounciness_ = 0.0;
    double angular_damping_ = 0.0;
    bool can_collide_ = true;
    ColorRgb color_{};
    double transparency_ = 0.0;

    std::vector<brush::Face> faces_;
    brush::Shape shape_;
    std::string faces_json_;
    std::uint64_t revision_ = 0;
    std::uint64_t shape_revision_ = 0;
    mutable std::shared_ptr<const brush::Mesh> mesh_;
};

namespace brush {
// Faces as the compact JSON Brush saves: [{"p":[9],"m":"GUID","u":[3],"v":[3],
// "o":[2],"s":[2],"r":0}], fields at their defaults left out.
std::string faces_to_json(const std::vector<Face>& faces);
// Nullopt and an error when the text is not such a list.
std::optional<std::vector<Face>> faces_from_json(const std::string& text, std::string& error);
// A fresh revision number, unique in the process.
std::uint64_t next_revision();
}  // namespace brush

}  // namespace engine_core
