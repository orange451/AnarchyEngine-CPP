#pragma once

#include "IRenderer.hpp"
#include "SnapshotPump.hpp"

#include <cstdint>
#include <mutex>

namespace runner {

// Hands each published VisualSnapshot from the engine's render thread to the
// Scene Views, which draw on the UI thread. The engine calls perform() with
// its front snapshot; a view calls latest() when it paints. Three buffers, so
// neither side waits on the other's copy or draw: perform() fills one, the
// newest finished one waits in the middle, and latest() reads the third.
class SceneFeed : public engine_core::IRenderer {
public:
    // RenderThread. Copies front, unless it is the frame already handed over.
    void perform(const engine_core::VisualSnapshot& front) override;
    void present() override {}

    // UI thread. The newest snapshot perform() finished. It stays valid, and
    // unchanged, until the next latest() call. Empty before the first frame.
    const engine_core::VisualSnapshot& latest();

private:
    std::mutex mu_;
    engine_core::VisualSnapshot buffers_[3];
    // Which buffer each side holds. Only the swap under mu_ moves them.
    int writing_ = 0;
    int waiting_ = 1;
    int reading_ = 2;
    bool fresh_ = false;
    // RenderThread only.
    std::uint64_t last_frame_ = 0;
};

}  // namespace runner
