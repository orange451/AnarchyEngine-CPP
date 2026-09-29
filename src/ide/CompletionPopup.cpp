#include "CompletionPopup.hpp"
#include "LockWaits.hpp"

#include "DataModelLock.hpp"
#include "Engine.hpp"
#include "IdeTheme.hpp"
#include "LuaSource.hpp"
#include "Utf8.hpp"

#include "jadefx/jadefx.hpp"
#include "jadefx/scene/controls/ScrollTrack.hpp"
#include "jadefx/scene/text/Font.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ide {
namespace {


// Text drawn bold the way the code area draws it: struck twice, the second
// time a little to the right. Open Sans has no bold face loaded.
class BoldText : public jadefx::Controls {
public:
    BoldText(const std::string& text, const jadefx::Color& fill) {
        setMouseTransparent(true);
        for (auto& strike : strikes_) {
            strike = jadefx::make<jadefx::Label>(text);
            strike->setTextFill(fill);
            strike->setMouseTransparent(true);
            children().add(strike);
        }
    }

    const char* getElementType() const override { return "bold-text"; }

protected:
    double preferredContentWidth(double innerAvailable) const override {
        return strikes_[0]->measuredWidth(innerAvailable) + kOffset;
    }
    double preferredContentHeight(double innerWidth) const override {
        return strikes_[0]->measuredHeight(innerWidth, -1);
    }
    void layoutChildren() override {
        const double width = std::max(0.0, contentWidth() - kOffset);
        strikes_[0]->performLayout(contentLeft(), contentTop(), width, contentHeight());
        strikes_[1]->performLayout(contentLeft() + kOffset, contentTop(), width, contentHeight());
    }

private:
    static constexpr double kOffset = 0.6;
    std::shared_ptr<jadefx::Label> strikes_[2];
};

constexpr int kMaxVisibleRows = 8;
constexpr double kPopupWidth = 420.0;
// Read from the theme each time a popup is built or repainted.
jadefx::Color RowHighlight() { return theme_color("--ide-popup-selection-color"); }
jadefx::Color HeaderFill() { return theme_color("--ide-popup-header-color"); }
jadefx::Color HeaderText() { return theme_color("--ide-popup-header-text-color"); }
jadefx::Color DetailText() { return theme_color("--ide-popup-detail-text-color"); }
jadefx::Color TitleText() { return theme_color("--ide-popup-text-color"); }
// Popup is 420 wide, with 4px padding and a 1px border. The note adds 8px on each side.
constexpr double kDocTextWidth = 386.0;

// Break `text` into lines that fit `width`. A newline already in the text starts a line.
std::string WrapToWidth(const std::string& text, double width) {
    if (text.empty()) {
        return {};
    }
    const jadefx::Font font;
    std::string wrapped;
    std::string line;
    std::size_t index = 0;
    auto append = [&] {
        if (!wrapped.empty()) {
            wrapped.push_back('\n');
        }
        wrapped += line;
        line.clear();
    };
    while (index < text.size()) {
        if (text[index] == '\n') {
            append();
            ++index;
            continue;
        }
        const std::size_t start = index;
        while (index < text.size() && text[index] != ' ' && text[index] != '\n') {
            ++index;
        }
        const std::string word = text.substr(start, index - start);
        while (index < text.size() && text[index] == ' ') {
            ++index;
        }
        if (word.empty()) {
            continue;
        }
        const std::string trial = line.empty() ? word : line + " " + word;
        if (width > 8.0 && static_cast<double>(font.measureWidth(trial)) > width && !line.empty()) {
            append();
            line = word;
            continue;
        }
        line = trial;
    }
    if (!line.empty()) {
        append();
    }
    return wrapped;
}

// The gray text beside a name. A function shows what it returns.
std::string RowDetail(const CompletionItem& item) {
    if (item.returns.empty()) {
        return item.detail;
    }
    if (!item.detail.empty() && item.detail.front() == '(') {
        if (item.returns == "returns nothing") {
            return item.detail + " -> ()";
        }
        return item.detail + " -> " + item.returns;
    }
    if (item.detail == "function" || item.detail.empty()) {
        if (item.returns == "returns nothing") {
            return "returns nothing";
        }
        return "-> " + item.returns;
    }
    return item.detail;
}

std::shared_ptr<jadefx::VBox> MakeDocs(const CompletionItem& item) {
    if (item.title.empty() && item.summary.empty()) {
        return nullptr;
    }
    auto box = jadefx::make<jadefx::VBox>();
    box->setSpacing(2);
    box->setPadding(jadefx::Insets{6, 8, 6, 8});
    box->setAlignment(jadefx::Pos::TopLeft);
    box->setBackground(HeaderFill());
    auto add = [&](const std::string& text, const jadefx::Color& fill) {
        if (text.empty()) {
            return;
        }
        auto label = jadefx::make<jadefx::Label>(WrapToWidth(text, kDocTextWidth));
        label->setTextFill(fill);
        label->setMouseTransparent(true);
        box->getChildren().add(label);
    };
    add(item.title, TitleText());
    if (item.returns == "returns nothing") {
        add("returns nothing", DetailText());
    } else if (item.returns.empty() && (item.detail == "library" || item.detail == "type")) {
        add(item.detail, DetailText());
    }
    add(item.summary, DetailText());
    return box;
}

class CompletionListPopup;

class CompletionScrollTrack : public jadefx::Region {
public:
    explicit CompletionScrollTrack(CompletionListPopup& popup) : popup_(&popup) { setDefaultCursor(jadefx::Cursor::Default); }

