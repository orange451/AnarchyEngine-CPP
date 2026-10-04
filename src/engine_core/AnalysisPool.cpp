#include "AnalysisPool.hpp"

#include <algorithm>
#include <memory>
#include <thread>

namespace engine_core {

AnalysisPool::AnalysisPool(unsigned threads, std::size_t stack_bytes) {
    threads = std::max(1u, threads);
    threads_.reserve(threads);
    try {
        for (unsigned i = 0; i < threads; ++i) {
            threads_.emplace_back(stack_bytes, [this] { work(); });
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (StackThread& thread : threads_) {
            thread.join();
        }
        throw;
    }
}

AnalysisPool::~AnalysisPool() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    for (StackThread& thread : threads_) {
        thread.join();
    }
}

void AnalysisPool::post(std::vector<std::function<void()>> tasks) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (std::function<void()>& task : tasks) {
            queue_.push_back(std::move(task));
        }
    }
    cv_.notify_all();
}

void AnalysisPool::run_all(std::vector<std::function<void()>> tasks) {
    if (tasks.empty()) {
        return;
    }
    struct Waiter {
        std::mutex mu;
        std::condition_variable cv;
        std::size_t left = 0;
    };
    auto waiter = std::make_shared<Waiter>();
    waiter->left = tasks.size();
    std::vector<std::function<void()>> counted;
    counted.reserve(tasks.size());
    for (std::function<void()>& task : tasks) {
        counted.push_back([task = std::move(task), waiter] {
            task();
            {
                std::lock_guard<std::mutex> lock(waiter->mu);
                --waiter->left;
            }
            waiter->cv.notify_all();
        });
    }
    post(std::move(counted));
    std::unique_lock<std::mutex> lock(waiter->mu);
    waiter->cv.wait(lock, [&waiter] { return waiter->left == 0; });
}

unsigned AnalysisPool::default_size() {
    const unsigned hardware = std::thread::hardware_concurrency();
    return hardware > 1 ? hardware - 1 : 1;
}

void AnalysisPool::work() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) {
                return;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        task();
    }
}

}  // namespace engine_core
