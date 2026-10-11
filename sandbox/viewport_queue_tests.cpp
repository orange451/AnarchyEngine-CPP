// ViewportQueue: which ViewportPanes a host renders each frame, without GL.

#include "runner/ViewportQueue.hpp"

#include "SnapshotPump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <vector>

namespace {

using engine_core::InstanceId;
using runner::ViewportQueue;

std::shared_ptr<const engine_core::VisualViewport> scene(InstanceId pane, std::uint64_t revision) {
    auto made = std::make_shared<engine_core::VisualViewport>();
    made->pane = pane;
    made->revision = revision;
    return made;
}

// A draw that records what it drew and answers result.
struct Recorder {
    std::vector<InstanceId> drawn;
    ViewportQueue::Result result = ViewportQueue::Result::Complete;
    ViewportQueue::Draw draw() {
        return [this](const ViewportQueue::Job& job) {
            drawn.push_back(job.pane);
            return result;
        };
    }
};

}  // namespace

TEST_CASE("VQ1 pixel sizes round up to a multiple of 8 and stop at 1024", "[viewport]") {
    REQUIRE(runner::ViewportPixels(64, 1.f) == 64);
    REQUIRE(runner::ViewportPixels(100, 1.f) == 104);
    REQUIRE(runner::ViewportPixels(100.2, 1.f) == 104);
    REQUIRE(runner::ViewportPixels(70, 2.f) == 144);
    REQUIRE(runner::ViewportPixels(5000, 1.f) == 1024);
    REQUIRE(runner::ViewportPixels(0, 1.f) == 0);
    REQUIRE(runner::ViewportPixels(-3, 1.f) == 0);
}

TEST_CASE("VQ2 a pane resized within its multiple of 8 is not drawn again", "[viewport]") {
    ViewportQueue queue;
    Recorder record;
    queue.setScene(7, scene(7, 1));
    queue.shown(7, runner::ViewportPixels(97, 1.f), runner::ViewportPixels(97, 1.f));
    REQUIRE(queue.renderPending(record.draw()) == 1);
    queue.shown(7, runner::ViewportPixels(99, 1.f), runner::ViewportPixels(98, 1.f));
    REQUIRE(queue.renderPending(record.draw()) == 0);
    queue.shown(7, runner::ViewportPixels(105, 1.f), runner::ViewportPixels(98, 1.f));
    REQUIRE(queue.renderPending(record.draw()) == 1);
}

TEST_CASE("VQ3 only shown panes draw, and a new revision draws again", "[viewport]") {
    ViewportQueue queue;
    Recorder record;
    queue.setScene(1, scene(1, 1));
    REQUIRE(queue.renderPending(record.draw()) == 0);
    queue.shown(1, 64, 64);
    REQUIRE(queue.renderPending(record.draw()) == 1);
    queue.shown(1, 64, 64);
    REQUIRE(queue.renderPending(record.draw()) == 0);
    queue.setScene(1, scene(1, 2));
    queue.shown(1, 64, 64);
    REQUIRE(queue.renderPending(record.draw()) == 1);
    int width = 0;
    int height = 0;
    REQUIRE(queue.image(1, width, height));
    REQUIRE(width == 64);
}

TEST_CASE("VQ4 incomplete draws are tried again; not ready draws leave no image", "[viewport]") {
    ViewportQueue queue;
    Recorder record;
    queue.setScene(1, scene(1, 1));
    record.result = ViewportQueue::Result::NotReady;
    queue.shown(1, 64, 64);
    REQUIRE(queue.renderPending(record.draw()) == 1);
    int width = 0;
    int height = 0;
    REQUIRE_FALSE(queue.image(1, width, height));
    record.result = ViewportQueue::Result::Incomplete;
    queue.shown(1, 64, 64);
    queue.renderPending(record.draw());
    REQUIRE(queue.image(1, width, height));
    queue.shown(1, 64, 64);
    REQUIRE(queue.renderPending(record.draw()) == 1);
    record.result = ViewportQueue::Result::Complete;
    queue.shown(1, 64, 64);
    queue.renderPending(record.draw());
    queue.shown(1, 64, 64);
    REQUIRE(queue.renderPending(record.draw()) == 0);
}

