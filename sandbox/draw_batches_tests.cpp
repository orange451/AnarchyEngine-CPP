// DrawBatches: the visible meshes sorted into instanced runs, and each
// instance's data, with no GL.

#include "runner/DrawBatches.hpp"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <vector>

using Catch::Approx;
using namespace runner;

namespace {

const float kBox[3] = {0.5f, 0.5f, 0.5f};

// A frame of items: each one's model, slot, tint and transparency.
struct Frame {
    std::vector<engine_core::Matrix4> models;
    std::vector<std::array<float, 3>> tints;
    std::vector<DrawItem> items;
    VisibilityResult visible;

    // Every item is visible, opaque below transparency 0, at LOD lod.
    void add(const engine_core::Matrix4& model, std::uint32_t slot, float transparency = 0.f,
             std::array<float, 3> tint = {1.f, 1.f, 1.f}, std::uint8_t lod = 0) {
        models.push_back(model);
        tints.push_back(tint);
        DrawItem item;
        item.boundsMin = kBox;
        item.boundsMax = kBox;
        item.slot = slot;
        item.transparency = transparency;
        item.drawable = true;
        items.push_back(item);
        VisibleDraw draw;
        draw.index = static_cast<int>(items.size()) - 1;
        draw.lod = lod;
        (transparency > 0.f ? visible.transparent : visible.opaque).push_back(draw);
    }

    // Points each item at its model and tint, once the vectors stop growing.
    const DrawItem* ready() {
        for (std::size_t i = 0; i < items.size(); ++i) {
            items[i].model = &models[i];
            items[i].tint = tints[i].data();
        }
        return items.data();
    }
};

// A camera at the origin looking down -Z: the view is the identity.
const engine_core::Matrix4 kView = engine_core::matrix4_identity();

engine_core::Matrix4 At(float x, float y, float z) { return engine_core::matrix4_translation(x, y, z); }

}  // namespace

TEST_CASE("B1 draws of one slot make one run, nearest first", "[batches]") {
    Frame frame;
    frame.add(At(0.f, 0.f, -30.f), 5);
    frame.add(At(0.f, 0.f, -10.f), 5);
    frame.add(At(0.f, 0.f, -20.f), 5);
    DrawBatches out;
    BuildBatches(frame.ready(), frame.visible, kView, out);
    REQUIRE(out.runs.size() == 1);
    REQUIRE(out.opaqueRuns == 1);
    REQUIRE(out.runs[0].first == 0);
    REQUIRE(out.runs[0].count == 3);
    REQUIRE(out.runs[0].draw == 1);  // its nearest MeshDraw, whose mesh and Material all share
    REQUIRE(out.instances.size() == 3);
    REQUIRE(out.instances[0].model[14] == -10.f);
    REQUIRE(out.instances[1].model[14] == -20.f);
    REQUIRE(out.instances[2].model[14] == -30.f);
}

TEST_CASE("B2 a mirrored draw never shares a run with unmirrored ones", "[batches]") {
    Frame frame;
    engine_core::Matrix4 mirrored = At(1.f, 0.f, -5.f);
    mirrored.m[0] = -1.f;
    frame.add(At(0.f, 0.f, -5.f), 5);
    frame.add(mirrored, 5);
    frame.add(At(2.f, 0.f, -5.f), 5);
    DrawBatches out;
    BuildBatches(frame.ready(), frame.visible, kView, out);
    REQUIRE(out.runs.size() == 2);
    REQUIRE(Mirrored(mirrored));
    REQUIRE_FALSE(Mirrored(At(0.f, 0.f, 0.f)));
    int mirroredRuns = 0;
    for (const DrawRun& run : out.runs) {
        mirroredRuns += run.mirrored ? 1 : 0;
        REQUIRE(run.count == (run.mirrored ? 1 : 2));
    }
    REQUIRE(mirroredRuns == 1);
}

TEST_CASE("B3 slot 0 never batches, even with identical draws", "[batches]") {
    Frame frame;
    frame.add(At(0.f, 0.f, -5.f), 0);
    frame.add(At(0.f, 0.f, -5.f), 0);
    frame.add(At(0.f, 0.f, -5.f), 0);
    DrawBatches out;
    BuildBatches(frame.ready(), frame.visible, kView, out);
    REQUIRE(out.runs.size() == 3);
    for (const DrawRun& run : out.runs) {
        REQUIRE(run.count == 1);
    }
}

