// Bit packing and quantization for packets
// (docs/superpowers/specs/2026-10-12-network-transport-design.md, section 8).

#include "Bits.hpp"
#include "Sequence.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>

using Catch::Approx;
using network::BitReader;
using network::BitWriter;

TEST_CASE("N1 bits round-trip across byte boundaries and the buffer is complete up to bytes_written", "[network][bits]") {
    std::uint8_t buffer[16] = {};
    BitWriter writer(buffer, sizeof(buffer));
    writer.bits(5, 3);
    writer.bits(0x1FFFF, 17);
    writer.bits(0xFFFFFFFFu, 32);
    writer.bits(2, 2);
    REQUIRE(writer.bits_written() == 54);
    REQUIRE(writer.bytes_written() == 7);
    REQUIRE_FALSE(writer.overflowed());
    BitReader reader(buffer, writer.bytes_written());
    REQUIRE(reader.bits(3) == 5);
    REQUIRE(reader.bits(17) == 0x1FFFF);
    REQUIRE(reader.bits(32) == 0xFFFFFFFFu);
    REQUIRE(reader.bits(2) == 2);
    REQUIRE_FALSE(reader.failed());
}

TEST_CASE("N2 a write that does not fit sets overflowed and writes nothing", "[network][bits]") {
    std::uint8_t buffer[2] = {};
    BitWriter writer(buffer, sizeof(buffer));
    writer.bits(0xABCD, 16);
    writer.bits(1, 1);
    REQUIRE(writer.overflowed());
    REQUIRE(writer.bits_written() == 16);
    REQUIRE(buffer[0] == 0xCD);
    REQUIRE(buffer[1] == 0xAB);
}

TEST_CASE("N3 a read past the end fails and returns 0", "[network][bits]") {
    std::uint8_t buffer[1] = {0xFF};
    BitReader reader(buffer, 1);
    REQUIRE(reader.bits(8) == 0xFF);
    REQUIRE(reader.bits(1) == 0);
    REQUIRE(reader.failed());
}

TEST_CASE("N4 integer uses the fewest bits for its range", "[network][bits]") {
    REQUIRE(network::bits_required(0) == 0);
    REQUIRE(network::bits_required(1) == 1);
    REQUIRE(network::bits_required(255) == 8);
    REQUIRE(network::bits_required(256) == 9);
    std::uint8_t buffer[8] = {};
    BitWriter writer(buffer, sizeof(buffer));
    writer.integer(-3, -10, 10);   // 21 values: 5 bits
    writer.integer(7, 7, 7);       // one value: 0 bits
    writer.integer(99, 0, 50);     // clamped to 50
    REQUIRE(writer.bits_written() == 5 + 0 + 6);
    BitReader reader(buffer, writer.bytes_written());
    REQUIRE(reader.integer(-10, 10) == -3);
    REQUIRE(reader.integer(7, 7) == 7);
    REQUIRE(reader.integer(0, 50) == 50);
}

TEST_CASE("N5 float_bounded quantizes to the resolution and clamps the edges", "[network][bits]") {
    std::uint8_t buffer[16] = {};
    BitWriter writer(buffer, sizeof(buffer));
    writer.float_bounded(1.23456f, -256.0f, 256.0f, 1.0f / 4096.0f);
    writer.float_bounded(-300.0f, -256.0f, 256.0f, 1.0f / 4096.0f);
    writer.float_bounded(256.0f, -256.0f, 256.0f, 1.0f / 4096.0f);
    BitReader reader(buffer, writer.bytes_written());
    REQUIRE(reader.float_bounded(-256.0f, 256.0f, 1.0f / 4096.0f) == Approx(1.23456f).margin(1.0f / 4096.0f));
    REQUIRE(reader.float_bounded(-256.0f, 256.0f, 1.0f / 4096.0f) == Approx(-256.0f).margin(1.0f / 4096.0f));
    REQUIRE(reader.float_bounded(-256.0f, 256.0f, 1.0f / 4096.0f) == Approx(256.0f).margin(1.0f / 4096.0f));
}

TEST_CASE("N6 smallest-three keeps a unit quaternion within its precision, any largest component", "[network][bits]") {
    const float inputs[4][4] = {
        {0.9f, 0.1f, -0.2f, 0.3f},
        {0.1f, -0.95f, 0.2f, 0.1f},
        {-0.3f, 0.2f, 0.85f, -0.3f},
        {0.2f, 0.2f, 0.2f, -0.9f},
    };
    for (const float* raw : inputs) {
        float q[4];
        const float length = std::sqrt(raw[0] * raw[0] + raw[1] * raw[1] + raw[2] * raw[2] + raw[3] * raw[3]);
        for (int i = 0; i < 4; ++i) q[i] = raw[i] / length;
        std::uint8_t buffer[8] = {};
        BitWriter writer(buffer, sizeof(buffer));
        writer.quaternion_smallest_three(q, 15);
        REQUIRE(writer.bits_written() == 2 + 3 * 15);
        float out[4];
        BitReader reader(buffer, writer.bytes_written());
        reader.quaternion_smallest_three(out, 15);
        // The same rotation: q and -q are equal, so compare the absolute dot product.
        const float dot = std::fabs(q[0] * out[0] + q[1] * out[1] + q[2] * out[2] + q[3] * out[3]);
        REQUIRE(dot == Approx(1.0f).margin(1e-4f));
    }
}

TEST_CASE("N7 bytes align first and read back whole", "[network][bits]") {
    std::uint8_t buffer[16] = {};
    BitWriter writer(buffer, sizeof(buffer));
    writer.bits(1, 3);
    const std::uint8_t data[4] = {1, 2, 3, 4};
    writer.bytes(data, 4);
    REQUIRE(writer.bits_written() == 8 + 32);
    BitReader reader(buffer, writer.bytes_written());
    REQUIRE(reader.bits(3) == 1);
    reader.align();
    REQUIRE(reader.byte_offset() == 1);
    REQUIRE(reader.bytes_left() == 4);
    std::uint8_t out[4];
    reader.bytes(out, 4);
    REQUIRE(std::memcmp(out, data, 4) == 0);
    REQUIRE(reader.bytes_left() == 0);
}

TEST_CASE("N8 sequence comparison wraps", "[network][bits]") {
    REQUIRE(network::sequence_greater_than(1, 0));
    REQUIRE(network::sequence_greater_than(0, 65535));
    REQUIRE_FALSE(network::sequence_greater_than(65535, 0));
    REQUIRE(network::sequence_greater_than(32768, 0));
    REQUIRE_FALSE(network::sequence_greater_than(32769, 0));
    REQUIRE(network::sequence_diff(0, 65535) == 1);
    REQUIRE(network::sequence_diff(65535, 0) == -1);
    REQUIRE(network::sequence_diff(10, 3) == 7);
}
