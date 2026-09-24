#pragma once

#include "jadefx/jadefx.hpp"
#include "../runner/Runner.hpp"

#include <memory>

namespace engine_core {
class Engine;
}

namespace ide {

class IdeDock;

// IDE shell, in the shape of OpenGLFX-IDE's IdeLayout.
// The constructor prepares the session and builds the shell. The app can
// then create instances. start() adds the scene view and launches the threads.
// The simulation stays paused until Test resumes it. Stop pauses it again.
// The Edit menu shows whichever of those two applies.
class IdeLayout {
public:
    // windowWidth and windowHeight are the window size in points, used to place the splitters.
    IdeLayout(double windowWidth, double windowHeight);

    engine_core::Engine& simulation();
    // Binds the scene view, then starts the simulation and render threads.
    void start();
    void mount(jadefx::Scene& scene);

private:
    // Declared first so the runner outlives the widgets during teardown.
    runner::Runner runner_;
    std::shared_ptr<jadefx::BorderPane> root_;
    IdeDock* sceneDock_ = nullptr;
};

}  // namespace ide
