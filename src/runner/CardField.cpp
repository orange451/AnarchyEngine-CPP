#include "CardField.hpp"

#include "ColorSpace.hpp"
#include "Renderer.hpp"
#include "Visibility.hpp"
#include "amesh.hpp"
#include "profiler/Profiler.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace runner {

namespace {

// A cell's side, in units: wide enough that a forest makes a few thousand,
// narrow enough that few straddle a band's ends.
constexpr float kCellSize = 64.f;
// Cells a block is across, each way: a block whose cards all rest on one
// side of the band is passed over without looking at its cells.
constexpr std::int64_t kCellsPerBlock = 4;
constexpr std::uint8_t kRestMixed = 0;
constexpr std::uint8_t kRestShown = 1;
constexpr std::uint8_t kRestHidden = 2;

}  // namespace

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
    slot.cellsStale = true;
    ++cards_;
}

void CardField::refresh(const MeshDraw& draw, int index) {
    Slot& slot = slots_[static_cast<std::size_t>(slotOf_[index])];
    const auto row = static_cast<std::size_t>(rowOf_[index]);
    InstanceData& kept = slot.rows[row];
    InstanceData next = kept;
    std::copy(draw.model.m, draw.model.m + 16, next.model);
    for (int channel = 0; channel < 3; ++channel) {
        next.tint[channel] = engine_core::srgb_to_linear(draw.tint[channel]);
    }
    const float nearSquared = draw.lodNear * draw.lodNear;
    const float farSquared = std::isfinite(draw.lodFar) ? draw.lodFar * draw.lodFar : FLT_MAX;
    if (nearSquared != slot.nearSquared || farSquared != slot.farSquared) {
        slot.nearSquared = nearSquared;
        slot.farSquared = farSquared;
        slot.cellsStale = true;
        unsettled_ = true;
    }
    if (std::equal(next.model, next.model + 16, kept.model) && std::equal(next.tint, next.tint + 3, kept.tint)) {
        return;
    }
    // Moved: its cell may be another now, and its fade due again.
    if (!std::equal(next.model + 12, next.model + 15, kept.model + 12)) {
        slot.cellsStale = true;
        unsettled_ = true;
    }
    kept = next;
    slot.patched.push_back(static_cast<int>(row));
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
    slot.cellsStale = true;
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
            slot.cellsStale = true;
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
            // Still a card of the same slot, as when its chunk was written
            // again for another row's sake: kept in its row.
            if (slotOf_[index] >= 0 && static_cast<std::uint32_t>(slotOf_[index]) == meshes[index].slot &&
                Holds(meshes[index])) {
                refresh(meshes[index], static_cast<int>(index));
                continue;
            }
            remove(static_cast<int>(index));
            if (Holds(meshes[index])) {
                add(meshes[index], static_cast<int>(index));
            }
        }
    }
}

