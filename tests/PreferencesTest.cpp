#include "ide/AiClientsPage.hpp"
#include "ide/IdeResources.hpp"
#include "ide/IdeTheme.hpp"
#include "ide/Preferences.hpp"
#include "ide/PreferencesPanel.hpp"
#include "ide/ThemeLibrary.hpp"

#include "jadefx/jadefx.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

// preferences.json, the themes folder, and the Preferences window's Appearance
// tab, each against a scratch config folder.
namespace {

namespace fs = std::filesystem;

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

void ExpectText(const std::string& got, const std::string& want, const char* message) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s: got [%s], want [%s]\n", message, got.c_str(), want.c_str());
        ++gFailures;
    }
}

std::string Read(const fs::path& path) {
    std::string text;
    std::string error;
    ide::read_file(path, text, error);
    return text;
}

void Write(const fs::path& path, const std::string& text) {
    std::string error;
    Expect(ide::write_file(path, text, error), "a test file is written");
}

// A fresh folder under the system's temporary folder, removed when the test ends.
struct Scratch {
    fs::path root;
    Scratch() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() / ("anarchy-preferences-test-" + std::to_string(stamp));
        fs::create_directories(root);
    }
    ~Scratch() {
        std::error_code error;
        fs::remove_all(root, error);
    }
};

void TestPreferences() {
    Scratch scratch;
    const fs::path file = scratch.root / "preferences.json";
    {
        ide::Preferences preferences(file);
        ExpectText(preferences.theme(), "light", "no file is the light theme");
        Expect(preferences.load_error().empty(), "a missing file is not an error");
        preferences.set_theme("dark");
        std::string error;
        Expect(preferences.save(error), "preferences save");
    }
    ExpectText(ide::Preferences(file).theme(), "dark", "the theme is read back");

    Write(file, "{\n  \"future\": 3,\n  \"theme\": \"dark\"\n}\n");
    {
        ide::Preferences preferences(file);
        preferences.set_theme("user:Mine.css");
        std::string error;
        preferences.save(error);
    }
    const std::string kept = Read(file);
    Expect(kept.find("\"future\": 3") != std::string::npos, "a key the studio does not know is kept");
    Expect(kept.find("\"theme\": \"user:Mine.css\"") != std::string::npos, "and the theme is replaced");

    Write(file, "{ not json");
    ide::Preferences broken(file);
    Expect(!broken.load_error().empty(), "bad JSON is reported");
    ExpectText(broken.theme(), "light", "and the defaults are used");

    ide::Preferences none{fs::path()};
    std::string error;
    Expect(none.save(error), "without a file, saving does nothing");
}

void TestAssetsView() {
    Scratch scratch;
    const fs::path file = scratch.root / "preferences.json";
    {
        ide::Preferences preferences(file);
        ExpectText(preferences.assets_view(), "icons", "a fresh Preferences shows the icon view");
        preferences.set_assets_view("columns");
        std::string error;
        Expect(preferences.save(error), "preferences save");
    }
    ExpectText(ide::Preferences(file).assets_view(), "columns", "the view is read back");

    Write(file, "{ \"assetsView\": \"grid\" }");
    ExpectText(ide::Preferences(file).assets_view(), "icons", "an unknown value reads as icons");
}

void TestMcpEnabled() {
    Scratch scratch;
    const fs::path file = scratch.root / "preferences.json";
    {
        ide::Preferences preferences(file);
        Expect(!preferences.mcp_enabled(), "the MCP server is off until turned on");
        preferences.set_mcp_enabled(true);
        std::string error;
        Expect(preferences.save(error), "preferences save");
    }
    Expect(ide::Preferences(file).mcp_enabled(), "turning it on is remembered");
    Write(file, "{ \"mcpEnabled\": \"yes\" }");
    Expect(!ide::Preferences(file).mcp_enabled(), "a value that is not true reads as off");
}

void TestSceneGrid() {
    Scratch scratch;
    const fs::path file = scratch.root / "preferences.json";
    {
        ide::Preferences preferences(file);
        Expect(preferences.scene_grid(), "the floor grid shows until turned off");
        preferences.set_scene_grid(false);
        std::string error;
        Expect(preferences.save(error), "preferences save");
    }
    Expect(!ide::Preferences(file).scene_grid(), "turning it off is remembered");
    Write(file, "{ \"sceneGrid\": 0 }");
    Expect(ide::Preferences(file).scene_grid(), "a value that is not a boolean reads as on");
}

