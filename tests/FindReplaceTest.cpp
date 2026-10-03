#include "ide/FindBar.hpp"
#include "ide/IdeScriptEditor.hpp"
#include "ide/IdeSearch.hpp"
#include "ide/IdeTheme.hpp"
#include "ide/TextUndoStack.hpp"

#include "ChangeHistoryService.hpp"
#include "Engine.hpp"
#include "Folder.hpp"
#include "ModuleScript.hpp"
#include "Script.hpp"

#include "jadefx/jadefx.hpp"

#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

// The script editor's find bar, its require completion, and the Search pane,
// driven through headless scenes.
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

// A left click at the middle of node.
void Click(jadefx::Scene& scene, const jadefx::Node& node) {
    const double x = node.getAbsoluteX() + node.getWidth() * 0.5;
    const double y = node.getAbsoluteY() + node.getHeight() * 0.5;
    scene.noteButton(0, true, x, y);
    scene.noteButton(0, false, x, y);
}

#if defined(__APPLE__)
constexpr int kToggleMods = jadefx::Key::ModSuper | jadefx::Key::ModAlt;
#else
constexpr int kToggleMods = jadefx::Key::ModAlt;
#endif

struct EditorRig {
    engine_core::Engine& engine;
    ide::TextUndoStack stack;
    std::shared_ptr<ide::IdeScriptEditor> editor;
    std::shared_ptr<jadefx::Scene> scene;
    jadefx::StyledTextArea* area = nullptr;
    double time = 0;

    EditorRig(engine_core::Engine& engine, const std::string& source) : engine(engine) {
        engine_core::DataModel& game = engine.datamodel();
        engine_core::Script& script = game.create<engine_core::Script>();
        game.set_name(script.id(), "Mover");
        game.set_parent(script.id(), game.scene_service("Workspace"));
        script.set_source(source);
        editor = jadefx::make<ide::IdeScriptEditor>(engine, script.id());
        editor->bindUndo(&stack);
        editor->setPrefWidthRatio(1);
        editor->setPrefHeightRatio(1);
        scene = jadefx::make<jadefx::Scene>(editor, 900, 600);
        frame();
        for (jadefx::Node* node : editor->getElementsByClassName("ide-script")) {
            area = dynamic_cast<jadefx::StyledTextArea*>(node);
        }
        editor->focus();
        frame();
    }

    void frame() {
        time += 0.1;
        scene->layout(900, 600, time);
    }
    // A key with modifiers, then its release with none, so typed text is not taken for a shortcut.
    void chord(int key, int mods) {
        scene->noteKey(key, true, false, mods);
        scene->noteKey(key, false, false, 0);
        frame();
    }
    void key(int code) { chord(code, 0); }
    void type(const std::string& text) {
        scene->noteText(text);
        frame();
    }
    ide::FindBar& bar() const { return *editor->findBar(); }
    std::string count() const {
        for (jadefx::Node* node : editor->getElementsByClassName("find-count")) {
            if (auto* label = dynamic_cast<jadefx::Label*>(node)) {
                return label->getText();
            }
        }
        return {};
    }
    std::string text() const { return editor->text(); }
    std::string selected() const { return area != nullptr ? area->selectedText() : std::string(); }
    // The style classes on the code point at index.
    std::string classesAt(int index) const {
        const jadefx::StyleSpans spans = area->getStyleSpans(index, index + 1);
        return spans.empty() ? std::string() : spans.spans().front().style.styleClass;
    }
};

bool Has(const std::string& classes, const char* name) { return classes.find(name) != std::string::npos; }

