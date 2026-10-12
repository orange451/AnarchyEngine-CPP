#pragma once

#include "Matrix4.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace engine_core {

// Simulated bodies drawn between their last two physics substeps. The engine
// arms it before a frame's last substep; PhysicsWorld opens a capture around
// that substep's Box3D step, pull, and held placement; every pose written
// meanwhile is recorded with what it replaced. The render window then draws
// each recorded instance's current pose rewound by the part of that substep
// not yet due: lerp(before, after, alpha) * inverse(after) * current. A pose
// written after the substep (a script turning a character, a held tool put
// on its hand) keeps the substep's motion. Engine thread only.
class PoseBlend {
public:
    enum class Kind : std::uint8_t { Body, Spatial };

    struct Entry {
        InstanceId id = 0;
        Kind kind = Kind::Body;
        Matrix4 before;
        Matrix4 after;
    };

    void arm() { armed_ = true; }
    void disarm() { armed_ = false; }
    bool armed() const { return armed_; }

    // Clears the entries: this substep's motion replaces the last one's.
    void begin_capture();
    void end_capture() { capturing_ = false; }
    bool capturing() const { return capturing_; }

    // Nothing unless capturing. An instance written twice keeps its first
    // before and its last after.
    void record(InstanceId id, Kind kind, const Matrix4& before, const Matrix4& after);
    // Forgets every entry: play starting or stopping.
    void clear();
    const std::vector<Entry>& entries() const { return entries_; }

    // current rewound by (1 - alpha) of entry's motion. current == entry.after
    // gives lerp(before, after, alpha); alpha 1 gives current exactly.
    static Matrix4 blended(const Entry& entry, const Matrix4& current, double alpha);

private:
    std::vector<Entry> entries_;
    std::unordered_map<InstanceId, std::size_t> index_;
    bool armed_ = false;
    bool capturing_ = false;
};

}  // namespace engine_core
