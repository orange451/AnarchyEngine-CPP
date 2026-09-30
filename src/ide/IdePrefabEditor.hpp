#pragma once

#include "IdePane.hpp"

#include "DataModel.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace ide {

// A page for editing one Prefab, docked like a script editor. Edit on a
// Prefab, or a double-click on one in an explorer or the Assets pane, opens
// it; editing a Prefab that already has one brings that one forward. The tab
// shows the Prefab's name and follows a rename. For now the page only says
// which Prefab it edits, or that the Prefab is gone.
class IdePrefabEditor : public IdePane {
public:
    IdePrefabEditor(engine_core::DataModel& world, engine_core::InstanceId prefab);

    engine_core::InstanceId prefab() const { return prefab_; }

protected:
    void layoutChildren() override;

private:
    // Reads the Prefab's name when the tree moved since the last read.
    void refresh();

    engine_core::DataModel& world_;
    engine_core::InstanceId prefab_;
    std::shared_ptr<jadefx::Label> heading_;
    std::shared_ptr<jadefx::Label> detail_;
    // The tree revision the name was read at.
    std::uint64_t seen_tree_ = ~std::uint64_t{0};
};

}  // namespace ide
