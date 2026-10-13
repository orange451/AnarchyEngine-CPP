#pragma once

// A growable array whose elements never move. Storage is a list of
// fixed-size chunks allocated as the array grows, so a reference or pointer
// to an element stays valid after later emplace_backs, which a std::vector
// would reallocate. Indexing costs one extra load.
//
// Per-slot tables use it: they used to reserve room for the whole instance
// cap up front so that nothing moved, which made memory follow the cap. Now
// memory follows what a place actually holds.

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace engine_core {

template <typename T, std::size_t ChunkBits = 12>
class StableArray {
public:
    static constexpr std::size_t kChunk = std::size_t{1} << ChunkBits;
    static constexpr std::size_t kMask = kChunk - 1;

    StableArray() = default;
    StableArray(const StableArray&) = delete;
    StableArray& operator=(const StableArray&) = delete;
    StableArray(StableArray&&) noexcept = default;
    StableArray& operator=(StableArray&&) noexcept = default;

    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    T& operator[](std::size_t index) { return chunks_[index >> ChunkBits][index & kMask]; }
    const T& operator[](std::size_t index) const { return chunks_[index >> ChunkBits][index & kMask]; }

    // Appends a value-initialized element, or one assigned from args.
    template <typename... Args>
    T& emplace_back(Args&&... args) {
        if ((size_ >> ChunkBits) == chunks_.size()) {
            chunks_.push_back(std::make_unique<T[]>(kChunk));
        }
        T& slot = (*this)[size_];
        if constexpr (sizeof...(Args) > 0) {
            slot = T(std::forward<Args>(args)...);
        }
        ++size_;
        return slot;
    }

    // Drops every element and frees the storage.
    void clear() {
        chunks_.clear();
        size_ = 0;
    }

    template <typename Owner, typename Value>
    class Iterator {
    public:
        Iterator(Owner* owner, std::size_t index) : owner_(owner), index_(index) {}
        Value& operator*() const { return (*owner_)[index_]; }
        Value* operator->() const { return &(*owner_)[index_]; }
        Iterator& operator++() {
            ++index_;
            return *this;
        }
        bool operator==(const Iterator& other) const { return index_ == other.index_; }
        bool operator!=(const Iterator& other) const { return index_ != other.index_; }

    private:
        Owner* owner_;
        std::size_t index_;
    };
    using iterator = Iterator<StableArray, T>;
    using const_iterator = Iterator<const StableArray, const T>;

    iterator begin() { return iterator(this, 0); }
    iterator end() { return iterator(this, size_); }
    const_iterator begin() const { return const_iterator(this, 0); }
    const_iterator end() const { return const_iterator(this, size_); }

private:
    // Chunk pointers move when this list grows; the chunks themselves never do.
    std::vector<std::unique_ptr<T[]>> chunks_;
    std::size_t size_ = 0;
};

}  // namespace engine_core
