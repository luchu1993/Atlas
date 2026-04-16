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

// ============================================================================
// ServerApp — base class for all Atlas server processes
//
// Lifecycle (run_app):
//   1. ServerConfig::load(argc, argv)
//   2. init(argc, argv)          — subclasses chain-call base first
//   3. run_loop()                — default: dispatcher_.run()
//   4. on_run_complete()
//   5. fini()                    — subclasses chain-call base last
//
// Tick sequence (advance_time, called by repeating timer):
//   measure elapsed → game_clock_.tick(fixed_delta)
//   → on_end_of_tick() → on_start_of_tick() → updatables_.call()
//   → on_tick_complete()
// ============================================================================

class ServerApp {
 public:
  ServerApp(EventDispatcher& dispatcher, NetworkInterface& network);
  virtual ~ServerApp();

  // Non-copyable, non-movable
  ServerApp(const ServerApp&) = delete;
  ServerApp& operator=(const ServerApp&) = delete;

  // ---- Entry point --------------------------------------------------------

  // Main entry: init → run → fini. Returns process exit code.
  auto RunApp(int argc, char* argv[]) -> int;

  // Request graceful shutdown (safe to call from any tick callback).
  void Shutdown();

  // ---- Accessors ----------------------------------------------------------

  [[nodiscard]] auto Dispatcher() -> EventDispatcher& { return dispatcher_; }
  [[nodiscard]] auto Network() -> NetworkInterface& { return network_; }
  [[nodiscard]] auto Config() const -> const ServerConfig& { return config_; }
  [[nodiscard]] auto GetGameClock() -> class GameClock& { return game_clock_; }
  [[nodiscard]] auto GetWatcherRegistry() -> class WatcherRegistry& { return watcher_registry_; }
  [[nodiscard]] auto GetMachinedClient() -> class MachinedClient& { return machined_client_; }

  // ---- Time ---------------------------------------------------------------

  [[nodiscard]] auto GameTime() const -> uint64_t { return game_clock_.FrameCount(); }
  [[nodiscard]] auto GameTimeSeconds() const -> double;
  [[nodiscard]] auto UptimeSeconds() const -> double;

  // ---- Updatable registration ---------------------------------------------

  auto RegisterForUpdate(Updatable* object, int level = 0) -> bool;
  auto DeregisterForUpdate(Updatable* object) -> bool;

 protected:
  // ---- Subclass override points -------------------------------------------

  // Called after config is loaded. Subclasses must call ServerApp::init() first.
  [[nodiscard]] virtual auto Init(int argc, char* argv[]) -> bool;

  // Called after run_loop() returns. Subclasses call ServerApp::fini() last.
  virtual void Fini();

  // Default: dispatcher_.run(). Override for custom loop behaviour.
  [[nodiscard]] virtual auto RunLoop() -> bool;

  // Called once run_loop() has returned, before fini().
  virtual void OnRunComplete() {}

  // ---- Tick hooks (called in order each frame) ----------------------------

  // After game_clock_.tick() — new tick value visible, Updatables not yet called.
  virtual void OnEndOfTick() {}

  // Immediately before updatables_.call().
  virtual void OnStartOfTick() {}

  // After all Updatables have been called.
  virtual void OnTickComplete() {}

  // ---- Signal hook --------------------------------------------------------

  // Default: SIGINT / SIGTERM → shutdown().
  virtual void OnSignal(Signal sig);

  // ---- Watcher registration -----------------------------------------------

  // Called once during init(). Subclasses override, call base first.
  virtual void RegisterWatchers();

 private:
  // Tick driver — registered as a repeating timer on dispatcher_.
  void AdvanceTime();

  // Raise file-descriptor / handle limits early in run_app().
  void RaiseFdLimit();

  EventDispatcher& dispatcher_;
  NetworkInterface& network_;
  ServerConfig config_;
  GameClock game_clock_;
  Updatables updatables_;
  WatcherRegistry watcher_registry_;

  // Tick performance statistics
  struct TickStats {
    Duration last_duration{};
    Duration max_duration{};
    uint64_t slow_count{0};
  } tick_stats_;

  TimePoint last_tick_time_{};
  TimePoint start_time_{};
  TimerHandle tick_timer_{};

  // Signal dispatch (FrequentTask → dispatch_pending_signals each loop iteration)
  SignalDispatchTask signal_task_;
  FrequentTaskRegistration signal_registration_;

  bool shutdown_requested_{false};

  MachinedClient machined_client_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SERVER_SERVER_APP_H_
