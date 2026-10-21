// The shader editor: a Shader opens in a docked tab, titled <Name>.glsl, whose
// typing reaches Source and whose Error is marked on its line.

#include "ide/IdeDock.hpp"
#include "ide/IdeAssets.hpp"
#include "ide/IdeLayout.hpp"
#include "ide/IdeProblems.hpp"
#include "ide/IdeShaderEditor.hpp"

#include "DataModel.hpp"
#include "Engine.hpp"
#include "SelectionService.hpp"
#include "Shader.hpp"
#include "runner/GameView.hpp"

#include "jadefx/jadefx.hpp"

#include <cstdio>
#include <string>

namespace {

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

constexpr const char* kBadHint = "shader surface;\n\n// glow is not a hint\nuniform float Shine : glow;\n\nvoid surface() {}\n";

ide::IdeShaderEditor* EditorFor(jadefx::Scene& scene, engine_core::InstanceId id) {
    for (jadefx::Node* area : scene.getElementsByClassName("ide-script")) {
        for (jadefx::Node* node = area; node != nullptr; node = node->getParent()) {
            auto* editor = dynamic_cast<ide::IdeShaderEditor*>(node);
            if (editor != nullptr && editor->instanceId() == id) {
                return editor;
            }
        }
    }
    return nullptr;
}

int CountEditors(jadefx::Scene& scene, engine_core::InstanceId id) {
    int count = 0;
    for (jadefx::Node* area : scene.getElementsByClassName("ide-script")) {
        for (jadefx::Node* node = area; node != nullptr; node = node->getParent()) {
            auto* editor = dynamic_cast<ide::IdeShaderEditor*>(node);
            if (editor != nullptr && editor->instanceId() == id) {
                ++count;
                break;
            }
        }
    }
    return count;
}

std::string SourceOf(engine_core::Engine& engine, engine_core::InstanceId id) {
    std::string out;
    engine.on_simulation([&](engine_core::DataModel& game) {
        if (auto* shader = dynamic_cast<engine_core::Shader*>(game.instance(id))) {
            out = shader->source();
        }
    });
    return out;
}

void TestShaderTab(ide::IdeLayout& layout, jadefx::Scene& scene) {
    engine_core::Engine& engine = layout.simulation();
    double time = 40.0;
    auto frame = [&] {
        scene.layout(1280, 800, time);
        time += 0.05;
    };
    engine_core::InstanceId id = 0;
    engine.on_simulation([&](engine_core::DataModel& game) {
        engine_core::Shader& shader = game.create<engine_core::Shader>();
        game.set_name(shader.id(), "Look");
        game.set_parent(shader.id(), game.service("Shaders"));
        id = shader.id();
    });

    // A double-click on the Shader in the Assets pane, in its Shaders folder.
    // JadeFX counts two presses on one spot within 0.4 s of real time as a double-click.
    // Assets may be a tab behind the Console: its dock is found through a pane in front.
    ide::IdeAssets* assets = nullptr;
    ide::IdeDock* assets_dock = nullptr;
    jadefx::Tab* was_front = nullptr;
    for (jadefx::Node* node : scene.getElementsByClassName("ide-pane")) {
        ide::IdeDock* dock = nullptr;
        for (jadefx::Node* up = node; up != nullptr && dock == nullptr; up = up->getParent()) {
            dock = dynamic_cast<ide::IdeDock*>(up);
        }
        if (dock == nullptr || assets != nullptr) {
            continue;
        }
        for (const std::shared_ptr<jadefx::Tab>& tab : dock->tabs()->getTabs().items()) {
            if (auto* pane = tab ? dynamic_cast<ide::IdeAssets*>(tab->getContent()) : nullptr) {
                assets_dock = dock;
                was_front = dock->tabs()->getSelectedTab();
                dock->tabs()->select(tab);
                assets = pane;
            }
        }
    }
    frame();
    frame();
    Expect(assets != nullptr, "the Assets pane is in the layout");
    if (assets == nullptr) {
        return;
    }
    auto edit = [&] {
        assets->openFolder(engine.datamodel().service("Shaders"));
        frame();
        frame();
        jadefx::Node* item = assets->itemNode(id);
        Expect(item != nullptr, "the Shader is in the Assets pane");
        if (item != nullptr) {
            const double x = item->getAbsoluteX() + item->getWidth() * 0.5;
            const double y = item->getAbsoluteY() + item->getHeight() * 0.5;
            for (int click = 0; click < 2; ++click) {
                scene.noteButton(0, true, x, y);
                scene.noteButton(0, false, x, y);
            }
        }
        frame();
        frame();
    };
    edit();
    ide::IdeShaderEditor* editor = EditorFor(scene, id);
    Expect(editor != nullptr, "Edit opens a shader editor");
    if (editor == nullptr) {
        return;
    }
    Expect(editor->title() == "Look.glsl", "the tab is titled <Name>.glsl");
    Expect(editor->text() == engine_core::kShaderStarterSource, "it shows the source");
    Expect(editor->errorLine() == 0, "the starter source has no error");
    edit();
    Expect(CountEditors(scene, id) == 1, "Edit again shows the same tab");

    engine.on_simulation([&](engine_core::DataModel& game) { game.set_name(id, "Glow"); });
    frame();
    frame();
    Expect(editor->title() == "Glow.glsl", "a rename retitles the tab");

    // Typing the whole text, then the pause: Source follows, and Error's line is marked.
    jadefx::Node* area = nullptr;
    for (jadefx::Node* node : scene.getElementsByClassName("ide-script")) {
        if (EditorFor(scene, id) != nullptr && node->getParent() == editor) {
            area = node;
        }
    }
    auto* code = dynamic_cast<jadefx::CodeArea*>(area);
    Expect(code != nullptr, "the editor holds a code area");
    if (code == nullptr) {
        return;
    }
    code->setText(kBadHint);
    editor->flush();
    frame();
    frame();
    // The header line is a property now, taken out of the text as spaces.
    const std::string written = SourceOf(engine, id);
    Expect(written.find("shader surface") == std::string::npos &&
               written.find("\n\n// glow is not a hint\nuniform float Shine : glow;") != std::string::npos,
           "a flush writes Source, less the header line");
    Expect(editor->errorLine() == 4, "the error's line is marked");
    // Text added above the error leaves Error's text the same; the mark still follows its line.
    const std::string padded =
        "shader surface;\n// padding added above the error\n// glow is not a hint\nuniform float Shine : glow;\n\nvoid surface() {}\n";
    code->setText(padded);
    editor->flush();
    frame();
    frame();
    const int line_four = static_cast<int>(padded.find("uniform"));
    Expect(editor->errorMarkStart() == line_four, "the mark moves with its line when Error's text stays");
    code->setText(engine_core::kShaderStarterSource);
    editor->flush();
    frame();
    frame();
    Expect(editor->errorLine() == 0, "fixing it clears the mark");

    editor->showLine(5);
    Expect(code->caretPosition() > 0, "showLine moves the caret");

    // Leave the studio as the later tests expect it: the editor closed, and the
    // tab that was in front of Assets in front again.
    for (ide::IdeDock* up = nullptr; up == nullptr;) {
        for (jadefx::Node* node = editor; node != nullptr && up == nullptr; node = node->getParent()) {
            up = dynamic_cast<ide::IdeDock*>(node);
        }
        if (up == nullptr) {
            break;
        }
        for (const std::shared_ptr<jadefx::Tab>& tab : up->tabs()->getTabs().items()) {
            if (tab && tab->getContent() == editor) {
                const std::shared_ptr<jadefx::Tab> closing = tab;
                up->tabs()->close(closing);
                break;
            }
        }
    }
    if (assets_dock != nullptr && was_front != nullptr) {
        assets_dock->tabs()->select(was_front);
    }
    frame();
    frame();
}

// Opening a Shader's problem in the Problems pane opens the shader editor at its line.
void TestProblemOpensShader(ide::IdeLayout& layout, jadefx::Scene& scene) {
    engine_core::Engine& engine = layout.simulation();
    double time = 60.0;
    auto frame = [&] {
        scene.layout(1280, 800, time);
        time += 0.1;
    };
    engine_core::InstanceId id = 0;
    engine.on_simulation([&](engine_core::DataModel& game) {
        engine_core::Shader& shader = game.create<engine_core::Shader>();
        game.set_name(shader.id(), "Broken");
        game.set_parent(shader.id(), game.service("Shaders"));
        shader.set_source(kBadHint);
        id = shader.id();
    });
    layout.show_problems();
    for (int i = 0; i < 8; ++i) {
        frame();
    }
    ide::IdeProblems* problems = nullptr;
    for (jadefx::Node* node : scene.getElementsByClassName("problems-pane")) {
        problems = problems != nullptr ? problems : dynamic_cast<ide::IdeProblems*>(node);
    }
    Expect(problems != nullptr, "Problems is open");
    if (problems == nullptr) {
        return;
    }
    std::size_t index = 0;
    bool listed = false;
    for (const ide::ScriptProblems& source : problems->list().scripts) {
        if (source.id == id) {
            listed = true;
            break;
        }
        ++index;
    }
    Expect(listed, "the Shader's error is listed");
    const auto& rows = problems->tree().getRoot()->getChildren();
    if (listed && index < rows.size() && !rows.items()[index]->getChildren().empty()) {
        problems->openRow(rows.items()[index]->getChildren().items()[0].get());
    }
    frame();
    frame();
    ide::IdeShaderEditor* editor = EditorFor(scene, id);
    Expect(editor != nullptr, "opening the problem opens the shader editor");
    if (editor == nullptr) {
        return;
    }
    jadefx::CodeArea* code = nullptr;
    for (jadefx::Node* node : scene.getElementsByClassName("ide-script")) {
        if (node->getParent() == editor) {
            code = dynamic_cast<jadefx::CodeArea*>(node);
        }
    }
    Expect(code != nullptr && code->currentParagraph() == 3, "with the caret on line 4");
    // Close it, so the Scene View's tab is in front for the tests after this one.
    for (jadefx::Node* node = editor; node != nullptr; node = node->getParent()) {
        if (auto* dock = dynamic_cast<ide::IdeDock*>(node)) {
            std::shared_ptr<jadefx::Tab> found;
            for (const std::shared_ptr<jadefx::Tab>& tab : dock->tabs()->getTabs().items()) {
                if (tab && tab->getContent() == editor) {
                    found = tab;
                }
            }
            if (found) {
                dock->tabs()->close(found);
            }
            break;
        }
    }
    engine.on_simulation([&](engine_core::DataModel& game) { game.destroy(id); });
    frame();
}

// Ctrl+F in the shader editor: the name under the caret is found, Replace All
// rewrites every match, and Escape closes the bar.
void TestFindReplace(ide::IdeLayout& layout, jadefx::Scene& scene) {
    engine_core::Engine& engine = layout.simulation();
    double time = 80.0;
    auto frame = [&] {
        scene.layout(1280, 800, time);
        time += 0.1;
    };
    constexpr const char* kSource =
        "shader surface;\nuniform float Progress : range(0, 1);\nvoid surface() {\n    ALPHA = Progress * Progress;\n}\n";
    engine_core::InstanceId id = 0;
    engine.on_simulation([&](engine_core::DataModel& game) {
        engine_core::Shader& shader = game.create<engine_core::Shader>();
        game.set_name(shader.id(), "Finder");
        game.set_parent(shader.id(), game.service("Shaders"));
        shader.set_source(kSource);
        id = shader.id();
    });
    layout.show_problems();
    for (int i = 0; i < 4; ++i) {
        frame();
    }
    // Open it the way Problems or the Assets pane would: here, through Problems'
    // host is not needed, so the Assets pane's double-click is reused.
    ide::IdeAssets* assets = nullptr;
    ide::IdeDock* assets_dock = nullptr;
    jadefx::Tab* was_front = nullptr;
    for (jadefx::Node* node : scene.getElementsByClassName("ide-pane")) {
        for (jadefx::Node* up = node; up != nullptr && assets == nullptr; up = up->getParent()) {
            if (auto* dock = dynamic_cast<ide::IdeDock*>(up)) {
                for (const std::shared_ptr<jadefx::Tab>& tab : dock->tabs()->getTabs().items()) {
                    if (auto* pane = tab ? dynamic_cast<ide::IdeAssets*>(tab->getContent()) : nullptr) {
                        assets_dock = dock;
                        was_front = dock->tabs()->getSelectedTab();
                        dock->tabs()->select(tab);
                        assets = pane;
                    }
                }
                break;
            }
        }
    }
    frame();
    Expect(assets != nullptr, "the Assets pane is in the layout");
    if (assets == nullptr) {
        return;
    }
    assets->openFolder(engine.datamodel().service("Shaders"));
    frame();
    frame();
    if (jadefx::Node* item = assets->itemNode(id)) {
        const double x = item->getAbsoluteX() + item->getWidth() * 0.5;
        const double y = item->getAbsoluteY() + item->getHeight() * 0.5;
        scene.noteButton(0, true, x, y);
        scene.noteButton(0, false, x, y);
        scene.noteButton(0, true, x, y);
        scene.noteButton(0, false, x, y);
    }
    frame();
    frame();
    ide::IdeShaderEditor* editor = EditorFor(scene, id);
    Expect(editor != nullptr, "the shader opens for find");
    if (editor == nullptr) {
        return;
    }
    jadefx::CodeArea* code = nullptr;
    for (jadefx::Node* node : scene.getElementsByClassName("ide-script")) {
        if (node->getParent() == editor) {
            code = dynamic_cast<jadefx::CodeArea*>(node);
        }
    }
    Expect(code != nullptr, "the editor holds a code area");
    if (code == nullptr) {
        return;
    }
    // The caret inside the first "Progress", then Ctrl+F.
    code->requestFocus();
    code->moveTo(1, 16);
    frame();
    scene.noteKey(jadefx::Key::F, true, false, jadefx::Key::ModControl);
    scene.noteKey(jadefx::Key::F, false, false, 0);
    frame();
    Expect(editor->findOpen(), "Ctrl+F opens the find bar");
    Expect(editor->findMatchCount() == 3, "the name under the caret is found three times");
    Expect(editor->replaceAll("Progress", "Amount") == 3, "Replace All rewrites every match");
    editor->flush();
    frame();
    frame();
    std::string first;
    engine.on_simulation([&](engine_core::DataModel& game) {
        if (auto* shader = dynamic_cast<engine_core::Shader*>(game.instance(id))) {
            first = shader->parse()->params.empty() ? std::string() : shader->parse()->params[0].name;
        }
    });
    Expect(first == "Amount", "after the write the uniform is Amount");
    editor->openFind(false);
    frame();
    code->requestFocus();
    scene.noteKey(jadefx::Key::Escape, true, false, 0);
    scene.noteKey(jadefx::Key::Escape, false, false, 0);
    frame();
    Expect(!editor->findOpen(), "Escape closes the find bar");

    for (jadefx::Node* node = editor; node != nullptr; node = node->getParent()) {
        if (auto* dock = dynamic_cast<ide::IdeDock*>(node)) {
            std::shared_ptr<jadefx::Tab> found;
            for (const std::shared_ptr<jadefx::Tab>& tab : dock->tabs()->getTabs().items()) {
                if (tab && tab->getContent() == editor) {
                    found = tab;
                }
            }
            if (found) {
                dock->tabs()->close(found);
            }
            break;
        }
    }
    if (assets_dock != nullptr && was_front != nullptr) {
        assets_dock->tabs()->select(was_front);
    }
    engine.on_simulation([&](engine_core::DataModel& game) { game.destroy(id); });
    frame();
}

// A surface Shader the driver refuses: the Scene View posts the error it got
// to the Shader, whose Error then marks the editor's line.
void TestCompileErrorReachesShader(ide::IdeLayout& layout, jadefx::Scene& scene) {
    engine_core::Engine& engine = layout.simulation();
    double time = 80.0;
    auto frame = [&] {
        scene.layout(1280, 800, time);
        time += 0.05;
    };
    runner::GameView* view = nullptr;
    for (jadefx::Node* node : scene.getElementsByClassName("ide-pane")) {
        auto* pane = dynamic_cast<ide::IdePane*>(node);
        if (view == nullptr && pane != nullptr && pane->name() == "Scene View") {
            view = dynamic_cast<runner::GameView*>(pane);
        }
    }
    Expect(view != nullptr, "the Scene View is in the layout");
    if (view == nullptr) {
        return;
    }
    engine_core::InstanceId id = 0;
    std::uint64_t version = 0;
    engine.on_simulation([&](engine_core::DataModel& game) {
        engine_core::Shader& shader = game.create<engine_core::Shader>();
        game.set_name(shader.id(), "Wobbly");
        game.set_parent(shader.id(), game.service("Shaders"));
        shader.set_source("shader surface;\n\nvoid surface() {\n    ALBEDO = wobble(ALBEDO);\n}\n");
        id = shader.id();
        version = shader.source_version();
    });
    view->postSurfaceErrors(
        {{id, version, engine_core::ShaderStage::Fragment, "4: 'wobble' : no matching overloaded function found"}});
    std::string error;
    engine.on_simulation([&](engine_core::DataModel& game) {
        if (auto* shader = dynamic_cast<engine_core::Shader*>(game.instance(id))) {
            error = shader->error();
        }
    });
    Expect(error == "fragment 4: 'wobble' : no matching overloaded function found",
           "the Scene View's error reaches Shader.Error");
    // Listed in Problems, whose row opens the editor at it.
    layout.show_problems();
    for (int i = 0; i < 8; ++i) {
        frame();
    }
    ide::IdeProblems* problems = nullptr;
    for (jadefx::Node* node : scene.getElementsByClassName("problems-pane")) {
        problems = problems != nullptr ? problems : dynamic_cast<ide::IdeProblems*>(node);
    }
    std::size_t index = 0;
    bool listed = false;
    if (problems != nullptr) {
        for (const ide::ScriptProblems& source : problems->list().scripts) {
            if (source.id == id) {
                listed = true;
                break;
            }
            ++index;
        }
    }
    Expect(listed, "the compile error is listed in Problems");
    if (listed) {
        const auto& rows = problems->tree().getRoot()->getChildren();
        if (index < rows.size() && !rows.items()[index]->getChildren().empty()) {
            problems->openRow(rows.items()[index]->getChildren().items()[0].get());
        }
    }
    frame();
    frame();
    ide::IdeShaderEditor* editor = EditorFor(scene, id);
    Expect(editor != nullptr && editor->errorLine() == 4, "the shader editor marks the compile error's line");
    // Close it, so the Scene View's tab is in front for the tests after this one.
    for (jadefx::Node* node = editor; node != nullptr; node = node->getParent()) {
        if (auto* dock = dynamic_cast<ide::IdeDock*>(node)) {
            std::shared_ptr<jadefx::Tab> found;
            for (const std::shared_ptr<jadefx::Tab>& tab : dock->tabs()->getTabs().items()) {
                if (tab && tab->getContent() == editor) {
                    found = tab;
                }
            }
            if (found) {
                dock->tabs()->close(found);
            }
            break;
        }
    }
    engine.on_simulation([&](engine_core::DataModel& game) { game.destroy_tree(id); });
    frame();
}

}  // namespace

int RunShaderEditorTests(ide::IdeLayout& layout, jadefx::Scene& scene) {
    gFailures = 0;
    TestShaderTab(layout, scene);
    TestProblemOpensShader(layout, scene);
    TestFindReplace(layout, scene);
    TestCompileErrorReachesShader(layout, scene);
    if (gFailures == 0) {
        std::printf("shader editor tests passed\n");
    }
    return gFailures;
}
