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
// Focusing a field selects its value. Tab and Shift+Tab commit the field and
// move to the next or previous one that can be typed in, a Vector3's X, Y, and
// Z in turn, wrapping at the ends. Position's axes are tinted red, green, and
// blue by the theme's --ide-properties-{x,y,z}-color.
//
// A Matrix4, such as a GameObject's Transform, is shown as its name with
// an arrow, then a Position line and an Orientation
// line, each an X, Y, and Z tinted as above. Orientation is in degrees, turned
// about Y, then X, then Z, shown to a thousandth. One axis writes only that
// axis: the rest of each instance's Transform, and the length of its axes,
// stay. Clicking the arrow or the name folds the row, and it stays folded for
// the session; Tab passes over a folded row.
//
// A Color3 row is a color picker. Closing its chooser on a new color writes
// it, so the whole pick is one undo step.
//
// A number whose property registers a range (lua_slider) is a slider with
// the field beside it. Dragging moves the field's number, rounded to a step
// that suits the range, and letting go writes it, so a drag is one undo step;
// an arrow key writes at once. The field takes any number, past either end;
// the thumb then rests at that end.
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

    // The widget for a row. part picks the Vector3 axis; for a Transform 0..2
    // Position's axes, 3..5 Orientation's, and 6 the fold arrow; for a
    // reference 0 the Name that picks and 1 Clear; or for a slider row 0 the
    // field and 1 the slider. Null when there is no such row.
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
