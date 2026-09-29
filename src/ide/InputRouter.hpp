#pragma once

#include "ChangeHistoryService.hpp"
#include "ide/TextUndoStack.hpp"

#include <cstdint>
#include <unordered_map>

namespace ide {

// Where keyboard focus is. Text foci never forward undo to the place.
enum class FocusKind { Viewport, Explorer, Properties, ScriptEditor, OtherTextField, None };

struct Focus {
    FocusKind kind = FocusKind::None;
    engine_core::InstanceId script = 0;
    std::uint64_t widget = 0;
};

enum class ChordKey { Z, Y, Other };

// primary is Ctrl on Windows and Linux, and Command on macOS.
struct KeyChord {
    bool primary = false;
    bool shift = false;
    bool alt = false;
    bool apple = false;
    ChordKey key = ChordKey::Other;
};

bool is_undo(const KeyChord& chord);
bool is_redo(const KeyChord& chord);

enum class UndoRoute { Ignore, Text, Place };

// Chooses the place stack or the focused document. A text chord is consumed
// even when that document cannot undo, so it does not fall through.
class InputRouter {
public:
    void set_focus(Focus focus) { focus_ = focus; }
    const Focus& focus() const { return focus_; }

    TextUndoStack& script_stack(engine_core::InstanceId id);
    TextUndoStack& widget_stack(std::uint64_t id);
    TextUndoStack* focused_stack();
    // Drops a script's stack, as when its editor closes. An editor bound to it
    // must be unbound first.
    void forget_script(engine_core::InstanceId id);
    // Drops every script's stack, as when the place those ids belong to goes away.
    void forget_scripts();

    // True when the chord belongs to undo or redo and was consumed.
    bool handle(const KeyChord& chord, engine_core::ChangeHistoryService* history);
    // Uses the focus set above. Text wins over the place, including an empty document.
    UndoRoute classify(const KeyChord& chord) const;

private:
    Focus focus_{};
    std::unordered_map<engine_core::InstanceId, TextUndoStack> scripts_;
    std::unordered_map<std::uint64_t, TextUndoStack> widgets_;
};

}  // namespace ide
