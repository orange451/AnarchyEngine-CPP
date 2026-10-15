#include "DrawBatches.hpp"

#include "ColorSpace.hpp"
#include "ParallelBlocks.hpp"
#include "profiler/Profiler.hpp"

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

// The last tint made linear. Neighbouring instances nearly always share
// their Color, so most skip the three powers.
struct LinearTint {
    float in[3] = {1.f, 1.f, 1.f};
    float out[3] = {1.f, 1.f, 1.f};

    const float* of(const float* tint) {
        static constexpr float kWhite[3] = {1.f, 1.f, 1.f};
        const float* color = tint != nullptr ? tint : kWhite;
        if (color[0] != in[0] || color[1] != in[1] || color[2] != in[2]) {
            for (int channel = 0; channel < 3; ++channel) {
                in[channel] = color[channel];
                out[channel] = engine_core::srgb_to_linear(color[channel]);
            }
        }
        return out;
    }
};

InstanceData MakeInstance(const DrawItem& item, float fade, LinearTint& linear) {
    InstanceData data{};
    std::copy(item.model->m, item.model->m + 16, data.model);
    // A billboard's normal is the vertex shader's, which faces the camera.
    if (!item.billboard) {
        NormalMatrix(*item.model, data.normal);
    }
    const float* tint = linear.of(item.tint);
    std::copy(tint, tint + 3, data.tint);
    data.fade = fade;
    data.boneBase = static_cast<float>(item.boneBase);
    return data;
}

// Each entry's instance, at its place in order.
void WriteInstances(const DrawItem* items, const std::vector<float>& fades,
                    const std::vector<DrawBatches::Entry>& order, InstanceData* out, std::size_t grain) {
    ParallelBlocks(order.size(), grain, [&](std::size_t, std::size_t begin, std::size_t end) {
        LinearTint linear;
        for (std::size_t i = begin; i < end; ++i) {
            const auto index = static_cast<std::size_t>(order[i].index);
            out[i] = MakeInstance(items[index], index < fades.size() ? fades[index] : kLodShown, linear);
        }
    });
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
                  DrawBatches& out, std::size_t grain) {
    out.runs.clear();
    out.opaqueRuns = 0;
    const std::size_t opaqueCount = visible.opaque.size();
    const std::size_t transparentCount = visible.transparent.size();
    // One instance per visible draw: the opaque ones in sorted order, then the see-through ones.
    out.instances.resize(opaqueCount + transparentCount);
    std::vector<DrawBatches::Entry>& order = out.order;
    order.resize(opaqueCount);
    {
        PROFILE_SCOPE("Batch keys", profiler::Group::Render);
        ParallelBlocks(opaqueCount, grain, [&](std::size_t, std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) {
                const VisibleDraw& draw = visible.opaque[i];
                const DrawItem& item = items[draw.index];
                DrawBatches::Entry& entry = order[i];
                // A billboard is turned to face the camera whatever its model: never mirrored.
                entry.key = BatchKey(item.slot, draw.lod, Mirrored(*item.model) && !item.billboard);
                entry.depth = item.terrain && item.boundsMin != nullptr && item.boundsMax != nullptr
                                  ? BoundsViewDepth(view, item)
                                  : ViewDepth(view, *item.model);
                entry.index = draw.index;
                entry.lod = draw.lod;
                entry.terrain = item.terrain;
            }
        });
    }
    // A total order, index last, so sorting in blocks puts them where one sort would.
    const auto before = [](const DrawBatches::Entry& a, const DrawBatches::Entry& b) {
        // Terrain chunks last, so the geometry pass changes program once.
        if (a.terrain != b.terrain) {
            return b.terrain;
        }
        if (a.key != b.key) {
            return a.key < b.key;
        }
        if (a.depth != b.depth) {
            return a.depth > b.depth;  // nearer first, so depth testing rejects more
        }
        return a.index < b.index;
    };
    {
        PROFILE_SCOPE("Batch sort", profiler::Group::Render);
        ParallelSort(order, grain, before);
    }
    {
        PROFILE_SCOPE("Batch runs", profiler::Group::Render);
        for (std::size_t i = 0; i < order.size(); ++i) {
            const DrawBatches::Entry& entry = order[i];
            // The key's top half is the draw's slot: slot 0 never joins a run.
            const bool joins = i > 0 && (entry.key >> 32) != 0 && !entry.terrain && !order[i - 1].terrain &&
                               order[i - 1].key == entry.key;
            if (!joins) {
                DrawRun run;
                run.first = static_cast<int>(i);
                run.draw = entry.index;
                run.lod = entry.lod;
                run.mirrored = (entry.key & 1u) != 0;
                out.runs.push_back(run);
            }
            ++out.runs.back().count;
        }
    }
    out.opaqueRuns = static_cast<int>(out.runs.size());
    {
        PROFILE_SCOPE("Batch instances", profiler::Group::Render);
        WriteInstances(items, visible.fades, order, out.instances.data(), grain);
    }

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
    for (std::size_t i = 0; i < order.size(); ++i) {
        const DrawBatches::Entry& entry = order[i];
        DrawRun run;
        run.first = static_cast<int>(opaqueCount + i);
        run.count = 1;
        run.draw = entry.index;
        run.lod = entry.lod;
        run.mirrored = Mirrored(*items[entry.index].model) && !items[entry.index].billboard;
        out.runs.push_back(run);
    }
    WriteInstances(items, visible.fades, order, out.instances.data() + opaqueCount, grain);
}

}  // namespace runner
