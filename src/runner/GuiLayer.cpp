#include "GuiLayer.hpp"

#include "AssetInstances.hpp"
#include "BillboardMath.hpp"
#include "ChangeHistoryService.hpp"
#include "DataModelLock.hpp"
#include "Engine.hpp"
#include "Folder.hpp"
#include "Gui.hpp"
#include "SceneService.hpp"
#include "ScriptRuntime.hpp"
#include "TextureCache.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
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
    // An ImagePane's Image as its Texture's Path, empty for none, and the
    // opacity ImageTransparency gives it, as the last sync read them.
    bool imagePane = false;
    std::string imagePath;
    bool imageFlipY = false;
    float imageOpacity = 1.f;
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
    {
        // The simulation may be inside a step. Skip this frame rather than wait.
        engine_core::DataModelLock lock(game_, engine_core::DataModelLock::Read, std::chrono::milliseconds(1));
        if (!lock.owns()) {
            return;
        }
        syncTree();
    }
    updateImages();
}

void GuiLayer::syncTree() {
    ++pass_;
    resourcesRoot_ = game_.resources_root();
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
    std::vector<engine_core::InstanceId> boardIds;
    std::vector<std::shared_ptr<jadefx::Node>> boardNodes;
    collectBillboards(boardIds, boardNodes);
    // Each billboard keeps last frame's placement until placeBillboards runs.
    std::vector<Placement> placements;
    placements.reserve(boardNodes.size());
    for (std::size_t i = 0; i < boardNodes.size(); ++i) {
        Placement next;
        for (const Placement& was : placements_) {
            if (was.id == boardIds[i] && was.node == boardNodes[i]) {
                next = was;
                break;
            }
        }
        next.id = boardIds[i];
        next.node = boardNodes[i];
        placements.push_back(std::move(next));
    }
    placements_ = std::move(placements);
    screens_ = std::move(screens);
    restack();
    // The screens and billboards it places may have changed.
    markLayoutDirty(LayoutDirt::Arrange);
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

void GuiLayer::collectBillboards(std::vector<engine_core::InstanceId>& ids,
                                 std::vector<std::shared_ptr<jadefx::Node>>& nodes) {
    std::vector<engine_core::InstanceId> found;
    game_.billboards(found);
    std::sort(found.begin(), found.end());
    for (engine_core::InstanceId id : found) {
        const auto* board = dynamic_cast<const engine_core::BillboardGui*>(game_.instance(id));
        if (board != nullptr && board->drawn()) {
            ids.push_back(id);
            nodes.push_back(build(id, *board));
        }
    }
}

void GuiLayer::placeBillboards(const std::vector<engine_core::VisualBillboard>& rows, const BillboardView& view) {
    // Billboards move with the camera, so they are placed again.
    markLayoutDirty(LayoutDirt::Arrange);
    for (Placement& placement : placements_) {
        placement.placed = false;
        const engine_core::VisualBillboard* row = nullptr;
        for (const engine_core::VisualBillboard& candidate : rows) {
            if (candidate.id == placement.id) {
                row = &candidate;
                break;
            }
        }
        if (row != nullptr) {
            const BillboardPlacement where =
                PlaceBillboard(view.view, view.fovYDegrees, static_cast<float>(view.paneWidth),
                               static_cast<float>(view.paneHeight), row->anchor);
            placement.placed = where.visible;
            placement.alwaysOnTop = row->always_on_top;
            placement.x = view.paneX + where.x;
            placement.y = view.paneY + where.y;
            placement.pixelsPerUnit = where.pixelsPerUnit;
            placement.distance = where.distance;
            placement.drawn = PlacedBillboard{where.depth, row->always_on_top};
        }
        const auto found = entries_.find(placement.id);
        const bool shown = found != entries_.end() && found->second->visible;
        const bool userTransparent = found != entries_.end() && found->second->mouseTransparent;
        // The scene under the cursor is nearer: the mouse goes past this billboard.
        const bool behindScene = !placement.alwaysOnTop && cursorDepth_ && *cursorDepth_ < placement.drawn.depth;
        placement.node->setVisible(shown && placement.placed);
        placement.node->setMouseTransparent(!placement.placed || behindScene || userTransparent);
    }
    // The layer lays out after this in the same pass, from these placements.
    restack();
}

void GuiLayer::setCursorDepth(std::optional<float> depth) { cursorDepth_ = depth; }

void GuiLayer::restack() {
    std::vector<const Placement*> sorted;
    sorted.reserve(placements_.size());
    for (const Placement& placement : placements_) {
        sorted.push_back(&placement);
    }
    // Depth tested first, then on top; each far to near, ties by id so the order holds still.
    std::sort(sorted.begin(), sorted.end(), [](const Placement* a, const Placement* b) {
        if (a->alwaysOnTop != b->alwaysOnTop) {
            return !a->alwaysOnTop;
        }
        if (a->distance != b->distance) {
            return a->distance > b->distance;
        }
        return a->id < b->id;
    });
    order_.clear();
    order_.reserve(sorted.size() + screens_.size());
    for (const Placement* placement : sorted) {
        order_.push_back(placement->node);
    }
    for (const auto& screen : screens_) {
        order_.push_back(screen);
    }
    // A reorder alone leaves the list be: visitChildren paints and picks in order_.
    std::vector<jadefx::Node*> members;
    members.reserve(order_.size());
    for (const auto& node : order_) {
        members.push_back(node.get());
    }
    std::sort(members.begin(), members.end());
    if (members != members_) {
        // Only the nodes that leave or arrive are taken out or put in, so the
        // rest keep their focus and presses.
        getChildren().removeIf([&](const std::shared_ptr<jadefx::Node>& child) {
            return !std::binary_search(members.begin(), members.end(), child.get());
        });
        for (const auto& node : order_) {
            if (!std::binary_search(members_.begin(), members_.end(), node.get())) {
                getChildren().add(node);
            }
        }
        members_ = std::move(members);
    }
}

std::vector<jadefx::Node*> GuiLayer::paintOrder() const {
    std::vector<jadefx::Node*> order;
    order.reserve(order_.size());
    for (const auto& node : order_) {
        order.push_back(node.get());
    }
    return order;
}

void GuiLayer::visitChildren(const std::function<void(jadefx::Node*)>& visitor) {
    std::size_t visited = 0;
    for (const auto& node : order_) {
        if (node->getParent() == this) {
            visitor(node.get());
            ++visited;
        }
    }
    if (visited == getChildren().size()) {
        return;
    }
    for (const auto& child : getChildren().items()) {
        if (!child) {
            continue;
        }
        const bool ordered = std::find(order_.begin(), order_.end(), child) != order_.end();
        if (!ordered) {
            visitor(child.get());
        }
    }
}

void GuiLayer::renderChildren(jadefx::UiRenderer& renderer, float opacity) {
    std::vector<jadefx::Node*> children;
    visitChildren([&](jadefx::Node* child) { children.push_back(child); });
    jadefx::Painter painter(renderer);
    for (jadefx::Node* child : children) {
        const PlacedBillboard* board = placedFor(child);
        const bool hide = board != nullptr && !board->alwaysOnTop && sceneDepth_.texture != 0;
        if (hide) {
            painter.setOccluder(sceneDepth_.texture, sceneDepth_.x, sceneDepth_.y, sceneDepth_.width,
                                sceneDepth_.height, board->depth);
        }
        child->render(renderer, opacity);
        if (hide) {
            painter.clearOccluder();
        }
    }
}

const GuiLayer::PlacedBillboard* GuiLayer::placedFor(const jadefx::Node* node) const {
    for (const Placement& placement : placements_) {
        if (placement.node.get() == node && placement.placed) {
            return &placement.drawn;
        }
    }
    return nullptr;
}

std::shared_ptr<jadefx::Node> GuiLayer::makeNode(engine_core::InstanceId id, const std::string& className) {
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
            // A ScreenGui or BillboardGui inside another GUI is drawn by neither.
            if (entry.container != nullptr && dynamic_cast<const engine_core::ScreenGui*>(object) == nullptr &&
                dynamic_cast<const engine_core::BillboardGui*>(object) == nullptr) {
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
        label->setTextFill(NodeColor(gui.color(GuiProperty::TextColor)));
        label->setFont(jadefx::Font(jadefx::Font().family(), static_cast<float>(gui.number(GuiProperty::FontSize))));
        label->setTextScaled(gui.flag(GuiProperty::TextScaled));
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

void GuiLayer::updateImages() {
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

std::shared_ptr<jadefx::Image> GuiLayer::loadImage(const std::string& path, bool flipY) {
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

void GuiLayer::layoutChildren() {
    for (const auto& screen : screens_) {
        screen->performLayout(contentLeft(), contentTop(), contentWidth(), contentHeight());
    }
    // A billboard's available size is one world unit at its distance, so a
    // percentage on it is world units; its content and Size work as anywhere.
    // performLayout takes a place relative to this layer, and placements are
    // in window points.
    const double left = getAbsoluteX();
    const double top = getAbsoluteY();
    for (const Placement& placement : placements_) {
        if (!placement.placed) {
            continue;
        }
        const double unit = std::min(placement.pixelsPerUnit, 1.0e6);
        const double width = placement.node->measuredWidth(unit);
        const double height = placement.node->measuredHeight(width, unit);
        placement.node->performLayout(placement.x - left - width / 2.0, placement.y - top - height / 2.0, width,
                                      height);
    }
}

}  // namespace runner
