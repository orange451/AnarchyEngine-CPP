#pragma once

#include "jadefx/jadefx.hpp"
#include "runner/Runner.hpp"
#include "IdeExplorer.hpp"
#include "InputRouter.hpp"
#include "PluginLoader.hpp"
#include "Preferences.hpp"
#include "ThemeLibrary.hpp"
#include "Project.hpp"

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
struct SaveConflict;
}

namespace runner {
class GameView;
}

namespace ide {

class IdeDock;
class IdePane;
enum class DropSide;
struct LayoutHost;
class IdeScriptEditor;
class IdePrefabEditor;
class IdeCssEditor;
struct PrefabEditorHost;
class IdeSearch;
class IdeConflicts;
class IdeProblems;
struct ProblemCounts;
namespace layout_detail {
class StatusChip;
}
class McpServer;
class UiCalls;
class LandingPage;
class PreferencesPanel;
class PropertiesPanel;

// IDE shell, in the shape of OpenGLFX-IDE's IdeLayout.
// The constructor prepares the session and builds the shell, scene view
// included. The app can then create instances. start() launches the threads.
// The simulation stays paused until Test resumes it. Pause during a test
// stops steps and leaves the session active. Resume continues them. Stop
// restores the place, including when that test is already paused.
// The ribbon under the menu bar holds Test, Pause, Resume, and Stop. Only
// the ones that apply to the session are enabled. F5 is Test, or Resume when
// paused. Shift+F5 is Stop. Grid, at its right end, shows or hides the Scene
// Views' floor grid; a test hides it until Stop either way.
// Explorer rows open Cut, Paste, and Rename. A script also has Edit, and a
// double-click runs it. Edit docks a script editor on the scene view's tab strip.
// A Prefab's Edit, and a double-click on it in an explorer or the Assets pane,
// docks a Prefab editor there instead, where its Models and the Mesh and
// Material each pairs are made and changed. Each script or Prefab has at most
// one editor: editing it again brings that one's tab forward.
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
// The Window menu lists the studio's one-of-a-kind windows: the two explorers,
// Properties, the console, and Search. A check marks each one that is open.
// Picking a closed one opens it where it last was, picking one hidden behind
// another tab brings it forward, and picking one that is showing closes it.
// Below them, New Scene View docks another view of the place beside the first,
// and New Terminal docks another shell in with the console.
// Save Layout as Default keeps the layout, but not the main window's place, in
// default-layout.json in the config folder. Reset to Default Layout puts the
// windows back as that has them, or, with none, as a new studio has them.
// Restore Built-in Default forgets the saved one and resets to the built-in layout.
// Where the docks are, what each holds, which windows are closed, and the
// main window's place and size are kept in layout.json in the config folder
// when the window closes, and the next start puts them back. Script editors, Prefab editors,
// extra scene views, and terminals are not kept. Without that file, or when it cannot be read, the studio starts
// with its default layout.
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
    // Starts or stops the MCP server, as decide_mcp says from the preference
    // Preferences' AI tab sets and the ANARCHY_MCP variables. The server lets
    // an LLM client read and edit the place. It listens on 7777, or on any free
    // port when another studio has 7777, and names itself and its project in
    // the studio registry, where the anarchy-mcp bridge finds it.
    // ANARCHY_MCP_PORT pins the port, and ANARCHY_MCP_TOKEN makes clients send
    // that bearer token. A toast says where it listens; the console says why it could not.
    void apply_mcp_setting();
    // Where the server listens, that it is off, or why it could not start.
    std::string mcp_status() const;
    void mount(jadefx::Scene& scene);
    // Grows the window after a frame when a dock's minimum no longer fits.
    // Gives the window the place and size layout.json had, and opens its
    // floating windows, so it comes after the stage has its scene.
    void attachFrame(jadefx::Stage& stage);
    // Stops a running test, closes script editors and the Welcome page, and loads
    // the project at root. A failure shows an alert, leaves the current place
    // open, and returns false.
    bool open_project_at(const std::filesystem::path& root);
    // Writes the place to root as Save As does, and binds there. A place never
    // saved also moves what its scratch folder holds into the project's
    // resources/. False, after an alert saying why, when it cannot.
    bool save_project_to(const std::filesystem::path& root);
    // True when a save would write something, or an editor holds text its
    // script's Source does not have yet.
    bool has_unsaved_changes();
    // Whether export_game saves first: has_unsaved_changes, or a write no
    // recording covered, such as one from the command line or the flown camera,
    // changed what a save writes.
    bool export_needs_save();
    // File > Export Game: when the place has changes, or was never saved, asks
    // to save it first, and Cancel exports nothing. Then asks for the game's
    // file name in a save dialog, and writes the game there as one file a
    // friend can run, off the UI thread (GameExport.hpp). A toast says what it
    // wrote, and the folder holding it opens.
    void export_game();
    // Opens the Preferences window, or leaves the open one be.
    // Opens Preferences, or brings it forward, at the tab titled page when one is given.
    void open_preferences(const std::string& page = {});
    // Shows a dockable pane: open docks it when no dock holds it. Otherwise its
    // tab is selected, and a floating window that holds it comes to the front.
    void reveal_window(IdePane* pane, const std::function<void()>& open = {});
    // Docks the Welcome page as the second tab of the scene view's strip and
    // selects it, or brings the open one forward. start() calls this unless
    // the page's "Don't show this page on startup" was checked; Window >
    // Welcome Page calls it too.
    void open_landing();
    // Closes the Welcome page's tab, if one is open.
    void close_landing();
    // What the MCP tool tabs lists: docks, each with its tabs in strip order,
    // and closed, the Window menu's windows no dock holds.
    engine_core::JsonValue tab_list();
    // The tab titled tab, or holding the page named tab, ignoring case: select
    // brings it forward, and close closes it as its × does. open docks the
    // Window menu's window, or the Welcome page, named tab, or brings it
    // forward when it is open. Each throws, saying why, when it cannot.
    void select_tab(const std::string& tab);
    void close_tab(const std::string& tab);
    void open_tab(const std::string& tab);
    // Compares the disk with the place: loads what only the disk changed, and
    // lists what both changed in the Conflicts window and as a count on the
    // ribbon. choices are applied too. Returns the rows still open, or nothing
    // when the check did not run: during a test, or when src/ does not read.
    std::optional<std::vector<engine_core::SaveConflict>> check_disk(
        const std::vector<engine_core::DiskChoice>& choices = {});
    // Shows the Conflicts window, docking it beside the left explorer when it is closed.
    void show_conflicts();
    // Opens the Problems window, or brings it forward.
    void show_problems();
    // The Problems entry's page, made when this layout was constructed so it
    // counts from startup. For tests, to check it before Problems is ever
    // opened; show_problems docks this very pane.
    IdePane* problemsPaneForTests() const;
    // Whether the Scene Views draw the floor grid now: Grid is on and no test runs.
    bool scene_grid() const { return runner_.sceneGrid(); }
    // Once a frame, after the scene lays out; the main window's stage calls it.
    // Coming back to the window checks the disk here.
    void flushFrame();
    // Writes the paused profiler's history as a page a browser shows. False, with why.
    bool save_profile_capture(const std::filesystem::path& file, std::string& error);
    // Writes the layout to layout.json in the config folder. A close request
    // on the main window does this. Nothing is written without a config folder.
    void save_layout();

private:
    struct Clip;

