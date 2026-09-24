#pragma once

#include "types.hpp"

#include <cstddef>
#include <vector>

namespace engine_core {

// One visual change. origin says which path produced it.
// Touched only while the DataModel write lock is held, or before threads start.
struct Invalidation {
    InstanceId id = 0;
    VisualField fields = VisualField::Transform;
    WriteOrigin origin = WriteOrigin::Simulation;
};

class InvalidationQueue {
public:
    void reserve(std::size_t capacity);

    // Drops the record and raises overflow when the ring is full.
    // Prepare then resyncs every live instance instead of guessing.
    void push(Invalidation item);
    bool overflow() const { return overflow_; }
    // Clears the overflow flag and the ring. True when a push was dropped.
    bool take_overflow();

    template <typename Fn>
    void drain(Fn&& fn) {
        while (size_ > 0) {
            fn(items_[head_]);
            head_ = (head_ + 1) % items_.size();
            --size_;
        }
        head_ = 0;
        tail_ = 0;
    }

    void clear();
    std::size_t size() const { return size_; }

private:
    std::vector<Invalidation> items_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t size_ = 0;
    bool overflow_ = false;
};

}  // namespace engine_core
