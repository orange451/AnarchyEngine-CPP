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
    out->instances.resize(front.instances.size());
    std::copy(front.instances.begin(), front.instances.end(), out->instances.begin());
    out->prefabs.resize(front.prefabs.size());
    std::copy(front.prefabs.begin(), front.prefabs.end(), out->prefabs.begin());
    out->lighting = front.lighting;
    out->sky = front.sky;
    out->bloom = front.bloom;
    out->draggers = front.draggers;
    out->billboards = front.billboards;
    out->resources_root = front.resources_root;
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
