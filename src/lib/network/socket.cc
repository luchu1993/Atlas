#include "network/socket.h"

#include <cstdio>
#include <format>
#include <utility>

#include "foundation/log.h"

#if ATLAS_PLATFORM_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace {

struct WinsockInit {
  WinsockInit() {
#if ATLAS_PLATFORM_WINDOWS
    WSADATA wsa;
    auto ret = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (ret != 0) {
      std::fprintf(stderr, "WSAStartup failed with error: %d\n", ret);
      std::abort();
    }
#endif
  }

  ~WinsockInit() {
#if ATLAS_PLATFORM_WINDOWS
    WSACleanup();
#endif
  }
};

void EnsureWinsock() {
  [[maybe_unused]] static WinsockInit init;
}

auto MapSocketError() -> atlas::Error {
#if ATLAS_PLATFORM_WINDOWS
  int err = WSAGetLastError();
  switch (err) {
    case WSAEWOULDBLOCK:
      return atlas::Error(atlas::ErrorCode::kWouldBlock, "Operation would block");
    case WSAECONNREFUSED:
      return atlas::Error(atlas::ErrorCode::kConnectionRefused, "Connection refused");
    case WSAECONNRESET:
      return atlas::Error(atlas::ErrorCode::kConnectionReset, "Connection reset");
    case WSAEADDRINUSE:
      return atlas::Error(atlas::ErrorCode::kAddressInUse, "Address in use");
    case WSAENETUNREACH:
      return atlas::Error(atlas::ErrorCode::kNetworkUnreachable, "Network unreachable");
    case WSAEMSGSIZE:
      return atlas::Error(atlas::ErrorCode::kMessageTooLarge, "Message too large");
    case WSAEINPROGRESS:
    case WSAEALREADY:
      return atlas::Error(atlas::ErrorCode::kWouldBlock, "Operation in progress");
    default:
      return atlas::Error(atlas::ErrorCode::kIoError, std::format("Socket error: {}", err));
  }
#else
  int err = errno;
  switch (err) {
    case EWOULDBLOCK:
    case EINPROGRESS:
    case EINTR:
      return atlas::Error(atlas::ErrorCode::kWouldBlock, "Operation would block");
    case ECONNREFUSED:
      return atlas::Error(atlas::ErrorCode::kConnectionRefused, "Connection refused");
    case ECONNRESET:
      return atlas::Error(atlas::ErrorCode::kConnectionReset, "Connection reset");
    case EADDRINUSE:
      return atlas::Error(atlas::ErrorCode::kAddressInUse, "Address in use");
    case ENETUNREACH:
      return atlas::Error(atlas::ErrorCode::kNetworkUnreachable, "Network unreachable");
    case EMSGSIZE:
      return atlas::Error(atlas::ErrorCode::kMessageTooLarge, "Message too large");
    default:
      return atlas::Error(atlas::ErrorCode::kIoError, std::format("Socket error: {}", err));
  }
#endif
}

void CloseSocket(atlas::FdHandle fd) {
#if ATLAS_PLATFORM_WINDOWS
  ::closesocket(static_cast<SOCKET>(fd));
#else
  ::close(fd);
#endif
}

}  // anonymous namespace

namespace atlas {

Socket::~Socket() {
  if (IsValid()) {
    Close();
  }
}

Socket::Socket(Socket&& other) noexcept : fd_(std::exchange(other.fd_, kInvalidFd)) {}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    if (IsValid()) {
      Close();
    }
    fd_ = std::exchange(other.fd_, kInvalidFd);
  }
  return *this;
}

void Socket::Close() {
  if (IsValid()) {
    CloseSocket(fd_);
    fd_ = kInvalidFd;
  }
}

