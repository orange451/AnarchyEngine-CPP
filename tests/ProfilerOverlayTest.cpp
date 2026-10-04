#include "ide/IdeLayout.hpp"
#include "ide/IdePane.hpp"
#include "runner/GameView.hpp"
#include "runner/ProfilerOverlay.hpp"
#include "runner/Runner.hpp"

#include "Engine.hpp"
#include "UserInputService.hpp"
#include "profiler/Profiler.hpp"

#include "jadefx/jadefx.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

int gFailures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

runner::GameView* scene_view(jadefx::Scene& scene, const std::string& name) {
    for (jadefx::Node* node : scene.getRoot()->getElementsByClassName("ide-pane")) {
        auto* pane = dynamic_cast<runner::GameView*>(node);
        if (pane != nullptr && pane->name() == name) {
            return pane;
        }
    }
    return nullptr;
}

jadefx::MenuItem* menu_item(jadefx::Scene& scene, const std::string& menu_text, const std::string& item_text) {
    // The menu bar is the first row of the root's top.
    auto* root = dynamic_cast<jadefx::BorderPane*>(scene.getRoot());
    auto* top = root != nullptr ? dynamic_cast<jadefx::VBox*>(root->getTop()) : nullptr;
    auto* bar = top != nullptr && !top->getChildren().empty()
                    ? dynamic_cast<jadefx::MenuBar*>(top->getChildren()[0].get())
                    : nullptr;
    if (bar == nullptr) {
        return nullptr;
    }
    for (const std::shared_ptr<jadefx::Menu>& menu : bar->getMenus().items()) {
        if (menu->getText() != menu_text) {
            continue;
        }
        for (const std::shared_ptr<jadefx::MenuItem>& item : menu->getItems().items()) {
            if (item && item->getText() == item_text) {
                return item.get();
            }
        }
    }
    return nullptr;
}

// Brings a tab to the front of its dock, as clicking it does.
void bring_forward(jadefx::Scene& scene, const std::string& title) {
    for (jadefx::Node* node : scene.getRoot()->getElementsByClassName("tab-pane")) {
        auto* tabs = dynamic_cast<jadefx::TabPane*>(node);
        if (tabs == nullptr) {
            continue;
        }
        for (const std::shared_ptr<jadefx::Tab>& tab : tabs->getTabs().items()) {
            if (tab && tab->getText() == title) {
                tabs->select(tab);
            }
        }
    }
}

// On screen with the overlay laid out over it. A view behind another tab draws nothing.
bool shows_overlay(runner::GameView* view) {
    return view != nullptr && view->getScene() != nullptr && view->profilerOverlay().isVisible() &&
           view->profilerOverlay().getWidth() > 100;
}

void click(jadefx::Scene& scene, const runner::ProfilerOverlay::Rect& rect) {
    const double x = rect.x + rect.w * 0.5;
    const double y = rect.y + rect.h * 0.5;
    scene.noteMove(x, y);
    scene.noteButton(0, true, x, y);
    scene.noteButton(0, false, x, y);
}

}  // namespace

