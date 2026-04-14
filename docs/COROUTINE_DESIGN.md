# C++20 协程基础设施设计

> 日期: 2026-04-14
> 状态: 待实现
> 适用范围: `src/lib/coro/` (新建库) + `InterfaceTable` 扩展 + `LoginApp` 试点迁移
> 关联: [登录回滚协议](LOGIN_ROLLBACK_PROTOCOL_20260412.md) | [BigWorld RPC 参考](BIGWORLD_RPC_REFERENCE.md)

---

## 1. 背景与动机

Atlas 当前所有异步操作（网络 RPC、数据库查询、定时器）基于回调模式。多步骤跨进程流程
（如 LoginApp 的 4 步登录链）需要手动管理:

- `PendingStage` 枚举跟踪异步状态
- `pending_` map 存储中间状态
- `request_id` 手动关联请求与响应
- 周期性 `cleanup_expired_*()` 处理超时
- 分散在多个 handler 中的回滚逻辑

本方案引入 C++20 stackless coroutine，使多步异步流程用顺序代码表达，同时通过 RAII
scope guard 保证分布式事务的回滚原子性。

### 1.1 设计原则

1. **渐进式迁移** — 协程与回调共存，不要求一次性重写
2. **零外部依赖** — 不依赖 Boost、cppcoro 等第三方库
3. **单线程安全** — 协程在 `EventDispatcher` 线程恢复，不引入同步开销
4. **编译器保证回滚** — 用 RAII 析构器代替手动 cleanup 调用
5. **约定优于配置** — 利用已有的消息约定（`request_id` 是序列化首字段）

### 1.2 不在本方案范围内

- 多线程协程调度器（保持单 dispatcher 单线程模型）
- `std::execution` / sender-receiver 模型（等 C++26）
- 全量重写所有 handler（只迁移多步流程，单步 handler 保持回调）

---

## 2. 架构总览

```
┌──────────────────────────────────────────────────────────┐
│                     应用层                                │
│   LoginApp::handle_login_coro()                          │
│     co_await rpc_call<AuthLoginResult>(...)               │
│     co_await rpc_call<AllocateBaseAppResult>(...)         │
│     co_await rpc_call<PrepareLoginResult>(...)            │
│                                                          │
│   ┌─────────────────────┐  ┌──────────────────────┐      │
│   │ PrepareLoginGuard   │  │ UsernameDedup         │     │
│   │ (RAII rollback)     │  │ (RAII dedup guard)    │     │
│   └─────────────────────┘  └──────────────────────┘      │
├──────────────────────────────────────────────────────────┤
│                   协程原语层 (src/lib/coro/)              │
│                                                          │
│  ┌──────────┐ ┌───────────────┐ ┌──────────────────┐     │
│  │ Task<T>  │ │ FireAndForget │ │ CancellationToken│     │
│  └──────────┘ └───────────────┘ └──────────────────┘     │
│  ┌──────────┐ ┌───────────────┐ ┌──────────────────┐     │
│  │ rpc_call │ │ async_sleep   │ │ await_bg_task    │     │
│  └─────┬────┘ └───────┬───────┘ └────────┬─────────┘     │
│        │              │                  │               │
│  ┌─────▼──────────────▼──────────────────▼─────────┐     │
│  │           PendingRpcRegistry                     │     │
│  │  key: (MessageID, request_id) → coroutine_handle│     │
│  └─────────────────────┬───────────────────────────┘     │
├────────────────────────┼────────────────────────────────┤
│                   网络层 (现有)                          │
│                        │                                │
│  InterfaceTable::dispatch()                             │
│    ├─ pre_dispatch_hook → PendingRpcRegistry::try_dispatch│
│    │   └─ 匹配: 恢复协程 (消息被消费)                     │
│    │   └─ 不匹配: 走原有 handler 路径                     │
│    └─ entry->handler->handle_message()  (不变)           │
│                                                          │
│  EventDispatcher                                         │
│    ├─ process_frequent_tasks()                           │
│    ├─ timers_.process()  ← async_sleep 在此恢复           │
│    └─ poller_->poll()    ← IO 事件                       │
└──────────────────────────────────────────────────────────┘
```

---

## 3. 核心类型

### 3.1 `Task<T>` — 惰性协程返回类型

**文件:** `src/lib/coro/task.hpp`

`Task<T>` 是协程的核心返回类型。惰性启动，只有被 `co_await` 时才开始执行。

```cpp
#pragma once

#include "foundation/error.hpp"

#include <coroutine>
#include <optional>
#include <utility>
#include <variant>

namespace atlas
{

template <typename T>
class Task;

namespace detail
{

// FinalAwaiter: 协程结束时 symmetric transfer 回到调用者
struct FinalAwaiter
{
    auto await_ready() noexcept -> bool { return false; }

    // symmetric transfer: 直接跳转到 continuation，不增加调用栈深度
    template <typename Promise>
    auto await_suspend(std::coroutine_handle<Promise> h) noexcept
        -> std::coroutine_handle<>
    {
        if (h.promise().continuation)
            return h.promise().continuation;
        return std::noop_coroutine();
    }

    void await_resume() noexcept {}
};

// PromiseBase: 所有 Task promise 的公共部分
template <typename T>
struct PromiseBase
{
    std::coroutine_handle<> continuation{};

    auto initial_suspend() noexcept -> std::suspend_always { return {}; }
    auto final_suspend() noexcept -> FinalAwaiter { return {}; }
    void unhandled_exception() { result_.template emplace<2>(std::current_exception()); }

    auto get_result() -> T
    {
        if (auto* ex = std::get_if<2>(&result_))
            std::rethrow_exception(*ex);
        return std::move(std::get<1>(result_));
    }

    // 0 = 未设置, 1 = 值, 2 = 异常
    std::variant<std::monostate, T, std::exception_ptr> result_;
};

// void 特化
template <>
struct PromiseBase<void>
{
    std::coroutine_handle<> continuation{};

    auto initial_suspend() noexcept -> std::suspend_always { return {}; }
    auto final_suspend() noexcept -> FinalAwaiter { return {}; }
    void unhandled_exception() { exception_ = std::current_exception(); }

    void get_result()
    {
        if (exception_)
            std::rethrow_exception(exception_);
    }

    std::exception_ptr exception_{};
};

}  // namespace detail

// ============================================================================
// Task<T>
// ============================================================================

template <typename T = void>
class [[nodiscard]] Task
{
public:
    struct promise_type : detail::PromiseBase<T>
    {
        auto get_return_object() -> Task
        {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        void return_value(T value)
        {
            this->result_.template emplace<1>(std::move(value));
        }
    };

    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    Task& operator=(Task&& other) noexcept
    {
        if (this != &other)
        {
            if (handle_)
                handle_.destroy();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    ~Task()
    {
        if (handle_)
            handle_.destroy();
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    // co_await 支持
    auto operator co_await() &
    {
        struct Awaiter
        {
            std::coroutine_handle<promise_type> handle;

            auto await_ready() -> bool { return handle.done(); }

            auto await_suspend(std::coroutine_handle<> caller) noexcept
                -> std::coroutine_handle<>
            {
                handle.promise().continuation = caller;
                return handle;  // symmetric transfer: 跳转执行此 Task
            }

            auto await_resume() -> T { return handle.promise().get_result(); }
        };
        return Awaiter{handle_};
    }

private:
    explicit Task(std::coroutine_handle<promise_type> h) : handle_(h) {}
    std::coroutine_handle<promise_type> handle_;
};

// ============================================================================
// Task<void> 特化
// ============================================================================

template <>
struct Task<void>::promise_type : detail::PromiseBase<void>
{
    auto get_return_object() -> Task
    {
        return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    void return_void() {}
};

}  // namespace atlas
```

