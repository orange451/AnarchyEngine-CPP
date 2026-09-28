#pragma once

#include "jadefx/jadefx.hpp"
#include "../runner/Runner.hpp"
#include "InputRouter.hpp"
#include "Preferences.hpp"
#include "ThemeLibrary.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine_core {
class DataModel;
class Engine;
class Project;
}

namespace ide {

class IdeDock;
class IdePane;
class IdeScriptEditor;
class IdeSearch;
class McpServer;
class PreferencesPanel;
class PropertiesPanel;

// IDE shell, in the shape of OpenGLFX-IDE's IdeLayout.
// The constructor prepares the session and builds the shell. The app can
// then create instances. start() adds the scene view and launches the threads.
// The simulation stays paused until Test resumes it. Pause during a test
// stops steps and leaves the session active. Resume continues them. Stop
// restores the place, including when that test is already paused.
// The ribbon under the menu bar holds Test, Pause, Resume, and Stop. Only
// the ones that apply to the session are enabled. F5 is Test, or Resume when
// paused. Shift+F5 is Stop.
// Explorer rows open Cut, Paste, and Rename. A script also has Edit, and a
// double-click runs it. Edit docks a script editor on the scene view's tab strip.
// The explorer edits a name in place and hands the result to rename. F shows
// the selection in every explorer: the branches above it open, and it scrolls into view.
// Properties, under the right-hand explorer, edits the selection's properties.
// Edit > Find in Scripts (Cmd+Shift+F) docks the Search pane beside the left
// explorer, and Replace in Scripts (Cmd+Shift+H) opens it with replace.
// File opens and saves a project folder through the system folder dialog.
// Until the first Save As, the place has no folder. New, Open, and closing the
// window ask first when the place has changes a save would write.
// File > Preferences (Cmd+,) opens the Preferences window. The theme it picks
// is kept in the config folder and drawn with from the start.
class IdeLayout {
public:
    // windowWidth and windowHeight are the window size in points, used to place the splitters.
    // config is where the user's preferences and themes are kept. Empty keeps
    // none: the studio draws with the light theme, and Preferences saves nothing.
    IdeLayout(double windowWidth, double windowHeight, const std::filesystem::path& config = {});
    ~IdeLayout();

    engine_core::Engine& simulation();
    // Binds the scene view, then starts the simulation and render threads.
    void start();
    // Starts the MCP server, so an LLM client can read and edit the place.
    // It listens on 7777, or on any free port when another studio has 7777,
    // and names itself and its project in the studio registry, where the
    // anarchy-mcp bridge finds it. ANARCHY_MCP_PORT pins the port,
    // ANARCHY_MCP=0 turns it off, and ANARCHY_MCP_TOKEN makes clients send
    // that bearer token. A toast says where it listens; the console says why it could not.
    void start_mcp();
    void mount(jadefx::Scene& scene);
    // Grows the window after a frame when a dock's minimum no longer fits.
    void attachFrame(jadefx::Stage& stage);
    // Stops a running test, closes script editors, and loads the project at root.
    // A failure shows an alert and leaves the current place open.
    void open_project_at(const std::filesystem::path& root);
    // True when a save would write something, or an editor holds text its
    // script's Source does not have yet.
    bool has_unsaved_changes();
    // Opens the Preferences window, or leaves the open one be.
    void open_preferences();

private:
    struct Clip;

