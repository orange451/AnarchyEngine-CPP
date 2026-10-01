# Game UI styling root: JadeFX SubScene and a blank default sheet

Written 2026-10-01 with the user; every decision below was reviewed.

## Goal

The in-game UI (the Gui service's ScreenGuis, drawn by `runner::GuiLayer`)
gets its own styling root. Nothing from the studio's styles reaches it: not
the IDE theme, not the stylesheets on IDE panels or the IDE scene, not the
custom properties and text color they set. Under that root an internal
default stylesheet gives a blank starting look, with no outlines,
backgrounds, or padding, so users style their UI from a clean slate. CSS
instances keep working as they do now, and CSS placed directly under the Gui
service starts applying to every ScreenGui.

## Why it leaks today

`GuiLayer` is a JadeFX node inside the studio's Scene, so for every game node
`Node::applyStyles` uses:

- the Scene's user-agent stylesheet, which `IdeTheme.cpp` sets app-wide to
  the studio theme (light or dark), including
  `button { background-color: var(--surface-color); }`;
- the author stylesheet of every ancestor, including the one `IdeLayout.cpp`
  sets on the IDE scene;
- inherited values from its parent: every theme custom property, the text
  color, the font, and the cursor.

Button and TextField also draw a 1px outline, a hover and press wash, and a
focus ring from C++ (`chrome::DrawBorder`, `DrawWash`, `DrawFocusRing`),
colored by `--border-color`, `--wash-color`, and `--outline-color`, falling
back to the light theme's values when nothing sets them.

## Decisions

- **The boundary is a JadeFX `SubScene`**, as JavaFX's: a Node that holds a
  root and is a styling and layout boundary, not a second Scene. Input,
  focus, popups, drag, hover popups, and the clipboard stay with the window's
  Scene. (A Node flag and a fully nested Scene were considered; the user
  chose this.)
- **The default sheet is the SubScene's user-agent stylesheet**, the lowest
  layer, so any user CSS wins without `!important`.
- **The blank look clears** outlines, button backgrounds, and padding, and
  **keeps** the TextField caret and selection, the hover and press wash, and
  the focus ring.
- **CSS directly under the Gui service applies** to every ScreenGui, above
  the default sheet and below ScreenGui and GuiBase CSS.

## 1. `jadefx::SubScene` (JadeFX_CPP)

New `include/jadefx/scene/SubScene.hpp` and `src/scene/SubScene.cpp`:

```cpp
class SubScene : public Node {
public:
    explicit SubScene(std::shared_ptr<Node> root = nullptr);
    const char* getElementType() const override { return "subscene"; }
    void setRoot(std::shared_ptr<Node> root);
    Node* getRoot() const;
    // light, dark, or CSS text, as Scene's takes. Empty uses the scene's.
    void setUserAgentStylesheet(std::string cssOrTheme);
    const std::string& getUserAgentStylesheet() const;
    const Stylesheet& userAgentStylesheet() const;
};
```

Styling of a node inside a SubScene, in `Node::applyStyles`, in the
single-node `applyCss` path, and in the pass that styles children:

- **User-agent sheet**: the nearest enclosing SubScene's. Empty falls back to
  the Scene's, as in JavaFX.
- **Author sheets**: collected from the node up to the SubScene's root and no
  further. Sheets on the SubScene node or its ancestors do not apply inside.
- **Inheritance**: the root is styled from a fresh `ComputedStyle{}`, not from
  the SubScene's inheritable style, so no custom property, text color, font,
  or cursor crosses in.
- **`:root`**: `setRoot` gives the new root the `root` pseudo-state and takes
  it from the old one.
- **The SubScene node itself** belongs to the outer cascade; outer CSS styles
  it as any node.
- **Switching the user-agent sheet** restyles the subtree at the next layout,
  as `Scene::setUserAgentStylesheet` does.

Layout and paint: the SubScene lays its root out over its content box, takes
its preferred and minimum sizes from the root, and clips its children's
drawing to its bounds.

`getScene()` inside still returns the window's Scene. A popup opened from
inside, such as a context menu, belongs to the outer Scene and its look;
nothing in the game UI opens one today.

## 2. Anarchy Engine wiring

**GameView**: wraps the GuiLayer in a SubScene instead of adding it directly:
`subScene_ = jadefx::make<jadefx::SubScene>(gui)`, with
`setPickOnBounds(false)` so the 3D view still takes the mouse where no element
is, and `setUserAgentStylesheet(GuiLayer::defaultStylesheet())`.
`layoutChildren` keeps calling `guiLayer_->sync()` and lays the SubScene out
over the whole content area where it laid out the GuiLayer. The FPS label and
camera list stay outside it and keep the studio look.

**GuiLayer**: is the SubScene's root, so it has `:root`; its element type stays
`gui-layer`. In `sync()`, the CSS instances that are direct children of the
Gui service are joined in child order and set with `GuiLayer::setStylesheet`,
cached so the sheet is parsed again only when the text changes. CSS inside a
Folder under the service is not read, since its parent is not a GuiBase, as
the existing rule that a CSS instance styles its parent says. Per-GuiBase CSS,
`Style`, `ClassList`, and the `Name` id are unchanged.

The cascade for a game node, lowest first:

1. the default game sheet;
2. values set from code (a Label's `TextColor` and `FontSize`, a pane's
   `BackgroundColor`);
3. Gui service CSS;
4. ScreenGui and GuiBase CSS, outermost first;
5. inline `Style`.

**Docs**: the comments in `GuiLayer.hpp` and `Gui.hpp`, and a dated note in
`2026-10-01-gui-design.md` saying service CSS now applies and the studio's
styles no longer reach the game UI (its line about the layer resetting the
inherited text color and size is now covered by the boundary).

## 3. The default game sheet

`static const char* GuiLayer::defaultStylesheet()`, a constant in
`GuiLayer.cpp`, as JadeFX keeps its theme CSS in `Theme.cpp`:

```css
/* The game UI's starting point: no outlines, backgrounds, or padding.
   Hover, press, and focus feedback stay, colored by the variables below. */
:root {
    --text-color: #000000;
    --border-color: transparent;          /* the built-in 1px outline */
    --surface-color: transparent;
    --accent-color: #1a73e8;
    --outline-color: var(--accent-color); /* the focus ring */
    --wash-color: rgba(0, 0, 0, 0.04);    /* hover; doubled when pressed */
    --text-selection-color: rgba(26, 115, 232, 0.3);
    color: var(--text-color);
}
button, textfield {
    padding: 0;
}
```

The colors are fixed light values; the studio's dark theme does not change
the game UI.

What a user gets and how to change it:

- Buttons draw only their text, wash on hover and press, and show the accent
  focus ring. TextFields draw text, caret, and selection with no box. Labels
  are as before.
- `button { border: 1px solid #888; }` draws a CSS border; `DrawBorder` skips
  the built-in outline whenever a solid CSS border is set.
- `:root { --border-color: #ccc; }` brings the built-in outline back.
- `--wash-color: transparent` and `--outline-color: transparent` turn off the
  hover wash and the focus ring.
- Unchanged, from C++: a disabled control's 45% opacity and the default font
  (Open Sans, 16px).

To confirm while implementing:

- `padding: 0` in a user-agent sheet overrides the padding ButtonBase and
  TextField set in their constructors. `applyStyles` seeds `style.padding`
  from the node and then applies user-agent declarations, so it should. If it
  does not, stop and report rather than work around it.
- The 4% wash is the light theme's value and will be faint over a 3D scene
  with no button background. It was kept on purpose; it is one line to raise.

## Tests

**JadeFX, `tests/subscene_tests.cpp`**:

- Inside nodes use the SubScene's user-agent sheet; with none, the Scene's.
- An ancestor's author stylesheet (on the Scene and on the SubScene node) does
  not match inside.
- Custom properties and text color do not inherit across the boundary.
- `:root` matches the root, and moves when `setRoot` replaces it.
- Outer CSS styles the SubScene node itself.
- `applyCss()` on one inner node gives the same result as a full pass.
- The root is laid out at the SubScene's size, and painting is clipped.

**Anarchy Engine, `tests/GuiStyleTest.cpp`**, on `StudioLayoutTest`'s
real-layout setup:

- With the studio in the dark theme, a game Button has no background and no
  outline color (its `--border-color` resolves to transparent).
- An IDE scene rule such as `button { color: red; }` does not reach a game
  Button.
- Gui service CSS styles a Button inside a ScreenGui, and ScreenGui CSS
  overrides it.
- `:root { --accent-color: ... }` in service CSS changes the focus ring color.
- Editing service CSS restyles on the next sync.
- A game Button's padding is 0.

## Build and order

The engine builds against the sibling `../JadeFX_CPP` checkout, so both
change together locally: SubScene lands in JadeFX first, then the engine
uses it. A build that downloads JadeFX instead needs the JadeFX change pushed
before it sees `SubScene`.

## Not done here

- Honoring `border-style: none` in `chrome::DrawBorder` as a way to drop the
  built-in outline per control.
- A nested Scene with its own focus, popups, and input.
- Game-styled popups (context menus opened from game controls).
- Reading CSS from Folders under the Gui service.
