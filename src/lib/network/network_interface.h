#ifndef ATLAS_LIB_NETWORK_NETWORK_INTERFACE_H_
#define ATLAS_LIB_NETWORK_NETWORK_INTERFACE_H_

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "foundation/clock.h"
#include "foundation/memory/stream_buffer_pool.h"
#include "network/address.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/frequent_task.h"
#include "network/interface_table.h"
#include "network/socket.h"

namespace atlas {

class Channel;
class TcpChannel;
class UdpChannel;
class ReliableUdpChannel;

// Thread safety: NOT thread-safe. All calls must originate from EventDispatcher's thread.
class NetworkInterface : public FrequentTask {
 public:
  struct RudpProfile {
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
  [[nodiscard]] auto StartTcpServer(const Address& addr) -> Result<void>;

  // TCP client (async connect) — always sets TCP_NODELAY for low-latency internal links
  [[nodiscard]] auto ConnectTcp(const Address& addr) -> Result<TcpChannel*>;

  // UDP endpoint (plain unreliable, used by machined etc.)
  [[nodiscard]] auto StartUdp(const Address& addr) -> Result<void>;

  // UDP client — opens a shared UDP socket (if not already open) and creates a
  // UdpChannel to the given remote address for fire-and-forget sends.
  [[nodiscard]] auto ConnectUdp(const Address& addr) -> Result<UdpChannel*>;

  // RUDP server — listens on a shared UDP socket; incoming datagrams from new peers
  // automatically create ReliableUdpChannel instances (used for external client connections).
  [[nodiscard]] auto StartRudpServer(const Address& addr) -> Result<void>;
  [[nodiscard]] auto StartRudpServer(const Address& addr, const RudpProfile& accept_profile)
      -> Result<void>;

  // RUDP client — opens a shared UDP socket (if not already open) and creates a
  // ReliableUdpChannel to the given remote address.
  [[nodiscard]] auto ConnectRudp(const Address& addr) -> Result<ReliableUdpChannel*>;
  [[nodiscard]] auto ConnectRudp(const Address& addr, const RudpProfile& profile)
      -> Result<ReliableUdpChannel*>;

  // RUDP client with congestion control disabled (nocwnd=true).
  // Use for intra-datacenter links where loss is near-zero and minimal latency
  // is more important than fairness (e.g. BaseApp ↔ CellApp).
  [[nodiscard]] auto ConnectRudpNocwnd(const Address& addr) -> Result<ReliableUdpChannel*>;

  [[nodiscard]] static auto InternetRudpProfile() -> RudpProfile;
  [[nodiscard]] static auto ClusterRudpProfile() -> RudpProfile;

  // Override the local address used when the first outbound RUDP client socket is bound.
  // The port may be left as 0 to request an ephemeral port from the OS.
  void SetRudpClientBindAddress(const Address& addr);

  // Channel access
  [[nodiscard]] auto FindChannel(const Address& addr) -> Channel*;
  [[nodiscard]] auto FindChannel(ChannelId id) -> Channel*;
  [[nodiscard]] auto ChannelCount() const -> size_t;

  // Message handling
  [[nodiscard]] auto InterfaceTable() -> InterfaceTable& { return interface_table_; }

  // Addresses
  [[nodiscard]] auto TcpAddress() const -> Address;
  [[nodiscard]] auto UdpAddress() const -> Address;
  [[nodiscard]] auto RudpAddress() const -> Address;

  // Rate limiting (packets per second per IP, 0 = disabled)
  void SetRateLimit(uint32_t max_per_second);

  // Accept callback — called after each successful TCP accept.
  // Allows the application to install per-connection callbacks (e.g. disconnect handlers).
  using AcceptCallback = std::function<void(Channel&)>;
  void SetAcceptCallback(AcceptCallback cb);

  // Disconnect callback — notified after NetworkInterface detaches the channel
  // from the active set. Useful for higher-level session cleanup.
  using DisconnectCallback = std::function<void(Channel&)>;
  void SetDisconnectCallback(DisconnectCallback cb);

  // Shutdown
  void PrepareForShutdown();

 private:
  // FrequentTask -- runs every tick for cleanup
  void DoTask() override;

  // IO callbacks
  void OnTcpAccept();
  void OnUdpReadable();
  void OnRudpReadable();

  // Channel lifecycle
  void OnChannelDisconnect(Channel& channel);
  void CondemnChannel(const Address& addr);
  void ProcessCondemnedChannels();
  [[nodiscard]] auto DatagramRecvBuffer() -> std::span<std::byte>;

  // Rate limiting
  auto CheckRateLimit(uint32_t ip) -> bool;
  void CleanupStaleRateTrackers();
  [[nodiscard]] static auto CallbackBudgetExhausted(std::size_t processed, std::size_t budget)
      -> bool;

  EventDispatcher& dispatcher_;
  // IMPORTANT: registration_ must be declared after dispatcher_ so that its
  // destructor (which calls dispatcher_.remove_frequent_task) runs first.
  FrequentTaskRegistration registration_;
  class InterfaceTable interface_table_;

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
  struct CondemnedEntry {
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
  struct RateTracker {
    uint32_t count{0};
    TimePoint window_start;
  };
  std::unordered_map<uint32_t, RateTracker> rate_trackers_;  // keyed by IP (net order)
  uint32_t rate_limit_{0};                                   // 0 = disabled
  TimePoint last_rate_cleanup_{};
  static constexpr Duration kRateCleanupInterval = std::chrono::seconds(60);

  static constexpr std::size_t kMaxChannels = 8192;
  static constexpr std::size_t kMaxDatagramSize = 64 * 1024;
  static constexpr std::size_t kMaxAcceptsPerCallback = 128;
  // Hard cap — safety net in case the OS recv queue never drains.
  // Normal exit is via kReadableCallbackBudget (time-based).
  static constexpr std::size_t kMaxDatagramsPerCallback = 4096;
  // Per-callback time budget for UDP/RUDP receive loops. Bounding by wall
  // time (rather than datagram count) prevents bursty traffic from starving
  // the EventDispatcher timer wheel: the loop exits after ~10 ms regardless
  // of how many packets are waiting.
  static constexpr Duration kReadableCallbackBudget = std::chrono::milliseconds(10);
  StreamBuffer datagram_recv_scratch_;

  bool shutting_down_{false};
  AcceptCallback accept_callback_;
  DisconnectCallback disconnect_callback_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_NETWORK_INTERFACE_H_
