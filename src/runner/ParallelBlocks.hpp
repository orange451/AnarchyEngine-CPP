#pragma once

// A loop over [0, count) cut into blocks of grain items, the blocks run on a
// pool of worker threads the runner keeps, and on the calling thread. Blocks
// are numbered in order, so a caller that keeps each block's output apart and
// joins them by number gets what one loop would have made, in the same order.

#include <algorithm>
#include <cstddef>
#include <functional>
#include <vector>

namespace runner {

// How many blocks ParallelBlocks cuts count items into: at least 1.
inline std::size_t BlockCount(std::size_t count, std::size_t grain) {
    if (grain == 0) {
        return 1;
    }
    return std::max<std::size_t>(1, (count + grain - 1) / grain);
}

// Calls body(block) for every block in [0, blocks) and returns once all are
// done: on the pool's threads and this one. With one block, no workers, or
// from inside another RunBlocks body, every block runs here, in order.
void RunBlocks(std::size_t blocks, const std::function<void(std::size_t)>& body);

// The pool's worker threads, besides the threads that call RunBlocks.
unsigned BlockWorkers();

// Calls body(block, begin, end) for every block and returns once all are
// done. With one block, it is called here, on this thread. The body must
// write only what its block owns.
template <class Body>
void ParallelBlocks(std::size_t count, std::size_t grain, const Body& body) {
    const std::size_t blocks = BlockCount(count, grain);
    if (blocks == 1) {
        body(std::size_t{0}, std::size_t{0}, count);
        return;
    }
    RunBlocks(blocks, [&](std::size_t block) {
        const std::size_t begin = block * grain;
        body(block, begin, std::min(count, begin + grain));
    });
}

// Every i in [0, count) for which keep(i), ascending, into out: blocks of
// grain on worker threads, each block's into its own list in blocks, then
// joined in order. keep must only read.
template <class Keep>
void ParallelSelect(std::size_t count, std::size_t grain, const Keep& keep, std::vector<std::vector<int>>& blocks,
                    std::vector<int>& out) {
    blocks.resize(BlockCount(count, grain));
    ParallelBlocks(count, grain, [&](std::size_t block, std::size_t begin, std::size_t end) {
        std::vector<int>& kept = blocks[block];
        kept.clear();
        for (std::size_t i = begin; i < end; ++i) {
            if (keep(i)) {
                kept.push_back(static_cast<int>(i));
            }
        }
    });
    out.clear();
    for (const std::vector<int>& kept : blocks) {
        out.insert(out.end(), kept.begin(), kept.end());
    }
}

// std::sort(items, before), with blocks of grain sorted on worker threads and
// then merged. before must be a strict total order, as then the result is the
// only sorted order there is, the same as std::sort's.
template <class T, class Before>
void ParallelSort(std::vector<T>& items, std::size_t grain, const Before& before) {
    const std::size_t count = items.size();
    const std::size_t blocks = BlockCount(count, grain);
    if (blocks == 1) {
        std::sort(items.begin(), items.end(), before);
        return;
    }
    ParallelBlocks(count, grain, [&](std::size_t, std::size_t begin, std::size_t end) {
        std::sort(items.begin() + static_cast<std::ptrdiff_t>(begin), items.begin() + static_cast<std::ptrdiff_t>(end),
                  before);
    });
    // Sorted runs of width items, merged two by two until one is left.
    for (std::size_t width = grain; width < count; width *= 2) {
        const std::size_t pairs = (count + 2 * width - 1) / (2 * width);
        RunBlocks(pairs, [&](std::size_t pair) {
            const std::size_t begin = pair * 2 * width;
            const std::size_t middle = std::min(count, begin + width);
            const std::size_t end = std::min(count, begin + 2 * width);
            if (middle < end) {
                std::inplace_merge(items.begin() + static_cast<std::ptrdiff_t>(begin),
                                   items.begin() + static_cast<std::ptrdiff_t>(middle),
                                   items.begin() + static_cast<std::ptrdiff_t>(end), before);
            }
        });
    }
}

}  // namespace runner
