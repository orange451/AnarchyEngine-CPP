#include "FlowControl.hpp"

#include <algorithm>

namespace network {

void FlowControl::update(double now, double rtt, float loss) {
    const double dt = last_update_ < 0.0 ? 0.0 : std::max(0.0, now - last_update_);
    last_update_ = now;
    if (loss > 0.10f || rtt > 0.250) {
        fraction_ = 0.5;
        return;
    }
    fraction_ = std::min(1.0, fraction_ + 0.1 * dt);
}

}  // namespace network
