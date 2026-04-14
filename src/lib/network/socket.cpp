#include "network/socket.hpp"

#include "foundation/log.hpp"

#include <cstdio>
#include <format>
#include <utility>

#if ATLAS_PLATFORM_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace
{

// ============================================================================
// Winsock initialization (Windows only)
// ============================================================================

struct WinsockInit
{
    WinsockInit()
    {
#if ATLAS_PLATFORM_WINDOWS
        WSADATA wsa;
        auto ret = WSAStartup(MAKEWORD(2, 2), &wsa);
        if (ret != 0)
        {
            std::fprintf(stderr, "WSAStartup failed with error: %d\n", ret);
            std::abort();
        }
#endif
    }

    ~WinsockInit()
    {
#if ATLAS_PLATFORM_WINDOWS
        WSACleanup();
#endif
    }
};

void ensure_winsock()
{
    [[maybe_unused]] static WinsockInit init;
}

// ============================================================================
// Error mapping
// ============================================================================

auto map_socket_error() -> atlas::Error
{
#if ATLAS_PLATFORM_WINDOWS
    int err = WSAGetLastError();
    switch (err)
    {
        case WSAEWOULDBLOCK:
            return atlas::Error(atlas::ErrorCode::WouldBlock, "Operation would block");
        case WSAECONNREFUSED:
            return atlas::Error(atlas::ErrorCode::ConnectionRefused, "Connection refused");
        case WSAECONNRESET:
            return atlas::Error(atlas::ErrorCode::ConnectionReset, "Connection reset");
        case WSAEADDRINUSE:
            return atlas::Error(atlas::ErrorCode::AddressInUse, "Address in use");
        case WSAENETUNREACH:
            return atlas::Error(atlas::ErrorCode::NetworkUnreachable, "Network unreachable");
        case WSAEMSGSIZE:
            return atlas::Error(atlas::ErrorCode::MessageTooLarge, "Message too large");
        case WSAEINPROGRESS:
        case WSAEALREADY:
            return atlas::Error(atlas::ErrorCode::WouldBlock, "Operation in progress");
        default:
            return atlas::Error(atlas::ErrorCode::IoError, std::format("Socket error: {}", err));
    }
#else
    int err = errno;
    switch (err)
    {
        case EWOULDBLOCK:  // same as EAGAIN on most systems
        case EINPROGRESS:
        case EINTR:
            return atlas::Error(atlas::ErrorCode::WouldBlock, "Operation would block");
        case ECONNREFUSED:
            return atlas::Error(atlas::ErrorCode::ConnectionRefused, "Connection refused");
        case ECONNRESET:
            return atlas::Error(atlas::ErrorCode::ConnectionReset, "Connection reset");
        case EADDRINUSE:
            return atlas::Error(atlas::ErrorCode::AddressInUse, "Address in use");
        case ENETUNREACH:
            return atlas::Error(atlas::ErrorCode::NetworkUnreachable, "Network unreachable");
        case EMSGSIZE:
            return atlas::Error(atlas::ErrorCode::MessageTooLarge, "Message too large");
        default:
            return atlas::Error(atlas::ErrorCode::IoError, std::format("Socket error: {}", err));
    }
#endif
}

// ============================================================================
// Platform socket close
// ============================================================================

void close_socket(atlas::FdHandle fd)
{
#if ATLAS_PLATFORM_WINDOWS
    ::closesocket(static_cast<SOCKET>(fd));
#else
    ::close(fd);
#endif
}

}  // anonymous namespace