// The profiler over the studio's Scene Views: the hotkeys, which view shows it,
// freeing a locked pointer, and clicks on the graph, the tabs, and the table.
int RunProfilerOverlayTests(ide::IdeLayout& layout, jadefx::Scene& scene) {
    gFailures = 0;
    profiler::reset_for_testing();
    double time = 90.0;
    auto frame = [&] {
        scene.layout(1280, 800, time);
        layout.flushFrame();
        time += 0.3;
    };
    auto key = [&](int code, int mods) {
        scene.noteKey(code, true, false, mods);
        scene.noteKey(code, false, false, 0);
        frame();
    };
    // Earlier tests leave script tabs in front of it.
    bring_forward(scene, "Scene View");
    frame();
    frame();
    runner::ProfilerUi& ui = runner::ProfilerUi::get();
    runner::GameView* first = scene_view(scene, "Scene View");
    expect(first != nullptr, "the studio's Scene View shows");
    if (first == nullptr) {
        return gFailures;
    }
    expect(!ui.shown() && !shows_overlay(first) && !profiler::enabled(), "the profiler starts hidden and off");

    // The View menu has both, with their shortcuts.
    jadefx::MenuItem* toggle = menu_item(scene, "View", "Profiler");
    jadefx::MenuItem* pause = menu_item(scene, "View", "Pause Profiler");
    expect(toggle != nullptr && toggle->getAcceleratorKey() == jadefx::Key::F6 &&
               toggle->getAcceleratorMods() == jadefx::Key::ModControl,
           "View > Profiler is Cmd+F6");
    expect(pause != nullptr && pause->getAcceleratorKey() == jadefx::Key::P &&
               pause->getAcceleratorMods() == jadefx::Key::ModControl,
           "View > Pause Profiler is Cmd+P");

    // Cmd+F6 shows it over the studio's first view and starts recording.
    key(jadefx::Key::F6, jadefx::Key::ModControl);
    expect(ui.shown() && shows_overlay(first) && profiler::enabled(), "Cmd+F6 shows the profiler and records");
    expect(!first->cameraList().isVisible(), "the camera list is out of the overlay's way");
    key(jadefx::Key::P, jadefx::Key::ModControl);
    expect(profiler::paused(), "Cmd+P pauses");
    key(jadefx::Key::P, jadefx::Key::ModControl);
    expect(!profiler::paused(), "Cmd+P again resumes");

    // Both work while a text field has the keyboard.
    for (jadefx::Node* node : scene.getRoot()->getElementsByClassName("text-field")) {
        if (node->isVisible()) {
            node->requestFocus();
            break;
        }
    }
    frame();
    key(jadefx::Key::P, jadefx::Key::ModControl);
    expect(profiler::paused(), "Cmd+P pauses with a text field focused");
    key(jadefx::Key::P, jadefx::Key::ModControl);
    key(jadefx::Key::F6, jadefx::Key::ModControl);
    expect(!ui.shown() && !shows_overlay(first) && !profiler::enabled(), "Cmd+F6 hides it with a text field focused");

    // A script's pointer lock is freed while the overlay shows; MouseBehavior keeps its value.
    engine_core::Engine& engine = layout.simulation();
    engine.datamodel().input().set_mouse_behavior(engine_core::UserInputService::kLockCenter);
    expect(first->pointerWanted(), "a LockCenter script wants the pointer");
    key(jadefx::Key::F6, jadefx::Key::ModControl);
    expect(!first->pointerWanted(), "the profiler frees it");
    expect(engine.datamodel().input().mouse_behavior() == engine_core::UserInputService::kLockCenter,
           "MouseBehavior stays as the script set it");
    key(jadefx::Key::F6, jadefx::Key::ModControl);
    expect(first->pointerWanted(), "hiding the profiler lets the lock come back");
    engine.datamodel().input().set_mouse_behavior(engine_core::UserInputService::kMouseBehaviorDefault);

    // Hidden while paused, it comes back live, not on the old frames.
    key(jadefx::Key::F6, jadefx::Key::ModControl);
    key(jadefx::Key::P, jadefx::Key::ModControl);
    key(jadefx::Key::F6, jadefx::Key::ModControl);
    key(jadefx::Key::F6, jadefx::Key::ModControl);
    expect(ui.shown() && !profiler::paused(), "shown again after hiding while paused, it is live");
    key(jadefx::Key::F6, jadefx::Key::ModControl);

    // Clicks: a frame bar selects that frame and pauses; the tabs switch; a header sorts.
    key(jadefx::Key::F6, jadefx::Key::ModControl);
    profiler::register_thread("Render");
    const profiler::ScopeId work = profiler::intern("Overlay test work", profiler::Group::Engine);
    for (int index = 0; index < 6; ++index) {
        profiler::frame_boundary();
        profiler::begin(work);
        profiler::end();
    }
    profiler::frame_boundary();
    profiler::collect();
    frame();
    runner::ProfilerOverlay& overlay = first->profilerOverlay();
    std::size_t count = 0;
    profiler::with_view([&](const profiler::History& history) { count = history.frames.size(); });
    expect(count == 6, "six frames recorded");
    click(scene, overlay.barRect(count - 3, count));
    frame();
    expect(profiler::paused(), "clicking a frame pauses");
    expect(ui.selected == count - 3, "and selects that frame");
    click(scene, overlay.tabRect(runner::ProfilerUi::Tab::Scopes));
    frame();
    expect(ui.tab == runner::ProfilerUi::Tab::Scopes, "the Scopes tab opens");
    click(scene, overlay.columnRect(runner::ProfilerOverlay::kAvgColumn));
    frame();
    expect(overlay.sort() == profiler::StatSort::Avg, "clicking Avg sorts by it");
    click(scene, overlay.tabRect(runner::ProfilerUi::Tab::Timeline));
    frame();
    expect(ui.tab == runner::ProfilerUi::Tab::Timeline, "the Timeline tab opens again");
    click(scene, overlay.pauseRect());
    frame();
    expect(!profiler::paused(), "the pause button resumes");

    // A second Scene View: clicking it moves the profiler there.
    if (jadefx::MenuItem* add = menu_item(scene, "Window", "New Scene View")) {
        add->fire();
    }
    frame();
    frame();
    // Earlier tests numbered other views; the new one is whichever is not the first.
    runner::GameView* second = nullptr;
    for (jadefx::Node* node : scene.getRoot()->getElementsByClassName("ide-pane")) {
        auto* view = dynamic_cast<runner::GameView*>(node);
        if (view != nullptr && view != first) {
            second = view;
        }
    }
    expect(second != nullptr, "a second Scene View opens");
    const std::string second_name = second != nullptr ? second->name() : std::string();
    if (second != nullptr) {
        const double x = second->getAbsoluteX() + second->getWidth() * 0.5;
        const double y = second->getAbsoluteY() + second->getHeight() - 8;
        scene.noteButton(0, true, x, y);
        scene.noteButton(0, false, x, y);
        frame();
        expect(shows_overlay(second) && !shows_overlay(first), "the profiler follows the view last clicked");
        // Closing that view leaves the profiler on the one that is left.
        for (jadefx::Node* node : scene.getRoot()->getElementsByClassName("tab-pane")) {
            if (auto* tabs = dynamic_cast<jadefx::TabPane*>(node)) {
                tabs->getTabs().removeIf(
                    [&](const std::shared_ptr<jadefx::Tab>& tab) { return tab && tab->getText() == second_name; });
            }
        }
        frame();
        frame();
        expect(scene_view(scene, second_name) == nullptr, "the second view closed");
        bring_forward(scene, "Scene View");
        frame();
        key(jadefx::Key::F6, jadefx::Key::ModControl);
        key(jadefx::Key::F6, jadefx::Key::ModControl);
        first = scene_view(scene, "Scene View");
        expect(shows_overlay(first), "the profiler shows on the remaining view");
    }
    key(jadefx::Key::F6, jadefx::Key::ModControl);
    expect(!ui.shown(), "hidden at the end");

    // Captures: File > Open Profile Capture, and Save from a paused profiler.
    expect(menu_item(scene, "File", "Open Profile Capture\u2026") != nullptr, "File has Open Profile Capture");
    key(jadefx::Key::F6, jadefx::Key::ModControl);
    for (int index = 0; index < 4; ++index) {
        profiler::frame_boundary();
        profiler::begin(work);
        profiler::end();
    }
    profiler::frame_boundary();
    profiler::collect();
    key(jadefx::Key::P, jadefx::Key::ModControl);
    expect(static_cast<bool>(ui.save), "a paused studio profiler can be saved");
    namespace fs = std::filesystem;
    const fs::path folder = fs::temp_directory_path() / "anarchy-profile-test";
    fs::create_directories(folder);
    const fs::path saved = folder / "saved.aprof.json";
    std::string error;
    expect(layout.save_profile_capture(saved, error) && fs::exists(saved), "save writes the paused history");
    key(jadefx::Key::P, jadefx::Key::ModControl);
    key(jadefx::Key::F6, jadefx::Key::ModControl);
    const bool opened = layout.open_profile_capture(saved, error);
    expect(opened, "the saved capture opens");
    frame();
    std::string name;
    std::size_t frames = 0;
    profiler::with_view([&](const profiler::History& history) {
        name = history.capture_name;
        frames = history.frames.size();
    });
    expect(ui.shown() && profiler::showing_capture() && profiler::paused(), "opening shows it, paused");
    expect(name == "saved.aprof.json" && frames == 4, "with its name and its frames");
    {
        std::ofstream(folder / "broken.aprof.json") << "{\"format\":\"anarchy-profile\",\"version\":";
    }
    error.clear();
    expect(!layout.open_profile_capture(folder / "broken.aprof.json", error) && !error.empty(),
           "a broken capture is refused, with why");
    profiler::with_view([&](const profiler::History& history) { name = history.capture_name; });
    expect(name == "saved.aprof.json", "and the capture showing stays");
    click(scene, first->profilerOverlay().closeCaptureRect());
    frame();
    expect(!profiler::showing_capture() && !profiler::paused(), "Close capture returns to live");
    key(jadefx::Key::F6, jadefx::Key::ModControl);
    fs::remove_all(folder);
    profiler::reset_for_testing();
    return gFailures;
}

