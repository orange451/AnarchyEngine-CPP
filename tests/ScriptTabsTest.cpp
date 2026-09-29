#include "ide/IdeDock.hpp"
#include "ide/IdeExplorer.hpp"
#include "ide/IdeLayout.hpp"
#include "ide/IdeScriptEditor.hpp"
#include "SelectionService.hpp"

#include "DataModel.hpp"
#include "Engine.hpp"
#include "LuaSource.hpp"
#include "Script.hpp"

#include "jadefx/jadefx.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

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

ide::IdeDock* DockOf(jadefx::Node* node) {
    for (; node != nullptr; node = node->getParent()) {
        if (auto* dock = dynamic_cast<ide::IdeDock*>(node)) {
            return dock;
        }
    }
    return nullptr;
}

std::shared_ptr<jadefx::Tab> TabOf(ide::IdeDock& dock, const jadefx::Node* pane) {
    for (const std::shared_ptr<jadefx::Tab>& tab : dock.tabs()->getTabs().items()) {
        if (tab && tab->getContent() == pane) {
            return tab;
        }
    }
    return nullptr;
}

ide::IdeScriptEditor* EditorFor(jadefx::Scene& scene, engine_core::InstanceId id) {
    for (jadefx::Node* area : scene.getElementsByClassName("ide-script")) {
        for (jadefx::Node* node = area; node != nullptr; node = node->getParent()) {
            auto* editor = dynamic_cast<ide::IdeScriptEditor*>(node);
            if (editor != nullptr && editor->instanceId() == id) {
                return editor;
            }
        }
    }
    return nullptr;
}

engine_core::InstanceId AddScript(engine_core::Engine& engine, const char* name, const char* source) {
    engine_core::InstanceId id = 0;
    engine.on_simulation([&](engine_core::DataModel& game) {
        engine_core::Script& script = game.create<engine_core::Script>();
        game.set_name(script.id(), name);
        script.set_source(source);
        game.set_parent(script.id(), game.id());
        id = script.id();
    });
    return id;
}

std::string SourceOf(engine_core::Engine& engine, engine_core::InstanceId id) {
    std::string out;
    engine.on_simulation([&](engine_core::DataModel& game) {
        if (auto* script = dynamic_cast<engine_core::LuaSource*>(game.instance(id))) {
            out = script->source();
        }
    });
    return out;
}

void SetSource(engine_core::Engine& engine, engine_core::InstanceId id, const char* source) {
    engine.on_simulation([&](engine_core::DataModel& game) {
        if (auto* script = dynamic_cast<engine_core::LuaSource*>(game.instance(id))) {
            script->set_source(source);
        }
    });
}

// A tab closed while stopped wrote its text already. Stop must not put that text
// back over a change made after the tab closed, such as Replace All or a file
// loaded from disk. A tab closed during a test is still written back at Stop.
void TestClosedTabs(ide::IdeLayout& layout, jadefx::Scene& scene) {
    engine_core::Engine& engine = layout.simulation();
    double time = 6.0;
    auto frame = [&] {
        scene.layout(1280, 800, time);
        time += 0.05;
    };
    auto key = [&](int code, int mods) {
        scene.noteKey(code, true, false, mods);
        scene.noteKey(code, false, false, 0);
        frame();
    };
    auto open = [&](engine_core::InstanceId id) -> ide::IdeScriptEditor* {
        engine.datamodel().selection().set({id});
        frame();
        frame();
        bool ran = false;
        for (jadefx::Node* node : scene.getElementsByClassName("explorer-pane")) {
            if (auto* explorer = dynamic_cast<ide::IdeExplorer*>(node); explorer != nullptr && !ran) {
                ran = explorer->run_on_selection(engine_core::InstanceAction::Edit);
            }
        }
        frame();
        frame();
        return ran ? EditorFor(scene, id) : nullptr;
    };
    auto close = [&](ide::IdeScriptEditor* editor) {
        ide::IdeDock* dock = DockOf(editor);
        const std::shared_ptr<jadefx::Tab> tab = dock != nullptr ? TabOf(*dock, editor) : nullptr;
        if (tab) {
            dock->tabs()->close(tab);
        }
        frame();
        return tab != nullptr;
    };

    const engine_core::InstanceId id = AddScript(engine, "KeptSource", "print('A')\n");
    ide::IdeScriptEditor* editor = open(id);
    Expect(editor != nullptr && editor->isLoaded(), "Edit opens the script in a tab");
    Expect(editor != nullptr && close(editor), "and its tab closes");
    SetSource(engine, id, "print('B')\n");
    key(jadefx::Key::F5, 0);
    key(jadefx::Key::F5, jadefx::Key::ModShift);
    ExpectText(SourceOf(engine, id), "print('B')\n", "Stop keeps a change made after the tab closed while stopped");

    editor = open(id);
    Expect(editor != nullptr, "the script opens again");
    key(jadefx::Key::F5, 0);
    std::string typed;
    if (editor != nullptr) {
        editor->focus();
        scene.noteText("-- typed ");
        frame();
        typed = editor->text();
        close(editor);
    }
    ExpectText(typed, "-- typed print('B')\n", "typing during a test reaches the buffer");
    key(jadefx::Key::F5, jadefx::Key::ModShift);
    ExpectText(SourceOf(engine, id), typed, "Stop writes back a tab closed during the test");
}

