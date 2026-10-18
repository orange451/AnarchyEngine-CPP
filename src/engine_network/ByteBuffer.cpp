#include "ByteBuffer.hpp"

#include <cstring>

// Values are copied in the host's byte order, which must be the wire's.
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "ByteBuffer copies values in host order, and the wire is little-endian"
#endif

namespace engine_network {

namespace {

// One value's bytes appended in a single step, not a push_back each.
template <typename T>
void append(std::vector<std::uint8_t>& data, T value) {
    const std::size_t at = data.size();
    data.resize(at + sizeof(T));
    std::memcpy(data.data() + at, &value, sizeof(T));
}

}  // namespace

void ByteWriter::u32(std::uint32_t value) { append(data_, value); }

void ByteWriter::varuint(std::uint64_t value) {
    while (value >= 0x80) {
        data_.push_back(static_cast<std::uint8_t>(value | 0x80));
        value >>= 7;
    }
    data_.push_back(static_cast<std::uint8_t>(value));
}

void ByteWriter::f32(float value) { append(data_, value); }

void ByteWriter::f64(double value) { append(data_, value); }

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
    std::memcpy(&value, data_ + at_, sizeof(value));
    at_ += sizeof(value);
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

float ByteReader::f32() {
    if (!take(4)) {
        return 0.0f;
    }
    float value = 0.0f;
    std::memcpy(&value, data_ + at_, sizeof(value));
    at_ += sizeof(value);
    return value;
}

double ByteReader::f64() {
    if (!take(8)) {
        return 0.0;
    }
    double value = 0.0;
    std::memcpy(&value, data_ + at_, sizeof(value));
    at_ += sizeof(value);
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
