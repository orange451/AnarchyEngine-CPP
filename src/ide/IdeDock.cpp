#include "IdeDock.hpp"

#include "IdeIcons.hpp"
#include "jadefx/scene/controls/TabPane.hpp"

#include <algorithm>
#include <cmath>

namespace ide {
namespace {

bool HorizontalTabs(const jadefx::TabPane& tabs) {
    const jadefx::Side side = tabs.getSide();
    return side == jadefx::Side::Top || side == jadefx::Side::Bottom;
}

}  // namespace

IdeDock::IdeDock() {
    tabs_ = jadefx::make<jadefx::TabPane>();
    tabs_->setTabClosingPolicy(jadefx::TabPane::TabClosingPolicy::AllTabs);
    Fill(*tabs_);
    setCenter(tabs_);
}

std::shared_ptr<jadefx::Tab> IdeDock::dock(const std::shared_ptr<IdePane>& pane, std::size_t index) {
    if (!pane || !tabs_) {
        return nullptr;
    }

    auto tab = jadefx::make<jadefx::Tab>(pane->title(), pane);
    if (std::shared_ptr<jadefx::ImageView> icon = icon_graphic(pane->iconFile())) {
        tab->setGraphic(std::move(icon));
    }
    tab->setClosable(pane->closable());
    if (!pane->closable()) {
        tab->setOnCloseRequest([](jadefx::TabCloseRequest& request) { request.consume(); });
    }
    std::weak_ptr<IdePane> page = pane;
    std::weak_ptr<jadefx::Tab> weak = tab;
    std::shared_ptr<jadefx::Tab> opened = tab;
    // The tab moves between docks and windows whole, so this follows it there.
    pane->setOnTitle([weak](const std::string& title) {
        if (const std::shared_ptr<jadefx::Tab> live = weak.lock()) {
            live->setText(title);
        }
    });
    tab->setOnMenu([page](jadefx::Menu& menu) {
        if (const std::shared_ptr<IdePane> live = page.lock()) {
            live->fillTabMenu(menu);
        }
    });
    tab->setOnSelectionChanged([page, weak] {
        const std::shared_ptr<IdePane> live = page.lock();
        const std::shared_ptr<jadefx::Tab> current = weak.lock();
        if (!live || !current) {
            return;
        }
        if (current->isSelected()) {
            live->onOpen();
        } else {
            live->onClose();
        }
    });
    tabs_->getTabs().insert(index, std::move(tab));
    tabs_->select(opened);
    sawTab_ = true;
    queuedEmpty_ = false;
    syncMinimum();
    return opened;
}

void IdeDock::take(const std::shared_ptr<jadefx::Tab>& tab) {
    if (!tab || !tabs_) {
        return;
    }
    tabs_->getTabs().add(tab);
    tabs_->select(tab);
    sawTab_ = true;
    queuedEmpty_ = false;
    syncMinimum();
}

bool IdeDock::empty() const { return !tabs_ || tabs_->getTabs().empty(); }

void IdeDock::syncMinimum() {
    if (!tabs_) {
        return;
    }
    double width = 0;
    double height = 0;
    for (const std::shared_ptr<jadefx::Tab>& tab : tabs_->getTabs().items()) {
        const jadefx::Node* content = tab ? tab->getContent() : nullptr;
        if (content == nullptr) {
            continue;
        }
        width = std::max(width, content->getMinWidth());
        height = std::max(height, content->getMinHeight());
    }
    const double header = tabs_->headerExtent();
    if (HorizontalTabs(*tabs_)) {
        height += header;
    } else {
        width += header;
    }
    if (std::fabs(getMinWidth() - width) > 0.5 || std::fabs(getMinHeight() - height) > 0.5) {
        setMinSize(width, height);
    }
}

void IdeDock::setOnEmpty(std::function<void()> handler) { onEmpty_ = std::move(handler); }

void IdeDock::setOnTabDrag(std::function<void(const jadefx::TabDrag&)> handler) {
    if (tabs_) {
        tabs_->setOnTabDrag(std::move(handler));
    }
}

void IdeDock::setOnFit(std::function<void()> handler) { onFit_ = std::move(handler); }

void IdeDock::layoutChildren() {
    const double previousWidth = getMinWidth();
    const double previousHeight = getMinHeight();
    const bool hadTabs = sawTab_;
    if (tabs_ && !tabs_->getTabs().empty()) {
        sawTab_ = true;
        queuedEmpty_ = false;
    } else if (hadTabs && !queuedEmpty_) {
        queuedEmpty_ = true;
        if (onEmpty_) {
            onEmpty_();
        }
    }
    syncMinimum();
    BorderPane::layoutChildren();
    const bool minChanged = std::fabs(getMinWidth() - previousWidth) > 0.5 || std::fabs(getMinHeight() - previousHeight) > 0.5;
    const bool shortOfMin = getWidth() + 1.0 < getMinWidth() || getHeight() + 1.0 < getMinHeight();
    if ((minChanged || shortOfMin) && onFit_) {
        onFit_();
    }
}

void IdeDock::select(const IdePane* pane) {
    if (pane == nullptr || !tabs_) {
        return;
    }
    for (const std::shared_ptr<jadefx::Tab>& tab : tabs_->getTabs().items()) {
        if (tab && tab->getContent() == pane) {
            tabs_->select(tab);
            return;
        }
    }
}

}  // namespace ide
