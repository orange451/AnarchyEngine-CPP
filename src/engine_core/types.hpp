#pragma once

// The value types live in engine_datatypes. Everything that includes this file
// uses them, so they come along.
#include "Color.hpp"
#include "Transform.hpp"
#include "Vector3.hpp"

#include <cstdint>

namespace engine_core {

// Low 16 bits are the slot index. High 16 bits are the generation.
// Zero is never a live id.
using InstanceId = std::uint32_t;

// Simulation phases stay contiguous at the front. Resume checks treat
// PreAnimation..Heartbeat as the simulation range.
// Render-frame order is RenderStepped, PreRender, then PostRender.
enum class Phase {
    PreAnimation,    // SimulationThread
    PreSimulation,   // SimulationThread, once per physics substep
    PhysicsSubstep,  // SimulationThread, once per physics substep
    PostSimulation,  // SimulationThread, once per physics substep
    Heartbeat,       // SimulationThread
    RenderStepped,   // RenderThread, inside Prepare, before PreRender and the copy
    PreRender,       // RenderThread, inside Prepare, before the copy
    PostRender       // RenderThread, after Present, lock released
};

// PostRender is the last enumerator. The scheduler's job tables use this bound.
inline constexpr int kPhaseCount = static_cast<int>(Phase::PostRender) + 1;

enum class VisualField : std::uint32_t {
    Transform = 1u << 0,
    Color = 1u << 1,
    Size = 1u << 2,
    Removed = 1u << 3
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

// Passed to GameObject::set_transform / set_color so a RenderStepped or PreRender
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

enum class ThreadRole {
    Unknown,
    Simulation,
    Render
};

void set_thread_role(ThreadRole role);
ThreadRole thread_role();

}  // namespace engine_core
