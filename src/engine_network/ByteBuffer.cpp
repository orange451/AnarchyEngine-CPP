#include "ByteBuffer.hpp"

#include <cstring>

namespace engine_network {

void ByteWriter::u32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        data_.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void ByteWriter::varuint(std::uint64_t value) {
    while (value >= 0x80) {
        data_.push_back(static_cast<std::uint8_t>(value | 0x80));
        value >>= 7;
    }
    data_.push_back(static_cast<std::uint8_t>(value));
}

void ByteWriter::f64(double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int shift = 0; shift < 64; shift += 8) {
        data_.push_back(static_cast<std::uint8_t>(bits >> shift));
    }
}

void ByteWriter::string(std::string_view text) {
    varuint(text.size());
    bytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

void ByteWriter::bytes(const std::uint8_t* data, std::size_t count) {
    data_.insert(data_.end(), data, data + count);
}

bool ByteReader::take(std::size_t count) {
    if (failed_ || count > size_ - at_) {
        failed_ = true;
        return false;
    }
    return true;
}

std::uint8_t ByteReader::u8() {
    if (!take(1)) {
        return 0;
    }
    return data_[at_++];
}

std::uint32_t ByteReader::u32() {
    if (!take(4)) {
        return 0;
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(data_[at_++]) << (8 * i);
    }
    return value;
}

std::uint64_t ByteReader::varuint() {
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        const std::uint8_t byte = u8();
        if (failed_) {
            return 0;
        }
        if (shift == 63 && byte > 1) {
            failed_ = true;   // the tenth byte holds one bit; more would overflow 64
            return 0;
        }
        value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            return value;
        }
    }
    failed_ = true;   // more than ten bytes: not a varint this writer makes
    return 0;
}

double ByteReader::f64() {
    if (!take(8)) {
        return 0.0;
    }
    std::uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) {
        bits |= static_cast<std::uint64_t>(data_[at_++]) << (8 * i);
    }
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::size_t ByteReader::take_rest(const std::uint8_t*& data) {
    const std::size_t count = left();
    data = data_ + (failed_ ? size_ : at_);
    at_ += count;
    return count;
}

std::string ByteReader::string() {
    const std::uint64_t length = varuint();
    if (failed_ || length > size_ - at_) {
        failed_ = true;
        return std::string();
    }
    std::string text(reinterpret_cast<const char*>(data_ + at_), static_cast<std::size_t>(length));
    at_ += static_cast<std::size_t>(length);
    return text;
}

}  // namespace engine_network
