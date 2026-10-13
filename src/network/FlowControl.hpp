#pragma once

namespace network {

// Fiedler's two-mode flow control: the full rate on a good link, half of it
// the moment loss passes 10% or RTT passes 250 ms, and 10% a second back
// toward full once the link is good again.
class FlowControl {
public:
    explicit FlowControl(double bytes_per_second) : configured_(bytes_per_second) {}
    void update(double now, double rtt, float loss);
    double bytes_per_second() const { return configured_ * fraction_; }
    bool good() const { return fraction_ >= 1.0; }

private:
    double configured_;
    double fraction_ = 1.0;
    double last_update_ = -1.0;
};

}  // namespace network
