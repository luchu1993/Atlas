#include "server/server_app.hpp"

#include "foundation/log.hpp"
#include "foundation/runtime.hpp"
#include "server/server_app_option.hpp"

#include <chrono>
#include <format>

#if defined(_WIN32)
#include <io.h>  // _setmaxstdio
#else
#include <sys/resource.h>
#endif

#if defined(_WIN32)
#include <process.h>  // _getpid
#else
#include <unistd.h>  // getpid
#endif

namespace atlas
{

// ============================================================================
// Construction / destruction
// ============================================================================

ServerApp::ServerApp(EventDispatcher& dispatcher, NetworkInterface& network)
    : dispatcher_(dispatcher), network_(network), machined_client_(dispatcher, network)
{
}

ServerApp::~ServerApp() = default;

// ============================================================================
// Public API
// ============================================================================

auto ServerApp::run_app(int argc, char* argv[]) -> int
{
    // 1. Load configuration
    auto cfg_result = ServerConfig::load(argc, argv);
    if (!cfg_result)
    {
        ATLAS_LOG_CRITICAL("Failed to load config: {}", cfg_result.error().message());
        return 1;
    }
    config_ = std::move(*cfg_result);
    // 2. Configure logger
    RuntimeConfig runtime_cfg;
    runtime_cfg.log_level = config_.log_level;
    auto runtime_result = Runtime::initialize(runtime_cfg);
    if (!runtime_result)
    {
        return 1;
    }

    // 3. Apply ServerAppOption values from raw config
    if (config_.raw_config)
        ServerAppOption<int>::apply_all(
            *config_.raw_config->root());  // template ignored; uses global list

    // 4. Raise fd / handle limits
    raise_fd_limit();

    // 5. Init
    if (!init(argc, argv))
    {
        ATLAS_LOG_CRITICAL("ServerApp::init() failed");
        Runtime::finalize();
        return 1;
    }
#if defined(_WIN32)
    int pid = _getpid();
#else
    int pid = getpid();
#endif
    ATLAS_LOG_INFO("{} started (pid={})", config_.process_name, pid);

    // 6. Run
    bool run_ok = run();

    // 7. Post-run hook
    on_run_complete();

    // 8. Cleanup
    fini();
    Runtime::finalize();

    return run_ok ? 0 : 1;
}

void ServerApp::shutdown()
{
    if (!shutdown_requested_)
    {
        shutdown_requested_ = true;
        dispatcher_.stop();
    }
}

auto ServerApp::game_time_seconds() const -> double
{
    using namespace std::chrono;
    return duration_cast<Seconds>(game_clock_.elapsed()).count();
}

auto ServerApp::uptime_seconds() const -> double
{
    using namespace std::chrono;
    auto elapsed = Clock::now() - start_time_;
    return duration_cast<Seconds>(elapsed).count();
}

auto ServerApp::register_for_update(Updatable* object, int level) -> bool
{
    return updatables_.add(object, level);
}

auto ServerApp::deregister_for_update(Updatable* object) -> bool
{
    return updatables_.remove(object);
}

// ============================================================================
// Protected lifecycle
// ============================================================================

auto ServerApp::init(int argc, char* argv[]) -> bool
{
    (void)argc;
    (void)argv;

    start_time_ = Clock::now();
    last_tick_time_ = start_time_;

    // Install signal handlers — callbacks run on main thread via SignalDispatchTask
    install_signal_handler(Signal::Interrupt, [this](Signal s) { on_signal(s); });
    install_signal_handler(Signal::Terminate, [this](Signal s) { on_signal(s); });

    // Register FrequentTask so dispatch_pending_signals() is called each loop
    signal_registration_ = dispatcher_.add_frequent_task(&signal_task_);

    // Register tick timer
    auto tick_interval = std::chrono::duration_cast<Duration>(
        std::chrono::duration<double>(1.0 / config_.update_hertz));
    tick_timer_ =
        dispatcher_.add_repeating_timer(tick_interval, [this](TimerHandle) { advance_time(); });

    // Register Watcher entries
    register_watchers();

    // Connect to machined (skip for the machined process itself)
    if (config_.process_type != ProcessType::Machined)
    {
        if (machined_client_.connect(config_.machined_address))
        {
            machined_client_.send_register(config_);
        }
        else
        {
            ATLAS_LOG_WARNING("ServerApp: could not connect to machined at {} — continuing",
                              config_.machined_address.to_string());
        }
    }

    return true;
}

void ServerApp::fini()
{
    // Deregister from machined before shutting down
    if (config_.process_type != ProcessType::Machined && machined_client_.is_connected())
    {
        machined_client_.send_deregister(config_);
    }

    // Cancel the tick timer to stop new advance_time() calls
    if (tick_timer_.is_valid())
        dispatcher_.cancel_timer(tick_timer_);

    // Signal registration cleaned up by FrequentTaskRegistration RAII destructor

    // Remove signal handlers
    remove_signal_handler(Signal::Interrupt);
    remove_signal_handler(Signal::Terminate);
}

auto ServerApp::run() -> bool
{
    dispatcher_.run();
    return true;
}

void ServerApp::on_signal(Signal sig)
{
    ATLAS_LOG_INFO("Received signal {}, shutting down...", static_cast<int>(sig));
    shutdown();
}

void ServerApp::register_watchers()
{
    auto& w = watcher_registry_;

    w.add<std::string>("app/type",
                       [this]() { return std::string(process_type_name(config_.process_type)); });
    w.add("app/name", config_.process_name);
    w.add<double>("app/uptime_seconds", [this]() { return uptime_seconds(); });
    w.add<uint64_t>("app/game_time", [this]() { return game_time(); });
    w.add("app/update_hertz", config_.update_hertz);

    w.add<double>("tick/duration_ms",
                  [this]()
                  {
                      using namespace std::chrono;
                      return duration_cast<Seconds>(tick_stats_.last_duration).count() * 1000.0;
                  });
    w.add<double>("tick/max_duration_ms",
                  [this]()
                  {
                      using namespace std::chrono;
                      return duration_cast<Seconds>(tick_stats_.max_duration).count() * 1000.0;
                  });
    w.add("tick/slow_count", tick_stats_.slow_count);
    w.add<uint64_t>("tick/total_count", [this]() { return game_time(); });

    // Register all ServerAppOption instances
    ServerAppOption<int>::register_all(w);  // template parameter ignored; uses global list
}

// ============================================================================
// Private: tick driver
// ============================================================================

void ServerApp::advance_time()
{
    auto now = Clock::now();
    auto actual_duration = now - last_tick_time_;
    last_tick_time_ = now;

    // Slow-tick detection: actual elapsed > 2x the expected interval
    auto expected = std::chrono::duration_cast<Duration>(
        std::chrono::duration<double>(1.0 / config_.update_hertz));

    if (actual_duration > expected * 2)
    {
        using namespace std::chrono;
        double actual_ms = duration_cast<Seconds>(actual_duration).count() * 1000.0;
        double expected_ms = duration_cast<Seconds>(expected).count() * 1000.0;
        ATLAS_LOG_WARNING("Slow tick: {:.1f}ms (expected {:.1f}ms)", actual_ms, expected_ms);
        ++tick_stats_.slow_count;
    }

    tick_stats_.last_duration = actual_duration;
    if (actual_duration > tick_stats_.max_duration)
        tick_stats_.max_duration = actual_duration;

    // Advance game clock by fixed step (deterministic)
    game_clock_.tick(expected);

    // Pump machined heartbeat
    if (config_.process_type != ProcessType::Machined)
    {
        machined_client_.tick();
    }

    // Tick hooks
    on_end_of_tick();
    on_start_of_tick();
    updatables_.call();
    on_tick_complete();
}

// ============================================================================
// Private: file descriptor / handle limit
// ============================================================================

void ServerApp::raise_fd_limit()
{
#if defined(_WIN32)
    _setmaxstdio(8192);
#elif defined(__linux__)
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0)
    {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
#endif
}

}  // namespace atlas
