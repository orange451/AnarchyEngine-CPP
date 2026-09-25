#pragma once

#include "jadefx/jadefx.hpp"
#include "../runner/Runner.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace engine_core {
class Engine;
}

namespace ide {

class IdeDock;
class IdeScriptEditor;

// IDE shell, in the shape of OpenGLFX-IDE's IdeLayout.
// The constructor prepares the session and builds the shell. The app can
// then create instances. start() adds the scene view and launches the threads.
// The simulation stays paused until Test resumes it. Pause during a test
// stops steps and leaves the session active. Resume continues them. Stop
// restores the place, including when that test is already paused.
// The Edit menu shows Test, or Stop with Pause or Resume.
// Explorer rows open Cut, Paste, and Rename. A script also has Edit, and a
// double-click runs it. Edit docks a script editor on the scene view's tab strip.
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
    void edit(std::uint32_t id);
    void close_prompt(bool apply);
    void show_rename(std::string current, std::function<void(std::string)> apply);
    std::shared_ptr<IdeScriptEditor> open_editor(std::uint32_t id) const;
    void flush_editors();
    void reapply_editors();
    void restore_closed_edits();

    // Declared first so the runner outlives the widgets during teardown.
    runner::Runner runner_;
    std::shared_ptr<jadefx::BorderPane> root_;
    IdeDock* sceneDock_ = nullptr;
    jadefx::Scene* scene_ = nullptr;
    std::unordered_map<std::uint32_t, std::weak_ptr<IdeScriptEditor>> open_scripts_;
    // Source from an editor that was closed while the simulation was running.
    // Stop restores the place, then these strings are written back.
    std::unordered_map<std::uint32_t, std::string> kept_sources_;
    std::unique_ptr<Clip> clip_;
    std::shared_ptr<Prompt> prompt_;
    // The rename sheet stays alive until the next action, so its button handler
    // is not destroyed while that handler is still on the stack.
    std::shared_ptr<Prompt> retiring_;
};

}  // namespace ide
