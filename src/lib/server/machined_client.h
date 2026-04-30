#ifndef ATLAS_LIB_SERVER_MACHINED_CLIENT_H_
#define ATLAS_LIB_SERVER_MACHINED_CLIENT_H_

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
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

  MachinedClient(EventDispatcher& dispatcher, NetworkInterface& network);
  ~MachinedClient();

  MachinedClient(const MachinedClient&) = delete;
  MachinedClient& operator=(const MachinedClient&) = delete;

  // Not thread-safe; use from the EventDispatcher thread.
  [[nodiscard]] auto Connect(const Address& machined_addr) -> bool;

  [[nodiscard]] auto IsConnected() const -> bool;

  void SendRegister(const ServerConfig& cfg);
  void SendDeregister(const ServerConfig& cfg);

  void SendHeartbeat(float load = 0.0f, uint32_t entity_count = 0);

  [[nodiscard]] auto QuerySync(ProcessType type, Duration timeout = std::chrono::seconds(5))
      -> std::vector<machined::ProcessInfo>;

  using QueryCallback = std::function<void(std::vector<machined::ProcessInfo>)>;
  void QueryAsync(ProcessType type, QueryCallback cb);

  void Subscribe(machined::ListenerType listener_type, ProcessType target_type,
                 BirthCallback on_birth, DeathCallback on_death);

  void Tick(float load = 0.0f, uint32_t entity_count = 0);

  static constexpr Duration kHeartbeatInterval = std::chrono::seconds(5);

 private:
  void RegisterHandlers();

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