    void run_action(std::string_view action, std::uint32_t id);
    bool action_enabled(std::string_view action) const;
    // Takes every id out of the place as one undo step. A selected child of a
    // selected instance goes with its ancestor.
    void cut(const std::vector<std::uint32_t>& ids);
    // Destroys each id and its descendants as one undo step.
    void delete_instances(std::vector<std::uint32_t> ids);
    void paste(std::uint32_t id);
    // Puts ids under parent in order, just before its child before, or last
    // when before is 0, as one undo step. One that would cycle stays put.
    void move(std::vector<std::uint32_t> ids, std::uint32_t parent);
    void rename(std::uint32_t id, std::string name);
    void edit(std::uint32_t id);
    std::shared_ptr<IdeScriptEditor> open_editor(std::uint32_t id) const;
    void flush_editors();
    void new_place();
    void open_project();
    // then runs after a successful save. A cancelled dialog or a failure skips it.
    void save_project(std::function<void()> then = {});
    void save_project_as(std::function<void()> then = {});
    bool save_project_to(const std::filesystem::path& root);
    bool save_open_project();
    // Runs proceed now when nothing is unsaved. Otherwise asks Save, Don't
    // Save, or Cancel; Save runs proceed only once the save succeeded.
    void confirm_discard(const std::string& question, std::function<void()> proceed);
    // The place as it is now is what is on disk, or the starting point of New.
    void mark_saved();
    // Recomputes the unsaved state when the place or an editor changed.
    void refresh_modified();
    bool editors_unflushed() const;
    // Runs fn on this thread with the simulation paused, then resumes a test
    // that was stepping. Play steps wait meanwhile.
    void run_now(const std::function<void(engine_core::DataModel&)>& fn);
    void show_session(bool testing, bool stepping);
    void start_test();
    void pause_test();
    void resume_test();
    void stop_test();
    void close_script_editors();
    void show_error(const std::string& heading, const std::string& detail);
    // News that needs no answer, as a JadeFX toast at the bottom right of the window.
    // One sent before mount waits for it.
    void show_toast(std::string text, double seconds = jadefx::Toast::LENGTH_SHORT);
    void update_title();
    // Writes the project's name and folder to the registry entry when they changed.
    void publish_studio();
    std::filesystem::path dialog_directory() const;
    void reapply_editors();
    void restore_closed_edits();
    void routeUndo(jadefx::KeyEvent& event, jadefx::Scene& scene);
    // Delete on a focused explorer deletes the selected instances. Text fields keep the key.
    void routeDelete(jadefx::KeyEvent& event, jadefx::Scene& scene);
    // F, outside a text field, shows the selection in every explorer.
    void routeReveal(jadefx::KeyEvent& event, jadefx::Scene& scene);
    // Cmd+Shift+F and Cmd+Shift+H in a window whose menu bar does not take them.
    void routeSearch(jadefx::KeyEvent& event, jadefx::Scene& scene);
    // Docks the Search pane, or brings it forward, and focuses its find field or,
    // with replace, its replace field. Docked without replace, it starts with
    // replace hidden. A selection on one line in the focused
    // editor becomes the find text.
    void open_search(bool replace, jadefx::Scene* scene);
    void noteScriptFocus();
    void adoptDock(const std::shared_ptr<IdeDock>& dock);
    void onTabDrag(IdeDock& from, const jadefx::TabDrag& drag);
    void previewDrag(IdeDock& from, const jadefx::TabDrag& drag);
    void applyDrag(IdeDock& from, const jadefx::TabDrag& drag);
    // style is the mark's inline CSS: its outline and fill.
    void showDropMark(jadefx::Scene& scene, double x, double y, double width, double height, const char* style);
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
    Preferences preferences_;
    ThemeLibrary themes_;
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
    // Kept while its tab is closed, so reopening it keeps the search.
    std::shared_ptr<IdeSearch> search_;
    std::vector<std::weak_ptr<class IdeExplorer>> explorers_;
    std::unique_ptr<McpServer> mcp_;
    struct McpIdentity;
    // Null while the server is off.
    std::shared_ptr<McpIdentity> mcp_identity_;
    // MCP tools that wait for the UI thread hold this weakly, so a task that
    // runs after the layout is gone does nothing.
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    // Edits the selection's properties. Docked under the right-hand explorer.
    std::unique_ptr<PropertiesPanel> properties_;
    InputRouter undo_router_;
    std::uint32_t last_script_focus_ = 0;
    // Source from an editor that was closed while the simulation was running.
    // Stop restores the place, then these strings are written back.
    std::unordered_map<std::uint32_t, std::string> kept_sources_;
    std::unique_ptr<Clip> clip_;
    // The open project. Null until Open or Save As.
    std::unique_ptr<engine_core::Project> project_;
    bool dialog_open_ = false;
    bool prompt_open_ = false;
    // Project::place_fingerprint when the place was last opened, saved, or made new.
    std::uint64_t saved_fingerprint_ = 0;
    // DataModel::authored_revision when place_modified_ was computed.
    std::uint64_t seen_revision_ = ~std::uint64_t{0};
    bool place_modified_ = false;
    // What the window title shows now.
    bool title_modified_ = false;
    // A play session is active: the ribbon enables Stop, and Shift+F5 stops.
    bool testing_ = false;
    // That session is executing, not paused. F5 resumes only a paused one.
    bool stepping_ = false;
    // The ribbon's Test, Pause, Resume, and Stop.
    jadefx::Node* session_buttons_[4] = {};
    // Open alerts. An alert must outlive its popup.
    std::vector<std::shared_ptr<jadefx::Alert>> alerts_;
    // Toasts sent before mount, with their seconds. Mount shows them.
    std::vector<std::pair<std::string, double>> pending_toasts_;
    // Last, so they go before the preferences and themes they edit.
    std::shared_ptr<jadefx::UtilityWindow> preferences_window_;
    std::shared_ptr<PreferencesPanel> preferences_panel_;
};

}  // namespace ide
