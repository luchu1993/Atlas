#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "server/entity_app.h"

using namespace atlas;

// ============================================================================
// Minimal concrete EntityApp for testing — bypasses real CLR via init override
// ============================================================================

class TestEntityApp : public EntityApp {
 public:
  int max_ticks{1};
  bool script_ready_called{false};

  // Watcher snapshot captured before shutdown
  uint32_t captured_bg_pending{0};
  uint32_t captured_bg_inflight{0};

  TestEntityApp(EventDispatcher& d, NetworkInterface& n) : EntityApp(d, n) {}

 protected:
  // Skip real CLR; wire up just enough for the framework to function.
  auto Init(int argc, char* argv[]) -> bool override {
    // Call ServerApp::Init() directly to set up tick timer + watchers
    if (!ServerApp::Init(argc, argv)) return false;

    // Install User1 handler (mirrors EntityApp::Init)
    InstallSignalHandler(Signal::kUser1, [this](Signal s) { OnSignal(s); });

    return true;
  }

  void Fini() override {
    captured_bg_pending = GetBgTaskManager().PendingCount();
    captured_bg_inflight = GetBgTaskManager().InFlightCount();

    RemoveSignalHandler(Signal::kUser1);
    ServerApp::Fini();
  }

  void OnTickComplete() override {
    if (++tick_count_ >= max_ticks) Shutdown();
  }

  void OnScriptReady() override { script_ready_called = true; }

 private:
  int tick_count_{0};
};

struct EntityArgv {
  std::vector<std::string> storage{"exe", "--type", "baseapp", "--update-hertz", "100"};
  std::vector<char*> ptrs;
  EntityArgv() {
    for (auto& s : storage) ptrs.push_back(s.data());
  }
  int argc() { return static_cast<int>(ptrs.size()); }
  char** argv() { return ptrs.data(); }
};

// ============================================================================
// Tests
// ============================================================================

TEST(EntityApp, StartsAndStops) {
  EventDispatcher dispatcher("test");
  NetworkInterface network(dispatcher);
  TestEntityApp app(dispatcher, network);
  app.max_ticks = 2;

  EntityArgv args;
  int code = app.RunApp(args.argc(), args.argv());
  EXPECT_EQ(code, 0);
}

TEST(EntityApp, BgTaskManagerAccessible) {
  EventDispatcher dispatcher("test");
  NetworkInterface network(dispatcher);
  TestEntityApp app(dispatcher, network);
  app.max_ticks = 1;

  EntityArgv args;
  app.RunApp(args.argc(), args.argv());

  // BgTaskManager should be idle after a clean shutdown
  EXPECT_EQ(app.captured_bg_pending, 0u);
  EXPECT_EQ(app.captured_bg_inflight, 0u);
}

TEST(EntityApp, EntityDefsAccessible) {
  EventDispatcher dispatcher("test");
  NetworkInterface network(dispatcher);
  TestEntityApp app(dispatcher, network);
  app.max_ticks = 1;

  EntityArgv args;
  app.RunApp(args.argc(), args.argv());

  // Registry is global and starts empty; just confirm it's reachable.
  EXPECT_EQ(app.EntityDefs().TypeCount(), 0u);
}

TEST(EntityApp, WatchersBgTasksRegistered) {
  EventDispatcher dispatcher("test");
  NetworkInterface network(dispatcher);
  TestEntityApp app(dispatcher, network);
  app.max_ticks = 1;

  EntityArgv args;
  app.RunApp(args.argc(), args.argv());

  EXPECT_TRUE(app.GetWatcherRegistry().Get("bg_tasks/pending").has_value());
  EXPECT_TRUE(app.GetWatcherRegistry().Get("bg_tasks/in_flight").has_value());
  // Inherited app watchers also present
  EXPECT_TRUE(app.GetWatcherRegistry().Get("app/type").has_value());
  EXPECT_TRUE(app.GetWatcherRegistry().Get("tick/total_count").has_value());
}

TEST(EntityApp, BgTaskRunsOnBackgroundThread) {
  EventDispatcher dispatcher("test");
  NetworkInterface network(dispatcher);
  TestEntityApp app(dispatcher, network);

  std::atomic<bool> bg_ran{false};
  std::atomic<bool> main_ran{false};

  struct WorkItem : BackgroundTask {
    std::atomic<bool>* bg;
    std::atomic<bool>* main;

    void DoBackgroundTask() override { *bg = true; }
    void DoMainThreadTask() override { *main = true; }
  };

  // Use a larger max_ticks so the bg task can complete
  app.max_ticks = 20;

  struct BgApp : TestEntityApp {
    std::atomic<bool>* bg_ran;
    std::atomic<bool>* main_ran;

    BgApp(EventDispatcher& d, NetworkInterface& n, std::atomic<bool>* b, std::atomic<bool>* m)
        : TestEntityApp(d, n), bg_ran(b), main_ran(m) {}

   protected:
    auto Init(int argc, char* argv[]) -> bool override {
      if (!TestEntityApp::Init(argc, argv)) return false;

      GetBgTaskManager().AddTask([this]() { *bg_ran = true; },
                                 [this]() {
                                   *main_ran = true;
                                   max_ticks = 1;  // stop after main callback fires
                                 });
      return true;
    }
  };

  BgApp bapp(dispatcher, network, &bg_ran, &main_ran);
  bapp.max_ticks = 50;

  EntityArgv args;
  bapp.RunApp(args.argc(), args.argv());

  EXPECT_TRUE(bg_ran.load());
  EXPECT_TRUE(main_ran.load());
}

TEST(EntityApp, ProcessTypeBaseApp) {
  EventDispatcher dispatcher("test");
  NetworkInterface network(dispatcher);
  TestEntityApp app(dispatcher, network);
  app.max_ticks = 1;

  EntityArgv args;
  app.RunApp(args.argc(), args.argv());

  EXPECT_EQ(app.Config().process_type, ProcessType::kBaseApp);
}
