#include "StackThread.hpp"

#include "Contract.hpp"

#include <cerrno>
#include <cstdint>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace engine_core {
namespace {

// Runs the body, then frees it. The thread owns it from the start.
struct Start {
    std::function<void()> body;
};

void run_start(void* arg) {
    std::unique_ptr<Start> start(static_cast<Start*>(arg));
    start->body();
}

#if defined(_WIN32)
unsigned __stdcall windows_entry(void* arg) {
    run_start(arg);
    return 0;
}
#else
void* posix_entry(void* arg) {
    run_start(arg);
    return nullptr;
}
#endif

}  // namespace

struct StackThread::Handle {
#if defined(_WIN32)
    HANDLE thread = nullptr;
#else
    pthread_t thread{};
#endif
};

StackThread::StackThread(std::size_t stack_bytes, std::function<void()> body) {
    auto start = std::make_unique<Start>();
    start->body = std::move(body);
    auto handle = std::make_unique<Handle>();
#if defined(_WIN32)
    const std::uintptr_t made = _beginthreadex(nullptr, static_cast<unsigned>(stack_bytes), windows_entry, start.get(),
                                               STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (made == 0) {
        throw std::system_error(errno, std::generic_category(), "could not start a thread");
    }
    handle->thread = reinterpret_cast<HANDLE>(made);
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_bytes);
    const int failed = pthread_create(&handle->thread, &attr, posix_entry, start.get());
    pthread_attr_destroy(&attr);
    if (failed != 0) {
        throw std::system_error(failed, std::generic_category(), "could not start a thread");
    }
#endif
    // The thread has it now.
    start.release();
    handle_ = std::move(handle);
}

StackThread::StackThread() = default;

StackThread::StackThread(StackThread&& other) noexcept : handle_(std::move(other.handle_)) {}

StackThread& StackThread::operator=(StackThread&& other) noexcept {
    if (joinable()) {
        contract_fail("a StackThread assigned over before it was joined");
    }
    handle_ = std::move(other.handle_);
    return *this;
}

StackThread::~StackThread() {
    if (joinable()) {
        contract_fail("a StackThread destroyed before it was joined");
    }
}

void StackThread::join() {
    if (!handle_) {
        return;
    }
#if defined(_WIN32)
    WaitForSingleObject(handle_->thread, INFINITE);
    CloseHandle(handle_->thread);
#else
    pthread_join(handle_->thread, nullptr);
#endif
    handle_.reset();
}

}  // namespace engine_core
