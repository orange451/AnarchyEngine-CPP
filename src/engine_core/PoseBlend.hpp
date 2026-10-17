#pragma once

#include "Matrix4.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine_core {

class DataModel;

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
    // Captured or not: id moved from before to after across the last substep,
    // as the view's Camera a PostSimulation script places on a body. It is
    // drawn through that substep with the bodies, so the two stay together.
    // before == after forgets id's entry, so a still Camera is drawn as it is.
    void follow(InstanceId id, Kind kind, const Matrix4& before, const Matrix4& after);
    // Forgets every entry: play starting or stopping.
    void clear();
    const std::vector<Entry>& entries() const { return entries_; }

    // current rewound by (1 - alpha) of entry's motion. current == entry.after
    // gives lerp(before, after, alpha); alpha 1 gives current exactly.
    static Matrix4 blended(const Entry& entry, const Matrix4& current, double alpha);

private:
    friend void swap_in_blend(DataModel& game, double alpha);
    friend void swap_out_blend(DataModel& game);

    // What swap_in_blend wrote, and the true pose it replaced.
    struct Swap {
        InstanceId id = 0;
        Kind kind = Kind::Body;
        Matrix4 truth;
        Matrix4 shown;
    };

    std::vector<Entry> entries_;
    std::vector<Swap> swaps_;
    // Per instance slot: its entry's index, valid while slot_stamp_ holds this
    // capture's stamp_. Sized once, so a warm capture allocates nothing.
    std::vector<std::uint32_t> slot_entry_;
    std::vector<std::uint32_t> slot_stamp_;
    std::uint32_t stamp_ = 0;
    bool armed_ = false;
    bool capturing_ = false;
};

// At the window's start: every entry's instance shows its blended pose,
// written quietly so nothing hears it. Its true pose is kept.
void swap_in_blend(DataModel& game, double alpha);
// After the copy: each instance still showing its blend gets its true pose
// back. One a window script wrote keeps the script's write, carried from the
// blended frame it was read in to the sim's (truth * inverse(shown) * write),
// so a relative write does not lose the rewind.
void swap_out_blend(DataModel& game);

}  // namespace engine_core
