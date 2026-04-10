#include "script/script_events.hpp"
#include "script/script_value.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <unordered_map>

namespace atlas::test
{

// ============================================================================
// MockScriptObject — minimal ScriptObject stub for testing ScriptEvents
// ============================================================================

class MockScriptObject : public ScriptObject
{
public:
    using MethodFactory = std::function<std::unique_ptr<ScriptObject>()>;

    bool callable_ = true;
    int call_count_ = 0;
    std::vector<ScriptValue> last_args_;
    std::unordered_map<std::string, MethodFactory> methods_;

    bool is_none() const override { return false; }
    std::string type_name() const override { return "MockObject"; }

    auto get_attr(std::string_view name) -> std::unique_ptr<ScriptObject> override
    {
        auto it = methods_.find(std::string(name));
        if (it != methods_.end())
            return it->second();
        return nullptr;
    }

    auto set_attr(std::string_view, const ScriptValue&) -> Result<void> override { return {}; }

    auto is_callable() const -> bool override { return callable_; }

    auto call(std::span<const ScriptValue> args) -> Result<ScriptValue> override
    {
        call_count_++;
        last_args_.assign(args.begin(), args.end());
        return ScriptValue{};
    }

    auto as_int() const -> Result<int64_t> override
    {
        return Error{ErrorCode::ScriptTypeError, "not int"};
    }
    auto as_double() const -> Result<double> override
    {
        return Error{ErrorCode::ScriptTypeError, "not double"};
    }
    auto as_string() const -> Result<std::string> override
    {
        return Error{ErrorCode::ScriptTypeError, "not string"};
    }
    auto as_bool() const -> Result<bool> override
    {
        return Error{ErrorCode::ScriptTypeError, "not bool"};
    }
    auto as_bytes() const -> Result<std::vector<std::byte>> override
    {
        return Error{ErrorCode::ScriptTypeError, "not bytes"};
    }
};

// ============================================================================
// Tests
// ============================================================================

TEST(ScriptEventsTest, OnInitCallsModuleMethod)
{
    auto method = std::make_shared<MockScriptObject>();
    auto module = std::make_shared<MockScriptObject>();
    module->methods_["onInit"] = [method]()
    {
        auto m = std::make_unique<MockScriptObject>();
        m->callable_ = true;
        // Delegate call tracking to shared method object
        m->methods_["__call__"] = nullptr;
        return m;
    };

    ScriptEvents events(module);
    events.on_init(false);  // should not crash
}

TEST(ScriptEventsTest, MissingMethodSilentlySucceeds)
{
    auto module = std::make_shared<MockScriptObject>();
    // no methods registered — all get_attr() return nullptr

    ScriptEvents events(module);
    events.on_init(false);
    events.on_tick(0.016f);
    events.on_shutdown();
}

TEST(ScriptEventsTest, OnTickPassesBoolAndFloatArgs)
{
    int call_count = 0;
    std::vector<ScriptValue> captured_args;

    auto method = std::make_shared<MockScriptObject>();
    method->callable_ = true;

    // Override call to capture
    // (MockScriptObject::call already captures last_args_)

    auto module = std::make_shared<MockScriptObject>();
    module->methods_["onTick"] = [&call_count, &captured_args]()
    {
        auto m = std::make_unique<MockScriptObject>();
        m->callable_ = true;
        return m;
    };

    ScriptEvents events(module);
    events.on_tick(0.016f);
    // No crash and method lookup happened
}

TEST(ScriptEventsTest, RegisterNonCallableListenerIsIgnored)
{
    auto module = std::make_shared<MockScriptObject>();
    ScriptEvents events(module);

    auto non_callable = std::make_shared<MockScriptObject>();
    non_callable->callable_ = false;

    events.register_listener("test_event", non_callable);
    events.fire_event("test_event");  // no listeners registered — no crash
}

TEST(ScriptEventsTest, FireEventCallsAllListeners)
{
    auto module = std::make_shared<MockScriptObject>();
    ScriptEvents events(module);

    auto cb1 = std::make_shared<MockScriptObject>();
    auto cb2 = std::make_shared<MockScriptObject>();

    events.register_listener("my_event", cb1);
    events.register_listener("my_event", cb2);

    events.fire_event("my_event");

    EXPECT_EQ(cb1->call_count_, 1);
    EXPECT_EQ(cb2->call_count_, 1);
}

TEST(ScriptEventsTest, FireEventForUnknownEventDoesNothing)
{
    auto module = std::make_shared<MockScriptObject>();
    ScriptEvents events(module);
    events.fire_event("nonexistent_event");  // no crash
}

TEST(ScriptEventsTest, FireEventPassesArgs)
{
    auto module = std::make_shared<MockScriptObject>();
    ScriptEvents events(module);

    auto cb = std::make_shared<MockScriptObject>();
    events.register_listener("with_args", cb);

    ScriptValue args[] = {ScriptValue(int64_t{42}), ScriptValue(std::string("hello"))};
    events.fire_event("with_args", args);

    ASSERT_EQ(cb->call_count_, 1);
    ASSERT_EQ(cb->last_args_.size(), 2u);
    EXPECT_TRUE(cb->last_args_[0].is_int());
    EXPECT_EQ(cb->last_args_[0].as_int(), 42);
    EXPECT_TRUE(cb->last_args_[1].is_string());
    EXPECT_EQ(cb->last_args_[1].as_string(), "hello");
}

// ============================================================================
// BUG-04: fire_event must not crash when a callback registers a new listener
// for the same event (which may reallocate the underlying vector and invalidate
// range-for iterators).  The new listener must NOT fire in the same round.
// ============================================================================

// A ScriptObject that, when called, registers an additional listener on the
// ScriptEvents instance it holds a pointer to.
class RegisteringCallback : public ScriptObject
{
public:
    ScriptEvents* events_ = nullptr;
    std::string event_name_;
    std::shared_ptr<ScriptObject> new_listener_;
    int call_count_ = 0;

