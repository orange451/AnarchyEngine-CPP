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

void to_sockaddr(const Address& address, sockaddr_in6& out) {
    std::memset(&out, 0, sizeof(out));
    out.sin6_family = AF_INET6;
    out.sin6_port = htons(address.port);
    if (address.family == Address::Family::IPv6) {
        std::memcpy(&out.sin6_addr, address.bytes.data(), 16);
    } else {
        const std::uint8_t mapped[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF,
                                         address.bytes[0], address.bytes[1], address.bytes[2], address.bytes[3]};
        std::memcpy(&out.sin6_addr, mapped, 16);
    }
}

Address from_sockaddr(const sockaddr_in6& in) {
    const std::uint8_t* raw = reinterpret_cast<const std::uint8_t*>(&in.sin6_addr);
    bool mapped = raw[10] == 0xFF && raw[11] == 0xFF;
    for (int i = 0; i < 10; ++i) mapped = mapped && raw[i] == 0;
    if (mapped) return Address::ipv4(raw[12], raw[13], raw[14], raw[15], ntohs(in.sin6_port));
    Address address;
    address.family = Address::Family::IPv6;
    std::memcpy(address.bytes.data(), raw, 16);
    address.port = ntohs(in.sin6_port);
    return address;
}

}  // namespace

std::unique_ptr<UdpSocket> UdpSocket::open(std::uint16_t port, std::string& error) {
    if (!start_sockets()) {
        error = "Winsock did not start";
        return nullptr;
    }
    const socket_t handle = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == kInvalid) {
        error = error_text(last_error());
        return nullptr;
    }
    int off = 0;
    setsockopt(handle, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off));
    sockaddr_in6 bind_address;
    std::memset(&bind_address, 0, sizeof(bind_address));
    bind_address.sin6_family = AF_INET6;
    bind_address.sin6_addr = in6addr_any;
    bind_address.sin6_port = htons(port);
    if (::bind(handle, reinterpret_cast<const sockaddr*>(&bind_address), sizeof(bind_address)) != 0) {
        error = "cannot bind port " + std::to_string(port) + ": " + error_text(last_error());
        close_socket(handle);
        return nullptr;
    }
#ifdef _WIN32
    u_long non_blocking = 1;
    if (ioctlsocket(handle, FIONBIO, &non_blocking) != 0) {
        error = "cannot make the socket non-blocking: " + error_text(last_error());
        close_socket(handle);
        return nullptr;
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
        return nullptr;
    }
#endif
    sockaddr_in6 bound;
    socklen_t length = sizeof(bound);
    if (getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
        error = "cannot read the socket's port: " + error_text(last_error());
        close_socket(handle);
        return nullptr;
    }
    std::unique_ptr<UdpSocket> socket(new UdpSocket());
    socket->handle_ = static_cast<std::uintptr_t>(handle);
    socket->port_ = ntohs(bound.sin6_port);
    return socket;
}

UdpSocket::~UdpSocket() {
    if (handle_ != 0) close_socket(static_cast<socket_t>(handle_));
}

bool UdpSocket::send(const Address& to, const std::uint8_t* bytes, std::size_t length) {
    if (failed_ || to.family == Address::Family::None) return false;
    sockaddr_in6 address;
    to_sockaddr(to, address);
    const auto sent = ::sendto(static_cast<socket_t>(handle_), reinterpret_cast<const char*>(bytes), static_cast<int>(length), 0,
                               reinterpret_cast<const sockaddr*>(&address), sizeof(address));
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
        sockaddr_in6 address;
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
        if (static_cast<std::size_t>(got) > capacity) continue;   // MSG_TRUNC: oversized, dropped
        from = from_sockaddr(address);
        return static_cast<std::size_t>(got);
    }
}

}  // namespace network
