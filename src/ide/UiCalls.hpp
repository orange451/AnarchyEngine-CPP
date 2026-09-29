#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace ide {

// Work the MCP server threads hand to the UI thread, and their waits. The
// studio closes by joining those threads from the UI thread, which then never
// runs the work, so close() ends every wait at once instead of letting each
// one run out.
class UiCalls {
public:
    // Queues a task for the UI thread, such as jadefx::runLater.
    using Post = std::function<void(std::function<void()>)>;

    explicit UiCalls(Post post) : shared_(std::make_shared<Shared>()), post_(std::move(post)) {}

    // Runs fn on the UI thread and waits for it. Throws with what fn threw, when
    // the wait runs out, or when the studio closes first. Server threads.
    void run(std::function<void()> fn, std::chrono::milliseconds wait) const {
        struct Call {
            bool done = false;
            std::string error;
        };
        auto call = std::make_shared<Call>();
        {
            std::lock_guard<std::mutex> guard(shared_->mu);
            if (shared_->closing) {
                throw std::runtime_error(kClosing);
            }
        }
        post_([shared = shared_, call, fn = std::move(fn)] {
            std::string error;
            try {
                fn();
            } catch (const std::exception& ex) {
                error = ex.what();
            }
            {
                std::lock_guard<std::mutex> guard(shared->mu);
                call->error = std::move(error);
                call->done = true;
            }
            shared->cv.notify_all();
        });
        std::unique_lock<std::mutex> lock(shared_->mu);
        shared_->cv.wait_for(lock, wait, [&] { return call->done || shared_->closing; });
        if (call->done) {
            if (!call->error.empty()) {
                throw std::runtime_error(call->error);
            }
            return;
        }
        if (shared_->closing) {
            throw std::runtime_error(kClosing);
        }
        throw std::runtime_error("The studio did not respond within " + describe(wait) + ".");
    }

    // Waits until ready() holds, checked under this object's lock. False when
    // the wait runs out. Throws when the studio closes first.
    bool wait_until(const std::function<bool()>& ready, std::chrono::milliseconds wait) const {
        std::unique_lock<std::mutex> lock(shared_->mu);
        shared_->cv.wait_for(lock, wait, [&] { return ready() || shared_->closing; });
        if (ready()) {
            return true;
        }
        if (shared_->closing) {
            throw std::runtime_error(kClosing);
        }
        return false;
    }

    // Makes a change a wait_until reads, under this object's lock, and wakes it.
    void update(const std::function<void()>& change) const {
        {
            std::lock_guard<std::mutex> guard(shared_->mu);
            change();
        }
        shared_->cv.notify_all();
    }

    // Ends every wait and refuses later calls. The UI thread, as the studio closes.
    void close() const {
        {
            std::lock_guard<std::mutex> guard(shared_->mu);
            shared_->closing = true;
        }
        shared_->cv.notify_all();
    }

private:
    static constexpr const char* kClosing = "The studio is closing.";

    // Outlives this object in tasks the UI thread has not run yet.
    struct Shared {
        std::mutex mu;
        std::condition_variable cv;
        bool closing = false;
    };

    static std::string describe(std::chrono::milliseconds wait) {
        if (wait.count() % 1000 != 0) {
            return std::to_string(wait.count()) + " ms";
        }
        const long long seconds = wait.count() / 1000;
        return std::to_string(seconds) + (seconds == 1 ? " second" : " seconds");
    }

    std::shared_ptr<Shared> shared_;
    Post post_;
};

}  // namespace ide
