#pragma once

#include "network/address.hpp"
#include "network/interface_table.hpp"
#include "network/frequent_task.hpp"
#include "network/socket.hpp"
#include "foundation/time.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace atlas
{

class EventDispatcher;
class Channel;
class TcpChannel;
class UdpChannel;

class NetworkInterface : public FrequentTask
{
public:
    explicit NetworkInterface(EventDispatcher& dispatcher);
    ~NetworkInterface() override;

    // Non-copyable
    NetworkInterface(const NetworkInterface&) = delete;
    NetworkInterface& operator=(const NetworkInterface&) = delete;

    // TCP server
    [[nodiscard]] auto start_tcp_server(const Address& addr) -> Result<void>;

    // TCP client (async connect)
    [[nodiscard]] auto connect_tcp(const Address& addr) -> Result<TcpChannel*>;

    // UDP endpoint
    [[nodiscard]] auto start_udp(const Address& addr) -> Result<void>;

    // Channel access
    [[nodiscard]] auto find_channel(const Address& addr) -> Channel*;
    [[nodiscard]] auto channel_count() const -> size_t;

    // Message handling
    [[nodiscard]] auto interface_table() -> InterfaceTable& { return interface_table_; }

    // Addresses
    [[nodiscard]] auto tcp_address() const -> Address;
    [[nodiscard]] auto udp_address() const -> Address;

    // Rate limiting (packets per second per IP, 0 = disabled)
    void set_rate_limit(uint32_t max_per_second);

    // Shutdown
    void prepare_for_shutdown();

private:
    // FrequentTask -- runs every tick for cleanup
    void do_task() override;

    // IO callbacks
    void on_tcp_accept();
    void on_udp_readable();

    // Channel lifecycle
    void on_channel_disconnect(Channel& channel);
    void condemn_channel(const Address& addr);
    void process_condemned_channels();

    // Rate limiting
    auto check_rate_limit(uint32_t ip) -> bool;

    EventDispatcher& dispatcher_;
    InterfaceTable interface_table_;

    // TCP server
    std::optional<Socket> tcp_listen_socket_;
    Address tcp_address_;

    // UDP
    std::optional<Socket> udp_socket_;
    Address udp_address_;

    // Active channels keyed by remote address
    std::unordered_map<Address, std::unique_ptr<Channel>> channels_;

    // Condemned channels awaiting cleanup
    struct CondemnedEntry
    {
        std::unique_ptr<Channel> channel;
        TimePoint condemned_at;
    };
    std::vector<CondemnedEntry> condemned_;
    static constexpr Duration kCondemnTimeout = std::chrono::seconds(60);

    // Rate limiting
    struct RateTracker
    {
        uint32_t count{0};
        TimePoint window_start;
    };
    std::unordered_map<uint32_t, RateTracker> rate_trackers_;  // keyed by IP (net order)
    uint32_t rate_limit_{0};  // 0 = disabled

    bool shutting_down_{false};
};

} // namespace atlas
