#include "QuickOpen.hpp"

#include "IdeIcons.hpp"
#include "IdeTheme.hpp"

#include "jadefx/jadefx.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

namespace ide {

namespace {

constexpr double kMaxWidth = 1000;
constexpr double kMargin = 20;
constexpr double kTop = 40;
constexpr double kFieldGap = 4;
constexpr int kMaxVisibleRows = 20;
constexpr std::size_t kMaxResults = 200;
// The path column starts this far across a row.
constexpr double kPathColumn = 1.0 / 3.0;

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

}  // namespace

std::vector<std::size_t> quick_open_filter(const std::vector<QuickOpenEntry>& entries, std::string_view query,
                                           std::size_t limit) {
    std::vector<std::size_t> out;
    if (query.empty()) {
        return out;
    }
    const std::string needle = lower(query);
    // 0: the name starts with it, 1: the name holds it, 2: the path holds it.
    std::vector<std::pair<int, std::size_t>> ranked;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const std::string name = lower(entries[index].name);
        const std::size_t at = name.find(needle);
        if (at == 0) {
            ranked.emplace_back(0, index);
        } else if (at != std::string::npos) {
            ranked.emplace_back(1, index);
        } else if (lower(entries[index].path).find(needle) != std::string::npos) {
            ranked.emplace_back(2, index);
        }
    }
    std::stable_sort(ranked.begin(), ranked.end(), [&](const auto& a, const auto& b) {
        if (a.first != b.first) {
            return a.first < b.first;
        }
        return entries[a.second].name.size() < entries[b.second].name.size();
    });
    for (const auto& [group, index] : ranked) {
        if (out.size() >= limit) {
            break;
        }
        out.push_back(index);
    }
    return out;
}

namespace {

class QuickOpenField : public jadefx::TextField {
public:
    explicit QuickOpenField(std::function<void()> changed) : changed_(std::move(changed)) {
        setPromptText("Find an instance by name");
    }

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

// Icon and name, the matched part highlighted as Find in Scripts does, then the
// parent's path in a column.
class QuickOpenRow : public jadefx::Controls {
public:
    QuickOpenRow(const QuickOpenEntry& entry, std::string_view query) {
        setCursor(jadefx::Cursor::Pointer);
        setStyle("border-radius: 3px; padding: 2px 6px;");
        if (std::shared_ptr<jadefx::ImageView> view = icon_view(entry.class_name)) {
            view->setMouseTransparent(true);
            view->setPrefSize(16, 16);
            view->setMinSize(16, 16);
            icon_ = view;
            children().add(std::move(view));
        }
        const std::string name = lower(entry.name);
        const std::size_t at = query.empty() ? std::string::npos : name.find(lower(query));
        if (at == std::string::npos) {
            addName(entry.name, false);
        } else {
            addName(entry.name.substr(0, at), false);
            addName(entry.name.substr(at, query.size()), true);
            addName(entry.name.substr(at + query.size()), false);
        }
        path_ = jadefx::make<jadefx::Label>(entry.path);
        path_->setMouseTransparent(true);
        path_->setStyle("color: var(--ide-popup-detail-text-color);");
        children().add(path_);
    }

    const char* getElementType() const override { return "quick-open-row"; }

protected:
    double preferredContentHeight(double) const override {
        double height = 16;
        for (const std::shared_ptr<jadefx::Label>& part : name_) {
            height = std::max(height, part->measuredHeight(-1, -1));
        }
        return height;
    }

    void layoutChildren() override {
        const double left = contentLeft();
        const double top = contentTop();
        const double width = contentWidth();
        const double height = contentHeight();
        double x = left;
        if (icon_) {
            icon_->performLayout(x, top + (height - 16) / 2, 16, 16);
            x += 16 + 4;
        }
        const double column = left + width * kPathColumn;
        for (const std::shared_ptr<jadefx::Label>& part : name_) {
            const double room = std::max(0.0, column - 8 - x);
            const double w = std::min(part->measuredWidth(-1), room);
            part->performLayout(x, top, w, height);
            x += w;
        }
        path_->performLayout(column, top, std::max(0.0, left + width - column), height);
    }

private:
    void addName(std::string text, bool matched) {
        if (text.empty()) {
            return;
        }
        auto label = jadefx::make<jadefx::Label>(std::move(text));
        label->setMouseTransparent(true);
        if (matched) {
            label->setStyle("background-color: var(--ide-find-match-color); border-radius: 2px;");
        }
        children().add(label);
        name_.push_back(std::move(label));
    }

