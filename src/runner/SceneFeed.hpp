#pragma once

#include "IRenderer.hpp"
#include "SnapshotPump.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace runner {

// Hands each published VisualSnapshot from the engine's render thread to the
// Scene Views, which draw on the UI thread. The engine calls perform() with
// its front snapshot; a view calls hold() in its layout and keeps the pointer
// through its paint, so both see the same frame. perform() writes only a
// buffer no one holds, so neither side waits on the other's copy or draw, and
// a view's frame is never written over however many views read.
class SceneFeed : public engine_core::IRenderer {
public:
    SceneFeed();

    // RenderThread. Copies front, unless it is the frame already handed over.
    void perform(const engine_core::VisualSnapshot& front) override;
    void present() override {}

    // UI thread. The newest snapshot perform() finished, unchanged while held.
    // Before the first frame, an empty snapshot at frame 0, never null.
    std::shared_ptr<const engine_core::VisualSnapshot> hold();
    // UI thread. The same, held by the feed until the next latest() call.
    const engine_core::VisualSnapshot& latest();

private:
    std::mutex mu_;
    // Every buffer. One only the pool holds is free to write. Reused, so
    // vectors and strings keep their storage.
    std::vector<std::shared_ptr<engine_core::VisualSnapshot>> pool_;
    std::shared_ptr<engine_core::VisualSnapshot> newest_;
    std::shared_ptr<const engine_core::VisualSnapshot> latest_;
    // RenderThread only.
    std::uint64_t last_frame_ = 0;
};

}  // namespace runner
