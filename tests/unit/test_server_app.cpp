#include "server/common_messages.hpp"
#include "server/server_app.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <vector>

using namespace atlas;

// ============================================================================
// Test helpers
// ============================================================================

// Minimal concrete ServerApp — no network binding needed for unit tests.
// Overrides init() to skip port binding and shorten the tick interval.
class TestServerApp : public ServerApp
{
public:
    TestServerApp(EventDispatcher& d, NetworkInterface& n) : ServerApp(d, n) {}

    // Lifecycle event tracking
    bool init_called{false};
    bool fini_called{false};
    bool run_called{false};

    std::vector<std::string> hook_order;

    int on_end_count{0};
    int on_start_count{0};
    int on_tick_count{0};

    int max_ticks{0};  // shutdown after this many on_tick_complete() calls

protected:
    auto init(int argc, char* argv[]) -> bool override
    {
        if (!ServerApp::init(argc, argv))
            return false;
        init_called = true;
        hook_order.push_back("init");
        return true;
    }

    void fini() override
    {
        fini_called = true;
        hook_order.push_back("fini");
        ServerApp::fini();
    }

    auto run_loop() -> bool override
    {
        run_called = true;
        hook_order.push_back("run");
        return ServerApp::run_loop();
    }

    void on_end_of_tick() override
    {
        ++on_end_count;
        hook_order.push_back("end");
    }

    void on_start_of_tick() override
    {
        ++on_start_count;
        hook_order.push_back("start");
    }

    void on_tick_complete() override
    {
        ++on_tick_count;
        hook_order.push_back("tick");
        if (max_ticks > 0 && on_tick_count >= max_ticks)
            shutdown();
    }
};

// Build a minimal argv that avoids port binding (no --machined resolution needed)
struct MinimalArgv
{
    std::vector<std::string> storage{"exe", "--type", "machined", "--update-hertz", "100"};
    std::vector<char*> ptrs;

    MinimalArgv()
    {
        for (auto& s : storage)
            ptrs.push_back(s.data());
    }

    int argc() { return static_cast<int>(ptrs.size()); }
    char** argv() { return ptrs.data(); }
};

// ============================================================================
// common_messages: static_assert already validates NetworkMessage concept.
// Basic serialise/deserialise round-trips.
// ============================================================================

TEST(CommonMessages, HeartbeatRoundTrip)
{
    msg::Heartbeat hb{42, 0.75f};

    BinaryWriter w;
    hb.serialize(w);

    BinaryReader r(w.data());
    auto result = msg::Heartbeat::deserialize(r);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->game_time, 42u);
    EXPECT_FLOAT_EQ(result->load, 0.75f);
}

TEST(CommonMessages, ShutdownRequestRoundTrip)
{
    msg::ShutdownRequest req{2};

    BinaryWriter w;
    req.serialize(w);

    BinaryReader r(w.data());
    auto result = msg::ShutdownRequest::deserialize(r);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->reason, 2u);
}

TEST(CommonMessages, HeartbeatDescriptor)
{
    const auto& desc = msg::Heartbeat::descriptor();
    EXPECT_EQ(desc.id, 100);
    EXPECT_EQ(desc.length_style, MessageLengthStyle::Fixed);
}

TEST(CommonMessages, ShutdownRequestDescriptor)
{
    const auto& desc = msg::ShutdownRequest::descriptor();
    EXPECT_EQ(desc.id, 101);
    EXPECT_EQ(desc.length_style, MessageLengthStyle::Fixed);
}

// ============================================================================
// ServerApp lifecycle
// ============================================================================

TEST(ServerApp, InitRunFiniOrder)
{
    EventDispatcher dispatcher("test");
    NetworkInterface network(dispatcher);
    TestServerApp app(dispatcher, network);
    app.max_ticks = 1;

    MinimalArgv args;
    int exit_code = app.run_app(args.argc(), args.argv());
    EXPECT_EQ(exit_code, 0);

    // init and fini must both have been called
    EXPECT_TRUE(app.init_called);
    EXPECT_TRUE(app.fini_called);
    EXPECT_TRUE(app.run_called);

    // Sequence: init → run → (ticks) → fini
    ASSERT_GE(app.hook_order.size(), 3u);
    EXPECT_EQ(app.hook_order.front(), "init");
    EXPECT_EQ(app.hook_order.back(), "fini");
}

