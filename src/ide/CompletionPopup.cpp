#include "CompletionPopup.hpp"

#include "DataModelLock.hpp"
#include "Engine.hpp"
#include "Script.hpp"

#include "jadefx/jadefx.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace ide {
namespace {

constexpr std::chrono::milliseconds kLockWait(5);

int CodePoints(std::string_view text) {
    int count = 0;
    for (std::size_t index = 0; index < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        std::size_t step = 1;
        if ((lead & 0xE0) == 0xC0) {
            step = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            step = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            step = 4;
        }
        if (index + step > text.size()) {
            step = 1;
        }
        index += step;
        ++count;
    }
    return count;
}

char32_t CodeAt(std::string_view text, int index) {
    int count = 0;
    for (std::size_t cursor = 0; cursor < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[cursor]);
        char32_t code = lead;
        std::size_t step = 1;
        if (lead >= 0x80) {
            step = lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
            code = 0;
        }
        if (count == index) {
            return code;
        }
        ++count;
        if (cursor + step > text.size()) {
            break;
        }
        cursor += step;
    }
    return 0;
}

}  // namespace

struct CompletionPopup::State {
    std::shared_ptr<jadefx::VBox> popup;
    std::vector<CompletionItem> items;
    CompleteSite site = CompleteSite::None;
    int selected = 0;
    int replace_begin = 0;
    int replace_end = 0;
    std::string prefix;
    char close_quote = 0;
    bool unclosed = false;
    bool picked = false;
    bool accepting = false;
    jadefx::Node* owner = nullptr;
    double caret_x = 0;
    double caret_y = 0;
    double caret_height = 0;
    std::function<void()> on_accept;
};

CompletionPopup::CompletionPopup() : state_(std::make_unique<State>()) {}

CompletionPopup::~CompletionPopup() { dismiss(); }

void CompletionPopup::setOnAccept(std::function<void()> handler) { state_->on_accept = std::move(handler); }

bool CompletionPopup::isOpen() const {
    return state_->popup && state_->owner != nullptr && state_->owner->getScene() != nullptr &&
           state_->owner->getScene()->isPopupShowing(state_->popup.get());
}

const CompletionItem* CompletionPopup::highlighted() const {
    if (!isOpen() || state_->items.empty()) {
        return nullptr;
    }
    int index = state_->selected;
    const int count = static_cast<int>(state_->items.size());
    if (index < 0) {
        index = 0;
    }
    if (index >= count) {
        index = count - 1;
    }
    return &state_->items[static_cast<std::size_t>(index)];
}

bool CompletionPopup::commitsName() const {
    const CompletionItem* item = highlighted();
    if (item == nullptr || state_->site != CompleteSite::Name) {
        return false;
    }
    if (item->name == state_->prefix) {
        return false;
    }
    return state_->picked || state_->items.size() == 1;
}

bool CompletionPopup::keyAccepts() const {
    const CompletionItem* item = highlighted();
    if (item == nullptr) {
        return false;
    }
    if (state_->site != CompleteSite::Name) {
        return true;
    }
    return item->name != state_->prefix;
}

bool CompletionPopup::commitsQuote(char quote) const {
    const CompletionItem* item = highlighted();
    if (item == nullptr || state_->site != CompleteSite::Argument || !state_->unclosed) {
        return false;
    }
    if (quote != state_->close_quote) {
        return false;
    }
    if (item->name == state_->prefix) {
        return false;
    }
    return state_->picked || state_->items.size() == 1;
}

bool CompletionPopup::accepting() const { return state_->accepting; }

int CompletionPopup::replaceEnd() const { return state_->replace_end; }

void CompletionPopup::dismiss() {
    if (!state_) {
        return;
    }
    state_->picked = false;
    state_->prefix.clear();
    state_->items.clear();
    state_->close_quote = 0;
    state_->unclosed = false;
    if (state_->popup && state_->owner != nullptr && state_->owner->getScene() != nullptr &&
        !state_->owner->getScene()->isTearingDown() && state_->owner->getScene()->isPopupShowing(state_->popup.get())) {
        state_->owner->getScene()->hidePopup(state_->popup.get());
    }
}

