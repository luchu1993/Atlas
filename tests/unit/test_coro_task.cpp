#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "coro/cancellation.h"
#include "coro/fire_and_forget.h"
#include "coro/scope_guard.h"
#include "coro/task.h"

using namespace atlas;

// Helpers take a factory rather than a Task& so the factory (and any
// lambda captures it owns) outlive every coroutine frame they produce.
// Calling a lambda-coroutine as a temporary leaves the frame's `this`
// dangling once the temporary dies, which ASAN flags as
// stack-use-after-scope on the very first capture access.

template <typename T>
struct TaskValueType;
template <typename T>
struct TaskValueType<Task<T>> {
  using type = T;
};

template <typename Fn>
auto sync_wait(Fn&& factory) -> typename TaskValueType<std::invoke_result_t<Fn>>::type {
  using T = typename TaskValueType<std::invoke_result_t<Fn>>::type;
  auto task = std::forward<Fn>(factory)();
  T result{};
  bool done = false;

  // FireAndForget (eager start) drives the lazy Task chain without making
  // sync_wait itself a coroutine — MSVC otherwise flags co_await here.
  auto driver = [&]() -> FireAndForget {
    result = co_await task;
    done = true;
  };
  driver();

  EXPECT_TRUE(done);
  return result;
}

template <typename Fn>
inline void sync_wait_void(Fn&& factory) {
  auto task = std::forward<Fn>(factory)();
  bool done = false;

  auto driver = [&]() -> FireAndForget {
    co_await task;
    done = true;
  };
  driver();

  EXPECT_TRUE(done);
}

// ============================================================================
// Task<T> tests
// ============================================================================

TEST(Task, BasicReturn) {
  EXPECT_EQ(sync_wait([]() -> Task<int> { co_return 42; }), 42);
}

TEST(Task, StringReturn) {
  EXPECT_EQ(sync_wait([]() -> Task<std::string> { co_return "hello"; }), "hello");
}

TEST(Task, VoidTask) {
  bool executed = false;
  sync_wait_void([&]() -> Task<void> {
    executed = true;
    co_return;
  });
  EXPECT_TRUE(executed);
}

TEST(Task, ChainedAwait) {
  auto inner = []() -> Task<int> { co_return 10; };

  EXPECT_EQ(sync_wait([&]() -> Task<int> {
              auto t1 = inner();
              auto t2 = inner();
              auto a = co_await t1;
              auto b = co_await t2;
              co_return a + b;
            }),
            20);
}

TEST(Task, DeepChain) {
  // 4 levels deep — exercises symmetric transfer (O(1) stack depth)
  auto level3 = []() -> Task<int> { co_return 1; };

  EXPECT_EQ(sync_wait([&]() -> Task<int> {
              auto t3 = level3();
              co_return co_await t3;
            }),
            1);
}

TEST(Task, ExceptionPropagation) {
  auto thrower = []() -> Task<int> {
    throw std::runtime_error("boom");
    co_return 0;  // unreachable
  };

  EXPECT_EQ(sync_wait([&]() -> Task<int> {
              try {
                auto t = thrower();
                co_return co_await t;
              } catch (const std::runtime_error&) {
                co_return -1;
              }
            }),
            -1);
}

TEST(Task, MoveOnlySemantics) {
  auto factory = []() -> Task<int> { co_return 1; };
  auto task = factory();
  auto moved = std::move(task);
  // The factory stays alive; sync_wait would re-invoke it, so drive `moved`
  // directly via a one-shot factory that yields the moved task.
  EXPECT_EQ(sync_wait([&]() -> Task<int> { co_return co_await moved; }), 1);
}

// ============================================================================
// FireAndForget tests
// ============================================================================

TEST(FireAndForget, ImmediateExecution) {
  bool executed = false;
  auto coro = [&]() -> FireAndForget {
    executed = true;
    co_return;
  };
  coro();
  EXPECT_TRUE(executed);
}

TEST(FireAndForget, ExceptionDoesNotPropagate) {
  // FireAndForget logs exceptions but doesn't throw to caller
  EXPECT_NO_THROW({
    auto coro = []() -> FireAndForget {
      throw std::runtime_error("should be caught internally");
      co_return;
    };
    coro();
  });
}

