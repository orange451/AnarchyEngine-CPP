#pragma once

#include <atomic>
#include <functional>
#include <memory>

namespace ide {

// Set from any thread when something a panel shows has changed. The panel
// takes it in its layout pass, which runs once a frame, so any number of
// changes in one frame redraw it once. It starts set, so the first pass reads.
class ChangeFlag {
public:
    ChangeFlag() : flag_(std::make_shared<std::atomic<bool>>(true)) {}

    // A setter for another thread to hold. It stays safe after the panel is gone.
    std::function<void()> setter() const {
        std::shared_ptr<std::atomic<bool>> flag = flag_;
        return [flag] { flag->store(true, std::memory_order_release); };
    }

    void set() { flag_->store(true, std::memory_order_release); }

    // True once for each set, clearing it.
    bool take() { return flag_->exchange(false, std::memory_order_acq_rel); }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

}  // namespace ide
