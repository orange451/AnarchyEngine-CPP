#include "ide/IdeProblems.hpp"

#include "ide/IdeDock.hpp"
#include "ide/IdeLayout.hpp"
#include "ide/IdeScriptEditor.hpp"

#include "DataModel.hpp"
#include "Engine.hpp"
#include "Folder.hpp"
#include "ModuleScript.hpp"
#include "Script.hpp"
#include "ScriptAnalysis.hpp"

#include "jadefx/jadefx.hpp"

#include <cmath>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

int gFailures = 0;

void expect(bool condition, const char* label) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", label);
        ++gFailures;
    }
}

// Pumps analysis until it has nothing left to publish, as the studio's frame does.
void settle(engine_core::Engine& engine) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!engine.analysis().idle() && std::chrono::steady_clock::now() < deadline) {
        engine.analysis().pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    engine.analysis().pump();
}

engine_core::Script& add_script(engine_core::DataModel& game, engine_core::InstanceId parent, const char* name,
                                const char* source) {
    engine_core::Script& script = game.create<engine_core::Script>();
    game.set_name(script.id(), name);
    script.set_source(source);
    game.set_parent(script.id(), parent);
    return script;
}

// A scene of its own with the pane in it, so its layout pass runs.
struct Harness {
    std::shared_ptr<jadefx::Scene> scene;
    std::shared_ptr<ide::IdeProblems> pane;
    std::vector<std::tuple<std::uint32_t, int, int, int>> opened;

    explicit Harness(engine_core::Engine& engine) {
        ide::ProblemsHost host;
        host.open = [this](std::uint32_t id, int line, int column, int column_end) {
            opened.emplace_back(id, line, column, column_end);
        };
        pane = jadefx::make<ide::IdeProblems>(engine, std::move(host));
        scene = jadefx::make<jadefx::Scene>(nullptr, 600, 400);
        scene->setRoot(pane);
        frame();
    }
    // The scene's clock, shared by every harness so it only goes forward.
    static double& clock() {
        static double t = 0;
        return t;
    }
    void frame() { frame_at(clock() + 0.1); }
    void frame_at(double at) {
        clock() = at;
        scene->layout(600, 400, at);
    }
    void key(int code) { scene->noteKey(code, true, false, 0); }
};

}  // namespace