    const char* getElementType() const override { return "scroll-bar"; }

    void clear() { popup_ = nullptr; }

protected:
    void handleMousePressed(const jadefx::MouseEvent& event) override;
    void handleMouseDragged(const jadefx::MouseEvent& event) override;
    void handleMouseReleased(const jadefx::MouseEvent&) override;
    void renderContent(jadefx::UiRenderer& renderer, float opacity) override;

private:
    CompletionListPopup* popup_ = nullptr;
};

// The list under the caret. Its height follows the rows, and stops at eight.
// Further rows scroll. The signature stays above that window. The highlighted
// row's return and explanation stay below it.
class CompletionListPopup : public jadefx::Controls {
    friend class CompletionScrollTrack;

public:
    CompletionListPopup(std::function<void(int)> activate, std::function<void(int)> reselect,
                        std::function<void()> refocus, std::function<void()> resized)
        : activate_(std::move(activate)), reselect_(std::move(reselect)), refocus_(std::move(refocus)),
          resized_(std::move(resized)) {
        setPrefWidth(kPopupWidth);
        setDefaultCursor(jadefx::Cursor::Default);
        setStyle(
            "background-color: var(--ide-popup-color); border-style: solid; border-width: 1px; "
            "border-color: var(--ide-popup-border-color); box-shadow: 0 2px 8px var(--ide-popup-shadow-color); "
            "padding: 4px;");
        track_ = std::make_shared<CompletionScrollTrack>(*this);
        children().add(track_);
        if (refocus_) {
            setOnMousePressed([this](const jadefx::MouseEvent&) { refocus_(); });
        }
    }

    ~CompletionListPopup() override {
        if (track_) {
            track_->clear();
        }
    }

    const char* getElementType() const override { return "completion-popup"; }

