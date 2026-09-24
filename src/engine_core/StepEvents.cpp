#include "StepEvents.hpp"

namespace engine_core {

void StepEvents::publish(double dt) {
    if (dt < 0) {
        dt = 0;
    }
    std::lock_guard<std::mutex> guard(mu_);
    if (size_ == kCapacity) {
        const int newest = (head_ + size_ - 1) % kCapacity;
        dts_[newest] += dt;
        return;
    }
    dts_[(head_ + size_) % kCapacity] = dt;
    ++size_;
}

double StepEvents::consume() {
    std::lock_guard<std::mutex> guard(mu_);
    double sum = 0;
    while (size_ > 0) {
        sum += dts_[head_];
        head_ = (head_ + 1) % kCapacity;
        --size_;
    }
    return sum;
}

}  // namespace engine_core
