#include "InsertPopup.hpp"

#include "ClassFilter.hpp"
#include "Containment.hpp"
#include "IdeIcons.hpp"
#include "IdeTheme.hpp"
#include "LuaApi.hpp"

#include "jadefx/jadefx.hpp"
#include "jadefx/scene/Painter.hpp"
#include "jadefx/scene/controls/ScrollTrack.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace ide {

bool insert_offers(const std::string& class_name) {
    return engine_core::lua_creatable_known(class_name.c_str()) && !engine_core::is_asset_class(class_name);
}

const char* describe_class(const std::string& name) {
    struct Row {
        const char* name;
        const char* text;
    };
    static constexpr Row kRows[] = {
        {"GameObject", "An object in the world you can see."},
        {"Script", "Code that runs when the game plays."},
        {"ModuleScript", "Code other scripts load with require."},
        {"Folder", "Groups things together to stay tidy."},
        {"Model", "A 3D model asset."},
        {"Mesh", "A 3D shape asset."},
        {"Texture", "An image asset."},
        {"Material", "How a surface looks."},
        {"Sound", "An audio asset."},
        {"Prefab", "A reusable object template."},
    };
    for (const Row& row : kRows) {
        if (name == row.name) {
            return row.text;
        }
    }
    return "";
}

constexpr double kOpenSeconds = 0.16;
constexpr double kShadowMargin = 12;
constexpr int kMaxVisibleRows = 8;
constexpr int kMaxPointRows = 10;
constexpr double kPopupWidth = 280;
constexpr double kFieldGap = 4;

class InsertList;

class InsertField : public jadefx::TextField {
public:
    explicit InsertField(std::function<void()> changed) : changed_(std::move(changed)) { setPromptText("Search"); }

protected:
    void handleText(jadefx::TextEvent& event) override {
        jadefx::TextField::handleText(event);
        notify();
    }

    void handleKey(jadefx::KeyEvent& event) override {
        const std::string before = getText();
        jadefx::TextField::handleKey(event);
        if (getText() != before) {
            notify();
        }
    }

private:
    void notify() {
        if (changed_) {
            changed_();
        }
    }

    std::function<void()> changed_;
};

class InsertScrollTrack : public jadefx::Region {
public:
    explicit InsertScrollTrack(InsertList& list) : list_(&list) { setDefaultCursor(jadefx::Cursor::Default); }

    const char* getElementType() const override { return "scroll-bar"; }

    void clear() { list_ = nullptr; }

protected:
    void handleMousePressed(const jadefx::MouseEvent& event) override;
    void handleMouseDragged(const jadefx::MouseEvent& event) override;
    void handleMouseReleased(const jadefx::MouseEvent&) override;
    void renderContent(jadefx::UiRenderer& renderer, float opacity) override;

private:
    InsertList* list_ = nullptr;
};

// Search field on top, then at most eight class rows. Further rows scroll.
class InsertList : public jadefx::Controls {
    friend class InsertScrollTrack;

public:
    InsertList() {
        setPrefWidth(kPopupWidth);
        setDefaultCursor(jadefx::Cursor::Default);
        setStyle(
            "background-color: var(--ide-popup-color); border-style: solid; border-width: 1px; "
            "border-color: var(--ide-popup-border-color); border-radius: 6px; "
            "box-shadow: 0 4px 14px var(--ide-popup-shadow-color); padding: 4px;");
        field_ = std::make_shared<InsertField>([this] { onTyped(); });
        field_->setOnAction([this](jadefx::ActionEvent&) { choose(selected_); });
        children().add(field_);
        info_ = jadefx::make<jadefx::Label>("");
        info_->setMouseTransparent(true);
        info_->setAlignment(jadefx::Pos::CenterLeft);
        info_->setOpacity(0.7f);
        info_->setStyle(
            "font-size: 12px; padding: 4px 4px 2px 4px; border-style: solid; border-width: 1px 0 0 0; "
            "border-color: var(--ide-popup-border-color);");
        children().add(info_);
        actions_ = jadefx::make<jadefx::HBox>();
        actions_->setSpacing(2);
        actions_->setAlignment(jadefx::Pos::CenterLeft);
        actions_->setStyle(
            "border-style: solid; border-width: 1px 0 0 0; border-color: var(--ide-popup-border-color); "
            "padding: 4px 0 0 0;");
        actions_->setVisible(false);
        children().add(actions_);
        track_ = std::make_shared<InsertScrollTrack>(*this);
        children().add(track_);
    }

