#include "network/socket.hpp"
#include "foundation/log.hpp"

#include <format>

#if ATLAS_PLATFORM_WINDOWS
#   include <winsock2.h>
#   include <ws2tcpip.h>
#   pragma comment(lib, "ws2_32.lib")
#else
#   include <arpa/inet.h>
#   include <fcntl.h>
#   include <netinet/in.h>
#   include <netinet/tcp.h>
#   include <sys/socket.h>
#   include <unistd.h>
#   include <cerrno>
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
        WSAStartup(MAKEWORD(2, 2), &wsa);
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
            return atlas::Error(atlas::ErrorCode::IoError,
                std::format("Socket error: {}", err));
    }
#else
    int err = errno;
    switch (err)
    {
        case EWOULDBLOCK: // same as EAGAIN on most systems
        case EINPROGRESS:
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
            return atlas::Error(atlas::ErrorCode::IoError,
                std::format("Socket error: {}", err));
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

} // anonymous namespace

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

Socket::Socket(Socket&& other) noexcept
    : fd_(other.fd_)
{
    other.fd_ = kInvalidFd;
}

Socket& Socket::operator=(Socket&& other) noexcept
{
    if (this != &other)
    {
        if (is_valid())
        {
            close();
        }
        fd_ = other.fd_;
        other.fd_ = kInvalidFd;
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
    sock.set_non_blocking(true);
    sock.set_reuse_addr(true);
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
    sock.set_non_blocking(true);
    sock.set_reuse_addr(true);
    return sock;
}

// ============================================================================
// Server operations
// ============================================================================

auto Socket::bind(const Address& addr) -> Result<void>
{
    auto sa = addr.to_sockaddr();
    int result = ::bind(
        static_cast<decltype(::socket(0, 0, 0))>(fd_),
        reinterpret_cast<const sockaddr*>(&sa),
        sizeof(sa));

    if (result != 0)
    {
        return map_socket_error();
    }
    return {};
}

auto Socket::listen(int backlog) -> Result<void>
{
    int result = ::listen(
        static_cast<decltype(::socket(0, 0, 0))>(fd_),
        backlog);

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
    SOCKET client_raw = ::accept(
        static_cast<SOCKET>(fd_),
        reinterpret_cast<sockaddr*>(&client_addr),
        &len);
    if (client_raw == INVALID_SOCKET)
    {
        return map_socket_error();
    }
    auto client_fd = static_cast<FdHandle>(client_raw);
#else
    int client_raw = ::accept(
        fd_,
        reinterpret_cast<sockaddr*>(&client_addr),
        &len);
    if (client_raw == -1)
    {
        return map_socket_error();
    }
    auto client_fd = static_cast<FdHandle>(client_raw);
#endif

    Socket client_sock(client_fd);
    client_sock.set_non_blocking(true);

    Address client_address(client_addr);
    return std::pair<Socket, Address>{std::move(client_sock), client_address};
}

// ============================================================================
// Client operations
// ============================================================================

auto Socket::connect(const Address& addr) -> Result<void>
{
    auto sa = addr.to_sockaddr();
    int result = ::connect(
        static_cast<decltype(::socket(0, 0, 0))>(fd_),
        reinterpret_cast<const sockaddr*>(&sa),
        sizeof(sa));

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
        return map_socket_error();
    }
    return {};
}

// ============================================================================
// Stream I/O (TCP)
// ============================================================================

auto Socket::send(std::span<const std::byte> data) -> Result<size_t>
{
#if ATLAS_PLATFORM_WINDOWS
    int sent = ::send(
        static_cast<SOCKET>(fd_),
        reinterpret_cast<const char*>(data.data()),
        static_cast<int>(data.size()),
        0);
    if (sent == SOCKET_ERROR)
    {
        return map_socket_error();
    }
#else
    auto sent = ::send(
        fd_,
        data.data(),
        data.size(),
        0);
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
    int received = ::recv(
        static_cast<SOCKET>(fd_),
        reinterpret_cast<char*>(buffer.data()),
        static_cast<int>(buffer.size()),
        0);
    if (received == SOCKET_ERROR)
    {
        return map_socket_error();
    }
#else
    auto received = ::recv(
        fd_,
        buffer.data(),
        buffer.size(),
        0);
    if (received == -1)
    {
        return map_socket_error();
    }
#endif

    return static_cast<size_t>(received);
}

