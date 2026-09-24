#include "InvalidationQueue.hpp"

namespace engine_core {

void InvalidationQueue::reserve(std::size_t capacity) {
    items_.assign(capacity, Invalidation{});
    head_ = 0;
    tail_ = 0;
    size_ = 0;
    overflow_ = false;
}

void InvalidationQueue::push(Invalidation item) {
    if (items_.empty() || size_ == items_.size()) {
        overflow_ = true;
        return;
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
    head_ = 0;
    tail_ = 0;
    size_ = 0;
}

}  // namespace engine_core