TEST_CASE("B4 different LODs and different slots make different runs", "[batches]") {
    Frame frame;
    frame.add(At(0.f, 0.f, -5.f), 5, 0.f, {1.f, 1.f, 1.f}, 0);
    frame.add(At(0.f, 0.f, -6.f), 5, 0.f, {1.f, 1.f, 1.f}, 1);
    frame.add(At(0.f, 0.f, -7.f), 6);
    frame.add(At(0.f, 0.f, -8.f), 5, 0.f, {1.f, 1.f, 1.f}, 0);
    DrawBatches out;
    BuildBatches(frame.ready(), frame.visible, kView, out);
    REQUIRE(out.runs.size() == 3);
    // Sorted by key: slot 5 LOD 0 (two), slot 5 LOD 1, slot 6.
    REQUIRE(out.runs[0].count == 2);
    REQUIRE(out.runs[0].lod == 0);
    REQUIRE(out.runs[1].lod == 1);
    REQUIRE(out.runs[2].draw == 2);
    REQUIRE(BatchKey(5, 0, false) < BatchKey(5, 1, false));
    REQUIRE(BatchKey(5, 1, true) < BatchKey(6, 0, false));
}

TEST_CASE("B5 see-through draws follow as runs of 1, farthest first", "[batches]") {
    Frame frame;
    frame.add(At(0.f, 0.f, -5.f), 5, 0.5f);
    frame.add(At(0.f, 0.f, -9.f), 5, 0.5f);
    frame.add(At(0.f, 0.f, -2.f), 5);
    frame.add(At(0.f, 0.f, -7.f), 5, 0.5f);
    DrawBatches out;
    BuildBatches(frame.ready(), frame.visible, kView, out);
    REQUIRE(out.opaqueRuns == 1);
    REQUIRE(out.runs.size() == 4);
    REQUIRE(out.runs[1].draw == 1);
    REQUIRE(out.runs[2].draw == 3);
    REQUIRE(out.runs[3].draw == 0);
    for (std::size_t r = 1; r < out.runs.size(); ++r) {
        REQUIRE(out.runs[r].count == 1);
        REQUIRE(out.instances[static_cast<std::size_t>(out.runs[r].first)].model[14] ==
                frame.models[static_cast<std::size_t>(out.runs[r].draw)].m[14]);
    }
}

TEST_CASE("B6 the normal matrix is the inverse transpose, and zeros when there is none", "[batches]") {
    float normal[9];
    // A turn keeps normals as the model turns them.
    const engine_core::Matrix4 turn = engine_core::matrix4_axis_angle({0.f, 1.f, 0.f}, 0.7);
    NormalMatrix(turn, normal);
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            REQUIRE(normal[column * 3 + row] == Approx(turn.m[column * 4 + row]).margin(1e-6));
        }
    }
    // Stretched 2 in X, a surface's normal in X shrinks by half.
    engine_core::Matrix4 stretched = engine_core::matrix4_identity();
    stretched.m[0] = 2.f;
    NormalMatrix(stretched, normal);
    REQUIRE(normal[0] == Approx(0.5f));
    REQUIRE(normal[4] == Approx(1.f));
    REQUIRE(normal[8] == Approx(1.f));
    // Scale 0 has no inverse.
    engine_core::Matrix4 flat = engine_core::matrix4_identity();
    flat.m[0] = flat.m[5] = flat.m[10] = 0.f;
    NormalMatrix(flat, normal);
    for (const float value : normal) {
        REQUIRE(value == 0.f);
    }
}

TEST_CASE("B7 each instance keeps its own tint, made linear", "[batches]") {
    Frame frame;
    frame.add(At(0.f, 0.f, -5.f), 5, 0.f, {1.f, 0.f, 0.f});
    frame.add(At(1.f, 0.f, -6.f), 5, 0.f, {0.f, 0.5f, 0.f});
    frame.items.push_back(frame.items[0]);
    frame.models.push_back(At(2.f, 0.f, -7.f));
    frame.tints.push_back({1.f, 1.f, 1.f});
    frame.visible.opaque.push_back(VisibleDraw{2, 0.f, 0});
    const DrawItem* items = frame.ready();
    frame.items[2].tint = nullptr;  // no tint is white
    DrawBatches out;
    BuildBatches(items, frame.visible, kView, out);
    REQUIRE(out.runs.size() == 1);
    REQUIRE(out.instances[0].tint[0] == 1.f);
    REQUIRE(out.instances[0].tint[1] == 0.f);
    REQUIRE(out.instances[1].tint[1] == Approx(0.2140f).margin(1e-3f));   // sRGB 0.5, not pow(0.5, 2.2)
    REQUIRE(out.instances[2].tint[2] == 1.f);
}

