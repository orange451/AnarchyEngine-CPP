#pragma once

#include "jadefx/jadefx.hpp"
#include "../runner/Runner.hpp"

#include <memory>

namespace ide {

// IDE shell, in the shape of OpenGLFX-IDE's IdeLayout.
// The shell owns the runner and starts it. The runner starts the Lua engine.
// Editing stays paused. Run Test, later, saves the project, simulates, and reloads that save.
class IdeLayout {
public:
    // windowWidth and windowHeight are the window size in points, used to place the splitters.
    IdeLayout(double windowWidth, double windowHeight);

    void mount(jadefx::Scene& scene);

private:
    // Declared first so the runner outlives the widgets during teardown.
    runner::Runner runner_;
    std::shared_ptr<jadefx::BorderPane> root_;
};

}  // namespace ide