void CompletionPopup::move(int delta) {
    if (!isOpen() || state_->items.empty()) {
        return;
    }
    const int count = static_cast<int>(state_->items.size());
    int next = state_->selected + delta;
    if (next < 0) {
        next = 0;
    }
    if (next >= count) {
        next = count - 1;
    }
    state_->selected = next;
    state_->picked = true;
    fill();
    if (state_->owner != nullptr) {
        place(*state_->owner, state_->caret_x, state_->caret_y, state_->caret_height);
    }
}

std::optional<CompletionEdit> CompletionPopup::take(bool parentheses, std::string_view text) {
    const CompletionItem* item = highlighted();
    if (item == nullptr) {
        dismiss();
        return std::nullopt;
    }
    const CompletionItem chosen = *item;
    const int begin = state_->replace_begin;
    const int end = state_->replace_end;
    CompletionEdit edit;
    edit.begin = begin;
    edit.end = end;
    edit.text = chosen.name;
    edit.caret = begin + CodePoints(chosen.name);
    if (parentheses && chosen.call && CodeAt(text, end) != U'(') {
        edit.text += "()";
        edit.caret = begin + CodePoints(chosen.name) + 1;
    }
    if (state_->close_quote != 0 && !chosen.call) {
        const auto quote = static_cast<char32_t>(static_cast<unsigned char>(state_->close_quote));
        if (CodeAt(text, end) == quote) {
            edit.caret = begin + CodePoints(edit.text) + 1;
        } else if (parentheses) {
            edit.text.push_back(state_->close_quote);
            edit.caret = begin + CodePoints(edit.text);
        }
    }
    state_->accepting = true;
    dismiss();
    return edit;
}

void CompletionPopup::finish() { state_->accepting = false; }

void CompletionPopup::fill() {
    if (!state_->popup) {
        state_->popup = jadefx::make<jadefx::VBox>();
        state_->popup->setSpacing(0);
        state_->popup->setPrefWidth(420);
        state_->popup->setStyle(
            "background-color: #ffffff; border-style: solid; border-width: 1px; border-color: #c5c8ce; "
            "box-shadow: 0 2px 8px rgba(32, 33, 36, 0.16); padding: 4px;");
    }
    constexpr int kVisible = 8;
    const int count = static_cast<int>(state_->items.size());
    int start = 0;
    if (state_->selected >= kVisible) {
        start = state_->selected - kVisible + 1;
    }
    const int max_start = std::max(0, count - kVisible);
    if (start > max_start) {
        start = max_start;
    }
    state_->popup->getChildren().clear();
    for (int index = start; index < count && index < start + kVisible; ++index) {
        const CompletionItem& item = state_->items[static_cast<std::size_t>(index)];
        auto row = jadefx::make<jadefx::HBox>();
        row->setSpacing(16);
        row->setPadding(jadefx::Insets{3, 8, 3, 8});
        row->setPrefWidth(412);
        row->setAlignment(jadefx::Pos::CenterLeft);
        if (index == state_->selected) {
            row->setBackground(jadefx::Color::parse("#d6e4f5"));
        }
        auto name = jadefx::make<jadefx::Label>(item.name);
        auto detail = jadefx::make<jadefx::Label>(item.detail);
        detail->setTextFill(jadefx::Color::parse("#5c6570"));
        row->getChildren().add(name);
        row->getChildren().add(detail);
        auto arm = [this, index](jadefx::Node& node) {
            node.setOnMousePressed([this, index](const jadefx::MouseEvent&) {
                state_->selected = index;
                if (state_->on_accept) {
                    state_->on_accept();
                }
            });
        };
        arm(*row);
        arm(*name);
        arm(*detail);
        state_->popup->getChildren().add(std::move(row));
    }
}

