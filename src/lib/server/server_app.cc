#include "server/server_app.h"

#include <chrono>
#include <format>

#include "foundation/log.h"
#include "foundation/profiler.h"
#include "foundation/runtime.h"
#include "server/server_app_option.h"

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

namespace atlas {

// ============================================================================
// Construction / destruction
// ============================================================================

ServerApp::ServerApp(EventDispatcher& dispatcher, NetworkInterface& network)
    : dispatcher_(dispatcher), network_(network), machined_client_(dispatcher, network) {}

ServerApp::~ServerApp() = default;

// ============================================================================
// Public API
// ============================================================================

auto ServerApp::RunApp(int argc, char* argv[]) -> int {
  // 1. Load configuration
  auto cfg_result = ServerConfig::Load(argc, argv);
  if (!cfg_result) {
    ATLAS_LOG_CRITICAL("Failed to load config: {}", cfg_result.Error().Message());
    return 1;
  }
  config_ = std::move(*cfg_result);

  // Resolve the frame label once, here, so the std::string lives for the
  // remaining process lifetime. Tracy keys frames by pointer identity —
  // mutating frame_name after this point would split a single logical
  // frame into two unrelated ones in the viewer.
  if (config_.frame_name.empty()) {
    config_.frame_name = config_.process_name + ".Tick";
  }

  // 2. Configure logger
  RuntimeConfig runtime_cfg;
  runtime_cfg.log_level = config_.log_level;
  auto runtime_result = Runtime::Initialize(runtime_cfg);
  if (!runtime_result) {
    return 1;
  }

  // 3. Apply ServerAppOption values from raw config
  if (config_.raw_config) ServerAppOptionBase::ApplyAll(*config_.raw_config->Root());

  // 4. Raise fd / handle limits
  RaiseFdLimit();

  // 5. Init
  if (!Init(argc, argv)) {
    ATLAS_LOG_CRITICAL("ServerApp::init() failed");
    Runtime::Finalize();
    return 1;
  }
#if defined(_WIN32)
  int pid = _getpid();
#else
  int pid = getpid();
#endif
  ATLAS_LOG_INFO("{} started (pid={})", config_.process_name, pid);

  // 6. Run
  bool run_ok = RunLoop();

  // 7. Post-run hook
  OnRunComplete();

  // 8. Cleanup
  Fini();
  Runtime::Finalize();

  return run_ok ? 0 : 1;
}

void ServerApp::Shutdown() {
  if (!shutdown_requested_) {
    shutdown_requested_ = true;
    dispatcher_.Stop();
  }
}

auto ServerApp::GameTimeSeconds() const -> double {
  using namespace std::chrono;
  return duration_cast<Seconds>(game_clock_.Elapsed()).count();
}

auto ServerApp::UptimeSeconds() const -> double {
  using namespace std::chrono;
  auto elapsed = Clock::now() - start_time_;
  return duration_cast<Seconds>(elapsed).count();
}

auto ServerApp::RegisterForUpdate(Updatable* object, int level) -> bool {
  return updatables_.Add(object, level);
}

auto ServerApp::DeregisterForUpdate(Updatable* object) -> bool {
  return updatables_.Remove(object);
}

// ============================================================================
// Protected lifecycle
// ============================================================================

auto ServerApp::Init(int argc, char* argv[]) -> bool {
  (void)argc;
  (void)argv;

  start_time_ = Clock::now();
  last_tick_time_ = start_time_;

  // Install signal handlers — callbacks run on main thread via SignalDispatchTask
  InstallSignalHandler(Signal::kInterrupt, [this](Signal s) { OnSignal(s); });
  InstallSignalHandler(Signal::kTerminate, [this](Signal s) { OnSignal(s); });

  // Register FrequentTask so dispatch_pending_signals() is called each loop
  signal_registration_ = dispatcher_.AddFrequentTask(&signal_task_);

  // Register tick timer
  auto tick_interval = std::chrono::duration_cast<Duration>(
      std::chrono::duration<double>(1.0 / config_.update_hertz));
  tick_timer_ =
      dispatcher_.AddRepeatingTimer(tick_interval, [this](TimerHandle) { AdvanceTime(); });

  // Register Watcher entries
  RegisterWatchers();

  // Connect to machined (skip for the machined process itself)
  if (config_.process_type != ProcessType::kMachined) {
    if (machined_client_.Connect(config_.machined_address)) {
      machined_client_.SendRegister(config_);
    } else {
      ATLAS_LOG_WARNING("ServerApp: could not connect to machined at {} — continuing",
                        config_.machined_address.ToString());
    }
  }

  return true;
}

void ServerApp::Fini() {
  // Deregister from machined before shutting down
  if (config_.process_type != ProcessType::kMachined && machined_client_.IsConnected()) {
    machined_client_.SendDeregister(config_);
  }

  // Cancel the tick timer to stop new AdvanceTime() calls
  if (tick_timer_.IsValid()) dispatcher_.CancelTimer(tick_timer_);

  // Signal registration cleaned up by FrequentTaskRegistration RAII destructor

  // Remove signal handlers
  RemoveSignalHandler(Signal::kInterrupt);
  RemoveSignalHandler(Signal::kTerminate);
}

auto ServerApp::RunLoop() -> bool {
  dispatcher_.Run();
  return true;
}

void ServerApp::OnSignal(Signal sig) {
  ATLAS_LOG_INFO("Received signal {}, shutting down...", static_cast<int>(sig));
  Shutdown();
}

void ServerApp::RegisterWatchers() {
  auto& w = watcher_registry_;

  w.Add<std::string>("app/type",
                     [this]() { return std::string(ProcessTypeName(config_.process_type)); });
  w.Add("app/name", config_.process_name);
  w.Add<double>("app/uptime_seconds", [this]() { return UptimeSeconds(); });
  w.Add<uint64_t>("app/game_time", [this]() { return GameTime(); });
  w.Add("app/update_hertz", config_.update_hertz);

  w.Add<double>("tick/duration_ms", [this]() {
    using namespace std::chrono;
    return duration_cast<Seconds>(tick_stats_.last_duration).count() * 1000.0;
  });
  w.Add<double>("tick/max_duration_ms", [this]() {
    using namespace std::chrono;
    return duration_cast<Seconds>(tick_stats_.max_duration).count() * 1000.0;
  });
  w.Add("tick/slow_count", tick_stats_.slow_count);
  w.Add<uint64_t>("tick/total_count", [this]() { return GameTime(); });

  // Register all ServerAppOption instances
  ServerAppOptionBase::RegisterAll(w);
}

// ============================================================================
// Private: tick driver
// ============================================================================

void ServerApp::AdvanceTime() {
  auto now = Clock::now();
  auto actual_duration = now - last_tick_time_;
  last_tick_time_ = now;

  // Slow-tick detection: actual elapsed > 2x the expected interval
  auto expected = std::chrono::duration_cast<Duration>(
      std::chrono::duration<double>(1.0 / config_.update_hertz));

  if (actual_duration > expected * 2) {
    using namespace std::chrono;
    double actual_ms = duration_cast<Seconds>(actual_duration).count() * 1000.0;
    double expected_ms = duration_cast<Seconds>(expected).count() * 1000.0;
    ATLAS_LOG_WARNING("Slow tick: {:.1f}ms (expected {:.1f}ms)", actual_ms, expected_ms);
    ++tick_stats_.slow_count;
  }

  tick_stats_.last_duration = actual_duration;
  if (actual_duration > tick_stats_.max_duration) tick_stats_.max_duration = actual_duration;

  // Advance game clock by fixed step (deterministic)
  game_clock_.Tick(expected);

  // Pump machined heartbeat
  if (config_.process_type != ProcessType::kMachined) {
    machined_client_.Tick();
  }

  // Tick hooks — bracketed so we measure actual work time, distinct from
  // `actual_duration` which also covers the wait between timer fires.
  // Load reporting (CellApp::LastTickWorkDuration) needs work time alone.
  const auto work_start = Clock::now();
  {
    ATLAS_PROFILE_ZONE_N("Tick");
    OnEndOfTick();
    OnStartOfTick();
    {
      ATLAS_PROFILE_ZONE_N("Updatables");
      updatables_.Call();
    }
    OnTickComplete();
  }
  tick_stats_.last_work_duration = Clock::now() - work_start;

  // Plot first, frame-mark last: the plot value belongs to the frame
  // we are about to close. Tracy associates plot samples with the
  // currently-open frame, so emitting after FrameMarkNamed would slide
  // the sample into the next bucket and the timeline would lag by one.
  using namespace std::chrono;
  const double work_ms = duration_cast<Seconds>(tick_stats_.last_work_duration).count() * 1000.0;
  ATLAS_PROFILE_PLOT("TickWorkMs", work_ms);
  ATLAS_PROFILE_FRAME(config_.frame_name.c_str());
}

// ============================================================================
// Private: file descriptor / handle limit
// ============================================================================

void ServerApp::RaiseFdLimit() {
#if defined(_WIN32)
  _setmaxstdio(8192);
#elif defined(__linux__)
  struct rlimit rl;
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
  }
#endif
}

}  // namespace atlas
