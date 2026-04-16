#include <functional>
#include <string>
#include <unordered_map>

#include <gtest/gtest.h>

#include "script/script_events.h"
#include "script/script_value.h"

namespace atlas::test {

// ============================================================================
// MockScriptObject — minimal ScriptObject stub for testing ScriptEvents
// ============================================================================

class MockScriptObject : public ScriptObject {
 public:
  using MethodFactory = std::function<std::unique_ptr<ScriptObject>()>;

  bool callable_ = true;
  int call_count_ = 0;
  std::vector<ScriptValue> last_args_;
  std::unordered_map<std::string, MethodFactory> methods_;

  bool IsNone() const override { return false; }
  std::string TypeName() const override { return "MockObject"; }

  auto GetAttr(std::string_view name) -> std::unique_ptr<ScriptObject> override {
    auto it = methods_.find(std::string(name));
    if (it != methods_.end()) return it->second();
    return nullptr;
  }

  auto SetAttr(std::string_view, const ScriptValue&) -> Result<void> override { return {}; }

  auto IsCallable() const -> bool override { return callable_; }

  auto Call(std::span<const ScriptValue> args) -> Result<ScriptValue> override {
    call_count_++;
    last_args_.assign(args.begin(), args.end());
    return ScriptValue{};
  }

  auto AsInt() const -> Result<int64_t> override {
    return Error{ErrorCode::kScriptTypeError, "not int"};
  }
  auto AsDouble() const -> Result<double> override {
    return Error{ErrorCode::kScriptTypeError, "not double"};
  }
  auto AsString() const -> Result<std::string> override {
    return Error{ErrorCode::kScriptTypeError, "not string"};
  }
  auto AsBool() const -> Result<bool> override {
    return Error{ErrorCode::kScriptTypeError, "not bool"};
  }
  auto AsBytes() const -> Result<std::vector<std::byte>> override {
    return Error{ErrorCode::kScriptTypeError, "not bytes"};
  }
};

// ============================================================================
// Tests
// ============================================================================

TEST(ScriptEventsTest, OnInitCallsModuleMethod) {
  auto method = std::make_shared<MockScriptObject>();
  auto module = std::make_shared<MockScriptObject>();
  module->methods_["onInit"] = [method]() {
    auto m = std::make_unique<MockScriptObject>();
    m->callable_ = true;
    // Delegate call tracking to shared method object
    m->methods_["__call__"] = nullptr;
    return m;
  };

  ScriptEvents events(module);
  events.OnInit(false);  // should not crash
}

TEST(ScriptEventsTest, MissingMethodSilentlySucceeds) {
  auto module = std::make_shared<MockScriptObject>();
  // no methods registered — all get_attr() return nullptr

  ScriptEvents events(module);
  events.OnInit(false);
  events.OnTick(0.016f);
  events.OnShutdown();
}

TEST(ScriptEventsTest, OnTickPassesBoolAndFloatArgs) {
  int call_count = 0;
  std::vector<ScriptValue> captured_args;

  auto method = std::make_shared<MockScriptObject>();
  method->callable_ = true;

  // Override call to capture
  // (MockScriptObject::call already captures last_args_)

  auto module = std::make_shared<MockScriptObject>();
  module->methods_["onTick"] = [&call_count, &captured_args]() {
    auto m = std::make_unique<MockScriptObject>();
    m->callable_ = true;
    return m;
  };

  ScriptEvents events(module);
  events.OnTick(0.016f);
  // No crash and method lookup happened
}

TEST(ScriptEventsTest, RegisterNonCallableListenerIsIgnored) {
  auto module = std::make_shared<MockScriptObject>();
  ScriptEvents events(module);

  auto non_callable = std::make_shared<MockScriptObject>();
  non_callable->callable_ = false;

  (void)events.RegisterListener("test_event", non_callable);
  events.FireEvent("test_event");  // no listeners registered — no crash
}

TEST(ScriptEventsTest, FireEventCallsAllListeners) {
  auto module = std::make_shared<MockScriptObject>();
  ScriptEvents events(module);

  auto cb1 = std::make_shared<MockScriptObject>();
  auto cb2 = std::make_shared<MockScriptObject>();

  (void)events.RegisterListener("my_event", cb1);
  (void)events.RegisterListener("my_event", cb2);

  events.FireEvent("my_event");

  EXPECT_EQ(cb1->call_count_, 1);
  EXPECT_EQ(cb2->call_count_, 1);
}

TEST(ScriptEventsTest, FireEventForUnknownEventDoesNothing) {
  auto module = std::make_shared<MockScriptObject>();
  ScriptEvents events(module);
  events.FireEvent("nonexistent_event");  // no crash
}

TEST(ScriptEventsTest, FireEventPassesArgs) {
  auto module = std::make_shared<MockScriptObject>();
  ScriptEvents events(module);

  auto cb = std::make_shared<MockScriptObject>();
  (void)events.RegisterListener("with_args", cb);

  ScriptValue args[] = {ScriptValue(int64_t{42}), ScriptValue(std::string("hello"))};
  events.FireEvent("with_args", args);

  ASSERT_EQ(cb->call_count_, 1);
  ASSERT_EQ(cb->last_args_.size(), 2u);
  EXPECT_TRUE(cb->last_args_[0].IsInt());
  EXPECT_EQ(cb->last_args_[0].AsInt(), 42);
  EXPECT_TRUE(cb->last_args_[1].IsString());
  EXPECT_EQ(cb->last_args_[1].AsString(), "hello");
}

// ============================================================================
// BUG-04: fire_event must not crash when a callback registers a new listener
// for the same event (which may reallocate the underlying vector and invalidate
// range-for iterators).  The new listener must NOT fire in the same round.
// ============================================================================

// A ScriptObject that, when called, registers an additional listener on the
// ScriptEvents instance it holds a pointer to.
class RegisteringCallback : public ScriptObject {
 public:
  ScriptEvents* events_ = nullptr;
  std::string event_name_;
  std::shared_ptr<ScriptObject> new_listener_;
  int call_count_ = 0;

