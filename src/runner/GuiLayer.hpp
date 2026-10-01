#pragma once

#include "types.hpp"

#include "jadefx/jadefx.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine_core {
class DataModel;
class Engine;
class GuiValues;
}

namespace runner {

// What the layer tells its Scene View about the mouse on a GUI element, which
// the game hears as processed. keepFocus is true when the element keeps the
// keyboard, as a TextField does. The layer and a ScreenGui's own area are not
// picked (setPickOnBounds), so the mouse anywhere else reaches the view itself.
struct GuiInput {
    std::function<void(const jadefx::MouseEvent&, bool keepFocus)> pressed;
    std::function<void(const jadefx::MouseEvent&)> released;
    std::function<void(const jadefx::MouseEvent&)> dragged;
    std::function<void(const jadefx::MouseEvent&)> moved;
};

// The Gui service's ScreenGuis, as JadeFX nodes over a Scene View. Each
// ScreenGui under the service, directly or through Folders, fills the layer;
// each GuiBase inside one, through a chain of GuiBases, is a node of its
// class's element type (screengui, pane, hbox, vbox, label, button,
// textfield) whose id is its Name and whose classes are its ClassList. The
// CSS instances under a GuiBase, joined in child order, are that node's
// stylesheet, and those directly under the service are the layer's, so they
// style every ScreenGui.
//
// The layer is the root of a jadefx::SubScene whose user-agent stylesheet is
// defaultStylesheet, so the studio's theme and stylesheets do not reach the
// game's nodes, and :root is the layer.
//
// sync reads the tree under a short read lock, on the UI thread, and changes
// only the nodes whose instance changed: a node keeps its state, such as a
// TextField's caret, while its instance does. Typing in a TextField writes
// Text back. Mouse events on a node fire its instance's events on the
// simulation thread, and the layer hands the mouse to its Scene View as
// GuiInput says. A ScreenGui's own area does not take the mouse.
class GuiLayer : public jadefx::Pane {
public:
    GuiLayer(engine_core::Engine& engine, GuiInput input);
    ~GuiLayer() override;

    const char* getElementType() const override { return "gui-layer"; }

    // The game UI's user-agent stylesheet: no outlines, backgrounds, or
    // padding. The hover wash, the focus ring, and a TextField's caret and
    // selection stay. Any CSS a project has wins over it.
    static const char* defaultStylesheet();

    // Brings the nodes up to date with the Gui service. Skipped, keeping the
    // nodes as they are, when the DataModel is busy.
    void sync();

    // The node for an instance, or null when it is not drawn. For tests.
    jadefx::Node* nodeFor(engine_core::InstanceId id) const;

protected:
    // Every ScreenGui fills the layer.
    void layoutChildren() override;

private:
    struct Entry;

    // The ScreenGuis under id, a service or a Folder, in tree order.
    void collectScreens(engine_core::InstanceId id, std::vector<std::shared_ptr<jadefx::Node>>& out);
    // The node for a GuiBase, made or brought up to date, with its children.
    std::shared_ptr<jadefx::Node> build(engine_core::InstanceId id, const engine_core::GuiValues& gui);
    std::shared_ptr<jadefx::Node> makeNode(engine_core::InstanceId id, const std::string& className);
    void apply(Entry& entry, const engine_core::GuiValues& gui);
    // Fires a GuiBase's event on the simulation thread.
    void fire(engine_core::InstanceId id, const char* event);
    // A TextField's typed text, written back to Text.
    void writeText(engine_core::InstanceId id, std::string text);

    engine_core::Engine& engine_;
    engine_core::DataModel& game_;
    std::shared_ptr<GuiInput> input_;
    std::unordered_map<engine_core::InstanceId, std::unique_ptr<Entry>> entries_;
    std::uint64_t pass_ = 0;
    std::vector<jadefx::Node*> shown_;
    // The service's CSS at the last sync, so it is parsed again only when it changes.
    std::string css_;
    // The root's GUID at the last sync. Another place starts the nodes over.
    std::string placeGuid_;
};

}  // namespace runner
