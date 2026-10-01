#pragma once

#include "DataModel.hpp"
#include "LuaApi.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace engine_core {

// The screen GUI classes, as the legacy engine had them, and CSS, which styles
// them. Each GUI instance is one node of the screen: a ScreenGui fills the
// view, and the GuiBases inside it lay out as JadeFX lays out its nodes, by
// their containers and CSS, not by absolute positions. Every ScreenGui under
// the Gui service, directly or through Folders, is drawn over the Scene View;
// one anywhere else is only data.
//
// GuiBase (Lua class, not made itself)
//   ClassList         string   CSS classes, separated by spaces. "".
//   Style             string   inline CSS declarations, as an HTML style attribute. "".
//   Size              Vector2  the preferred size in points; 0 on an axis
//                              leaves that axis to the content and CSS. (0, 0).
//   Alignment         EnumItem Enum.GuiAlignment, where the children sit. TopLeft.
//   Visible           boolean  true.
//   MouseTransparent  boolean  when true the mouse passes through it. false.
//   MouseClicked, MousePressed, MouseReleased, MouseEntered, MouseExited:
//     events with no arguments. Only the left button presses and clicks.
// GuiBasePane (Lua class, not made itself): a GuiBase with
//   BackgroundColor         Color3  white.
//   BackgroundTransparency  number  0 to 1. 0.
// ScreenGui  the root. It fills the view and ignores Size; its own area never
//            takes the mouse, so a press on no element reaches the scene.
// Pane       a GuiBasePane that stacks its children by Alignment. Size (100, 100).
// HBox, VBox GuiBasePanes in a row or a column, with Spacing (number, 0 and up, 0).
// Label      Text ("Label"), TextColor (Color3, black), FontSize (1 to 512, 16).
// Button     Text ("Button"), and the event Action, on a click or Enter.
// TextField  Text (""), Prompt ("Prompt"), and the event Action, on Enter.
//            Typing writes Text.
// CSS        Source ("/* CSS Document */"): a stylesheet for its parent
//            GuiBase and everything inside it, or, directly under the Gui
//            service, for every ScreenGui. The CSS editor edits Source;
//            Properties does not show it. The studio's styles never reach
//            the game's GUIs; they start from a blank default sheet
//            (runner::GuiLayer::defaultStylesheet).
//
// The Name of a GuiBase is its CSS id, its ClassList its classes, and its
// class, lowercase, its element type: screengui, pane, hbox, vbox, label,
// button, textfield.
//
// Each property is a saved registry property (lua_saved_property), so
// DataModel saves, loads, undoes, and restores it at Stop.

// Every property any of these classes has, as a slot in GuiValues.
enum class GuiProperty : int {
    ClassList,
    Style,
    Size,
    Alignment,
    Visible,
    MouseTransparent,
    BackgroundColor,
    BackgroundTransparency,
    Spacing,
    Text,
    TextColor,
    FontSize,
    Prompt,
    Source,
    Count
};

// What the GUI classes share: their property values, by GuiProperty, each
// held as the LuaSlot a script reads. A class uses only the slots it registers.
class GuiValues : public DataModel {
public:
    GuiValues(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);

    const LuaSlot& value(GuiProperty property) const { return values_[static_cast<int>(property)]; }
    const std::string& text(GuiProperty property) const { return value(property).text; }
    double number(GuiProperty property) const { return value(property).number; }
    bool flag(GuiProperty property) const { return value(property).flag; }
    ColorRgb color(GuiProperty property) const { return value(property).color; }
    // Size's x and y.
    Vec2 vec2(GuiProperty property) const { return Vec2{value(property).vec.x, value(property).vec.y}; }

    // Moves each time any property here changes, so a reader that keeps what
    // it last saw knows when to look again.
    std::uint64_t revision() const { return revision_; }

    // SimulationThread. Checks and stores the value as a write from a script
    // or Properties does: the wrong kind, or a number or color that is not
    // finite, is refused, and returns why; a number is clamped to its range.
    // A new value records undo and fires Changed.
    std::optional<std::string> set_value(GuiProperty property, LuaSlot value);
    // A TextField's typing: the same as set_value with a string.
    std::optional<std::string> set_text(GuiProperty property, std::string text);

    // The value a new instance of this class has.
    static LuaSlot default_value(GuiProperty property, const char* class_name);

protected:
    void on_reuse() override;
    // Puts every slot back to this class's defaults. A subclass's constructor
    // calls it, since class_name is not its own until then.
    void reset_values();

private:

    LuaSlot values_[static_cast<int>(GuiProperty::Count)];
    std::uint64_t revision_ = 1;
};

class GuiBase : public GuiValues {
public:
    using GuiValues::GuiValues;
};

class ScreenGui : public GuiBase {
public:
    ScreenGui(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);
    const char* class_name() const override;
};

class GuiBasePane : public GuiBase {
public:
    using GuiBase::GuiBase;
};

class Pane : public GuiBasePane {
public:
    Pane(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);
    const char* class_name() const override;
};

class HBox : public GuiBasePane {
public:
    HBox(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);
    const char* class_name() const override;
};

class VBox : public GuiBasePane {
public:
    VBox(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);
    const char* class_name() const override;
};

class Label : public GuiBase {
public:
    Label(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);
    const char* class_name() const override;
};

class Button : public GuiBase {
public:
    Button(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);
    const char* class_name() const override;
};

class TextField : public GuiBase {
public:
    TextField(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);
    const char* class_name() const override;
};

class Css : public GuiValues {
public:
    Css(DataModel::ChildTag tag, DataModel::State& state, InstanceId id);
    const char* class_name() const override;
    const std::string& source() const { return text(GuiProperty::Source); }
    // Edit, then the actions every instance has. Edit is the double-click and opens the CSS editor.
    void context_actions(std::vector<ContextAction>& out) const override;
};

// The events every GuiBase has, and the one a Button and a TextField add.
inline constexpr const char* kGuiMouseClicked = "MouseClicked";
inline constexpr const char* kGuiMousePressed = "MousePressed";
inline constexpr const char* kGuiMouseReleased = "MouseReleased";
inline constexpr const char* kGuiMouseEntered = "MouseEntered";
inline constexpr const char* kGuiMouseExited = "MouseExited";
inline constexpr const char* kGuiAction = "Action";

}  // namespace engine_core