**关键设计决策:**

| 决策 | 原因 |
|------|------|
| `initial_suspend` → `suspend_always` | 惰性启动：Task 不会在构造时立刻运行，只有被 `co_await` 才执行 |
| `final_suspend` → `FinalAwaiter` | symmetric transfer: 协程结束时直接跳回调用者，避免递归调用栈溢出 |
| `std::variant<monostate, T, exception_ptr>` | 统一存储正常值和异常，`get_result()` 透明重抛 |
| move-only | 协程 frame 所有权唯一，避免 double-destroy |

**symmetric transfer 工作原理:**

普通实现中，`co_await` 内部 Task 完成后会在回调栈中恢复外部 Task，形成嵌套调用:

```
A co_await B co_await C co_await D
→ A.resume() → B.resume() → C.resume() → D runs → C.resume returns → ...
→ 调用栈深度: O(N)，深嵌套时可能栈溢出
```

symmetric transfer 通过让 `final_suspend` 返回 `coroutine_handle<>` 实现尾调用优化:

```
D 完成 → final_suspend 返回 C 的 handle → 编译器直接跳到 C → C 不在 D 的栈上
→ 调用栈深度: O(1)
```

MSVC (VS2022 17.4+) 完整支持此优化。

---

### 3.2 `FireAndForget` — 即发即忘协程

**文件:** `src/lib/coro/fire_and_forget.hpp`

用于处理入站请求 — 每个客户端请求 spawn 一个独立协程，无人 `co_await` 它。

```cpp
#pragma once

#include "foundation/callback_utils.hpp"
#include "foundation/log.hpp"

#include <coroutine>

namespace atlas
{

class FireAndForget
{
public:
    struct promise_type
    {
        auto get_return_object() -> FireAndForget { return {}; }
        auto initial_suspend() noexcept -> std::suspend_never { return {}; }
        auto final_suspend() noexcept -> std::suspend_never { return {}; }
        void return_void() {}

        void unhandled_exception()
        {
            // 捕获并记录，不传播（无人等待此协程）
            try
            {
                std::rethrow_exception(std::current_exception());
            }
            catch (const std::exception& e)
            {
                ATLAS_LOG_ERROR("FireAndForget coroutine threw: {}", e.what());
            }
            catch (...)
            {
                ATLAS_LOG_ERROR("FireAndForget coroutine threw unknown exception");
            }
        }
    };
};

}  // namespace atlas
```

**生命周期:**

- `initial_suspend` → `suspend_never`: 构造后立即开始执行，执行到第一个 `co_await` 挂起
- `final_suspend` → `suspend_never`: 协程正常完成后 frame 自动销毁
- 无人持有 `coroutine_handle` — frame 生命周期完全由协程自身管理
- 如果协程在某个 `co_await` 处挂起等待回复，frame 由恢复者（timer callback / RPC
  registry）持有的 `coroutine_handle` 维持

**异常处理:**

协程内部的异常在 `unhandled_exception()` 中被捕获并记录日志，不会传播到事件循环。
这与现有 `safe_invoke()` 的行为一致。

---

### 3.3 `CancellationToken` / `CancellationSource` — 协作式取消

**文件:** `src/lib/coro/cancellation.hpp`

取消机制用于在客户端断连、进程关闭等场景下安全中断挂起中的协程。

```cpp
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace atlas
{

class CancellationToken;

// 取消回调注册令牌，析构时自动反注册
class CancelRegistration
{
public:
    CancelRegistration() = default;
    ~CancelRegistration();

    CancelRegistration(CancelRegistration&&) noexcept;
    CancelRegistration& operator=(CancelRegistration&&) noexcept;

    CancelRegistration(const CancelRegistration&) = delete;
    CancelRegistration& operator=(const CancelRegistration&) = delete;

private:
    friend class CancellationToken;
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// 共享的取消状态
struct CancellationState
{
    struct Entry
    {
        uint64_t id{0};
        std::function<void()> callback;
    };

    bool cancelled{false};
    uint64_t next_id{1};
    std::vector<Entry> callbacks;
};

// 取消源 — 持有者调用 request_cancellation() 触发取消
class CancellationSource
{
public:
    CancellationSource() : state_(std::make_shared<CancellationState>()) {}

    void request_cancellation()
    {
        if (state_->cancelled)
            return;
        state_->cancelled = true;

        // 调用所有注册的回调（取消 timer、中断 pending RPC 等）
        for (auto& entry : state_->callbacks)
        {
            if (entry.callback)
                entry.callback();
        }
        state_->callbacks.clear();
    }

    [[nodiscard]] auto token() const -> CancellationToken;

    [[nodiscard]] auto is_cancellation_requested() const -> bool
    {
        return state_->cancelled;
    }

private:
    std::shared_ptr<CancellationState> state_;
};

// 取消令牌 — 传递给 awaitable，用于检查取消状态和注册回调
class CancellationToken
{
public:
    CancellationToken() = default;  // 空 token，永不取消

    [[nodiscard]] auto is_cancelled() const -> bool
    {
        return state_ && state_->cancelled;
    }

    // 注册取消回调（RAII: CancelRegistration 析构时自动反注册）
    // 如果已经取消，立即调用 callback 并返回空 registration
    [[nodiscard]] auto on_cancel(std::function<void()> callback) -> CancelRegistration
    {
        if (!state_)
            return {};

        if (state_->cancelled)
        {
            callback();  // 已取消，立即调用
            return {};
        }

        auto id = state_->next_id++;
        state_->callbacks.push_back({id, std::move(callback)});
        // 返回 CancelRegistration，持有反注册能力
        // ... (实现细节省略)
        return {};
    }

    [[nodiscard]] auto is_valid() const -> bool { return state_ != nullptr; }

private:
    friend class CancellationSource;
    explicit CancellationToken(std::shared_ptr<CancellationState> state)
        : state_(std::move(state))
    {
    }

    std::shared_ptr<CancellationState> state_;
};
```

