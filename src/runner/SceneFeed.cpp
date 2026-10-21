#include "SceneFeed.hpp"

#include <algorithm>
#include <atomic>
#include <utility>

namespace runner {

SceneFeed::SceneFeed() {
    newest_ = std::make_shared<engine_core::VisualSnapshot>();
    pool_.push_back(newest_);
}

void SceneFeed::perform(const engine_core::VisualSnapshot& front) {
    // A Prepare that missed the lock presents the previous frame again.
    if (front.frame == last_frame_ && front.frame != 0) {
        return;
    }
    last_frame_ = front.frame;
    std::shared_ptr<engine_core::VisualSnapshot> out;
    {
        // Only the pool holding a buffer means no reader can reach it: readers
        // copy only newest_, under this lock. Copying it into out marks it taken.
        std::lock_guard<std::mutex> guard(mu_);
        for (const auto& buffer : pool_) {
            if (buffer.use_count() == 1) {
                out = buffer;
                break;
            }
        }
        if (out) {
            // use_count() is a relaxed load; a reader's last shared_ptr
            // release can land after it with no ordering between them. This
            // fence makes that load, having observed the release, happen
            // after it, so the reader's reads of the old frame are ordered
            // before this thread's writes into the buffer below.
            std::atomic_thread_fence(std::memory_order_acquire);
        }
        if (!out) {
            out = std::make_shared<engine_core::VisualSnapshot>();
            pool_.push_back(out);
        }
    }
    // Element by element into a buffer that held a recent frame, so its
    // vectors and strings keep their storage.
    out->frame = front.frame;
    out->camera = front.camera;
    // Chunk pointers, shared with the engine's frame: rows are not copied.
    out->instances = front.instances;
    out->prefabs.resize(front.prefabs.size());
    std::copy(front.prefabs.begin(), front.prefabs.end(), out->prefabs.begin());
    out->lighting = front.lighting;
    out->sky = front.sky;
    out->dynamic_sky = front.dynamic_sky;
    out->bloom = front.bloom;
    out->effects = front.effects;
    out->atmosphere = front.atmosphere;
    out->reflections = front.reflections;
    out->occlusion = front.occlusion;
    out->draggers = front.draggers;
    out->billboards = front.billboards;
    out->wire_lines = front.wire_lines;
    // Each Terrain's chunks and look, shared by pointer.
    out->terrains = front.terrains;
    // Each emitter's particles, shared by pointer.
    out->particles = front.particles;
    out->brushes = front.brushes;
    out->resources_root = front.resources_root;
    // Each ViewportPane's scene, shared by pointer.
    out->viewports = front.viewports;
    std::lock_guard<std::mutex> guard(mu_);
    newest_ = std::move(out);
}

std::shared_ptr<const engine_core::VisualSnapshot> SceneFeed::hold() {
    std::lock_guard<std::mutex> guard(mu_);
    return newest_;
}

const engine_core::VisualSnapshot& SceneFeed::latest() {
    latest_ = hold();
    return *latest_;
}

}  // namespace runner
