#include "runner/SceneFeed.hpp"

#include "amesh.hpp"

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// SceneFeed hands the render thread's snapshots to the Scene Views: the newest
// finished frame, unchanged until the next latest(), with no torn frame.
namespace {

int gFailures = 0;

void Expect(bool condition, const char* label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", label);
        ++gFailures;
    }
}

// A frame whose every field says which frame it is.
engine_core::VisualSnapshot Frame(std::uint64_t number, std::size_t rows) {
    engine_core::VisualSnapshot snapshot;
    snapshot.frame = number;
    for (std::size_t i = 0; i < rows; ++i) {
        engine_core::VisualInstance row;
        row.id = static_cast<engine_core::InstanceId>(i + 1);
        row.world.m[12] = static_cast<float>(number);
        row.prefab = 1;
        snapshot.instances.push_back(row);
    }
    snapshot.prefabs.resize(2);
    engine_core::VisualMesh file;
    file.path = "meshes/" + std::to_string(number) + ".amesh";
    snapshot.prefabs[1].meshes.push_back(file);
    // A play-session mesh rides along by pointer, stamped with the frame.
    engine_core::VisualMesh session;
    session.session = std::make_shared<const anarchy::amesh::Data>();
    session.revision = number;
    session.mesh = 7;
    snapshot.prefabs[1].meshes.push_back(session);
    snapshot.lighting.exposure = static_cast<float>(number);
    snapshot.sky.present = true;
    snapshot.sky.image = "textures/" + std::to_string(number) + ".hdr";
    snapshot.sky.rotation = static_cast<float>(number);
    return snapshot;
}

bool Whole(const engine_core::VisualSnapshot& snapshot) {
    for (const engine_core::VisualInstance& row : snapshot.instances) {
        if (row.world.m[12] != static_cast<float>(snapshot.frame)) {
            return false;
        }
    }
    return snapshot.frame == 0 ||
           (snapshot.lighting.exposure == static_cast<float>(snapshot.frame) && snapshot.sky.present &&
            snapshot.sky.image == "textures/" + std::to_string(snapshot.frame) + ".hdr" &&
            snapshot.sky.rotation == static_cast<float>(snapshot.frame) && snapshot.prefabs.size() == 2 && snapshot.prefabs[1].meshes.size() == 2 &&
            snapshot.prefabs[1].meshes[0].path == "meshes/" + std::to_string(snapshot.frame) + ".amesh" &&
            snapshot.prefabs[1].meshes[1].session != nullptr && snapshot.prefabs[1].meshes[1].revision == snapshot.frame);
}

}  // namespace

int RunSceneFeedTests() {
    runner::SceneFeed feed;
    Expect(feed.latest().frame == 0 && feed.latest().instances.empty(), "before any frame the view reads an empty one");

    feed.perform(Frame(1, 3));
    const engine_core::VisualSnapshot& first = feed.latest();
    Expect(first.frame == 1 && first.instances.size() == 3 && Whole(first), "latest is the frame just performed");
    Expect(&feed.latest() == &first && feed.latest().frame == 1, "with no new frame, latest stays the same");

    // Two frames before the view looks: it gets the newer, and the older is gone.
    feed.perform(Frame(2, 2));
    feed.perform(Frame(3, 4));
    Expect(first.frame == 1, "a frame being read is not written over");
    const engine_core::VisualSnapshot& third = feed.latest();
    Expect(third.frame == 3 && third.instances.size() == 4 && Whole(third), "latest skips to the newest frame");

    // The render loop presents the last frame again when it misses the lock.
    feed.perform(Frame(3, 1));
    Expect(feed.latest().frame == 3 && feed.latest().instances.size() == 4, "a repeated frame is not copied again");

    // Under load from another thread, every frame the view sees is whole and in order.
    std::atomic<bool> done{false};
    std::thread render([&] {
        for (std::uint64_t number = 4; number < 4000; ++number) {
            feed.perform(Frame(number, 1 + number % 7));
        }
        done.store(true);
    });
    std::uint64_t seen = 3;
    bool whole = true;
    bool ordered = true;
    while (!done.load()) {
        const engine_core::VisualSnapshot& now = feed.latest();
        whole = whole && Whole(now);
        ordered = ordered && now.frame >= seen;
        seen = now.frame;
    }
    render.join();
    Expect(whole, "no frame the view reads is torn");
    Expect(ordered, "frames reach the view in order");
    Expect(feed.latest().frame == 3999, "the last frame arrives");

    // Every field crosses to the view, the Dragger rows included.
    engine_core::VisualSnapshot handles = Frame(4000, 1);
    engine_core::VisualDragger row;
    row.frame.origin = engine_core::Vec3{1.f, 2.f, 3.f};
    row.hovered = engine_core::DraggerHandle::Y;
    handles.draggers.push_back(row);
    feed.perform(handles);
    const engine_core::VisualSnapshot& drawn = feed.latest();
    Expect(drawn.draggers.size() == 1 && drawn.draggers[0].frame.origin.y == 2.f &&
               drawn.draggers[0].hovered == engine_core::DraggerHandle::Y,
           "the Dragger rows reach the view");
    return gFailures;
}
