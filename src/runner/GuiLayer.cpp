#include "GuiLayer.hpp"

#include "ChangeHistoryService.hpp"
#include "DataModelLock.hpp"
#include "Engine.hpp"
#include "Folder.hpp"
#include "Gui.hpp"
#include "SceneService.hpp"

#include <chrono>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace runner {
namespace {

using engine_core::GuiProperty;

// A JadeFX node standing for one GUI instance: its class's element type, so a
// stylesheet's type selectors match it, and the mouse handed to the Scene
// View, as processed input, as well as to the control itself.
template <typename Base>
class GuiNode final : public Base {
public:
    template <typename... Args>
    GuiNode(const char* type, std::shared_ptr<GuiInput> input, bool keepFocus, Args&&... args)
        : Base(std::forward<Args>(args)...), type_(type), input_(std::move(input)), keepFocus_(keepFocus) {}

    const char* getElementType() const override { return type_; }

protected:
    void handleMousePressed(const jadefx::MouseEvent& event) override {
        Base::handleMousePressed(event);
        if (input_->pressed) {
            input_->pressed(event, keepFocus_);
        }
    }
    void handleMouseReleased(const jadefx::MouseEvent& event) override {
        Base::handleMouseReleased(event);
        if (input_->released) {
            input_->released(event);
        }
    }
    void handleMouseDragged(const jadefx::MouseEvent& event) override {
        Base::handleMouseDragged(event);
        if (input_->dragged) {
            input_->dragged(event);
        }
    }
    void handleMouseMoved(const jadefx::MouseEvent& event) override {
        Base::handleMouseMoved(event);
        if (input_->moved) {
            input_->moved(event);
        }
    }

private:
    const char* type_;
    std::shared_ptr<GuiInput> input_;
    bool keepFocus_;
};

// ClassList's classes: the words between spaces.
std::vector<std::string> SplitClasses(const std::string& list) {
    std::vector<std::string> out;
    std::istringstream words(list);
    std::string word;
    while (words >> word) {
        out.push_back(word);
    }
    return out;
}

jadefx::Pos AlignmentPos(double value) {
    // Enum.GuiAlignment's items are jadefx::Pos's first nine, in the same order.
    const int item = static_cast<int>(value);
    return item >= 0 && item <= static_cast<int>(jadefx::Pos::BottomRight) ? static_cast<jadefx::Pos>(item)
                                                                           : jadefx::Pos::TopLeft;
}

jadefx::Color NodeColor(engine_core::ColorRgb color, float alpha = 1.f) {
    return jadefx::Color{color.r, color.g, color.b, alpha};
}

bool IsPaneClass(const std::string& name) { return name == "Pane" || name == "HBox" || name == "VBox"; }

}  // namespace

struct GuiLayer::Entry {
    std::shared_ptr<jadefx::Node> node;
    // The node's children list; null for a Label, Button, or TextField.
    jadefx::Pane* container = nullptr;
    jadefx::TextField* field = nullptr;
    std::string className;
    // The instance's revision at the last apply. 0 forces one.
    std::uint64_t revision = 0;
    std::string name;
    std::string css;
    std::vector<jadefx::Node*> children;
    std::uint64_t pass = 0;
    // Whether Size has given the node a preferred width or height. JadeFX
    // cannot unset one, so going back to 0 makes the node again.
    bool prefWidth = false;
    bool prefHeight = false;
    // A TextField's Text as the instance last had it, and as the field last
    // showed or wrote it.
    std::string instanceText;
    std::string fieldText;
};

const char* GuiLayer::defaultStylesheet() {
    return R"CSS(
/* The game UI's starting point: no outlines, backgrounds, or padding.
   Hover, press, and focus feedback stay, colored by the variables below. */
:root {
    --text-color: #000000;
    --border-color: transparent;
    --surface-color: transparent;
    --accent-color: #1a73e8;
    --outline-color: var(--accent-color);
    --wash-color: rgba(0, 0, 0, 0.04);
    --text-selection-color: rgba(26, 115, 232, 0.3);
    color: var(--text-color);
}
button, textfield {
    padding: 0;
}
)CSS";
}

GuiLayer::GuiLayer(engine_core::Engine& engine, GuiInput input)
    : engine_(engine), game_(engine.datamodel()), input_(std::make_shared<GuiInput>(std::move(input))) {
    setMinSize(0, 0);
    setPickOnBounds(false);
}

GuiLayer::~GuiLayer() = default;

jadefx::Node* GuiLayer::nodeFor(engine_core::InstanceId id) const {
    const auto found = entries_.find(id);
    return found != entries_.end() ? found->second->node.get() : nullptr;
}