void TestProfiler() {
    Scratch scratch;
    const fs::path file = scratch.root / "preferences.json";
    {
        ide::Preferences preferences(file);
        Expect(preferences.profiler_tab() == "timeline", "the profiler opens on the Timeline");
        Expect(preferences.profiler_split() == 0.45, "with the lower half at 45%");
        preferences.set_profiler_tab("scopes");
        preferences.set_profiler_split(0.6);
        std::string error;
        Expect(preferences.save(error), "preferences save");
    }
    Expect(ide::Preferences(file).profiler_tab() == "scopes", "the profiler's tab is remembered");
    Expect(ide::Preferences(file).profiler_split() == 0.6, "and its split");
    Write(file, "{ \"profilerTab\": \"graph\", \"profilerSplit\": 0.95 }");
    Expect(ide::Preferences(file).profiler_tab() == "timeline", "an unknown tab reads as the Timeline");
    Expect(ide::Preferences(file).profiler_split() == 0.8, "a split past 0.8 reads as 0.8");
    Write(file, "{ \"profilerSplit\": 0.01 }");
    Expect(ide::Preferences(file).profiler_split() == 0.2, "a split under 0.2 reads as 0.2");
}

void TestLibrary() {
    Scratch scratch;
    const fs::path folder = scratch.root / "themes";
    ide::ThemeLibrary library(folder);

    std::vector<ide::ThemeEntry> entries = library.list();
    Expect(!entries.empty() && std::all_of(entries.begin(), entries.end(), [](const auto& e) { return e.shipped; }),
           "before the folder exists there are only the shipped themes");
    const std::size_t shipped = entries.size();

    std::string error;
    const std::string id = library.save("", "Night Owl", "dark", "dark",
                                        {{"--ide-editor-color", "#010203"}, {"--accent-color", "#ff8800"}}, error);
    ExpectText(id, "user:Night Owl.css", "a new theme is a file named for it");
    const std::string css = Read(folder / "Night Owl.css");
    Expect(css.find("--theme-name: \"Night Owl\";") != std::string::npos, "the file names the theme");
    Expect(css.find("--theme-base: dark;") != std::string::npos, "and its base");
    Expect(css.find("--theme-extends") == std::string::npos, "and extends nothing past its base");
    Expect(css.find("/* Script Editor */") != std::string::npos && css.find("/* Controls */") != std::string::npos,
           "variables sit under their group's heading");
    Expect(css.find("--ide-panel-color") == std::string::npos, "and only what the theme changes is written");

    ide::IdeTheme theme;
    Expect(library.load(id, theme, error), "the theme loads");
    ExpectText(theme.value("--ide-editor-color"), "#010203", "with its own color");
    ExpectText(theme.value("--ide-panel-color"), "#202124", "over the shipped dark theme");
    ExpectText(theme.value("--surface-color"), "#292a2d", "over JadeFX's dark look");
    ExpectText(theme.value("--accent-color"), "#ff8800", "and it can set JadeFX's colors");

    ExpectText(library.save("", "Night Owl", "dark", "dark", {}, error), "user:Night Owl 2.css",
               "a taken name is numbered");
    ExpectText(library.save("", "a/b:c", "light", "light", {}, error), "user:a-b-c.css",
               "a name is made safe for a file");

    // A copy of Dracula is built on Dracula.
    const std::string vampire =
        library.save("", "Vampire", "dark", "dracula", {{"--ide-editor-color", "#000000"}}, error);
    Expect(Read(folder / "Vampire.css").find("--theme-extends: dracula;") != std::string::npos,
           "a theme built on another shipped theme names it");
    Expect(library.load(vampire, theme, error), "and loads");
    ExpectText(library.parent_of(theme), "dracula", "over it");
    ExpectText(theme.value("--ide-editor-color"), "#000000", "with its own colors");
    ExpectText(theme.value("--ide-syntax-string-color"), "#f1fa8c", "and Dracula's for the rest");
    ExpectText(theme.value("--ide-fps-color"), "rgba(0, 0, 0, 0.45)", "and so on up to Dark");
    Write(folder / "Stray.css", ":root { --theme-base: dark; --theme-extends: nowhere; --ide-editor-color: #111; }");
    Expect(library.load("user:Stray.css", theme, error) && library.parent_of(theme) == "dark",
           "a theme that extends no shipped theme is built on its base");
    Expect(library.remove(vampire, error) && library.remove("user:Stray.css", error), "the extra themes are deleted");

    const fs::path outside = scratch.root / "Solar.css";
    Write(outside, "/* by hand */\n:root { --ide-editor-color: #fdf6e3; }\n");
    const std::string imported = library.import_file(outside, error);
    ExpectText(imported, "user:Solar.css", "an imported file keeps its name");
    ExpectText(Read(folder / "Solar.css"), Read(outside), "and its bytes");
    Expect(library.load(imported, theme, error) && theme.base() == "light", "a theme without a base is light");

    Write(scratch.root / "notes.css", "body { color: red; }");
    Expect(library.import_file(scratch.root / "notes.css", error).empty() && !error.empty(),
           "a file with no :root colors is not a theme");

    entries = library.list();
    std::vector<std::string> names;
    for (std::size_t index = shipped; index < entries.size(); ++index) {
        names.push_back(entries[index].name);
    }
    Expect(entries.size() > shipped && !entries[shipped].shipped && entries[shipped - 1].shipped,
           "the user's themes come after the shipped ones");
    Expect(names == std::vector<std::string>{"a/b:c", "Night Owl", "Night Owl", "Solar"},
           "the user's themes follow by name, whatever its case; one without --theme-name uses its file's");

    Expect(!library.load("user:../preferences.json", theme, error), "an id cannot name a file out of the folder");
    Expect(library.save("light", "Light", "light", "light", {}, error).empty(), "a shipped theme is never written");
    Expect(library.save("nord", "Nord", "dark", "dark", {}, error).empty(), "nor is any other");
    Expect(library.remove(imported, error) && !fs::exists(folder / "Solar.css"), "a theme is deleted");
    Expect(!library.remove("dark", error), "a shipped theme is not");

    ide::ThemeLibrary shipped_only{fs::path()};
    Expect(shipped_only.list().size() == shipped, "without a folder there are only the shipped themes");
    Expect(shipped_only.save("", "X", "light", "light", {}, error).empty(), "and nothing to save to");
}

