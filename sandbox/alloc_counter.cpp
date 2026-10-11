// The sandbox's global operator new and delete: malloc and free, counted
// while alloc_counter is armed. Every form is replaced (plain, array,
// aligned, nothrow), so nothing the engine allocates goes uncounted.

#include "alloc_counter.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {

std::atomic<bool> armed{false};
std::atomic<std::size_t> allocations{0};

void note() {
    if (armed.load(std::memory_order_relaxed)) {
        allocations.fetch_add(1, std::memory_order_relaxed);
    }
}

void* plain(std::size_t size) {
    note();
    void* memory = std::malloc(size == 0 ? 1 : size);
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    return memory;
}

void* aligned(std::size_t size, std::align_val_t alignment) {
    note();
    const std::size_t align = static_cast<std::size_t>(alignment);
#if defined(_WIN32)
    void* memory = _aligned_malloc(size == 0 ? 1 : size, align);
#else
    void* memory = nullptr;
    if (posix_memalign(&memory, align, size == 0 ? 1 : size) != 0) {
        memory = nullptr;
    }
#endif
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    return memory;
}

void release_aligned(void* memory) {
#if defined(_WIN32)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

}  // namespace

namespace alloc_counter {

void arm() {
    allocations.store(0);
    armed.store(true);
}

void disarm() { armed.store(false); }

std::size_t count() { return allocations.load(); }

}  // namespace alloc_counter

void* operator new(std::size_t size) { return plain(size); }
void* operator new[](std::size_t size) { return plain(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return plain(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return plain(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new(std::size_t size, std::align_val_t alignment) { return aligned(size, alignment); }
void* operator new[](std::size_t size, std::align_val_t alignment) { return aligned(size, alignment); }
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return aligned(size, alignment);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return aligned(size, alignment);
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete[](void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete(void* memory, std::align_val_t) noexcept { release_aligned(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { release_aligned(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { release_aligned(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept { release_aligned(memory); }
void operator delete(void* memory, std::align_val_t, const std::nothrow_t&) noexcept { release_aligned(memory); }
void operator delete[](void* memory, std::align_val_t, const std::nothrow_t&) noexcept { release_aligned(memory); }
