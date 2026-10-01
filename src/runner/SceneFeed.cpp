#include "SceneFeed.hpp"

#include <algorithm>
#include <utility>

namespace runner {

void SceneFeed::perform(const engine_core::VisualSnapshot& front) {
    // A Prepare that missed the lock presents the previous frame again.
    if (front.frame == last_frame_ && front.frame != 0) {
        return;
    }
    last_frame_ = front.frame;
    // Element by element into a buffer that held a recent frame, so its
    // vectors and strings keep their storage.
    engine_core::VisualSnapshot& out = buffers_[writing_];
    out.frame = front.frame;
    out.camera = front.camera;
    out.instances.resize(front.instances.size());
    std::copy(front.instances.begin(), front.instances.end(), out.instances.begin());
    out.prefabs.resize(front.prefabs.size());
    std::copy(front.prefabs.begin(), front.prefabs.end(), out.prefabs.begin());
    out.lighting = front.lighting;
    out.sky = front.sky;
    std::lock_guard<std::mutex> guard(mu_);
    std::swap(writing_, waiting_);
    fresh_ = true;
}

const engine_core::VisualSnapshot& SceneFeed::latest() {
    std::lock_guard<std::mutex> guard(mu_);
    if (fresh_) {
        std::swap(reading_, waiting_);
        fresh_ = false;
    }
    return buffers_[reading_];
}

}  // namespace runner