// The player's view has no menu bar, so it takes Cmd+F6 and Cmd+P itself.
int RunProfilerPlayerKeyTests() {
    gFailures = 0;
    profiler::reset_for_testing();
    runner::Runner player;
    player.prepare();
    auto view = jadefx::make<runner::GameView>(player, "Game");
    view->setPlayerView(true);
    runner::GameView* shown = view.get();
    auto scene = jadefx::make<jadefx::Scene>(view, 800, 600);
    scene->layout(800, 600, 0.1);
    runner::ProfilerUi& ui = runner::ProfilerUi::get();
    // The studio's views from the tests before are still alive in this process; a game has only its own.
    ui.owner = nullptr;
    expect(!ui.shown(), "a game starts with the profiler hidden");
    scene->noteKey(jadefx::Key::F6, true, false, jadefx::Key::ModControl);
    scene->noteKey(jadefx::Key::F6, false, false, 0);
    scene->layout(800, 600, 0.2);
    expect(ui.shown() && shown->profilerOverlay().isVisible(), "Cmd+F6 shows it over the game");
    scene->noteKey(jadefx::Key::P, true, false, jadefx::Key::ModControl);
    scene->noteKey(jadefx::Key::P, false, false, 0);
    expect(profiler::paused(), "Cmd+P pauses it");
    scene->noteKey(jadefx::Key::P, true, false, jadefx::Key::ModControl);
    scene->noteKey(jadefx::Key::P, false, false, 0);
    scene->noteKey(jadefx::Key::F6, true, false, jadefx::Key::ModControl);
    scene->noteKey(jadefx::Key::F6, false, false, 0);
    expect(!ui.shown() && !profiler::paused(), "and Cmd+F6 hides it again");
    profiler::reset_for_testing();
    return gFailures;
}

