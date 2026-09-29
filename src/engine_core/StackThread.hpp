#pragma once

#include <cstddef>
#include <functional>
#include <memory>

namespace engine_core {

// A thread with a stack of a size the caller picks, where std::thread takes the
// platform's default: 1 MB on Windows, 512 KB for a secondary thread on macOS.
// Luau's parser and type checker recurse as deep as their own limits allow,
// which in a Debug build needs more than that. Only the reservation is this
// size; pages are committed as the stack grows.
//
// Like std::thread: join before it is destroyed or assigned over.
class StackThread {
public:
    StackThread();
    StackThread(std::size_t stack_bytes, std::function<void()> body);
    StackThread(StackThread&& other) noexcept;
    StackThread& operator=(StackThread&& other) noexcept;
    StackThread(const StackThread&) = delete;
    StackThread& operator=(const StackThread&) = delete;
    ~StackThread();

    bool joinable() const { return handle_ != nullptr; }
    void join();

private:
    struct Handle;
    std::unique_ptr<Handle> handle_;
};

}  // namespace engine_core