    ~InsertList() override {
        if (track_) {
            track_->clear();
        }
    }

    const char* getElementType() const override { return "insert-popup"; }

    void setOnCreate(std::function<void(const std::string&)> handler) { create_ = std::move(handler); }

    void bindOwner(const std::shared_ptr<InsertList>& self) { self_ = self; }

    bool open() const {
        jadefx::Scene* scene = getScene();
        return scene != nullptr && !scene->isTearingDown() && scene->isPopupShowing(this);
    }

    void showAtPoint(jadefx::Node& owner, double x, double y, std::vector<InsertAction> actions) {
        engine_core::lua_creatable_names(all_);
        if (field_) {
            field_->setText("");
        }
        rebuild();
        setActions(std::move(actions));
        openedAt_ = std::chrono::steady_clock::now();
        anchor_ = &owner;
        pinned_x_ = x;
        pinned_y_ = y;
        pinned_w_ = 0;
        pinned_h_ = 0;
        pinned_ = true;
        atPoint_ = true;
        place(owner);
        if (field_) {
            field_->requestFocus();
        }
    }

    void showAt(jadefx::Node& anchor) {
        engine_core::lua_creatable_names(all_);
        all_.erase(std::remove_if(all_.begin(), all_.end(),
                                  [](const std::string& name) { return !insert_offers(name); }),
                   all_.end());
        if (field_) {
            field_->setText("");
        }
        rebuild();
        setActions({});
        atPoint_ = false;
        openedAt_ = std::chrono::steady_clock::now();
        // The + is hidden, and laid out at the corner, once the pointer leaves
        // the row. Later filter updates keep this opening position.
        pinAnchor(anchor);
        place(anchor);
        if (field_) {
            field_->requestFocus();
        }
    }

    void dismiss() {
        anchor_ = nullptr;
        pinned_ = false;
        jadefx::Scene* scene = getScene();
        if (scene == nullptr || scene->isTearingDown() || !scene->isPopupShowing(this)) {
            return;
        }
        scene->hidePopup(this);
    }

    void render(jadefx::UiRenderer& renderer, float opacity) override {
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - openedAt_).count();
        const double t = std::clamp(elapsed / kOpenSeconds, 0.0, 1.0);
        if (t >= 1.0) {
            jadefx::Controls::render(renderer, opacity);
            return;
        }
        const double eased = 1.0 - std::pow(1.0 - t, 3.0);
        const float x = static_cast<float>(getAbsoluteX() - kShadowMargin);
        const float y = static_cast<float>(getAbsoluteY());
        const float width = static_cast<float>(getWidth() + 2 * kShadowMargin);
        const float full = static_cast<float>(getHeight() + kShadowMargin);
        const float shown = static_cast<float>(full * eased);
        jadefx::Painter painter(renderer);
        if (dropsUp_) {
            painter.pushClip(x, y - static_cast<float>(kShadowMargin) + full - shown, width, shown);
        } else {
            painter.pushClip(x, y, width, shown);
        }
        jadefx::Controls::render(renderer, opacity * static_cast<float>(0.25 + 0.75 * eased));
        painter.popClip();
    }

