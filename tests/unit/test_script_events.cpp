#include <gtest/gtest.h>
#include "script/script_events.hpp"
#include "pyscript/py_interpreter.hpp"

using namespace atlas;

class ScriptEventsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!PyInterpreter::is_initialized())
            (void)PyInterpreter::initialize();
    }
};

TEST_F(ScriptEventsTest, OnInitCallsModuleMethod)
{
    // Create a Python module with onInit
    (void)PyInterpreter::exec(
        "import types\n"
        "test_personality = types.ModuleType('test_personality')\n"
        "test_personality.init_called = False\n"
        "test_personality.init_reload = None\n"
        "def _onInit(is_reload):\n"
        "    test_personality.init_called = True\n"
        "    test_personality.init_reload = is_reload\n"
        "test_personality.onInit = _onInit\n"
    );

    auto mod = PyInterpreter::import("__main__");
    ASSERT_TRUE(mod.has_value());
    auto personality = mod->get_attr("test_personality");
    ASSERT_TRUE(static_cast<bool>(personality));

    ScriptEvents events(std::move(personality));
    events.on_init(false);

    // Verify Python callback was invoked
    auto result = PyInterpreter::exec("assert test_personality.init_called == True");
    EXPECT_TRUE(result.has_value()) << result.error().message();
}

TEST_F(ScriptEventsTest, OnTickCallsModuleMethod)
{
    (void)PyInterpreter::exec(
        "import types\n"
        "test_tick = types.ModuleType('test_tick')\n"
        "test_tick.dt_value = 0.0\n"
        "def _onTick(dt):\n"
        "    test_tick.dt_value = dt\n"
        "test_tick.onTick = _onTick\n"
    );

    auto mod = PyInterpreter::import("__main__");
    auto personality = mod->get_attr("test_tick");

    ScriptEvents events(std::move(personality));
    events.on_tick(0.016f);

    auto result = PyInterpreter::exec("assert abs(test_tick.dt_value - 0.016) < 0.001");
    EXPECT_TRUE(result.has_value()) << result.error().message();
}

TEST_F(ScriptEventsTest, MissingMethodDoesNotCrash)
{
    // Module with no onInit/onTick/onShutdown — should silently succeed
    (void)PyInterpreter::exec(
        "import types\n"
        "empty_mod = types.ModuleType('empty_mod')\n"
    );

    auto mod = PyInterpreter::import("__main__");
    auto personality = mod->get_attr("empty_mod");

    ScriptEvents events(std::move(personality));
    events.on_init(false);    // no crash
    events.on_tick(0.016f);   // no crash
    events.on_shutdown();     // no crash
}

TEST_F(ScriptEventsTest, CustomEventFiresListeners)
{
    (void)PyInterpreter::exec(
        "import types\n"
        "event_mod = types.ModuleType('event_mod')\n"
        "event_mod.received = []\n"
    );

    auto mod = PyInterpreter::import("__main__");
    auto personality = mod->get_attr("event_mod");

    ScriptEvents events(std::move(personality));

    // Register a Python callback
    (void)PyInterpreter::exec(
        "def _on_custom():\n"
        "    event_mod.received.append('fired')\n"
    );
    auto callback = mod->get_attr("_on_custom");
    ASSERT_TRUE(callback.is_callable());

    events.register_listener("custom_event", std::move(callback));
    events.fire_event("custom_event");

    auto result = PyInterpreter::exec("assert len(event_mod.received) == 1");
    EXPECT_TRUE(result.has_value()) << result.error().message();
}

// ============================================================================
// Review fix: register_listener rejects non-callable objects
// ============================================================================

TEST_F(ScriptEventsTest, RegisterNonCallableListenerIsIgnored)
{
    (void)PyInterpreter::exec(
        "import types\n"
        "noncall_mod = types.ModuleType('noncall_mod')\n"
        "noncall_mod.count = 0\n"
    );

    auto mod = PyInterpreter::import("__main__");
    auto personality = mod->get_attr("noncall_mod");

    ScriptEvents events(std::move(personality));

    // Register a non-callable (an int) — should be silently rejected
    auto non_callable = PyObjectPtr(PyLong_FromLong(42));
    events.register_listener("test_event", std::move(non_callable));

    // Fire event — should not crash (no listeners were actually registered)
    events.fire_event("test_event");
}

// ============================================================================
// Review fix: on_tick passes float correctly via PyTuple_Pack (not release())
// ============================================================================

TEST_F(ScriptEventsTest, OnTickPassesFloatAccurately)
{
    (void)PyInterpreter::exec(
        "import types\n"
        "tick_mod2 = types.ModuleType('tick_mod2')\n"
        "tick_mod2.values = []\n"
        "def _onTick2(dt):\n"
        "    tick_mod2.values.append(dt)\n"
        "tick_mod2.onTick = _onTick2\n"
    );

    auto mod = PyInterpreter::import("__main__");
    auto personality = mod->get_attr("tick_mod2");

    ScriptEvents events(std::move(personality));

    // Call on_tick multiple times with different values
    events.on_tick(0.016f);
    events.on_tick(0.033f);
    events.on_tick(0.050f);

    auto result = PyInterpreter::exec(
        "assert len(tick_mod2.values) == 3\n"
        "assert abs(tick_mod2.values[0] - 0.016) < 0.001\n"
        "assert abs(tick_mod2.values[1] - 0.033) < 0.001\n"
        "assert abs(tick_mod2.values[2] - 0.050) < 0.001\n"
    );
    EXPECT_TRUE(result.has_value()) << result.error().message();
}
