#include "ViewportNode.hpp"

#include "ViewportRenderer.hpp"

namespace runner {

ViewportNode::ViewportNode(engine_core::InstanceId pane, ViewportRenderer* renderer)
    : pane_(pane), renderer_(renderer) {}

void ViewportNode::setScene(std::shared_ptr<const engine_core::VisualViewport> scene) {
    scene_ = std::move(scene);
    if (renderer_ != nullptr) {
        renderer_->setScene(pane_, scene_);
    }
}

void ViewportNode::renderChildren(jadefx::UiRenderer& renderer, float opacity) {
    // Node::render draws the background, then the children, then the content;
    // the image goes over the background and under the children.
    if (renderer_ != nullptr) {
        renderer_->composite(renderer, pane_, getAbsoluteX(), getAbsoluteY(), getWidth(), getHeight(),
                             opacity * imageOpacity_);
    }
    jadefx::StackPane::renderChildren(renderer, opacity);
}

}  // namespace runner