void TestWriteTheme() {
    const ide::ThemeValues values = {{"--zzz-custom", "1px"}, {"--ide-syntax-string-color", "#00ff00"},
                                     {"--ide-window-color", "rgba(1, 2, 3, 0.5)"}};
    const std::string css = ide::write_theme("Odd \"*/ name;", "dark", "", values);
    const ide::ThemeValues parsed = ide::parse_theme_variables(css);
    Expect(parsed == ide::ThemeValues{{"--theme-name", "\"Odd * name\""}, {"--theme-base", "dark"},
                                      {"--ide-window-color", "rgba(1, 2, 3, 0.5)"},
                                      {"--ide-syntax-string-color", "#00ff00"}, {"--zzz-custom", "1px"}},
           "a written theme reads back, in list order, with a name that cannot break the file");

    Expect(ide::write_theme("X", "dark", "dracula", {}).find("--theme-extends: dracula;") != std::string::npos,
           "a theme names the shipped theme it extends");
    Expect(ide::write_theme("X", "dark", "dark", {}).find("--theme-extends") == std::string::npos,
           "unless that is its base");
    ExpectText(ide::css_color(jadefx::Color::rgb8(26, 115, 232)), "#1a73e8", "an opaque color is hex");
    ExpectText(ide::css_color(jadefx::Color::rgb8(234, 92, 0, 56)), "rgba(234, 92, 0, 0.22)",
               "a translucent one is rgba");
}

void TestCurrentTheme() {
    int heard = 0;
    {
        auto probe = jadefx::make<jadefx::Pane>();
        probe->setStyle("background-color: var(--ide-editor-color);");
        auto scene = jadefx::make<jadefx::Scene>(probe, 100, 100);
        scene->layout(100, 100, 0.1);
        ExpectText(probe->computedStyle().background.color.toHex(), "#ffffff", "a scene draws with the light theme");

        ide::ThemeListener listener([&heard] { ++heard; });
        ide::set_current_theme(ide::shipped_theme("dark"));
        Expect(heard == 1, "a listener hears a change of theme");
        ExpectText(ide::current_theme().base(), "dark", "the current theme is the one set");

        scene->layout(100, 100, 0.2);
        ExpectText(probe->computedStyle().background.color.toHex(), "#1e1f22",
                   "and at its next layout with the theme set since");
        ExpectText(scene->computedStyle().variable("--ide-editor-color"), "#1e1f22",
                   "every scene gets the theme's variables");
        ExpectText(scene->computedStyle().variable("--surface-color"), "#292a2d", "and its base look's");
    }
    ide::set_current_theme(ide::shipped_theme("light"));
    Expect(heard == 1, "a listener that is gone hears nothing");
}