**与 Channel 断连的联动模式:**

```cpp
// 在协程入口处创建 CancellationSource，绑定到客户端 Channel
auto cancel_source = CancellationSource{};
auto token = cancel_source.token();

// Channel 断连时触发取消
auto disconnect_guard = ScopeGuard([&] {
    cancel_source.request_cancellation();
});
```

当 `request_cancellation()` 被调用时:

1. 设置 `cancelled = true`
2. 调用所有注册的回调（每个 `rpc_call` 的 awaitable 会注册一个回调）
3. 回调中取消超时定时器、从 `PendingRpcRegistry` 移除条目、恢复协程
4. 协程在 `co_await` 点收到 `ErrorCode::Cancelled` 错误
5. 协程 `co_return` 或抛异常，栈展开触发所有 scope guard 析构器

---

### 3.4 `ScopeGuard` — RAII 回滚守卫

**文件:** `src/lib/coro/scope_guard.hpp`

通用的 RAII 守卫，用于在协程退出时执行清理操作。

```cpp
#pragma once

#include <functional>
#include <utility>

namespace atlas
{

class ScopeGuard
{
public:
    explicit ScopeGuard(std::function<void()> cleanup)
        : cleanup_(std::move(cleanup))
    {
    }

    ~ScopeGuard()
    {
        if (!dismissed_ && cleanup_)
            cleanup_();
    }

    // 成功路径: 解除守卫，不再执行清理
    void dismiss() noexcept { dismissed_ = true; }

    ScopeGuard(ScopeGuard&& other) noexcept
        : cleanup_(std::move(other.cleanup_)), dismissed_(other.dismissed_)
    {
        other.dismissed_ = true;
    }

    ScopeGuard& operator=(ScopeGuard&&) = delete;
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;

private:
    std::function<void()> cleanup_;
    bool dismissed_{false};
};

// 便捷宏，用于创建匿名 scope guard
// 用法: ATLAS_SCOPE_EXIT { cleanup_code; };
// 注意: 大括号内的代码在 scope 退出时执行
#define ATLAS_SCOPE_EXIT_CAT2(x, y) x##y
#define ATLAS_SCOPE_EXIT_CAT(x, y) ATLAS_SCOPE_EXIT_CAT2(x, y)
#define ATLAS_SCOPE_EXIT \
    auto ATLAS_SCOPE_EXIT_CAT(scope_exit_, __LINE__) = ScopeGuard

}  // namespace atlas
```

---

## 4. Awaitable 原语

### 4.1 `async_sleep` — 定时器等待

**文件:** `src/lib/coro/async_sleep.hpp`

```cpp
#pragma once

#include "coro/cancellation.hpp"
#include "foundation/error.hpp"
#include "foundation/time.hpp"
#include "network/event_dispatcher.hpp"

#include <coroutine>

namespace atlas
{

// co_await async_sleep(dispatcher, Milliseconds(100));
// 返回 Result<void>: 成功或 Cancelled
inline auto async_sleep(EventDispatcher& dispatcher, Duration delay,
                        CancellationToken token = {})
{
    struct Awaiter
    {
        EventDispatcher& dispatcher;
        Duration delay;
        CancellationToken token;
        TimerHandle timer_handle{};
        CancelRegistration cancel_reg{};
        bool cancelled{false};

        auto await_ready() -> bool
        {
            // 已取消则立即返回
            return token.is_cancelled();
        }

        void await_suspend(std::coroutine_handle<> caller)
        {
            // 注册定时器
            timer_handle = dispatcher.add_timer(delay,
                [caller](TimerHandle) mutable { caller.resume(); });

            // 注册取消回调: 取消定时器并恢复协程
            if (token.is_valid())
            {
                cancel_reg = token.on_cancel([this, caller]() mutable {
                    cancelled = true;
                    dispatcher.cancel_timer(timer_handle);
                    caller.resume();
                });
            }
        }

        auto await_resume() -> Result<void>
        {
            if (cancelled || token.is_cancelled())
                return Error{ErrorCode::Cancelled, "sleep cancelled"};
            return {};
        }
    };

    return Awaiter{dispatcher, delay, std::move(token)};
}

}  // namespace atlas
```

**注意:** 需要在 `ErrorCode` 枚举中新增 `Cancelled` 值（见第 7 节）。

---

### 4.2 `await_bg_task` — 线程池等待

**文件:** `src/lib/coro/await_bg_task.hpp`

将工作提交到 `BgTaskManager` 线程池，`co_await` 等待主线程回调完成。

```cpp
#pragma once

#include "network/bg_task_manager.hpp"

#include <coroutine>
#include <optional>
#include <type_traits>
#include <utility>

namespace atlas
{

template <typename F>
    requires std::invocable<F>
auto await_bg_task(BgTaskManager& mgr, F&& work)
{
    using R = std::invoke_result_t<F>;

    struct Awaiter
    {
        BgTaskManager& mgr;
        F work;
        std::optional<R> result;

        auto await_ready() -> bool { return false; }

        void await_suspend(std::coroutine_handle<> caller)
        {
            mgr.add_task(
                // 线程池线程: 执行工作
                [this]() { result.emplace(work()); },
                // 主线程: 恢复协程
                [caller]() mutable { caller.resume(); }
            );
        }

        auto await_resume() -> R { return std::move(*result); }
    };

    return Awaiter{mgr, std::forward<F>(work)};
}

// void 返回值特化
template <typename F>
    requires(std::invocable<F> && std::is_void_v<std::invoke_result_t<F>>)
auto await_bg_task(BgTaskManager& mgr, F&& work)
{
    struct Awaiter
    {
        BgTaskManager& mgr;
        F work;

        auto await_ready() -> bool { return false; }

        void await_suspend(std::coroutine_handle<> caller)
        {
            mgr.add_task(
                [this]() { work(); },
                [caller]() mutable { caller.resume(); }
            );
        }

        void await_resume() {}
    };

    return Awaiter{mgr, std::forward<F>(work)};
}

}  // namespace atlas
```