    std::shared_ptr<jadefx::ImageView> icon_;
    std::vector<std::shared_ptr<jadefx::Label>> name_;
    std::shared_ptr<jadefx::Label> path_;
};

}  // namespace

struct QuickOpen::List : jadefx::Controls {
    List() {
        setDefaultCursor(jadefx::Cursor::Default);
        setStyle(
            "background-color: var(--ide-popup-color); border-style: solid; border-width: 1px; "
            "border-color: var(--ide-popup-border-color); border-radius: 6px; "
            "box-shadow: 0 4px 14px var(--ide-popup-shadow-color); padding: 6px;");
        title_ = jadefx::make<jadefx::Label>("Quick Open");
        title_->setMouseTransparent(true);
        title_->setAlignment(jadefx::Pos::CenterLeft);
        title_->setStyle("font-size: 12px; padding: 0 2px 4px 2px;");
        children().add(title_);
        field_ = std::make_shared<QuickOpenField>([this] { rebuild(); });
        field_->setOnAction([this](jadefx::ActionEvent&) { choose(selected_); });
        children().add(field_);
    }

    const char* getElementType() const override { return "quick-open"; }

    void show(jadefx::Scene& scene, std::vector<QuickOpenEntry> entries) {
        entries_ = std::move(entries);
        scene_ = &scene;
        jadefx::PopupOptions options;
        options.autoHide = true;
        options.animate = false;
        scene.showPopup(self_.lock(), 0, 0, -1, -1, options);
        // Typing again over what was found last time picks up where it left off.
        rebuild();
        field_->selectAll();
        field_->requestFocus();
    }

    void dismiss() {
        jadefx::Scene* scene = getScene();
        if (scene == nullptr || scene->isTearingDown() || !scene->isPopupShowing(this)) {
            return;
        }
        scene->hidePopup(this);
    }

    std::weak_ptr<List> self_;
    std::function<void(std::uint32_t)> choose_;

protected:
    double preferredContentHeight(double innerWidth) const override {
        double height = title_->measuredHeight(innerWidth, -1) + field_->measuredHeight(innerWidth, -1);
        const int visible = std::min(static_cast<int>(rows_.size()), kMaxVisibleRows);
        if (visible > 0) {
            height += kFieldGap + visible * rowExtent(innerWidth);
        }
        return height;
    }

    void layoutChildren() override {
        const double left = contentLeft();
        double top = contentTop();
        const double width = contentWidth();
        const double title = title_->measuredHeight(width, -1);
        title_->performLayout(left, top, width, title);
        top += title;
        const double field = field_->measuredHeight(width, -1);
        field_->performLayout(left, top, width, field);
        top += field + kFieldGap;
        const double row = rowExtent(width);
        rowHeight_ = row;
        const int count = static_cast<int>(rows_.size());
        const int visible = std::min(count, kMaxVisibleRows);
        if (ensure_ >= 0) {
            int start = static_cast<int>(scroll_);
            if (ensure_ < start) {
                start = ensure_;
            } else if (ensure_ >= start + visible) {
                start = ensure_ - visible + 1;
            }
            scroll_ = start;
            ensure_ = -1;
        }
        scroll_ = std::clamp(scroll_, 0, std::max(0, count - visible));
        for (int index = 0; index < count; ++index) {
            const auto& node = rows_[static_cast<std::size_t>(index)];
            const bool shown = index >= scroll_ && index < scroll_ + visible;
            node->setVisible(shown);
            if (shown) {
                node->performLayout(left, top + (index - scroll_) * row, width, row);
            } else {
                node->performLayout(0, 0, 0, 0);
            }
        }
    }

