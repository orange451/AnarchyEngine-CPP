#pragma once

// A list of rows stored in fixed chunks that copies share. Copying the list
// copies chunk pointers, not rows, and writing a row first gives the writer
// its own copy of that row's chunk when anyone else shares it (copy on write).
//
// The render snapshot keeps its rows here: the engine thread publishes a copy
// every frame and the UI keeps one, so with a hundred thousand rows a frame
// used to copy tens of megabytes twice; now it copies a few hundred pointers,
// and only chunks with a changed row are copied. A chunk's address also tells
// a reader which rows may have changed since it last looked (chunk()).
//
// Threads: one thread owns a list and writes it. Other threads may read their
// own copies while it writes: a chunk any copy shares is never written, only
// copied (unique() checks that, with an acquire fence after it, as the
// readers' last uses happen before they release their references).

#include <atomic>
#include <cstddef>
#include <iterator>
#include <memory>
#include <vector>

namespace engine_core {

template <typename T, std::size_t ChunkRows = 256>
class SharedRows {
public:
    static constexpr std::size_t kChunkRows = ChunkRows;
    using Chunk = std::vector<T>;

    SharedRows() = default;

    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    const T& operator[](std::size_t index) const { return (*chunks_[index / kChunkRows])[index % kChunkRows]; }
    const T& back() const { return (*this)[size_ - 1]; }

    // The row to write, after making its chunk this list's own.
    T& at_mut(std::size_t index) { return own(index / kChunkRows)[index % kChunkRows]; }

    void push_back(T row) {
        if (size_ % kChunkRows == 0) {
            auto chunk = std::make_shared<Chunk>();
            chunk->reserve(kChunkRows);
            chunks_.push_back(std::move(chunk));
        }
        own(chunks_.size() - 1).push_back(std::move(row));
        ++size_;
    }

    void pop_back() {
        own(chunks_.size() - 1).pop_back();
        --size_;
        if (size_ % kChunkRows == 0) {
            chunks_.pop_back();
        }
    }

    void clear() {
        chunks_.clear();
        size_ = 0;
    }

    // Shrinks, or grows with default rows.
    void resize(std::size_t count) {
        while (size_ > count) {
            pop_back();
        }
        while (size_ < count) {
            push_back(T{});
        }
    }

    // Chunk k's storage: the same pointer as long as none of its rows changed
    // in this list and its copies. Rows [k * kChunkRows, ...) are in it.
    std::size_t chunk_count() const { return chunks_.size(); }
    const Chunk* chunk(std::size_t k) const { return chunks_[k].get(); }

    class const_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;
        const_iterator(const SharedRows* rows, std::size_t index) : rows_(rows), index_(index) {}
        reference operator*() const { return (*rows_)[index_]; }
        pointer operator->() const { return &(*rows_)[index_]; }
        const_iterator& operator++() {
            ++index_;
            return *this;
        }
        const_iterator operator++(int) {
            const_iterator was = *this;
            ++index_;
            return was;
        }
        bool operator==(const const_iterator& other) const { return index_ == other.index_; }
        bool operator!=(const const_iterator& other) const { return index_ != other.index_; }

    private:
        const SharedRows* rows_;
        std::size_t index_;
    };
    const_iterator begin() const { return const_iterator(this, 0); }
    const_iterator end() const { return const_iterator(this, size_); }

private:
    Chunk& own(std::size_t k) {
        std::shared_ptr<Chunk>& chunk = chunks_[k];
        if (chunk.use_count() != 1) {
            chunk = std::make_shared<Chunk>(*chunk);
        } else {
            // Another thread's last read of this chunk happens before its
            // release, which use_count saw: order that before this write.
            std::atomic_thread_fence(std::memory_order_acquire);
        }
        return *chunk;
    }

    std::vector<std::shared_ptr<Chunk>> chunks_;
    std::size_t size_ = 0;
};

}  // namespace engine_core
