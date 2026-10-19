#include "Socket.hpp"

#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>   // SIO_UDP_CONNRESET
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace network {

namespace {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalid = INVALID_SOCKET;
// Winsock is started once for the process and never stopped: WSACleanup
// counts calls, and the last socket closing does not need it.
bool start_sockets() {
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return started;
}
int last_error() { return WSAGetLastError(); }
bool would_block(int error) { return error == WSAEWOULDBLOCK; }
bool oversized(int error) { return error == WSAEMSGSIZE; }
bool peer_gone(int error) { return error == WSAECONNRESET; }
// Errors that mean the socket itself is unusable; everything else is per-send or transient.
bool fatal(int error) {
    return error == WSAENOTSOCK || error == WSANOTINITIALISED || error == WSAEINVAL;
}
void close_socket(socket_t s) { closesocket(s); }
constexpr int kReceiveFlags = 0;
#else
using socket_t = int;
constexpr socket_t kInvalid = -1;
bool start_sockets() { return true; }
int last_error() { return errno; }
bool would_block(int error) { return error == EWOULDBLOCK || error == EAGAIN; }
bool oversized(int) { return false; }
bool peer_gone(int error) { return error == ECONNREFUSED; }
// Errors that mean the socket itself is unusable; everything else is per-send or transient.
bool fatal(int error) { return error == ENOTSOCK || error == EBADF || error == EINVAL; }
void close_socket(socket_t s) { ::close(s); }
// MSG_TRUNC reports the datagram's real length, so an oversized one is dropped
// here as Windows and the link drop it.
constexpr int kReceiveFlags = MSG_TRUNC;
#endif

std::string error_text(int error) {
#ifdef _WIN32
    char* text = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   static_cast<DWORD>(error), 0, reinterpret_cast<char*>(&text), 0, nullptr);
    std::string result = text != nullptr ? text : "socket error";
    if (text != nullptr) LocalFree(text);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ')) result.pop_back();
    return result;
#else
    return std::strerror(error);
#endif
}

// The sockaddr to send to: an IPv4 socket takes sockaddr_in, a dual-stack one
// sockaddr_in6 with IPv4 peers as mapped addresses. Returns its length.
socklen_t to_sockaddr(const Address& address, bool ipv4, sockaddr_storage& out) {
    std::memset(&out, 0, sizeof(out));
    if (ipv4) {
        sockaddr_in& in = reinterpret_cast<sockaddr_in&>(out);
        in.sin_family = AF_INET;
        in.sin_port = htons(address.port);
        std::memcpy(&in.sin_addr, address.bytes.data(), 4);
        return sizeof(sockaddr_in);
    }
    sockaddr_in6& in6 = reinterpret_cast<sockaddr_in6&>(out);
    in6.sin6_family = AF_INET6;
    in6.sin6_port = htons(address.port);
    if (address.family == Address::Family::IPv6) {
        std::memcpy(&in6.sin6_addr, address.bytes.data(), 16);
    } else {
        const std::uint8_t mapped[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF,
                                         address.bytes[0], address.bytes[1], address.bytes[2], address.bytes[3]};
        std::memcpy(&in6.sin6_addr, mapped, 16);
    }
    return sizeof(sockaddr_in6);
}

// Either family; a mapped IPv6 address comes back as IPv4.
Address from_sockaddr(const sockaddr* in) {
    if (in->sa_family == AF_INET) {
        const sockaddr_in& in4 = *reinterpret_cast<const sockaddr_in*>(in);
        const std::uint8_t* raw = reinterpret_cast<const std::uint8_t*>(&in4.sin_addr);
        return Address::ipv4(raw[0], raw[1], raw[2], raw[3], ntohs(in4.sin_port));
    }
    const sockaddr_in6& in6 = *reinterpret_cast<const sockaddr_in6*>(in);
    const std::uint8_t* raw = reinterpret_cast<const std::uint8_t*>(&in6.sin6_addr);
    bool mapped = raw[10] == 0xFF && raw[11] == 0xFF;
    for (int i = 0; i < 10; ++i) mapped = mapped && raw[i] == 0;
    if (mapped) return Address::ipv4(raw[12], raw[13], raw[14], raw[15], ntohs(in6.sin6_port));
    Address address;
    address.family = Address::Family::IPv6;
    std::memcpy(address.bytes.data(), raw, 16);
    address.port = ntohs(in6.sin6_port);
    return address;
}

// Sizes the buffers, binds port, makes the socket non-blocking, and reads back
// the port bound. On failure the handle is closed and error says why.
bool prepare(socket_t handle, int family, std::uint16_t port, std::uint16_t& bound_port, std::string& error) {
    // Best effort: a busy server drains a burst of full packets per tick, and the default
    // buffers would drop some of them.
    const int buffer_bytes = 4 * 1024 * 1024;
    setsockopt(handle, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buffer_bytes), sizeof(buffer_bytes));
    setsockopt(handle, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buffer_bytes), sizeof(buffer_bytes));
    sockaddr_storage bind_address;
    std::memset(&bind_address, 0, sizeof(bind_address));
    socklen_t bind_length;
    if (family == AF_INET) {
        sockaddr_in& in = reinterpret_cast<sockaddr_in&>(bind_address);
        in.sin_family = AF_INET;
        in.sin_addr.s_addr = htonl(INADDR_ANY);
        in.sin_port = htons(port);
        bind_length = sizeof(sockaddr_in);
    } else {
        sockaddr_in6& in6 = reinterpret_cast<sockaddr_in6&>(bind_address);
        in6.sin6_family = AF_INET6;
        in6.sin6_addr = in6addr_any;
        in6.sin6_port = htons(port);
        bind_length = sizeof(sockaddr_in6);
    }
    if (::bind(handle, reinterpret_cast<const sockaddr*>(&bind_address), bind_length) != 0) {
        error = "cannot bind port " + std::to_string(port) + ": " + error_text(last_error());
        close_socket(handle);
        return false;
    }
