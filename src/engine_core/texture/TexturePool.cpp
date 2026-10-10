#include "texture/TexturePool.hpp"

#include <algorithm>

namespace engine_core::texture {

TexturePool::TexturePool(int threads) {
    if (threads <= 0) {
        threads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 2);
    }
    for (int i = 0; i < threads; ++i) {
        threads_.emplace_back([this] { work(); });
    }
}

TexturePool::~TexturePool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        while (!queue_.empty()) queue_.pop();
    }
    wake_.notify_all();
    for (std::thread& thread : threads_) thread.join();
}

void TexturePool::submit(JobPriority priority, std::function<void()> job) {
    std::function<void(JobPriority)> on_submit;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        on_submit = on_submit_;
    }
    if (on_submit) on_submit(priority);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        queue_.push(Job{static_cast<int>(priority), next_sequence_++, std::move(job)});
    }
    wake_.notify_one();
}

void TexturePool::wait_idle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return queue_.empty() && running_ == 0; });
}

void TexturePool::set_on_submit(std::function<void(JobPriority)> on_submit) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_submit_ = std::move(on_submit);
}

TexturePool& TexturePool::shared() {
    static TexturePool pool;
    return pool;
}

void TexturePool::work() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            job = queue_.top();
            queue_.pop();
            ++running_;
        }
        job.run();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            --running_;
            if (queue_.empty() && running_ == 0) idle_.notify_all();
        }
    }
}

}  // namespace engine_core::texture
