#pragma once

#include "PhysicsBase.hpp"

#include <algorithm>

namespace engine_core {

// A body for a walking character: a PhysicsBase (which see for Transform,
// Velocity, Anchored, Mass, and LinearDamping) that is an upright
// cylinder of Radius, standing from the hover gap above its feet up to
// Height. Its contact friction and bounciness are 0, so it slides along
// walls, and it never turns but about Y. While the place plays, PhysicsWorld
// holds its cylinder hover_gap() above the ground under it, so it walks up
// anything that tall, and slows it there by Friction.
//
// Transform's position is the feet: the ground point under the cylinder's
// middle. Any Transform written keeps only its position and its turn about Y.
// It moves the GameObject it is under, if any: it has no GameObject field.
// The Scale of the GameObject it moves does not size it.
//
// Friction    number   8, not below 0. Per second, only while OnGround: the
//                      speed across the ground, relative to the ground's
//                      own, decays by exp(-Friction * dt).
// Radius      number   0.5, at least kMinSize.
// Height      number   2, at least kMinSize. Ground to top of head.
// StepHeight  number   0.4, not below 0. How far the cylinder hovers, and so
//                      the tallest edge it walks up. Read back as written;
//                      hover_gap() caps it.
// MaxSlope    number   45, degrees in [0, 89]. Steeper ground slides.
// Enabled     boolean  true. Off, it is an ordinary body with its cylinder:
//                      it turns every way and tumbles, keeps a Transform's
//                      full rotation, neither hovers nor walks, ranks as any
//                      body for its assembly's root, and lets joints hold at
//                      its assembly (a driver welded into a car). Welded
//                      into an assembly it does not root, it weighs nothing
//                      there, and ranks as massless for the root; alone it
//                      keeps its Mass. On again, it stands up where it is.
// OnGround    boolean  read only: on ground no steeper than MaxSlope.
// IsSliding   boolean  read only: on ground steeper than MaxSlope.
//
// The first six are saved registry properties. OnGround and IsSliding are
// not saved, record no history, and read false while the place is not playing.
class PlayerController : public PhysicsBase {
public:
    static constexpr double kDefaultFriction = 8.0;
    static constexpr double kDefaultRadius = 0.5;
    static constexpr double kDefaultHeight = 2.0;
    static constexpr double kDefaultStepHeight = 0.4;
    static constexpr double kDefaultMaxSlope = 45.0;
    static constexpr double kMaxSlopeLimit = 89.0;

    PlayerController(DataModel::ChildTag tag, DataModel::State& state, InstanceId id) : PhysicsBase(tag, state, id) {}

    const char* class_name() const override { return "PlayerController"; }

    double friction() const { return friction_; }
    double radius() const { return radius_; }
    double height() const { return height_; }
    double step_height() const { return step_height_; }
    double max_slope() const { return max_slope_; }
    bool on_ground() const { return on_ground_ && simulation_running(); }
    bool is_sliding() const { return sliding_ && simulation_running(); }
    bool enabled() const { return enabled_; }

    // How far the cylinder's bottom stands above the feet: StepHeight, capped
    // so the cylinder is at least kMinSize tall.
    double hover_gap() const { return std::max(0.0, std::min(step_height_, height_ - kMinSize)); }

    // SimulationThread. Each returns why it refused the value, changing nothing.
    std::optional<std::string> set_transform(const Matrix4& transform) override;
    std::optional<std::string> set_friction(double friction);
    std::optional<std::string> set_radius(double radius);
    std::optional<std::string> set_height(double height);
    std::optional<std::string> set_step_height(double step_height);
    std::optional<std::string> set_max_slope(double degrees);
    void set_enabled(bool enabled);

    // The physics world's side: what its ground probe found this step, with
    // no Changed and no history.
    void store_ground(bool on_ground, bool sliding) {
        on_ground_ = on_ground;
        sliding_ = sliding;
    }

protected:
    void on_reuse() override;

private:
    bool enabled_ = true;
    double friction_ = kDefaultFriction;
    double radius_ = kDefaultRadius;
    double height_ = kDefaultHeight;
    double step_height_ = kDefaultStepHeight;
    double max_slope_ = kDefaultMaxSlope;
    bool on_ground_ = false;
    bool sliding_ = false;
};

// transform's position, turned only as it turns about Y: its tilt and scale
// taken out. Its +Z axis, laid flat, gives the turn; when that points up or
// down, its +X axis does.
Matrix4 upright_transform(const Matrix4& transform);

// object as an Enabled PlayerController, else null: every rule special to a
// walking character (upright, hovering, walking, first as root, no joints)
// asks for this one.
inline const PlayerController* enabled_controller(const DataModel* object) {
    const auto* controller = dynamic_cast<const PlayerController*>(object);
    return controller != nullptr && controller->enabled() ? controller : nullptr;
}

}  // namespace engine_core
