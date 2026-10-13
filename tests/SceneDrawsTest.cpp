// RowDrawCache: a scene view's row draws kept from frame to frame. Whatever it
// keeps, what it leaves must be what drawing every row again gives.

#include "runner/SceneDraws.hpp"

#include <cstdio>
#include <vector>

namespace {

int gFailures = 0;

void Expect(bool condition, const char* label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", label);
        ++gFailures;
    }
}

using engine_core::VisualInstance;
using engine_core::VisualRows;
using runner::DrawRange;
using runner::LightDraw;
using runner::MeshDraw;

// Prefab 1 draws two meshes, prefab 2 one, prefab 3 three; 0 is none.
std::vector<std::vector<MeshDraw>> Templates(float shade = 1.f) {
    std::vector<std::vector<MeshDraw>> templates(4);
    std::uint32_t slot = 1;
    for (std::size_t prefab = 1; prefab < templates.size(); ++prefab) {
        const std::size_t meshes = prefab == 1 ? 2 : prefab == 2 ? 1 : 3;
        for (std::size_t m = 0; m < meshes; ++m) {
            MeshDraw draw;
            draw.slot = slot++;
            draw.color[0] = shade;
            draw.transparency = m == 1 ? 0.25f : 0.f;
            templates[prefab].push_back(draw);
        }
    }
    return templates;
}

VisualInstance Row(std::uint32_t id) {
    VisualInstance row;
    row.id = id;
    row.world = engine_core::matrix4_translation(static_cast<float>(id), 0.f, 0.f);
    row.prefab = id % 5;  // 4 is past the templates: it draws nothing
    row.color = {0.5f, 0.25f, static_cast<float>(id % 7) / 7.f, 1.f};
    row.transparency = id % 3 == 0 ? 0.5f : 0.f;
    row.scale = 1.f + static_cast<float>(id % 4);
    row.alive = id % 11 != 0;
    if (id % 50 == 0) {
        row.light.kind = engine_core::VisualLight::Kind::Point;
        row.light.enabled = true;
        row.light.radius = static_cast<float>(id);
    }
    return row;
}

// What drawing every row again gives: the loop the cache stands in for.
void DrawAll(const VisualRows& rows, const std::vector<std::vector<MeshDraw>>& templates,
             std::vector<MeshDraw>& draws, std::vector<LightDraw>& lights) {
    draws.clear();
    lights.clear();
    for (const VisualInstance& row : rows) {
        if (!row.alive) {
            continue;
        }
        LightDraw light;
        if (runner::LightDrawFor(row, light)) {
            lights.push_back(light);
        }
        if (row.prefab == 0 || row.prefab >= templates.size()) {
            continue;
        }
        runner::AppendRowDraws(row, templates[row.prefab], draws);
    }
}

bool SameLights(const std::vector<LightDraw>& a, const std::vector<LightDraw>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].kind != b[i].kind || a[i].radius != b[i].radius || a[i].position[0] != b[i].position[0]) {
            return false;
        }
    }
    return true;
}

// One scene view, a frame at a time, checked against drawing every row again.
struct View {
    runner::RowDrawCache cache;
    std::vector<MeshDraw> draws;
    std::vector<LightDraw> lights;
    std::vector<DrawRange> written;

    // The frame's draws, plus a terrain draw appended after them as a scene view does.
    void frame(const VisualRows& rows, const std::vector<std::vector<MeshDraw>>& templates, const char* label) {
        lights.clear();
        cache.update(rows, templates, draws, lights, written);
        std::vector<MeshDraw> expected;
        std::vector<LightDraw> expectedLights;
        DrawAll(rows, templates, expected, expectedLights);
        bool same = draws.size() == expected.size();
        for (std::size_t i = 0; same && i < draws.size(); ++i) {
            same = runner::SameDraw(draws[i], expected[i]);
        }
        Expect(same, label);
        Expect(SameLights(lights, expectedLights), label);
        MeshDraw terrain;
        terrain.terrainLook = 9;
        draws.push_back(terrain);
    }

