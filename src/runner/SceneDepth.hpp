#pragma once

namespace runner {

// The depth a Scene View's last draw left, for UI drawn inside the pane to
// hide behind: the renderer's depth texture, 0 near to 1 far, and the
// framebuffer rectangle it covers, in pixels from the bottom left. texture is
// 0 when that draw drew no meshes or sky, or failed: nothing to hide behind.
struct SceneDepth {
    unsigned texture = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

}  // namespace runner
