#include "InvalidationQueue.hpp"

namespace engine_core {

void InvalidationQueue::reserve(std::size_t capacity) {
    items_.assign(capacity, Invalidation{});
    queued_.assign(std::size_t{1} << 16, 0);
    head_ = 0;
    tail_ = 0;
    size_ = 0;
    overflow_ = false;
}

void InvalidationQueue::push(Invalidation item) {
    const bool transform_only = item.fields == VisualField::Transform && !queued_.empty();
    if (transform_only) {
        const std::size_t waiting = queued_[id_slot(item.id)];
        if (waiting != 0 && items_[waiting - 1].id == item.id && items_[waiting - 1].origin == item.origin) {
            return;
        }
    }
    if (items_.empty() || size_ == items_.size()) {
        overflow_ = true;
        return;
    }
    if (transform_only) {
        queued_[id_slot(item.id)] = tail_ + 1;
    }
    items_[tail_] = item;
    tail_ = (tail_ + 1) % items_.size();
    ++size_;
}

bool InvalidationQueue::take_overflow() {
    if (!overflow_) {
        return false;
    }
    overflow_ = false;
    clear();
    return true;
}

void InvalidationQueue::clear() {
    for (std::size_t i = 0, at = head_; i < size_ && !items_.empty(); ++i, at = (at + 1) % items_.size()) {
        unqueue(items_[at]);
    }
    head_ = 0;
    tail_ = 0;
    size_ = 0;
}

}  // namespace engine_core
