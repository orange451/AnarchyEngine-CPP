#include "IdeDock.hpp"

namespace ide {

IdeDock::IdeDock() {
    tabs_ = jadefx::make<jadefx::TabPane>();
    tabs_->setTabClosingPolicy(jadefx::TabPane::TabClosingPolicy::AllTabs);
    Fill(*tabs_);
    setCenter(tabs_);
}

void IdeDock::dock(const std::shared_ptr<IdePane>& pane) {
    if (!pane || !tabs_) {
        return;
    }

    auto tab = jadefx::make<jadefx::Tab>(pane->name(), pane);
    tab->setClosable(pane->closable());
    if (!pane->closable()) {
        tab->setOnCloseRequest([](jadefx::TabCloseRequest& request) { request.consume(); });
    }
    std::shared_ptr<IdePane> page = pane;
    std::shared_ptr<jadefx::Tab> opened = tab;
    tab->setOnSelectionChanged([page, opened] {
        if (opened->isSelected()) {
            page->onOpen();
        } else {
            page->onClose();
        }
    });
    tabs_->getTabs().add(std::move(tab));
}

}  // namespace ide
