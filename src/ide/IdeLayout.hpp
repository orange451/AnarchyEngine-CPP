#pragma once

#include "jadefx/jadefx.hpp"
#include "../runner/Runner.hpp"
#include "InputRouter.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace engine_core {
class Engine;
}

namespace ide {

class IdeDock;
class IdePane;
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
// The explorer edits a name in place and hands the result to rename.
class IdeLayout {
public:
    // windowWidth and windowHeight are the window size in points, used to place the splitters.
    IdeLayout(double windowWidth, double windowHeight);
    ~IdeLayout();

    engine_core::Engine& simulation();
    // Binds the scene view, then starts the simulation and render threads.
    void start();
    void mount(jadefx::Scene& scene);
    // Grows the window after a frame when a dock's minimum no longer fits.
    void attachFrame(jadefx::Stage& stage);

private:
    struct Clip;

    void run_action(std::string_view action, std::uint32_t id);
    bool action_enabled(std::string_view action) const;
    void cut(std::uint32_t id);
    void paste(std::uint32_t id);
    void rename(std::uint32_t id, std::string name);
    void edit(std::uint32_t id);
    std::shared_ptr<IdeScriptEditor> open_editor(std::uint32_t id) const;
    void flush_editors();
    void reapply_editors();
    void restore_closed_edits();
    void routeUndo(jadefx::KeyEvent& event, jadefx::Scene& scene);
    void noteScriptFocus();
    void adoptDock(const std::shared_ptr<IdeDock>& dock);
    void onTabDrag(IdeDock& from, const jadefx::TabDrag& drag);
    void previewDrag(IdeDock& from, const jadefx::TabDrag& drag);
    void applyDrag(IdeDock& from, const jadefx::TabDrag& drag);
    void showDropMark(jadefx::Scene& scene, double x, double y, double width, double height, const char* border,
                      jadefx::Color fill);
    void hideDropMark();
    void floatTab(const std::shared_ptr<jadefx::Tab>& tab, double screenX, double screenY);
    void flushFrame();
    void removeDock(const std::shared_ptr<IdeDock>& dock);
    void noteReplaced(jadefx::Node& owner, const std::shared_ptr<jadefx::Node>&,
                      const std::shared_ptr<jadefx::Node>& replacement);
    void rebindUtilities();
    void forgetWindow(jadefx::UtilityWindow* window);
    jadefx::UtilityWindow* utilityOf(const IdeDock* dock) const;
    bool utilityHasDock(const jadefx::UtilityWindow* window) const;
    std::vector<jadefx::Stage*> utilityStages() const;
    std::shared_ptr<jadefx::Node> shareNode(jadefx::Node* node) const;
    IdeDock* editorHome();
    IdeDock* dockContaining(const IdePane* pane) const;
    IdeDock* dockForPane(const jadefx::TabPane* pane) const;
    void forgetDock(const std::shared_ptr<IdeDock>& dock);

    struct Floating {
        std::shared_ptr<jadefx::UtilityWindow> window;
        int lastRequestedW = 0;
        int lastRequestedH = 0;
        int lastSceneW = -1;
        int lastSceneH = -1;
        std::string title;
    };

    // Declared first so the runner outlives the widgets during teardown.
    runner::Runner runner_;
    std::shared_ptr<jadefx::BorderPane> root_;
    std::shared_ptr<jadefx::Node> workArea_;
    std::vector<std::shared_ptr<IdeDock>> docks_;
    std::vector<std::shared_ptr<IdeDock>> pendingEmpty_;
    std::vector<Floating> floating_;
    // Docks currently parented in a utility window. The close hook reads this
    // after the scene root has already been cleared.
    std::unordered_map<IdeDock*, jadefx::UtilityWindow*> dockWindow_;
    std::shared_ptr<jadefx::Pane> dropMark_;
    jadefx::Scene* dropMarkScene_ = nullptr;
    std::function<void(int, int)> resizeWindow_;
    jadefx::Stage* mainStage_ = nullptr;
    bool fitPending_ = false;
    int lastRequestedW_ = 0;
    int lastRequestedH_ = 0;
    int lastSceneW_ = -1;
    int lastSceneH_ = -1;
    IdeDock* sceneDock_ = nullptr;
    jadefx::Scene* scene_ = nullptr;
    std::unordered_map<std::uint32_t, std::weak_ptr<IdeScriptEditor>> open_scripts_;
    std::weak_ptr<class IdeConsole> console_;
    InputRouter undo_router_;
    std::uint32_t last_script_focus_ = 0;
    // Source from an editor that was closed while the simulation was running.
    // Stop restores the place, then these strings are written back.
    std::unordered_map<std::uint32_t, std::string> kept_sources_;
    std::unique_ptr<Clip> clip_;
};

}  // namespace ide
