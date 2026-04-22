#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "server/manager_app.h"
#include "server/script_app.h"

using namespace atlas;

// ============================================================================
// Mock ScriptEngine — no CLR, tracks lifecycle calls
// ============================================================================

class MockScriptEngine : public ScriptEngine {
 public:
  bool initialized{false};
  bool finalized{false};
  int on_init_count{0};
  int on_tick_count{0};
  int on_shutdown_count{0};
  bool last_is_reload{false};
  float last_dt{0.0f};
  bool fail_initialize{false};

  auto Initialize() -> Result<void> override {
    if (fail_initialize) return Error{ErrorCode::kInternalError, "mock init failure"};
    initialized = true;
    return {};
  }

  void Finalize() override { finalized = true; }

  auto LoadModule(const std::filesystem::path&) -> Result<void> override { return {}; }

  void OnTick(float dt) override {
    ++on_tick_count;
    last_dt = dt;
  }

  void OnInit(bool is_reload) override {
    ++on_init_count;
    last_is_reload = is_reload;
  }

  void OnShutdown() override { ++on_shutdown_count; }

  auto CallFunction(std::string_view, std::string_view, std::span<const ScriptValue>)
      -> Result<ScriptValue> override {
    return ScriptValue{};
  }

  auto RuntimeName() const -> std::string_view override { return "Mock"; }
};

// ============================================================================
// Mock INativeApiProvider
// ============================================================================

class MockNativeProvider : public INativeApiProvider {
 public:
  bool created{false};
  MockNativeProvider() { created = true; }

  void LogMessage(int32_t, const char*, int32_t) override {}
  double ServerTime() override { return 0.0; }
  float DeltaTime() override { return 0.0f; }
  uint8_t GetProcessPrefix() override { return 0; }
  void SendClientRpc(uint32_t, uint32_t, const std::byte*, int32_t) override {}
  void SendCellRpc(uint32_t, uint32_t, const std::byte*, int32_t) override {}
  void SendBaseRpc(uint32_t, uint32_t, const std::byte*, int32_t) override {}
  void RegisterEntityType(const std::byte*, int32_t) override {}
  void UnregisterAllEntityTypes() override {}
  void WriteToDb(uint32_t, const std::byte*, int32_t) override {}
  void GiveClientTo(uint32_t, uint32_t) override {}
  auto CreateBaseEntity(uint16_t, uint32_t) -> uint32_t override { return 0; }
  void SetAoIRadius(uint32_t, float, float) override {}
  void SetNativeCallbacks(const void*, int32_t) override {}

  // Phase 10 CellApp-specific stubs — non-CellApp test mocks just need
  // to stay concrete as INativeApiProvider's surface grows.
  void SetEntityPosition(uint32_t, float, float, float) override {}
  void PublishReplicationFrame(uint32_t, uint64_t, uint64_t, const std::byte*, int32_t,
                               const std::byte*, int32_t, const std::byte*, int32_t,
                               const std::byte*, int32_t) override {}
  auto AddMoveController(uint32_t, float, float, float, float, int32_t) -> int32_t override {
    return 0;
  }
  auto AddTimerController(uint32_t, float, bool, int32_t) -> int32_t override { return 0; }
  auto AddProximityController(uint32_t, float, int32_t) -> int32_t override { return 0; }
  void CancelController(uint32_t, int32_t) override {}
};

// ============================================================================
// TestScriptApp — injects mock engine, skips real CLR
// ============================================================================

class TestScriptApp : public ScriptApp {
 public:
  MockScriptEngine* mock_engine{nullptr};
  MockNativeProvider* mock_provider{nullptr};
  bool script_ready_called{false};
  int max_ticks{1};

  // Snapshot captured in Fini() before the engine is destroyed.
  int captured_on_init_count{0};
  int captured_on_tick_count{0};
  int captured_on_shutdown_count{0};
  bool captured_finalized{false};
  bool captured_last_is_reload{false};

  TestScriptApp(EventDispatcher& d, NetworkInterface& n) : ScriptApp(d, n) {}

 protected:
  auto CreateNativeProvider() -> std::unique_ptr<INativeApiProvider> override {
    auto p = std::make_unique<MockNativeProvider>();
    mock_provider = p.get();
    return p;
  }

  // Override Init to inject the mock engine instead of ClrScriptEngine.
  auto Init(int argc, char* argv[]) -> bool override {
    // Call ServerApp::Init() directly (skip ScriptApp::Init() real CLR path)
    if (!ServerApp::Init(argc, argv)) return false;

    // Inject provider
    auto provider = CreateNativeProvider();
    SetNativeApiProvider(provider.get());
    // Keep ownership via a captured unique_ptr in the lambda — store it locally.
    // For test purposes we just leak it (process-lifetime).
    provider.release();

    // Inject mock engine
    auto engine = std::make_unique<MockScriptEngine>();
    mock_engine = engine.get();

    if (mock_engine->fail_initialize) {
      // Return false as real Init() would
      return false;
    }

    mock_engine->initialized = true;
    mock_engine->OnInit(false);

    // Transfer ownership via the protected accessor: we need to expose
    // script_engine_ — instead, store it and expose via OnTickComplete.
    injected_engine_ = std::move(engine);

    OnScriptReady();
    return true;
  }

