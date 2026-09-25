#include "IdeDock.hpp"

namespace ide {

IdeDock::IdeDock() {
    tabs_ = jadefx::make<jadefx::TabPane>();
    tabs_->setTabClosingPolicy(jadefx::TabPane::TabClosingPolicy::AllTabs);
    Fill(*tabs_);
    setCenter(tabs_);
}

std::shared_ptr<jadefx::Tab> IdeDock::dock(const std::shared_ptr<IdePane>& pane) {
    if (!pane || !tabs_) {
        return nullptr;
    }

    auto tab = jadefx::make<jadefx::Tab>(pane->name(), pane);
    tab->setClosable(pane->closable());
    if (!pane->closable()) {
        tab->setOnCloseRequest([](jadefx::TabCloseRequest& request) { request.consume(); });
    }
    std::weak_ptr<IdePane> page = pane;
    std::weak_ptr<jadefx::Tab> weak = tab;
    std::shared_ptr<jadefx::Tab> opened = tab;
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
    tabs_->getTabs().add(std::move(tab));
    tabs_->select(opened);
    return opened;
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