protected:
    double preferredContentHeight(double innerWidth) const override {
        const int visible = std::min(static_cast<int>(rows_.size()), maxRows());
        double height = fieldHeight(innerWidth);
        if (visible > 0) {
            height += kFieldGap + static_cast<double>(visible) * rowExtent(innerWidth);
            if (info_) {
                height += kFieldGap + info_->measuredHeight(std::max(0.0, innerWidth), -1);
            }
        }
        if (!actionList_.empty() && actions_) {
            height += kFieldGap + actions_->measuredHeight(std::max(0.0, innerWidth), -1);
        }
        return height;
    }

    void layoutChildren() override {
        const double left = contentLeft();
        const double top = contentTop();
        const double width = contentWidth();
        const double field = fieldHeight(width);
        if (field_) {
            field_->setVisible(true);
            field_->performLayout(left, top, width, field);
        }
        const double row = rowExtent(width);
        rowHeight_ = row;
        const int count = static_cast<int>(rows_.size());
        const int visible = std::min(count, maxRows());
        const double listTop = top + field + (count > 0 ? kFieldGap : 0);
        if (ensure_ >= 0) {
            reveal(ensure_, row, count, visible);
            ensure_ = -1;
        }
        clampScroll(row, count, visible);
        const bool bars = count > maxRows() && visible > 0 && row > 0.0;
        const double gutter = bars ? static_cast<double>(jadefx::ScrollTrack::kThickness) : 0.0;
        const double rowWidth = std::max(0.0, width - gutter);
        const int start = windowStart(row, count, visible);
        for (int index = 0; index < count; ++index) {
            const std::shared_ptr<jadefx::HBox>& node = rows_[static_cast<std::size_t>(index)];
            const bool shown = index >= start && index < start + visible;
            node->setVisible(shown);
            if (!shown || row <= 0.0) {
                node->performLayout(0, 0, 0, 0);
                continue;
            }
            node->performLayout(left, listTop + static_cast<double>(index - start) * row, rowWidth, row);
        }
        double below = listTop + static_cast<double>(visible) * row;
        if (info_) {
            if (visible > 0) {
                const double infoHeight = info_->measuredHeight(width, -1);
                info_->setVisible(true);
                info_->performLayout(left, below + kFieldGap, width, infoHeight);
                below += kFieldGap + infoHeight;
            } else {
                info_->setVisible(false);
                info_->performLayout(0, 0, 0, 0);
            }
        }
        if (actions_) {
            if (actionList_.empty()) {
                actions_->setVisible(false);
                actions_->performLayout(0, 0, 0, 0);
            } else {
                const double actionsTop = below + kFieldGap;
                actions_->setVisible(true);
                actions_->performLayout(left, actionsTop, width, actions_->measuredHeight(width, -1));
            }
        }
        if (!track_) {
            return;
        }
        if (!bars) {
            bar_ = {};
            track_->setVisible(false);
            track_->performLayout(0, 0, 0, 0);
            return;
        }
        const double listHeight = static_cast<double>(visible) * row;
        bar_ = jadefx::ScrollTrack::vertical(static_cast<float>(left + width - jadefx::ScrollTrack::kThickness),
                                           static_cast<float>(listTop), static_cast<float>(listHeight),
                                           static_cast<float>(static_cast<double>(count) * row),
                                           static_cast<float>(listHeight), scroll_);
        const double trackX = std::max(left, static_cast<double>(bar_.cross) - jadefx::ScrollTrack::kHitSlop);
        const double trackRight = std::min(left + width, static_cast<double>(bar_.cross + bar_.thickness));
        track_->setVisible(true);
        track_->performLayout(trackX, listTop, std::max(0.0, trackRight - trackX), listHeight);
    }

    void handleScroll(jadefx::ScrollEvent& event) override {
        if (static_cast<int>(rows_.size()) <= maxRows()) {
            return;
        }
        const double delta = event.deltaY != 0.0 ? event.deltaY : event.deltaX;
        scrollBy(delta);
        event.consume();
    }

    void handleKey(jadefx::KeyEvent& event) override {
        if (!event.pressed) {
            return;
        }
        if (event.key == jadefx::Key::Up) {
            moveHighlight(-1);
            event.consume();
        } else if (event.key == jadefx::Key::Down) {
            moveHighlight(1);
            event.consume();
        }
    }

