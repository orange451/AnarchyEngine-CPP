#include "ide/TextUndoStack.hpp"

#include <algorithm>

namespace ide {
namespace {

std::size_t utf8_length(unsigned char lead) {
    if ((lead & 0x80u) == 0) {
        return 1;
    }
    if ((lead & 0xE0u) == 0xC0u) {
        return 2;
    }
    if ((lead & 0xF0u) == 0xE0u) {
        return 3;
    }
    if ((lead & 0xF8u) == 0xF0u) {
        return 4;
    }
    return 1;
}

int code_points(const std::string& text) {
    int count = 0;
    for (std::size_t index = 0; index < text.size();) {
        index += utf8_length(static_cast<unsigned char>(text[index]));
        ++count;
    }
    return count;
}

std::size_t byte_at(const std::string& text, int point) {
    if (point <= 0) {
        return 0;
    }
    std::size_t index = 0;
    int seen = 0;
    while (index < text.size() && seen < point) {
        index += utf8_length(static_cast<unsigned char>(text[index]));
        ++seen;
    }
    if (index > text.size()) {
        return text.size();
    }
    return index;
}

int point_at(const std::string& text, std::size_t byte) {
    int count = 0;
    std::size_t index = 0;
    const std::size_t end = std::min(byte, text.size());
    while (index < end) {
        const std::size_t next = index + utf8_length(static_cast<unsigned char>(text[index]));
        if (next > end) {
            break;
        }
        index = next;
        ++count;
    }
    return count;
}

}  // namespace

void TextUndoStack::reset(std::string text) {
    text_ = std::move(text);
    undo_.clear();
    redo_.clear();
    caret_ = code_points(text_);
}

void TextUndoStack::push(Edit edit) {
    undo_.push_back(std::move(edit));
    redo_.clear();
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
    edit.caret_after_undo = point_at(text_, index);
    text_.insert(index, edit.inserted);
    edit.caret_after_redo = point_at(text_, index + edit.inserted.size());
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
    edit.caret_after_undo = point_at(text_, byte) + code_points(edit.removed);
    text_.replace(byte, remove_count, edit.inserted);
    edit.caret_after_redo = point_at(text_, byte) + code_points(edit.inserted);
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
    edit.caret_after_undo = point_at(text_, index + count);
    edit.caret_after_redo = point_at(text_, index);
    text_.erase(index, count);
    caret_ = edit.caret_after_redo;
    push(std::move(edit));
}

void TextUndoStack::record_change(int code_point, const std::string& removed, const std::string& inserted) {
    if (removed.empty() && inserted.empty()) {
        return;
    }
    const std::size_t byte = byte_at(text_, code_point);
    if (byte > text_.size() || text_.compare(byte, removed.size(), removed) != 0) {
        undo_.clear();
        redo_.clear();
        return;
    }
    Edit edit;
    edit.byte = byte;
    edit.removed = removed;
    edit.inserted = inserted;
    edit.caret_after_undo = code_point + code_points(removed);
    edit.caret_after_redo = code_point + code_points(inserted);
    text_.replace(byte, removed.size(), inserted);
    caret_ = edit.caret_after_redo;
    push(std::move(edit));
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