int RunProblemsPaneTests(engine_core::Engine& engine) {
    gFailures = 0;
    engine_core::DataModel& game = engine.datamodel();
    const engine_core::InstanceId workspace = game.scene_service("Workspace");
    engine_core::Folder& logic = game.create<engine_core::Folder>();
    game.set_name(logic.id(), "Logic");
    game.set_parent(logic.id(), workspace);
    engine_core::Script& broken = add_script(game, logic.id(), "Broken", "--!strict\nlocal --[[é]]x: number = \"x\"\n");
    engine_core::Script& warned = add_script(game, workspace, "Warned", "wiat(1)\n");
    engine_core::Script& clean = add_script(game, workspace, "Clean", "print(1)\n");
    settle(engine);

    Harness h(engine);
    h.pane->refresh();
    const ide::ProblemList& list = h.pane->list();
    expect(list.scripts.size() == 2, "the two scripts with problems are listed, the clean one is not");
    expect(list.scripts.size() == 2 && list.scripts[0].id == broken.id(), "the script with an error comes first");
    expect(list.scripts.size() == 2 && list.scripts[0].path == "Workspace.Logic", "its path leaves out game");
    expect(h.pane->title() == "Problems (2)", "the tab counts the error and the warning");
    expect(h.pane->summary() == "1 error, 1 warning in 2 scripts", "the summary says the same");
    expect(h.pane->tree().getRoot()->getChildren().size() == 2, "a tree row per script");

    // Renaming or moving a folder no script refers to rechecks nothing, so no
    // diagnostics change: the tree revision alone brings the paths up to date.
    game.set_name(logic.id(), "Gameplay");
    settle(engine);
    h.frame();
    expect(!h.pane->list().scripts.empty() && h.pane->list().scripts[0].path == "Workspace.Gameplay",
           "renaming a folder renames the path of the scripts under it");
    engine_core::Folder& holder = game.create<engine_core::Folder>();
    game.set_name(holder.id(), "Holder");
    game.set_parent(holder.id(), workspace);
    game.set_parent(logic.id(), holder.id());
    settle(engine);
    h.frame();
    expect(!h.pane->list().scripts.empty() && h.pane->list().scripts[0].path == "Workspace.Holder.Gameplay",
           "moving a folder moves the path of the scripts under it");
    game.set_parent(logic.id(), workspace);
    game.set_name(logic.id(), "Logic");
    game.destroy(holder.id());
    settle(engine);
    h.frame();
    expect(!h.pane->list().scripts.empty() && h.pane->list().scripts[0].path == "Workspace.Logic",
           "and moving it back and renaming it back restores the path");

    // A pane no scene lays out, as a tab that is not showing: its tick keeps the title counting.
    {
        auto loose = jadefx::make<ide::IdeProblems>(engine, ide::ProblemsHost{});
        loose->tick(Harness::clock());
        expect(loose->title() == "Problems (2)", "a pane that is never laid out still titles itself on tick");
        engine_core::Script& more = add_script(game, workspace, "MoreBroken", "nope()\n");
        settle(engine);
        loose->tick(Harness::clock() + 1);
        expect(loose->title() == "Problems (3)", "and keeps counting as scripts break");
        const int ticked = loose->rebuilds();
        loose->tick(Harness::clock() + 1);
        expect(loose->rebuilds() == ticked, "a second tick in the same frame does nothing");
        game.destroy(more.id());
        settle(engine);
        h.frame();
    }

    // Keys from the filter: Down walks into the list, Enter opens the first
    // problem, Escape clears the filter.
    h.pane->filterInput().focusAll();
    h.frame();
    h.key(jadefx::Key::Down);
    expect(h.pane->tree().isFocused(), "Down from the filter focuses the list");
    expect(h.pane->tree().getSelectedItem() != nullptr &&
               h.pane->tree().getSelectedItem() == h.pane->tree().getRoot()->getChildren().items()[0].get(),
           "and selects its first row");
    h.pane->filterInput().focusAll();
    h.pane->filterInput().field().setText("wiat");
    h.opened.clear();
    h.key(jadefx::Key::Enter);
    expect(h.opened.size() == 1 && std::get<0>(h.opened[0]) == warned.id() && std::get<1>(h.opened[0]) == 1,
           "Enter in the filter opens the first problem it shows");
    h.opened.clear();
    h.key(jadefx::Key::Escape);
    expect(h.pane->filterInput().text().empty(), "Escape clears the filter");
    h.frame();
    expect(h.pane->list().scripts.size() == 2, "and the list shows everything again");
    h.key(jadefx::Key::Escape);
    expect(h.pane->filterInput().text().empty() && h.opened.empty(), "Escape on an empty filter is harmless");
    h.frame();

    // The toggles are as wide as their word and count.
    {
        ide::FindButton& toggle = h.pane->warningsToggle();
        toggle.setText("Warnings (123)");
        h.frame();
        const jadefx::ComputedStyle& style = toggle.computedStyle();
        const double text = jadefx::Font(style.fontFamily, style.fontSize).measureWidth("Warnings (123)");
        expect(toggle.getPrefWidth() >= text + style.padding.width() + style.border.width(),
               "a toggle with a 3-digit count is wide enough for its text");
        expect(toggle.getWidth() >= text, "and is laid out that wide");
    }

    // A script under Core is the studio's own tooling, not the place's: never listed.
    engine_core::Script& core_script = add_script(game, game.core(), "CoreBroken", "nope()\n");
    settle(engine);
    h.frame();
    expect(!engine.analysis().diagnostics(core_script.id()).empty(),
           "the checker does check a Core script, so excluding it is the pane's choice");
    bool core_listed = false;
    for (const ide::ScriptProblems& script : h.pane->list().scripts) {
        core_listed = core_listed || script.id == core_script.id();
    }
    expect(!core_listed, "a script under Core is not listed");

    // Opening the error: line 2, at the "x" string literal.
    const auto& rows = h.pane->tree().getRoot()->getChildren();
    if (!rows.empty() && !rows.items()[0]->getChildren().empty()) {
        h.pane->openRow(rows.items()[0]->getChildren().items()[0].get());
    }
    expect(h.opened.size() == 1 && std::get<0>(h.opened[0]) == broken.id() && std::get<1>(h.opened[0]) == 2,
           "opening a problem opens its script at its line");
    // The block comment holding é is 2 bytes but 1 code point, so a byte
    // column here would be 26, not 25: proof the column is in code points.
    expect(h.opened.size() == 1 && std::get<2>(h.opened[0]) == 25, "at its code-point column");

    // A script row opens the script at its first problem.
    h.opened.clear();
    if (rows.size() == 2) {
        h.pane->openRow(rows.items()[1].get());
    }
    expect(h.opened.size() == 1 && std::get<0>(h.opened[0]) == warned.id() && std::get<1>(h.opened[0]) == 1,
           "opening a script row opens its first problem");

    // Toggles and filter.
    h.pane->warningsToggle().setChecked(false);
    h.frame();
    expect(h.pane->list().scripts.size() == 1, "Warnings off hides the warned script");
    h.pane->warningsToggle().setChecked(true);
    h.pane->filterInput().field().setText("wiat");
    h.frame();
    expect(h.pane->list().scripts.size() == 1 && h.pane->list().scripts[0].id == warned.id(), "the filter narrows");
    h.pane->filterInput().field().setText("");
    h.frame();
    const int after_filter = h.pane->rebuilds();
    h.frame();
    expect(h.pane->rebuilds() == after_filter, "a quiet frame after toggling the filter does not rebuild");

    // Collapsing survives a rebuild.
    if (!h.pane->tree().getRoot()->getChildren().empty()) {
        h.pane->tree().getRoot()->getChildren().items()[0]->setExpanded(false);
    }
    clean.set_source("print(2)\n");
    settle(engine);
    h.frame();
    expect(!h.pane->tree().getRoot()->getChildren().empty() &&
               !h.pane->tree().getRoot()->getChildren().items()[0]->isExpanded(),
           "a collapsed script stays collapsed after a rebuild");

    // Fixing the error updates the pane by itself.
    broken.set_source("--!strict\nlocal --[[é]]x: number = 1\n");
    settle(engine);
    h.frame();
    expect(h.pane->list().scripts.size() == 1 && h.pane->title() == "Problems (1)", "a fixed error leaves the list");

    // Many changes, one rebuild per layout pass.
    const int before = h.pane->rebuilds();
    for (int i = 0; i < 20; ++i) {
        add_script(game, workspace, ("Many" + std::to_string(i)).c_str(), "nope()\n");
    }
    settle(engine);
    h.frame();
    expect(h.pane->rebuilds() == before + 1, "many changes, one rebuild");
    const int after_many = h.pane->rebuilds();
    h.frame();
    expect(h.pane->rebuilds() == after_many, "a quiet frame does not rebuild");

    // During a long check batch the rows follow at most every 250 ms of scene
    // time. Each frame here renames a folder, so a tree change waits every
    // time, and edits a script, so the checker stays busy.
    {
        engine_core::Folder& churn = game.create<engine_core::Folder>();
        game.set_name(churn.id(), "Churn");
        game.set_parent(churn.id(), workspace);
        settle(engine);
        const double start = Harness::clock() + 1;
        h.frame_at(start);
        const int base = h.pane->rebuilds();
        int busy_frames = 0;
        for (int i = 0; i < 10; ++i) {
            clean.set_source("print(" + std::to_string(i + 10) + ")\n");
            game.set_name(churn.id(), "Churn" + std::to_string(i));
            engine.analysis().pump();
            busy_frames += engine.analysis().busy() ? 1 : 0;
            // 0.30 to 0.48 s after the last rebuild: all within one 250 ms span.
            h.frame_at(start + 0.30 + 0.02 * i);
        }
        expect(busy_frames == 10, "the checker is busy for every frame of the batch");
        expect(h.pane->rebuilds() == base + 1, "frames within 250 ms of a busy batch rebuild once");
        clean.set_source("print(30)\n");
        game.set_name(churn.id(), "ChurnLate");
        engine.analysis().pump();
        const bool still_busy = engine.analysis().busy();
        h.frame_at(start + 0.30 + 0.26);
        expect(!still_busy || h.pane->rebuilds() == base + 2, "250 ms on, a busy batch rebuilds again");
        settle(engine);
        const int before_idle = h.pane->rebuilds();
        h.frame_at(start + 0.30 + 0.27);
        expect(h.pane->rebuilds() == before_idle + 1, "and once more when it goes idle");
        game.destroy(churn.id());
        settle(engine);
        h.frame();
    }

    // A destroyed script's row goes, and opening an old row does nothing harmful.
    // Held, so the old row outlives the rebuild and its address is not reused.
    const std::shared_ptr<jadefx::TreeItem> stale = h.pane->tree().getRoot()->getChildren().empty()
                                                        ? nullptr
                                                        : h.pane->tree().getRoot()->getChildren().items()[0];
    game.destroy(warned.id());
    settle(engine);
    h.frame();
    bool listed = false;
    for (const ide::ScriptProblems& script : h.pane->list().scripts) {
        listed = listed || script.id == warned.id();
    }
    expect(!listed, "a destroyed script's row goes");
    const std::size_t opened_before_stale = h.opened.size();
    expect(stale != nullptr && !h.pane->openRow(stale.get()), "opening a row from before the rebuild does nothing");
    expect(h.opened.size() == opened_before_stale, "and does not call open");

    // Play: checking stops and the note shows; the list keeps its rows.
    const std::size_t shown = h.pane->list().scripts.size();
    game.start_simulation();
    h.frame();
    expect(h.pane->playNoteShown(), "the play note shows during a playtest");
    expect(h.pane->list().scripts.size() == shown, "and the last results stay");
    game.stop_simulation();
    settle(engine);
    h.frame();
    expect(!h.pane->playNoteShown(), "the note goes after Stop");

    // Analysis off.
    engine.analysis().set_enabled(false);
    h.frame();
    expect(h.pane->offNoticeShown(), "analysis off shows the notice");
    expect(!h.pane->tree().isVisible(), "and hides the tree");
    expect(h.pane->list().scripts.empty(), "and the list is empty");
    expect(!h.pane->headerShown(), "and hides the filter and toggles");
    expect(h.pane->summary().empty(), "and does not say No problems above the notice");
    engine.analysis().set_enabled(true);
    settle(engine);
    h.frame();
    expect(!h.pane->offNoticeShown(), "and on again hides it");
    expect(h.pane->headerShown() && !h.pane->summary().empty(), "and brings back the header and the summary");

    // Clean up the scripts this test added.
    for (const ide::ScriptProblems& script : h.pane->list().scripts) {
        game.destroy(script.id);
    }
    game.destroy(clean.id());
    game.destroy(logic.id());
    game.destroy(core_script.id());
    settle(engine);
    return gFailures;
}

