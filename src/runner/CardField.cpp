#include "CardField.hpp"

#include "ColorSpace.hpp"
#include "Renderer.hpp"
#include "Visibility.hpp"
#include "amesh.hpp"
#include "profiler/Profiler.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>

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
    row.fade = kLodShown;
    slotOf_[index] = static_cast<int>(draw.slot);
    rowOf_[index] = static_cast<int>(slot.rows.size());
    slot.rows.push_back(row);
    slot.draws.push_back(index);
    // Every card of a slot comes from one template, so one band.
    slot.nearSquared = draw.lodNear * draw.lodNear;
    slot.farSquared = std::isfinite(draw.lodFar) ? draw.lodFar * draw.lodFar : FLT_MAX;
    unsettled_ = true;
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
        // Kept through a rebuild: a card added again carries on fading where it was.
        fadeOf_.resize(total, std::numeric_limits<float>::quiet_NaN());
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
    fadeOf_.resize(total, std::numeric_limits<float>::quiet_NaN());
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

void CardField::fade(const float* eye, float step) {
    PROFILE_SCOPE("Card fades", profiler::Group::Render);
    // Nothing fading, nothing new, and the camera still: every card stays as it is.
    const bool moved = eye[0] != eye_[0] || eye[1] != eye_[1] || eye[2] != eye_[2];
    if (!moved && !unsettled_ && fading_ == 0) {
        return;
    }
    std::copy(eye, eye + 3, eye_);
    unsettled_ = false;
    fading_ = 0;
    for (Slot& slot : slots_) {
        for (std::size_t row = 0; row < slot.rows.size(); ++row) {
            const float* origin = slot.rows[row].model + 12;
            const float dx = origin[0] - eye[0];
            const float dy = origin[1] - eye[1];
            const float dz = origin[2] - eye[2];
            const float squared = dx * dx + dy * dy + dz * dz;
            float& fade = fadeOf_[static_cast<std::size_t>(slot.draws[row])];
            fade = StepLodFade(fade, !(squared < slot.nearSquared) && squared < slot.farSquared, step);
            // At rest a card is whole, and the vertex shader's band test alone
            // shows or hides it; only while it fades does the shader dither it.
            const bool resting = fade == 1.f || fade == 0.f;
            const float sent = resting ? kLodShown : fade;
            fading_ += resting ? 0 : 1;
            if (slot.rows[row].fade != sent) {
                slot.rows[row].fade = sent;
                slot.patched.push_back(static_cast<int>(row));
            }
        }
    }
}

void CardField::upload() {
    for (Slot& slot : slots_) {
        // Rows patched since: written alone, unless the whole slot goes.
        if (!slot.dirty) {
            for (const int row : slot.patched) {
                slot.buffer.patch(row, slot.rows[static_cast<std::size_t>(row)]);
            }
        }
        slot.patched.clear();
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