TEST(ServerApp, TickHookOrder)
{
    EventDispatcher dispatcher("test");
    NetworkInterface network(dispatcher);
    TestServerApp app(dispatcher, network);
    app.max_ticks = 1;

    MinimalArgv args;
    app.run_app(args.argc(), args.argv());

    // Within a tick, order must be: end → start → tick
    // Find the first occurrence of each hook after "run"
    auto it = std::find(app.hook_order.begin(), app.hook_order.end(), "run");
    ASSERT_NE(it, app.hook_order.end());

    std::vector<std::string> tick_hooks(it + 1, app.hook_order.end());
    // Remove "fini" from the end
    tick_hooks.erase(std::remove(tick_hooks.begin(), tick_hooks.end(), "fini"), tick_hooks.end());

    // Expect at least one complete cycle: end, start, tick
    ASSERT_GE(tick_hooks.size(), 3u);
    EXPECT_EQ(tick_hooks[0], "end");
    EXPECT_EQ(tick_hooks[1], "start");
    EXPECT_EQ(tick_hooks[2], "tick");
}

TEST(ServerApp, ShutdownStopsRun)
{
    EventDispatcher dispatcher("test");
    NetworkInterface network(dispatcher);
    TestServerApp app(dispatcher, network);
    app.max_ticks = 3;

    MinimalArgv args;
    int exit_code = app.run_app(args.argc(), args.argv());
    EXPECT_EQ(exit_code, 0);
    EXPECT_EQ(app.on_tick_count, 3);
}

TEST(ServerApp, UpdatableCalledEachTick)
{
    EventDispatcher dispatcher("test");
    NetworkInterface network(dispatcher);
    TestServerApp app(dispatcher, network);
    app.max_ticks = 3;

    struct Counter : Updatable
    {
        int count{0};
        void update() override { ++count; }
    } counter;

    // Register updatable before run_app — init() will add the tick timer,
    // but we can register the updatable before or after init.
    // We'll do it by subclassing the init hook.
    // For simplicity, register directly before run_app:
    // The updatable needs to be registered on the app. We expose it via a
    // wrapper that calls register_for_update from within init.
    struct CounterApp : TestServerApp
    {
        Counter* counter_ptr{nullptr};

        CounterApp(EventDispatcher& d, NetworkInterface& n) : TestServerApp(d, n) {}

        auto init(int argc, char* argv[]) -> bool override
        {
            if (!TestServerApp::init(argc, argv))
                return false;
            register_for_update(counter_ptr);
            return true;
        }
    };

    CounterApp capp(dispatcher, network);
    capp.counter_ptr = &counter;
    capp.max_ticks = 3;

    MinimalArgv args;
    capp.run_app(args.argc(), args.argv());

    EXPECT_EQ(counter.count, 3);
}

TEST(ServerApp, GameTimeProvidesMonotonicallyIncreasingFrameCount)
{
    EventDispatcher dispatcher("test");
    NetworkInterface network(dispatcher);
    TestServerApp app(dispatcher, network);
    app.max_ticks = 5;

    MinimalArgv args;
    app.run_app(args.argc(), args.argv());

    EXPECT_GE(app.game_time(), 5u);
}

TEST(ServerApp, WatcherRegisteredAfterInit)
{
    EventDispatcher dispatcher("test");
    NetworkInterface network(dispatcher);
    TestServerApp app(dispatcher, network);
    app.max_ticks = 1;

    MinimalArgv args;
    app.run_app(args.argc(), args.argv());

    // Standard watchers must be present
    EXPECT_TRUE(app.watcher_registry().get("app/type").has_value());
    EXPECT_TRUE(app.watcher_registry().get("app/name").has_value());
    EXPECT_TRUE(app.watcher_registry().get("tick/slow_count").has_value());
    EXPECT_TRUE(app.watcher_registry().get("tick/total_count").has_value());
}

TEST(ServerApp, ConfigLoadedFromArgs)
{
    EventDispatcher dispatcher("test");
    NetworkInterface network(dispatcher);
    TestServerApp app(dispatcher, network);
    app.max_ticks = 1;

    std::vector<std::string> storage{"exe",       "--type",         "machined", "--name",
                                     "test_proc", "--update-hertz", "50"};
    std::vector<char*> ptrs;
    for (auto& s : storage)
        ptrs.push_back(s.data());

    app.run_app(static_cast<int>(ptrs.size()), ptrs.data());

    EXPECT_EQ(app.config().process_type, ProcessType::Machined);
    EXPECT_EQ(app.config().process_name, "test_proc");
    EXPECT_EQ(app.config().update_hertz, 50);
}