TEST_CASE("B8 a second frame replaces the first", "[batches]") {
    Frame frame;
    frame.add(At(0.f, 0.f, -5.f), 5);
    frame.add(At(0.f, 0.f, -6.f), 6, 0.5f);
    DrawBatches out;
    BuildBatches(frame.ready(), frame.visible, kView, out);
    REQUIRE(out.runs.size() == 2);
    VisibilityResult none;
    BuildBatches(frame.items.data(), none, kView, out);
    REQUIRE(out.runs.empty());
    REQUIRE(out.instances.empty());
    REQUIRE(out.opaqueRuns == 0);
}

TEST_CASE("B9 InstanceData is 112 bytes, in slot order", "[batches]") {
    REQUIRE(sizeof(InstanceData) == 112);
    REQUIRE(offsetof(InstanceData, normal) == 64);
    REQUIRE(offsetof(InstanceData, tint) == 100);
}

TEST_CASE("B10 terrain chunks draw alone, after every other opaque run", "[batches]") {
    Frame frame;
    frame.add(At(0.f, 0.f, -5.f), 0);
    frame.add(At(0.f, 0.f, -9.f), 0);
    frame.add(At(0.f, 0.f, -2.f), 0);
    frame.add(At(0.f, 0.f, -7.f), 5);
    frame.add(At(0.f, 0.f, -3.f), 5);
    frame.items[0].terrain = true;
    frame.items[2].terrain = true;
    DrawBatches out;
    BuildBatches(frame.ready(), frame.visible, kView, out);
    // Draw 1 (slot 0 sorts first), the slot 5 pair, then the two terrain chunks, nearest first.
    REQUIRE(out.runs.size() == 4);
    REQUIRE(out.opaqueRuns == 4);
    REQUIRE(out.runs[0].draw == 1);
    REQUIRE(out.runs[1].count == 2);
    REQUIRE(out.runs[1].draw == 4);
    REQUIRE(out.runs[2].draw == 2);
    REQUIRE(out.runs[3].draw == 0);
    REQUIRE(out.runs[2].count == 1);
    REQUIRE(out.runs[3].count == 1);
}

TEST_CASE("B11 one Terrain's chunks, sharing its model, draw nearest box first", "[batches][terrain]") {
    // Three chunks of a Terrain moved 4 back: every one has the same model,
    // so only each chunk's own box tells which is nearer.
    const float nearMin[3] = {0.f, 0.f, -8.f};
    const float nearMax[3] = {8.f, 8.f, 0.f};
    const float farMin[3] = {0.f, 0.f, -40.f};
    const float farMax[3] = {8.f, 8.f, -32.f};
    const float midMin[3] = {0.f, 0.f, -24.f};
    const float midMax[3] = {8.f, 8.f, -16.f};
    Frame frame;
    for (int i = 0; i < 3; ++i) {
        frame.add(At(0.f, 0.f, -4.f), 0);
        frame.items.back().terrain = true;
    }
    frame.items[0].boundsMin = farMin;
    frame.items[0].boundsMax = farMax;
    frame.items[1].boundsMin = nearMin;
    frame.items[1].boundsMax = nearMax;
    frame.items[2].boundsMin = midMin;
    frame.items[2].boundsMax = midMax;
    DrawBatches out;
    BuildBatches(frame.ready(), frame.visible, kView, out);
    REQUIRE(out.runs.size() == 3);
    REQUIRE(out.runs[0].draw == 1);
    REQUIRE(out.runs[1].draw == 2);
    REQUIRE(out.runs[2].draw == 0);
}

TEST_CASE("B12 a tint at the sRGB knee decodes linearly, not by a power", "[batches]") {
    Frame frame;
    frame.add(At(0.f, 0.f, -5.f), 5, 0.f, {0.04045f, 0.f, 1.f});
    const DrawItem* items = frame.ready();
    DrawBatches out;
    BuildBatches(items, frame.visible, kView, out);
    REQUIRE(out.instances.size() == 1);
    REQUIRE(out.instances[0].tint[0] == Approx(0.04045f / 12.92f).margin(1e-6f));
    REQUIRE(out.instances[0].tint[1] == 0.f);
    REQUIRE(out.instances[0].tint[2] == 1.f);
}
