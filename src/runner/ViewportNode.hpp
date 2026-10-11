#pragma once

#include "SnapshotPump.hpp"
#include "types.hpp"

#include "jadefx/jadefx.hpp"

#include <memory>

namespace runner {

class ViewportRenderer;

// A ViewportPane's node: a StackPane, so GUI children lay out over it, that
// draws its pane's last rendered image between its background and its
// children. It holds the pane's scene for its host's ViewportRenderer, which
// owns every GL object; the node owns none. With no renderer, as in a
// headless test, it draws only its background and children.
class ViewportNode : public jadefx::StackPane {
public:
    ViewportNode(engine_core::InstanceId pane, ViewportRenderer* renderer);

    void setScene(std::shared_ptr<const engine_core::VisualViewport> scene);
    const std::shared_ptr<const engine_core::VisualViewport>& scene() const { return scene_; }
    // 1 - ImageTransparency.
    void setImageOpacity(float opacity) { imageOpacity_ = opacity; }
    float imageOpacity() const { return imageOpacity_; }

protected:
    void renderChildren(jadefx::UiRenderer& renderer, float opacity) override;

private:
    engine_core::InstanceId pane_;
    ViewportRenderer* renderer_;
    std::shared_ptr<const engine_core::VisualViewport> scene_;
    float imageOpacity_ = 1.f;
};

}  // namespace runner