void TestPanel() {
    Scratch scratch;
    ide::ThemeLibrary library(scratch.root / "themes");
    ide::Preferences preferences(scratch.root / "preferences.json");
    auto panel = jadefx::make<ide::PreferencesPanel>(library, preferences);
    auto scene = jadefx::make<jadefx::Scene>(panel, 700, 640);
    scene->layout(700, 640, 0.1);

    ExpectText(panel->theme_id(), "light", "the panel shows the preferred theme");
    const std::vector<jadefx::Node*> scrolls = panel->getElementsByClassName("prefs-scroll");
    Expect(scrolls.size() == 1 && static_cast<jadefx::ScrollPane*>(scrolls.front())->getVvalue() == 0,
           "the colors open scrolled to the top");
    Expect(panel->picker("--ide-syntax-keyword-color") != nullptr && panel->picker("--accent-color") != nullptr,
           "every studio and JadeFX color has a picker");
    ExpectText(panel->picker("--ide-syntax-keyword-color")->getValue().toHex(), "#7a3e9d", "showing its color");

    Expect(panel->select_theme("dark"), "picking Dark");
    ExpectText(ide::current_theme().base(), "dark", "draws with it");
    ExpectText(ide::Preferences(scratch.root / "preferences.json").theme(), "dark", "and remembers it");

    // Dragging in the chooser shows the color at once, but does not keep it.
    jadefx::ColorPicker* editor = panel->picker("--ide-editor-color");
    editor->setValue(jadefx::Color::rgb8(0, 0, 255));
    ExpectText(ide::current_theme().value("--ide-editor-color"), "#0000ff", "a picker's color previews live");
    Expect(!panel->modified(), "without being an edit");
    editor->setValue(jadefx::Color::parse("#1e1f22"));

    panel->set_color("--ide-editor-color", jadefx::Color::rgb8(255, 0, 0));
    panel->set_color("--ide-panel-color", jadefx::Color::parse("#202124"));
    Expect(panel->modified(), "a color is an edit");
    ExpectText(ide::current_theme().value("--ide-editor-color"), "#ff0000", "that the studio draws");
    Expect(!panel->save() && panel->theme_id() == "dark", "a shipped theme cannot be saved over");
    Expect(!fs::exists(scratch.root / "themes"), "so nothing was written");

    Expect(panel->save_as("  Crimson  "), "Save As");
    ExpectText(panel->theme_id(), "user:Crimson.css", "picks the new theme");
    Expect(!panel->modified(), "with nothing left unsaved");
    ExpectText(ide::Preferences(scratch.root / "preferences.json").theme(), "user:Crimson.css", "and remembers it");
    const ide::ThemeValues saved = ide::parse_theme_variables(Read(scratch.root / "themes" / "Crimson.css"));
    Expect(saved == ide::ThemeValues{{"--theme-name", "\"Crimson\""}, {"--theme-base", "dark"},
                                     {"--ide-editor-color", "#ff0000"}},
           "the file holds only the color that differs from Dark");

    panel->reset_color("--ide-editor-color");
    ExpectText(ide::current_theme().value("--ide-editor-color"), "#1e1f22", "Reset gives back Dark's color");
    Expect(panel->modified(), "as an edit");
    panel->revert();
    ExpectText(ide::current_theme().value("--ide-editor-color"), "#ff0000", "Revert drops it");

    panel->set_color("--ide-syntax-string-color", jadefx::Color::rgb8(1, 2, 3));
    Expect(panel->save(), "a user's theme saves in place");
    Expect(Read(scratch.root / "themes" / "Crimson.css").find("--ide-syntax-string-color: #010203;") !=
               std::string::npos,
           "with the new color");

    Expect(!panel->save_as("   "), "Save As needs a name");
    Expect(panel->status().find("name") != std::string::npos, "and says so");

    const fs::path outside = scratch.root / "Paper.css";
    Write(outside, ":root { --theme-name: \"Paper\"; --ide-editor-color: #fffff0; }");
    Expect(panel->import_theme(ide::utf8_path(outside)), "Import");
    ExpectText(panel->theme_id(), "user:Paper.css", "picks the imported theme");
    ExpectText(ide::current_theme().value("--ide-editor-color"), "#fffff0", "and draws with it");
    Expect(!panel->import_theme(ide::utf8_path(scratch.root / "missing.css")), "a missing file is refused");
    Expect(panel->status().find("Could not import") == 0, "and the status line says why");

    // A theme file dropped on the window is imported too.
    Write(scratch.root / "Dusk.css", ":root { --theme-base: dark; --ide-editor-color: #102030; }");
    scene->layout(700, 640, 0.2);
    Expect(scene->noteFileDrop(350, 320, {ide::utf8_path(scratch.root / "Dusk.css")}), "a dropped theme file is taken");
    ExpectText(panel->theme_id(), "user:Dusk.css", "and picked");

    Expect(panel->delete_theme(), "Delete");
    ExpectText(panel->theme_id(), "dark", "picks the shipped theme of its base");
    Expect(!fs::exists(scratch.root / "themes" / "Dusk.css"), "and removes the file");
    Expect(!panel->delete_theme(), "a shipped theme cannot be deleted");

    // Edits to Nord save as a theme built on Nord, and deleting it goes back to Nord.
    Expect(panel->select_theme("nord"), "picking Nord");
    ExpectText(ide::current_theme().value("--ide-syntax-keyword-color"), "#81a1c1", "draws with Nord's colors");
    panel->set_color("--ide-syntax-string-color", jadefx::Color::rgb8(255, 255, 0));
    Expect(panel->save_as("Fjord"), "Save As from Nord");
    const std::string fjord = Read(scratch.root / "themes" / "Fjord.css");
    Expect(fjord.find("--theme-extends: nord;") != std::string::npos, "builds the copy on Nord");
    Expect(fjord.find("--ide-syntax-string-color: #ffff00;") != std::string::npos &&
               fjord.find("--ide-syntax-keyword-color") == std::string::npos,
           "with only the color that differs from Nord");
    ExpectText(ide::current_theme().value("--ide-syntax-keyword-color"), "#81a1c1", "the copy keeps Nord's colors");
    Expect(panel->delete_theme(), "deleting the copy");
    ExpectText(panel->theme_id(), "nord", "goes back to Nord");

    panel->set_color("--ide-editor-color", jadefx::Color::rgb8(9, 9, 9));
    bool closed = false;
    Expect(panel->request_close([&closed] { closed = true; }) == false, "closing with edits asks first");
    panel->revert();
    Expect(panel->request_close([&closed] { closed = true; }), "without edits the window just closes");

    ide::set_current_theme(ide::shipped_theme("light"));
}