    void run_action(engine_core::InstanceAction action, std::uint32_t id);
    bool action_enabled(engine_core::InstanceAction action) const;
    // Takes every id out of the place as one undo step. A selected child of a
    // selected instance goes with its ancestor.
    void cut(const std::vector<std::uint32_t>& ids);
    void copy(const std::vector<std::uint32_t>& ids);
    void duplicate(const std::vector<std::uint32_t>& ids);
    std::vector<int> recall_folds(const std::string& guid);
    void remember_folds(const std::string& guid, const std::vector<int>& lines);
    void sync_fold_file();
    // Destroys each id and its descendants as one undo step.
    void delete_instances(std::vector<std::uint32_t> ids);
    // Pastes into each of ids, or beside each, under its parent, when beside is
    // true: one copy each. A cut goes to the first, and copies of it to the rest.
    void paste(const std::vector<std::uint32_t>& ids, bool beside = false);
    // Puts ids under parent in order, just before its child before, or last
    // when before is 0, as one undo step. One that would cycle stays put.
    void move(std::vector<std::uint32_t> ids, std::uint32_t parent);
    void rename(std::uint32_t id, std::string name);
    void edit(std::uint32_t id);
    std::shared_ptr<IdeScriptEditor> open_editor(std::uint32_t id) const;
    // Docks a Prefab editor for prefab on the scene view's tab strip, or brings
    // the one already open forward. home is where a new one docks.
    void edit_prefab(std::uint32_t prefab, IdeDock& home);
    // A CSS instance's editor: the open one, or a new one docked at home.
    void edit_css(std::uint32_t css, IdeDock& home);
    std::shared_ptr<IdePrefabEditor> open_prefab_editor(std::uint32_t prefab) const;
    // A Prefab editor's writes, each one undo step on the simulation thread.
    PrefabEditorHost prefab_editor_host();
    void flush_editors();
    // Runs the built-in plugins again. The place was just made, opened, or rebuilt.
    void load_plugins();
    // An empty, untitled place, with a new scratch folder for its resources.
    void new_place();
    // Asks to save, then for a folder, and opens the project there.
    void open_project();
    // The profiler's Save button: a dialog, then the page.
    void save_profile_capture_as();
    // then runs after a successful save. A cancelled dialog or a failure skips it.
    void save_project(std::function<void()> then = {});
    void save_project_as(std::function<void()> then = {});
    // Saves the open project and runs then. When files changed on disk since it
    // was opened or saved, asks whether to overwrite them and returns false; if
    // Overwrite saves, then runs after that save. overwrite, when set, lists the
    // conflicts to write over; any other still asks. A failed save skips then.
    bool save_open_project(std::function<void()> then = {},
                           const std::vector<engine_core::SaveConflict>* overwrite = nullptr);
    // Lists the files a save found changed on disk. Overwrite saves over those
    // and runs then; Cancel writes nothing.
    // Where a save gate's rows came from, which sets what it offers.
    enum class GateRows {
        // A check: Show Conflicts, and Overwrite All settles each for the studio's side.
        Checked,
        // The save's own guard, with the check unable to read src/: Overwrite All saves over exactly these.
        Guarded,
        // The guard during a test, when changes on disk wait for Stop: only Cancel.
        DuringTest,
    };
    void confirm_overwrite(const std::vector<engine_core::SaveConflict>& conflicts, std::function<void()> then,
                           GateRows from);
    // A new place, or another project: no conflicts from the last one.
    void forget_conflicts();
    // Runs proceed now when nothing is unsaved. Otherwise asks Save, Don't
    // Save, or Cancel; Save runs proceed only once the save succeeded.
    void confirm_discard(const std::string& question, std::function<void()> proceed);
    // Puts the title's unsaved mark where the place and the editors say it
    // belongs. Called each frame.
    void refresh_modified();
    bool editors_unflushed() const;
    // Runs fn on this thread with the simulation paused, then resumes a test
    // that was stepping. Play steps wait meanwhile.
    void run_now(const std::function<void(engine_core::DataModel&)>& fn);
    // No play session, or a test running or paused.
    enum class PlayState { Stopped, Running, Paused };
    void show_session(PlayState state);
    // Turns the ribbon's Grid on or off, and keeps the choice.
    void set_grid(bool on);
    // Tells the Scene Views whether to draw the grid, and lights the button to match.
    void show_grid();
    bool in_test() const { return play_ != PlayState::Stopped; }
    void start_test();
    void pause_test();
    void resume_test();
    void stop_test();
    // Closes every script editor and Prefab editor: their ids belong to a place that is going away.
    void close_script_editors();
    void show_error(const std::string& heading, const std::string& detail);
    // News that needs no answer, as a JadeFX toast at the bottom right of the window.
    // One sent before mount waits for it.
    void show_toast(std::string text, double seconds = jadefx::Toast::LENGTH_SHORT);
    // show_toast from any thread, such as the simulation's: it runs on the UI
    // thread, unless the layout is gone by then. alive is alive_, copied on the
    // UI thread before the work was sent off. layout is used only while alive.
    static void toast_later(IdeLayout* layout, std::weak_ptr<int> alive, std::string text);
    void update_title();
    // Writes the project's name and folder to the registry entry when they changed.
    void publish_studio();
    std::filesystem::path dialog_directory() const;
    // The place's resources folder: the project's, or scratch_resources_.
    std::filesystem::path place_resources() const;
    // Points the place's resources at a new scratch folder, deleting the old one.
    void begin_scratch();
    // Deletes the scratch folder and forgets it.
    void end_scratch();
    // Shows the folder picker, unless one is up already, starting in
    // dialog_directory, and calls chosen with the folder picked. `hint` follows
    // the message shown when the system has no picker.
    void pick_folder(jadefx::FolderDialogOptions options, const std::string& hint,
                     std::function<void(const std::filesystem::path&)> chosen);
    // export_game after any save: asks for the game's file name and exports
    // the project as it is on disk. It does not check for changes again, since
    // a save during play leaves the edit dirty set as it was.
    void export_saved_game();
    void reapply_editors();
    void restore_closed_edits();
    // The keys every studio window routes the same way, in this order.
    void routeKeys(jadefx::KeyEvent& event, jadefx::Scene& scene);
    void routeUndo(jadefx::KeyEvent& event, jadefx::Scene& scene);
    // Delete on a focused explorer deletes the selected instances. Text fields keep the key.
    void routeDelete(jadefx::KeyEvent& event, jadefx::Scene& scene);
    // F, outside a text field, shows the selection in every explorer.
    void routeReveal(jadefx::KeyEvent& event, jadefx::Scene& scene);
    // Cmd+Shift+F and Cmd+Shift+H in a window whose menu bar does not take them.
    void routeSearch(jadefx::KeyEvent& event, jadefx::Scene& scene);
    void routeZoom(jadefx::KeyEvent& event);
    void routeClipboard(jadefx::KeyEvent& event, jadefx::Scene& scene);
    // Zooms the studio and remembers it. announce shows a toast with the new
    // zoom; the status bar's slider, which shows it already, does not.
    void set_zoom(double zoom, bool announce = true);
    // Docks the Search pane, or brings it forward, and focuses its find field or,
    // with replace, its replace field. Docked without replace, it starts with
    // replace hidden. A selection on one line in the focused
    // editor becomes the find text.
    void open_search(bool replace, jadefx::Scene* scene);
    // Adds the one-of-a-kind windows, New Scene View, and New Terminal to the Window menu.
    void fill_window_menu(jadefx::Menu& menu);
    struct WindowEntry;
    // Runs open when no dock holds pane. Otherwise brings its tab forward, or
    // closes it when it is already the tab showing.
    void toggle_window(IdePane* pane, const std::function<void()>& open);
    // Docks the page in the dock its tab last closed from, or else where its home puts it.
    void show_window(WindowEntry& entry);
    // Brings the page's tab forward, or docks it with show_window when it is closed.
    void open_window(WindowEntry& entry);
    // The entry's page, made now when it has not been.
    const std::shared_ptr<IdePane>& window_page(WindowEntry& entry);
    // Keeps the dock the page's tab closes from, for show_window. Called each time it is docked.
    void watch_close(WindowEntry& entry);
    // A new dock on one side of target, depth points across. Target null is the whole work area.
    IdeDock* dock_beside(jadefx::Node* target, DropSide side, double depth);
    void new_scene_view();
    // Docks a new shell in with the console. Closing its tab ends the shell.
    void new_terminal();
    // The console's dock, or a new one under the scene view when the console is closed.
    IdeDock* beside_console();
    // Builds the default layout's docks in the main window, and hands each
    // page to place with the dock it goes in.
    void default_layout(double windowWidth, double windowHeight,
                        const std::function<void(IdeDock&, const std::shared_ptr<IdePane>&)>& place);
    // Puts the windows back as the saved default has them, or, with none, as
    // the built-in layout does. Script editors, extra scene views, and terminals move in
    // beside the scene view.
    void reset_layout();
    // The built-in layout, as apply_builtin_layout puts it. When that cannot be
    // read, the studio's own: the four open, Search and Conflicts closed, and no floating windows.
    void reset_builtin_layout();
    // Docks the pages as resources/layouts/default-layout.json has them. False,
    // having changed nothing and said why in the console, when it cannot be read or docks nothing.
    bool apply_builtin_layout();
    // Keeps the layout, but not the main window's place, as the default in
    // default-layout.json. Without a config folder it is kept until the studio closes.
    void save_default_layout();
    // Forgets the saved default and resets to the built-in layout.
    void restore_builtin_layout();
    // True when there is a saved default for Restore Built-in Default to forget.
    bool has_default_layout() const;
    // Docks the pages as saved, a layout capture_layout wrote, moving tabs that
    // are open and closing the windows it has closed. False, having changed
    // nothing, when it docks nothing in the main window.
    bool apply_layout(const engine_core::JsonValue& saved);
    // The scene view, or the page of the window entry with this name. Null for a name this studio does not know.
    std::shared_ptr<IdePane> page_named(const std::string& name);
    // Opens a floating window for each one in windows, a layout's "floating", docking pages through host.
    void open_saved_floating(const engine_core::JsonValue& windows, const LayoutHost& host);
    // Docks the pages as layout.json left them. False, having docked nothing,
    // when there is no file or nothing in it could be docked.
    bool restore_layout();
    // Opens the floating windows layout.json had. Needs the main window.
    void restore_floating();
    // Gives the main window the place, size, and maximized state layout.json had.
    void restore_window(jadefx::Stage& stage);
    LayoutHost layout_host();
    // The layout as save_layout writes it.
    engine_core::JsonValue capture_layout();
    // Registers each dock under node with the shell.
    void adopt_tree(const std::shared_ptr<jadefx::Node>& node);
    // The Search pane, made the first time it is asked for.
    const std::shared_ptr<IdeSearch>& search_pane();
    // The Conflicts window, made the first time it is asked for.
    const std::shared_ptr<IdeConflicts>& conflicts_pane();
    // Build the pages for their window entries.
    std::shared_ptr<IdePane> make_search();
    std::shared_ptr<IdePane> make_conflicts();
    std::shared_ptr<IdePane> make_problems();
    // The user's shell, started in the project's folder.
    std::shared_ptr<IdePane> make_terminal();
    // The Assets pane, over the place, with the explorers' actions.
    std::shared_ptr<IdePane> make_assets();
    // Add as GameObject: a GameObject in Workspace for each Prefab in prefabs,
    // as one undo step, and they become the selection.
    void add_as_game_objects(std::vector<engine_core::InstanceId> prefabs);
    // A drag from the Assets pane onto view that holds a Prefab adds each
    // Prefab in it as a GameObject. A drag with none is refused.
    void accept_prefab_drops(jadefx::Node& view);
    // Image, model, and sound files dropped on node, or on anything under it
    // that does not take them, go to import_files. A drop with none is refused.
    void accept_file_drops(jadefx::Node& node);
    // Asks whether to import the image, model, and sound files among files,
    // unless ask is false. Yes copies each image into the project's resources, as
    // import_texture_file does, and makes a Texture under Assets.Textures named
    // after its file with Path set to the copy; each sound the same way, into
    // resources/sounds and a Sound under Assets.Audio. Each model is read and
    // written out as import_model_file does, then made a Prefab with its
    // assets, as build_model_assets does; what it left out goes to the console.
    // Each goes into folder instead when folder is in its category, as
    // place_assets says. All of it is one undo step, the Textures, Sounds, and
    // Prefabs made are selected, and the Assets pane shows them. Needs a
    // stopped test. A place never saved imports into its scratch folder, which
    // its first Save moves into the project.
    void import_files(const std::vector<std::string>& files, engine_core::InstanceId folder = 0, bool ask = true);
    // The Assets pane's Import <kind>: picks files of kind, a Texture's images,
    // a Prefab's models, or a Sound's sounds, then imports them into folder as
    // import_files does, without asking.
    void choose_import(engine_core::InstanceId folder, const std::string& kind);
    // import_files once it may go ahead: reads files into resources, then places them.
    void place_imports(const std::filesystem::path& resources, const std::vector<std::string>& files,
                       engine_core::InstanceId folder);
    // Where Search and Conflicts dock: beside the left explorer, else where editors dock.
    IdeDock* side_home();
    // The status bar's count and the Conflicts window's rows, from conflicts_.
    void show_conflict_count();
    // The status bar's error and warning counts, from Problems' unfiltered list.
    void show_problem_count(const ProblemCounts& total);
    // The status bar's other chips. Each sets its labels only when what it shows changed.
    void show_play_state();
    void show_save_state(bool unsaved);
    void show_cursor_position();
    void show_frame_time();
    void show_zoom();
    void show_ai_client();
    // Selects the instance with this GUID and shows it in every explorer.
    void select_guid(const std::string& guid);
    // Selects only this instance, when it is alive, and shows it in every explorer.
    void select_instance(std::uint32_t id);
    // Puts Select In Explorer, for the instance the page edits, on the page's tab menu.
    void add_select_to_tab_menu(IdePane& pane, std::uint32_t id);
    // A rename or a Properties field is being typed in: a check waits for it.
    bool editing_field() const;
    // Opens a utility window around the node fill returns, and keeps it with
    // the shell's windows. Null, with no window left open, when it cannot open
    // or fill returns null.
    jadefx::UtilityWindow* open_floating(const std::string& title, int width, int height, double screenX,
                                         double screenY, const std::function<std::shared_ptr<jadefx::Node>()>& fill);
    void noteScriptFocus();
    void adoptDock(const std::shared_ptr<IdeDock>& dock);
    void onTabDrag(IdeDock& from, const jadefx::TabDrag& drag);
    void previewDrag(IdeDock& from, const jadefx::TabDrag& drag);
    void applyDrag(IdeDock& from, const jadefx::TabDrag& drag);
    // style is the mark's inline CSS: its outline and fill.
    void showDropMark(jadefx::Scene& scene, double x, double y, double width, double height, const char* style);
    void hideDropMark();
    void floatTab(const std::shared_ptr<jadefx::Tab>& tab, double screenX, double screenY);
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
    // The explorers' actions, kept for the Assets pane, which is made later.
    ExplorerHost explorer_host_;
    ThemeLibrary themes_;
    // Before the docks and pages below, so it is destroyed after them: the
    // console and script editors hold raw pointers into it.
    InputRouter undo_router_;
    // Edits the selection's properties. Docked under the right-hand explorer,
    // and declared before the docks so it outlives its own dock page.
    std::unique_ptr<PropertiesPanel> properties_;
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
    // The open Prefab editors, by Prefab id.
    std::unordered_map<std::uint32_t, std::weak_ptr<IdePrefabEditor>> open_prefabs_;
    std::unordered_map<std::uint32_t, std::weak_ptr<IdeCssEditor>> open_css_;
    std::weak_ptr<class IdeConsole> console_;
    // The Search and Conflicts pages, typed, as their window entries' make last
    // built them. Null until first made. Kept while the tab is closed, so
    // reopening Search keeps the search.
    std::shared_ptr<IdeSearch> search_;
    std::shared_ptr<IdeConflicts> conflicts_pane_;
    // The rows the last check left open.
    std::vector<engine_core::SaveConflict> conflicts_;
    // Why the last check could not read src/, so it is said once.
    std::string disk_problem_;
    // A check is due: the window came back, a test stopped, or an edit held one back.
    bool check_pending_ = false;
    bool was_focused_ = true;
    // A test holds changes on disk back until it stops, and a toast said so.
    bool noted_play_check_ = false;
    // The status bar's conflict count: shown only when there are conflicts.
    jadefx::Node* conflict_count_ = nullptr;
    jadefx::Label* conflict_count_text_ = nullptr;
    std::shared_ptr<jadefx::Tooltip> conflict_tip_;
    // The status bar's error and warning counts, and the counts they last showed.
    jadefx::Label* problem_errors_text_ = nullptr;
    jadefx::Label* problem_warnings_text_ = nullptr;
    std::shared_ptr<jadefx::Tooltip> problem_tip_;
    int shown_errors_ = 0;
    int shown_warnings_ = 0;
    // The status bar's left end: play state and save state.
    layout_detail::StatusChip* play_chip_ = nullptr;
    jadefx::Label* play_state_text_ = nullptr;
    layout_detail::StatusChip* save_chip_ = nullptr;
    jadefx::Label* save_state_text_ = nullptr;
    std::shared_ptr<jadefx::Tooltip> save_tip_;
    int shown_unsaved_ = -1;
    // Its right end: the caret, the frame time, the zoom, and the AI client.
    layout_detail::StatusChip* cursor_chip_ = nullptr;
    jadefx::Label* cursor_text_ = nullptr;
    jadefx::Label* frame_text_ = nullptr;
    std::shared_ptr<jadefx::Tooltip> frame_tip_;
    // The Scene View the frame time is measured on: the studio's first, which stays open.
    runner::GameView* frame_view_ = nullptr;
    // The frame time is set four times a second at most, so it can be read.
    double frame_shown_at_ = -1;
    jadefx::Label* zoom_text_ = nullptr;
    double shown_zoom_ = -1;
    // The zoom chip's slider, made the first time it opens, and the zoom it
    // picked this frame, set at the frame's end. Negative for none.
    std::shared_ptr<class ZoomPopover> zoom_popover_;
    double pending_zoom_ = -1;
    layout_detail::StatusChip* ai_chip_ = nullptr;
    jadefx::Label* ai_text_ = nullptr;
    std::shared_ptr<jadefx::Tooltip> ai_tip_;
    std::vector<std::weak_ptr<class IdeExplorer>> explorers_;
    // The windows the Window menu opens and closes, and layout.json keeps.
    std::vector<std::unique_ptr<WindowEntry>> windows_;
    // The Search, Conflicts, Problems, and Assets entries in windows_.
    WindowEntry* search_window_ = nullptr;
    WindowEntry* conflicts_window_ = nullptr;
    WindowEntry* problems_window_ = nullptr;
    WindowEntry* assets_window_ = nullptr;
    // The Welcome page while one is made. It is not kept in layout.json.
    std::weak_ptr<LandingPage> landing_;
    // Scene views opened so far, which numbers the next one's tab.
    int scene_views_ = 1;
    // The studio's first scene view. It stays open.
    std::shared_ptr<IdePane> scene_view_;
    // layout.json in the config folder. Empty keeps no layout.
    std::filesystem::path layout_file_;
    // default-layout.json in the config folder, which Save Layout as Default writes.
    std::filesystem::path default_layout_file_;
    // The saved default, as last saved or read. Null when there is none.
    engine_core::JsonValue default_layout_;
    // Greyed out while there is no saved default.
    jadefx::MenuItem* restore_builtin_item_ = nullptr;
    // The floating windows layout.json had, until the main window is up to open them.
    engine_core::JsonValue saved_floating_;
    // The main window's place and size from layout.json, until attachFrame.
    engine_core::JsonValue saved_window_;
    // Frames flushed so far.
    std::uint64_t frames_ = 0;
    // The layout just before a floating window closed its tabs, and the frame
    // that was in. A save in that same frame is a quit, and writes this instead.
    engine_core::JsonValue quit_layout_;
    std::uint64_t quit_frame_ = ~std::uint64_t{0};
    void start_mcp();
    // Closes the server and takes this studio out of the registry.
    void stop_mcp();
    std::unique_ptr<McpServer> mcp_;
    // Why the server last failed to start. Cleared when it starts or is turned off.
    std::string mcp_error_;
    // The server's calls into the UI thread. Closed first as the studio closes.
    std::shared_ptr<UiCalls> ui_calls_;
    struct McpIdentity;
    // Null while the server is off.
    std::shared_ptr<McpIdentity> mcp_identity_;
    // MCP tools that wait for the UI thread hold this weakly, so a task that
    // runs after the layout is gone does nothing.
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    // An export is writing; another waits for it to finish.
    bool exporting_ = false;
    std::uint32_t last_script_focus_ = 0;
    // Source from an editor that was closed while the simulation was running.
    // Stop restores the place, then these strings are written back.
    std::unordered_map<std::uint32_t, std::string> kept_sources_;
    std::unique_ptr<Clip> clip_;
    // The studio's built-in plugins, reloaded each time the place is made, opened, or rebuilt.
    PluginLoader plugins_;
    std::unordered_map<std::string, std::vector<int>> script_folds_;
    std::filesystem::path fold_file_;
    // The open project. Null until Open or Save As.
    std::unique_ptr<engine_core::Project> project_;
    // While there is no project, the place's resources folder: what imports
    // and Mesh shapes write goes here until the first Save moves it into the
    // project. Made by start and New, deleted by New, Open, and the destructor.
    // Empty while a project is open.
    std::filesystem::path scratch_resources_;
    // Why the last save wrote nothing: an error, or the files changed on disk. Empty after a save that wrote.
    std::string save_failure_;
    bool dialog_open_ = false;
    bool prompt_open_ = false;
    // What the window title shows now.
    bool title_modified_ = false;
    // During a test the ribbon enables Stop and Shift+F5 stops. F5 resumes
    // only a paused one.
    PlayState play_ = PlayState::Stopped;
    // The ribbon's Test, Pause, Resume, and Stop.
    jadefx::Node* session_buttons_[4] = {};
    // The ribbon's Grid, and whether it is on: the Scene Views draw the floor
    // grid while it is on and no test runs. Kept in the preferences.
    jadefx::Node* grid_button_ = nullptr;
    bool grid_on_ = true;
    // Open alerts. An alert must outlive its popup.
    std::vector<std::shared_ptr<jadefx::Alert>> alerts_;
    // Toasts sent before mount, with their seconds. Mount shows them.
    std::vector<std::pair<std::string, double>> pending_toasts_;
    // Last, so they go before the preferences and themes they edit.
    std::shared_ptr<jadefx::UtilityWindow> preferences_window_;
    std::shared_ptr<PreferencesPanel> preferences_panel_;
};

// An Escape that nothing in `scene` took leaves the focused text field, keeping
// its text, as the explorer's filter does. A field with its own use for Escape,
// such as a rename, a Properties value, or the find bar, keeps that. Every
// studio window's stage calls this with the keys its scene did not consume.
void leave_field_on_escape(jadefx::Scene& scene, int key, bool pressed);

}  // namespace ide