void CompletionPopup::place(jadefx::Node& owner, double caret_x, double caret_y, double caret_height) {
    if (!state_->popup) {
        return;
    }
    jadefx::Scene* scene = owner.getScene();
    if (scene == nullptr || scene->isTearingDown()) {
        return;
    }
    state_->owner = &owner;
    state_->caret_x = caret_x;
    state_->caret_y = caret_y;
    state_->caret_height = caret_height;
    jadefx::PopupOptions options;
    options.owner = &owner;
    options.autoHide = true;
    if (!scene->isPopupShowing(state_->popup.get())) {
        scene->showPopup(state_->popup, caret_x, caret_y + caret_height + 2, -1, -1, options);
    }
    double width = state_->popup->getWidth();
    double height = state_->popup->getHeight();
    if (width < 1) {
        width = 420;
    }
    if (height < 1) {
        height = 28;
    }
    double x = caret_x;
    double y = caret_y + caret_height + 2;
    if (scene->getWidth() > 0 && x + width > scene->getWidth()) {
        x = std::max(0.0, scene->getWidth() - width);
    }
    if (x < 0) {
        x = 0;
    }
    if (scene->getHeight() > 0 && y + height > scene->getHeight() && caret_y > height + 2) {
        y = caret_y - height - 2;
    }
    scene->movePopup(state_->popup.get(), x, y, width, height);
}

void CompletionPopup::moveTo(jadefx::Node& owner, double caret_x, double caret_y, double caret_height) {
    place(owner, caret_x, caret_y, caret_height);
}

void CompletionPopup::present(const CompletionList& list, bool force, jadefx::Node& owner, double caret_x, double caret_y,
                              double caret_height) {
    if (state_->accepting) {
        return;
    }
    const bool open = isOpen();
    if (list.site == CompleteSite::None || list.items.empty()) {
        dismiss();
        return;
    }
    if (list.site == CompleteSite::Name && !force) {
        const bool longer = std::any_of(list.items.begin(), list.items.end(), [&](const CompletionItem& item) {
            return item.name.size() > list.prefix.size();
        });
        if (list.prefix.empty() || !longer) {
            dismiss();
            return;
        }
    }
    std::string previous;
    const bool keep_pick = open && state_->picked && state_->site == list.site && state_->selected >= 0 &&
                           state_->selected < static_cast<int>(state_->items.size());
    if (keep_pick) {
        previous = state_->items[static_cast<std::size_t>(state_->selected)].name;
    }
    state_->items = list.items;
    state_->site = list.site;
    state_->prefix = list.prefix;
    state_->replace_begin = list.replace_begin;
    state_->replace_end = list.replace_end;
    state_->close_quote = list.close_quote;
    state_->unclosed = list.unclosed;
    state_->owner = &owner;
    int exact = -1;
    int kept = -1;
    for (int index = 0; index < static_cast<int>(state_->items.size()); ++index) {
        const std::string& name = state_->items[static_cast<std::size_t>(index)].name;
        if (exact < 0 && name == list.prefix) {
            exact = index;
        }
        if (kept < 0 && !previous.empty() && name == previous) {
            kept = index;
        }
    }
    if (keep_pick && kept >= 0) {
        state_->selected = kept;
    } else {
        state_->picked = false;
        state_->selected = exact >= 0 ? exact : 0;
    }
    fill();
    place(owner, caret_x, caret_y, caret_height);
}

std::vector<engine_core::LuaNode> completion_world(engine_core::Engine& engine, std::uint32_t script_id,
                                                   const std::string* buffer) {
    std::vector<engine_core::LuaNode> nodes;
    engine_core::DataModel& model = engine.datamodel();
    engine_core::DataModelLock lock(model, engine_core::DataModelLock::Read, kLockWait);
    if (!lock.owns()) {
        return nodes;
    }
    engine_core::LuaNode root;
    root.id = 0;
    root.parent = engine_core::DataModel::kNoParent;
    root.name = model.name(0);
    root.class_name = "DataModel";
    nodes.push_back(std::move(root));
    model.for_each_instance([&](engine_core::DataModel& object) {
        if (object.id() == 0) {
            return;
        }
        engine_core::LuaNode node;
        node.id = object.id();
        node.parent = model.parent(object.id());
        node.name = model.name(object.id());
        const char* class_name = object.class_name();
        node.class_name = class_name != nullptr ? class_name : "";
        if (auto* source = dynamic_cast<engine_core::LuaSource*>(&object)) {
            node.source = buffer != nullptr && object.id() == script_id ? *buffer : source->source();
        }
        nodes.push_back(std::move(node));
    });
    return nodes;
}

}  // namespace ide