    void handleScroll(jadefx::ScrollEvent& event) override {
        const int count = static_cast<int>(rows_.size());
        if (count <= kMaxVisibleRows) {
            return;
        }
        const double delta = event.deltaY != 0.0 ? event.deltaY : event.deltaX;
        const int steps = std::max(1, static_cast<int>(std::round(std::fabs(delta))));
        scroll_ += delta > 0 ? -steps : steps;
        relayout();
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
        } else if (event.key == jadefx::Key::PageUp) {
            moveHighlight(-kMaxVisibleRows);
            event.consume();
        } else if (event.key == jadefx::Key::PageDown) {
            moveHighlight(kMaxVisibleRows);
            event.consume();
        }
    }

private:
    void rebuild() {
        for (const auto& row : rows_) {
            children().removeIf([&](const std::shared_ptr<jadefx::Node>& child) { return child.get() == row.get(); });
        }
        rows_.clear();
        const std::string query = field_->getText();
        shown_ = quick_open_filter(entries_, query, kMaxResults);
        selected_ = shown_.empty() ? -1 : 0;
        scroll_ = 0;
        ensure_ = selected_;
        for (int index = 0; index < static_cast<int>(shown_.size()); ++index) {
            auto row = std::make_shared<QuickOpenRow>(entries_[shown_[static_cast<std::size_t>(index)]], query);
            row->setOnMouseEntered([this, index](const jadefx::MouseEvent&) {
                if (selected_ != index) {
                    selected_ = index;
                    paint();
                }
            });
            row->setOnMousePressed([this, index](const jadefx::MouseEvent&) { choose(index); });
            children().add(row);
            rows_.push_back(std::move(row));
        }
        paint();
        place();
    }

    void paint() {
        const jadefx::Color highlight = theme_color("--ide-popup-selection-color");
        for (int index = 0; index < static_cast<int>(rows_.size()); ++index) {
            rows_[static_cast<std::size_t>(index)]->setBackground(index == selected_ ? highlight
                                                                                      : jadefx::Color::transparent());
        }
    }

    void moveHighlight(int delta) {
        const int count = static_cast<int>(shown_.size());
        if (count <= 0) {
            return;
        }
        const int next = std::clamp(selected_ < 0 ? 0 : selected_ + delta, 0, count - 1);
        if (next == selected_) {
            return;
        }
        selected_ = next;
        ensure_ = next;
        paint();
        relayout();
    }

    void choose(int index) {
        if (index < 0 || index >= static_cast<int>(shown_.size())) {
            return;
        }
        const std::uint32_t id = entries_[shown_[static_cast<std::size_t>(index)]].id;
        const std::function<void(std::uint32_t)> handler = choose_;
        dismiss();
        if (handler) {
            handler(id);
        }
    }

    // Centered across the top of the window, as wide as fits.
    void place() {
        jadefx::Scene* scene = scene_;
        if (scene == nullptr || scene->isTearingDown() || !scene->isPopupShowing(this)) {
            return;
        }
        const double sceneWidth = scene->getWidth();
        const double width = std::max(200.0, std::min(kMaxWidth, sceneWidth - 2 * kMargin));
        const double height = measuredHeight(width, -1);
        const double x = std::max(0.0, (sceneWidth - width) / 2);
        scene->movePopup(this, x, kTop, width, height);
    }

    void relayout() {
        if (getWidth() > 0.0 && getHeight() > 0.0) {
            performLayout(getX(), getY(), getWidth(), getHeight());
        }
    }

    double rowExtent(double innerWidth) const {
        if (rows_.empty()) {
            return 0;
        }
        const double height = rows_.front()->measuredHeight(std::max(0.0, innerWidth), -1);
        return height < 1.0 ? 22.0 : height;
    }

    std::shared_ptr<jadefx::Label> title_;
    std::shared_ptr<QuickOpenField> field_;
    std::vector<QuickOpenEntry> entries_;
    std::vector<std::size_t> shown_;
    std::vector<std::shared_ptr<QuickOpenRow>> rows_;
    jadefx::Scene* scene_ = nullptr;
    double rowHeight_ = 0;
    int selected_ = -1;
    int scroll_ = 0;
    int ensure_ = -1;
};

QuickOpen::QuickOpen() : list_(std::make_shared<List>()) { list_->self_ = list_; }

QuickOpen::~QuickOpen() { hide(); }

void QuickOpen::setOnChoose(std::function<void(std::uint32_t id)> handler) { list_->choose_ = std::move(handler); }

void QuickOpen::show(jadefx::Scene& scene, std::vector<QuickOpenEntry> entries) {
    list_->show(scene, std::move(entries));
}

void QuickOpen::hide() { list_->dismiss(); }

}  // namespace ide