void TestFindBar(engine_core::Engine& engine) {
    EditorRig rig(engine, "local part = 1\nlocal Part = part + 1\nprint(part, parts)\n");
    Expect(rig.area != nullptr && rig.area->isFocused(), "the editor's text has the focus");
    Expect(!rig.editor->findOpen(), "the find bar starts closed");

    // The caret is at the start of "local", so that name is what to find.
    rig.chord(jadefx::Key::F, jadefx::Key::ModControl);
    Expect(rig.editor->findOpen(), "Cmd+F opens the find bar");
    Expect(rig.bar().findInput().field().isFocused(), "the find field takes the focus");
    ExpectText(rig.bar().findInput().text(), "local", "the name under the caret is the find text");
    Expect(!rig.bar().replaceShown(), "Cmd+F opens with replace hidden");
    ExpectText(rig.count(), "1 of 2", "the count shows the match the caret is on");
    Expect(rig.bar().getWidth() > 200 && rig.bar().getAbsoluteX() > 400, "the bar sits at the editor's top right");

    // Typing replaces the selected find text, and the search follows as you type.
    rig.type("part");
    ExpectText(rig.count(), "1 of 5", "plain text ignores case");
    ExpectText(rig.selected(), "part", "the first match from the old one is selected");
    Expect(rig.area->selection().start == 6, "the first match after the caret is the current one");
    Expect(Has(rig.classesAt(6), "find-current"), "the current match is highlighted strongly");
    Expect(Has(rig.classesAt(21), "find-match") && !Has(rig.classesAt(21), "find-current"),
           "the other matches are highlighted");
    Expect(Has(rig.classesAt(0), "keyword") && !Has(rig.classesAt(0), "find"), "text outside matches keeps its style");

    rig.chord(jadefx::Key::C, kToggleMods);
    ExpectText(rig.count(), "1 of 4", "Match Case drops Part");
    rig.chord(jadefx::Key::W, kToggleMods);
    ExpectText(rig.count(), "1 of 3", "Match Whole Word drops parts");

    rig.key(jadefx::Key::Enter);
    ExpectText(rig.count(), "2 of 3", "Enter goes to the next match");
    Expect(rig.area->selection().start == 28, "the next match is selected");
    Expect(Has(rig.classesAt(28), "find-current") && !Has(rig.classesAt(6), "find-current"),
           "the strong highlight moves with the current match");
    rig.key(jadefx::Key::Enter);
    rig.key(jadefx::Key::Enter);
    ExpectText(rig.count(), "1 of 3", "Enter wraps from the last match to the first");
    rig.chord(jadefx::Key::Enter, jadefx::Key::ModShift);
    ExpectText(rig.count(), "3 of 3", "Shift+Enter goes back, wrapping to the last");
    rig.chord(jadefx::Key::Enter, jadefx::Key::ModShift);
    ExpectText(rig.count(), "2 of 3", "Shift+Enter steps back");

    // Replace: the chevron shows the replace row, and Tab moves to it.
    rig.key(jadefx::Key::Tab);
    Expect(rig.bar().findInput().field().isFocused(), "Tab stays in find while replace is hidden");
    rig.bar().setReplaceShown(true);
    rig.frame();
    rig.key(jadefx::Key::Tab);
    Expect(rig.bar().replaceInput().field().isFocused(), "Tab moves to the replace field");
    // Enter replaces the current match and moves on.
    rig.type("piece");
    rig.key(jadefx::Key::Enter);
    ExpectText(rig.text(), "local part = 1\nlocal Part = piece + 1\nprint(part, parts)\n", "Enter replaces the current match");
    ExpectText(rig.count(), "2 of 2", "the next match is current after a replace");
    Expect(rig.stack.can_undo(), "a replace is an undo step");

    rig.chord(jadefx::Key::Enter, jadefx::Key::ModControl);
    ExpectText(rig.text(), "local piece = 1\nlocal Part = piece + 1\nprint(piece, parts)\n", "Cmd+Enter replaces all");
    ExpectText(rig.count(), "No results", "nothing is left to find");
    rig.stack.undo();
    rig.editor->applyUndoText();
    rig.frame();
    ExpectText(rig.text(), "local part = 1\nlocal Part = piece + 1\nprint(part, parts)\n", "one undo reverts Replace All");

    // A regex with groups.
    rig.key(jadefx::Key::Tab);
    Expect(rig.bar().findInput().field().isFocused(), "Tab from replace goes back to find");
    rig.chord(jadefx::Key::W, kToggleMods);
    rig.chord(jadefx::Key::R, kToggleMods);
    rig.type("(");
    ExpectText(rig.count(), "Invalid regex", "a regex that does not compile says so");
    rig.chord(jadefx::Key::A, jadefx::Key::ModControl);
    rig.type("(p\\w+) = (\\w+)");
    ExpectText(rig.count(), "1 of 1", "a regex finds with Match Case still on");
    rig.chord(jadefx::Key::C, kToggleMods);
    ExpectText(rig.count(), "1 of 2", "turning Match Case off finds the other line");
    rig.key(jadefx::Key::Tab);
    rig.chord(jadefx::Key::A, jadefx::Key::ModControl);
    rig.type("$2 = $1");
    rig.chord(jadefx::Key::Enter, jadefx::Key::ModControl);
    ExpectText(rig.text(), "local 1 = part\nlocal piece = Part + 1\nprint(part, parts)\n", "a regex replacement expands groups");

    // Escape closes the bar and gives the focus back to the text.
    rig.key(jadefx::Key::Escape);
    Expect(!rig.editor->findOpen(), "Escape closes the bar");
    Expect(rig.area->isFocused(), "the text has the focus again");
    Expect(!Has(rig.classesAt(6), "find"), "closing the bar clears the highlights");

    // With the text focused: a selection seeds the find text, and Escape there closes the bar too.
    rig.area->selectRange(15, 20);
    rig.chord(jadefx::Key::F, jadefx::Key::ModControl);
    ExpectText(rig.bar().findInput().text(), "local", "a selection seeds the find text");
    rig.editor->focus();
    rig.frame();
    rig.chord(jadefx::Key::G, jadefx::Key::ModControl);
    Expect(rig.area->selection().start == 0, "Cmd+G in the text goes to the next match");
    rig.key(jadefx::Key::Escape);
    Expect(!rig.editor->findOpen(), "Escape in the text closes the bar");

    // Opening again hides replace, and showing it makes the bar taller.
    rig.chord(jadefx::Key::F, jadefx::Key::ModControl);
    Expect(!rig.bar().replaceShown(), "the bar opens again with replace hidden");
    const double short_height = rig.bar().getHeight();
    rig.bar().setReplaceShown(true);
    rig.frame();
    Expect(rig.bar().getHeight() > short_height + 10, "showing replace adds its row");
    rig.chord(jadefx::Key::F, jadefx::Key::ModControl);
    Expect(rig.bar().replaceShown(), "Cmd+F on an open bar keeps replace shown");
    rig.bar().setReplaceShown(false);
    rig.frame();
#if defined(__APPLE__)
    rig.chord(jadefx::Key::F, jadefx::Key::ModSuper | jadefx::Key::ModAlt);
#else
    rig.chord(jadefx::Key::H, jadefx::Key::ModControl);
#endif
    Expect(rig.bar().replaceShown() && rig.bar().replaceInput().field().isFocused(),
           "the replace shortcut shows replace and focuses it");

    // The clear buttons empty their fields and leave the focus in them.
    ide::SearchInput& find = rig.bar().findInput();
    ide::SearchInput& replace = rig.bar().replaceInput();
    Expect(!find.text().empty() && find.clearButton().isVisible(), "find's clear button shows with find text");
    // The three toggles take some 63 pixels of the field's right end.
    const double find_right = find.field().getAbsoluteX() + find.field().getWidth();
    Expect(find.clearButton().getAbsoluteX() + find.clearButton().getWidth() <= find_right - 60 &&
               find.clearButton().getAbsoluteX() > find_right - 95,
           "the clear button sits left of the find field's toggles");
    const double replace_right = replace.field().getAbsoluteX() + replace.field().getWidth();
    Expect(replace.clearButton().getAbsoluteX() + replace.clearButton().getWidth() <= replace_right &&
               replace.clearButton().getAbsoluteX() > replace_right - 30,
           "with no toggles, the clear button sits at the replace field's right end");
    Click(*rig.scene, find.clearButton());
    rig.frame();
    rig.frame();
    ExpectText(find.text(), "", "the clear button empties the find field");
    Expect(find.field().isFocused(), "the find field keeps the focus after clearing");
    ExpectText(rig.count(), "No results", "an empty find has no results");
    Expect(!Has(rig.classesAt(0), "find"), "clearing find drops the highlights");
    Expect(!find.clearButton().isVisible(), "the clear button is hidden once the field is empty");
    Expect(!replace.text().empty() && replace.clearButton().isVisible(), "replace has a clear button of its own");
    Click(*rig.scene, replace.clearButton());
    rig.frame();
    ExpectText(replace.text(), "", "the clear button empties the replace field");
    Expect(replace.field().isFocused(), "the replace field takes the focus when cleared");
    Expect(!replace.clearButton().isVisible(), "replace's clear button is hidden once it is empty");
}

