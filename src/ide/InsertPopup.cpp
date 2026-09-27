#include "InsertPopup.hpp"

#include "ClassFilter.hpp"
#include "IdeIcons.hpp"
#include "LuaApi.hpp"

#include "jadefx/jadefx.hpp"
#include "jadefx/scene/controls/ScrollTrack.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace ide {

constexpr int kMaxVisibleRows = 8;
constexpr double kPopupWidth = 280;
constexpr double kFieldGap = 4;
const jadefx::Color kRowHighlight = jadefx::Color::parse("#d6e4f5");

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
            "background-color: #ffffff; border-style: solid; border-width: 1px; border-color: #c5c8ce; "
            "box-shadow: 0 2px 8px rgba(32, 33, 36, 0.16); padding: 4px;");
        field_ = std::make_shared<InsertField>([this] { onTyped(); });
        field_->setOnAction([this](jadefx::ActionEvent&) { choose(selected_); });
        children().add(field_);
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

    void showAt(jadefx::Node& anchor) {
        engine_core::lua_creatable_names(all_);
        if (field_) {
            field_->setText("");
        }
        rebuild();
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

protected:
    double preferredContentHeight(double innerWidth) const override {
        const int visible = std::min(static_cast<int>(rows_.size()), kMaxVisibleRows);
        double height = fieldHeight(innerWidth);
        if (visible > 0) {
            height += kFieldGap + static_cast<double>(visible) * rowExtent(innerWidth);
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
        const int visible = std::min(count, kMaxVisibleRows);
        const double listTop = top + field + (count > 0 ? kFieldGap : 0);
        if (ensure_ >= 0) {
            reveal(ensure_, row, count, visible);
            ensure_ = -1;
        }
        clampScroll(row, count, visible);
        const bool bars = count > kMaxVisibleRows && visible > 0 && row > 0.0;
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
        if (static_cast<int>(rows_.size()) <= kMaxVisibleRows) {
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
            if (index == selected_) {
                row->setBackground(kRowHighlight);
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
    }

    void paint() {
        const int count = static_cast<int>(rows_.size());
        for (int index = 0; index < count; ++index) {
            rows_[static_cast<std::size_t>(index)]->setBackground(index == selected_ ? kRowHighlight
                                                                                      : jadefx::Color::transparent());
        }
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
        options.owner = &anchor;
        options.autoHide = true;
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
        double x = originX + originW - width;
        double y = originY + originH + 2;
        if (scene->getWidth() > 0 && x + width > scene->getWidth()) {
            x = scene->getWidth() - width;
        }
        if (x < 0) {
            x = 0;
        }
        if (scene->getHeight() > 0 && y + height > scene->getHeight() && originY > height + 2) {
            y = originY - height - 2;
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
        const int visible = std::min(count, kMaxVisibleRows);
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
        clampScroll(rowHeight_, count, std::min(count, kMaxVisibleRows));
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
        clampScroll(rowHeight_, count, std::min(count, kMaxVisibleRows));
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

bool InsertPopup::isOpen() const { return list_ && list_->open(); }

void InsertPopup::show(jadefx::Node& anchor) {
    if (list_) {
        list_->showAt(anchor);
    }
}

void InsertPopup::hide() {
    if (list_) {
        list_->dismiss();
    }
}

}  // namespace ide
