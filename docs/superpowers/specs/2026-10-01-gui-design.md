# Gui service, Gui instances, and CSS

Written 2026-10-01 while the user was away; decisions below were made without
review and are listed so they can be revisited.

## Goal

Screen GUIs built from instances, as the legacy Java engine had them
(`engine/lua/type/object/insts/ui/*.java`, LWJGUI underneath), drawn over the
Scene View and styled with CSS that is edited in the studio.

## What the legacy engine had

`GuiBase` (ClassList, Size, Alignment, MouseTransparent; MouseClicked,
MouseEntered, MouseExited, MousePressed, MouseReleased) with subclasses `Gui`
(the root), `Pane`, `HBox`, `VBox` (through `GuiBasePane`, BackgroundColor),
`Label` (Text, TextColor, FontSize), `Button` (Text, Action), and `TextField`
(Text, Prompt; typing writes Text). A `CSS` instance (Source) styled its parent
and everything under it. The instance's Name was the node's CSS id, ClassList
its classes, and the class its element type. Layout was the containers'
(stack, row, column) plus CSS sizes, never absolute positions. The IDE opened a
CSS editor on a double-click and saved it as you typed.

JadeFX is LWJGUI's successor and has the same node model and a larger CSS
subset, so the mapping carries over almost one to one.

## Decisions

- **The service is `Gui`**, a fifth scene service after `Scripts`
  (`game.Gui`, `game:GetService("Gui")`), with the fixed GUID `gui`. A place
  whose files lack it gets it, as with the other services.
- **The root class is `ScreenGui`**, not `Gui`, since the service has that
  name. It fills the view. Its own area never takes the mouse: a press that
  hits no element inside it goes on to the scene, so a ScreenGui does not block
  the camera.
- What renders: every `ScreenGui` under `Gui`, directly or through Folders, and
  every `GuiBase` inside one through a chain of GuiBases. A GuiBase anywhere
  else is only data, like a GameObject in Storage. GUI classes may be parented
  anywhere.
- **Classes**: `GuiBase` (abstract) → `ScreenGui`, `Label`, `Button`,
  `TextField`, and `GuiBasePane` (abstract) → `Pane`, `HBox`, `VBox`. Plus
  `CSS`, a plain Instance.
- **Properties** (all saved registry properties):
  - GuiBase: `ClassList` (string, space separated), `Style` (string, inline
    CSS, new), `Size` (Vector2, the preferred size; 0 on an axis leaves it to
    the content and CSS), `Alignment` (`Enum.GuiAlignment`, TopLeft),
    `Visible` (boolean, true, new), `MouseTransparent` (boolean, false).
  - GuiBasePane: `BackgroundColor` (Color3, white), `BackgroundTransparency`
    (number 0 to 1, 0). The legacy Color4 becomes Color3 plus transparency,
    since a script's colors here are Color3.
  - Pane: Size defaults to 100, 100, as before. HBox, VBox: `Spacing` (number, 0, new).
  - Label: `Text` ("Label"), `TextColor` (Color3, black), `FontSize` (16).
  - Button: `Text` ("Button").
  - TextField: `Text` (""), `Prompt` ("Prompt").
  - CSS: `Source` ("/* CSS Document */").
- **Events**: GuiBase has `MouseClicked`, `MousePressed`, `MouseReleased`,
  `MouseEntered`, `MouseExited`; Button and TextField have `Action`. They pass
  no arguments; only the left button presses and clicks, as JadeFX gives
  them. A press or click bubbles to the enclosing elements, as in the DOM.
  Input that lands on an element still reaches UserInputService, with
  `gameProcessedEvent` true.
- **CSS**: element types are `screengui`, `pane`, `hbox`, `vbox`, `label`,
  `button`, `textfield`; `#Name` and `.class` match. A CSS instance styles its
  parent GuiBase and everything inside it; several under one parent are joined
  in child order. The Gui layer resets the inherited text color and size, so
  the studio's theme does not color the game's text.
- **Vector2 properties**: `LuaSlot` gains a `Vec2` kind, saved as `[x, y]` and
  shown in Properties as X and Y fields.
- **Instance events**: a new kind of signal a class declares in its registry
  fields (`lua_event`), kept per instance in its signal bag, fired with
  `DataModel::fire_event`. The studio posts GUI input to the simulation thread,
  which fires them.
- **Rendering**: `runner::GuiLayer`, a JadeFX node in each Scene View above the
  3D drawing, rebuilt from the tree each layout under a short read lock, the
  way the camera list is. The layer and each ScreenGui do not pick on their own bounds (a
  `setPickOnBounds(false)` added to JadeFX for this), so only elements take the
  mouse, even with several full-view ScreenGuis stacked. Only what changed is touched; a stylesheet is parsed
  again only when its text changes. The MCP screenshot reads the 3D image
  before child nodes paint, so it does not show GUIs.
- **CSS editor**: double-clicking a CSS instance (or Edit) docks a CSS editor
  tab beside the scripts: a CodeArea with CSS highlighting (comments,
  selectors, properties, values, strings, numbers, colors), auto-indent and a
  closing brace after `{`, writing Source back half a second after typing as
  one undo step, like the script editor. Properties does not show Source, as it does not a script's.

## Changes

- 2026-10-01: the game GUIs have a cascade of their own
  (`2026-10-01-game-ui-subscene-design.md`). The Gui layer is the root of a
  JadeFX `SubScene` whose user-agent stylesheet is a blank default (no
  outlines, backgrounds, or padding), so the studio's theme, stylesheets, and
  text color no longer reach them; that replaces the layer resetting the
  inherited text color and size. CSS directly under the Gui service now
  styles every ScreenGui.

## Not done here

- Source in its own `.css` file on disk, as a Script's is `.luau`; it is a
  JSON string for now.
- `PlayerGui` / per-player cloning: there are no Players yet.
- ImageLabel / ImageButton, scrolling panes, absolute positioning.