#ifdef _WIN32
    u_long non_blocking = 1;
    if (ioctlsocket(handle, FIONBIO, &non_blocking) != 0) {
        error = "cannot make the socket non-blocking: " + error_text(last_error());
        close_socket(handle);
        return false;
    }
    // Without this, an ICMP port-unreachable from a peer that went away makes
    // the next recvfrom fail with WSAECONNRESET.
    BOOL report = FALSE;
    DWORD returned = 0;
    WSAIoctl(handle, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &returned, nullptr, nullptr);
#else
    if (fcntl(handle, F_SETFL, fcntl(handle, F_GETFL, 0) | O_NONBLOCK) != 0) {
        error = "cannot make the socket non-blocking: " + error_text(last_error());
        close_socket(handle);
        return false;
    }
#endif
    sockaddr_storage bound;
    socklen_t length = sizeof(bound);
    if (getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
        error = "cannot read the socket's port: " + error_text(last_error());
        close_socket(handle);
        return false;
    }
    bound_port = from_sockaddr(reinterpret_cast<const sockaddr*>(&bound)).port;
    return true;
}

// A port as typed: 1 to 5 digits, at most 65535.
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

}  // namespace

std::unique_ptr<UdpSocket> UdpSocket::open(std::uint16_t port, std::string& error) {
    if (!start_sockets()) {
        error = "Winsock did not start";
        return nullptr;
    }
    // A host with IPv6 turned off cannot make this socket, and one that cannot clear
    // IPV6_V6ONLY would never hear IPv4 peers: both get an IPv4 socket instead.
    const socket_t handle = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == kInvalid) return open_ipv4(port, error);
    int off = 0;
    if (setsockopt(handle, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off)) != 0) {
        close_socket(handle);
        return open_ipv4(port, error);
    }
    std::uint16_t bound_port = 0;
    if (!prepare(handle, AF_INET6, port, bound_port, error)) return nullptr;
    std::unique_ptr<UdpSocket> socket(new UdpSocket());
    socket->handle_ = static_cast<std::uintptr_t>(handle);
    socket->port_ = bound_port;
    return socket;
}

