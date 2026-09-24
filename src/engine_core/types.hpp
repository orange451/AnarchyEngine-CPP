#pragma once

#include <cstdint>

namespace engine_core {

// Column-major 4x4. Translation lives in m[12], m[13], m[14].
struct Transform {
    float m[16] = {};
};

struct ColorRgb {
    float r = 1.f;
    float g = 1.f;
    float b = 1.f;
    float a = 1.f;
};

// Low 16 bits are the slot index. High 16 bits are the generation.
// Zero is never a live id.
using InstanceId = std::uint32_t;

enum class Phase {
    PreAnimation,    // SimulationThread
    PreSimulation,   // SimulationThread, once per physics substep
    PhysicsSubstep,  // SimulationThread, once per physics substep
    PostSimulation,  // SimulationThread, once per physics substep
    Heartbeat,       // SimulationThread
    PreRender        // RenderThread, inside Prepare, before the copy
};

enum class VisualField : std::uint32_t {
    Transform = 1u << 0,
    Color = 1u << 1,
    Size = 1u << 2,
    Removed = 1u << 3
};

// Which writer produced a visual field.
// Simulation and PreRenderDataModel are real DataModel writes.
// SnapshotOverride never touches the DataModel and dies after one Prepare.
enum class WriteOrigin {
    Simulation,
    PreRenderDataModel,
    SnapshotOverride
};

// Passed to set_transform / set_color so a PreRender job can write a
// simulated part. The next physics substep overwrites that transform.
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

inline Transform transform_identity() {
    Transform out;
    out.m[0] = out.m[5] = out.m[10] = out.m[15] = 1.f;
    return out;
}

inline Transform transform_translation(float x, float y, float z) {
    Transform out = transform_identity();
    out.m[12] = x;
    out.m[13] = y;
    out.m[14] = z;
    return out;
}

enum class ThreadRole {
    Unknown,
    Simulation,
    Render
};

void set_thread_role(ThreadRole role);
ThreadRole thread_role();

}  // namespace engine_core
