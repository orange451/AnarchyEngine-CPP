#pragma once

#include "types.hpp"

#include "jadefx/jadefx.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace engine_core {
class DataModel;
class Engine;
class GuiValues;
}

namespace runner {

// What the nodes tell their owner about the mouse on a GUI element: a Scene
// View hands it to the game as processed input. keepFocus is true when the
// element keeps the keyboard, as a TextField does. A call left empty is skipped.
struct GuiInput {
    std::function<void(const jadefx::MouseEvent&, bool keepFocus)> pressed;
    std::function<void(const jadefx::MouseEvent&)> released;
    std::function<void(const jadefx::MouseEvent&)> dragged;
    std::function<void(const jadefx::MouseEvent&)> moved;
};

// The JadeFX nodes for GUI instances, made once and updated in place: each
// GuiBase is a node of its class's element type (screengui, billboardgui,
// dockwidget, pane, imagepane, hbox, vbox, label, button, textfield) whose id
// is its Name and whose classes are its ClassList, and the CSS instances
// under one, joined in child order, are its stylesheet. A node keeps its
// state, such as a TextField's caret, while its instance does. Mouse events on
// a node fire its instance's events on the simulation thread, and typing in a
// TextField writes Text back.
//
// A pass reads the tree under the DataModel read lock: beginPass, build each
// root that is drawn, endPass, which lets go of what no build reached. Then,
// without the lock, updateImages gives each ImagePane the file its Texture's
// Path names under the resources folder resourcesRoot gave at beginPass, each
// file looked at again at most once a second.
class GuiTree {
public:
    GuiTree(engine_core::Engine& engine, std::shared_ptr<GuiInput> input,
            std::function<std::filesystem::path()> resourcesRoot);
    ~GuiTree();
    GuiTree(const GuiTree&) = delete;
    GuiTree& operator=(const GuiTree&) = delete;

    void beginPass();
    // The node for a GuiBase, made or brought up to date, with its children.
    std::shared_ptr<jadefx::Node> build(engine_core::InstanceId id, const engine_core::GuiValues& gui);
    void endPass();
    void updateImages();

    // The node for an instance, or null when the last pass did not build it.
    jadefx::Node* nodeFor(engine_core::InstanceId id) const;
    // Visible and MouseTransparent as the last build read them; false when not built.
    bool visible(engine_core::InstanceId id) const;
    bool mouseTransparent(engine_core::InstanceId id) const;
    // For a tree drawn as a studio pane: a Label whose TextColor is still its
    // default takes the studio theme's text color, as the studio's own labels do.
    void setThemedText(bool themed) { themedText_ = themed; }

private:
    struct Entry;

    bool themedText_ = false;

    std::shared_ptr<jadefx::Node> makeNode(engine_core::InstanceId id, const std::string& className);
    void apply(Entry& entry, const engine_core::GuiValues& gui);
    // Fires a GuiBase's event on the simulation thread.
    void fire(engine_core::InstanceId id, const char* event);
    // A TextField's typed text, written back to Text.
    void writeText(engine_core::InstanceId id, std::string text);
    // The decoded file at path under resourcesRoot_, upside down with flipY, or null.
    std::shared_ptr<jadefx::Image> loadImage(const std::string& path, bool flipY);

    // A file an ImagePane draws, as last read.
    struct LoadedImage {
        std::shared_ptr<jadefx::Image> image;
        std::filesystem::file_time_type stamp{};
        std::chrono::steady_clock::time_point checked{};
        bool tried = false;
        // updateImages' pass that last wanted it. One no pass wants is let go.
        std::uint64_t pass = 0;
    };

    engine_core::Engine& engine_;
    engine_core::DataModel& game_;
    std::shared_ptr<GuiInput> input_;
    std::function<std::filesystem::path()> resourcesRootFn_;
    std::unordered_map<engine_core::InstanceId, std::unique_ptr<Entry>> entries_;
    std::uint64_t pass_ = 0;
    // The resources folder at the last pass, and the files read from it, by Path.
    std::filesystem::path resourcesRoot_;
    // The folder images_ was read from.
    std::filesystem::path imagesRoot_;
    // Each by flipY: [0] upright, [1] flipped.
    std::unordered_map<std::string, LoadedImage> images_[2];
    std::uint64_t imagePass_ = 0;
};

}  // namespace runner
