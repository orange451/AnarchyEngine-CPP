#pragma once

#include "jadefx/jadefx.hpp"
#include "../runner/Runner.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

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
// Explorer rows open that instance's context actions. Cut, Paste, and Rename
// apply to every instance. A script also has Edit, and a double-click runs it.
class IdeLayout {
public:
    // windowWidth and windowHeight are the window size in points, used to place the splitters.
    IdeLayout(double windowWidth, double windowHeight);
    ~IdeLayout();

    engine_core::Engine& simulation();
    // Binds the scene view, then starts the simulation and render threads.
    void start();
    void mount(jadefx::Scene& scene);

private:
    struct Prompt;
    struct Clip;

    void run_action(std::string_view action, std::uint32_t id);
    bool action_enabled(std::string_view action) const;
    void cut(std::uint32_t id);
    void paste(std::uint32_t id);
    void rename(std::uint32_t id);
    void close_prompt(bool apply);
    void show_rename(std::string current, std::function<void(std::string)> apply);

    // Declared first so the runner outlives the widgets during teardown.
    runner::Runner runner_;
    std::shared_ptr<jadefx::BorderPane> root_;
    IdeDock* sceneDock_ = nullptr;
    jadefx::Scene* scene_ = nullptr;
    std::unique_ptr<Clip> clip_;
    std::shared_ptr<Prompt> prompt_;
    // The rename sheet stays alive until the next action, so its button handler
    // is not destroyed while that handler is still on the stack.
    std::shared_ptr<Prompt> retiring_;
};

}  // namespace ide