TEST_CASE("VQ5 sixty icons and one animated preview: 8 a frame, oldest first, none starved", "[viewport]") {
    ViewportQueue queue;
    Recorder record;
    for (InstanceId pane = 1; pane <= 60; ++pane) {
        queue.setScene(pane, scene(pane, 1));
    }
    std::uint64_t spin = 1;
    int framesToAll = 0;
    std::vector<bool> done(61, false);
    for (int frame = 0; frame < 20; ++frame) {
        queue.setScene(100, scene(100, ++spin));
        for (InstanceId pane = 1; pane <= 60; ++pane) {
            queue.shown(pane, 64, 64);
        }
        queue.shown(100, 512, 512);
        record.drawn.clear();
        REQUIRE(queue.renderPending(record.draw()) <= runner::kViewportPanesPerFrame);
        for (const InstanceId drawn : record.drawn) {
            if (drawn <= 60) {
                done[drawn] = true;
            }
        }
        if (framesToAll == 0 && std::count(done.begin() + 1, done.end(), true) == 60) {
            framesToAll = frame + 1;
        }
    }
    REQUIRE(framesToAll != 0);
    REQUIRE(framesToAll <= 9);
    // The animated preview keeps getting drawn once the icons are done.
    record.drawn.clear();
    queue.setScene(100, scene(100, ++spin));
    queue.shown(100, 512, 512);
    queue.renderPending(record.draw());
    REQUIRE(record.drawn == std::vector<InstanceId>{100});
}

TEST_CASE("VQ6 the pixel budget stops a frame early, but always draws one", "[viewport]") {
    ViewportQueue queue;
    Recorder record;
    queue.setScene(1, scene(1, 1));
    queue.setScene(2, scene(2, 1));
    queue.shown(1, 1024, 1024);
    queue.shown(2, 1024, 1024);
    REQUIRE(queue.renderPending(record.draw()) == 1);
    queue.shown(1, 1024, 1024);
    queue.shown(2, 1024, 1024);
    REQUIRE(queue.renderPending(record.draw()) == 1);
}

TEST_CASE("VQ7 a pane not shown for 60 frames is forgotten", "[viewport]") {
    ViewportQueue queue;
    Recorder record;
    queue.setScene(1, scene(1, 1));
    queue.shown(1, 64, 64);
    queue.renderPending(record.draw());
    std::vector<InstanceId> gone;
    for (int frame = 0; frame < runner::kViewportForgetFrames; ++frame) {
        queue.renderPending(record.draw());
        queue.takeForgotten(gone);
        REQUIRE(gone.empty());
    }
    queue.renderPending(record.draw());
    queue.takeForgotten(gone);
    REQUIRE(gone == std::vector<InstanceId>{1});
    int width = 0;
    int height = 0;
    REQUIRE_FALSE(queue.image(1, width, height));
}

TEST_CASE("VQ8 panes whose draws stay incomplete do not starve the others", "[viewport]") {
    // While anything streams, every draw comes back Incomplete: each of 24
    // shown panes still gets an image within 3 frames, 8 a frame.
    ViewportQueue queue;
    Recorder record;
    record.result = ViewportQueue::Result::Incomplete;
    for (InstanceId pane = 1; pane <= 24; ++pane) {
        queue.setScene(pane, scene(pane, 1));
    }
    for (int frame = 0; frame < 3; ++frame) {
        for (InstanceId pane = 1; pane <= 24; ++pane) {
            queue.shown(pane, 64, 64);
        }
        queue.renderPending(record.draw());
    }
    int width = 0;
    int height = 0;
    for (InstanceId pane = 1; pane <= 24; ++pane) {
        INFO(pane);
        REQUIRE(queue.image(pane, width, height));
    }
}