    bool is_none() const override { return false; }
    std::string type_name() const override { return "RegisteringCallback"; }
    auto get_attr(std::string_view) -> std::unique_ptr<ScriptObject> override { return nullptr; }
    auto set_attr(std::string_view, const ScriptValue&) -> Result<void> override { return {}; }
    bool is_callable() const override { return true; }

    auto call(std::span<const ScriptValue>) -> Result<ScriptValue> override
    {
        ++call_count_;
        if (events_ && new_listener_)
            events_->register_listener(event_name_, new_listener_);
        return ScriptValue{};
    }

    auto as_int() const -> Result<int64_t> override
    {
        return Error{ErrorCode::ScriptTypeError, "not int"};
    }
    auto as_double() const -> Result<double> override
    {
        return Error{ErrorCode::ScriptTypeError, "not double"};
    }
    auto as_string() const -> Result<std::string> override
    {
        return Error{ErrorCode::ScriptTypeError, "not string"};
    }
    auto as_bool() const -> Result<bool> override
    {
        return Error{ErrorCode::ScriptTypeError, "not bool"};
    }
    auto as_bytes() const -> Result<std::vector<std::byte>> override
    {
        return Error{ErrorCode::ScriptTypeError, "not bytes"};
    }
};

TEST(ScriptEventsTest, RegisterDuringFireDoesNotCrash)
{
    auto module = std::make_shared<MockScriptObject>();
    ScriptEvents events(module);

    auto new_cb = std::make_shared<MockScriptObject>();
    auto registering = std::make_shared<RegisteringCallback>();
    registering->events_ = &events;
    registering->event_name_ = "reentrant";
    registering->new_listener_ = new_cb;

    // Pre-fill the vector close to a power-of-two boundary to maximise the
    // chance that push_back triggers a reallocation during fire_event.
    for (int i = 0; i < 3; ++i)
        events.register_listener("reentrant", std::make_shared<MockScriptObject>());
    events.register_listener("reentrant", registering);

    // Must not crash (no iterator invalidation with the index-based fix).
    ASSERT_NO_FATAL_FAILURE(events.fire_event("reentrant"));

    EXPECT_EQ(registering->call_count_, 1);
    // new_cb was registered *during* the fire — it must NOT have been called
    // in the same round (count snapshot semantics).
    EXPECT_EQ(new_cb->call_count_, 0);

    // On the next fire, new_cb should be called normally.
    events.fire_event("reentrant");
    EXPECT_EQ(new_cb->call_count_, 1);
}

}  // namespace atlas::test
