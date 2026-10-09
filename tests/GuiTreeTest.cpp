#include "ide/IdeLayout.hpp"
#include "runner/GuiTree.hpp"

#include "DataModel.hpp"
#include "DataModelLock.hpp"
#include "Engine.hpp"
#include "Gui.hpp"

#include "jadefx/jadefx.hpp"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

// GuiTree alone: a DockWidget's subtree as nodes, and the nodes following edits.
namespace {

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

}  // namespace

int RunGuiTreeTests(ide::IdeLayout& layout) {
    engine_core::Engine& engine = layout.simulation();
    engine_core::InstanceId widget = 0;
    engine_core::InstanceId pane = 0;
    engine_core::InstanceId label = 0;
    engine_core::InstanceId screen = 0;
    engine.on_simulation([&](engine_core::DataModel& game) {
        auto& made = game.create<engine_core::DockWidget>();
        made.set_origin("T", "W");
        game.set_parent(made.id(), game.core());
        auto& body = game.create<engine_core::Pane>();
        game.set_name(body.id(), "Body");
        game.set_parent(body.id(), made.id());
        auto& text = game.create<engine_core::Label>();
        game.set_parent(text.id(), body.id());
        auto& filled = game.create<engine_core::ScreenGui>();
        game.set_parent(filled.id(), made.id());
        widget = made.id();
        pane = body.id();
        label = text.id();
        screen = filled.id();
    });
    runner::GuiTree tree(engine, std::make_shared<runner::GuiInput>(), [] { return std::filesystem::path(); });
    auto pass = [&] {
        std::shared_ptr<jadefx::Node> root;
        {
            engine_core::DataModelLock lock(engine.datamodel(), engine_core::DataModelLock::Read);
            tree.beginPass();
            if (const auto* gui = dynamic_cast<const engine_core::GuiValues*>(engine.datamodel().instance(widget))) {
                root = tree.build(widget, *gui);
            }
            tree.endPass();
        }
        tree.updateImages();
        return root;
    };
    std::shared_ptr<jadefx::Node> root = pass();
    Expect(root != nullptr && std::string(root->getElementType()) == "dockwidget", "a DockWidget is a dockwidget node");
    Expect(tree.nodeFor(label) != nullptr, "the label inside is built");
    Expect(tree.nodeFor(pane) != nullptr && tree.nodeFor(pane)->getElementId() == "Body", "Name is the CSS id");
    Expect(tree.nodeFor(screen) != nullptr, "a ScreenGui directly in a DockWidget fills it");
    Expect(tree.visible(pane) && !tree.mouseTransparent(pane), "visible and mouseTransparent read the instance");

    // Reparenting the pane out of the widget drops its nodes on the next pass.
    engine.on_simulation([&](engine_core::DataModel& game) { game.set_parent(pane, game.core()); });
    pass();
    Expect(tree.nodeFor(pane) == nullptr && tree.nodeFor(label) == nullptr, "GUI moved out of the widget leaves it");
    engine.on_simulation([&](engine_core::DataModel& game) {
        game.destroy(widget);
        game.destroy(pane);
    });
    return gFailures;
}