    std::size_t writtenCount() const {
        std::size_t total = 0;
        for (const DrawRange& range : written) {
            total += range.count;
        }
        return total;
    }
};

}  // namespace

int RunSceneDrawsTests() {
    gFailures = 0;
    const std::vector<std::vector<MeshDraw>> templates = Templates();
    VisualRows base;
    for (std::uint32_t id = 1; id <= 3000; ++id) {
        base.push_back(Row(id));
    }
    View view;
    // Each frame draws a copy, as the engine publishes one and keeps writing its own.
    VisualRows frame = base;
    view.frame(frame, templates, "RD1 the first frame draws every row");
    Expect(view.written.size() == 1 && view.written[0].first == 0 && view.writtenCount() == view.draws.size() - 1,
           "RD1 the first frame writes every draw");

    frame = base;
    view.frame(frame, templates, "RD2 a frame with nothing changed keeps every draw");
    Expect(view.written.empty(), "RD2 nothing is written again");

    // One row moves: only its chunk is drawn again.
    base.at_mut(700).world.m[13] = 42.f;
    frame = base;
    view.frame(frame, templates, "RD3 a moved row is drawn where it is now");
    Expect(view.written.size() == 1 && view.writtenCount() < 1000, "RD3 only the moved row's chunk is written");

    // A row joins at the end: only the last chunk.
    base.push_back(Row(3001));
    frame = base;
    view.frame(frame, templates, "RD4 a row that joins is drawn");
    Expect(view.written.size() == 1 && view.writtenCount() < 1000, "RD4 only the last chunk is written");

    // A row early on changes Prefab, so its chunk's count changes and every
    // chunk after it moves along.
    base.at_mut(10).prefab = 3;
    frame = base;
    view.frame(frame, templates, "RD5 a row with a new Prefab moves the draws after it");
    Expect(!view.written.empty(), "RD5 its chunk and the ones after are written");

    // Rows leave from the end, and one is replaced as an erase does.
    base.at_mut(5) = base.back();
    base.pop_back();
    base.pop_back();
    frame = base;
    view.frame(frame, templates, "RD6 rows that leave are no longer drawn");

    // A row stops being alive, and a light turns off.
    base.at_mut(2049).alive = false;
    base.at_mut(49).light.enabled = false;
    frame = base;
    view.frame(frame, templates, "RD7 a row no longer alive draws nothing");

    // New templates, as when a texture finishes loading: everything again.
    const std::vector<std::vector<MeshDraw>> loaded = Templates(0.5f);
    frame = base;
    view.frame(frame, loaded, "RD8 new templates draw every row again");
    Expect(view.writtenCount() == view.draws.size() - 1, "RD8 every draw is written");

    // The draws vector lost what was left in it: everything again.
    view.draws.clear();
    frame = base;
    view.frame(frame, loaded, "RD9 a cleared draws vector draws every row again");

    // A whole new set of rows, as after a resync.
    VisualRows fresh;
    for (std::uint32_t id = 5000; id < 5300; ++id) {
        fresh.push_back(Row(id));
    }
    view.frame(fresh, loaded, "RD10 a new set of rows replaces the old");

    // No rows at all.
    view.frame(VisualRows{}, loaded, "RD11 no rows draw nothing");
    Expect(view.draws.size() == 1, "RD11 only the appended terrain draw is left");

    // Forgotten: the next frame draws everything though nothing changed.
    frame = base;
    view.frame(frame, loaded, "RD12 rows drawn after a clear");
    view.cache.clear();
    view.frame(frame, loaded, "RD12 a cleared cache draws every row again");
    Expect(view.writtenCount() == view.draws.size() - 1, "RD12 every draw is written after a clear");
    if (gFailures == 0) {
        std::printf("scene draws tests passed\n");
    }
    return gFailures;
}
