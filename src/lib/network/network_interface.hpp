#pragma once

#include "foundation/memory/stream_buffer_pool.hpp"
#include "foundation/time.hpp"
#include "network/address.hpp"
#include "network/channel.hpp"
#include "network/event_dispatcher.hpp"
#include "network/frequent_task.hpp"
#include "network/interface_table.hpp"
#include "network/socket.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace atlas
{

class Channel;
class TcpChannel;
class UdpChannel;
class ReliableUdpChannel;

// Thread safety: NOT thread-safe. All calls must originate from EventDispatcher's thread.
class NetworkInterface : public FrequentTask
{
public:
    struct RudpProfile
    {
        bool nocwnd{false};
        uint32_t send_window{256};
        uint32_t recv_window{256};
    };

    explicit NetworkInterface(EventDispatcher& dispatcher);
    ~NetworkInterface() override;

    // Non-copyable
    NetworkInterface(const NetworkInterface&) = delete;
    NetworkInterface& operator=(const NetworkInterface&) = delete;

    // TCP server
    [[nodiscard]] auto start_tcp_server(const Address& addr) -> Result<void>;

    // TCP client (async connect) — always sets TCP_NODELAY for low-latency internal links
    [[nodiscard]] auto connect_tcp(const Address& addr) -> Result<TcpChannel*>;

    // UDP endpoint (plain unreliable, used by machined etc.)
    [[nodiscard]] auto start_udp(const Address& addr) -> Result<void>;

    // UDP client — opens a shared UDP socket (if not already open) and creates a
    // UdpChannel to the given remote address for fire-and-forget sends.
    [[nodiscard]] auto connect_udp(const Address& addr) -> Result<UdpChannel*>;

    // RUDP server — listens on a shared UDP socket; incoming datagrams from new peers
    // automatically create ReliableUdpChannel instances (used for external client connections).
    [[nodiscard]] auto start_rudp_server(const Address& addr,
                                         const RudpProfile& accept_profile = RudpProfile{})
        -> Result<void>;

    // RUDP client — opens a shared UDP socket (if not already open) and creates a
    // ReliableUdpChannel to the given remote address.
    [[nodiscard]] auto connect_rudp(const Address& addr, const RudpProfile& profile = RudpProfile{})
        -> Result<ReliableUdpChannel*>;

    // RUDP client with congestion control disabled (nocwnd=true).
    // Use for intra-datacenter links where loss is near-zero and minimal latency
    // is more important than fairness (e.g. BaseApp ↔ CellApp).
    [[nodiscard]] auto connect_rudp_nocwnd(const Address& addr) -> Result<ReliableUdpChannel*>;

    [[nodiscard]] static auto internet_rudp_profile() -> RudpProfile;
    [[nodiscard]] static auto cluster_rudp_profile() -> RudpProfile;

    // Override the local address used when the first outbound RUDP client socket is bound.
    // The port may be left as 0 to request an ephemeral port from the OS.
    void set_rudp_client_bind_address(const Address& addr);

    // Channel access
    [[nodiscard]] auto find_channel(const Address& addr) -> Channel*;
    [[nodiscard]] auto find_channel(ChannelId id) -> Channel*;
    [[nodiscard]] auto channel_count() const -> size_t;

    // Message handling
    [[nodiscard]] auto interface_table() -> InterfaceTable& { return interface_table_; }

    // Addresses
    [[nodiscard]] auto tcp_address() const -> Address;
    [[nodiscard]] auto udp_address() const -> Address;
    [[nodiscard]] auto rudp_address() const -> Address;

    // Rate limiting (packets per second per IP, 0 = disabled)
    void set_rate_limit(uint32_t max_per_second);

    // Accept callback — called after each successful TCP accept.
    // Allows the application to install per-connection callbacks (e.g. disconnect handlers).
    using AcceptCallback = std::function<void(Channel&)>;
    void set_accept_callback(AcceptCallback cb);

    // Disconnect callback — notified after NetworkInterface detaches the channel
    // from the active set. Useful for higher-level session cleanup.
    using DisconnectCallback = std::function<void(Channel&)>;
    void set_disconnect_callback(DisconnectCallback cb);

    // Shutdown
    void prepare_for_shutdown();

private:
    // FrequentTask -- runs every tick for cleanup
    void do_task() override;

    // IO callbacks
    void on_tcp_accept();
    void on_udp_readable();
    void on_rudp_readable();

    // Channel lifecycle
    void on_channel_disconnect(Channel& channel);
    void condemn_channel(const Address& addr);
    void process_condemned_channels();
    [[nodiscard]] auto datagram_recv_buffer() -> std::span<std::byte>;

    // Rate limiting
    auto check_rate_limit(uint32_t ip) -> bool;
    void cleanup_stale_rate_trackers();
    [[nodiscard]] static auto callback_budget_exhausted(std::size_t processed, std::size_t budget)
        -> bool;

    EventDispatcher& dispatcher_;
    // IMPORTANT: registration_ must be declared after dispatcher_ so that its
    // destructor (which calls dispatcher_.remove_frequent_task) runs first.
    FrequentTaskRegistration registration_;
    InterfaceTable interface_table_;

    // TCP server
    std::optional<Socket> tcp_listen_socket_;
    Address tcp_address_;

    // UDP (plain unreliable)
    std::optional<Socket> udp_socket_;
    Address udp_address_;

    // RUDP (reliable UDP — shared socket for all ReliableUdpChannels)
    std::optional<Socket> rudp_socket_;
    Address rudp_address_;
    bool rudp_server_mode_{false};  // true: auto-create channels for unknown peers
    std::optional<Address> rudp_client_bind_address_;
    RudpProfile rudp_accept_profile_{};

    // Active channels keyed by remote address
    std::unordered_map<Address, std::unique_ptr<Channel>> channels_;
    std::unordered_map<ChannelId, Channel*> channels_by_id_;
    ChannelId next_channel_id_{1};

    // Condemned channels awaiting cleanup
    struct CondemnedEntry
    {
        std::unique_ptr<Channel> channel;
        TimePoint condemned_at;
    };
    std::deque<CondemnedEntry> condemned_;
    static constexpr Duration kCondemnTimeout = std::chrono::seconds(5);
    // Cap the condemned list to prevent unbounded growth under DDoS-style
    // connect/disconnect floods.  Entries over this limit are force-closed
    // immediately (oldest first) rather than waiting for the timeout.
    static constexpr std::size_t kMaxCondemnedChannels = 4096;

    // Rate limiting
    struct RateTracker
    {
        uint32_t count{0};
        TimePoint window_start;
    };
    std::unordered_map<uint32_t, RateTracker> rate_trackers_;  // keyed by IP (net order)
    uint32_t rate_limit_{0};                                   // 0 = disabled
    TimePoint last_rate_cleanup_{};
    static constexpr Duration kRateCleanupInterval = std::chrono::seconds(60);

    static constexpr std::size_t kMaxDatagramSize = 64 * 1024;
    static constexpr std::size_t kMaxAcceptsPerCallback = 128;
    static constexpr std::size_t kMaxDatagramsPerCallback = 1024;
    StreamBuffer datagram_recv_scratch_;

    bool shutting_down_{false};
    AcceptCallback accept_callback_;
    DisconnectCallback disconnect_callback_;
};

}  // namespace atlas
