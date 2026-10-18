#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace engine_network {

// Appends values to a growing buffer, little-endian.
class ByteWriter {
public:
    void u8(std::uint8_t value) { data_.push_back(value); }
    void u32(std::uint32_t value);
    // Seven bits a byte, low first, the high bit set on every byte but the last.
    void varuint(std::uint64_t value);
    void f64(double value);
    void string(std::string_view text);
    void bytes(const std::uint8_t* data, std::size_t count);
    const std::vector<std::uint8_t>& data() const { return data_; }
    std::vector<std::uint8_t>& data() { return data_; }
    void clear() { data_.clear(); }

private:
    std::vector<std::uint8_t> data_;
};

// Reads what ByteWriter wrote. It never reads past the end: a read that would
// sets failed() and returns 0 or "", and every later read does the same.
class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
    std::uint8_t u8();
    std::uint32_t u32();
    std::uint64_t varuint();
    double f64();
    // Refuses a length longer than what is left, before allocating anything.
    std::string string();
    bool failed() const { return failed_; }
    std::size_t left() const { return failed_ ? 0 : size_ - at_; }
    // Nothing left and nothing failed.
    bool done() const { return !failed_ && at_ == size_; }

private:
    bool take(std::size_t count);
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t at_ = 0;
    bool failed_ = false;
};

}  // namespace engine_network
