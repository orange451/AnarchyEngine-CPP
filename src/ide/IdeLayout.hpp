#pragma once

#include "jadefx/jadefx.hpp"
#include "../runner/Runner.hpp"

#include <memory>

namespace ide {

// IDE shell, in the shape of OpenGLFX-IDE's IdeLayout.
// The shell owns the runner. It prepares the session, builds the pages that
// bind phase jobs, then starts the threads. The simulation stays paused until
// Test resumes it. Stop pauses it again. The Edit menu shows whichever of
// those two applies.
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
