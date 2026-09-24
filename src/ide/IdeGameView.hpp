#pragma once

#include "IdePane.hpp"
#include "../runner/Renderer.hpp"

namespace ide {

// Scene viewport. The runner draws its picture here.
// Today that picture is the rainbow triangle. This page stays open.
class IdeGameView : public IdePane {
public:
    IdeGameView();

protected:
    void renderContent(jadefx::UiRenderer& renderer, float opacity) override;
    void sceneChanged(jadefx::Scene* previous) override;

private:
    bool ensureGraphics();

    runner::Renderer renderer_;
    bool graphicsAttempted_ = false;
    bool graphicsReady_ = false;
};

}  // namespace ide
