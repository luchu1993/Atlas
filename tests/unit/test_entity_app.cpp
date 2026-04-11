#include "server/entity_app.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using namespace atlas;

// ============================================================================
// Minimal concrete EntityApp for testing — bypasses real CLR via init override
// ============================================================================

class TestEntityApp : public EntityApp
{
public:
    int max_ticks{1};
    bool script_ready_called{false};

    // Watcher snapshot captured before shutdown
    uint32_t captured_bg_pending{0};
    uint32_t captured_bg_inflight{0};

    TestEntityApp(EventDispatcher& d, NetworkInterface& n) : EntityApp(d, n) {}

protected:
    // Skip real CLR; wire up just enough for the framework to function.
    auto init(int argc, char* argv[]) -> bool override
    {
        // Call ServerApp::init() directly to set up tick timer + watchers
        if (!ServerApp::init(argc, argv))
            return false;

        // Install User1 handler (mirrors EntityApp::init)
        install_signal_handler(Signal::User1, [this](Signal s) { on_signal(s); });

        return true;
    }

    void fini() override
    {
        captured_bg_pending = bg_task_manager().pending_count();
        captured_bg_inflight = bg_task_manager().in_flight_count();

        remove_signal_handler(Signal::User1);
        ServerApp::fini();
    }

    void on_tick_complete() override
    {
        if (++tick_count_ >= max_ticks)
            shutdown();
    }

    void on_script_ready() override { script_ready_called = true; }

private:
    int tick_count_{0};
};

struct EntityArgv
{
    std::vector<std::string> storage{"exe", "--type", "baseapp", "--update-hertz", "100"};
    std::vector<char*> ptrs;
    EntityArgv()
    {
        for (auto& s : storage)
            ptrs.push_back(s.data());
    }
    int argc() { return static_cast<int>(ptrs.size()); }
    char** argv() { return ptrs.data(); }
};

// ============================================================================
// Tests
// ============================================================================

TEST(EntityApp, StartsAndStops)
{
    EventDispatcher dispatcher("test");
    NetworkInterface network(dispatcher);
    TestEntityApp app(dispatcher, network);
    app.max_ticks = 2;

    EntityArgv args;
    int code = app.run_app(args.argc(), args.argv());
    EXPECT_EQ(code, 0);
}

TEST(EntityApp, BgTaskManagerAccessible)
{
    EventDispatcher dispatcher("test");
    NetworkInterface network(dispatcher);
    TestEntityApp app(dispatcher, network);
    app.max_ticks = 1;

    EntityArgv args;
    app.run_app(args.argc(), args.argv());

    // BgTaskManager should be idle after a clean shutdown
    EXPECT_EQ(app.captured_bg_pending, 0u);
    EXPECT_EQ(app.captured_bg_inflight, 0u);
}

TEST(EntityApp, EntityDefsAccessible)
{
    EventDispatcher dispatcher("test");
    NetworkInterface network(dispatcher);
    TestEntityApp app(dispatcher, network);
    app.max_ticks = 1;

    EntityArgv args;
    app.run_app(args.argc(), args.argv());

    // Registry is global and starts empty; just confirm it's reachable.
    EXPECT_EQ(app.entity_defs().type_count(), 0u);
}

TEST(EntityApp, WatchersBgTasksRegistered)
{
    EventDispatcher dispatcher("test");
    NetworkInterface network(dispatcher);
    TestEntityApp app(dispatcher, network);
    app.max_ticks = 1;

    EntityArgv args;
    app.run_app(args.argc(), args.argv());

    EXPECT_TRUE(app.watcher_registry().get("bg_tasks/pending").has_value());
    EXPECT_TRUE(app.watcher_registry().get("bg_tasks/in_flight").has_value());
    // Inherited app watchers also present
    EXPECT_TRUE(app.watcher_registry().get("app/type").has_value());
    EXPECT_TRUE(app.watcher_registry().get("tick/total_count").has_value());
}

TEST(EntityApp, BgTaskRunsOnBackgroundThread)
{
    EventDispatcher dispatcher("test");
    NetworkInterface network(dispatcher);
    TestEntityApp app(dispatcher, network);

    std::atomic<bool> bg_ran{false};
    std::atomic<bool> main_ran{false};

    struct WorkItem : BackgroundTask
    {
        std::atomic<bool>* bg;
        std::atomic<bool>* main;

        void do_background_task() override { *bg = true; }
        void do_main_thread_task() override { *main = true; }
    };

    // Use a larger max_ticks so the bg task can complete
    app.max_ticks = 20;

    struct BgApp : TestEntityApp
    {
        std::atomic<bool>* bg_ran;
        std::atomic<bool>* main_ran;

        BgApp(EventDispatcher& d, NetworkInterface& n, std::atomic<bool>* b, std::atomic<bool>* m)
            : TestEntityApp(d, n), bg_ran(b), main_ran(m)
        {
        }

    protected:
        auto init(int argc, char* argv[]) -> bool override
        {
            if (!TestEntityApp::init(argc, argv))
                return false;

            bg_task_manager().add_task([this]() { *bg_ran = true; },
                                       [this]()
                                       {
                                           *main_ran = true;
                                           max_ticks = 1;  // stop after main callback fires
                                       });
            return true;
        }
    };

    BgApp bapp(dispatcher, network, &bg_ran, &main_ran);
    bapp.max_ticks = 50;

    EntityArgv args;
    bapp.run_app(args.argc(), args.argv());

    EXPECT_TRUE(bg_ran.load());
    EXPECT_TRUE(main_ran.load());
}

TEST(EntityApp, ProcessTypeBaseApp)
{
    EventDispatcher dispatcher("test");
    NetworkInterface network(dispatcher);
    TestEntityApp app(dispatcher, network);
    app.max_ticks = 1;

    EntityArgv args;
    app.run_app(args.argc(), args.argv());

    EXPECT_EQ(app.config().process_type, ProcessType::BaseApp);
}