void Click(jadefx::Scene& scene, jadefx::Node& node, double& time) {
    const double x = node.getAbsoluteX() + node.getWidth() * 0.5;
    const double y = node.getAbsoluteY() + node.getHeight() * 0.5;
    scene.noteMove(x, y);
    scene.noteButton(0, true, x, y, 0);
    scene.noteButton(0, false, x, y, 0);
    jadefx::drainRunLater();
    time += 0.1;
    scene.layout(700, 640, time);
}

// The list's own row, clicked: picking a theme rebuilds what the list shows,
// which must wait until the list is done with the click.
void TestPickFromList() {
    Scratch scratch;
    ide::ThemeLibrary library(scratch.root / "themes");
    ide::Preferences preferences(scratch.root / "preferences.json");
    auto panel = jadefx::make<ide::PreferencesPanel>(library, preferences);
    auto scene = jadefx::make<jadefx::Scene>(panel, 700, 640);
    double time = 0.1;
    scene->layout(700, 640, time);
    const std::vector<jadefx::Node*> lists = panel->getElementsByClassName("prefs-theme-list");
    Expect(lists.size() == 1, "the theme list is on the tab");
    if (lists.empty()) {
        return;
    }
    Click(*scene, *lists.front(), time);
    jadefx::Node* dark = nullptr;
    for (double y = 0; y < 640 && dark == nullptr; y += 4) {
        for (jadefx::Node* node = scene->pick(lists.front()->getAbsoluteX() + 12, y); node != nullptr;
             node = node->getParent()) {
            if (std::strcmp(node->getElementType(), "combo-row") == 0 && node->getElementId() == "Dark") {
                dark = node;
                break;
            }
        }
    }
    Expect(dark != nullptr, "the open list shows Dark");
    if (dark == nullptr) {
        return;
    }
    Click(*scene, *dark, time);
    ExpectText(panel->theme_id(), "dark", "clicking Dark in the list picks it");
    ExpectText(ide::current_theme().base(), "dark", "and the studio draws with it");
    ide::set_current_theme(ide::shipped_theme("light"));
}