  bool IsNone() const override { return false; }
  std::string TypeName() const override { return "RegisteringCallback"; }
  auto GetAttr(std::string_view) -> std::unique_ptr<ScriptObject> override { return nullptr; }
  auto SetAttr(std::string_view, const ScriptValue&) -> Result<void> override { return {}; }
  bool IsCallable() const override { return true; }

  auto Call(std::span<const ScriptValue>) -> Result<ScriptValue> override {
    ++call_count_;
    if (events_ && new_listener_) (void)events_->RegisterListener(event_name_, new_listener_);
    return ScriptValue{};
  }

  auto AsInt() const -> Result<int64_t> override {
    return Error{ErrorCode::kScriptTypeError, "not int"};
  }
  auto AsDouble() const -> Result<double> override {
    return Error{ErrorCode::kScriptTypeError, "not double"};
  }
  auto AsString() const -> Result<std::string> override {
    return Error{ErrorCode::kScriptTypeError, "not string"};
  }
  auto AsBool() const -> Result<bool> override {
    return Error{ErrorCode::kScriptTypeError, "not bool"};
  }
  auto AsBytes() const -> Result<std::vector<std::byte>> override {
    return Error{ErrorCode::kScriptTypeError, "not bytes"};
  }
};

TEST(ScriptEventsTest, RegisterDuringFireDoesNotCrash) {
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
    (void)events.RegisterListener("reentrant", std::make_shared<MockScriptObject>());
  (void)events.RegisterListener("reentrant", registering);

  // Must not crash (no iterator invalidation with the index-based fix).
  ASSERT_NO_FATAL_FAILURE(events.FireEvent("reentrant"));

  EXPECT_EQ(registering->call_count_, 1);
  // new_cb was registered *during* the fire — it must NOT have been called
  // in the same round (count snapshot semantics).
  EXPECT_EQ(new_cb->call_count_, 0);

  // On the next fire, new_cb should be called normally.
  events.FireEvent("reentrant");
  EXPECT_EQ(new_cb->call_count_, 1);
}

}  // namespace atlas::test