    // signature[bold_begin, bold_end) is the parameter being typed, drawn bold.
    void showList(const std::string& signature, int bold_begin, int bold_end, const std::vector<CompletionItem>& items,
                  int selected) {
        rows_.clear();
        header_.reset();
        docs_.reset();
        items_ = items;
        children().clear();
        if (!signature.empty()) {
            header_ = jadefx::make<jadefx::HBox>();
            header_->setPadding(jadefx::Insets{4, 8, 4, 8});
            header_->setAlignment(jadefx::Pos::CenterLeft);
            header_->setBackground(HeaderFill());
            const auto add = [this](const std::string& text) {
                if (text.empty()) {
                    return;
                }
                auto label = jadefx::make<jadefx::Label>(text);
                label->setTextFill(HeaderText());
                label->setMouseTransparent(true);
                header_->getChildren().add(std::move(label));
            };
            const int size = static_cast<int>(signature.size());
            if (bold_begin >= 0 && bold_begin < bold_end && bold_end <= size) {
                const auto begin = static_cast<std::size_t>(bold_begin);
                const auto end = static_cast<std::size_t>(bold_end);
                add(signature.substr(0, begin));
                header_->getChildren().add(jadefx::make<BoldText>(signature.substr(begin, end - begin), HeaderText()));
                add(signature.substr(end));
            } else {
                add(signature);
            }
            children().add(header_);
        }
        const int count = static_cast<int>(items.size());
        if (selected < 0 || selected >= count) {
            selected = count > 0 ? 0 : -1;
        }
        selected_ = selected;
        rows_.reserve(static_cast<std::size_t>(std::max(0, count)));
        for (int index = 0; index < count; ++index) {
            const CompletionItem& item = items[static_cast<std::size_t>(index)];
            auto row = jadefx::make<jadefx::HBox>();
            row->setSpacing(16);
            row->setPadding(jadefx::Insets{3, 8, 3, 8});
            row->setAlignment(jadefx::Pos::CenterLeft);
            if (index == selected_) {
                row->setBackground(RowHighlight());
            }
            auto name = jadefx::make<jadefx::Label>(item.name);
            auto detail = jadefx::make<jadefx::Label>(RowDetail(item));
            detail->setTextFill(DetailText());
            name->setMouseTransparent(true);
            detail->setMouseTransparent(true);
            row->getChildren().add(name);
            row->getChildren().add(detail);
            row->setOnMousePressed([this, index](const jadefx::MouseEvent&) {
                selected_ = index;
                if (activate_) {
                    activate_(index);
                }
            });
            rows_.push_back(row);
            children().add(row);
        }
        attachDocs(selected_);
        if (track_) {
            children().add(track_);
        }
        scrollDrag_ = false;
        // Bring the highlighted row into the window on the next layout, once row height is known.
        ensure_ = selected_;
    }

protected:
    double preferredContentHeight(double innerWidth) const override {
        const int count = static_cast<int>(rows_.size());
        const int visible = std::min(count, kMaxVisibleRows);
        return headerHeight(innerWidth) + static_cast<double>(visible) * rowExtent(innerWidth) + docsHeight(innerWidth);
    }

