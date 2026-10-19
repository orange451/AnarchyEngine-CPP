#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace network {

// An IP address and port. Only literal addresses parse; resolve() below also
// looks names up.
struct Address {
    enum class Family : std::uint8_t { None, IPv4, IPv6 };
    Family family = Family::None;
    std::array<std::uint8_t, 16> bytes{};
    std::uint16_t port = 0;

    static Address ipv4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d, std::uint16_t port);
    static Address loopback(std::uint16_t port) { return ipv4(127, 0, 0, 1, port); }
    // "1.2.3.4:5" or "[::1]:5".
    static std::optional<Address> parse(std::string_view text);
    std::string to_string() const;

    bool operator==(const Address& other) const {
        return family == other.family && port == other.port && bytes == other.bytes;
    }
    bool operator!=(const Address& other) const { return !(*this == other); }
};

// "host:port": a literal, as Address::parse, or a name looked up with the
// system resolver, preferring an IPv4 answer. nullopt with error worded for a player.
// It blocks while the resolver works.
std::optional<Address> resolve(std::string_view host_port, std::string& error);

// What an Endpoint sends and receives through: a real socket, a link in one
// process, or the conditioner wrapping either.
class ISocket {
public:
    virtual ~ISocket() = default;
    virtual bool send(const Address& to, const std::uint8_t* bytes, std::size_t length) = 0;
    // The next datagram waiting, or 0. A datagram over capacity is dropped.
    virtual std::size_t receive(Address& from, std::uint8_t* buffer, std::size_t capacity) = 0;
    virtual bool failed() const = 0;
};

// Sockets in one process that deliver to each other by address, for tests.
class LinkHub {
public:
    LinkHub();
    ~LinkHub();
    std::unique_ptr<ISocket> open(const Address& own);
    std::size_t pending() const;

    struct State;

private:
    std::shared_ptr<State> state_;
};

// A non-blocking UDP socket over Winsock or BSD sockets, dual-stack: IPv4
// peers arrive as mapped addresses and are given back as IPv4. A host that
// cannot make a dual-stack socket gets an IPv4 one, which cannot reach IPv6 peers.
class UdpSocket : public ISocket {
public:
    static std::unique_ptr<UdpSocket> open(std::uint16_t port, std::string& error);
    // IPv4 only: a send to an IPv6 address returns false and leaves the socket working.
    static std::unique_ptr<UdpSocket> open_ipv4(std::uint16_t port, std::string& error);
    ~UdpSocket() override;
    std::uint16_t port() const { return port_; }
    bool send(const Address& to, const std::uint8_t* bytes, std::size_t length) override;
    std::size_t receive(Address& from, std::uint8_t* buffer, std::size_t capacity) override;
    bool failed() const override { return failed_; }

private:
    UdpSocket() = default;
    std::uintptr_t handle_ = 0;
    std::uint16_t port_ = 0;
    bool failed_ = false;
    bool ipv4_ = false;
};

}  // namespace network