**线程安全保证:**

- `work()` 在线程池线程执行 — 不得访问网络对象
- `caller.resume()` 在主线程执行（通过 `BgTaskManager::do_task()` →
  `FrequentTask` 回调）
- 恢复后的协程在主线程继续执行，可安全访问所有对象

---

### 4.3 `PendingRpcRegistry` — RPC 注册表

**文件:** `src/lib/coro/pending_rpc_registry.hpp`, `src/lib/coro/pending_rpc_registry.cpp`

管理所有挂起中的协程 RPC 调用。收到回复消息时查找并恢复对应协程。

```cpp
#pragma once

#include "foundation/error.hpp"
#include "foundation/time.hpp"
#include "network/event_dispatcher.hpp"
#include "network/message.hpp"

#include <coroutine>
#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>

namespace atlas
{

class PendingRpcRegistry
{
public:
    explicit PendingRpcRegistry(EventDispatcher& dispatcher);
    ~PendingRpcRegistry();

    PendingRpcRegistry(const PendingRpcRegistry&) = delete;
    PendingRpcRegistry& operator=(const PendingRpcRegistry&) = delete;

    // 回复数据的回调签名
    // payload 是完整的消息体（含 request_id）
    using ReplyCallback = std::function<void(std::span<const std::byte> payload)>;
    // 超时/取消时调用的回调
    using ErrorCallback = std::function<void(Error error)>;

    struct PendingHandle
    {
        MessageID reply_id{0};
        uint32_t request_id{0};
        [[nodiscard]] auto is_valid() const -> bool { return reply_id != 0; }
    };

    // 注册一个挂起的 RPC 调用
    //
    // reply_id:     期望的回复消息 ID
    // request_id:   请求关联 ID
    // on_reply:     收到回复时的回调（主线程调用）
    // on_error:     超时或取消时的回调（主线程调用）
    // timeout:      超时时长
    //
    // 返回 PendingHandle 用于手动取消
    auto register_pending(MessageID reply_id, uint32_t request_id,
                          ReplyCallback on_reply, ErrorCallback on_error,
                          Duration timeout) -> PendingHandle;

    // 尝试处理入站消息
    // 由 InterfaceTable 的 pre-dispatch hook 调用
    //
    // 约定: 所有 RPC Reply 消息的 request_id 是序列化的第一个字段 (uint32_t LE)
    //
    // 返回 true: 消息已被消费（恢复了对应的协程）
    // 返回 false: 没有匹配的 pending entry，消息走原有 handler 路径
    auto try_dispatch(MessageID id, std::span<const std::byte> payload) -> bool;

    // 取消指定 pending（从 map 移除，取消 timer，调用 error callback）
    void cancel(PendingHandle handle);

    // 取消所有 pending（进程关闭时调用）
    void cancel_all();

    // 查询
    [[nodiscard]] auto pending_count() const -> size_t;

private:
    // 复合键: (MessageID, request_id)
    struct PendingKey
    {
        MessageID reply_id;
        uint32_t request_id;

        auto operator==(const PendingKey&) const -> bool = default;
    };

    struct PendingKeyHash
    {
        auto operator()(const PendingKey& k) const -> size_t
        {
            // 将两个整数组合成一个 hash
            auto h1 = std::hash<uint16_t>{}(k.reply_id);
            auto h2 = std::hash<uint32_t>{}(k.request_id);
            return h1 ^ (h2 << 16);
        }
    };

    struct PendingEntry
    {
        ReplyCallback on_reply;
        ErrorCallback on_error;
        TimerHandle timeout_timer;
    };

    // 从 payload 前 4 字节读取 request_id (little-endian uint32_t)
    static auto extract_request_id(std::span<const std::byte> payload) -> uint32_t;

    std::unordered_map<PendingKey, PendingEntry, PendingKeyHash> pending_;
    EventDispatcher& dispatcher_;
};
```

**`try_dispatch` 实现:**

```cpp
auto PendingRpcRegistry::try_dispatch(MessageID id,
                                      std::span<const std::byte> payload) -> bool
{
    if (payload.size() < sizeof(uint32_t))
        return false;

    auto request_id = extract_request_id(payload);
    auto key = PendingKey{id, request_id};
    auto it = pending_.find(key);

    if (it == pending_.end())
        return false;  // 没有匹配的协程在等待此回复

    // 取消超时定时器
    dispatcher_.cancel_timer(it->second.timeout_timer);

    // 保存回调（erase 前移动出来，防止回调中重入修改 map）
    auto on_reply = std::move(it->second.on_reply);
    pending_.erase(it);

    // 调用回调，传入完整 payload（由 rpc_call 的 awaiter 反序列化）
    on_reply(payload);

    return true;
}
```

**`extract_request_id` 实现:**

```cpp
auto PendingRpcRegistry::extract_request_id(
    std::span<const std::byte> payload) -> uint32_t
{
    uint32_t id = 0;
    std::memcpy(&id, payload.data(), sizeof(uint32_t));
    return atlas::endian::from_little(id);
}
```

> **关键约定:** 所有可作为 RPC Reply 的消息类型，`request_id` (uint32_t) 必须是
> `serialize()` 写入的第一个字段。当前所有消息定义已满足此约定（已验证
> `login_messages.hpp`、`dbapp_messages.hpp`、`baseapp_messages.hpp` 中全部消息）。

---

### 4.4 `rpc_call` — 核心 RPC 等待

**文件:** `src/lib/coro/rpc_call.hpp`

发送一个请求消息，挂起协程等待回复，支持超时和取消。