    void layoutChildren() override {
        const double left = contentLeft();
        const double top = contentTop();
        const double width = contentWidth();
        const double row = rowExtent(width);
        rowHeight_ = row;
        const int count = static_cast<int>(rows_.size());
        const int visible = std::min(count, kMaxVisibleRows);
        double header = 0;
        if (header_) {
            header = headerHeight(width);
            header_->setVisible(true);
            header_->performLayout(left, top, width, header);
        }
        if (ensure_ >= 0) {
            reveal(ensure_, row, count, visible);
            ensure_ = -1;
        }
        clampScroll(row, count, visible);
        const bool bars = count > kMaxVisibleRows && visible > 0 && row > 0.0;
        const double gutter = bars ? static_cast<double>(jadefx::ScrollTrack::kThickness) : 0.0;
        const double rowWidth = std::max(0.0, width - gutter);
        const int start = windowStart(row, count, visible);
        const double listTop = top + header;
        for (int index = 0; index < count; ++index) {
            const std::shared_ptr<jadefx::HBox>& node = rows_[static_cast<std::size_t>(index)];
            const bool shown = index >= start && index < start + visible;
            node->setVisible(shown);
            if (!shown || row <= 0.0) {
                node->performLayout(0, 0, 0, 0);
                continue;
            }
            const double y = listTop + static_cast<double>(index - start) * row;
            node->performLayout(left, y, rowWidth, row);
        }
        placeDocs(left, listTop + static_cast<double>(visible) * row, width);
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

private:
    double headerHeight(double innerWidth) const {
        if (!header_) {
            return 0;
        }
        return header_->measuredHeight(std::max(0.0, innerWidth), -1);
    }

    double docsHeight(double innerWidth) const {
        if (!docs_) {
            return 0;
        }
        return docs_->measuredHeight(std::max(0.0, innerWidth), -1);
    }

    void placeDocs(double left, double y, double width) {
        if (!docs_) {
            return;
        }
        const double height = docsHeight(width);
        docs_->setVisible(true);
        docs_->performLayout(left, y, width, height);
    }

    void attachDocs(int index) {
        docs_.reset();
        if (index < 0 || index >= static_cast<int>(items_.size())) {
            return;
        }
        docs_ = MakeDocs(items_[static_cast<std::size_t>(index)]);
        if (!docs_) {
            return;
        }
        if (refocus_) {
            docs_->setOnMousePressed([this](const jadefx::MouseEvent&) { refocus_(); });
        }
        children().add(docs_);
    }

    void refreshDocs(int index) {
        if (docs_) {
            const std::shared_ptr<jadefx::Node> previous = docs_;
            children().removeIf([&](const std::shared_ptr<jadefx::Node>& node) { return node == previous; });
            docs_.reset();
        }
        attachDocs(index);
    }

    // The highlight moved with the scroll. The note follows it, and the popup grows to fit.
    void followSelection(int before) {
        if (following_ || selected_ == before) {
            return;
        }
        following_ = true;
        refreshDocs(selected_);
        relayout();
        if (resized_) {
            resized_();
        }
        following_ = false;
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
        const int before = selected_;
        const double magnitude = std::fabs(deltaY);
        double pixels = deltaY;
        // A mouse notch is about ±1. Anything else is already a pixel distance.
        if (magnitude > 0.0 && std::fabs(magnitude - 1.0) <= 0.2) {
            pixels = std::copysign(rowHeight_, deltaY);
        }
        // A negative delta reveals later rows, matching TreeView and ComboBox.
        scroll_ -= pixels;
        clampScroll(rowHeight_, count, visible);
        keepSelectionVisible();
        followSelection(before);
        if (selected_ == before) {
            relayout();
        }
    }

    void keepSelectionVisible() {
        if (rowHeight_ <= 0.0 || rows_.empty() || !reselect_) {
            return;
        }
        const int count = static_cast<int>(rows_.size());
        const int visible = std::min(count, kMaxVisibleRows);
        const int start = windowStart(rowHeight_, count, visible);
        int next = selected_;
        if (next < start) {
            next = start;
        }
        if (next >= start + visible) {
            next = start + visible - 1;
        }
        if (next == selected_ || next < 0 || next >= count) {
            return;
        }
        selected_ = next;
        for (int index = 0; index < count; ++index) {
            rows_[static_cast<std::size_t>(index)]->setBackground(index == next ? RowHighlight()
                                                                                  : jadefx::Color::transparent());
        }
        reselect_(next);
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
        const int before = selected_;
        scroll_ = bar_.offsetFromPage(scroll_, where == jadefx::ScrollTrack::Part::After);
        const int count = static_cast<int>(rows_.size());
        clampScroll(rowHeight_, count, std::min(count, kMaxVisibleRows));
        keepSelectionVisible();
        scrollGrab_ = bar_.thumbLength * 0.5f;
        followSelection(before);
        if (selected_ == before) {
            relayout();
        }
    }

    void dragScroll(const jadefx::MouseEvent& event) {
        if (!scrollDrag_) {
            return;
        }
        const int before = selected_;
        const float localX = static_cast<float>(event.x - getAbsoluteX());
        const float localY = static_cast<float>(event.y - getAbsoluteY());
        scroll_ = bar_.offsetFromDrag(localX, localY, scrollGrab_);
        const int count = static_cast<int>(rows_.size());
        clampScroll(rowHeight_, count, std::min(count, kMaxVisibleRows));
        keepSelectionVisible();
        followSelection(before);
        if (selected_ == before) {
            relayout();
        }
    }

    void releaseScroll() { scrollDrag_ = false; }

    void paintScroll(jadefx::UiRenderer& renderer, float opacity) const {
        bar_.draw(renderer, static_cast<float>(getAbsoluteX()), static_cast<float>(getAbsoluteY()), opacity);
    }

    std::function<void(int)> activate_;
    std::function<void(int)> reselect_;
    std::function<void()> refocus_;
    std::function<void()> resized_;
    std::vector<CompletionItem> items_;
    std::shared_ptr<jadefx::HBox> header_;
    std::shared_ptr<jadefx::VBox> docs_;
    std::vector<std::shared_ptr<jadefx::HBox>> rows_;
    std::shared_ptr<CompletionScrollTrack> track_;
    jadefx::ScrollTrack bar_{};
    double scroll_ = 0;
    double rowHeight_ = 0;
    float scrollGrab_ = 0;
    int selected_ = -1;
    int ensure_ = -1;
    bool scrollDrag_ = false;
    bool following_ = false;
};

void CompletionScrollTrack::handleMousePressed(const jadefx::MouseEvent& event) {
    if (popup_ != nullptr) {
        popup_->pressScroll(event);
    }
}

void CompletionScrollTrack::handleMouseDragged(const jadefx::MouseEvent& event) {
    if (popup_ != nullptr) {
        popup_->dragScroll(event);
    }
}

void CompletionScrollTrack::handleMouseReleased(const jadefx::MouseEvent&) {
    if (popup_ != nullptr) {
        popup_->releaseScroll();
    }
}

void CompletionScrollTrack::renderContent(jadefx::UiRenderer& renderer, float opacity) {
    if (popup_ != nullptr) {
        popup_->paintScroll(renderer, opacity);
    }
}

}  // namespace

struct CompletionPopup::State {
    std::shared_ptr<CompletionListPopup> popup;
    std::vector<CompletionItem> items;
    CompleteSite site = CompleteSite::None;
    int selected = 0;
    int replace_begin = 0;
    int replace_end = 0;
    std::string prefix;
    std::string signature;
    int signature_bold_begin = -1;
    int signature_bold_end = -1;
    char close_quote = 0;
    bool unclosed = false;
    bool picked = false;
    bool accepting = false;
    std::uint64_t dismissals = 0;
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
    if (item == nullptr || state_->site != CompleteSite::Name || item->snippet) {
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

bool CompletionPopup::commitsQuote(char quote, bool unclosed_only) const {
    const CompletionItem* item = highlighted();
    if (item == nullptr || state_->site != CompleteSite::Argument) {
        return false;
    }
    if (unclosed_only && !state_->unclosed) {
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
    ++state_->dismissals;
    state_->picked = false;
    state_->prefix.clear();
    state_->signature.clear();
    state_->signature_bold_begin = -1;
    state_->signature_bold_end = -1;
    state_->items.clear();
    state_->close_quote = 0;
    state_->unclosed = false;
    if (state_->popup && state_->owner != nullptr && state_->owner->getScene() != nullptr &&
        !state_->owner->getScene()->isTearingDown() && state_->owner->getScene()->isPopupShowing(state_->popup.get())) {
        state_->owner->getScene()->hidePopup(state_->popup.get());
    }
}

std::uint64_t CompletionPopup::dismissals() const { return state_->dismissals; }

void CompletionPopup::narrow(const CompletionList& frame, jadefx::Node& owner, double caret_x, double caret_y,
                             double caret_height) {
    if (!isOpen() || state_->accepting) {
        return;
    }
    if (frame.site != state_->site || frame.replace_begin != state_->replace_begin) {
        dismiss();
        return;
    }
    CompletionList kept = frame;
    kept.items.clear();
    for (const CompletionItem& item : state_->items) {
        if (item.name.compare(0, frame.prefix.size(), frame.prefix) == 0) {
            kept.items.push_back(item);
        }
    }
    kept.signature = state_->signature;
    kept.signature_bold_begin = state_->signature_bold_begin;
    kept.signature_bold_end = state_->signature_bold_end;
    present(kept, false, owner, caret_x, caret_y, caret_height);
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
    const std::string& written = chosen.insert.empty() ? chosen.name : chosen.insert;
    CompletionEdit edit;
    edit.begin = begin;
    edit.end = end;
    edit.text = written;
    edit.caret = begin + CodePoints(written);
    if (parentheses && chosen.call && CodePointAt(text, end) != U'(') {
        edit.text += "()";
        edit.caret = begin + CodePoints(written) + 1;
    }
    if (state_->close_quote != 0 && !chosen.call) {
        const auto quote = static_cast<char32_t>(static_cast<unsigned char>(state_->close_quote));
        if (CodePointAt(text, end) == quote) {
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
        state_->popup = std::make_shared<CompletionListPopup>(
            [this](int index) {
                state_->selected = index;
                if (state_->on_accept) {
                    state_->on_accept();
                }
            },
            [this](int index) {
                state_->selected = index;
                state_->picked = true;
            },
            [this] {
                if (state_->owner != nullptr) {
                    state_->owner->requestFocus();
                }
            },
            [this] {
                if (state_->owner != nullptr) {
                    place(*state_->owner, state_->caret_x, state_->caret_y, state_->caret_height);
                }
            });
    }
    state_->popup->showList(state_->signature, state_->signature_bold_begin, state_->signature_bold_end, state_->items,
                            state_->selected);
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
    double x = caret_x;
    double y = caret_y + caret_height + 2;
    // Size from the rows just built. movePopup locks the size it is given, so a
    // later list must be measured again or the box stays at the previous height.
    scene->showPopup(state_->popup, x, y, -1, -1, options);
    double width = state_->popup->getWidth();
    double height = state_->popup->getHeight();
    if (width < 1) {
        width = kPopupWidth;
    }
    if (height < 1) {
        height = 28;
    }
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
    if (list.site == CompleteSite::None || (list.items.empty() && list.signature.empty())) {
        dismiss();
        return;
    }
    // An empty name, or a name nothing extends, stays closed. A call that knows
    // its parameters still shows that list, without every in-scope name.
    // A finished directive matches exactly and closes the same way. `--!` has an
    // empty prefix and still opens, because the bang is what asked for the list.
    // A value whose type names its strings or keys opens with those rows alone.
    bool signature_only = false;
    bool expected_only = false;
    const bool extends = std::any_of(list.items.begin(), list.items.end(), [&](const CompletionItem& item) {
        return item.name.size() > list.prefix.size();
    });
    if (list.site == CompleteSite::Name && !force && (list.prefix.empty() || !extends)) {
        if (list.prefix.empty() && list.open_expected) {
            expected_only = true;
        } else if (list.signature.empty()) {
            dismiss();
            return;
        }
        signature_only = true;
    } else if (list.site == CompleteSite::Directive && !force && !list.prefix.empty() && !extends) {
        dismiss();
        return;
    }
    std::string previous;
    const bool keep_pick = open && state_->picked && state_->site == list.site && state_->selected >= 0 &&
                           state_->selected < static_cast<int>(state_->items.size());
    if (keep_pick) {
        previous = state_->items[static_cast<std::size_t>(state_->selected)].name;
    }
    state_->items.clear();
    if (expected_only) {
        std::copy_if(list.items.begin(), list.items.end(), std::back_inserter(state_->items),
                     [](const CompletionItem& item) { return item.expected; });
    } else if (!signature_only) {
        state_->items = list.items;
    }
    state_->site = list.site;
    state_->prefix = list.prefix;
    state_->signature = list.signature;
    state_->signature_bold_begin = list.signature_bold_begin;
    state_->signature_bold_end = list.signature_bold_end;
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

const std::vector<engine_core::LuaNode>& completion_world(engine_core::Engine& engine, std::uint32_t script_id,
                                                          const std::string* buffer, CompletionWorldCache& cache) {
    engine_core::DataModel& game = engine.datamodel();
    const std::uint64_t tree = game.tree_revision();
    const std::uint64_t authored = game.authored_revision();
    const std::uint64_t sources = game.source_revision();
    // The last full read of any place, by world, for a new cache whose first
    // read cannot take the lock. UI thread only.
    static const void* shared_world = nullptr;
    static std::vector<engine_core::LuaNode> shared_nodes;
    const bool same_place = cache.world == &game && cache.tree == tree && cache.authored == authored;
    if (same_place && cache.sources != sources) {
        engine_core::DataModelLock lock(game, engine_core::DataModelLock::Read, kActionLockWait);
        if (lock.owns()) {
            for (std::size_t index = 0; index < cache.nodes.size(); ++index) {
                if (cache.versions[index] == 0) {
                    continue;
                }
                const auto* script = dynamic_cast<const engine_core::LuaSource*>(game.instance(cache.nodes[index].id));
                if (script != nullptr && script->source_version() != cache.versions[index]) {
                    cache.nodes[index].source = script->source();
                    cache.versions[index] = script->source_version();
                }
            }
            cache.sources = sources;
        }
    } else if (!same_place) {
        engine_core::DataModelLock lock(game, engine_core::DataModelLock::Read, kActionLockWait);
        if (lock.owns()) {
            // The revisions were read before the lock. A change that lands in
            // between is read now and stamped older, so the next call reads once
            // more; it is never missed.
            cache.nodes.clear();
            cache.versions.clear();
            std::vector<engine_core::LuaNode>& nodes = cache.nodes;
            engine_core::LuaNode root;
            root.id = 0;
            root.parent = engine_core::DataModel::kNoParent;
            root.name = game.name(0);
            root.class_name = game.class_name();
            nodes.push_back(std::move(root));
            cache.versions.push_back(0);
            // The tree in sibling order, then any instance outside it with what is under
            // it, so children keep the order FindFirstChild walks, as analysis sees it.
            std::unordered_set<engine_core::InstanceId> placed{0};
            std::vector<engine_core::InstanceId> stack;
            const auto add = [&](engine_core::InstanceId id) {
                engine_core::DataModel* object = game.instance(id);
                if (object == nullptr || !placed.insert(id).second) {
                    return;
                }
                engine_core::LuaNode node;
                node.id = id;
                node.parent = game.parent(id);
                node.name = game.name(id);
                const char* class_name = object->class_name();
                node.class_name = class_name != nullptr ? class_name : "";
                std::uint64_t version = 0;
                if (auto* source = dynamic_cast<engine_core::LuaSource*>(object)) {
                    node.source = source->source();
                    version = source->source_version();
                }
                nodes.push_back(std::move(node));
                cache.versions.push_back(version);
            };
            const auto walk = [&](engine_core::InstanceId top) {
                stack.push_back(top);
                while (!stack.empty()) {
                    const engine_core::InstanceId id = stack.back();
                    stack.pop_back();
                    if (id != 0) {
                        add(id);
                    }
                    std::vector<engine_core::InstanceId> children;
                    for (engine_core::InstanceId child = game.first_child(id); child != 0;
                         child = game.next_sibling(child)) {
                        children.push_back(child);
                    }
                    for (auto child = children.rbegin(); child != children.rend(); ++child) {
                        stack.push_back(*child);
                    }
                }
            };
            walk(0);
            game.for_each_instance([&](engine_core::DataModel& object) {
                if (placed.count(object.id()) == 0 && game.parent(object.id()) == engine_core::DataModel::kNoParent) {
                    walk(object.id());
                }
            });
            game.for_each_instance([&](engine_core::DataModel& object) { add(object.id()); });
            cache.world = &game;
            cache.tree = tree;
            cache.authored = authored;
            cache.sources = sources;
            shared_world = &game;
            shared_nodes = cache.nodes;
        } else if (cache.world != &game && shared_world == &game) {
            // Stamped as never read, so the next call reads the place itself.
            cache.nodes = shared_nodes;
            cache.versions.assign(cache.nodes.size(), 0);
        }
    }
    if (buffer != nullptr) {
        for (engine_core::LuaNode& node : cache.nodes) {
            if (node.id == script_id) {
                node.source = *buffer;
            }
        }
    }
    return cache.nodes;
}

}  // namespace ide