namespace {

double linear(float channel) {
    return channel <= 0.04045f ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
}

double luminance(const jadefx::Color& color) {
    return 0.2126 * linear(color.r) + 0.7152 * linear(color.g) + 0.0722 * linear(color.b);
}

double contrast(const jadefx::Color& a, const jadefx::Color& b) {
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

}  // namespace

// Every block's label reads: bright fills with dark text, and anything dark,
// such as a block dimmed behind a highlighted scope, with light text.
int RunProfilerColorTests() {
    gFailures = 0;
    using profiler::Group;
    for (Group group : {Group::Engine, Group::Physics, Group::Render, Group::Script, Group::User, Group::Gpu}) {
        const jadefx::Color fill = runner::ProfilerOverlay::blockColor(group, false);
        expect(fill.a == 1.f, "a block is drawn opaque");
        expect(contrast(fill, runner::ProfilerOverlay::labelColor(fill)) >= 7.0,
               "a block's label has at least 7:1 contrast");
        expect(luminance(fill) >= 0.35, "and the block itself is bright");
        const jadefx::Color dim = runner::ProfilerOverlay::blockColor(group, true);
        expect(contrast(dim, runner::ProfilerOverlay::labelColor(dim)) >= 4.5,
               "a dimmed block's label still has 4.5:1 contrast");
    }
    const jadefx::Color dark = jadefx::Color::rgb8(40, 40, 48);
    expect(luminance(runner::ProfilerOverlay::labelColor(dark)) > 0.8, "a dark fill gets light text");
    return gFailures;
}