void GuiLayer::sync() {
    // The simulation may be inside a step. Skip this frame rather than wait.
    engine_core::DataModelLock lock(game_, engine_core::DataModelLock::Read, std::chrono::milliseconds(1));
    if (!lock.owns()) {
        return;
    }
    ++pass_;
    std::vector<std::shared_ptr<jadefx::Node>> screens;
    std::string css;
    if (const engine_core::InstanceId service = game_.scene_service("Gui"); service != 0) {
        collectScreens(service, screens);
        // The CSS instances under the service style every ScreenGui.
        for (engine_core::InstanceId child = game_.first_child(service); child != 0; child = game_.next_sibling(child)) {
            if (const auto* sheet = dynamic_cast<const engine_core::Css*>(game_.instance(child))) {
                css += sheet->source();
                css += '\n';
            }
        }
    }
    if (css != css_) {
        setStylesheet(css);
        css_ = std::move(css);
    }
    std::vector<jadefx::Node*> shown;
    shown.reserve(screens.size());
    for (const auto& screen : screens) {
        shown.push_back(screen.get());
    }
    if (shown != shown_) {
        getChildren().setAll(std::move(screens));
        shown_ = std::move(shown);
    }
    // What is no longer drawn lets go of its children and goes.
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second->pass == pass_) {
            ++it;
            continue;
        }
        if (it->second->container != nullptr) {
            it->second->container->getChildren().clear();
        }
        it = entries_.erase(it);
    }
}

void GuiLayer::collectScreens(engine_core::InstanceId id, std::vector<std::shared_ptr<jadefx::Node>>& out) {
    for (engine_core::InstanceId child = game_.first_child(id); child != 0; child = game_.next_sibling(child)) {
        const engine_core::DataModel* object = game_.instance(child);
        if (const auto* screen = dynamic_cast<const engine_core::ScreenGui*>(object)) {
            out.push_back(build(child, *screen));
        } else if (dynamic_cast<const engine_core::Folder*>(object) != nullptr) {
            collectScreens(child, out);
        }
    }
}

std::shared_ptr<jadefx::Node> GuiLayer::makeNode(engine_core::InstanceId id, const std::string& className) {
    std::shared_ptr<jadefx::Node> node;
    const std::shared_ptr<GuiInput>& input = input_;
    if (className == "ScreenGui") {
        node = std::make_shared<GuiNode<jadefx::StackPane>>("screengui", input, false);
        // Its own area is the scene's: only what is in it takes the mouse.
        node->setPickOnBounds(false);
    } else if (className == "Pane") {
        node = std::make_shared<GuiNode<jadefx::StackPane>>("pane", input, false);
    } else if (className == "HBox") {
        node = std::make_shared<GuiNode<jadefx::HBox>>("hbox", input, false);
    } else if (className == "VBox") {
        node = std::make_shared<GuiNode<jadefx::VBox>>("vbox", input, false);
    } else if (className == "Label") {
        node = std::make_shared<GuiNode<jadefx::Label>>("label", input, false, std::string());
    } else if (className == "Button") {
        auto button = std::make_shared<GuiNode<jadefx::Button>>("button", input, false, std::string());
        button->setOnAction([this, id](jadefx::ActionEvent&) { fire(id, engine_core::kGuiAction); });
        node = button;
    } else if (className == "TextField") {
        auto field = std::make_shared<GuiNode<jadefx::TextField>>("textfield", input, true);
        field->setOnAction([this, id](jadefx::ActionEvent&) { fire(id, engine_core::kGuiAction); });
        node = field;
    } else {
        return nullptr;
    }
    // Each node fires its own instance's events; a press or click is heard by
    // every node it is inside, as in the DOM.
    node->setOnMousePressed([this, id](const jadefx::MouseEvent&) { fire(id, engine_core::kGuiMousePressed); });
    node->setOnMouseReleased([this, id](const jadefx::MouseEvent&) { fire(id, engine_core::kGuiMouseReleased); });
    node->setOnMouseClicked([this, id](const jadefx::MouseEvent&) { fire(id, engine_core::kGuiMouseClicked); });
    node->setOnMouseEntered([this, id](const jadefx::MouseEvent&) { fire(id, engine_core::kGuiMouseEntered); });
    node->setOnMouseExited([this, id](const jadefx::MouseEvent&) { fire(id, engine_core::kGuiMouseExited); });
    return node;
}