namespace atlas
{

// ============================================================================
// Lifetime
// ============================================================================

Socket::~Socket()
{
    if (is_valid())
    {
        close();
    }
}

Socket::Socket(Socket&& other) noexcept : fd_(std::exchange(other.fd_, kInvalidFd)) {}

Socket& Socket::operator=(Socket&& other) noexcept
{
    if (this != &other)
    {
        if (is_valid())
        {
            close();
        }
        fd_ = std::exchange(other.fd_, kInvalidFd);
    }
    return *this;
}

void Socket::close()
{
    if (is_valid())
    {
        close_socket(fd_);
        fd_ = kInvalidFd;
    }
}

// ============================================================================
// Factory
// ============================================================================

auto Socket::create_tcp() -> Result<Socket>
{
    ensure_winsock();

#if ATLAS_PLATFORM_WINDOWS
    SOCKET raw = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (raw == INVALID_SOCKET)
    {
        return map_socket_error();
    }
    auto fd = static_cast<FdHandle>(raw);
#else
    int raw = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (raw == -1)
    {
        return map_socket_error();
    }
    auto fd = static_cast<FdHandle>(raw);
#endif

    Socket sock(fd);
    if (auto r = sock.set_non_blocking(true); !r)
        return r.error();
    if (auto r = sock.set_reuse_addr(true); !r)
        return r.error();
    if (auto r = sock.set_no_delay(true); !r)
        return r.error();
    if (auto r = sock.set_send_buffer_size(256 * 1024); !r)
        return r.error();
    if (auto r = sock.set_recv_buffer_size(256 * 1024); !r)
        return r.error();
    return sock;
}

auto Socket::create_udp() -> Result<Socket>
{
    ensure_winsock();

#if ATLAS_PLATFORM_WINDOWS
    SOCKET raw = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (raw == INVALID_SOCKET)
    {
        return map_socket_error();
    }
    // Disable WSAECONNRESET on UDP sockets — Windows reports ICMP port
    // unreachable as a recv error which disrupts the receive loop.
    BOOL new_behavior = FALSE;
    DWORD bytes_returned = 0;
    ::WSAIoctl(raw, SIO_UDP_CONNRESET, &new_behavior, sizeof(new_behavior), nullptr, 0,
               &bytes_returned, nullptr, nullptr);
    auto fd = static_cast<FdHandle>(raw);
#else
    int raw = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (raw == -1)
    {
        return map_socket_error();
    }
    auto fd = static_cast<FdHandle>(raw);
#endif

    Socket sock(fd);
    if (auto r = sock.set_non_blocking(true); !r)
        return r.error();
    if (auto r = sock.set_reuse_addr(true); !r)
        return r.error();
    if (auto r = sock.set_send_buffer_size(512 * 1024); !r)
        return r.error();
    if (auto r = sock.set_recv_buffer_size(512 * 1024); !r)
        return r.error();
    return sock;
}

// ============================================================================
// Server operations
// ============================================================================

auto Socket::bind(const Address& addr) -> Result<void>
{
    auto sa = addr.to_sockaddr();
    int result = ::bind(static_cast<decltype(::socket(0, 0, 0))>(fd_),
                        reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));

    if (result != 0)
    {
        return map_socket_error();
    }
    return {};
}

auto Socket::listen(int backlog) -> Result<void>
{
    int result = ::listen(static_cast<decltype(::socket(0, 0, 0))>(fd_), backlog);

    if (result != 0)
    {
        return map_socket_error();
    }
    return {};
}

auto Socket::accept() -> Result<std::pair<Socket, Address>>
{
    sockaddr_in client_addr{};
    socklen_t len = sizeof(client_addr);

#if ATLAS_PLATFORM_WINDOWS
    SOCKET client_raw =
        ::accept(static_cast<SOCKET>(fd_), reinterpret_cast<sockaddr*>(&client_addr), &len);
    if (client_raw == INVALID_SOCKET)
    {
        return map_socket_error();
    }
    auto client_fd = static_cast<FdHandle>(client_raw);
#else
    int client_raw = ::accept4(fd_, reinterpret_cast<sockaddr*>(&client_addr), &len, SOCK_NONBLOCK);
    if (client_raw == -1)
    {
        return map_socket_error();
    }
    auto client_fd = static_cast<FdHandle>(client_raw);
#endif

    Socket client_sock(client_fd);
#if ATLAS_PLATFORM_WINDOWS
    if (auto r = client_sock.set_non_blocking(true); !r)
        return r.error();
#endif
    if (auto r = client_sock.set_no_delay(true); !r)
        return r.error();
    if (auto r = client_sock.set_send_buffer_size(256 * 1024); !r)
        return r.error();
    if (auto r = client_sock.set_recv_buffer_size(256 * 1024); !r)
        return r.error();

    Address client_address(client_addr);
    return std::pair<Socket, Address>{std::move(client_sock), client_address};
}

// ============================================================================
// Client operations
// ============================================================================

