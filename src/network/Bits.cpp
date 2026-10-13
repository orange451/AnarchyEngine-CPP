#include "Bits.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace network {

int bits_required(std::uint64_t range) {
    int count = 0;
    while (range > 0) {
        ++count;
        range >>= 1;
    }
    return count;
}

BitWriter::BitWriter(std::uint8_t* buffer, std::size_t capacity_bytes)
    : buffer_(buffer), capacity_bits_(capacity_bytes * 8) {}

void BitWriter::bits(std::uint32_t value, int count) {
    if (count <= 0 || overflowed_) return;
    if (bits_written_ + static_cast<std::size_t>(count) > capacity_bits_) {
        overflowed_ = true;
        return;
    }
    if (count < 32) value &= (1u << count) - 1u;
    scratch_ |= static_cast<std::uint64_t>(value) << scratch_bits_;
    scratch_bits_ += count;
    bits_written_ += static_cast<std::size_t>(count);
    while (scratch_bits_ >= 8) {
        buffer_[byte_index_++] = static_cast<std::uint8_t>(scratch_ & 0xFF);
        scratch_ >>= 8;
        scratch_bits_ -= 8;
    }
    // The partial byte is written too, so the buffer is complete at any time.
    if (scratch_bits_ > 0) buffer_[byte_index_] = static_cast<std::uint8_t>(scratch_ & 0xFF);
}

void BitWriter::integer(std::int64_t value, std::int64_t min, std::int64_t max) {
    const std::uint64_t range = static_cast<std::uint64_t>(max - min);
    const int count = bits_required(range);
    if (count == 0) return;
    const std::int64_t clamped = std::min(std::max(value, min), max);
    bits(static_cast<std::uint32_t>(clamped - min), count);
}

void BitWriter::float_bounded(float value, float min, float max, float resolution) {
    const float clamped = std::min(std::max(value, min), max);
    const std::uint32_t steps = static_cast<std::uint32_t>(std::ceil((max - min) / resolution));
    std::uint32_t q = static_cast<std::uint32_t>(std::lround((clamped - min) / resolution));
    if (q > steps) q = steps;
    integer(q, 0, steps);
}

void BitWriter::quaternion_smallest_three(const float q[4], int bits_per_component) {
    int largest = 0;
    for (int i = 1; i < 4; ++i) {
        if (std::fabs(q[i]) > std::fabs(q[largest])) largest = i;
    }
    // q and -q are one rotation; flip so the dropped component is positive.
    const float sign = q[largest] < 0.0f ? -1.0f : 1.0f;
    const float kBound = 0.707107f;
    const std::uint32_t range = (1u << bits_per_component) - 1u;
    bits(static_cast<std::uint32_t>(largest), 2);
    for (int i = 0; i < 4; ++i) {
        if (i == largest) continue;
        const float c = std::min(std::max(q[i] * sign, -kBound), kBound);
        const std::uint32_t v = static_cast<std::uint32_t>(std::lround((c + kBound) / (2.0f * kBound) * range));
        bits(std::min(v, range), bits_per_component);
    }
}

void BitWriter::align() {
    const int pad = static_cast<int>((8 - bits_written_ % 8) % 8);
    if (pad > 0) bits(0, pad);
}

void BitWriter::bytes(const std::uint8_t* data, std::size_t count) {
    if (overflowed_) return;
    const std::size_t pad = (8 - bits_written_ % 8) % 8;
    if (bits_written_ + pad + count * 8 > capacity_bits_) {
        overflowed_ = true;
        return;
    }
    align();
    std::memcpy(buffer_ + byte_index_, data, count);
    byte_index_ += count;
    bits_written_ += count * 8;
}

BitReader::BitReader(const std::uint8_t* buffer, std::size_t length_bytes)
    : buffer_(buffer), total_bits_(length_bytes * 8) {}

std::uint32_t BitReader::bits(int count) {
    if (count <= 0) return 0;
    if (bits_read_ + static_cast<std::size_t>(count) > total_bits_) {
        failed_ = true;
        return 0;
    }
    while (scratch_bits_ < count) {
        scratch_ |= static_cast<std::uint64_t>(buffer_[byte_index_++]) << scratch_bits_;
        scratch_bits_ += 8;
    }
    const std::uint64_t mask = count == 32 ? 0xFFFFFFFFull : ((1ull << count) - 1ull);
    const std::uint32_t value = static_cast<std::uint32_t>(scratch_ & mask);
    scratch_ >>= count;
    scratch_bits_ -= count;
    bits_read_ += static_cast<std::size_t>(count);
    return value;
}

std::int64_t BitReader::integer(std::int64_t min, std::int64_t max) {
    const int count = bits_required(static_cast<std::uint64_t>(max - min));
    if (count == 0) return min;
    return min + static_cast<std::int64_t>(bits(count));
}

float BitReader::float_bounded(float min, float max, float resolution) {
    const std::uint32_t steps = static_cast<std::uint32_t>(std::ceil((max - min) / resolution));
    const std::int64_t q = integer(0, steps);
    return std::min(max, min + static_cast<float>(q) * resolution);
}

void BitReader::quaternion_smallest_three(float q[4], int bits_per_component) {
    const int largest = static_cast<int>(bits(2));
    const float kBound = 0.707107f;
    const std::uint32_t range = (1u << bits_per_component) - 1u;
    float sum = 0.0f;
    for (int i = 0; i < 4; ++i) {
        if (i == largest) continue;
        const float c = static_cast<float>(bits(bits_per_component)) / static_cast<float>(range) * (2.0f * kBound) - kBound;
        q[i] = c;
        sum += c * c;
    }
    q[largest] = std::sqrt(std::max(0.0f, 1.0f - sum));
}

void BitReader::align() {
    const int pad = static_cast<int>((8 - bits_read_ % 8) % 8);
    if (pad > 0) bits(pad);
}

void BitReader::bytes(std::uint8_t* out, std::size_t count) {
    align();
    if (bits_read_ + count * 8 > total_bits_) {
        failed_ = true;
        std::memset(out, 0, count);
        return;
    }
    // After align() the scratch holds no bits, so the bytes are copied straight
    // from the buffer and the next bits() call resumes after them.
    std::memcpy(out, buffer_ + bits_read_ / 8, count);
    bits_read_ += count * 8;
    byte_index_ = bits_read_ / 8;
    scratch_ = 0;
    scratch_bits_ = 0;
}

}  // namespace network
