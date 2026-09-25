#pragma once

#include "IdePane.hpp"

#include <memory>

namespace ide {

// A tab strip of IdePanes. The first docked page is selected.
class IdeDock : public jadefx::BorderPane {
public:
    IdeDock();

    // Adds the page and selects it.
    std::shared_ptr<jadefx::Tab> dock(const std::shared_ptr<IdePane>& pane);
    void select(const IdePane* pane);

private:
    std::shared_ptr<jadefx::TabPane> tabs_;
};

}  // namespace ide