// Find matches and problems as bands on the editor's scroll bar.
void TestScrollBarMarks(engine_core::Engine& engine) {
    EditorRig rig(engine, "local part = 1\nprint(part)\nlocal broken = = 2\n");
    auto lane = [&rig](jadefx::ScrollMarkLane which) {
        std::vector<jadefx::ScrollMark> out;
        for (const jadefx::ScrollMark& mark : rig.area->scrollMarks()) {
            if (mark.lane == which) {
                out.push_back(mark);
            }
        }
        return out;
    };
    // The syntax error comes from the analysis worker.
    for (int wait = 0; wait < 500 && lane(jadefx::ScrollMarkLane::Right).empty(); ++wait) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        rig.frame();
    }
    const std::vector<jadefx::ScrollMark> problems = lane(jadefx::ScrollMarkLane::Right);
    const jadefx::Color error = ide::theme_color("--error-color");
    Expect(!problems.empty() && problems.back().start >= 27 && problems.back().color.r == error.r &&
               problems.back().color.g == error.g && problems.back().color.b == error.b,
           "the syntax error on the third line is an error band on the right of the scroll bar");
    Expect(lane(jadefx::ScrollMarkLane::Left).empty(), "a closed find bar marks nothing");

    rig.chord(jadefx::Key::F, jadefx::Key::ModControl);
    rig.type("part");
    const std::vector<jadefx::ScrollMark> found = lane(jadefx::ScrollMarkLane::Left);
    const jadefx::Color color = ide::theme_color("--ide-find-scroll-color");
    Expect(found.size() == 2 && found[0].start == 6 && found[0].end == 10 && found[1].start == 21 &&
               found[0].color.r == color.r && found[0].color.g == color.g && found[0].color.b == color.b,
           "each find match is a band on the left of the scroll bar");
    Expect(!lane(jadefx::ScrollMarkLane::Right).empty(), "the problems stay while finding");

    rig.type("x");
    Expect(lane(jadefx::ScrollMarkLane::Left).empty(), "text that finds nothing marks nothing");
    rig.key(jadefx::Key::Escape);
    Expect(!rig.editor->findOpen() && lane(jadefx::ScrollMarkLane::Left).empty(), "closing the bar clears its bands");
}