```cpp
#pragma once

#include "coro/cancellation.hpp"
#include "coro/pending_rpc_registry.hpp"
#include "foundation/error.hpp"
#include "network/channel.hpp"
#include "network/message.hpp"
#include "serialization/binary_stream.hpp"

#include <coroutine>
#include <optional>
#include <span>

namespace atlas
{

// 要求 Reply 消息有 request_id 字段
template <typename T>
concept RpcReplyMessage = NetworkMessage<T> && requires(const T& msg) {
    { msg.request_id } -> std::convertible_to<uint32_t>;
};

template <RpcReplyMessage Reply, NetworkMessage Request>
auto rpc_call(PendingRpcRegistry& registry, Channel& channel,
              const Request& request, Duration timeout,
              CancellationToken token = {})
{
    struct Awaiter
    {
        PendingRpcRegistry& registry;
        Channel& channel;
        const Request& request;
        Duration timeout;
        CancellationToken token;

        // 结果存储
        Result<Reply> result{Error{ErrorCode::InternalError, "rpc_call: not completed"}};
        PendingRpcRegistry::PendingHandle pending_handle{};
        CancelRegistration cancel_reg{};

        auto await_ready() -> bool
        {
            if (token.is_cancelled())
            {
                result = Error{ErrorCode::Cancelled, "rpc_call: already cancelled"};
                return true;
            }
            return false;
        }

        void await_suspend(std::coroutine_handle<> caller)
        {
            // 1. 发送请求
            auto send_result = channel.send_message(request);
            if (!send_result)
            {
                result = Error{send_result.error().code(),
                    std::string("rpc_call: send failed: ") +
                    std::string(send_result.error().message())};
                caller.resume();
                return;
            }

            // 2. 注册 pending entry
            auto reply_id = Reply::descriptor().id;
            auto request_id = request.request_id;

            pending_handle = registry.register_pending(
                reply_id, request_id,
                // on_reply: 反序列化回复消息并恢复协程
                [this, caller](std::span<const std::byte> payload) mutable {
                    BinaryReader reader(payload);
                    auto reply = Reply::deserialize(reader);
                    if (reply.has_value())
                        result = std::move(reply.value());
                    else
                        result = reply.error();
                    caller.resume();
                },
                // on_error: 超时或取消
                [this, caller](Error error) mutable {
                    result = std::move(error);
                    caller.resume();
                },
                timeout
            );

            // 3. 注册取消回调
            if (token.is_valid())
            {
                cancel_reg = token.on_cancel([this]() {
                    registry.cancel(pending_handle);
                    // cancel 内部会调用 on_error callback → 恢复协程
                });
            }
        }

        auto await_resume() -> Result<Reply>
        {
            return std::move(result);
        }
    };

    return Awaiter{registry, channel, request, timeout, std::move(token)};
}

}  // namespace atlas
```

**完整生命周期:**

```
rpc_call<AuthLoginResult>(registry, channel, auth_msg, 10s, token)
  │
  ├─ await_ready(): 检查 token 是否已取消
  │   └─ 已取消 → 立即返回 Cancelled 错误
  │
  ├─ await_suspend(caller):
  │   ├─ channel.send_message(request) → 发送到网络
  │   │   └─ 发送失败 → 立即 resume with 错误
  │   │
  │   ├─ registry.register_pending(reply_id, request_id, ...)
  │   │   └─ 在 map 中注册 (AuthLoginResult::id, request_id) → callbacks
  │   │   └─ 启动超时 timer
  │   │
  │   └─ token.on_cancel(callback)
  │       └─ 注册取消回调: cancel pending entry
  │
  ├─ [挂起中] 等待以下三者之一:
  │   │
  │   ├─ 回复到达: InterfaceTable::dispatch()
  │   │   → pre_dispatch_hook → PendingRpcRegistry::try_dispatch()
  │   │   → 匹配 (reply_id, request_id) → 取消 timer
  │   │   → 调用 on_reply callback → Reply::deserialize(payload)
  │   │   → caller.resume()
  │   │
  │   ├─ 超时: timer 到期
  │   │   → 从 map 移除 entry
  │   │   → 调用 on_error(Timeout) callback
  │   │   → caller.resume()
  │   │
  │   └─ 取消: CancellationSource::request_cancellation()
  │       → 取消回调执行 registry.cancel(handle)
  │       → 取消 timer + 从 map 移除
  │       → 调用 on_error(Cancelled) callback
  │       → caller.resume()
  │
  └─ await_resume(): 返回 Result<Reply>
      ├─ 成功: Reply 对象
      ├─ 超时: Error{Timeout}
      └─ 取消: Error{Cancelled}
```

---

## 5. InterfaceTable 集成

### 5.1 修改 `InterfaceTable`

**修改文件:** `src/lib/network/interface_table.hpp`

新增 pre-dispatch hook。这是对现有代码**唯一的结构性修改**:

```cpp
// 新增类型
using PreDispatchHook = std::function<bool(MessageID, std::span<const std::byte>)>;

// 新增方法
void set_pre_dispatch_hook(PreDispatchHook hook) { pre_dispatch_hook_ = std::move(hook); }

// 新增成员
PreDispatchHook pre_dispatch_hook_;
```

**修改文件:** `src/lib/network/interface_table.cpp`

在 `dispatch()` 方法开头插入 hook 调用:

```cpp
auto InterfaceTable::dispatch(const Address& source, Channel* channel, MessageID id,
                              BinaryReader& data) -> Result<void>
{
    // ── 新增: pre-dispatch hook ──
    if (pre_dispatch_hook_)
    {
        // 构造 span 但不消耗 reader 的位置
        auto payload = std::span<const std::byte>(
            data.data().data() + data.position(), data.remaining());
        if (pre_dispatch_hook_(id, payload))
        {
            return {};  // hook 消费了此消息
        }
    }

    // ── 原有逻辑不变 ──
    auto* entry = entries_.get(id);
    // ...
}
```

### 5.2 修改 `BinaryReader`

**修改文件:** `src/lib/serialization/binary_stream.hpp`

新增 `data()` 访问器（如果不存在），使 `dispatch()` 能构造 payload span:

```cpp
// BinaryReader 新增:
[[nodiscard]] auto data() const -> std::span<const std::byte> { return data_; }
```

---

## 6. 回滚机制 — 分布式 Saga 保证

### 6.1 问题域

登录流程是一个跨 4 个进程的分布式 Saga。每一步可能分配远程资源（DBApp checkout、
BaseApp entity），失败时必须回滚已分配的资源。

```
Step 1: Auth        → 无远程资源    → 失败: 无需回滚
Step 2: Allocate    → 无远程资源    → 失败: 无需回滚
Step 3: Prepare     → BaseApp entity + DBApp checkout → 失败: 必须回滚
```

### 6.2 协程化的回滚策略: 逐步 RAII Guard

核心思想: **在每一步分配远程资源后，立即创建对应的 RAII scope guard。成功完成后
`dismiss()`；协程提前退出时，析构器自动执行回滚。**