void CardField::sortCells(Slot& slot) {
    PROFILE_SCOPE("Card cells", profiler::Group::Render);
    slot.cellsStale = false;
    slot.cells.clear();
    slot.blocks.clear();
    const std::size_t count = slot.rows.size();
    // By block, then by cell within it, so each block's cells lie together.
    struct Keyed {
        std::int64_t bx, bz, cx, cz;
        int row;
        bool operator<(const Keyed& other) const {
            if (bx != other.bx) return bx < other.bx;
            if (bz != other.bz) return bz < other.bz;
            if (cx != other.cx) return cx < other.cx;
            if (cz != other.cz) return cz < other.cz;
            return row < other.row;
        }
    };
    std::vector<Keyed> keyed(count);
    for (std::size_t row = 0; row < count; ++row) {
        const float* origin = slot.rows[row].model + 12;
        const auto cx = static_cast<std::int64_t>(std::floor(origin[0] / kCellSize));
        const auto cz = static_cast<std::int64_t>(std::floor(origin[2] / kCellSize));
        const auto floorDiv = [](std::int64_t value) {
            return value >= 0 ? value / kCellsPerBlock : -((-value + kCellsPerBlock - 1) / kCellsPerBlock);
        };
        keyed[row] = Keyed{floorDiv(cx), floorDiv(cz), cx, cz, static_cast<int>(row)};
    }
    std::sort(keyed.begin(), keyed.end());
    const auto grow = [](Slot::Cell& cell, const float* origin) {
        for (int axis = 0; axis < 3; ++axis) {
            cell.min[axis] = std::min(cell.min[axis], origin[axis]);
            cell.max[axis] = std::max(cell.max[axis], origin[axis]);
        }
    };
    const auto start = [](const float* origin, std::size_t begin) {
        Slot::Cell cell{};
        std::copy(origin, origin + 3, cell.min);
        std::copy(origin, origin + 3, cell.max);
        cell.begin = static_cast<std::uint32_t>(begin);
        cell.end = static_cast<std::uint32_t>(begin);
        cell.rest = kRestMixed;
        return cell;
    };
    slot.order.resize(count);
    for (std::size_t at = 0; at < count; ++at) {
        const Keyed& key = keyed[at];
        slot.order[at] = key.row;
        const float* origin = slot.rows[static_cast<std::size_t>(key.row)].model + 12;
        const bool newBlock = at == 0 || key.bx != keyed[at - 1].bx || key.bz != keyed[at - 1].bz;
        if (newBlock || key.cx != keyed[at - 1].cx || key.cz != keyed[at - 1].cz) {
            if (newBlock) {
                slot.blocks.push_back(start(origin, slot.cells.size()));
            }
            slot.cells.push_back(start(origin, at));
            slot.blocks.back().end = static_cast<std::uint32_t>(slot.cells.size());
        }
        grow(slot.cells.back(), origin);
        grow(slot.blocks.back(), origin);
        slot.cells.back().end = static_cast<std::uint32_t>(at + 1);
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
        if (slot.cellsStale) {
            sortCells(slot);
        }
        // Which side of the band every card in bounds is on, or kRestMixed when
        // they may straddle it: the nearest and farthest any can be, squared,
        // summed as each card's is so the bounds hold exactly.
        const auto sideOf = [&](const Slot::Cell& bounds) {
            float nearest[3];
            float farthest[3];
            for (int axis = 0; axis < 3; ++axis) {
                const float low = bounds.min[axis] - eye[axis];
                const float high = bounds.max[axis] - eye[axis];
                nearest[axis] = low > 0.f ? low : (high < 0.f ? high : 0.f);
                farthest[axis] = std::max(std::abs(low), std::abs(high));
            }
            const float least = nearest[0] * nearest[0] + nearest[1] * nearest[1] + nearest[2] * nearest[2];
            const float most = farthest[0] * farthest[0] + farthest[1] * farthest[1] + farthest[2] * farthest[2];
            if (!(least < slot.nearSquared) && most < slot.farSquared) {
                return kRestShown;
            }
            if (most < slot.nearSquared || !(least < slot.farSquared)) {
                return kRestHidden;
            }
            return kRestMixed;
        };
        for (Slot::Cell& block : slot.blocks) {
            // A whole block resting on the side it is on: nothing in it moves.
            const std::uint8_t blockSide = sideOf(block);
            if (blockSide != kRestMixed && block.rest == blockSide) {
                continue;
            }
            std::uint8_t blockRest = kRestMixed;
            for (std::uint32_t c = block.begin; c < block.end; ++c) {
                Slot::Cell& cell = slot.cells[c];
                const std::uint8_t side = blockSide != kRestMixed ? blockSide : sideOf(cell);
                if (side == kRestMixed || cell.rest != side) {
                    int shown = 0;
                    int hidden = 0;
                    for (std::uint32_t at = cell.begin; at < cell.end; ++at) {
                        const auto row = static_cast<std::size_t>(slot.order[at]);
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
                        shown += fade == 1.f ? 1 : 0;
                        hidden += fade == 0.f ? 1 : 0;
                        if (slot.rows[row].fade != sent) {
                            slot.rows[row].fade = sent;
                            slot.patched.push_back(static_cast<int>(row));
                        }
                    }
                    const auto size = static_cast<int>(cell.end - cell.begin);
                    cell.rest = shown == size ? kRestShown : (hidden == size ? kRestHidden : kRestMixed);
                }
                // The block rests where every one of its cells rests alike.
                blockRest = c == block.begin ? cell.rest : (blockRest == cell.rest ? blockRest : kRestMixed);
            }
            block.rest = blockRest;
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
            std::vector<Slot::Cell>().swap(slot.cells);
            std::vector<Slot::Cell>().swap(slot.blocks);
            std::vector<int>().swap(slot.order);
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
