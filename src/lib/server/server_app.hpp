#pragma once

#include "foundation/time.hpp"
#include "network/event_dispatcher.hpp"
#include "network/network_interface.hpp"
#include "platform/signal_handler.hpp"
#include "server/machined_client.hpp"
#include "server/server_config.hpp"
#include "server/signal_dispatch_task.hpp"
#include "server/updatable.hpp"
#include "server/watcher.hpp"

#include <cstdint>

namespace atlas
{

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

class ServerApp
{
public:
    ServerApp(EventDispatcher& dispatcher, NetworkInterface& network);
    virtual ~ServerApp();

    // Non-copyable, non-movable
    ServerApp(const ServerApp&) = delete;
    ServerApp& operator=(const ServerApp&) = delete;

    // ---- Entry point --------------------------------------------------------

    // Main entry: init → run → fini. Returns process exit code.
    auto run_app(int argc, char* argv[]) -> int;

    // Request graceful shutdown (safe to call from any tick callback).
    void shutdown();

    // ---- Accessors ----------------------------------------------------------

    [[nodiscard]] auto dispatcher() -> EventDispatcher& { return dispatcher_; }
    [[nodiscard]] auto network() -> NetworkInterface& { return network_; }
    [[nodiscard]] auto config() const -> const ServerConfig& { return config_; }
    [[nodiscard]] auto game_clock() -> GameClock& { return game_clock_; }
    [[nodiscard]] auto watcher_registry() -> WatcherRegistry& { return watcher_registry_; }
    [[nodiscard]] auto machined_client() -> MachinedClient& { return machined_client_; }

    // ---- Time ---------------------------------------------------------------

    [[nodiscard]] auto game_time() const -> uint64_t { return game_clock_.frame_count(); }
    [[nodiscard]] auto game_time_seconds() const -> double;
    [[nodiscard]] auto uptime_seconds() const -> double;

    // ---- Updatable registration ---------------------------------------------

    auto register_for_update(Updatable* object, int level = 0) -> bool;
    auto deregister_for_update(Updatable* object) -> bool;

protected:
    // ---- Subclass override points -------------------------------------------

    // Called after config is loaded. Subclasses must call ServerApp::init() first.
    [[nodiscard]] virtual auto init(int argc, char* argv[]) -> bool;

    // Called after run_loop() returns. Subclasses call ServerApp::fini() last.
    virtual void fini();

    // Default: dispatcher_.run(). Override for custom loop behaviour.
    [[nodiscard]] virtual auto run_loop() -> bool;

    // Called once run_loop() has returned, before fini().
    virtual void on_run_complete() {}

    // ---- Tick hooks (called in order each frame) ----------------------------

    // After game_clock_.tick() — new tick value visible, Updatables not yet called.
    virtual void on_end_of_tick() {}

    // Immediately before updatables_.call().
    virtual void on_start_of_tick() {}

    // After all Updatables have been called.
    virtual void on_tick_complete() {}

    // ---- Signal hook --------------------------------------------------------

    // Default: SIGINT / SIGTERM → shutdown().
    virtual void on_signal(Signal sig);

    // ---- Watcher registration -----------------------------------------------

    // Called once during init(). Subclasses override, call base first.
    virtual void register_watchers();

private:
    // Tick driver — registered as a repeating timer on dispatcher_.
    void advance_time();

    // Raise file-descriptor / handle limits early in run_app().
    void raise_fd_limit();

    EventDispatcher& dispatcher_;
    NetworkInterface& network_;
    ServerConfig config_;
    GameClock game_clock_;
    Updatables updatables_;
    WatcherRegistry watcher_registry_;

    // Tick performance statistics
    struct TickStats
    {
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