```cpp
FireAndForget LoginApp::handle_login_coro(uint64_t client_channel_id,
                                          login::LoginRequest request)
{
    // ═══════ Guard 1: 用户名去重 ═══════
    // 构造时: pending_by_username_[username] = request_id
    // 析构时: pending_by_username_.erase(username)
    auto dedup_guard = ScopeGuard([&, username = request.username] {
        pending_by_username_.erase(username);
    });
    if (pending_by_username_.contains(request.username))
    {
        dedup_guard.dismiss();  // 没有插入，不需要清理
        send_login_error(client_channel_id, LoginStatus::LoginInProgress);
        co_return;
    }
    auto rid = next_request_id_++;
    pending_by_username_[request.username] = rid;

    // ═══════ Guard 2: 客户端断连 → 取消协程 ═══════
    auto cancel_source = CancellationSource{};
    auto token = cancel_source.token();
    // 注册断连回调
    auto saved_disconnect = /* save existing disconnect callback */;
    external_network_.set_disconnect_callback(
        [&cancel_source, client_channel_id, saved_disconnect](Channel& ch) {
            if (ch.channel_id() == client_channel_id)
                cancel_source.request_cancellation();
            if (saved_disconnect)
                saved_disconnect(ch);  // 保留原有处理
        });
    auto disconnect_guard = ScopeGuard([&] {
        external_network_.set_disconnect_callback(std::move(saved_disconnect));
    });

    // ═══════ Step 1: 认证（无远程资源）═══════
    login::AuthLogin auth{};
    auth.request_id = rid;
    auth.username = request.username;
    auth.password_hash = request.password_hash;
    auth.auto_create = config().auto_create_accounts;

    auto auth_result = co_await rpc_call<login::AuthLoginResult>(
        rpc_registry_, *dbapp_channel_, auth, kPendingTimeout, token);
    if (!auth_result)
    {
        send_login_error(client_channel_id, LoginStatus::InternalError,
                         auth_result.error().message());
        co_return;  // dedup_guard 析构 → 移除用户名
    }
    if (!auth_result->success)
    {
        send_login_error(client_channel_id, auth_result->status);
        co_return;
    }

    // ═══════ Step 2: 分配 BaseApp（无远程资源）═══════
    login::AllocateBaseApp alloc{};
    alloc.request_id = rid;
    alloc.type_id = auth_result->type_id;
    alloc.dbid = auth_result->dbid;

    auto alloc_result = co_await rpc_call<login::AllocateBaseAppResult>(
        rpc_registry_, *baseappmgr_channel_, alloc, kPendingTimeout, token);
    if (!alloc_result || !alloc_result->success)
    {
        send_login_error(client_channel_id, LoginStatus::ServerFull);
        co_return;
    }

    // ═══════ Step 3: PrepareLogin（分配远程资源！）═══════
    auto session_key = SessionKey::generate();
    login::PrepareLogin prep{};
    prep.request_id = rid;
    prep.type_id = auth_result->type_id;
    prep.dbid = auth_result->dbid;
    prep.session_key = session_key;
    // ... 其他字段

    // ── Guard 3: PrepareLogin 回滚 ──
    // 此 guard 在 PrepareLogin 发送后创建
    // 析构时: 发送 CancelPrepareLogin → BaseApp abort checkout → DBApp release
    auto prepare_guard = ScopeGuard([&, addr = alloc_result->internal_addr] {
        if (auto* ch = network().find_channel(addr))
        {
            login::CancelPrepareLogin cancel;
            cancel.request_id = rid;
            (void)ch->send_message(cancel);
        }
        // 如果 channel 不存在（BaseApp 已断开），不需要发送取消:
        // BaseApp 有 8 秒超时自动清理 pending_logins_
        // DBApp 有 checkout_mgr_ 清理 + on_baseapp_death 兜底
    });

    auto prep_result = co_await rpc_call<login::PrepareLoginResult>(
        rpc_registry_, *baseapp_ch, prep, kPendingTimeout, token);
    if (!prep_result || !prep_result->success)
    {
        send_login_error(client_channel_id, LoginStatus::InternalError,
                         prep_result ? prep_result->error : "prepare timeout");
        co_return;  // prepare_guard 析构 → 发送 CancelPrepareLogin
    }

    // ═══════ Step 4: 成功！解除所有回滚 guard ═══════
    prepare_guard.dismiss();  // 不需要取消 PrepareLogin

    auto* client_ch = external_network_.find_channel(client_channel_id);
    if (!client_ch)
    {
        // 客户端已断连但 cancel token 尚未触发
        // prepare_guard 已 dismiss，需要手动回滚
        if (auto* ch = network().find_channel(alloc_result->internal_addr))
        {
            login::CancelPrepareLogin cancel;
            cancel.request_id = rid;
            (void)ch->send_message(cancel);
        }
        co_return;
    }

    login::LoginResult result{};
    result.status = LoginStatus::Success;
    result.session_key = session_key;
    result.baseapp_addr = alloc_result->external_addr;
    (void)client_ch->send_message(result);
    ++login_success_total_;
    // dedup_guard 析构 → 移除用户名（正常完成）
}
```

### 6.3 回滚保证矩阵

| 失败场景 | Guard 动作 | 回滚链 |
|---------|-----------|--------|
| Auth 超时/失败 | `dedup_guard` 析构 | 移除 `pending_by_username_` 条目 |
| Allocate 超时/失败 | `dedup_guard` 析构 | 同上 |
| PrepareLogin 超时 | `prepare_guard` 析构 | → `CancelPrepareLogin` → BaseApp `cancel_inflight_checkout` → `AbortCheckout` → DBApp `release_checkout` |
| 客户端断连 (Step 1-2) | `cancel_source` → token 取消 → `rpc_call` 返回 Cancelled → `dedup_guard` 析构 | 移除 `pending_by_username_` |
| 客户端断连 (Step 3) | `cancel_source` → token 取消 → `rpc_call` 返回 Cancelled → `prepare_guard` 析构 | → `CancelPrepareLogin` → 级联回滚 |
| LoginApp 进程关闭 | `PendingRpcRegistry::cancel_all()` + CoroutineOwner 析构 | 所有 guard 析构 → 级联回滚 |
| BaseApp 未响应 | `rpc_call` 超时 → `prepare_guard` 析构 | → `CancelPrepareLogin` (可能失败) → BaseApp 8 秒自动超时兜底 |

### 6.4 对比: 回调 vs 协程的回滚保证

| 方面 | 回调模式 | 协程 + RAII |
|------|---------|------------|
| 回滚触发 | 手动: 每个失败路径显式调用 cleanup | 自动: **编译器保证析构器运行** |
| 新增失败路径 | 需审查是否遗漏 cleanup 调用 | 自动被已有 guard 覆盖 |
| 代码位置 | 分散在 5+ handler + cleanup 函数 | 集中在协程函数内，guard 紧跟资源分配 |
| 正确性验证 | 需要逐路径人工审查 | `dismiss()` 只在成功路径调用，其余路径天然覆盖 |
| 超时处理 | 每 tick `cleanup_expired_*()` 扫描 | `rpc_call` 内置 timeout，超时即返回 |

