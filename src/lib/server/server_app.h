#ifndef ATLAS_LIB_SERVER_SERVER_APP_H_
#define ATLAS_LIB_SERVER_SERVER_APP_H_

#include <cstdint>

#include "foundation/clock.h"
#include "network/event_dispatcher.h"
#include "network/network_interface.h"
#include "platform/signal_handler.h"
#include "server/machined_client.h"
#include "server/server_config.h"
#include "server/signal_dispatch_task.h"
#include "server/updatable.h"
#include "server/watcher.h"

namespace atlas {

class ServerApp {
 public:
  ServerApp(EventDispatcher& dispatcher, NetworkInterface& network);
  virtual ~ServerApp();

  ServerApp(const ServerApp&) = delete;
  ServerApp& operator=(const ServerApp&) = delete;

  auto RunApp(int argc, char* argv[]) -> int;

  void Shutdown();

  [[nodiscard]] auto Dispatcher() -> EventDispatcher& { return dispatcher_; }
  [[nodiscard]] auto Network() -> NetworkInterface& { return network_; }
  [[nodiscard]] auto Config() const -> const ServerConfig& { return config_; }
  [[nodiscard]] auto GetGameClock() -> class GameClock& { return game_clock_; }
  [[nodiscard]] auto GetWatcherRegistry() -> class WatcherRegistry& { return watcher_registry_; }
  [[nodiscard]] auto GetMachinedClient() -> class MachinedClient& { return machined_client_; }

  [[nodiscard]] auto GameTime() const -> uint64_t { return game_clock_.FrameCount(); }
  [[nodiscard]] auto GameTimeSeconds() const -> double;
  [[nodiscard]] auto UptimeSeconds() const -> double;

  auto RegisterForUpdate(Updatable* object, int level = 0) -> bool;
  auto DeregisterForUpdate(Updatable* object) -> bool;

  // Last tick's work duration (time spent inside OnEndOfTick ->
  // OnTickComplete). Subclasses consume this for load reporting; the
  // normalised load fraction is `last_work_duration / expected_tick_period`.
  [[nodiscard]] auto LastTickWorkDuration() const -> Duration {
    return tick_stats_.last_work_duration;
  }

  // Expected wall-clock duration of a single tick.
  [[nodiscard]] auto ExpectedTickPeriod() const -> Duration {
    return std::chrono::duration_cast<Duration>(
        std::chrono::duration<double>(1.0 / config_.update_hertz));
  }

 protected:
  [[nodiscard]] virtual auto Init(int argc, char* argv[]) -> bool;

  virtual void Fini();

  [[nodiscard]] virtual auto RunLoop() -> bool;

  virtual void OnRunComplete() {}

  virtual void OnEndOfTick() {}
  virtual void OnStartOfTick() {}
  virtual void OnTickComplete() {}

  // Flush deferred-send bundles staged by the just-completed tick.
  // Runs after OnTickComplete inside AdvanceTime; pairs with the
  // FlushDirtySendChannels calls at the end of NetworkInterface's
  // readable callbacks.
  virtual void FlushTickDirtyChannels() { network_.FlushDirtySendChannels(); }

  virtual void OnSignal(Signal sig);

  virtual void RegisterWatchers();

 private:
  void AdvanceTime();

  void RaiseFdLimit();

  EventDispatcher& dispatcher_;
  NetworkInterface& network_;
  ServerConfig config_;
  GameClock game_clock_;
  Updatables updatables_;
  WatcherRegistry watcher_registry_;

  struct TickStats {
    Duration last_duration{};
    Duration last_work_duration{};
    Duration max_duration{};
    uint64_t slow_count{0};
  } tick_stats_;

  TimePoint last_tick_time_{};
  TimePoint start_time_{};
  TimerHandle tick_timer_{};

  SignalDispatchTask signal_task_;
  FrequentTaskRegistration signal_registration_;

  bool shutdown_requested_{false};

  MachinedClient machined_client_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_SERVER_APP_H_
