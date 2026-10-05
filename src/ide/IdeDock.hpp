#pragma once

#include "IdePane.hpp"

#include <cstdint>
#include <functional>
#include <memory>

namespace ide {

// A tab strip of IdePanes. The first docked page is selected.
// The dock's minimum size is the largest minimum of its pages, plus the tab strip.
class IdeDock : public jadefx::BorderPane {
public:
    IdeDock();

    // Adds the page at index, or last when index is past the end, and selects it.
    std::shared_ptr<jadefx::Tab> dock(const std::shared_ptr<IdePane>& pane, std::size_t index = SIZE_MAX);
    // Moves an existing tab onto this strip and selects it.
    void take(const std::shared_ptr<jadefx::Tab>& tab);
    void select(const IdePane* pane);

    jadefx::TabPane* tabs() const { return tabs_.get(); }
    bool empty() const;
    void syncMinimum();

    // The strip had a tab and now has none. Fired from layout, after the close returns.
    void setOnEmpty(std::function<void()> handler);
    void setOnTabDrag(std::function<void(const jadefx::TabDrag&)> handler);
    // The minimum no longer fits the space this dock was given.
    void setOnFit(std::function<void()> handler);

protected:
    void layoutChildren() override;

private:
    std::shared_ptr<jadefx::TabPane> tabs_;
    std::function<void()> onEmpty_;
    std::function<void()> onFit_;
    bool sawTab_ = false;
    bool queuedEmpty_ = false;
};

}  // namespace ide