std::unique_ptr<UdpSocket> UdpSocket::open_ipv4(std::uint16_t port, std::string& error) {
    if (!start_sockets()) {
        error = "Winsock did not start";
        return nullptr;
    }
    const socket_t handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == kInvalid) {
        error = error_text(last_error());
        return nullptr;
    }
    std::uint16_t bound_port = 0;
    if (!prepare(handle, AF_INET, port, bound_port, error)) return nullptr;
    std::unique_ptr<UdpSocket> socket(new UdpSocket());
    socket->handle_ = static_cast<std::uintptr_t>(handle);
    socket->port_ = bound_port;
    socket->ipv4_ = true;
    return socket;
}

UdpSocket::~UdpSocket() {
    if (handle_ != 0) close_socket(static_cast<socket_t>(handle_));
}

bool UdpSocket::send(const Address& to, const std::uint8_t* bytes, std::size_t length) {
    if (failed_ || to.family == Address::Family::None) return false;
    // Out of an IPv4 socket's reach, which says nothing about the socket.
    if (ipv4_ && to.family == Address::Family::IPv6) return false;
    sockaddr_storage address;
    const socklen_t address_length = to_sockaddr(to, ipv4_, address);
    const auto sent = ::sendto(static_cast<socket_t>(handle_), reinterpret_cast<const char*>(bytes), static_cast<int>(length), 0,
                               reinterpret_cast<const sockaddr*>(&address), address_length);
    if (sent < 0) {
        const int error = last_error();
        if (would_block(error) || peer_gone(error)) return false;
        if (fatal(error)) failed_ = true;
        return false;
    }
    return true;
}

std::size_t UdpSocket::receive(Address& from, std::uint8_t* buffer, std::size_t capacity) {
    if (failed_) return 0;
    for (;;) {
        sockaddr_storage address;
        socklen_t length = sizeof(address);
        const auto got = ::recvfrom(static_cast<socket_t>(handle_), reinterpret_cast<char*>(buffer), static_cast<int>(capacity),
                                    kReceiveFlags, reinterpret_cast<sockaddr*>(&address), &length);
        if (got < 0) {
            const int error = last_error();
            if (would_block(error)) return 0;
            if (oversized(error) || peer_gone(error)) continue;   // that datagram is gone; look for the next
            if (fatal(error)) failed_ = true;
            return 0;
        }
        // MSG_TRUNC: oversized, dropped. An empty datagram is skipped too, since 0 means "nothing waiting".
        if (got == 0 || static_cast<std::size_t>(got) > capacity) continue;
        from = from_sockaddr(reinterpret_cast<const sockaddr*>(&address));
        return static_cast<std::size_t>(got);
    }
}

std::optional<Address> resolve(std::string_view host_port, std::string& error) {
    if (std::optional<Address> literal = Address::parse(host_port)) return literal;
    static const char* const kShape = "Enter the server as host:port, for example localhost:7777";
    // A bracketed literal that did not parse is malformed; a name never has brackets.
    const std::size_t colon = host_port.rfind(':');
    if (host_port.empty() || host_port.front() == '[' || colon == std::string_view::npos) {
        error = kShape;
        return std::nullopt;
    }
    const std::string host(host_port.substr(0, colon));
    std::uint16_t port = 0;
    if (host.empty() || !parse_port(host_port.substr(colon + 1), port)) {
        error = kShape;
        return std::nullopt;
    }
    if (!start_sockets()) {
        error = "Networking did not start";
        return std::nullopt;
    }
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || found == nullptr) {
        error = "Could not find a server named " + host;
        return std::nullopt;
    }
    // The first IPv4 answer, since every socket can reach one; else the first IPv6.
    const addrinfo* chosen = nullptr;
    for (const addrinfo* entry = found; entry != nullptr; entry = entry->ai_next) {
        if (entry->ai_family == AF_INET) {
            chosen = entry;
            break;
        }
        if (entry->ai_family == AF_INET6 && chosen == nullptr) chosen = entry;
    }
    std::optional<Address> result;
    if (chosen != nullptr) {
        result = from_sockaddr(chosen->ai_addr);
        result->port = port;
    } else {
        error = "Could not find a server named " + host;
    }
    freeaddrinfo(found);
    return result;
}

}  // namespace network
