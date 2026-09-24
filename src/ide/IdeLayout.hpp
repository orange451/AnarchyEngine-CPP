#pragma once

#include "jadefx/jadefx.hpp"

#include <memory>

namespace ide {

// IDE shell, in the shape of OpenGLFX-IDE's IdeLayout.
// The runner is a separate program. This shell will attach to one later.
// Editing stays paused. Run Test, later, saves the project, simulates, and reloads that save.
class IdeLayout {
public:
    // windowWidth and windowHeight are the window size in points, used to place the splitters.
    IdeLayout(double windowWidth, double windowHeight);

    void mount(jadefx::Scene& scene);

private:
    std::shared_ptr<jadefx::BorderPane> root_;
};

}  // namespace ide