// Groups start closed. A click on a heading puts its colors in the list, and another takes them out.
void TestGroups() {
    Scratch scratch;
    ide::ThemeLibrary library(scratch.root / "themes");
    ide::Preferences preferences(scratch.root / "preferences.json");
    auto panel = jadefx::make<ide::PreferencesPanel>(library, preferences);
    auto scene = jadefx::make<jadefx::Scene>(panel, 700, 640);
    double time = 0.1;
    scene->layout(700, 640, time);
    jadefx::Node* studio = panel->group_heading("Studio");
    jadefx::Node* ribbon = panel->group_heading("Ribbon");
    Expect(studio != nullptr && ribbon != nullptr && panel->group_heading("Controls") != nullptr,
           "each group has a heading");
    Expect(panel->group_heading("Nowhere") == nullptr, "and only the groups there are");
    if (studio == nullptr || ribbon == nullptr) {
        return;
    }
    const auto listed = [&panel](const char* name) { return panel->picker(name)->getScene() != nullptr; };
    Expect(!listed("--ide-window-color") && !listed("--ide-ribbon-color") && !listed("--accent-color"),
           "every group starts closed");

    // A short list leaves room below it: the tab still fills the window, its buttons at the bottom.
    const auto footer_at_bottom = [&panel]() {
        const std::vector<jadefx::Node*> footers = panel->getElementsByClassName("prefs-footer");
        if (footers.size() != 1) {
            return false;
        }
        const double bottom = footers.front()->getAbsoluteY() + footers.front()->getHeight();
        return bottom > 636 && bottom <= 640.5 && footers.front()->getWidth() > 690;
    };
    Expect(footer_at_bottom(), "with every group closed the buttons sit at the window's bottom");

    Click(*scene, *studio, time);
    Expect(listed("--ide-window-color") && listed("--ide-text-color"), "a click on its heading opens a group");
    Expect(!listed("--ide-ribbon-color"), "and leaves the others closed");
    Click(*scene, *ribbon, time);
    Expect(listed("--ide-ribbon-color"), "the group now below it opens too");
    Expect(footer_at_bottom(), "and with a long list, where they were");
    Click(*scene, *studio, time);
    Expect(!listed("--ide-window-color"), "another click closes it again");

    const std::vector<jadefx::Node*> filters = panel->getElementsByClassName("prefs-filter");
    Expect(filters.size() == 1, "the filter is on the tab");
    if (filters.empty()) {
        return;
    }
    auto* filter = static_cast<jadefx::TextField*>(filters.front());
    filter->setText("window");
    scene->layout(700, 640, time += 0.1);
    Expect(listed("--ide-window-color"), "a filter opens a closed group with a match");
    Expect(!listed("--ide-text-color"), "showing only what matches");
    filter->setText("");
    scene->layout(700, 640, time += 0.1);
    Expect(!listed("--ide-window-color") && !listed("--ide-popup-color"), "clearing it closes them again");
    Expect(listed("--ide-ribbon-color"), "and the group that was open stays open");
}

