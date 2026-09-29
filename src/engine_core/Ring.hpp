#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace engine_core {

// Doubles a full FIFO ring, at least to minimum. The size items from head move
// to the front, oldest first, so head becomes 0 and tail becomes size.
template <typename T>
void grow_ring(std::vector<T>& ring, std::size_t& head, std::size_t& tail, std::size_t size, std::size_t minimum) {
    std::vector<T> larger(std::max(minimum, ring.size() * 2));
    for (std::size_t n = 0; n < size; ++n) {
        larger[n] = ring[(head + n) % ring.size()];
    }
    ring.swap(larger);
    head = 0;
    tail = size;
}

}  // namespace engine_core
