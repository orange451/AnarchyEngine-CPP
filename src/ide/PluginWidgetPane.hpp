#pragma once

#include "IdePane.hpp"
#include "types.hpp"

#include <memory>
#include <string>

namespace engine_core {
class Engine;
}

namespace runner {
class GuiTree;
}

namespace ide {

// A plugin's DockWidget as a studio page. A GuiTree draws the widget's GUI in
// the studio's own scene, so the studio's theme and stylesheet reach it as
// they reach Explorer, with the plugin's CSS over them. The mouse and keys stay
// in the page. An ImagePane's Texture path is under the studio's resources.
class PluginWidgetPane : public IdePane {
public:
    PluginWidgetPane(engine_core::Engine& engine, engine_core::InstanceId widget, std::string paneName);
    ~PluginWidgetPane() override;

    engine_core::InstanceId widget() const { return widget_; }
    // Reads the widget's subtree under a short read lock and updates the nodes.
    // Skipped, keeping the nodes as they are, when the DataModel is busy.
    void sync();

protected:
    void layoutChildren() override;

private:
    engine_core::Engine& engine_;
    engine_core::InstanceId widget_;
    std::unique_ptr<runner::GuiTree> tree_;
    jadefx::Node* shown_ = nullptr;
};

}  // namespace ide
