#pragma once

#include "IdePane.hpp"

#include <memory>

namespace ide {

// A tab strip of IdePanes. The first docked page is selected.
class IdeDock : public jadefx::BorderPane {
public:
    IdeDock();

    void dock(const std::shared_ptr<IdePane>& pane);

private:
    std::shared_ptr<jadefx::TabPane> tabs_;
};

}  // namespace ide