  void Fini() override {
    if (injected_engine_) {
      injected_engine_->OnShutdown();
      injected_engine_->Finalize();
      // Capture before destroying
      captured_on_init_count = injected_engine_->on_init_count;
      captured_on_tick_count = injected_engine_->on_tick_count;
      captured_on_shutdown_count = injected_engine_->on_shutdown_count;
      captured_finalized = injected_engine_->finalized;
      captured_last_is_reload = injected_engine_->last_is_reload;
      injected_engine_.reset();
    }
    SetNativeApiProvider(nullptr);
    ServerApp::Fini();
  }

  void OnTickComplete() override {
    if (injected_engine_) injected_engine_->OnTick(0.016f);

    if (++tick_count_ >= max_ticks) Shutdown();
  }

  void OnScriptReady() override { script_ready_called = true; }

 private:
  std::unique_ptr<MockScriptEngine> injected_engine_;
  int tick_count_{0};
};

// Minimal argv (no CLR paths needed)
struct ScriptArgv {
  std::vector<std::string> storage{"exe", "--type", "baseapp", "--update-hertz", "100"};
  std::vector<char*> ptrs;
  ScriptArgv() {
    for (auto& s : storage) ptrs.push_back(s.data());
  }
  int argc() { return static_cast<int>(ptrs.size()); }
  char** argv() { return ptrs.data(); }
};

// ============================================================================
// ManagerApp tests (lightweight — just verifies it compiles and runs)
// ============================================================================

class TestManagerApp : public ManagerApp {
 public:
  int max_ticks{1};
  TestManagerApp(EventDispatcher& d, NetworkInterface& n) : ManagerApp(d, n) {}

 protected:
  void OnTickComplete() override {
    if (++tick_count_ >= max_ticks) Shutdown();
  }

 private:
  int tick_count_{0};
};

TEST(ManagerApp, StartsAndStops) {
  EventDispatcher dispatcher("test");
  NetworkInterface network(dispatcher);
  TestManagerApp app(dispatcher, network);
  app.max_ticks = 2;

  std::vector<std::string> s{"exe", "--type", "baseappmgr", "--update-hertz", "100"};
  std::vector<char*> p;
  for (auto& a : s) p.push_back(a.data());
  int code = app.RunApp(static_cast<int>(p.size()), p.data());
  EXPECT_EQ(code, 0);
}

TEST(ManagerApp, WatchersPresent) {
  EventDispatcher dispatcher("test");
  NetworkInterface network(dispatcher);
  TestManagerApp app(dispatcher, network);
  app.max_ticks = 1;

  std::vector<std::string> s{"exe", "--type", "cellappmgr", "--update-hertz", "100"};
  std::vector<char*> p;
  for (auto& a : s) p.push_back(a.data());
  app.RunApp(static_cast<int>(p.size()), p.data());

  EXPECT_TRUE(app.GetWatcherRegistry().Get("app/type").has_value());
  EXPECT_EQ(app.GetWatcherRegistry().Get("app/type").value_or(""), "cellappmgr");
}

// ============================================================================
// ScriptApp tests
// ============================================================================

TEST(ScriptApp, NativeProviderCreated) {
  EventDispatcher dispatcher("test");
  NetworkInterface network(dispatcher);
  TestScriptApp app(dispatcher, network);

  ScriptArgv args;
  app.RunApp(args.argc(), args.argv());

  ASSERT_NE(app.mock_provider, nullptr);
  EXPECT_TRUE(app.mock_provider->created);
}

TEST(ScriptApp, ScriptEngineInitCalledOnStartup) {
  EventDispatcher dispatcher("test");
  NetworkInterface network(dispatcher);
  TestScriptApp app(dispatcher, network);

  ScriptArgv args;
  app.RunApp(args.argc(), args.argv());

  ASSERT_NE(app.mock_engine, nullptr);
  EXPECT_TRUE(app.mock_engine->initialized);
  EXPECT_EQ(app.captured_on_init_count, 1);
  EXPECT_FALSE(app.captured_last_is_reload);
}

TEST(ScriptApp, OnScriptReadyCalled) {
  EventDispatcher dispatcher("test");
  NetworkInterface network(dispatcher);
  TestScriptApp app(dispatcher, network);

  ScriptArgv args;
  app.RunApp(args.argc(), args.argv());

  EXPECT_TRUE(app.script_ready_called);
}

TEST(ScriptApp, OnTickDrivesScriptTick) {
  EventDispatcher dispatcher("test");
  NetworkInterface network(dispatcher);
  TestScriptApp app(dispatcher, network);
  app.max_ticks = 5;

  ScriptArgv args;
  app.RunApp(args.argc(), args.argv());

  EXPECT_EQ(app.captured_on_tick_count, 5);
}

TEST(ScriptApp, ScriptEngineShutdownAndFinalizeCalledOnFini) {
  EventDispatcher dispatcher("test");
  NetworkInterface network(dispatcher);
  TestScriptApp app(dispatcher, network);

  ScriptArgv args;
  app.RunApp(args.argc(), args.argv());

  EXPECT_EQ(app.captured_on_shutdown_count, 1);
  EXPECT_TRUE(app.captured_finalized);
}

TEST(ScriptApp, ProcessTypeSetCorrectly) {
  EventDispatcher dispatcher("test");
  NetworkInterface network(dispatcher);
  TestScriptApp app(dispatcher, network);

  ScriptArgv args;
  app.RunApp(args.argc(), args.argv());

  EXPECT_EQ(app.Config().process_type, ProcessType::kBaseApp);
}