int RunProblemsWindowTests(ide::IdeLayout& layout, jadefx::Scene& scene) {
    gFailures = 0;
    expect(scene.getElementsByClassName("problems-pane").empty(), "Problems starts closed");
    layout.show_problems();
    scene.layout(1280, 800, 50.0);
    const std::vector<jadefx::Node*> panes = scene.getElementsByClassName("problems-pane");
    expect(panes.size() == 1, "show_problems opens one Problems pane");
    auto* pane = panes.empty() ? nullptr : dynamic_cast<ide::IdeProblems*>(panes.front());
    expect(pane != nullptr && pane->name() == "Problems", "named Problems for the Window menu and layout.json");

    // It opens a script at the problem's line.
    engine_core::Engine& engine = layout.simulation();
    engine_core::DataModel& game = engine.datamodel();
    engine_core::Script& script =
        add_script(game, game.scene_service("Workspace"), "Opened", "local a = 1\nwiat(1)\n");
    settle(engine);
    scene.layout(1280, 800, 50.1);
    if (pane != nullptr) {
        pane->refresh();
        const auto& rows = pane->tree().getRoot()->getChildren();
        jadefx::TreeItem* row = nullptr;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (!rows.items()[i]->getChildren().empty()) {
                row = rows.items()[i]->getChildren().items()[0].get();
            }
        }
        expect(pane->openRow(row), "a problem row opens");
    }
    scene.layout(1280, 800, 50.2);
    // IdeScriptEditor keeps its IdePane name as "Script.lua" (the constructor's
    // fixed name()); it titles its tab with the script's name instead, as
    // "Opened.lua" (IdeScriptEditor::setTitleText). So an opened script is
    // found by instanceId(), not by name().
    ide::IdeScriptEditor* opened_editor = nullptr;
    for (jadefx::Node* node : scene.getElementsByClassName("ide-pane")) {
        if (auto* editor = dynamic_cast<ide::IdeScriptEditor*>(node);
            editor != nullptr && editor->instanceId() == script.id()) {
            opened_editor = editor;
        }
    }
    expect(opened_editor != nullptr, "opening a problem opens its script in an editor");

    // The editor docks in the Scene View's own tab strip and comes to the
    // front, the way Search's result does: close it, as every other test that
    // opens an editor does, so the Scene View's tab is selected (and so
    // mounted) again for tests that run after this one.
    if (opened_editor != nullptr) {
        ide::IdeDock* dock = nullptr;
        for (jadefx::Node* cursor = opened_editor; cursor != nullptr && dock == nullptr;
             cursor = cursor->getParent()) {
            dock = dynamic_cast<ide::IdeDock*>(cursor);
        }
        if (dock != nullptr) {
            std::shared_ptr<jadefx::Tab> found;
            for (const std::shared_ptr<jadefx::Tab>& tab : dock->tabs()->getTabs().items()) {
                if (tab && tab->getContent() == opened_editor) {
                    found = tab;
                }
            }
            if (found) {
                dock->tabs()->close(found);
            }
        }
        scene.layout(1280, 800, 50.21);
    }

    // Renaming a script renames its row.
    game.set_name(script.id(), "Renamed");
    settle(engine);
    scene.layout(1280, 800, 50.3);
    bool renamed = false;
    if (pane != nullptr) {
        for (const ide::ScriptProblems& listed : pane->list().scripts) {
            renamed = renamed || (listed.id == script.id() && listed.name == "Renamed");
        }
    }
    expect(renamed, "renaming a script renames its row");

    // Its default home is a tab beside the Console. With the Console's tab in
    // front, the Problems page is not laid out, and the studio's per-frame
    // hook keeps its title counting.
    ide::IdeDock* dock = nullptr;
    for (jadefx::Node* cursor = pane; cursor != nullptr && dock == nullptr; cursor = cursor->getParent()) {
        dock = dynamic_cast<ide::IdeDock*>(cursor);
    }
    std::shared_ptr<jadefx::Tab> problems_tab;
    std::shared_ptr<jadefx::Tab> other_tab;
    if (dock != nullptr) {
        for (const std::shared_ptr<jadefx::Tab>& tab : dock->tabs()->getTabs().items()) {
            if (tab && tab->getContent() == pane) {
                problems_tab = tab;
            } else if (tab && !other_tab) {
                other_tab = tab;
            }
        }
    }
    expect(problems_tab != nullptr && other_tab != nullptr, "Problems docks as a tab beside another");
    if (pane != nullptr && problems_tab != nullptr && other_tab != nullptr) {
        dock->tabs()->select(other_tab);
        scene.layout(1280, 800, 50.4);
        const std::string before = pane->title();
        engine_core::Script& hidden =
            add_script(game, game.scene_service("Workspace"), "HiddenTabBroken", "nope()\nnope()\n");
        settle(engine);
        scene.layout(1280, 800, 50.5);
        layout.flushFrame();
        expect(pane->title() != before && pane->title() == "Problems (" + std::to_string(pane->list().total.errors +
                                                                                     pane->list().total.warnings) +
                                                              ")",
               "an unselected Problems tab's title follows new problems");
        expect(pane->list().total.scripts >= 2, "the hidden pane's list has the new script");
        game.destroy(hidden.id());
        settle(engine);
        scene.layout(1280, 800, 50.6);
        layout.flushFrame();
        expect(pane->title() == before, "and drops it again when it goes");
        dock->tabs()->select(problems_tab);
        scene.layout(1280, 800, 50.7);
    }

    game.destroy(script.id());
    settle(engine);
    return gFailures;
}