void TestFrameRate() {
    Scratch scratch;
    const fs::path file = scratch.root / "preferences.json";
    {
        ide::Preferences preferences(file);
        Expect(preferences.frame_rate() == 120, "the studio draws at 120 fps unless told otherwise");
        preferences.set_frame_rate(-1);
        Expect(preferences.frame_rate() == -1, "-1 is uncapped");
        Expect(ide::Preferences::stage_frame_rate(-1) == 0, "which the stage takes as 0");
        preferences.set_frame_rate(3);
        Expect(preferences.frame_rate() == 15, "a rate too slow to use is raised");
    }
    Write(file, "{ \"frameRate\": 0 }");
    Expect(ide::Preferences(file).frame_rate() == 120, "a rate of 0 in the file is the default");
    Write(file, "{ \"frameRate\": \"fast\" }");
    Expect(ide::Preferences(file).frame_rate() == 120, "and so is one that is not a number");

    ide::ThemeLibrary library(scratch.root / "themes");
    ide::Preferences preferences(file);
    auto panel = jadefx::make<ide::PreferencesPanel>(library, preferences);
    auto scene = jadefx::make<jadefx::Scene>(panel, 700, 640);
    scene->layout(700, 640, 0.1);
    int heard = 0;
    panel->set_on_frame_rate([&heard](int fps) { heard = fps; });
    Expect(panel->frame_rate_field() != nullptr && panel->frame_rate_field()->getText() == "120",
           "the Performance tab shows the limit");

    Expect(panel->set_frame_rate(" 144 "), "a whole number is a limit");
    Expect(heard == 144, "that the studio is told");
    Expect(ide::Preferences(file).frame_rate() == 144, "and that is remembered");
    Expect(panel->set_frame_rate("-1") && heard == -1, "-1 uncaps it");
    Expect(ide::Preferences(file).frame_rate() == -1, "which is remembered too");
    heard = 0;
    for (const char* bad : {"", "abc", "60fps", "0", "5", "-2", "2000", "99999999999"}) {
        Expect(!panel->set_frame_rate(bad), (std::string("not a limit: '") + bad + "'").c_str());
    }
    Expect(heard == 0 && ide::Preferences(file).frame_rate() == -1, "and those change nothing");
    Expect(!panel->frame_rate_status().empty(), "saying why");

    // There is no Apply button: leaving the field takes what was typed.
    auto* tabs = dynamic_cast<jadefx::TabPane*>(panel->getCenter());
    jadefx::TextField* field = panel->frame_rate_field();
    Expect(tabs != nullptr && field != nullptr, "the panel is tabs");
    if (tabs == nullptr || field == nullptr) {
        return;
    }
    double time = 0.2;
    const auto leave_field = [&] {
        tabs->select(std::size_t{0});
        scene->layout(700, 640, time += 0.1);
        tabs->select(std::size_t{1});
        scene->layout(700, 640, time += 0.1);
    };
    tabs->select(std::size_t{1});
    scene->layout(700, 640, time += 0.1);
    field->requestFocus();
    Expect(field->isFocused(), "the field takes the focus");
    field->setText("90");
    leave_field();
    Expect(heard == 90 && ide::Preferences(file).frame_rate() == 90, "leaving the field sets the limit");

    field->requestFocus();
    field->setText("fast");
    leave_field();
    Expect(heard == 90 && ide::Preferences(file).frame_rate() == 90, "leaving it on text that is not a limit keeps the limit");
    ExpectText(field->getText(), "90", "and the field shows the limit again");
    Expect(!panel->frame_rate_status().empty(), "saying why");

    heard = 0;
    field->requestFocus();
    leave_field();
    Expect(heard == 0, "leaving it unchanged sets nothing");

    field->requestFocus();
    field->setText("75");
    Expect(panel->request_close([] {}), "the window closes with no colors changed");
    Expect(heard == 75 && ide::Preferences(file).frame_rate() == 75, "and a limit still being typed is set");
}

}  // namespace