auto Socket::connect(const Address& addr) -> Result<void>
{
    auto sa = addr.to_sockaddr();
    int result = ::connect(static_cast<decltype(::socket(0, 0, 0))>(fd_),
                           reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));

    if (result != 0)
    {
#if ATLAS_PLATFORM_WINDOWS
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK)
        {
            return Error(ErrorCode::WouldBlock, "Connection in progress");
        }
#else
        int err = errno;
        if (err == EINPROGRESS)
        {
            return Error(ErrorCode::WouldBlock, "Connection in progress");
        }
#endif
        // Use already-captured err instead of re-reading errno/WSAGetLastError
        return Error(ErrorCode::IoError, std::format("connect failed: {}", err));
    }
    return {};
}

// ============================================================================
// Stream I/O (TCP)
// ============================================================================

auto Socket::send(std::span<const std::byte> data) -> Result<size_t>
{
#if ATLAS_PLATFORM_WINDOWS
    int sent = ::send(static_cast<SOCKET>(fd_), reinterpret_cast<const char*>(data.data()),
                      static_cast<int>(data.size()), 0);
    if (sent == SOCKET_ERROR)
    {
        return map_socket_error();
    }
#else
    auto sent = ::send(fd_, data.data(), data.size(), 0);
    if (sent == -1)
    {
        return map_socket_error();
    }
#endif

    return static_cast<size_t>(sent);
}

auto Socket::recv(std::span<std::byte> buffer) -> Result<size_t>
{
#if ATLAS_PLATFORM_WINDOWS
    int received = ::recv(static_cast<SOCKET>(fd_), reinterpret_cast<char*>(buffer.data()),
                          static_cast<int>(buffer.size()), 0);
    if (received == SOCKET_ERROR)
    {
        return map_socket_error();
    }
#else
    auto received = ::recv(fd_, buffer.data(), buffer.size(), 0);
    if (received == -1)
    {
        return map_socket_error();
    }
#endif

    return static_cast<size_t>(received);
}

// ============================================================================
// Scatter-gather I/O (TCP)
// ============================================================================

auto Socket::send_iov(std::span<const IoVec> iov) -> Result<size_t>
{
    if (iov.empty())
    {
        return size_t{0};
    }

    static constexpr std::size_t kMaxBufs = 16;
    size_t total_sent = 0;

    // Process in batches of kMaxBufs to avoid silent truncation
    for (std::size_t offset = 0; offset < iov.size(); offset += kMaxBufs)
    {
        auto count = std::min(iov.size() - offset, kMaxBufs);

#if ATLAS_PLATFORM_WINDOWS
        WSABUF bufs[kMaxBufs];
        for (std::size_t i = 0; i < count; ++i)
        {
            bufs[i].buf = reinterpret_cast<char*>(const_cast<std::byte*>(iov[offset + i].data));
            bufs[i].len = static_cast<ULONG>(iov[offset + i].size);
        }
        DWORD sent = 0;
        int result = ::WSASend(static_cast<SOCKET>(fd_), bufs, static_cast<DWORD>(count), &sent, 0,
                               nullptr, nullptr);
        if (result == SOCKET_ERROR)
        {
            if (total_sent > 0)
                return total_sent;
            return map_socket_error();
        }
        total_sent += sent;
#else
        struct iovec vecs[kMaxBufs];
        for (std::size_t i = 0; i < count; ++i)
        {
            vecs[i].iov_base = const_cast<std::byte*>(iov[offset + i].data);
            vecs[i].iov_len = iov[offset + i].size;
        }
        auto sent = ::writev(fd_, vecs, static_cast<int>(count));
        if (sent == -1)
        {
            if (total_sent > 0)
                return total_sent;
            return map_socket_error();
        }
        total_sent += static_cast<size_t>(sent);
#endif
    }

    return total_sent;
}

// ============================================================================
// Datagram I/O (UDP)
// ============================================================================

