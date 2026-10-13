#pragma once

// The value types live in engine_datatypes. Everything that includes this file
// uses them, so they come along.
#include "Color.hpp"
#include "Color3.hpp"
#include "Matrix4.hpp"
#include "Vector2.hpp"
#include "Vector3.hpp"

#include <cstdint>

namespace engine_core {

// Low 20 bits are the slot index, so a place can hold about a million
// instances. High 12 bits are the generation, bumped each time a slot is
// reused, so a stale id stops resolving. (EnTT's default entity layout.)
// Zero is never a live id.
using InstanceId = std::uint32_t;

constexpr std::uint32_t kSlotBits = 20u;
constexpr std::uint32_t kSlotMask = (1u << kSlotBits) - 1u;
constexpr std::uint32_t id_slot(InstanceId id) { return id & kSlotMask; }
constexpr std::uint32_t id_generation(InstanceId id) { return id >> kSlotBits; }
constexpr InstanceId make_instance_id(std::uint32_t generation, std::uint32_t slot) {
    return (generation << kSlotBits) | slot;
}
// A slot whose generation reaches this is not reused.
constexpr std::uint32_t kMaxGeneration = (1u << (32u - kSlotBits)) - 1u;
static_assert(kMaxGeneration == 0xfffu, "generation is the 12 bits above the slot");

// Simulation phases stay contiguous at the front. Resume checks treat
// PreAnimation..Heartbeat as the simulation range.
// Every phase runs on the engine thread under the frame's lock: the sim step's
// phases first, then the render window's RenderStepped and PreRender.
enum class Phase {
    PreAnimation,    // the sim step
    PreSimulation,   // the sim step, once per physics substep
    PhysicsSubstep,  // the sim step, once per physics substep
    PostSimulation,  // the sim step, once per physics substep
    Heartbeat,       // the sim step
    RenderStepped,   // the window, before PreRender and the copy
    PreRender        // the window, before the copy
};

// PreRender is the last enumerator. The scheduler's job tables use this bound.
inline constexpr int kPhaseCount = static_cast<int>(Phase::PreRender) + 1;

enum class VisualField : std::uint32_t {
    Transform = 1u << 0,
    Removed = 1u << 1,
    // Moved into or out of Workspace. The pump re-evaluates the row, and a
    // row that joins reads every field, since none was kept while it was out.
    Ancestry = 1u << 2,
    // GameObject.Prefab: which Prefab's Models the row draws.
    Prefab = 1u << 3,
    // Camera.FieldOfView.
    Camera = 1u << 4,
    // A Light's Color, Intensity, Radius, Enabled, or a SpotLight's cone.
    Light = 1u << 5,
    // GameObject.Color or Transparency.
    Appearance = 1u << 6,
    // GameObject.Scale.
    Scale = 1u << 7
};

// Which writer produced a visual field.
// Simulation and PreRenderDataModel are real DataModel writes.
// RenderStepped is path B and records PreRenderDataModel.
// SnapshotOverride never touches the DataModel and dies after one Prepare.
enum class WriteOrigin {
    Simulation,
    PreRenderDataModel,
    SnapshotOverride
};

// Passed to GameObject::set_transform so a RenderStepped or PreRender
// job can write a simulated part. The next physics substep overwrites that transform.
struct ForceSimWrite {
    explicit ForceSimWrite() = default;
};

inline VisualField operator|(VisualField a, VisualField b) {
    return static_cast<VisualField>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

inline VisualField operator&(VisualField a, VisualField b) {
    return static_cast<VisualField>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}

inline bool any(VisualField field, VisualField bit) {
    return (static_cast<std::uint32_t>(field) & static_cast<std::uint32_t>(bit)) != 0;
}

// Simulation is the engine thread, which runs the sim step and the window.
enum class ThreadRole {
    Unknown,
    Simulation
};

void set_thread_role(ThreadRole role);
ThreadRole thread_role();

}  // namespace engine_core