// The AI tab against a pretend claude, which keeps what is registered as anarchy.
void TestAiPage() {
    struct FakeClaude {
        std::string registered;  // The command, or empty.
        int fail_add = 0;
        std::vector<std::string> calls;
    };
    auto fake = std::make_shared<FakeClaude>();
    auto enabled = std::make_shared<bool>(false);
    const fs::path bridge = ide::path_from_utf8("C:/Anarchy/anarchy-mcp.exe");

    const auto make_page = [&](fs::path cli, ide::McpSwitch forced) {
        ide::AiClientsPage::Server server;
        server.setting = [enabled, forced] {
            return forced.forced_by.empty() ? ide::McpSwitch{*enabled, {}} : forced;
        };
        server.set_enabled = [enabled](bool on) { *enabled = on; };
        server.status = [enabled] { return *enabled ? std::string("Listening") : std::string("Off"); };
        ide::AiClientsPage::ClaudeCode claude;
        claude.cli = std::move(cli);
        claude.bridge = bridge;
        claude.run = [fake](const std::vector<std::string>& args) {
            std::string joined;
            for (const std::string& arg : args) {
                joined += (joined.empty() ? "" : " ") + arg;
            }
            fake->calls.push_back(joined);
            ide::ProcessResult result;
            result.started = true;
            result.exit_code = 0;
            if (args[1] == "get") {
                if (fake->registered.empty()) {
                    result.exit_code = 1;
                    result.output = "No MCP server named \"anarchy\".\n";
                } else {
                    result.output = "anarchy:\n  Command: " + fake->registered + "\n  Args: \n";
                }
            } else if (args[1] == "add") {
                if (fake->fail_add != 0) {
                    result.exit_code = fake->fail_add;
                    result.output = "\nboom\nmore\n";
                } else {
                    fake->registered = args.back();
                }
            } else if (args[1] == "remove") {
                fake->registered.clear();
            }
            return result;
        };
        claude.async = [](std::function<void()> work, std::function<void()> done) {
            work();
            done();
        };
        return jadefx::make<ide::AiClientsPage>(std::move(server), std::move(claude));
    };

    auto page = make_page("claude", {});
    Expect(!page->server_box()->isSelected() && !page->server_box()->isDisable(), "the server box starts off, and works");
    ExpectText(page->server_status(), "Off", "the server's state shows");
    Expect(fake->calls.size() == 1 && fake->calls[0] == "mcp get anarchy", "opening the tab asks what is registered");
    Expect(page->registration() == ide::ClaudeRegistration::NotRegistered, "nothing is");
    Expect(!page->connect_button()->isDisable() && page->disconnect_button()->isDisable(),
           "so Connect works and Disconnect does not");

    page->connect_claude();
    ExpectText(fake->calls[1], "mcp add anarchy --scope user -- " + ide::utf8_path(bridge), "Connect adds the bridge");
    ExpectText(fake->calls[2], "mcp get anarchy", "then looks again");
    Expect(page->registration() == ide::ClaudeRegistration::Connected, "and finds it connected");
    Expect(page->claude_message().find("Turn on the MCP server") != std::string::npos,
           "with a reminder while the server is off");
    Expect(page->connect_button()->isDisable() && !page->disconnect_button()->isDisable(), "Disconnect now works");

    page->set_server_enabled(true);
    Expect(*enabled && page->server_box()->isSelected(), "the box turns the server on");
    ExpectText(page->server_status(), "Listening", "and its state shows");
    Expect(page->claude_message().find("Turn on") == std::string::npos, "and the reminder goes");

    fake->registered = "C:/Old/anarchy-mcp.exe";
    fake->calls.clear();
    page->refresh_claude();
    Expect(page->registration() == ide::ClaudeRegistration::Elsewhere, "another bridge is seen");
    Expect(page->claude_status().find("C:/Old/anarchy-mcp.exe") != std::string::npos, "and named");
    page->connect_claude();
    Expect(fake->calls.size() == 4 && fake->calls[1] == "mcp remove anarchy --scope user" &&
               fake->calls[2].rfind("mcp add", 0) == 0,
           "Connect replaces it");
    Expect(page->registration() == ide::ClaudeRegistration::Connected, "with this one");

    page->disconnect_claude();
    Expect(page->registration() == ide::ClaudeRegistration::NotRegistered && fake->registered.empty(),
           "Disconnect takes it out");

    fake->fail_add = 1;
    page->connect_claude();
    ExpectText(page->claude_message(), "Could not connect: boom", "a failure shows what claude said first");
    Expect(page->registration() == ide::ClaudeRegistration::NotRegistered, "and nothing changed");
    fake->fail_add = 0;

    auto forced = make_page("claude", {true, "ANARCHY_MCP_PORT"});
    Expect(forced->server_box()->isSelected() && forced->server_box()->isDisable(),
           "an environment variable decides, and the box cannot");

    fake->calls.clear();
    auto missing = make_page({}, {});
    Expect(fake->calls.empty(), "without claude, nothing runs");
    Expect(missing->claude_status().find("not found") != std::string::npos, "and the tab says it was not found");
    Expect(missing->connect_button()->isDisable(), "so Connect does not work");
}

int RunPreferencesTests() {
    gFailures = 0;
    TestPreferences();
    TestAssetsView();
    TestMcpEnabled();
    TestSceneGrid();
    TestProfiler();
    TestLibrary();
    TestWriteTheme();
    TestCurrentTheme();
    TestPanel();
    TestPickFromList();
    TestGroups();
    TestFrameRate();
    TestAiPage();
    return gFailures;
}
