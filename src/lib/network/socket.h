#ifndef ATLAS_LIB_NETWORK_SOCKET_H_
#define ATLAS_LIB_NETWORK_SOCKET_H_

#include <cstddef>
#include <span>
#include <utility>

#include "foundation/error.h"
#include "network/address.h"
#include "platform/io_poller.h"

namespace atlas {

class Socket {
 public:
  ~Socket();

  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;

  [[nodiscard]] static auto CreateTcp() -> Result<Socket>;
  [[nodiscard]] static auto CreateUdp() -> Result<Socket>;

  [[nodiscard]] auto Bind(const Address& addr) -> Result<void>;
  [[nodiscard]] auto Listen(int backlog = 128) -> Result<void>;
  [[nodiscard]] auto Accept() -> Result<std::pair<Socket, Address>>;

  [[nodiscard]] auto Connect(const Address& addr) -> Result<void>;

  [[nodiscard]] auto Send(std::span<const std::byte> data) -> Result<size_t>;
  [[nodiscard]] auto Recv(std::span<std::byte> buffer) -> Result<size_t>;

  struct IoVec {
    const std::byte* data;
    std::size_t size;
  };
  [[nodiscard]] auto SendIov(std::span<const IoVec> iov) -> Result<size_t>;

  [[nodiscard]] auto SendTo(std::span<const std::byte> data, const Address& dest) -> Result<size_t>;
  [[nodiscard]] auto RecvFrom(std::span<std::byte> buffer) -> Result<std::pair<size_t, Address>>;

  [[nodiscard]] auto SetNonBlocking(bool enable) -> Result<void>;
  [[nodiscard]] auto SetReuseAddr(bool enable) -> Result<void>;
  [[nodiscard]] auto SetNoDelay(bool enable) -> Result<void>;
  [[nodiscard]] auto SetSendBufferSize(int size) -> Result<void>;
  [[nodiscard]] auto SetRecvBufferSize(int size) -> Result<void>;

  [[nodiscard]] auto Fd() const noexcept -> FdHandle { return fd_; }
  [[nodiscard]] auto LocalAddress() const -> Result<Address>;
  [[nodiscard]] auto IsValid() const noexcept -> bool { return fd_ != kInvalidFd; }

  void Close();

 private:
  explicit Socket(FdHandle fd) : fd_(fd) {}
  FdHandle fd_{kInvalidFd};
};

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_SOCKET_H_
