#ifndef ATLAS_LIB_NETWORK_NETWORK_INTERFACE_H_
#define ATLAS_LIB_NETWORK_NETWORK_INTERFACE_H_

#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
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

// Not thread-safe; all calls must originate from EventDispatcher's thread.
class NetworkInterface : public FrequentTask {
 public:
  struct RudpProfile {
    bool nocwnd{false};
    uint32_t send_window{256};
    uint32_t recv_window{256};
    // UDP body MTU; both endpoints must agree for fragment reassembly.
    std::size_t mtu{1400};
    // OOM safety net; tick-end flush remains the primary drain.
    std::size_t deferred_flush_threshold{60 * 1024};
  };

  explicit NetworkInterface(EventDispatcher& dispatcher);
  ~NetworkInterface() override;

  NetworkInterface(const NetworkInterface&) = delete;
  NetworkInterface& operator=(const NetworkInterface&) = delete;

  [[nodiscard]] auto StartTcpServer(const Address& addr) -> Result<void>;

  [[nodiscard]] auto ConnectTcp(const Address& addr) -> Result<TcpChannel*>;

  [[nodiscard]] auto StartUdp(const Address& addr) -> Result<void>;

  [[nodiscard]] auto ConnectUdp(const Address& addr) -> Result<UdpChannel*>;

  // Unknown peers create ReliableUdpChannel instances on the shared socket.
  [[nodiscard]] auto StartRudpServer(const Address& addr) -> Result<void>;
  [[nodiscard]] auto StartRudpServer(const Address& addr, const RudpProfile& accept_profile)
      -> Result<void>;

  [[nodiscard]] auto ConnectRudp(const Address& addr) -> Result<ReliableUdpChannel*>;
  [[nodiscard]] auto ConnectRudp(const Address& addr, const RudpProfile& profile)
      -> Result<ReliableUdpChannel*>;

  // For intra-DC links where latency outweighs fairness.
  [[nodiscard]] auto ConnectRudpNocwnd(const Address& addr) -> Result<ReliableUdpChannel*>;

  [[nodiscard]] static auto InternetRudpProfile() -> RudpProfile;
  [[nodiscard]] static auto ClusterRudpProfile() -> RudpProfile;

  // Port 0 lets the OS choose the outbound RUDP client port.
  void SetRudpClientBindAddress(const Address& addr);

  [[nodiscard]] auto FindChannel(const Address& addr) -> Channel*;
  [[nodiscard]] auto FindChannel(ChannelId id) -> Channel*;
  [[nodiscard]] auto ChannelCount() const -> size_t;
  [[nodiscard]] auto CondemnedChannelCount() const -> size_t { return condemned_.size(); }

  [[nodiscard]] auto InterfaceTable() -> InterfaceTable& { return interface_table_; }

  [[nodiscard]] auto TcpAddress() const -> Address;
  [[nodiscard]] auto UdpAddress() const -> Address;
  [[nodiscard]] auto RudpAddress() const -> Address;

  // packets/sec/IP; 0 disables.
  void SetRateLimit(uint32_t max_per_second);

  // Runs after each successful TCP accept.
  using AcceptCallback = std::function<void(Channel&)>;
  void SetAcceptCallback(AcceptCallback cb);

  // Runs after the channel leaves the active set.
  using DisconnectCallback = std::function<void(Channel&)>;
  void SetDisconnectCallback(DisconnectCallback cb);

  // Apps with their own tick cadence call FlushDirtySendChannels at tick end.
  void MarkChannelDirty(Channel& channel) { dirty_channels_.insert(&channel); }
  void FlushDirtySendChannels();

  void PrepareForShutdown();

 private:
  void DoTask() override;

  void OnTcpAccept();
  void OnUdpReadable();
  void OnRudpReadable();

  void OnChannelDisconnect(Channel& channel);
  void CondemnChannel(Address addr);
  void ProcessCondemnedChannels();
  [[nodiscard]] auto ShouldDeleteCondemned(const Channel& channel, TimePoint condemned_at,
                                           TimePoint now) const -> bool;
  // Address alone is unsafe after same-peer re-condemn.
  void EraseCondemnedRudpIndex(Channel* channel);
  [[nodiscard]] auto DatagramRecvBuffer() -> std::span<std::byte>;

  // Resumes RUDP channels that yielded mid-cascade.
  void DrainHotChannels(TimePoint deadline);

  auto CheckRateLimit(uint32_t ip) -> bool;
  void CleanupStaleRateTrackers();
  [[nodiscard]] static auto CallbackBudgetExhausted(std::size_t processed, std::size_t budget)
      -> bool;

  EventDispatcher& dispatcher_;
  FrequentTaskRegistration registration_;
  class InterfaceTable interface_table_;

  std::optional<Socket> tcp_listen_socket_;
  Address tcp_address_;

  std::optional<Socket> udp_socket_;
  Address udp_address_;

  std::optional<Socket> rudp_socket_;
  Address rudp_address_;
  bool rudp_server_mode_{false};
  std::optional<Address> rudp_client_bind_address_;
  RudpProfile rudp_accept_profile_{};

  std::unordered_map<Address, std::unique_ptr<Channel>> channels_;
  std::unordered_map<ChannelId, Channel*> channels_by_id_;
  ChannelId next_channel_id_{1};

  // Bare pointers; CondemnChannel must scrub them.
  std::unordered_set<ReliableUdpChannel*> hot_channels_;

  // Bare pointers; CondemnChannel must scrub them.
  std::unordered_set<Channel*> dirty_channels_;

  struct CondemnedEntry {
    std::unique_ptr<Channel> channel;
    TimePoint condemned_at;
  };
  std::list<CondemnedEntry> condemned_;
  // Keeps tail ACKs attached to the condemned RUDP instance.
  std::unordered_map<Address, ReliableUdpChannel*> condemned_rudp_by_addr_;
  // Backstop above kDeadLinkTimeout.
  static constexpr Duration kCondemnAgeLimit = std::chrono::seconds(6);
  // OOM safety net for broken dead-link paths.
  static constexpr std::size_t kMaxCondemnedChannels = 4096;

  struct RateTracker {
    uint32_t count{0};
    TimePoint window_start;
  };
  std::unordered_map<uint32_t, RateTracker> rate_trackers_;
  uint32_t rate_limit_{0};
  TimePoint last_rate_cleanup_{};
  static constexpr Duration kRateCleanupInterval = std::chrono::seconds(60);

  static constexpr std::size_t kMaxChannels = 8192;
  static constexpr std::size_t kMaxDatagramSize = 64 * 1024;
  static constexpr std::size_t kMaxAcceptsPerCallback = 128;
  // Hard cap for a recv queue that outruns the time budget.
  static constexpr std::size_t kMaxDatagramsPerCallback = 4096;
  // Keeps bursty recv loops from starving timers.
  static constexpr Duration kReadableCallbackBudget = std::chrono::milliseconds(10);
  StreamBuffer datagram_recv_scratch_;

  bool shutting_down_{false};
  AcceptCallback accept_callback_;
  DisconnectCallback disconnect_callback_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_NETWORK_NETWORK_INTERFACE_H_