auto Socket::CreateTcp() -> Result<Socket> {
  EnsureWinsock();

#if ATLAS_PLATFORM_WINDOWS
  SOCKET raw = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (raw == INVALID_SOCKET) {
    return MapSocketError();
  }
  auto fd = static_cast<FdHandle>(raw);
#else
  int raw = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (raw == -1) {
    return MapSocketError();
  }
  auto fd = static_cast<FdHandle>(raw);
#endif

  Socket sock(fd);
  if (auto r = sock.SetNonBlocking(true); !r) return r.Error();
  if (auto r = sock.SetReuseAddr(true); !r) return r.Error();
  if (auto r = sock.SetNoDelay(true); !r) return r.Error();
  if (auto r = sock.SetSendBufferSize(256 * 1024); !r) return r.Error();
  if (auto r = sock.SetRecvBufferSize(256 * 1024); !r) return r.Error();
  return sock;
}

auto Socket::CreateUdp() -> Result<Socket> {
  EnsureWinsock();

#if ATLAS_PLATFORM_WINDOWS
  SOCKET raw = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (raw == INVALID_SOCKET) {
    return MapSocketError();
  }
  // Disable WSAECONNRESET; Windows reports ICMP port
  // unreachable as a recv error which disrupts the receive loop.
  BOOL new_behavior = FALSE;
  DWORD bytes_returned = 0;
  ::WSAIoctl(raw, SIO_UDP_CONNRESET, &new_behavior, sizeof(new_behavior), nullptr, 0,
             &bytes_returned, nullptr, nullptr);
  auto fd = static_cast<FdHandle>(raw);
#else
  int raw = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (raw == -1) {
    return MapSocketError();
  }
  auto fd = static_cast<FdHandle>(raw);
#endif

  Socket sock(fd);
  if (auto r = sock.SetNonBlocking(true); !r) return r.Error();
  if (auto r = sock.SetReuseAddr(true); !r) return r.Error();
  if (auto r = sock.SetSendBufferSize(512 * 1024); !r) return r.Error();
  if (auto r = sock.SetRecvBufferSize(512 * 1024); !r) return r.Error();
  return sock;
}

auto Socket::Bind(const Address& addr) -> Result<void> {
  auto sa = addr.ToSockaddr();
  int result = ::bind(static_cast<decltype(::socket(0, 0, 0))>(fd_),
                      reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));

  if (result != 0) {
    return MapSocketError();
  }
  return {};
}

auto Socket::Listen(int backlog) -> Result<void> {
  int result = ::listen(static_cast<decltype(::socket(0, 0, 0))>(fd_), backlog);

  if (result != 0) {
    return MapSocketError();
  }
  return {};
}

auto Socket::Accept() -> Result<std::pair<Socket, Address>> {
  sockaddr_in client_addr{};
  socklen_t len = sizeof(client_addr);

#if ATLAS_PLATFORM_WINDOWS
  SOCKET client_raw =
      ::accept(static_cast<SOCKET>(fd_), reinterpret_cast<sockaddr*>(&client_addr), &len);
  if (client_raw == INVALID_SOCKET) {
    return MapSocketError();
  }
  auto client_fd = static_cast<FdHandle>(client_raw);
#else
  int client_raw = ::accept4(fd_, reinterpret_cast<sockaddr*>(&client_addr), &len, SOCK_NONBLOCK);
  if (client_raw == -1) {
    return MapSocketError();
  }
  auto client_fd = static_cast<FdHandle>(client_raw);
#endif

  Socket client_sock(client_fd);
#if ATLAS_PLATFORM_WINDOWS
  if (auto r = client_sock.SetNonBlocking(true); !r) return r.Error();
#endif
  if (auto r = client_sock.SetNoDelay(true); !r) return r.Error();
  if (auto r = client_sock.SetSendBufferSize(256 * 1024); !r) return r.Error();
  if (auto r = client_sock.SetRecvBufferSize(256 * 1024); !r) return r.Error();

  Address client_address(client_addr);
  return std::pair<Socket, Address>{std::move(client_sock), client_address};
}