void TestShowRange(engine_core::Engine& engine) {
    EditorRig rig(engine, "a\nlocal target = 1\n");
    rig.editor->showRange(2, 6, 12);
    ExpectText(rig.selected(), "target", "showRange selects columns of a line");
    rig.editor->showRange(9, 0, 3);
    Expect(rig.area->selection().start == rig.area->absolutePosition(2, 0), "a line past the end is the last line");

    ide::SearchQuery query;
    query.pattern = "1";
    Expect(rig.editor->replaceMatches(query, "2") == 1, "replaceMatches counts what it replaced");
    ExpectText(rig.text(), "a\nlocal target = 2\n", "replaceMatches edits the buffer");
    query.pattern = "a";
    Expect(rig.editor->replaceMatches(query, "b", 2) == 2, "replaceMatches can keep to one line");
    ExpectText(rig.text(), "a\nlocbl tbrget = 2\n", "only the given line changed");
}

// A `.` that starts a line at the top of the script offers ModuleScripts and
// services. Enter writes the declaration over what was typed.
void TestRequireCompletion(engine_core::Engine& engine) {
    engine_core::DataModel& game = engine.datamodel();
    engine_core::Folder& folder = game.create<engine_core::Folder>();
    game.set_name(folder.id(), "Folder");
    game.set_parent(folder.id(), game.scene_service("Workspace"));
    engine_core::ModuleScript& config = game.create<engine_core::ModuleScript>();
    game.set_name(config.id(), "Config");
    game.set_parent(config.id(), folder.id());
    config.set_source("return {}\n");

    auto type_each = [](EditorRig& rig, const std::string& text) {
        for (const char unit : text) {
            rig.type(std::string(1, unit));
        }
    };

    EditorRig rig(engine, "print(1)\n");
    rig.area->moveTo(rig.area->length());
    type_each(rig, ".Conf");
    rig.key(jadefx::Key::Enter);
    ExpectText(rig.text(), "print(1)\nlocal Config = require(game.Workspace.Folder.Config)",
               "Enter on .Conf requires the module by its path");
    Expect(rig.area->caretPosition() == rig.area->length(), "the caret ends after the declaration");

    rig.key(jadefx::Key::Enter);
    type_each(rig, ".UserIn");
    rig.key(jadefx::Key::Enter);
    ExpectText(rig.text(),
               "print(1)\nlocal Config = require(game.Workspace.Folder.Config)\n"
               "local UserInputService = game:GetService(\"UserInputService\")",
               "Enter on .UserIn gets the service");

    EditorRig inner(engine, "local function f()\n    \nend\n");
    inner.area->moveTo(inner.area->absolutePosition(1, 4));
    type_each(inner, ".Conf");
    inner.key(jadefx::Key::Enter);
    Expect(inner.text().find("require") == std::string::npos, "inside a function a dot does not offer a require");

    game.destroy_tree(folder.id());
}

