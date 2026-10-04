#include "ide/IdeLayout.hpp"
#include "ide/IdeProblems.hpp"
#include "runner/ProfilerOverlay.hpp"

#include "ChangeHistoryService.hpp"
#include "DataModel.hpp"
#include "Engine.hpp"
#include "GameObject.hpp"
#include "Script.hpp"
#include "ScriptAnalysis.hpp"

#include "jadefx/jadefx.hpp"

#include <chrono>
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
