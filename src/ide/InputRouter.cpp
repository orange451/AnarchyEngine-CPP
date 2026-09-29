#include "ide/InputRouter.hpp"

namespace ide {
namespace {

bool text_focus(FocusKind kind) {
    return kind == FocusKind::ScriptEditor || kind == FocusKind::OtherTextField;
}

bool place_focus(FocusKind kind) {
    return kind == FocusKind::Viewport || kind == FocusKind::Explorer || kind == FocusKind::Properties ||
           kind == FocusKind::None;
}

}  // namespace

bool is_undo(const KeyChord& chord) {
    return chord.primary && !chord.shift && !chord.alt && chord.key == ChordKey::Z;
}

bool is_redo(const KeyChord& chord) {
    if (!chord.primary || chord.alt) {
        return false;
    }
    // Ctrl/Cmd+Y, and Ctrl/Cmd+Shift+Z. The text editor keeps Shift+Z when it is focused.
    if (chord.key == ChordKey::Y && !chord.shift) {
        return true;
    }
    return chord.key == ChordKey::Z && chord.shift;
}

TextUndoStack& InputRouter::script_stack(engine_core::InstanceId id) { return scripts_[id]; }

TextUndoStack& InputRouter::widget_stack(std::uint64_t id) { return widgets_[id]; }

void InputRouter::forget_script(engine_core::InstanceId id) { scripts_.erase(id); }

void InputRouter::forget_scripts() { scripts_.clear(); }

TextUndoStack* InputRouter::focused_stack() {
    if (focus_.kind == FocusKind::ScriptEditor) {
        const auto found = scripts_.find(focus_.script);
        return found == scripts_.end() ? nullptr : &found->second;
    }
    if (focus_.kind == FocusKind::OtherTextField) {
        const auto found = widgets_.find(focus_.widget);
        return found == widgets_.end() ? nullptr : &found->second;
    }
    return nullptr;
}

UndoRoute InputRouter::classify(const KeyChord& chord) const {
    if (!is_undo(chord) && !is_redo(chord)) {
        return UndoRoute::Ignore;
    }
    if (text_focus(focus_.kind)) {
        return UndoRoute::Text;
    }
    if (place_focus(focus_.kind)) {
        return UndoRoute::Place;
    }
    return UndoRoute::Ignore;
}

bool InputRouter::handle(const KeyChord& chord, engine_core::ChangeHistoryService* history) {
    const bool undo = is_undo(chord);
    const bool redo = is_redo(chord);
    if (!undo && !redo) {
        return false;
    }
    // apple is accepted on both platforms: Cmd and Ctrl share `primary`.
    (void)chord.apple;
    if (text_focus(focus_.kind)) {
        if (TextUndoStack* stack = focused_stack()) {
            if (undo) {
                stack->undo();
            } else {
                stack->redo();
            }
        }
        return true;
    }
    if (place_focus(focus_.kind) && history != nullptr) {
        if (undo) {
            history->undo();
        } else {
            history->redo();
        }
    }
    return place_focus(focus_.kind);
}

}  // namespace ide