auto Socket::Connect(const Address& addr) -> Result<void> {
  auto sa = addr.ToSockaddr();
  int result = ::connect(static_cast<decltype(::socket(0, 0, 0))>(fd_),
                         reinterpret_cast<const sockaddr*>(&sa), sizeof(sa));

  if (result != 0) {
#if ATLAS_PLATFORM_WINDOWS
    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK) {
      return Error(ErrorCode::kWouldBlock, "Connection in progress");
    }
#else
    int err = errno;
    if (err == EINPROGRESS) {
      return Error(ErrorCode::kWouldBlock, "Connection in progress");
    }
#endif
    return Error(ErrorCode::kIoError, std::format("connect failed: {}", err));
  }
  return {};
}

auto Socket::Send(std::span<const std::byte> data) -> Result<size_t> {
#if ATLAS_PLATFORM_WINDOWS
  int sent = ::send(static_cast<SOCKET>(fd_), reinterpret_cast<const char*>(data.data()),
                    static_cast<int>(data.size()), 0);
  if (sent == SOCKET_ERROR) {
    return MapSocketError();
  }
#else
  auto sent = ::send(fd_, data.data(), data.size(), 0);
  if (sent == -1) {
    return MapSocketError();
  }
#endif

  return static_cast<size_t>(sent);
}

auto Socket::Recv(std::span<std::byte> buffer) -> Result<size_t> {
#if ATLAS_PLATFORM_WINDOWS
  int received = ::recv(static_cast<SOCKET>(fd_), reinterpret_cast<char*>(buffer.data()),
                        static_cast<int>(buffer.size()), 0);
  if (received == SOCKET_ERROR) {
    return MapSocketError();
  }
#else
  auto received = ::recv(fd_, buffer.data(), buffer.size(), 0);
  if (received == -1) {
    return MapSocketError();
  }
#endif

  return static_cast<size_t>(received);
}

auto Socket::SendIov(std::span<const IoVec> iov) -> Result<size_t> {
  if (iov.empty()) {
    return size_t{0};
  }

  static constexpr std::size_t kMaxBufs = 16;
  size_t total_sent = 0;

  for (std::size_t offset = 0; offset < iov.size(); offset += kMaxBufs) {
    auto count = (std::min)(iov.size() - offset, kMaxBufs);

#if ATLAS_PLATFORM_WINDOWS
    WSABUF bufs[kMaxBufs];
    for (std::size_t i = 0; i < count; ++i) {
      bufs[i].buf = reinterpret_cast<char*>(const_cast<std::byte*>(iov[offset + i].data));
      bufs[i].len = static_cast<ULONG>(iov[offset + i].size);
    }
    DWORD sent = 0;
    int result = ::WSASend(static_cast<SOCKET>(fd_), bufs, static_cast<DWORD>(count), &sent, 0,
                           nullptr, nullptr);
    if (result == SOCKET_ERROR) {
      if (total_sent > 0) return total_sent;
      return MapSocketError();
    }
    total_sent += sent;
#else
    struct iovec vecs[kMaxBufs];
    for (std::size_t i = 0; i < count; ++i) {
      vecs[i].iov_base = const_cast<std::byte*>(iov[offset + i].data);
      vecs[i].iov_len = iov[offset + i].size;
    }
    auto sent = ::writev(fd_, vecs, static_cast<int>(count));
    if (sent == -1) {
      if (total_sent > 0) return total_sent;
      return MapSocketError();
    }
    total_sent += static_cast<size_t>(sent);
#endif
  }

  return total_sent;
}

auto Socket::SendTo(std::span<const std::byte> data, const Address& dest) -> Result<size_t> {
  auto sa = dest.ToSockaddr();

#if ATLAS_PLATFORM_WINDOWS
  int sent = ::sendto(static_cast<SOCKET>(fd_), reinterpret_cast<const char*>(data.data()),
                      static_cast<int>(data.size()), 0, reinterpret_cast<const sockaddr*>(&sa),
                      sizeof(sa));
  if (sent == SOCKET_ERROR) {
    return MapSocketError();
  }
#else
  auto sent = ::sendto(fd_, data.data(), data.size(), 0, reinterpret_cast<const sockaddr*>(&sa),
                       sizeof(sa));
  if (sent == -1) {
    return MapSocketError();
  }
#endif

  return static_cast<size_t>(sent);
}

