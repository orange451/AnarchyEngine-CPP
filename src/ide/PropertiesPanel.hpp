#pragma once

#include "IdePane.hpp"
#include "PropertySheet.hpp"

#include <functional>
#include <memory>
#include <string>

namespace engine_core {
class ChangeHistoryService;
class DataModel;
class SelectionService;
}  // namespace engine_core

namespace ide {

// Runs a write where the DataModel may be written: the simulation thread in
// the studio. The default calls it here, which is right while the engine
// threads are not running.
using PropertiesRun = std::function<void(std::function<void(engine_core::DataModel&)>)>;

// The Properties page. It edits the properties every selected instance has,
// read from the class registry, so a new class property shows up without a
// change here. The selection is the world's SelectionService.
//
// A row whose instances disagree is mixed and its editor is blank, not the
// first instance's value. A commit writes the typed value to every selected
// instance as one undo step, "Set <Property>". A blank mixed field that was
// never typed in commits nothing; typing and then clearing it writes "".
// Enter, leaving the field, a checkbox click, and a pick commit. Escape
// cancels. A value changed elsewhere shows at once, except in a field that is
// focused and typed in; that one resyncs when it commits or cancels.
//
// A reference row shows the instance's Name, with its path as a tooltip.
// Clicking the Name waits for the next selection change, such as a click in
// the explorer, uses the instance picked, and puts the selection back.
// Clicking it again cancels. Clear sets nil. A Parent that
// would put an instance under itself is refused.
class PropertiesPanel {
public:
    PropertiesPanel();
    ~PropertiesPanel();

    PropertiesPanel(const PropertiesPanel&) = delete;
    PropertiesPanel& operator=(const PropertiesPanel&) = delete;

    void bind(engine_core::DataModel& world, engine_core::SelectionService& selection,
              engine_core::ChangeHistoryService& history);
    void set_runner(PropertiesRun run);
    // Reads the selection and the world now and updates the rows. The page
    // also does this on every layout.
    void rebuild();
    // The page to dock. Its tab reads "Properties".
    std::shared_ptr<IdePane> dock_widget() const;

    // True when node is on this page.
    bool owns(const jadefx::Node* node) const;
    // Undo or redo inside the focused field's own typing. False when no field
    // here is focused or it has nothing to undo, so the place stack gets it.
    bool field_undo(bool redo);

    // The widget for a row. part picks the Vector3 axis, or for a reference
    // 0 the Name that picks and 1 Clear. Null when there is no such row.
    jadefx::Node* editor(const std::string& property, int part = 0) const;

    // What the rows show now.
    const PropertySheet& sheet() const;
    // Waiting for a pick, and for which property.
    bool picking() const;
    const std::string& pick_property() const;
    // The last message under the rows: a refused edit, or pick instructions.
    const std::string& status() const;

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

}  // namespace ide