std::shared_ptr<jadefx::Node> GuiLayer::build(engine_core::InstanceId id, const engine_core::GuiValues& gui) {
    std::unique_ptr<Entry>& slot = entries_[id];
    if (!slot) {
        slot = std::make_unique<Entry>();
    }
    Entry& entry = *slot;
    const std::string className = gui.class_name();
    const engine_core::Vec2 size = gui.vec2(GuiProperty::Size);
    const bool remake = !entry.node || entry.className != className || (entry.prefWidth && size.x <= 0.f) ||
                        (entry.prefHeight && size.y <= 0.f);
    if (remake) {
        if (entry.container != nullptr) {
            entry.container->getChildren().clear();
        }
        entry = Entry{};
        entry.node = makeNode(id, className);
        entry.container = dynamic_cast<jadefx::Pane*>(entry.node.get());
        entry.field = dynamic_cast<jadefx::TextField*>(entry.node.get());
        entry.className = className;
    }
    entry.pass = pass_;
    if (entry.field != nullptr && entry.revision != 0 && entry.field->getText() != entry.fieldText) {
        entry.fieldText = entry.field->getText();
        writeText(id, entry.fieldText);
    }
    if (gui.revision() != entry.revision) {
        apply(entry, gui);
        entry.revision = gui.revision();
    }
    std::string name = game_.name(id);
    if (name != entry.name) {
        entry.node->setElementId(name);
        entry.name = std::move(name);
    }

    std::string css;
    std::vector<std::shared_ptr<jadefx::Node>> children;
    for (engine_core::InstanceId child = game_.first_child(id); child != 0; child = game_.next_sibling(child)) {
        const engine_core::DataModel* object = game_.instance(child);
        if (const auto* sheet = dynamic_cast<const engine_core::Css*>(object)) {
            css += sheet->source();
            css += '\n';
        } else if (const auto* inner = dynamic_cast<const engine_core::GuiBase*>(object)) {
            // A ScreenGui inside another is drawn by neither.
            if (entry.container != nullptr && dynamic_cast<const engine_core::ScreenGui*>(object) == nullptr) {
                children.push_back(build(child, *inner));
            }
        }
    }
    if (css != entry.css) {
        entry.node->setStylesheet(css);
        entry.css = std::move(css);
    }
    if (entry.container != nullptr) {
        std::vector<jadefx::Node*> raw;
        raw.reserve(children.size());
        for (const auto& child : children) {
            raw.push_back(child.get());
        }
        if (raw != entry.children) {
            entry.container->getChildren().setAll(std::move(children));
            entry.children = std::move(raw);
        }
    }
    return entry.node;
}

void GuiLayer::apply(Entry& entry, const engine_core::GuiValues& gui) {
    jadefx::Node& node = *entry.node;
    node.setVisible(gui.flag(GuiProperty::Visible));
    node.setMouseTransparent(gui.flag(GuiProperty::MouseTransparent));
    node.setAlignment(AlignmentPos(gui.number(GuiProperty::Alignment)));
    if (node.getStyle() != gui.text(GuiProperty::Style)) {
        node.setStyle(gui.text(GuiProperty::Style));
    }
    node.getClassList().setAll(SplitClasses(gui.text(GuiProperty::ClassList)));
    // A ScreenGui fills the view whatever its Size.
    if (entry.className != "ScreenGui") {
        const engine_core::Vec2 size = gui.vec2(GuiProperty::Size);
        if (size.x > 0.f) {
            node.setPrefWidth(size.x);
            entry.prefWidth = true;
        }
        if (size.y > 0.f) {
            node.setPrefHeight(size.y);
            entry.prefHeight = true;
        }
    }
    if (IsPaneClass(entry.className)) {
        const float opacity = 1.f - static_cast<float>(gui.number(GuiProperty::BackgroundTransparency));
        node.setBackground(NodeColor(gui.color(GuiProperty::BackgroundColor), opacity));
    }
    if (auto* box = dynamic_cast<jadefx::DirectionalBox*>(&node)) {
        box->setSpacing(gui.number(GuiProperty::Spacing));
    }
    if (auto* label = dynamic_cast<jadefx::Label*>(&node)) {
        label->setText(gui.text(GuiProperty::Text));
        label->setTextFill(NodeColor(gui.color(GuiProperty::TextColor)));
        label->setFont(jadefx::Font(jadefx::Font().family(), static_cast<float>(gui.number(GuiProperty::FontSize))));
    } else if (auto* button = dynamic_cast<jadefx::Button*>(&node)) {
        button->setText(gui.text(GuiProperty::Text));
    } else if (entry.field != nullptr) {
        entry.field->setPromptText(gui.text(GuiProperty::Prompt));
        // Only a change on the instance's side reaches the field, so text
        // typed while it was on its way to the instance is not put back.
        const std::string& text = gui.text(GuiProperty::Text);
        if (text != entry.instanceText) {
            entry.instanceText = text;
            if (entry.field->getText() != text) {
                entry.field->setText(text);
            }
            entry.fieldText = text;
        }
    }
}

void GuiLayer::fire(engine_core::InstanceId id, const char* event) {
    engine_.on_simulation([id, event](engine_core::DataModel& game) {
        if (game.alive(id)) {
            game.fire_event(id, event);
        }
    });
}

void GuiLayer::writeText(engine_core::InstanceId id, std::string text) {
    engine_.on_simulation([id, text = std::move(text)](engine_core::DataModel& game) {
        auto* gui = dynamic_cast<engine_core::GuiValues*>(game.instance(id));
        if (gui == nullptr || gui->text(GuiProperty::Text) == text) {
            return;
        }
        // Typing while stopped is an edit, one undo step at a time; in play it is play.
        std::optional<std::string> recording;
        if (!game.simulation_running()) {
            recording = game.history().try_begin_recording("Type Text");
        }
        gui->set_text(GuiProperty::Text, text);
        if (recording) {
            game.history().finish_recording(*recording, engine_core::FinishRecordingOperation::Commit);
        }
    });
}

void GuiLayer::layoutChildren() {
    for (jadefx::Node* screen : shown_) {
        screen->performLayout(contentLeft(), contentTop(), contentWidth(), contentHeight());
    }
}

}  // namespace runner
