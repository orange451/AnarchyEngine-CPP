#include "ide/TextUndoStack.hpp"

#include "ide/Utf8.hpp"

#include <algorithm>

namespace ide {

void TextUndoStack::reset(std::string text) {
    text_ = std::move(text);
    undo_.clear();
    redo_.clear();
    caret_ = CodePoints(text_);
}

void TextUndoStack::set_limits(std::size_t edits, std::size_t bytes) {
    max_edits_ = edits;
    max_bytes_ = bytes;
}

void TextUndoStack::push(Edit edit) {
    undo_.push_back(std::move(edit));
    redo_.clear();
    const auto bytes_of = [](const Edit& kept) { return sizeof(Edit) + kept.removed.size() + kept.inserted.size(); };
    std::size_t bytes = 0;
    for (const Edit& kept : undo_) {
        bytes += bytes_of(kept);
    }
    std::size_t drop = 0;
    while (undo_.size() - drop > 1 && (undo_.size() - drop > max_edits_ || bytes > max_bytes_)) {
        bytes -= bytes_of(undo_[drop]);
        ++drop;
    }
    undo_.erase(undo_.begin(), undo_.begin() + static_cast<std::ptrdiff_t>(drop));
}

void TextUndoStack::apply(const Edit& edit, bool inverse, int caret) {
    const std::string& insert = inverse ? edit.removed : edit.inserted;
    const std::string& remove = inverse ? edit.inserted : edit.removed;
    if (edit.byte > text_.size() || edit.byte + remove.size() > text_.size()) {
        return;
    }
    text_.replace(edit.byte, remove.size(), insert);
    caret_ = caret;
}

void TextUndoStack::insert(std::size_t index, std::string text) {
    if (index > text_.size()) {
        index = text_.size();
    }
    Edit edit;
    edit.byte = index;
    edit.inserted = std::move(text);
    edit.caret_after_undo = CodePointsBefore(text_, index);
    text_.insert(index, edit.inserted);
    edit.caret_after_redo = CodePointsBefore(text_, index + edit.inserted.size());
    caret_ = edit.caret_after_redo;
    push(std::move(edit));
}

void TextUndoStack::replace(std::size_t byte, std::size_t remove_count, std::string inserted) {
    if (byte > text_.size()) {
        return;
    }
    if (byte + remove_count > text_.size()) {
        remove_count = text_.size() - byte;
    }
    if (remove_count == 0 && inserted.empty()) {
        return;
    }
    Edit edit;
    edit.byte = byte;
    edit.removed = text_.substr(byte, remove_count);
    edit.inserted = std::move(inserted);
    if (edit.removed == edit.inserted) {
        return;
    }
    edit.caret_after_undo = CodePointsBefore(text_, byte) + CodePoints(edit.removed);
    text_.replace(byte, remove_count, edit.inserted);
    edit.caret_after_redo = CodePointsBefore(text_, byte) + CodePoints(edit.inserted);
    caret_ = edit.caret_after_redo;
    push(std::move(edit));
}

void TextUndoStack::erase(std::size_t index, std::size_t count) {
    if (index > text_.size()) {
        return;
    }
    if (index + count > text_.size()) {
        count = text_.size() - index;
    }
    if (count == 0) {
        return;
    }
    Edit edit;
    edit.byte = index;
    edit.removed = text_.substr(index, count);
    edit.caret_after_undo = CodePointsBefore(text_, index + count);
    edit.caret_after_redo = CodePointsBefore(text_, index);
    text_.erase(index, count);
    caret_ = edit.caret_after_redo;
    push(std::move(edit));
}

void TextUndoStack::record_text(const std::string& next) {
    const std::string& previous = text_;
    if (next == previous) {
        return;
    }
    std::size_t start = 0;
    while (start < previous.size() && start < next.size() && previous[start] == next[start]) {
        ++start;
    }
    std::size_t previous_end = previous.size();
    std::size_t next_end = next.size();
    while (previous_end > start && next_end > start && previous[previous_end - 1] == next[next_end - 1]) {
        --previous_end;
        --next_end;
    }
    while (start > 0 && (static_cast<unsigned char>(previous[start]) & 0xC0u) == 0x80u) {
        --start;
    }
    while (previous_end < previous.size() && next_end < next.size() &&
           (static_cast<unsigned char>(previous[previous_end]) & 0xC0u) == 0x80u) {
        ++previous_end;
        ++next_end;
    }
    replace(start, previous_end - start, next.substr(start, next_end - start));
}

bool TextUndoStack::record_change(int code_point, const std::string& removed, const std::string& inserted) {
    if (removed.empty() && inserted.empty()) {
        return true;
    }
    // A position past the end means the widget holds text this stack never saw.
    // CodePointByte would clamp it and quietly record the edit against the wrong buffer.
    if (code_point < 0 || code_point > CodePoints(text_)) {
        undo_.clear();
        redo_.clear();
        return false;
    }
    const std::size_t byte = CodePointByte(text_, code_point);
    if (byte > text_.size() || text_.compare(byte, removed.size(), removed) != 0) {
        undo_.clear();
        redo_.clear();
        return false;
    }
    Edit edit;
    edit.byte = byte;
    edit.removed = removed;
    edit.inserted = inserted;
    edit.caret_after_undo = code_point + CodePoints(removed);
    edit.caret_after_redo = code_point + CodePoints(inserted);
    text_.replace(byte, removed.size(), inserted);
    caret_ = edit.caret_after_redo;
    push(std::move(edit));
    return true;
}

bool TextUndoStack::undo() {
    if (undo_.empty()) {
        return false;
    }
    Edit edit = std::move(undo_.back());
    undo_.pop_back();
    const int caret = edit.caret_after_undo;
    apply(edit, true, caret);
    redo_.push_back(std::move(edit));
    return true;
}

bool TextUndoStack::redo() {
    if (redo_.empty()) {
        return false;
    }
    Edit edit = std::move(redo_.back());
    redo_.pop_back();
    const int caret = edit.caret_after_redo;
    apply(edit, false, caret);
    undo_.push_back(std::move(edit));
    return true;
}

}  // namespace ide
