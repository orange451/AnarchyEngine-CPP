#include "PluginWidgetPane.hpp"

#include "IdeResources.hpp"
#include "runner/GuiTree.hpp"

#include "DataModelLock.hpp"
#include "Engine.hpp"
#include "Gui.hpp"

#include <chrono>
#include <utility>

namespace ide {

PluginWidgetPane::PluginWidgetPane(engine_core::Engine& engine, engine_core::InstanceId widget, std::string paneName)
    : IdePane(std::move(paneName), true), engine_(engine), widget_(widget),
      tree_(std::make_unique<runner::GuiTree>(engine, std::make_shared<runner::GuiInput>(), [] {
          // The folder resources/icons is in, so a plugin's images name files the studio ships.
          return find_resource("icons").parent_path();
      })) {
    getClassList().add("plugin-widget");
    tree_->setThemedText(true);
    setIconFile("Script.png");
}

PluginWidgetPane::~PluginWidgetPane() = default;

void PluginWidgetPane::sync() {
    std::shared_ptr<jadefx::Node> root;
    {
        engine_core::DataModelLock lock(engine_.datamodel(), engine_core::DataModelLock::Read,
                                        std::chrono::milliseconds(1));
        if (!lock.owns()) {
            return;
        }
        tree_->beginPass();
        if (const auto* gui = dynamic_cast<const engine_core::DockWidget*>(engine_.datamodel().instance(widget_))) {
            root = tree_->build(widget_, *gui);
        }
        tree_->endPass();
    }
    tree_->updateImages();
    if (root.get() != shown_) {
        getChildren().clear();
        if (root) {
            Fill(*root);
            getChildren().add(root);
        }
        shown_ = root.get();
    }
}

void PluginWidgetPane::layoutChildren() {
    sync();
    IdePane::layoutChildren();
}

}  // namespace ide
