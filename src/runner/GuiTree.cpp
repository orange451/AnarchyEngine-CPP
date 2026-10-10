#include "GuiTree.hpp"

#include "AssetInstances.hpp"
#include "ChangeHistoryService.hpp"
#include "Engine.hpp"
#include "Gui.hpp"
#include "ScriptRuntime.hpp"
#include "TextureCache.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
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

bool IsPaneClass(const std::string& name) {
    return name == "Pane" || name == "ImagePane" || name == "HBox" || name == "VBox";
}

// How often a file an ImagePane draws is looked at again.
constexpr std::chrono::seconds kImageRecheck{1};

// The image file decoded upside down, as a Texture's FlipY draws it, or null.
// DecodeTexture gives the bottom row first, and fromRgba takes its first row
// as the top.
std::shared_ptr<jadefx::Image> loadFlipped(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    TexturePixels pixels;
    std::string why;
    if (!DecodeTexture(bytes.data(), bytes.size(), pixels, why)) {
        return nullptr;
    }
    return jadefx::Image::fromRgba(pixels.width, pixels.height, std::move(pixels.rgba));
}

}  // namespace

struct GuiTree::Entry {
    std::shared_ptr<jadefx::Node> node;
    // The node's children list; null for a Label, Button, or TextField.
    jadefx::Pane* container = nullptr;
    jadefx::TextField* field = nullptr;
    jadefx::Slider* slider = nullptr;
    jadefx::Button* assetButton = nullptr;
    std::string className;
    // The instance's revision at the last apply. 0 forces one.
    std::uint64_t revision = 0;
    std::string name;
    std::string css;
    std::vector<jadefx::Node*> children;
    std::uint64_t pass = 0;
    // Visible and MouseTransparent as the instance last had them, which a
    // billboard's placement combines with its own.
    bool visible = true;
    bool mouseTransparent = false;
    // Whether Size has given the node a preferred width or height. JadeFX
    // cannot unset one, so going back to 0 makes the node again.
    bool prefWidth = false;
    bool prefHeight = false;
    // A TextField's Text as the instance last had it, and as the field last
    // showed or wrote it.
    std::string instanceText;
    std::string fieldText;
    // A Slider's Value as the instance last had it, and as the slider last
    // showed or wrote it.
    double instanceValue = 0;
    double sliderValue = 0;
    // An AssetPicker's class, Value, and the name shown, as the last build read them.
    std::string assetClass;
    engine_core::InstanceId asset = 0;
    std::string assetText;
    // An ImagePane's Image as its Texture's Path, empty for none, and the
    // opacity ImageTransparency gives it, as the last sync read them.
    bool imagePane = false;
    std::string imagePath;
    bool imageFlipY = false;
    float imageOpacity = 1.f;
};

GuiTree::GuiTree(engine_core::Engine& engine, std::shared_ptr<GuiInput> input,
                 std::function<std::filesystem::path()> resourcesRoot)
    : engine_(engine), game_(engine.datamodel()), input_(std::move(input)), resourcesRootFn_(std::move(resourcesRoot)) {}

GuiTree::~GuiTree() = default;

void GuiTree::beginPass() {
    ++pass_;
    resourcesRoot_ = resourcesRootFn_ ? resourcesRootFn_() : std::filesystem::path();
}

