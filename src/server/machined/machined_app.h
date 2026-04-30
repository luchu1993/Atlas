#ifndef ATLAS_SERVER_MACHINED_MACHINED_APP_H_
#define ATLAS_SERVER_MACHINED_MACHINED_APP_H_

#include "machined/listener_manager.h"
#include "machined/process_registry.h"
#include "machined/watcher_forwarder.h"
#include "network/frequent_task.h"
#include "server/manager_app.h"

namespace atlas::machined {

class MachinedApp : public ManagerApp {
 public:
  MachinedApp(EventDispatcher& dispatcher, NetworkInterface& network);

  static auto Run(int argc, char* argv[]) -> int;

 protected:
  [[nodiscard]] auto Init(int argc, char* argv[]) -> bool override;
  void Fini() override;

  void RegisterWatchers() override;

  void OnTickComplete() override;

 private:
  void OnRegister(const Address& src, Channel* ch, const RegisterMessage& msg);
  void OnDeregister(const Address& src, Channel* ch, const DeregisterMessage& msg);
  void OnHeartbeat(const Address& src, Channel* ch, const HeartbeatMessage& msg);
  void OnQuery(const Address& src, Channel* ch, const QueryMessage& msg);
  void OnListenerRegister(const Address& src, Channel* ch, const ListenerRegister& msg);
  void OnWatcherRequest(const Address& src, Channel* ch, const WatcherRequest& msg);
  void OnWatcherReply(const Address& src, Channel* ch, const WatcherReply& msg);

  void OnAccept(Channel& ch);

  void OnDisconnect(Channel& ch);

  void CheckHeartbeatTimeouts();

  ProcessRegistry process_registry_;
  ListenerManager listener_manager_;
  WatcherForwarder watcher_forwarder_;

  static constexpr Duration kHeartbeatTimeout = std::chrono::seconds(15);

  struct HeartbeatEntry {
    Channel* channel{nullptr};
    TimePoint last_heartbeat;
  };
  std::vector<HeartbeatEntry> heartbeat_entries_;

  uint16_t heartbeat_udp_port_{0};
};

}  // namespace atlas::machined

#endif  // ATLAS_SERVER_MACHINED_MACHINED_APP_H_
