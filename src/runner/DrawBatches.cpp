#include "DrawBatches.hpp"

#include "ColorSpace.hpp"

#include <algorithm>
#include <cmath>

namespace runner {

namespace {

// How far along the view's -Z model's origin is: larger is nearer.
float ViewDepth(const engine_core::Matrix4& view, const engine_core::Matrix4& model) {
    const float* v = view.m;
    const float* t = model.m + 12;
    return v[2] * t[0] + v[6] * t[1] + v[10] * t[2] + v[14];
}

// How far along the view's -Z the middle of item's mesh box is. A Terrain's
// chunks share one model, so its origin would tie them all; each chunk's own
// box keeps them nearest first.
float BoundsViewDepth(const engine_core::Matrix4& view, const DrawItem& item) {
    const float* m = item.model->m;
    float local[3];
    for (int axis = 0; axis < 3; ++axis) {
        local[axis] = 0.5f * (item.boundsMin[axis] + item.boundsMax[axis]);
    }
    float world[3];
    for (int row = 0; row < 3; ++row) {
        world[row] = m[row] * local[0] + m[4 + row] * local[1] + m[8 + row] * local[2] + m[12 + row];
    }
    const float* v = view.m;
    return v[2] * world[0] + v[6] * world[1] + v[10] * world[2] + v[14];
}

void Cross(const float* a, const float* b, float* out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

InstanceData MakeInstance(const DrawItem& item) {
    InstanceData data{};
    std::copy(item.model->m, item.model->m + 16, data.model);
    NormalMatrix(*item.model, data.normal);
    for (int channel = 0; channel < 3; ++channel) {
        const float tint = item.tint != nullptr ? item.tint[channel] : 1.f;
        data.tint[channel] = engine_core::srgb_to_linear(tint);
    }
    data.boneBase = static_cast<float>(item.boneBase);
    return data;
}

}  // namespace

std::uint64_t BatchKey(std::uint32_t slot, std::uint8_t lod, bool mirrored) {
    return static_cast<std::uint64_t>(slot) << 32 | static_cast<std::uint64_t>(lod) << 1 |
           static_cast<std::uint64_t>(mirrored ? 1 : 0);
}

bool Mirrored(const engine_core::Matrix4& model) {
    const float* m = model.m;
    const float determinant = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) +
                              m[8] * (m[1] * m[6] - m[5] * m[2]);
    return determinant < 0.f;
}

void NormalMatrix(const engine_core::Matrix4& model, float out[9]) {
    // With columns a0, a1, a2, the inverse transpose's columns are
    // (a1 x a2, a2 x a0, a0 x a1) over the determinant a0 . (a1 x a2).
    const float* a0 = model.m;
    const float* a1 = model.m + 4;
    const float* a2 = model.m + 8;
    Cross(a1, a2, out);
    Cross(a2, a0, out + 3);
    Cross(a0, a1, out + 6);
    const float determinant = a0[0] * out[0] + a0[1] * out[1] + a0[2] * out[2];
    if (!(std::abs(determinant) > 1e-30f) || !std::isfinite(determinant)) {
        std::fill(out, out + 9, 0.f);
        return;
    }
    for (int i = 0; i < 9; ++i) {
        out[i] /= determinant;
    }
}

void BuildBatches(const DrawItem* items, const VisibilityResult& visible, const engine_core::Matrix4& view,
                  DrawBatches& out) {
    out.runs.clear();
    out.instances.clear();
    out.opaqueRuns = 0;
    std::vector<DrawBatches::Entry>& order = out.order;
    order.clear();
    for (const VisibleDraw& draw : visible.opaque) {
        const DrawItem& item = items[draw.index];
        DrawBatches::Entry entry;
        entry.key = BatchKey(item.slot, draw.lod, Mirrored(*item.model));
        entry.depth = item.terrain && item.boundsMin != nullptr && item.boundsMax != nullptr
                          ? BoundsViewDepth(view, item)
                          : ViewDepth(view, *item.model);
        entry.index = draw.index;
        entry.lod = draw.lod;
        order.push_back(entry);
    }
    std::sort(order.begin(), order.end(), [items](const DrawBatches::Entry& a, const DrawBatches::Entry& b) {
        // Terrain chunks last, so the geometry pass changes program once.
        if (items[a.index].terrain != items[b.index].terrain) {
            return items[b.index].terrain;
        }
        if (a.key != b.key) {
            return a.key < b.key;
        }
        if (a.depth != b.depth) {
            return a.depth > b.depth;  // nearer first, so depth testing rejects more
        }
        return a.index < b.index;
    });
    for (std::size_t i = 0; i < order.size(); ++i) {
        const DrawBatches::Entry& entry = order[i];
        const DrawItem& item = items[entry.index];
        const bool joins = i > 0 && item.slot != 0 && !item.terrain && !items[order[i - 1].index].terrain &&
                           order[i - 1].key == entry.key;
        if (!joins) {
            DrawRun run;
            run.first = static_cast<int>(out.instances.size());
            run.draw = entry.index;
            run.lod = entry.lod;
            run.mirrored = (entry.key & 1u) != 0;
            out.runs.push_back(run);
        }
        ++out.runs.back().count;
        out.instances.push_back(MakeInstance(item));
    }
    out.opaqueRuns = static_cast<int>(out.runs.size());

    // See-through: farthest first by the Transform's depth, as the pass always sorted them.
    order.clear();
    for (const VisibleDraw& draw : visible.transparent) {
        DrawBatches::Entry entry;
        entry.depth = ViewDepth(view, *items[draw.index].model);
        entry.index = draw.index;
        entry.lod = draw.lod;
        order.push_back(entry);
    }
    std::stable_sort(order.begin(), order.end(),
                     [](const DrawBatches::Entry& a, const DrawBatches::Entry& b) { return a.depth < b.depth; });
    for (const DrawBatches::Entry& entry : order) {
        const DrawItem& item = items[entry.index];
        DrawRun run;
        run.first = static_cast<int>(out.instances.size());
        run.count = 1;
        run.draw = entry.index;
        run.lod = entry.lod;
        run.mirrored = Mirrored(*item.model);
        out.runs.push_back(run);
        out.instances.push_back(MakeInstance(item));
    }
}

}  // namespace runner
