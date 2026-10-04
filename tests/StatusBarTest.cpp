#include "ide/IdeLayout.hpp"
#include "ide/IdeProblems.hpp"
#include "ide/ZoomPopover.hpp"
#include "runner/ProfilerOverlay.hpp"

#include "ChangeHistoryService.hpp"
#include "DataModel.hpp"
#include "Engine.hpp"
#include "GameObject.hpp"
#include "Script.hpp"
#include "ScriptAnalysis.hpp"

#include "jadefx/jadefx.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>

namespace {

int gFailures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

std::string Text(jadefx::Scene& scene, const char* id) {
    const auto* label = dynamic_cast<const jadefx::Label*>(scene.getElementById(id));
    return label != nullptr ? label->getText() : std::string("<missing>");
}

bool Shown(jadefx::Scene& scene, const char* id) {
    const jadefx::Node* node = scene.getElementById(id);
    return node != nullptr && node->isVisible();
}

}  // namespace

// The status bar's play state, save state, caret, frame time, zoom, and AI
// client chips, on the studio's own layout.
int RunStatusBarTests(ide::IdeLayout& layout, jadefx::Scene& scene) {
    gFailures = 0;
    double time = 60.0;
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
    frame();

    // Play state follows Test and Stop.
    expect(Text(scene, "play-state-text") == "Editing", "the studio starts editing");
    key(jadefx::Key::F5, 0);
    expect(Text(scene, "play-state-text") == "Playing", "F5 shows Playing");
    key(jadefx::Key::F5, jadefx::Key::ModShift);
    expect(Text(scene, "play-state-text") == "Editing", "Shift+F5 shows Editing again");

    // Save state follows the undo history.
    engine_core::Engine& engine = layout.simulation();
    engine_core::DataModel& game = engine.datamodel();
    expect(Shown(scene, "save-state"), "the save state is always shown");
    const bool was_unsaved = layout.has_unsaved_changes();
    expect(Text(scene, "save-state-text") == (was_unsaved ? "Unsaved" : "Saved"), "it says whether there is work to save");
    engine_core::InstanceId part = 0;
    engine.on_simulation([&part](engine_core::DataModel& world) {
        const std::optional<std::string> step = world.history().try_begin_recording("Add part");
        engine_core::GameObject& made = world.create<engine_core::GameObject>();
        world.set_name(made.id(), "StatusBarPart");
        world.set_parent(made.id(), world.scene_service("Workspace"));
        part = made.id();
        if (step) {
            world.history().finish_recording(*step, engine_core::FinishRecordingOperation::Commit);
        }
    });
    frame();
    expect(Text(scene, "save-state-text") == "Unsaved", "an edit shows Unsaved");

    // The caret shows only while a code editor has the keyboard.
    expect(!Shown(scene, "cursor-position"), "no caret position without a code editor focused");
    engine_core::Script& script = game.create<engine_core::Script>();
    game.set_name(script.id(), "StatusBarScript");
    script.set_source("local a = 1\nnope()\n");
    game.set_parent(script.id(), game.scene_service("Workspace"));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!engine.analysis().idle() && std::chrono::steady_clock::now() < deadline) {
        engine.analysis().pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    engine.analysis().pump();
    layout.show_problems();
    frame();
    auto* problems = dynamic_cast<ide::IdeProblems*>(layout.problemsPaneForTests());
    jadefx::TreeItem* row = nullptr;
    if (problems != nullptr) {
        problems->refresh();
        // Rows are in the list's order. Other tests may have left scripts with problems.
        const auto& scripts = problems->list().scripts;
        const auto& rows = problems->tree().getRoot()->getChildren().items();
        for (std::size_t i = 0; i < scripts.size() && i < rows.size(); ++i) {
            if (scripts[i].id == script.id() && !rows[i]->getChildren().empty()) {
                row = rows[i]->getChildren().items()[0].get();
            }
        }
    }
    expect(problems != nullptr && problems->openRow(row), "the script's problem opens its editor");
    frame();
    frame();
    jadefx::CodeArea* area = nullptr;
    for (jadefx::Node* node = scene.focusedNode(); node != nullptr && area == nullptr; node = node->getParent()) {
        area = dynamic_cast<jadefx::CodeArea*>(node);
    }
    expect(area != nullptr, "the opened editor has the keyboard");
    if (area != nullptr) {
        expect(Shown(scene, "cursor-position"), "the caret position shows while it does");
        expect(Text(scene, "cursor-position-text") == "Ln 2, Col 5 (4 selected)",
               "at the problem, a 1-based line and column, with what is selected");
        area->selectRange(0, 0);
        frame();
        expect(Text(scene, "cursor-position-text") == "Ln 1, Col 1", "and follows the caret");
        scene.releaseFocus(area);
        frame();
        expect(!Shown(scene, "cursor-position"), "and goes when the editor lets go of the keyboard");
    }

    // A test does not paint the Scene View, so there is no frame time to show.
    expect(Text(scene, "frame-time-text") == "-- ms", "no frame time without paints");

    // Clicking the frame time shows the profiler, and clicking again hides it.
    auto click = [&](const char* id) {
        const jadefx::Node* node = scene.getElementById(id);
        if (node == nullptr) {
            return;
        }
        const double x = node->getAbsoluteX() + node->getWidth() * 0.5;
        const double y = node->getAbsoluteY() + node->getHeight() * 0.5;
        scene.noteButton(0, true, x, y, 0);
        scene.noteButton(0, false, x, y, 0);
        frame();
    };
    runner::ProfilerUi& profiler = runner::ProfilerUi::get();
    const bool profiler_was = profiler.shown();
    profiler.setShown(false);
    click("frame-time");
    expect(profiler.shown(), "clicking the frame time shows the profiler");
    click("frame-time");
    expect(!profiler.shown(), "clicking it again hides the profiler");
    profiler.setShown(profiler_was);

    // Zoom follows the View menu's shortcuts.
    expect(Text(scene, "zoom-level-text") == "100%", "the studio starts at actual size");
    key(jadefx::Key::Equal, jadefx::Key::ModControl);
    expect(Text(scene, "zoom-level-text") == "110%", "zooming in shows 110%");
    key(jadefx::Key::Digit0, jadefx::Key::ModControl);
    expect(Text(scene, "zoom-level-text") == "100%", "actual size shows 100%");

    // Clicking the zoom opens a slider over it, which zooms as it moves.
    click("zoom-level");
    auto* popover = dynamic_cast<ide::ZoomPopover*>(scene.getElementById("zoom-popover"));
    expect(popover != nullptr && popover->showing(), "clicking the zoom opens its slider");
    if (popover != nullptr) {
        const jadefx::Node* chip = scene.getElementById("zoom-level");
        expect(chip != nullptr && popover->getAbsoluteY() + popover->getHeight() <= chip->getAbsoluteY() + 0.5,
               "above the zoom, since the status bar is at the bottom");
        expect(popover->slider().getValue() == 1.0 && Text(scene, "zoom-popover-value") == "100%",
               "showing the zoom now");
        expect(scene.focusedNode() == &popover->slider(), "with the slider taking the keyboard");
        key(jadefx::Key::Right, 0);
        expect(std::abs(jadefx::Stage::getZoom() - 1.1) < 1e-9, "Right on the slider zooms in a step");
        expect(Text(scene, "zoom-level-text") == "110%" && Text(scene, "zoom-popover-value") == "110%",
               "the chip and the slider both show it");
        expect(scene.getElementsByClassName("toast").empty() ||
                   dynamic_cast<jadefx::Label*>(scene.getElementsByClassName("toast").back())->getText() != "Zoom 110%",
               "without a toast saying what the slider shows");
        popover->slider().adjustValue(1.54);
        frame();
        expect(std::abs(jadefx::Stage::getZoom() - 1.5) < 1e-9, "the slider snaps to a 10% step");
        key(jadefx::Key::Equal, jadefx::Key::ModControl);
        expect(std::abs(popover->slider().getValue() - 1.6) < 1e-9 && Text(scene, "zoom-popover-value") == "160%",
               "a shortcut that zooms moves the slider too");
        click("zoom-reset");
        expect(jadefx::Stage::getZoom() == 1.0 && Text(scene, "zoom-level-text") == "100%",
               "Reset goes back to 100%");
        expect(popover->showing(), "and leaves the slider open");
        expect(chip != nullptr && popover->getAbsoluteY() + popover->getHeight() <= chip->getAbsoluteY() + 0.5,
               "still against the zoom after it moved");
        click("zoom-level");
        expect(!popover->showing(), "a second click on the zoom closes it");
        click("zoom-level");
        expect(popover->showing(), "and another opens it again");
        key(jadefx::Key::Escape, 0);
        expect(!popover->showing(), "Escape closes it");
    }

    // The mouse, as a window delivers it: the Stage divides window points by
    // the zoom and lays the scene out at the window's size over the zoom, so a
    // new zoom moves everything under a pointer that has not moved.
    if (popover != nullptr) {
        constexpr double kWindowWidth = 1280;
        constexpr double kWindowHeight = 800;
        auto window_frame = [&] {
            const double zoom = jadefx::Stage::getZoom();
            scene.layout(kWindowWidth / zoom, kWindowHeight / zoom, time);
            layout.flushFrame();
            time += 0.05;
        };
        auto window_button = [&](bool down, double x, double y) {
            const double zoom = jadefx::Stage::getZoom();
            scene.noteButton(0, down, x / zoom, y / zoom, 0);
            window_frame();
        };
        auto window_move = [&](double x, double y) {
            const double zoom = jadefx::Stage::getZoom();
            scene.noteMove(x / zoom, y / zoom);
            window_frame();
        };
        // A node's point, as fractions of its box, in window points at the zoom now.
        auto window_point = [](const jadefx::Node& node, double fx, double fy) {
            const double zoom = jadefx::Stage::getZoom();
            return std::make_pair((node.getAbsoluteX() + node.getWidth() * fx) * zoom,
                                  (node.getAbsoluteY() + node.getHeight() * fy) * zoom);
        };
        // The window x of a slider value: the thumb's 16-point travel is the box less 16.
        auto window_x_of = [](const jadefx::Slider& slider, double value) {
            const double fraction = (value - slider.getMin()) / (slider.getMax() - slider.getMin());
            return (slider.getAbsoluteX() + 8 + fraction * (slider.getWidth() - 16)) * jadefx::Stage::getZoom();
        };
        window_frame();
        const jadefx::Node* chip = scene.getElementById("zoom-level");
        if (chip != nullptr) {
            const auto [cx, cy] = window_point(*chip, 0.5, 0.5);
            window_button(true, cx, cy);
            window_button(false, cx, cy);
        }
        expect(popover->showing(), "a click on the zoom opens the slider");
        jadefx::Slider& slider = popover->slider();

        // A press on the track at 200% zooms there at once, and a hand that
        // wobbles a pixel either way while holding keeps it there.
        const double sy = window_point(slider, 0.5, 0.3).second;
        const double track_x = window_x_of(slider, 2.0);
        window_button(true, track_x, sy);
        expect(std::abs(jadefx::Stage::getZoom() - 2.0) < 1e-9, "a press on the track zooms to that point at once");
        bool steady = true;
        for (int i = 0; i < 8; ++i) {
            window_move(track_x + (i % 2 == 0 ? 1 : -1), sy);
            steady = steady && std::abs(jadefx::Stage::getZoom() - 2.0) < 1e-9;
        }
        expect(steady, "the zoom holds while the pointer wobbles, though each zoom moved the slider");
        window_button(false, track_x, sy);
        expect(std::abs(jadefx::Stage::getZoom() - 2.0) < 1e-9, "and letting go leaves it there");
        expect(Text(scene, "zoom-level-text") == "200%" && Text(scene, "zoom-popover-value") == "200%",
               "the chip and the slider say so");
        expect(popover->showing(), "the slider stays open after a zoom");

        // The thumb, dragged in small steps, zooms at each step it passes, and
        // a pointer brought back to where it pressed brings the zoom back.
        const double thumb_y = window_point(slider, 0.5, 0.3).second;
        const double thumb_x = window_x_of(slider, 2.0);
        const double step = (slider.getWidth() - 16) * jadefx::Stage::getZoom() / 25.0;
        window_button(true, thumb_x, thumb_y);
        window_move(thumb_x + step, thumb_y);
        expect(std::abs(jadefx::Stage::getZoom() - 2.1) < 1e-9, "a step's drag zooms a step while still held");
        window_move(thumb_x + step * 3, thumb_y);
        expect(std::abs(jadefx::Stage::getZoom() - 2.3) < 1e-9, "and three steps' drag three steps");
        window_move(thumb_x - step * 5, thumb_y);
        expect(std::abs(jadefx::Stage::getZoom() - 1.5) < 1e-9, "back past the press, it zooms out as far");
        window_move(thumb_x, thumb_y);
        expect(std::abs(jadefx::Stage::getZoom() - 2.0) < 1e-9, "and at the press point it is where it began");
        window_move(thumb_x + step * 2, thumb_y);
        window_button(false, thumb_x + step * 2, thumb_y);
        expect(std::abs(jadefx::Stage::getZoom() - 2.2) < 1e-9, "letting go two steps right leaves 220%");
        expect(Text(scene, "zoom-popover-value") == "220%" && Text(scene, "zoom-level-text") == "220%",
               "the slider and the chip agree");
        const jadefx::Node* chip_now = scene.getElementById("zoom-level");
        expect(chip_now != nullptr && popover->getAbsoluteY() + popover->getHeight() <= chip_now->getAbsoluteY() + 0.5 &&
                   popover->getAbsoluteX() + popover->getWidth() <= scene.getWidth() + 0.5,
               "the slider sits over its chip, inside the window, at the new zoom");

        key(jadefx::Key::Escape, 0);
        if (jadefx::Stage::getZoom() != 1.0) {
            key(jadefx::Key::Digit0, jadefx::Key::ModControl);
        }
        scene.layout(kWindowWidth, kWindowHeight, time);
        expect(jadefx::Stage::getZoom() == 1.0, "the zoom goes back to 100% for the tests after");
    }

    // The AI chip says the server is off when it is.
    if (layout.mcp_status() == "Off") {
        expect(Text(scene, "ai-client-text") == "AI off", "the AI chip says the server is off");
    } else {
        expect(Text(scene, "ai-client-text") != "AI off", "the AI chip says the server is on");
    }

    game.destroy(script.id());
    engine.on_simulation([part](engine_core::DataModel& world) { world.destroy(part); });
    return gFailures;
}
