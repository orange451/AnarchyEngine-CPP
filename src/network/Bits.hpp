#pragma once

#include <cstddef>
#include <cstdint>

namespace network {

// The bits that hold every value from 0 to range; 0 when range is 0.
int bits_required(std::uint64_t range);

// Packs values into a byte buffer, low bits first. A write that does not fit
// sets overflowed and is not written; the buffer is complete through
// bytes_written() after every write, so no flush is needed.
class BitWriter {
public:
    BitWriter(std::uint8_t* buffer, std::size_t capacity_bytes);

    void bits(std::uint32_t value, int count);
    void boolean(bool value) { bits(value ? 1u : 0u, 1); }
    void integer(std::int64_t value, std::int64_t min, std::int64_t max);
    void float_bounded(float value, float min, float max, float resolution);
    // Fiedler's smallest three: the index of the largest component in 2 bits,
    // then the other three, each in bits_per_component over ±0.707107.
    void quaternion_smallest_three(const float q[4], int bits_per_component);
    void align();
    void bytes(const std::uint8_t* data, std::size_t count);

    bool overflowed() const { return overflowed_; }
    std::size_t bits_written() const { return bits_written_; }
    std::size_t bytes_written() const { return (bits_written_ + 7) / 8; }
    std::size_t bits_left() const { return capacity_bits_ - bits_written_; }

private:
    std::uint8_t* buffer_;
    std::size_t capacity_bits_;
    std::size_t bits_written_ = 0;
    std::size_t byte_index_ = 0;
    std::uint64_t scratch_ = 0;
    int scratch_bits_ = 0;
    bool overflowed_ = false;
};

// Reads what BitWriter wrote. A read past the end sets failed and returns 0;
// callers check failed() once at the end rather than after every read.
class BitReader {
public:
    BitReader(const std::uint8_t* buffer, std::size_t length_bytes);

    std::uint32_t bits(int count);
    bool boolean() { return bits(1) != 0; }
    std::int64_t integer(std::int64_t min, std::int64_t max);
    float float_bounded(float min, float max, float resolution);
    void quaternion_smallest_three(float q[4], int bits_per_component);
    void align();
    void bytes(std::uint8_t* out, std::size_t count);

    bool failed() const { return failed_; }
    std::size_t bits_read() const { return bits_read_; }
    std::size_t byte_offset() const { return bits_read_ / 8; }
    std::size_t bytes_left() const { return (total_bits_ - bits_read_) / 8; }

private:
    const std::uint8_t* buffer_;
    std::size_t total_bits_;
    std::size_t bits_read_ = 0;
    std::size_t byte_index_ = 0;
    std::uint64_t scratch_ = 0;
    int scratch_bits_ = 0;
    bool failed_ = false;
};

}  // namespace network