### 6.5 兜底机制（不变）

协程化不改变下游进程的超时兜底逻辑:

- **BaseApp**: `cleanup_expired_pending_requests()` — 8 秒超时，自动
  `cancel_inflight_checkout`
- **BaseApp**: `rollback_prepared_login_entity()` — 10 秒超时，自动释放 checkout
- **DBApp**: `on_baseapp_death()` → `clear_all_for()` — BaseApp 崩溃时清理所有
  checkout
- **DBApp**: `on_abort_checkout()` — 幂等处理（`cleared_dbid` 防止重复释放）

这些机制作为协程回滚的安全网，即使 `CancelPrepareLogin` 消息丢失也能最终一致回收。

---

## 7. 对现有代码的修改

### 7.1 `ErrorCode` 新增 `Cancelled`

**修改文件:** `src/lib/foundation/error.hpp`

```cpp
enum class ErrorCode : uint32_t
{
    // ... 现有值 ...
    ChannelCondemned,

    // Coroutine error codes
    Cancelled,           // 新增: 协程被取消

    // Script error codes
    ScriptError,
    // ...
};
```

需要同步更新 `error.cpp` 中的 `error_code_name()` 函数。

### 7.2 `BinaryReader` 新增 `data()` 访问器

**修改文件:** `src/lib/serialization/binary_stream.hpp`

```cpp
class BinaryReader
{
public:
    // 新增: 获取底层数据 span（用于 pre-dispatch hook 构造 payload）
    [[nodiscard]] auto data() const -> std::span<const std::byte> { return data_; }

    // ... 现有方法不变 ...
};
```

### 7.3 `InterfaceTable` 添加 pre-dispatch hook

见第 5 节。

---

## 8. 文件清单

### 8.1 新增文件

```
src/lib/coro/
  CMakeLists.txt                # atlas_coro 库定义
  task.hpp                      # Task<T> 协程返回类型
  fire_and_forget.hpp           # FireAndForget 即发即忘协程
  cancellation.hpp              # CancellationToken / CancellationSource
  scope_guard.hpp               # ScopeGuard RAII 回滚守卫
  async_sleep.hpp               # 定时器 awaitable
  await_bg_task.hpp             # 线程池 awaitable
  rpc_call.hpp                  # RPC awaitable
  pending_rpc_registry.hpp      # RPC 注册表头文件
  pending_rpc_registry.cpp      # RPC 注册表实现

tests/unit/
  test_coro_task.cpp            # Task / FireAndForget 单元测试
  test_coro_rpc.cpp             # rpc_call / PendingRpcRegistry 测试
  test_coro_sleep.cpp           # async_sleep 测试
```

### 8.2 修改文件

```
src/lib/CMakeLists.txt                    # 添加 add_subdirectory(coro)
src/lib/foundation/error.hpp              # 添加 ErrorCode::Cancelled
src/lib/foundation/error.cpp              # 添加 error_code_name 映射
src/lib/serialization/binary_stream.hpp   # BinaryReader 添加 data()
src/lib/network/interface_table.hpp       # 添加 PreDispatchHook
src/lib/network/interface_table.cpp       # dispatch() 调用 hook
src/server/loginapp/loginapp.hpp          # 添加 rpc_registry_、协程方法
src/server/loginapp/loginapp.cpp          # 协程化登录流程，删除旧 pending 代码
tests/unit/CMakeLists.txt                 # 添加新测试
```

### 8.3 CMake 定义

```cmake
# src/lib/coro/CMakeLists.txt
atlas_add_library(atlas_coro
    SOURCES
        pending_rpc_registry.cpp
    DEPS
        atlas_foundation
        atlas_network
        atlas_serialization
)
```

大部分是 header-only 模板代码，只有 `pending_rpc_registry.cpp` 包含非模板实现。

---

## 9. 实现阶段

### Phase 1: 核心类型（基础设施）

1. 创建 `src/lib/coro/` 目录和 `CMakeLists.txt`
2. 实现 `Task<T>` 和 `Task<void>`（含 symmetric transfer）
3. 实现 `FireAndForget`
4. 实现 `ScopeGuard`
5. 实现 `CancellationToken` / `CancellationSource`
6. 添加 `ErrorCode::Cancelled`
7. 编写 `test_coro_task.cpp` 单元测试
8. 验证 MSVC + GCC/Clang 编译通过

**Phase 1 测试用例:**

```cpp
TEST(Task, BasicReturn)
{
    auto task = []() -> Task<int> { co_return 42; }();
    auto wrapper = [&]() -> Task<int> { co_return co_await task; }();
    // 验证结果为 42
}

TEST(Task, ChainedAwait)
{
    auto inner = []() -> Task<int> { co_return 1; };
    auto outer = [&]() -> Task<int> {
        auto a = co_await inner();
        auto b = co_await inner();
        co_return a + b;
    }();
    // 验证结果为 2
}

TEST(Task, VoidTask)
{
    bool executed = false;
    auto task = [&]() -> Task<void> { executed = true; co_return; }();
    auto wrapper = [&]() -> Task<void> { co_await task; }();
    // 验证 executed == true
}

TEST(FireAndForget, ImmediateExecution)
{
    bool executed = false;
    auto coro = [&]() -> FireAndForget { executed = true; co_return; };
    coro();
    // 验证 executed == true（因为 initial_suspend = suspend_never）
}

TEST(CancellationToken, BasicCancel)
{
    CancellationSource source;
    auto token = source.token();
    EXPECT_FALSE(token.is_cancelled());
    bool callback_fired = false;
    auto reg = token.on_cancel([&] { callback_fired = true; });
    source.request_cancellation();
    EXPECT_TRUE(token.is_cancelled());
    EXPECT_TRUE(callback_fired);
}

TEST(ScopeGuard, ExecutesOnDestruction)
{
    bool cleaned = false;
    { ScopeGuard guard([&] { cleaned = true; }); }
    EXPECT_TRUE(cleaned);
}

TEST(ScopeGuard, DismissPreventsExecution)
{
    bool cleaned = false;
    {
        ScopeGuard guard([&] { cleaned = true; });
        guard.dismiss();
    }
    EXPECT_FALSE(cleaned);
}
```

### Phase 2: Awaitable 原语

1. 实现 `async_sleep`
2. 实现 `await_bg_task`
3. 实现 `PendingRpcRegistry`
4. 修改 `InterfaceTable`（添加 pre-dispatch hook）
5. 修改 `BinaryReader`（添加 `data()` 访问器）
6. 实现 `rpc_call`
7. 编写 `test_coro_rpc.cpp` 和 `test_coro_sleep.cpp`

