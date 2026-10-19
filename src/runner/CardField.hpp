#pragma once

#include "DrawBatches.hpp"
#include "InstanceBuffer.hpp"
#include "Visibility.hpp"

#include <cstdint>
#include <vector>

namespace runner {

struct MeshDraw;

// The scene's opaque billboard cards, kept on the GPU from paint to paint:
// each card slot's instances in a buffer of their own, written again only
// where the draws changed. Neither culling nor batching sees them, so a
// forest of cards costs the CPU nothing a paint, still or moving. The
// geometry pass draws each slot in one call; the vertex shader leaves out
// the cards outside the slot's band, and the rasterizer the ones out of view.
class CardField {
public:
    // A card the field draws: an opaque, uploaded billboard, drawn as itself
    // (not only for shadows, which cards never cast).
    static bool Holds(const MeshDraw& draw);

    // Brought up to date with meshes, count long: every draw when all, else
    // only the ranges (sorted, apart), as Renderer::findVisible works them out.
    void update(const MeshDraw* meshes, int count, bool all, const std::vector<DrawRange>& changed);
    // Moves each card's fade (StepLodFade) on by step, the camera at eye.
    void fade(const float* eye, float step);
    // Sends each slot whose cards changed, and each fading card's row. Needs the GL context.
    void upload();

    struct Slot {
        // One instance a card, and the MeshDraw each came from this paint.
        std::vector<InstanceData> rows;
        std::vector<int> draws;
        // Rows whose fade changed since the last upload.
        std::vector<int> patched;
        // Its band, squared, from the camera to each card's origin.
        float nearSquared = 0.f;
        float farSquared = 0.f;
        InstanceBuffer buffer;
        bool dirty = false;
        // Its cards by cell of the ground, so a fade skips whole cells that
        // sit at rest on one side of the band. Made again when rows change.
        struct Cell {
            float min[3];
            float max[3];
            std::uint32_t begin;
            std::uint32_t end;
            // Every card in it resting shown (kRestShown) or hidden (kRestHidden), or neither.
            std::uint8_t rest;
        };
        std::vector<Cell> cells;
        // Cells grouped kCellsPerBlock square: each block's bounds, and its cells, begin to end.
        std::vector<Cell> blocks;
        // Rows in cell order: cells index into it.
        std::vector<int> order;
        bool cellsStale = true;
    };
    // By slot number; most are empty.
    const std::vector<Slot>& slots() const { return slots_; }
    // Cards held.
    int cards() const { return cards_; }
    // Whether draw index is one of them.
    bool holds(int index) const {
        return index >= 0 && static_cast<std::size_t>(index) < slotOf_.size() && slotOf_[index] >= 0;
    }
    void destroy();

private:
    void add(const MeshDraw& draw, int index);
    void remove(int index);
    // A card still in its slot, its row brought up to date where it changed.
    void refresh(const MeshDraw& draw, int index);
    static void sortCells(Slot& slot);

    std::vector<Slot> slots_;
    // Per draw index: its slot and row, or -1.
    std::vector<int> slotOf_;
    std::vector<int> rowOf_;
    // Per draw index: its card's fade (StepLodFade), NaN until its first. Not
    // per row, so it outlives the card's row being made again.
    std::vector<float> fadeOf_;
    int cards_ = 0;
    bool stale_ = true;
    // The camera at the last fade, cards fading then, and whether cards came since.
    float eye_[3] = {0.f, 0.f, 0.f};
    int fading_ = 0;
    bool unsettled_ = true;
};

}  // namespace runner