auto Socket::RecvFrom(std::span<std::byte> buffer) -> Result<std::pair<size_t, Address>> {
  sockaddr_in src{};
  socklen_t len = sizeof(src);

#if ATLAS_PLATFORM_WINDOWS
  int received =
      ::recvfrom(static_cast<SOCKET>(fd_), reinterpret_cast<char*>(buffer.data()),
                 static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&src), &len);
  if (received == SOCKET_ERROR) {
    return MapSocketError();
  }
#else
  auto received =
      ::recvfrom(fd_, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr*>(&src), &len);
  if (received == -1) {
    return MapSocketError();
  }
#endif

  return std::pair<size_t, Address>{static_cast<size_t>(received), Address(src)};
}

auto Socket::SetNonBlocking(bool enable) -> Result<void> {
#if ATLAS_PLATFORM_WINDOWS
  u_long mode = enable ? 1 : 0;
  if (ioctlsocket(static_cast<SOCKET>(fd_), static_cast<long>(FIONBIO), &mode) != 0) {
    return Error{ErrorCode::kInternalError, "ioctlsocket(FIONBIO) failed"};
  }
#else
  int flags = fcntl(fd_, F_GETFL, 0);
  if (flags < 0) {
    return Error{ErrorCode::kInternalError, "fcntl(F_GETFL) failed"};
  }
  int new_flags = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  if (fcntl(fd_, F_SETFL, new_flags) < 0) {
    return Error{ErrorCode::kInternalError, "fcntl(F_SETFL) failed"};
  }
#endif
  return Result<void>{};
}

auto Socket::SetReuseAddr(bool enable) -> Result<void> {
  int optval = enable ? 1 : 0;
  if (::setsockopt(static_cast<decltype(::socket(0, 0, 0))>(fd_), SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&optval), sizeof(optval)) != 0) {
    return Error{ErrorCode::kInternalError, "setsockopt(SO_REUSEADDR) failed"};
  }
  return Result<void>{};
}

auto Socket::SetNoDelay(bool enable) -> Result<void> {
  int optval = enable ? 1 : 0;
  if (::setsockopt(static_cast<decltype(::socket(0, 0, 0))>(fd_), IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&optval), sizeof(optval)) != 0) {
    return Error{ErrorCode::kInternalError, "setsockopt(TCP_NODELAY) failed"};
  }
  return Result<void>{};
}

auto Socket::SetSendBufferSize(int size) -> Result<void> {
  if (::setsockopt(static_cast<decltype(::socket(0, 0, 0))>(fd_), SOL_SOCKET, SO_SNDBUF,
                   reinterpret_cast<const char*>(&size), sizeof(size)) != 0) {
    return Error{ErrorCode::kInternalError, "setsockopt(SO_SNDBUF) failed"};
  }
  return Result<void>{};
}

auto Socket::SetRecvBufferSize(int size) -> Result<void> {
  if (::setsockopt(static_cast<decltype(::socket(0, 0, 0))>(fd_), SOL_SOCKET, SO_RCVBUF,
                   reinterpret_cast<const char*>(&size), sizeof(size)) != 0) {
    return Error{ErrorCode::kInternalError, "setsockopt(SO_RCVBUF) failed"};
  }
  return Result<void>{};
}

auto Socket::LocalAddress() const -> Result<Address> {
  sockaddr_in sa{};
  socklen_t len = sizeof(sa);

  int result = ::getsockname(static_cast<decltype(::socket(0, 0, 0))>(fd_),
                             reinterpret_cast<sockaddr*>(&sa), &len);

  if (result != 0) {
    return MapSocketError();
  }
  return Address(sa);
}

}  // namespace atlas
