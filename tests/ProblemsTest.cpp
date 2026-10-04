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
    void frame() {
        static double t = 0;
        t += 0.1;
        scene->layout(600, 400, t);
    }
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

    // A destroyed script's row goes, and opening an old row does nothing harmful.
    const jadefx::TreeItem* stale = h.pane->tree().getRoot()->getChildren().empty()
                                        ? nullptr
                                        : h.pane->tree().getRoot()->getChildren().items()[0].get();
    game.destroy(warned.id());
    settle(engine);
    h.frame();
    bool listed = false;
    for (const ide::ScriptProblems& script : h.pane->list().scripts) {
        listed = listed || script.id == warned.id();
    }
    expect(!listed, "a destroyed script's row goes");
    const std::size_t opened_before_stale = h.opened.size();
    expect(!h.pane->openRow(stale), "opening a destroyed row's stale pointer does nothing");
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
    engine.analysis().set_enabled(true);
    settle(engine);
    h.frame();
    expect(!h.pane->offNoticeShown(), "and on again hides it");

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

    game.destroy(script.id());
    settle(engine);
    return gFailures;
}