void GuiTree::endPass() {
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

jadefx::Node* GuiTree::nodeFor(engine_core::InstanceId id) const {
    const auto found = entries_.find(id);
    return found != entries_.end() ? found->second->node.get() : nullptr;
}

bool GuiTree::visible(engine_core::InstanceId id) const {
    const auto found = entries_.find(id);
    return found != entries_.end() && found->second->visible;
}

bool GuiTree::mouseTransparent(engine_core::InstanceId id) const {
    const auto found = entries_.find(id);
    return found != entries_.end() && found->second->mouseTransparent;
}

std::shared_ptr<jadefx::Node> GuiTree::makeNode(engine_core::InstanceId id, const std::string& className) {
    std::shared_ptr<jadefx::Node> node;
    const std::shared_ptr<GuiInput>& input = input_;
    if (className == "ScreenGui") {
        node = std::make_shared<GuiNode<jadefx::StackPane>>("screengui", input, false);
        // Its own area is the scene's: only what is in it takes the mouse.
        node->setPickOnBounds(false);
    } else if (className == "BillboardGui") {
        node = std::make_shared<GuiNode<jadefx::StackPane>>("billboardgui", input, false);
        // Like a ScreenGui, only what is in it takes the mouse.
        node->setPickOnBounds(false);
    } else if (className == "DockWidget") {
        node = std::make_shared<GuiNode<jadefx::StackPane>>("dockwidget", input, false);
    } else if (className == "Pane") {
        node = std::make_shared<GuiNode<jadefx::StackPane>>("pane", input, false);
    } else if (className == "ImagePane") {
        node = std::make_shared<GuiNode<jadefx::StackPane>>("imagepane", input, false);
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
    } else if (className == "AssetPicker") {
        auto button = std::make_shared<GuiNode<jadefx::Button>>("assetpicker", input, false, std::string());
        jadefx::Button* raw = button.get();
        button->setOnAction([this, id, raw](jadefx::ActionEvent&) {
            const auto found = entries_.find(id);
            if (assetPicking_ && found != entries_.end()) {
                assetPicking_(*raw, id, found->second->assetClass, found->second->asset);
            }
        });
        node = button;
    } else if (className == "Slider") {
        node = std::make_shared<GuiNode<jadefx::Slider>>("slider", input, true, 0.0, 1.0, 0.0);
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

std::shared_ptr<jadefx::Node> GuiTree::build(engine_core::InstanceId id, const engine_core::GuiValues& gui) {
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
        entry.slider = dynamic_cast<jadefx::Slider*>(entry.node.get());
        entry.assetButton = className == "AssetPicker" ? dynamic_cast<jadefx::Button*>(entry.node.get()) : nullptr;
        entry.className = className;
    }
    entry.pass = pass_;
    if (entry.field != nullptr && entry.revision != 0 && entry.field->getText() != entry.fieldText) {
        entry.fieldText = entry.field->getText();
        writeText(id, entry.fieldText);
    }
    if (entry.slider != nullptr && entry.revision != 0 && entry.slider->getValue() != entry.sliderValue) {
        entry.sliderValue = entry.slider->getValue();
        writeValue(id, entry.sliderValue);
    }
    if (gui.revision() != entry.revision) {
        apply(entry, gui);
        entry.revision = gui.revision();
    }
    // Read at every sync, since the asset's Name can change without the picker's revision moving.
    if (const auto* picker = dynamic_cast<const engine_core::AssetPicker*>(&gui)) {
        entry.assetClass = picker->asset_class();
        entry.asset = picker->asset_id();
        std::string text = entry.asset != 0 ? game_.name(entry.asset) : std::string("None");
        if (text != entry.assetText && entry.assetButton != nullptr) {
            entry.assetButton->setText(text);
            entry.assetText = std::move(text);
        }
    }
    // Read at every sync, since the Texture's Path can change without the
    // ImagePane's revision moving.
    if (const auto* imagePane = dynamic_cast<const engine_core::ImagePane*>(&gui)) {
        const engine_core::Texture* texture = imagePane->image_texture();
        entry.imagePane = true;
        entry.imagePath = texture != nullptr ? texture->path() : std::string();
        entry.imageFlipY = texture != nullptr && texture->flip_y();
        entry.imageOpacity = 1.f - static_cast<float>(gui.number(GuiProperty::ImageTransparency));
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
            const bool screen = dynamic_cast<const engine_core::ScreenGui*>(object) != nullptr;
            const bool board = dynamic_cast<const engine_core::BillboardGui*>(object) != nullptr;
            // A ScreenGui or BillboardGui inside another GUI is drawn by neither, except a
            // ScreenGui directly in a DockWidget, which fills the pane as it fills the view.
            if (entry.container != nullptr && !board && (!screen || entry.className == "DockWidget")) {
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

void GuiTree::apply(Entry& entry, const engine_core::GuiValues& gui) {
    jadefx::Node& node = *entry.node;
    entry.visible = gui.flag(GuiProperty::Visible);
    entry.mouseTransparent = gui.flag(GuiProperty::MouseTransparent);
    node.setVisible(entry.visible);
    node.setMouseTransparent(entry.mouseTransparent);
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
        const engine_core::ColorRgb fill = gui.color(GuiProperty::TextColor);
        const engine_core::ColorRgb unset = engine_core::GuiValues::default_value(GuiProperty::TextColor, "Label").color;
        if (!themedText_ || fill.r != unset.r || fill.g != unset.g || fill.b != unset.b) {
            label->setTextFill(NodeColor(fill));
        }
        label->setFont(jadefx::Font(jadefx::Font().family(), static_cast<float>(gui.number(GuiProperty::FontSize))));
        label->setTextScaled(gui.flag(GuiProperty::TextScaled));
    } else if (entry.assetButton != nullptr) {
        // Its text is the picked asset's name, which build reads each pass.
    } else if (auto* button = dynamic_cast<jadefx::Button*>(&node)) {
        button->setText(gui.text(GuiProperty::Text));
        button->setTextScaled(gui.flag(GuiProperty::TextScaled));
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
    } else if (entry.slider != nullptr) {
        jadefx::Slider& slider = *entry.slider;
        const double step = gui.number(GuiProperty::Step);
        slider.setMin(gui.number(GuiProperty::Min));
        slider.setMax(gui.number(GuiProperty::Max));
        // The instance snaps what the slider writes; the keys move a Step, or a tenth.
        slider.setBlockIncrement(step > 0 ? step : (slider.getMax() - slider.getMin()) / 10);
        const double value = gui.number(GuiProperty::Value);
        if (value != entry.instanceValue || entry.revision == 0) {
            entry.instanceValue = value;
            slider.setValue(value);
            entry.sliderValue = slider.getValue();
        }
    }
}

void GuiTree::fire(engine_core::InstanceId id, const char* event) {
    engine_.on_simulation([id, event](engine_core::DataModel& game) {
        if (game.alive(id)) {
            game.fire_event(id, event);
        }
    });
}

void GuiTree::writeText(engine_core::InstanceId id, std::string text) {
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

void GuiTree::writeValue(engine_core::InstanceId id, double value) {
    engine_.on_simulation([id, value](engine_core::DataModel& game) {
        auto* gui = dynamic_cast<engine_core::GuiValues*>(game.instance(id));
        if (gui == nullptr || gui->number(GuiProperty::Value) == value) {
            return;
        }
        std::optional<std::string> recording;
        if (!game.simulation_running()) {
            recording = game.history().try_begin_recording("Move Slider");
        }
        engine_core::LuaSlot slot;
        slot.kind = engine_core::LuaSlot::Kind::Number;
        slot.number = value;
        gui->set_value(GuiProperty::Value, slot);
        if (recording) {
            game.history().finish_recording(*recording, engine_core::FinishRecordingOperation::Commit);
        }
    });
}

void GuiTree::pickAsset(engine_core::InstanceId picker, engine_core::InstanceId asset) {
    engine_.on_simulation([picker, asset](engine_core::DataModel& game) {
        auto* gui = dynamic_cast<engine_core::AssetPicker*>(game.instance(picker));
        if (gui == nullptr || gui->asset_id() == asset) {
            return;
        }
        std::optional<std::string> recording;
        if (!game.simulation_running()) {
            recording = game.history().try_begin_recording("Pick Asset");
        }
        engine_core::LuaSlot slot;
        if (asset != 0) {
            slot.kind = engine_core::LuaSlot::Kind::Instance;
            slot.id = asset;
        }
        gui->set_asset(slot);
        if (recording) {
            game.history().finish_recording(*recording, engine_core::FinishRecordingOperation::Commit);
        }
    });
}

void GuiTree::updateImages() {
    ++imagePass_;
    if (resourcesRoot_ != imagesRoot_) {
        for (auto& images : images_) {
            images.clear();
        }
        imagesRoot_ = resourcesRoot_;
    }
    for (const auto& [id, entry] : entries_) {
        if (!entry->imagePane) {
            continue;
        }
        std::shared_ptr<jadefx::Image> image =
            entry->imagePath.empty() ? nullptr : loadImage(entry->imagePath, entry->imageFlipY);
        jadefx::Node& node = *entry->node;
        if (image != node.getBackgroundImage() || entry->imageOpacity != node.getBackgroundImageOpacity()) {
            node.setBackgroundImage(std::move(image), entry->imageOpacity);
        }
    }
    for (auto& images : images_) {
        for (auto it = images.begin(); it != images.end();) {
            it = it->second.pass == imagePass_ ? std::next(it) : images.erase(it);
        }
    }
}

std::shared_ptr<jadefx::Image> GuiTree::loadImage(const std::string& path, bool flipY) {
    if (imagesRoot_.empty()) {
        return nullptr;
    }
    LoadedImage& loaded = images_[flipY ? 1 : 0][path];
    loaded.pass = imagePass_;
    const auto now = std::chrono::steady_clock::now();
    if (loaded.tried && now - loaded.checked < kImageRecheck) {
        return loaded.image;
    }
    loaded.checked = now;
    auto report = [this](const std::string& message) {
        engine_.scripts().append_output(engine_core::ScriptRuntime::OutputKind::Error, message);
    };
    // Texture Paths use '/', which every platform's path splits on.
    const std::filesystem::path file = imagesRoot_ / std::filesystem::u8path(path);
    std::error_code error;
    const std::filesystem::file_time_type stamp = std::filesystem::last_write_time(file, error);
    if (error) {
        const bool wasThere = !loaded.tried || loaded.image != nullptr || loaded.stamp != std::filesystem::file_time_type{};
        loaded.tried = true;
        loaded.stamp = {};
        loaded.image = nullptr;
        if (wasThere) {
            report("Texture " + path + " was not found in the resources folder");
        }
        return nullptr;
    }
    if (loaded.tried && stamp == loaded.stamp) {
        return loaded.image;
    }
    loaded.tried = true;
    loaded.stamp = stamp;
    loaded.image = flipY ? loadFlipped(file) : jadefx::Image::load(file.u8string());
    if (loaded.image == nullptr) {
        report("Texture " + path + " is not an image an ImagePane can draw");
    }
    return loaded.image;
}

}  // namespace runner
