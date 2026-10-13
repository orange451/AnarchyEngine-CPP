#include "Socket.hpp"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <vector>

namespace network {

Address Address::ipv4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d, std::uint16_t port) {
    Address address;
    address.family = Family::IPv4;
    address.bytes = {a, b, c, d};
    address.port = port;
    return address;
}

namespace {

bool parse_port(std::string_view text, std::uint16_t& port) {
    if (text.empty() || text.size() > 5) return false;
    unsigned value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    if (value > 65535) return false;
    port = static_cast<std::uint16_t>(value);
    return true;
}

bool parse_ipv4(std::string_view text, std::array<std::uint8_t, 16>& bytes) {
    unsigned parts[4];
    int count = 0;
    unsigned value = 0;
    int digits = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '.') {
            if (digits == 0 || count == 4 || value > 255) return false;
            parts[count++] = value;
            value = 0;
            digits = 0;
            continue;
        }
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + static_cast<unsigned>(text[i] - '0');
        ++digits;
        if (digits > 3 || value > 255) return false;
    }
    if (count != 4) return false;
    for (int i = 0; i < 4; ++i) bytes[i] = static_cast<std::uint8_t>(parts[i]);
    return true;
}

// Hex groups separated by ':', with one "::" standing for the zero groups.
bool parse_ipv6(std::string_view text, std::array<std::uint8_t, 16>& bytes) {
    std::vector<std::uint16_t> head;
    std::vector<std::uint16_t> tail;
    bool seen_gap = false;
    std::vector<std::uint16_t>* current = &head;
    std::size_t i = 0;
    if (text.size() >= 2 && text[0] == ':' && text[1] == ':') {
        seen_gap = true;
        current = &tail;
        i = 2;
    }
    while (i < text.size()) {
        unsigned value = 0;
        int digits = 0;
        while (i < text.size() && text[i] != ':') {
            const char c = text[i];
            unsigned digit;
            if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = 10u + static_cast<unsigned>(c - 'a');
            else if (c >= 'A' && c <= 'F') digit = 10u + static_cast<unsigned>(c - 'A');
            else return false;
            value = value * 16 + digit;
            if (++digits > 4) return false;
            ++i;
        }
        if (digits == 0) return false;
        current->push_back(static_cast<std::uint16_t>(value));
        if (i == text.size()) break;
        ++i;   // the ':'
        if (i < text.size() && text[i] == ':') {
            if (seen_gap) return false;
            seen_gap = true;
            current = &tail;
            ++i;
        } else if (i == text.size()) {
            return false;
        }
    }
    const std::size_t total = head.size() + tail.size();
    if ((seen_gap && total > 7) || (!seen_gap && total != 8)) return false;
    std::uint16_t groups[8] = {};
    for (std::size_t g = 0; g < head.size(); ++g) groups[g] = head[g];
    for (std::size_t g = 0; g < tail.size(); ++g) groups[8 - tail.size() + g] = tail[g];
    for (int g = 0; g < 8; ++g) {
        bytes[g * 2] = static_cast<std::uint8_t>(groups[g] >> 8);
        bytes[g * 2 + 1] = static_cast<std::uint8_t>(groups[g] & 0xFF);
    }
    return true;
}

}  // namespace

std::optional<Address> Address::parse(std::string_view text) {
    Address address;
    if (!text.empty() && text[0] == '[') {
        const std::size_t close = text.find(']');
        if (close == std::string_view::npos || close + 1 >= text.size() || text[close + 1] != ':') return std::nullopt;
        if (!parse_ipv6(text.substr(1, close - 1), address.bytes)) return std::nullopt;
        if (!parse_port(text.substr(close + 2), address.port)) return std::nullopt;
        address.family = Family::IPv6;
        return address;
    }
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos) return std::nullopt;
    if (!parse_ipv4(text.substr(0, colon), address.bytes)) return std::nullopt;
    if (!parse_port(text.substr(colon + 1), address.port)) return std::nullopt;
    address.family = Family::IPv4;
    return address;
}

std::string Address::to_string() const {
    char buffer[64];
    if (family == Family::IPv4) {
        std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u:%u", bytes[0], bytes[1], bytes[2], bytes[3], port);
        return buffer;
    }
    if (family == Family::IPv6) {
        // The longest run of two or more zero groups becomes "::".
        std::uint16_t groups[8];
        for (int g = 0; g < 8; ++g) groups[g] = static_cast<std::uint16_t>((bytes[g * 2] << 8) | bytes[g * 2 + 1]);
        int best_start = -1;
        int best_length = 0;
        for (int g = 0; g < 8;) {
            if (groups[g] != 0) {
                ++g;
                continue;
            }
            int end = g;
            while (end < 8 && groups[end] == 0) ++end;
            if (end - g >= 2 && end - g > best_length) {
                best_start = g;
                best_length = end - g;
            }
            g = end;
        }
        std::string text = "[";
        for (int g = 0; g < 8;) {
            if (g == best_start) {
                text += "::";
                g += best_length;
                continue;
            }
            if (text.back() != '[' && text.back() != ':') text += ':';
            std::snprintf(buffer, sizeof(buffer), "%x", groups[g]);
            text += buffer;
            ++g;
        }
        std::snprintf(buffer, sizeof(buffer), "]:%u", port);
        return text + buffer;
    }
    return "none";
}

// The hub's datagrams: one queue per opened address.
struct LinkHub::State {
    struct Datagram {
        Address from;
        std::vector<std::uint8_t> bytes;
    };
    struct Slot {
        Address address;
        std::deque<Datagram> queue;
    };
    std::vector<Slot> slots;
    // Looked up fresh on every call: open() may grow the vector.
    Slot* find(const Address& address) {
        for (Slot& slot : slots) {
            if (slot.address == address) return &slot;
        }
        return nullptr;
    }
};

namespace {

class LinkSocket : public ISocket {
public:
    LinkSocket(std::shared_ptr<LinkHub::State> state, Address own) : state_(std::move(state)), own_(own) {}
    bool send(const Address& to, const std::uint8_t* bytes, std::size_t length) override {
        LinkHub::State::Slot* slot = state_->find(to);
        if (slot == nullptr) return false;
        slot->queue.push_back({own_, std::vector<std::uint8_t>(bytes, bytes + length)});
        return true;
    }
    std::size_t receive(Address& from, std::uint8_t* buffer, std::size_t capacity) override {
        LinkHub::State::Slot* slot = state_->find(own_);
        while (slot != nullptr && !slot->queue.empty()) {
            LinkHub::State::Datagram datagram = std::move(slot->queue.front());
            slot->queue.pop_front();
            // Dropped, as a socket drops an oversized datagram; an empty one is skipped too,
            // since 0 means "nothing waiting" to the caller.
            if (datagram.bytes.size() > capacity || datagram.bytes.empty()) continue;
            from = datagram.from;
            std::copy(datagram.bytes.begin(), datagram.bytes.end(), buffer);
            return datagram.bytes.size();
        }
        return 0;
    }
    bool failed() const override { return false; }

private:
    std::shared_ptr<LinkHub::State> state_;
    Address own_;
};

}  // namespace

LinkHub::LinkHub() : state_(std::make_shared<State>()) {}
LinkHub::~LinkHub() = default;

std::unique_ptr<ISocket> LinkHub::open(const Address& own) {
    if (state_->find(own) == nullptr) state_->slots.push_back({own, {}});
    return std::make_unique<LinkSocket>(state_, own);
}

std::size_t LinkHub::pending() const {
    std::size_t count = 0;
    for (const State::Slot& slot : state_->slots) count += slot.queue.size();
    return count;
}

}  // namespace network