**Phase 2 测试用例:**

```cpp
TEST(AsyncSleep, BasicSleep)
{
    EventDispatcher dispatcher("test");
    dispatcher.set_max_poll_wait(Milliseconds(1));

    bool completed = false;
    auto coro = [&]() -> FireAndForget {
        auto result = co_await async_sleep(dispatcher, Milliseconds(10));
        EXPECT_TRUE(result.has_value());
        completed = true;
    };
    coro();

    // 驱动事件循环直到完成
    auto deadline = Clock::now() + Milliseconds(500);
    while (!completed && Clock::now() < deadline)
        dispatcher.process_once();
    EXPECT_TRUE(completed);
}

TEST(AsyncSleep, Cancellation)
{
    EventDispatcher dispatcher("test");
    dispatcher.set_max_poll_wait(Milliseconds(1));

    CancellationSource source;
    bool completed = false;
    ErrorCode result_code = ErrorCode::None;

    auto coro = [&]() -> FireAndForget {
        auto result = co_await async_sleep(dispatcher, Seconds(10), source.token());
        result_code = result ? ErrorCode::None : result.error().code();
        completed = true;
    };
    coro();

    // 立即取消
    source.request_cancellation();
    dispatcher.process_once();
    EXPECT_TRUE(completed);
    EXPECT_EQ(result_code, ErrorCode::Cancelled);
}

TEST(PendingRpcRegistry, BasicDispatch)
{
    EventDispatcher dispatcher("test");
    PendingRpcRegistry registry(dispatcher);

    bool reply_received = false;
    registry.register_pending(
        42,  // reply message ID
        100, // request_id
        [&](std::span<const std::byte> payload) { reply_received = true; },
        [&](Error) {},
        Seconds(5)
    );

    // 模拟收到回复: request_id=100
    uint32_t request_id = 100;
    std::vector<std::byte> payload(sizeof(request_id));
    std::memcpy(payload.data(), &request_id, sizeof(request_id));

    bool consumed = registry.try_dispatch(42, payload);
    EXPECT_TRUE(consumed);
    EXPECT_TRUE(reply_received);
}

TEST(PendingRpcRegistry, Timeout)
{
    EventDispatcher dispatcher("test");
    dispatcher.set_max_poll_wait(Milliseconds(1));
    PendingRpcRegistry registry(dispatcher);

    bool timed_out = false;
    registry.register_pending(
        42, 100,
        [&](std::span<const std::byte>) {},
        [&](Error err) { timed_out = (err.code() == ErrorCode::Timeout); },
        Milliseconds(10)
    );

    auto deadline = Clock::now() + Milliseconds(500);
    while (!timed_out && Clock::now() < deadline)
        dispatcher.process_once();
    EXPECT_TRUE(timed_out);
    EXPECT_EQ(registry.pending_count(), 0u);
}
```

### Phase 3: LoginApp 完整迁移

1. 在 `LoginApp` 中添加 `PendingRpcRegistry` 成员
2. 注册 pre-dispatch hook
3. 实现 `handle_login_coro()` 协程（含所有 scope guard）
4. 修改 `on_login_request()` 入口调用协程
5. 删除旧的 pending 状态机代码
6. 使用现有 `test_login_flow.cpp` 集成测试验证

**可删除的代码:**

- `PendingStage` 枚举
- `PendingLogin` 结构体
- `pending_` / `pending_by_username_` / `canceled_requests_` map
- `on_auth_login_result()` / `on_allocate_baseapp_result()` /
  `on_prepare_login_result()` / `on_checkout_entity_ack()` handler
- `cleanup_expired_logins()` 超时函数
- `cancel_prepare_login()` / `abandon_pending_login()` / `remove_pending()` 辅助函数
- 对应的 `register_typed_handler` 注册

**保留不变:**

- `LoginRequest` 的客户端 handler 注册（协程入口点）
- Rate limiting 逻辑
- 统计计数器
- 非登录相关的 handler

### Phase 4: 扩展（后续）

1. 数据库 Awaitable（包装 `IDatabase` 回调接口）
2. 协程 frame 池分配（`promise_type::operator new/delete`）
3. BaseApp 迁移（`pending_logins_` / `pending_force_logoffs_` 等）

---

## 10. 风险与缓解

### 10.1 MSVC 协程 codegen

**风险:** MSVC 的协程支持历史上有边界 bug（特别是 symmetric transfer 和异常处理）。

**缓解:**
- VS2022 17.4+ 对 symmetric transfer 支持已成熟
- Phase 1 单元测试覆盖所有关键路径（链式 await、异常传播、取消）
- 项目已使用 `/permissive-` 确保标准一致行为

### 10.2 协程 frame 悬空引用

**风险:** `FireAndForget` 协程在 `co_await` 处挂起时，如果持有者对象（LoginApp）
被销毁，frame 中的引用变为悬空。

**缓解:**
- `PendingRpcRegistry::cancel_all()` 在析构函数中调用，恢复所有挂起协程使其退出
- `rpc_call` awaiter 中的回调通过值捕获 `caller` handle，不持有 registry 引用
- 协程函数中通过值（非引用）捕获需要跨 `co_await` 存活的数据

### 10.3 request_id 碰撞

**风险:** `next_request_id_` 是 per-app 的 `uint32_t`，长时间运行可能绕回。

**缓解:**
- `PendingRpcRegistry` 用 `(MessageID, request_id)` 复合键，不同消息类型可复用
  同一 request_id
- RPC 超时（默认 10 秒）确保旧条目不会堆积
- 2^32 个 ID 在每秒 1000 次登录的负载下需要 ~50 天才会绕回

### 10.4 回调中重入

**风险:** `PendingRpcRegistry::try_dispatch()` 中的 `on_reply` 回调可能触发新的
`register_pending()` 调用（协程恢复后立即发下一个 RPC）。

**缓解:**
- `try_dispatch()` 先 `erase` map entry，再调用回调
- map 操作完成后才执行用户代码，避免迭代器失效

### 10.5 CancelPrepareLogin 消息丢失

**风险:** 协程的 `prepare_guard` 析构时发送 `CancelPrepareLogin`，但消息可能因
Channel 断开而无法送达。

**缓解:** 这与当前回调模式面临的问题完全相同，现有兜底机制（BaseApp 8 秒超时、
DBApp `on_baseapp_death`）提供最终一致性保证。协程化不改变也不弱化这些保证。