// A Search pane over a place of its own. One script plays an open editor whose
// text is ahead of its Source.
struct SearchRig {
    engine_core::Engine& engine;
    engine_core::InstanceId mover = 0;
    engine_core::InstanceId util = 0;
    engine_core::InstanceId quiet = 0;
    engine_core::InstanceId open = 0;
    std::string open_text = "print(part)\n";
    std::vector<std::tuple<engine_core::InstanceId, int, int, int>> opened;
    std::shared_ptr<ide::IdeSearch> pane;
    std::shared_ptr<jadefx::Scene> scene;
    double time = 0;

    engine_core::InstanceId add_script(engine_core::DataModel& game, engine_core::InstanceId parent, const char* name,
                                       const char* source, bool module = false) {
        engine_core::LuaSource* script = nullptr;
        if (module) {
            script = &game.create<engine_core::ModuleScript>();
        } else {
            script = &game.create<engine_core::Script>();
        }
        game.set_name(script->id(), name);
        game.set_parent(script->id(), parent);
        script->set_source(source);
        return script->id();
    }

    explicit SearchRig(engine_core::Engine& engine) : engine(engine) {
        engine_core::DataModel& game = engine.datamodel();
        // A place of its own: the scene services start empty for this rig.
        // Assets and its categories are fixed services, not scripts to search.
        for (engine_core::InstanceId service : game.get_children(game.id())) {
            const engine_core::DataModel* object = game.instance(service);
            if (object == nullptr || !object->is_scene_service()) {
                continue;
            }
            for (engine_core::InstanceId child : game.get_children(service)) {
                game.destroy_tree(child);
            }
        }
        mover = add_script(game, game.scene_service("Workspace"), "Mover", "local part = 1\nlocal other = part + part\n");
        engine_core::Folder& folder = game.create<engine_core::Folder>();
        game.set_name(folder.id(), "Tools");
        game.set_parent(folder.id(), game.scene_service("Workspace"));
        util = add_script(game, folder.id(), "Util", "-- nothing here\nreturn { Part = 2 }\n", true);
        quiet = add_script(game, game.scene_service("Workspace"), "Quiet", "print('hello')\n");
        open = add_script(game, game.scene_service("Workspace"), "Open", "print('stale')\n");
        // The studio's own tools are in Core, which the search leaves out.
        add_script(game, game.core(), "Tool", "local part = 0\n");
        // The studio closes each edit's gesture, so a replace is a step of its own.
        game.history().end_gesture();

        ide::SearchHost host;
        host.editor_text = [this](std::uint32_t id) -> std::optional<std::string> {
            if (id == open) {
                return open_text;
            }
            return std::nullopt;
        };
        host.replace_in_editor = [this](std::uint32_t id, const ide::SearchQuery& query, const std::string& replacement,
                                        int line) {
            if (id != open) {
                return -1;
            }
            int count = 0;
            open_text = ide::TextSearch(query).replace_all(open_text, replacement, line - 1, &count);
            return count;
        };
        host.open = [this](std::uint32_t id, int line, int column, int column_end) {
            opened.emplace_back(id, line, column, column_end);
        };
        pane = jadefx::make<ide::IdeSearch>(engine, std::move(host));
        pane->setPrefWidthRatio(1);
        pane->setPrefHeightRatio(1);
        scene = jadefx::make<jadefx::Scene>(pane, 360, 600);
        frame();
    }