// ============================================================================
// Datagram I/O (UDP)
// ============================================================================

auto Socket::send_to(std::span<const std::byte> data, const Address& dest) -> Result<size_t>
{
    auto sa = dest.to_sockaddr();

#if ATLAS_PLATFORM_WINDOWS
    int sent = ::sendto(
        static_cast<SOCKET>(fd_),
        reinterpret_cast<const char*>(data.data()),
        static_cast<int>(data.size()),
        0,
        reinterpret_cast<const sockaddr*>(&sa),
        sizeof(sa));
    if (sent == SOCKET_ERROR)
    {
        return map_socket_error();
    }
#else
    auto sent = ::sendto(
        fd_,
        data.data(),
        data.size(),
        0,
        reinterpret_cast<const sockaddr*>(&sa),
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
    int received = ::recvfrom(
        static_cast<SOCKET>(fd_),
        reinterpret_cast<char*>(buffer.data()),
        static_cast<int>(buffer.size()),
        0,
        reinterpret_cast<sockaddr*>(&src),
        &len);
    if (received == SOCKET_ERROR)
    {
        return map_socket_error();
    }
#else
    auto received = ::recvfrom(
        fd_,
        buffer.data(),
        buffer.size(),
        0,
        reinterpret_cast<sockaddr*>(&src),
        &len);
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

void Socket::set_non_blocking(bool enable)
{
#if ATLAS_PLATFORM_WINDOWS
    u_long mode = enable ? 1 : 0;
    ioctlsocket(static_cast<SOCKET>(fd_), FIONBIO, &mode);
#else
    int flags = fcntl(fd_, F_GETFL, 0);
    if (enable)
    {
        fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    }
    else
    {
        fcntl(fd_, F_SETFL, flags & ~O_NONBLOCK);
    }
#endif
}

void Socket::set_reuse_addr(bool enable)
{
    int optval = enable ? 1 : 0;
    ::setsockopt(
        static_cast<decltype(::socket(0, 0, 0))>(fd_),
        SOL_SOCKET,
        SO_REUSEADDR,
        reinterpret_cast<const char*>(&optval),
        sizeof(optval));
}

void Socket::set_no_delay(bool enable)
{
    int optval = enable ? 1 : 0;
    ::setsockopt(
        static_cast<decltype(::socket(0, 0, 0))>(fd_),
        IPPROTO_TCP,
        TCP_NODELAY,
        reinterpret_cast<const char*>(&optval),
        sizeof(optval));
}

void Socket::set_send_buffer_size(int size)
{
    ::setsockopt(
        static_cast<decltype(::socket(0, 0, 0))>(fd_),
        SOL_SOCKET,
        SO_SNDBUF,
        reinterpret_cast<const char*>(&size),
        sizeof(size));
}

void Socket::set_recv_buffer_size(int size)
{
    ::setsockopt(
        static_cast<decltype(::socket(0, 0, 0))>(fd_),
        SOL_SOCKET,
        SO_RCVBUF,
        reinterpret_cast<const char*>(&size),
        sizeof(size));
}

// ============================================================================
// Accessors
// ============================================================================

auto Socket::local_address() const -> Result<Address>
{
    sockaddr_in sa{};
    socklen_t len = sizeof(sa);

    int result = ::getsockname(
        static_cast<decltype(::socket(0, 0, 0))>(fd_),
        reinterpret_cast<sockaddr*>(&sa),
        &len);

    if (result != 0)
    {
        return map_socket_error();
    }
    return Address(sa);
}

} // namespace atlas
