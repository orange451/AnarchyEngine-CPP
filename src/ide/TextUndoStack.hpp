#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ide {

// Per-document text undo. Keystrokes stay here. A place waypoint is a separate
// whole-buffer Source write, not one of these edits.
class TextUndoStack {
public:
    void reset(std::string text);

    // Byte indexes. Tests and ASCII edits use these.
    void insert(std::size_t index, std::string text);
    void erase(std::size_t index, std::size_t count);
    // One undo step that replaces [byte, byte + remove_count).
    void replace(std::size_t byte, std::size_t remove_count, std::string inserted);

    // Code-point index, matching a text widget's plain-text change. False when the
    // change does not fit the tracked text; the stack is then out of sync and the
    // caller should reset it from the widget.
    bool record_change(int code_point, const std::string& removed, const std::string& inserted);

    // The whole new text of a field. Records one replace for the span that
    // differs, so a paste or a selection overwrite is one step. The span's ends
    // move out to UTF-8 boundaries. Nothing is recorded when next is the same.
    void record_text(const std::string& next);

    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    bool undo();
    bool redo();

    const std::string& text() const { return text_; }
    // Caret after the last undo or redo, in code points.
    int caret() const { return caret_; }

private:
    struct Edit {
        std::size_t byte = 0;
        std::string removed;
        std::string inserted;
        int caret_after_undo = 0;
        int caret_after_redo = 0;
    };

    void push(Edit edit);
    void apply(const Edit& edit, bool inverse, int caret);

    std::string text_;
    std::vector<Edit> undo_;
    std::vector<Edit> redo_;
    int caret_ = 0;
};

}  // namespace ide