    void frame() {
        time += 0.5;
        scene->layout(360, 600, time);
    }
    void search(const std::string& text) {
        pane->setFindText(text);
        frame();
        frame();
    }
    std::string source(engine_core::InstanceId id) const {
        const auto* script = dynamic_cast<const engine_core::LuaSource*>(engine.datamodel().instance(id));
        return script != nullptr ? script->source() : std::string();
    }
    jadefx::TreeItem* row(std::size_t script, int line = -1) const {
        jadefx::TreeItem* top = pane->tree().getRoot()->getChildren()[script].get();
        return line < 0 ? top : top->getChildren()[static_cast<std::size_t>(line)].get();
    }
    void click(jadefx::TreeItem* item) {
        jadefx::Node* cell = pane->tree().getCell(item);
        if (cell == nullptr) {
            Expect(false, "the row is on screen");
            return;
        }
        const double x = cell->getAbsoluteX() + cell->getWidth() * 0.6;
        const double y = cell->getAbsoluteY() + cell->getHeight() * 0.5;
        scene->noteButton(0, true, x, y);
        scene->noteButton(0, false, x, y);
        frame();
    }
};

void TestSearchPane(engine_core::Engine& engine) {
    SearchRig rig(engine);
    ExpectText(rig.pane->summary(), "Search every Script and ModuleScript", "an empty search says what it does");

    rig.search("part");
    const std::vector<ide::ScriptHits>& results = rig.pane->results();
    Expect(results.size() == 3, "three scripts have part in them");
    if (results.size() == 3) {
        Expect(results[0].id == rig.mover && results[1].id == rig.util && results[2].id == rig.open,
               "scripts come in the explorer's order");
        ExpectText(results[0].name, "Mover", "a script's name");
        ExpectText(results[1].path, "game.Workspace.Tools", "a script's path is its parent's");
        ExpectText(results[1].class_name, "ModuleScript", "a module script says so");
        Expect(results[0].matches == 3 && results[0].lines.size() == 2, "matches on one line are one row");
        if (results[0].lines.size() == 2) {
            const ide::ScriptHits::Line& line = results[0].lines[1];
            Expect(line.line == 2 && line.text == "local other = part + part", "a row has its line number and text");
            Expect(line.ranges.size() == 2 && line.ranges[0] == std::make_pair(14, 18) &&
                       line.ranges[1] == std::make_pair(21, 25),
                   "a row marks each match in its line");
        }
        ExpectText(results[2].lines.front().text, "print(part)", "an open editor's text wins over Source");
    }
    ExpectText(rig.pane->summary(), "5 results in 3 scripts", "the summary counts results and scripts");
    Expect(rig.pane->tree().getRoot()->getChildren().size() == 3 && rig.row(0)->getChildren().size() == 2,
           "each script is a row with its lines under it");

    // Clicking a line opens its script there, with the first match on the line.
    rig.click(rig.row(0, 1));
    Expect(!rig.opened.empty() && rig.opened.back() == std::make_tuple(rig.mover, 2, 14, 18),
           "a click on a line opens the script at that line");
    const std::size_t opens = rig.opened.size();
    rig.click(rig.row(1));
    Expect(rig.opened.size() == opens, "a click on a script's row opens nothing");
    rig.pane->tree().select(rig.row(1, 0));
    rig.pane->tree().requestFocus();
    rig.scene->noteKey(jadefx::Key::Enter, true, false, 0);
    Expect(rig.opened.size() == opens + 1 && std::get<0>(rig.opened.back()) == rig.util, "Enter on a line opens it");

    // A closed script stays closed when the results change.
    rig.row(0)->setExpanded(false);
    rig.search("par");
    Expect(!rig.row(0)->isExpanded() && rig.row(1)->isExpanded(), "a closed script stays closed across searches");
    rig.row(0)->setExpanded(true);

    // The toggles narrow the search.
    rig.search("part");
    rig.pane->findInput().field().requestFocus();
#if defined(__APPLE__)
    const int toggle = jadefx::Key::ModSuper | jadefx::Key::ModAlt;
#else
    const int toggle = jadefx::Key::ModAlt;
#endif
    rig.scene->noteKey(jadefx::Key::C, true, false, toggle);
    rig.frame();
    rig.frame();
    ExpectText(rig.pane->summary(), "4 results in 2 scripts", "Match Case drops Part");
    rig.scene->noteKey(jadefx::Key::C, true, false, toggle);
    rig.scene->noteKey(jadefx::Key::R, true, false, toggle);
    rig.search("(");
    ExpectText(rig.pane->summary(), "Invalid regular expression", "a broken regex says so");
    rig.scene->noteKey(jadefx::Key::R, true, false, toggle);
    rig.search("nowhere");
    ExpectText(rig.pane->summary(), "No results", "nothing found says so");
    Expect(rig.pane->tree().getRoot()->getChildren().empty(), "no results leave no rows");

    // Edits show up without a new search.
    rig.search("hello");
    ExpectText(rig.pane->summary(), "1 result in 1 script", "one result");
    rig.open_text = "print('hello', 'hello')\n";
    rig.frame();
    rig.frame();
    rig.frame();
    ExpectText(rig.pane->summary(), "3 results in 2 scripts", "results follow an editor's typing");

    // Replace in one line, then in everything.
    rig.open_text = "print(part)\n";
    rig.search("part");
    rig.pane->focusReplace();
    Expect(rig.pane->replaceShown() && rig.pane->replaceInput().field().isFocused(), "replace shows and takes the focus");
    rig.pane->replaceInput().field().setText("piece");
    rig.pane->replaceIn(rig.mover, 2);
    ExpectText(rig.source(rig.mover), "local part = 1\nlocal other = piece + piece\n", "a row's replace keeps to its line");
    ExpectText(rig.pane->summary(), "3 results in 3 scripts", "the results follow the replace");
    rig.pane->replaceAll(false);
    ExpectText(rig.source(rig.mover), "local piece = 1\nlocal other = piece + piece\n", "Replace All writes Source");
    ExpectText(rig.source(rig.util), "-- nothing here\nreturn { piece = 2 }\n", "Replace All reaches a module script");
    ExpectText(rig.open_text, "print(piece)\n", "an open editor replaces in its own text");
    ExpectText(rig.source(rig.open), "print('stale')\n", "an open editor's script is left to its editor");
    ExpectText(rig.source(rig.quiet), "print('hello')\n", "a script without a match is untouched");
    ExpectText(rig.pane->summary(), "No results", "nothing is left after Replace All");
    const std::pair<bool, std::string> after = engine.datamodel().history().can_undo();
    Expect(after.first && after.second == "Replace in Scripts", "Replace All is one place undo step");
    engine.datamodel().history().undo();
    ExpectText(rig.source(rig.util), "-- nothing here\nreturn { Part = 2 }\n", "one undo puts every script back");
    ExpectText(rig.source(rig.mover), "local part = 1\nlocal other = piece + piece\n",
               "undo stops at the step before Replace All");

    // The clear buttons empty the search and the replacement.
    rig.search("part");
    Expect(!rig.pane->results().empty(), "a search to clear");
    Click(*rig.scene, rig.pane->findInput().clearButton());
    rig.frame();
    rig.frame();
    ExpectText(rig.pane->findInput().text(), "", "the clear button empties the search");
    Expect(rig.pane->findInput().field().isFocused(), "the search field has the focus after clearing");
    Expect(rig.pane->results().empty(), "clearing the search drops its results");
    ExpectText(rig.pane->summary(), "Search every Script and ModuleScript", "a cleared search says what it does");
    Expect(!rig.pane->findInput().clearButton().isVisible(), "the search's clear button is hidden when empty");
    Click(*rig.scene, rig.pane->replaceInput().clearButton());
    rig.frame();
    ExpectText(rig.pane->replaceInput().text(), "", "the clear button empties the replacement");
}

}  // namespace

int RunFindReplaceTests(engine_core::Engine& engine) {
    gFailures = 0;
    TestFindBar(engine);
    TestScrollBarMarks(engine);
    TestShowRange(engine);
    TestRequireCompletion(engine);
    TestSearchPane(engine);
    return gFailures;
}
