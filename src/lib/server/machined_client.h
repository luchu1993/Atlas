#ifndef ATLAS_LIB_SERVER_MACHINED_CLIENT_H_
#define ATLAS_LIB_SERVER_MACHINED_CLIENT_H_

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "foundation/clock.h"
#include "network/address.h"
#include "network/machined_types.h"
#include "server/server_config.h"

namespace atlas {

class Channel;
class EventDispatcher;
class NetworkInterface;

class MachinedClient {
 public:
  using BirthCallback = std::function<void(const machined::BirthNotification&)>;
  using DeathCallback = std::function<void(const machined::DeathNotification&)>;
  using WatcherCallback =
      std::function<void(bool found, const std::string& source_name, const std::string& value)>;

  MachinedClient(EventDispatcher& dispatcher, NetworkInterface& network);
  ~MachinedClient();

  MachinedClient(const MachinedClient&) = delete;
  MachinedClient& operator=(const MachinedClient&) = delete;

  // Not thread-safe; use from the EventDispatcher thread.
  // Caches the address so Tick can transparently reconnect after a drop.
  [[nodiscard]] auto Connect(const Address& machined_addr) -> bool;

  [[nodiscard]] auto IsConnected() const -> bool;

  // Caches the registration so we can replay it after a reconnect.
  void SendRegister(const ServerConfig& cfg);
  void SendDeregister(const ServerConfig& cfg);

  void SendHeartbeat(float load = 0.0f, uint32_t entity_count = 0);

  [[nodiscard]] auto QuerySync(ProcessType type, Duration timeout = std::chrono::seconds(5))
      -> std::vector<machined::ProcessInfo>;

  using QueryCallback = std::function<void(std::vector<machined::ProcessInfo>)>;
  void QueryAsync(ProcessType type, QueryCallback cb);

  void Subscribe(machined::ListenerType listener_type, ProcessType target_type,
                 BirthCallback on_birth, DeathCallback on_death);

  // Forwards a watcher path query through machined to a registered process.
  // empty target_name = first instance of target_type. Returns request id.
  auto QueryWatcher(ProcessType target_type, std::string_view target_name,
                    std::string_view watcher_path, WatcherCallback cb) -> uint32_t;

  // Asks machined to forward a ShutdownRequest to a registered process.
  // empty target_name = all instances of target_type.
  void RequestShutdownTarget(ProcessType target_type, std::string_view target_name,
                             uint8_t reason = 0);

  void Tick(float load = 0.0f, uint32_t entity_count = 0);

  static constexpr Duration kHeartbeatInterval = std::chrono::seconds(5);
  static constexpr Duration kInitialReconnectBackoff = std::chrono::seconds(1);
  static constexpr Duration kMaxReconnectBackoff = std::chrono::seconds(30);

 private:
  void RegisterHandlers();
  void DetectStaleChannel();
  void HandleDisconnect();
  void MaybeReconnect();
  void SendCachedRegister();

  void OnRegisterAck(const Address& src, Channel* ch, const machined::RegisterAck& msg);
  void OnHeartbeatAck(const Address& src, Channel* ch, const machined::HeartbeatAck& msg);
  void OnQueryResponse(const Address& src, Channel* ch, const machined::QueryResponse& msg);
  void OnBirthNotification(const Address& src, Channel* ch, const machined::BirthNotification& msg);
  void OnDeathNotification(const Address& src, Channel* ch, const machined::DeathNotification& msg);
  void OnListenerAck(const Address& src, Channel* ch, const machined::ListenerAck& msg);
  void OnWatcherResponse(const Address& src, Channel* ch, const machined::WatcherResponse& msg);
  void OnDeathFromMachined(const Address& src, Channel* ch, const machined::DeathNotification& msg);

  EventDispatcher& dispatcher_;
  NetworkInterface& network_;
  Channel* channel_{nullptr};
  Address machined_addr_{};

  struct SyncQuery {
    bool done{false};
    std::vector<machined::ProcessInfo> result;
  };
  std::optional<SyncQuery> sync_query_;

  QueryCallback async_query_cb_;

  struct Subscription {
    machined::ListenerType listener_type;
    ProcessType target_type;
    BirthCallback on_birth;
    DeathCallback on_death;
  };
  std::vector<Subscription> subscriptions_;

  // Cached registration for replay after reconnect.
  struct RegistrationInfo {
    ProcessType process_type;
    std::string name;
    uint16_t internal_port;
    uint16_t external_port;
  };
  std::optional<RegistrationInfo> cached_reg_;

  // Reconnect bookkeeping; disabled after explicit Deregister.
  bool reconnect_enabled_{true};
  Duration reconnect_backoff_{kInitialReconnectBackoff};
  TimePoint next_reconnect_attempt_{};

  // Watcher-request demuxing: incoming WatcherResponse is matched on request_id.
  uint32_t next_watcher_request_id_{1};
  std::unordered_map<uint32_t, WatcherCallback> pending_watchers_;

  // Heartbeat tracking
  TimePoint last_heartbeat_{};
  bool registered_{false};

  // When machined advertises a UDP heartbeat port, we send heartbeats there
  // via a plain UdpChannel instead of the TCP control channel.
  Address machined_heartbeat_udp_addr_;  // {0,0} = not set, use TCP

  bool handlers_registered_{false};
};

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_MACHINED_CLIENT_H_
