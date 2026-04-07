#pragma once

#include "foundation/error.hpp"
#include "network/address.hpp"
#include "platform/io_poller.hpp"

#include <cstddef>
#include <span>
#include <utility>

namespace atlas
{

class Socket
{
public:
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    // Factory
    [[nodiscard]] static auto create_tcp() -> Result<Socket>;
    [[nodiscard]] static auto create_udp() -> Result<Socket>;

    // Server
    [[nodiscard]] auto bind(const Address& addr) -> Result<void>;
    [[nodiscard]] auto listen(int backlog = 128) -> Result<void>;
    [[nodiscard]] auto accept() -> Result<std::pair<Socket, Address>>;

    // Client
    [[nodiscard]] auto connect(const Address& addr) -> Result<void>;

    // Stream I/O (TCP)
    [[nodiscard]] auto send(std::span<const std::byte> data) -> Result<size_t>;
    [[nodiscard]] auto recv(std::span<std::byte> buffer) -> Result<size_t>;

    // Datagram I/O (UDP)
    [[nodiscard]] auto send_to(std::span<const std::byte> data, const Address& dest)
        -> Result<size_t>;
    [[nodiscard]] auto recv_from(std::span<std::byte> buffer) -> Result<std::pair<size_t, Address>>;

    // Options — return Result<void> so callers can detect and handle failures.
    [[nodiscard]] auto set_non_blocking(bool enable) -> Result<void>;
    [[nodiscard]] auto set_reuse_addr(bool enable) -> Result<void>;
    [[nodiscard]] auto set_no_delay(bool enable) -> Result<void>;
    [[nodiscard]] auto set_send_buffer_size(int size) -> Result<void>;
    [[nodiscard]] auto set_recv_buffer_size(int size) -> Result<void>;

    // Accessors
    [[nodiscard]] auto fd() const noexcept -> FdHandle { return fd_; }
    [[nodiscard]] auto local_address() const -> Result<Address>;
    [[nodiscard]] auto is_valid() const noexcept -> bool { return fd_ != kInvalidFd; }

    void close();

private:
    explicit Socket(FdHandle fd) : fd_(fd) {}
    FdHandle fd_{kInvalidFd};
};

}  // namespace atlas