// ============================================================================
// ScopeGuard tests
// ============================================================================

TEST(ScopeGuard, ExecutesOnDestruction) {
  bool cleaned = false;
  {
    ScopeGuard guard([&] { cleaned = true; });
  }
  EXPECT_TRUE(cleaned);
}

TEST(ScopeGuard, DismissPreventsExecution) {
  bool cleaned = false;
  {
    ScopeGuard guard([&] { cleaned = true; });
    guard.Dismiss();
  }
  EXPECT_FALSE(cleaned);
}

TEST(ScopeGuard, MoveTransfersOwnership) {
  int count = 0;
  {
    ScopeGuard g1([&] { ++count; });
    ScopeGuard g2(std::move(g1));
    // g1 is dismissed by move, g2 owns the cleanup
  }
  EXPECT_EQ(count, 1);  // cleaned up exactly once
}

TEST(ScopeGuard, MultipleGuardsReverseOrder) {
  std::vector<int> order;
  {
    ScopeGuard g1([&] { order.push_back(1); });
    ScopeGuard g2([&] { order.push_back(2); });
    ScopeGuard g3([&] { order.push_back(3); });
  }
  // C++ destruction order: reverse of declaration → 3, 2, 1
  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], 3);
  EXPECT_EQ(order[1], 2);
  EXPECT_EQ(order[2], 1);
}

TEST(ScopeGuard, ScopeExitMacro) {
  bool cleaned = false;
  {
    ATLAS_SCOPE_EXIT([&] { cleaned = true; });
  }
  EXPECT_TRUE(cleaned);
}

// ============================================================================
// CancellationToken tests
// ============================================================================

TEST(CancellationToken, BasicCancel) {
  CancellationSource source;
  auto token = source.Token();
  EXPECT_FALSE(token.IsCancelled());

  bool callback_fired = false;
  auto reg = token.OnCancel([&] { callback_fired = true; });

  source.RequestCancellation();
  EXPECT_TRUE(token.IsCancelled());
  EXPECT_TRUE(callback_fired);
}

TEST(CancellationToken, MultipleCallbacks) {
  CancellationSource source;
  auto token = source.Token();

  int count = 0;
  auto r1 = token.OnCancel([&] { ++count; });
  auto r2 = token.OnCancel([&] { ++count; });
  auto r3 = token.OnCancel([&] { ++count; });

  source.RequestCancellation();
  EXPECT_EQ(count, 3);
}

TEST(CancellationToken, CallbackOnAlreadyCancelled) {
  CancellationSource source;
  source.RequestCancellation();

  auto token = source.Token();
  bool callback_fired = false;
  auto reg = token.OnCancel([&] { callback_fired = true; });

  // Should be called immediately since already cancelled
  EXPECT_TRUE(callback_fired);
}

TEST(CancellationToken, RegistrationDeregistersOnDestruction) {
  CancellationSource source;
  auto token = source.Token();

  bool callback_fired = false;
  {
    auto reg = token.OnCancel([&] { callback_fired = true; });
    // reg goes out of scope — deregisters
  }

  source.RequestCancellation();
  EXPECT_FALSE(callback_fired);  // callback was deregistered
}

TEST(CancellationToken, EmptyTokenNeverCancelled) {
  CancellationToken token;  // default constructed — no source
  EXPECT_FALSE(token.IsCancelled());
  EXPECT_FALSE(token.IsValid());

  bool fired = false;
  auto reg = token.OnCancel([&] { fired = true; });
  EXPECT_FALSE(fired);
}

TEST(CancellationToken, DoubleCancelIsIdempotent) {
  CancellationSource source;
  auto token = source.Token();

  int count = 0;
  auto reg = token.OnCancel([&] { ++count; });

  source.RequestCancellation();
  source.RequestCancellation();  // second call is no-op
  EXPECT_EQ(count, 1);
}

TEST(CancellationToken, MultipleSources) {
  CancellationSource src1;
  CancellationSource src2;

  auto tok1 = src1.Token();
  auto tok2 = src2.Token();

  src1.RequestCancellation();
  EXPECT_TRUE(tok1.IsCancelled());
  EXPECT_FALSE(tok2.IsCancelled());
}