private:
    void pinAnchor(jadefx::Node& anchor) {
        anchor_ = &anchor;
        pinned_x_ = anchor.getAbsoluteX();
        pinned_y_ = anchor.getAbsoluteY();
        pinned_w_ = anchor.getWidth();
        pinned_h_ = anchor.getHeight();
        pinned_ = pinned_w_ > 0.5 && pinned_h_ > 0.5;
    }

    void onTyped() {
        rebuild();
        jadefx::Scene* scene = anchor_ != nullptr ? anchor_->getScene() : getScene();
        if (scene == nullptr || scene->isTearingDown() || anchor_ == nullptr) {
            return;
        }
        place(*anchor_);
    }

    void rebuild() {
        for (const std::shared_ptr<jadefx::HBox>& row : rows_) {
            children().removeIf([&](const std::shared_ptr<jadefx::Node>& child) { return child.get() == row.get(); });
        }
        rows_.clear();
        const std::string query = field_ ? field_->getText() : std::string();
        filter_class_names(all_, query, shown_);
        selected_ = shown_.empty() ? -1 : 0;
        scroll_ = 0;
        ensure_ = selected_;
        const int count = static_cast<int>(shown_.size());
        for (int index = 0; index < count; ++index) {
            const std::string& name = shown_[static_cast<std::size_t>(index)];
            auto row = jadefx::make<jadefx::HBox>();
            row->setSpacing(8);
            row->setPadding(jadefx::Insets{3, 8, 3, 8});
            row->setAlignment(jadefx::Pos::CenterLeft);
            row->setCursor(jadefx::Cursor::Pointer);
            row->setStyle("border-radius: 4px; transition: background-color 0.1s;");
            if (index == selected_) {
                row->setBackground(theme_color("--ide-popup-selection-color"));
            }
            if (std::shared_ptr<jadefx::ImageView> icon = icon_view(name)) {
                icon->setMouseTransparent(true);
                icon->setPrefSize(16, 16);
                icon->setMinSize(16, 16);
                row->getChildren().add(std::move(icon));
            }
            auto label = jadefx::make<jadefx::Label>(name);
            label->setMouseTransparent(true);
            row->getChildren().add(std::move(label));
            row->setOnMouseEntered([this, index](const jadefx::MouseEvent&) {
                if (selected_ == index) {
                    return;
                }
                selected_ = index;
                paint();
            });
            row->setOnMousePressed([this, index](const jadefx::MouseEvent&) { choose(index); });
            const std::size_t at = track_ && !children().empty() ? children().size() - 1 : children().size();
            children().insert(at, row);
            rows_.push_back(std::move(row));
        }
        describe();
    }

    void describe() {
        if (!info_) {
            return;
        }
        const bool valid = selected_ >= 0 && selected_ < static_cast<int>(shown_.size());
        info_->setText(valid ? describe_class(shown_[static_cast<std::size_t>(selected_)]) : "");
    }

    void setActions(std::vector<InsertAction> actions) {
        actionList_ = std::move(actions);
        if (!actions_) {
            return;
        }
        actions_->getChildren().clear();
        for (std::size_t index = 0; index < actionList_.size(); ++index) {
            const InsertAction& action = actionList_[index];
            auto button = jadefx::make<jadefx::HBox>();
            button->setAlignment(jadefx::Pos::Center);
            button->setPadding(jadefx::Insets{4, 6, 4, 6});
            button->setStyle("border-radius: 4px; transition: background-color 0.1s;");
            if (std::shared_ptr<jadefx::ImageView> icon = icon_graphic(action.icon)) {
                icon->setMouseTransparent(true);
                icon->setPrefSize(16, 16);
                icon->setMinSize(16, 16);
                icon->setElementId("menu-label:" + action.label);
                button->getChildren().add(std::move(icon));
            } else {
                auto label = jadefx::make<jadefx::Label>(action.label);
                label->setMouseTransparent(true);
                label->setElementId("menu-label:" + action.label);
                button->getChildren().add(std::move(label));
            }
            jadefx::Tooltip::install(button.get(), jadefx::make<jadefx::Tooltip>(action.label));
            if (!action.enabled) {
                button->setOpacity(0.35f);
            } else {
                button->setCursor(jadefx::Cursor::Pointer);
                jadefx::HBox* raw = button.get();
                button->setOnMouseEntered([raw](const jadefx::MouseEvent&) {
                    raw->setBackground(theme_color("--ide-popup-selection-color"));
                });
                button->setOnMouseExited(
                    [raw](const jadefx::MouseEvent&) { raw->setBackground(jadefx::Color::transparent()); });
                button->setOnMousePressed([this, index](const jadefx::MouseEvent&) { runAction(index); });
            }
            actions_->getChildren().add(std::move(button));
        }
    }

    int maxRows() const { return atPoint_ ? kMaxPointRows : kMaxVisibleRows; }

    void runAction(std::size_t index) {
        if (index >= actionList_.size()) {
            return;
        }
        const std::function<void()> run = actionList_[index].run;
        dismiss();
        if (run) {
            run();
        }
    }

    void paint() {
        const int count = static_cast<int>(rows_.size());
        const jadefx::Color highlight = theme_color("--ide-popup-selection-color");
        for (int index = 0; index < count; ++index) {
            rows_[static_cast<std::size_t>(index)]->setBackground(index == selected_ ? highlight
                                                                                      : jadefx::Color::transparent());
        }
        describe();
    }

    void choose(int index) {
        if (index < 0 || index >= static_cast<int>(shown_.size())) {
            return;
        }
        const std::string name = shown_[static_cast<std::size_t>(index)];
        const std::function<void(const std::string&)> create = create_;
        dismiss();
        if (create) {
            create(name);
        }
    }

    void moveHighlight(int delta) {
        const int count = static_cast<int>(shown_.size());
        if (count <= 0) {
            return;
        }
        int next = selected_ < 0 ? (delta < 0 ? count - 1 : 0) : selected_ + delta;
        if (next < 0) {
            next = 0;
        }
        if (next >= count) {
            next = count - 1;
        }
        if (next == selected_) {
            return;
        }
        selected_ = next;
        ensure_ = next;
        paint();
        relayout();
    }

    void place(jadefx::Node& anchor) {
        jadefx::Scene* scene = anchor.getScene();
        if (scene == nullptr || scene->isTearingDown()) {
            return;
        }
        const std::shared_ptr<InsertList> self = self_.lock();
        if (!self) {
            return;
        }
        if (!pinned_) {
            pinAnchor(anchor);
        }
        jadefx::PopupOptions options;
        options.owner = atPoint_ ? nullptr : &anchor;
        options.autoHide = true;
        options.animate = false;
        scene->showPopup(self, 0, 0, -1, -1, options);
        double width = getWidth();
        double height = getHeight();
        if (width < 1) {
            width = kPopupWidth;
        }
        if (height < 1) {
            height = 32;
        }
        if (scene->getWidth() > 0 && width > scene->getWidth()) {
            width = scene->getWidth();
        }
        const double originX = pinned_ ? pinned_x_ : anchor.getAbsoluteX();
        const double originY = pinned_ ? pinned_y_ : anchor.getAbsoluteY();
        const double originW = pinned_ ? pinned_w_ : anchor.getWidth();
        const double originH = pinned_ ? pinned_h_ : anchor.getHeight();
        const double gap = atPoint_ ? 0.0 : 2.0;
        double x = atPoint_ ? originX : originX + originW - width;
        double y = originY + originH + gap;
        if (scene->getWidth() > 0 && x + width > scene->getWidth()) {
            x = scene->getWidth() - width;
        }
        if (x < 0) {
            x = 0;
        }
        dropsUp_ = false;
        if (scene->getHeight() > 0 && y + height > scene->getHeight() && originY > height + gap) {
            y = originY - height - gap;
            dropsUp_ = true;
        }
        if (y < 0) {
            y = 0;
        }
        scene->movePopup(this, x, y, width, height);
    }

    double fieldHeight(double innerWidth) const {
        if (!field_) {
            return 0;
        }
        return field_->measuredHeight(std::max(0.0, innerWidth), -1);
    }

    double rowExtent(double innerWidth) const {
        double height = 0;
        const double width = std::max(0.0, innerWidth);
        for (const std::shared_ptr<jadefx::HBox>& row : rows_) {
            if (row) {
                height = std::max(height, row->measuredHeight(width, -1));
            }
        }
        if (height < 1.0 && !rows_.empty()) {
            height = 22.0;
        }
        return height;
    }

    int windowStart(double row, int count, int visible) const {
        if (row <= 0.0 || count <= visible) {
            return 0;
        }
        const int maxStart = count - visible;
        int start = static_cast<int>(std::floor(scroll_ / row + 1e-9));
        if (start < 0) {
            start = 0;
        }
        if (start > maxStart) {
            start = maxStart;
        }
        return start;
    }

    void clampScroll(double row, int count, int visible) {
        const double maxScroll = row > 0.0 && count > visible ? static_cast<double>(count - visible) * row : 0.0;
        if (scroll_ < 0.0) {
            scroll_ = 0.0;
        }
        if (scroll_ > maxScroll) {
            scroll_ = maxScroll;
        }
    }

    void reveal(int index, double row, int count, int visible) {
        if (count <= visible || row <= 0.0 || visible <= 0) {
            scroll_ = 0;
            return;
        }
        if (index < 0) {
            index = 0;
        }
        if (index >= count) {
            index = count - 1;
        }
        const int maxStart = count - visible;
        int start = windowStart(row, count, visible);
        if (index < start) {
            start = index;
        }
        if (index >= start + visible) {
            start = index - visible + 1;
        }
        if (start < 0) {
            start = 0;
        }
        if (start > maxStart) {
            start = maxStart;
        }
        scroll_ = static_cast<double>(start) * row;
    }

    void scrollBy(double deltaY) {
        const int count = static_cast<int>(rows_.size());
        const int visible = std::min(count, maxRows());
        if (count <= visible || rowHeight_ <= 0.0) {
            return;
        }
        const double magnitude = std::fabs(deltaY);
        double pixels = deltaY;
        if (magnitude > 0.0 && std::fabs(magnitude - 1.0) <= 0.2) {
            pixels = std::copysign(rowHeight_, deltaY);
        }
        scroll_ -= pixels;
        clampScroll(rowHeight_, count, visible);
        relayout();
    }

    void relayout() {
        if (getWidth() <= 0.0 || getHeight() <= 0.0) {
            return;
        }
        performLayout(getX(), getY(), getWidth(), getHeight());
    }

    void pressScroll(const jadefx::MouseEvent& event) {
        const float localX = static_cast<float>(event.x - getAbsoluteX());
        const float localY = static_cast<float>(event.y - getAbsoluteY());
        const jadefx::ScrollTrack::Part where = bar_.part(localX, localY);
        if (where == jadefx::ScrollTrack::Part::None) {
            scrollDrag_ = false;
            return;
        }
        scrollDrag_ = true;
        if (where == jadefx::ScrollTrack::Part::Thumb) {
            scrollGrab_ = localY - bar_.thumb;
            return;
        }
        scroll_ = bar_.offsetFromPage(scroll_, where == jadefx::ScrollTrack::Part::After);
        const int count = static_cast<int>(rows_.size());
        clampScroll(rowHeight_, count, std::min(count, maxRows()));
        scrollGrab_ = bar_.thumbLength * 0.5f;
        relayout();
    }

    void dragScroll(const jadefx::MouseEvent& event) {
        if (!scrollDrag_) {
            return;
        }
        const float localX = static_cast<float>(event.x - getAbsoluteX());
        const float localY = static_cast<float>(event.y - getAbsoluteY());
        scroll_ = bar_.offsetFromDrag(localX, localY, scrollGrab_);
        const int count = static_cast<int>(rows_.size());
        clampScroll(rowHeight_, count, std::min(count, maxRows()));
        relayout();
    }

    void releaseScroll() { scrollDrag_ = false; }

    void paintScroll(jadefx::UiRenderer& renderer, float opacity) const {
        bar_.draw(renderer, static_cast<float>(getAbsoluteX()), static_cast<float>(getAbsoluteY()), opacity);
    }

    std::function<void(const std::string&)> create_;
    std::weak_ptr<InsertList> self_;
    std::shared_ptr<InsertField> field_;
    std::shared_ptr<InsertScrollTrack> track_;
    std::vector<std::string> all_;
    std::vector<std::string> shown_;
    std::vector<std::shared_ptr<jadefx::HBox>> rows_;
    jadefx::Node* anchor_ = nullptr;
    double pinned_x_ = 0;
    double pinned_y_ = 0;
    double pinned_w_ = 0;
    double pinned_h_ = 0;
    bool pinned_ = false;
    bool dropsUp_ = false;
    bool atPoint_ = false;
    std::shared_ptr<jadefx::HBox> actions_;
    std::shared_ptr<jadefx::Label> info_;
    std::vector<InsertAction> actionList_;
    std::chrono::steady_clock::time_point openedAt_{};
    jadefx::ScrollTrack bar_{};
    double scroll_ = 0;
    double rowHeight_ = 0;
    float scrollGrab_ = 0;
    int selected_ = -1;
    int ensure_ = -1;
    bool scrollDrag_ = false;
};

