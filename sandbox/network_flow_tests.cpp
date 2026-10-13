// The two-mode budget: full, or half while the link is bad (spec section 4).

#include "FlowControl.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;
using network::FlowControl;

TEST_CASE("N40 a good link keeps the configured rate", "[network][flow]") {
    FlowControl flow(32000.0);
    flow.update(0.0, 0.05, 0.0f);
    flow.update(1.0, 0.05, 0.05f);
    REQUIRE(flow.good());
    REQUIRE(flow.bytes_per_second() == Approx(32000.0));
}

TEST_CASE("N41 loss over 10% or RTT over 250 ms halves the rate at once", "[network][flow]") {
    FlowControl by_loss(32000.0);
    by_loss.update(0.0, 0.05, 0.0f);
    by_loss.update(0.1, 0.05, 0.11f);
    REQUIRE_FALSE(by_loss.good());
    REQUIRE(by_loss.bytes_per_second() == Approx(16000.0));
    FlowControl by_rtt(32000.0);
    by_rtt.update(0.0, 0.05, 0.0f);
    by_rtt.update(0.1, 0.26, 0.0f);
    REQUIRE(by_rtt.bytes_per_second() == Approx(16000.0));
}

TEST_CASE("N42 the rate recovers 10% a second and stops at the configured rate", "[network][flow]") {
    FlowControl flow(32000.0);
    flow.update(0.0, 0.05, 0.0f);
    flow.update(0.1, 0.05, 0.5f);
    REQUIRE(flow.bytes_per_second() == Approx(16000.0));
    flow.update(1.1, 0.05, 0.0f);
    REQUIRE(flow.bytes_per_second() == Approx(32000.0 * 0.6));
    flow.update(3.1, 0.05, 0.0f);
    REQUIRE(flow.bytes_per_second() == Approx(32000.0 * 0.8));
    flow.update(10.0, 0.05, 0.0f);
    REQUIRE(flow.good());
    REQUIRE(flow.bytes_per_second() == Approx(32000.0));
    // Bad again while recovering: back to half.
    flow.update(10.5, 0.05, 0.2f);
    REQUIRE(flow.bytes_per_second() == Approx(16000.0));
}
