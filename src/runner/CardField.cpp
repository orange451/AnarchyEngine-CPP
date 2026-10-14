#include "CardField.hpp"

#include "ColorSpace.hpp"
#include "Renderer.hpp"
#include "Visibility.hpp"
#include "amesh.hpp"
#include "profiler/Profiler.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace runner {

bool CardField::Holds(const MeshDraw& draw) {
    return draw.billboard && draw.mesh != nullptr && draw.mesh->valid() && !(draw.transparency > 0.f) &&
           !draw.shadowOnly && draw.terrainLook == 0 && draw.bones == nullptr && draw.slot != 0;
}

void CardField::add(const MeshDraw& draw, int index) {
    if (slots_.size() <= draw.slot) {
        slots_.resize(draw.slot + 1);
    }
    Slot& slot = slots_[draw.slot];
    InstanceData row{};
    std::copy(draw.model.m, draw.model.m + 16, row.model);
    // The vertex shader turns the card and gives it the camera-facing normal.
    for (int channel = 0; channel < 3; ++channel) {
        row.tint[channel] = engine_core::srgb_to_linear(draw.tint[channel]);
    }
    row.boneBase = -1.f;
    slotOf_[index] = static_cast<int>(draw.slot);
    rowOf_[index] = static_cast<int>(slot.rows.size());
    slot.rows.push_back(row);
    slot.draws.push_back(index);
    // Every card of a slot comes from one template, so one band, widened for its cross-fades.
    const float keptNear = LodKeptNear(draw.lodNear);
    const float keptFar = LodKeptFar(draw.lodFar);
    slot.nearSquared = keptNear * keptNear;
    slot.farSquared = std::isfinite(keptFar) ? keptFar * keptFar : FLT_MAX;
    slot.dirty = true;
    ++cards_;
}

void CardField::remove(int index) {
    const int number = slotOf_[index];
    if (number < 0) {
        return;
    }
    Slot& slot = slots_[static_cast<std::size_t>(number)];
    const auto row = static_cast<std::size_t>(rowOf_[index]);
    // The last card takes the gap.
    const std::size_t last = slot.rows.size() - 1;
    if (row != last) {
        slot.rows[row] = slot.rows[last];
        slot.draws[row] = slot.draws[last];
        rowOf_[static_cast<std::size_t>(slot.draws[row])] = static_cast<int>(row);
    }
    slot.rows.pop_back();
    slot.draws.pop_back();
    slotOf_[index] = -1;
    rowOf_[index] = -1;
    slot.dirty = true;
    --cards_;
}

void CardField::update(const MeshDraw* meshes, int count, bool all, const std::vector<DrawRange>& changed) {
    PROFILE_SCOPE("Card field", profiler::Group::Render);
    const auto total = static_cast<std::size_t>(std::max(count, 0));
    // After destroy, or the first time, nothing held matches the draws.
    if (all || stale_) {
        stale_ = false;
        for (Slot& slot : slots_) {
            slot.dirty = slot.dirty || !slot.rows.empty();
            slot.rows.clear();
            slot.draws.clear();
        }
        cards_ = 0;
        slotOf_.assign(total, -1);
        rowOf_.assign(total, -1);
        for (std::size_t index = 0; index < total; ++index) {
            if (Holds(meshes[index])) {
                add(meshes[index], static_cast<int>(index));
            }
        }
        return;
    }
    // Draws past the end are gone.
    for (std::size_t index = total; index < slotOf_.size(); ++index) {
        remove(static_cast<int>(index));
    }
    slotOf_.resize(total, -1);
    rowOf_.resize(total, -1);
    for (const DrawRange& range : changed) {
        const std::size_t end = std::min(range.first + range.count, total);
        for (std::size_t index = range.first; index < end; ++index) {
            remove(static_cast<int>(index));
            if (Holds(meshes[index])) {
                add(meshes[index], static_cast<int>(index));
            }
        }
    }
}

void CardField::upload() {
    for (Slot& slot : slots_) {
        if (slot.dirty && !slot.rows.empty()) {
            slot.buffer.upload(slot.rows.data(), static_cast<int>(slot.rows.size()));
        } else if (slot.dirty) {
            // Emptied, as when its cards moved to another slot: nothing kept for it.
            slot.buffer.destroy();
            std::vector<InstanceData>().swap(slot.rows);
            std::vector<int>().swap(slot.draws);
        }
        slot.dirty = false;
    }
}

void CardField::destroy() {
    for (Slot& slot : slots_) {
        slot.buffer.destroy();
    }
    slots_.clear();
    slotOf_.clear();
    rowOf_.clear();
    cards_ = 0;
    stale_ = true;
}

}  // namespace runner