void InsertScrollTrack::handleMousePressed(const jadefx::MouseEvent& event) {
    if (list_ != nullptr) {
        list_->pressScroll(event);
    }
}

void InsertScrollTrack::handleMouseDragged(const jadefx::MouseEvent& event) {
    if (list_ != nullptr) {
        list_->dragScroll(event);
    }
}

void InsertScrollTrack::handleMouseReleased(const jadefx::MouseEvent&) {
    if (list_ != nullptr) {
        list_->releaseScroll();
    }
}

void InsertScrollTrack::renderContent(jadefx::UiRenderer& renderer, float opacity) {
    if (list_ != nullptr) {
        list_->paintScroll(renderer, opacity);
    }
}

struct InsertPopup::List : InsertList {};

InsertPopup::InsertPopup() : list_(std::make_shared<List>()) { list_->bindOwner(list_); }

InsertPopup::~InsertPopup() { hide(); }

void InsertPopup::setOnCreate(std::function<void(const std::string&)> handler) {
    if (list_) {
        list_->setOnCreate(std::move(handler));
    }
}

void InsertPopup::show(jadefx::Node& anchor) {
    if (list_) {
        list_->showAt(anchor);
    }
}

void InsertPopup::show_at(jadefx::Node& owner, double x, double y, std::vector<InsertAction> actions) {
    if (list_) {
        list_->showAtPoint(owner, x, y, std::move(actions));
    }
}

void InsertPopup::hide() {
    if (list_) {
        list_->dismiss();
    }
}

}  // namespace ide