// The color picker's key hook belongs to the scene. An editor that leaves the
// scene with the picker open takes the hook with it.
void TestPickerHookLeavesWithEditor(engine_core::Engine& engine) {
    const engine_core::InstanceId id = AddScript(engine, "Tint", "local c = Color3.new(1, 0, 0)\n");
    auto editor = jadefx::make<ide::IdeScriptEditor>(engine, id);
    editor->setPrefWidthRatio(1);
    editor->setPrefHeightRatio(1);
    auto holder = jadefx::make<jadefx::StackPane>();
    holder->getChildren().add(editor);
    auto scene = jadefx::make<jadefx::Scene>(holder, 900, 600);
    double time = 0;
    auto frame = [&] {
        time += 0.1;
        scene->layout(900, 600, time);
    };
    auto open_picker = [&] {
        const std::vector<jadefx::Node*> swatches = editor->getElementsByClassName("script-swatch");
        if (swatches.empty()) {
            return false;
        }
        jadefx::Node* swatch = swatches.front();
        const double x = swatch->getAbsoluteX() + swatch->getWidth() * 0.5;
        const double y = swatch->getAbsoluteY() + swatch->getHeight() * 0.5;
        scene->noteButton(0, true, x, y);
        scene->noteButton(0, false, x, y);
        frame();
        return true;
    };
    auto escape = [&] {
        const bool taken = scene->noteKey(jadefx::Key::Escape, true, false, 0);
        scene->noteKey(jadefx::Key::Escape, false, false, 0);
        frame();
        return taken;
    };
    frame();
    frame();
    Expect(open_picker(), "a Color3 literal has a swatch");
    Expect(escape(), "with the picker open, the picker takes Escape");
    Expect(open_picker(), "the swatch opens the picker again");
    holder->getChildren().clear();
    frame();
    Expect(!escape(), "once the editor leaves the scene, the picker no longer takes Escape");
}

// Undo can bring a deleted script back with the same id. Its editor, read-only
// while the script was gone, takes it up again.
void TestEditorComesBackWithItsScript(engine_core::Engine& engine) {
    const engine_core::InstanceId id = AddScript(engine, "Phoenix", "print(1)\n");
    // Its creation is its own undo step, apart from the delete below.
    engine.on_simulation([](engine_core::DataModel& game) { game.history().end_gesture(); });
    auto editor = jadefx::make<ide::IdeScriptEditor>(engine, id);
    editor->setPrefWidthRatio(1);
    editor->setPrefHeightRatio(1);
    auto holder = jadefx::make<jadefx::StackPane>();
    holder->getChildren().add(editor);
    auto scene = jadefx::make<jadefx::Scene>(holder, 900, 600);
    double time = 0;
    auto frame = [&] {
        time += 0.1;
        scene->layout(900, 600, time);
    };
    auto area = [&]() -> jadefx::StyledTextArea* {
        for (jadefx::Node* node : editor->getElementsByClassName("ide-script")) {
            if (auto* text = dynamic_cast<jadefx::StyledTextArea*>(node)) {
                return text;
            }
        }
        return nullptr;
    };
    frame();
    frame();
    Expect(area() != nullptr && area()->isEditable(), "an open script's editor takes typing");
    engine.on_simulation([id](engine_core::DataModel& game) {
        game.destroy(id);
        game.history().end_gesture();
    });
    frame();
    Expect(area() != nullptr && !area()->isEditable(), "the editor of a deleted script is read-only");
    engine.on_simulation([](engine_core::DataModel& game) { game.history().undo(); });
    frame();
    Expect(area() != nullptr && area()->isEditable(), "undoing the delete makes the editor take typing again");
    ExpectText(editor->text(), "print(1)\n", "and it shows the script's text");
    holder->getChildren().clear();
    frame();
}

}  // namespace

int RunScriptTabTests(ide::IdeLayout& layout, jadefx::Scene& scene) {
    gFailures = 0;
    TestClosedTabs(layout, scene);
    TestPickerHookLeavesWithEditor(layout.simulation());
    TestEditorComesBackWithItsScript(layout.simulation());
    return gFailures;
}
