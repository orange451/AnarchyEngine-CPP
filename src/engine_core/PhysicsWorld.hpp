#pragma once

#include "types.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace engine_core {

class DataModel;

// The Box3D world behind PhysicsObject. Engine::step_physics calls step once
// per physics substep while the place plays, before Heartbeat. Only this
// class's source includes Box3D.
//
// Each step, on SimulationThread under the step lock:
//   1. A Stop since the last step (a new world_generation) drops every body.
//   2. Every PhysicsObject in Workspace, at any depth, gets a body; one that
//      left Workspace or was destroyed loses its body. Of several naming one
//      GameObject, the first in tree order wins, and each other warns once.
//   3. What scripts and Properties changed since the last step goes into the
//      bodies (PhysicsObject::take_dirty). A driven GameObject whose
//      Transform is not what physics last wrote was moved by someone else,
//      and its body jumps there.
//   4. Box3D steps by dt, in one substep: the engine's own substeps are
//      already 240 Hz.
//   5. Every body Box3D moved writes its PhysicsObject's Transform, Velocity,
//      and AngularVelocity, and its GameObject's Transform with that
//      GameObject's scale kept. These writes fire no Changed and record no
//      history, as a GameObject's own velocity integration does.
//
// Gravity is (0, -9.81, 0).
class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    // Does nothing while game is not playing.
    void step(DataModel& game, double dt);

    // How many bodies the world holds, and whether this PhysicsObject has one.
    std::size_t body_count() const;
    bool has_body(InstanceId id) const;

    // Where a warning goes, such as a Hull that fell back to a Box. Unset, it
    // goes nowhere.
    void set_warning_sink(std::function<void(const std::string&)> sink);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace engine_core