auto Socket::send_to(std::span<const std::byte> data, const Address& dest) -> Result<size_t>
{
    auto sa = dest.to_sockaddr();

#if ATLAS_PLATFORM_WINDOWS
    int sent = ::sendto(static_cast<SOCKET>(fd_), reinterpret_cast<const char*>(data.data()),
                        static_cast<int>(data.size()), 0, reinterpret_cast<const sockaddr*>(&sa),
                        sizeof(sa));
    if (sent == SOCKET_ERROR)
    {
        return map_socket_error();
    }
#else
    auto sent = ::sendto(fd_, data.data(), data.size(), 0, reinterpret_cast<const sockaddr*>(&sa),
                         sizeof(sa));
    if (sent == -1)
    {
        return map_socket_error();
    }
#endif

    return static_cast<size_t>(sent);
}

auto Socket::recv_from(std::span<std::byte> buffer) -> Result<std::pair<size_t, Address>>
{
    sockaddr_in src{};
    socklen_t len = sizeof(src);

#if ATLAS_PLATFORM_WINDOWS
    int received =
        ::recvfrom(static_cast<SOCKET>(fd_), reinterpret_cast<char*>(buffer.data()),
                   static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&src), &len);
    if (received == SOCKET_ERROR)
    {
        return map_socket_error();
    }
#else
    auto received =
        ::recvfrom(fd_, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr*>(&src), &len);
    if (received == -1)
    {
        return map_socket_error();
    }
#endif

    return std::pair<size_t, Address>{static_cast<size_t>(received), Address(src)};
}

// ============================================================================
// Socket options
// ============================================================================

auto Socket::set_non_blocking(bool enable) -> Result<void>
{
#if ATLAS_PLATFORM_WINDOWS
    u_long mode = enable ? 1 : 0;
    if (ioctlsocket(static_cast<SOCKET>(fd_), FIONBIO, &mode) != 0)
    {
        return Error{ErrorCode::InternalError, "ioctlsocket(FIONBIO) failed"};
    }
#else
    int flags = fcntl(fd_, F_GETFL, 0);
    if (flags < 0)
    {
        return Error{ErrorCode::InternalError, "fcntl(F_GETFL) failed"};
    }
    int new_flags = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (fcntl(fd_, F_SETFL, new_flags) < 0)
    {
        return Error{ErrorCode::InternalError, "fcntl(F_SETFL) failed"};
    }
#endif
    return Result<void>{};
}

auto Socket::set_reuse_addr(bool enable) -> Result<void>
{
    int optval = enable ? 1 : 0;
    if (::setsockopt(static_cast<decltype(::socket(0, 0, 0))>(fd_), SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&optval), sizeof(optval)) != 0)
    {
        return Error{ErrorCode::InternalError, "setsockopt(SO_REUSEADDR) failed"};
    }
    return Result<void>{};
}

auto Socket::set_no_delay(bool enable) -> Result<void>
{
    int optval = enable ? 1 : 0;
    if (::setsockopt(static_cast<decltype(::socket(0, 0, 0))>(fd_), IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&optval), sizeof(optval)) != 0)
    {
        return Error{ErrorCode::InternalError, "setsockopt(TCP_NODELAY) failed"};
    }
    return Result<void>{};
}

auto Socket::set_send_buffer_size(int size) -> Result<void>
{
    if (::setsockopt(static_cast<decltype(::socket(0, 0, 0))>(fd_), SOL_SOCKET, SO_SNDBUF,
                     reinterpret_cast<const char*>(&size), sizeof(size)) != 0)
    {
        return Error{ErrorCode::InternalError, "setsockopt(SO_SNDBUF) failed"};
    }
    return Result<void>{};
}

auto Socket::set_recv_buffer_size(int size) -> Result<void>
{
    if (::setsockopt(static_cast<decltype(::socket(0, 0, 0))>(fd_), SOL_SOCKET, SO_RCVBUF,
                     reinterpret_cast<const char*>(&size), sizeof(size)) != 0)
    {
        return Error{ErrorCode::InternalError, "setsockopt(SO_RCVBUF) failed"};
    }
    return Result<void>{};
}

// ============================================================================
// Accessors
// ============================================================================

auto Socket::local_address() const -> Result<Address>
{
    sockaddr_in sa{};
    socklen_t len = sizeof(sa);

    int result = ::getsockname(static_cast<decltype(::socket(0, 0, 0))>(fd_),
                               reinterpret_cast<sockaddr*>(&sa), &len);

    if (result != 0)
    {
        return map_socket_error();
    }
    return Address(sa);
}

}  // namespace atlas
