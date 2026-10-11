#include "PluginWidgetPane.hpp"

#include "runner/SceneFeed.hpp"

#include "AssetChoices.hpp"
#include "AssetPicker.hpp"
#include "IdeResources.hpp"
#include "runner/GuiTree.hpp"

#include "DataModelLock.hpp"
#include "Engine.hpp"
#include "Gui.hpp"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace ide {

PluginWidgetPane::PluginWidgetPane(engine_core::Engine& engine, runner::SceneFeed& feed, engine_core::InstanceId widget,
                                   std::string paneName)
    : IdePane(std::move(paneName), true), engine_(engine), feed_(feed), widget_(widget),
      tree_(std::make_unique<runner::GuiTree>(engine, std::make_shared<runner::GuiInput>(), [] {
          // The folder resources/icons is in, so a plugin's images name files the studio ships.
          return find_resource("icons").parent_path();
      })) {
    getClassList().add("plugin-widget");
    tree_->setThemedText(true);
    tree_->setViewportRenderer(&viewports_);
    tree_->setAssetPicking([this](jadefx::Node& anchor, engine_core::InstanceId picker, const std::string& asset_class,
                                  engine_core::InstanceId current) {
        std::vector<AssetChoice> choices;
        {
            engine_core::DataModelLock lock(engine_.datamodel(), engine_core::DataModelLock::Read,
                                            std::chrono::milliseconds(50));
            if (!lock.owns()) {
                return;
            }
            choices = asset_choices(engine_.datamodel(), asset_class);
        }
        if (!picker_) {
            picker_ = AssetPicker::create();
        }
        picker_->open(anchor, asset_class, std::move(choices), current,
                      [this, picker](engine_core::InstanceId chosen) { tree_->pickAsset(picker, chosen); });
    });
    setIconFile("Script.png");
}

PluginWidgetPane::~PluginWidgetPane() {
    if (picker_) {
        picker_->dismiss();
    }
}

void PluginWidgetPane::sync() {
    std::shared_ptr<jadefx::Node> root;
    // The engine's newest snapshot, for the ViewportPanes' scenes.
    const std::shared_ptr<const engine_core::VisualSnapshot> held = feed_.hold();
    {
        engine_core::DataModelLock lock(engine_.datamodel(), engine_core::DataModelLock::Read,
                                        std::chrono::milliseconds(1));
        if (!lock.owns()) {
            return;
        }
        tree_->setViewports(held ? &held->viewports : nullptr);
        viewports_.setResourcesRoot(engine_.datamodel().resources_root());
        tree_->beginPass();
        if (const auto* gui = dynamic_cast<const engine_core::DockWidget*>(engine_.datamodel().instance(widget_))) {
            root = tree_->build(widget_, *gui);
        }
        tree_->endPass();
        tree_->setViewports(nullptr);
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

void PluginWidgetPane::renderChildren(jadefx::UiRenderer& renderer, float opacity) {
    // The ViewportPanes due this frame, each into its own texture, before the widget paints them.
    viewports_.renderPending();
    IdePane::renderChildren(renderer, opacity);
}

void PluginWidgetPane::sceneChanged(jadefx::Scene* previous) {
    IdePane::sceneChanged(previous);
    // Leaving a live window releases the panes' GL objects while its context
    // is current; a window being torn down has none left to release.
    if (previous != nullptr && !previous->isTearingDown() && getScene() != previous) {
        viewports_.release();
    }
}

}  // namespace ide
