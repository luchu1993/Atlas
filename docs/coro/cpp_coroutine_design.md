# C++20 协程基础设施

> 状态：✅ 已落地（`src/lib/coro/`）
> 关联：[C# 协程基础设施](csharp_coroutine_design.md) | [BigWorld RPC 参考](../bigworld_ref/BIGWORLD_RPC_REFERENCE.md)

---

## 1. 动机

跨进程多步异步流程（登录链、entity-RPC、DB 查询）若用回调 + 状态机表达，需要
`PendingStage` 枚举、`pending_` map、`request_id` 手工关联、`cleanup_expired_*`
扫描函数、分散的回滚 handler。每新增一步，错误路径就要走一遍人工审查。

C++20 stackless coroutine 把这套流程压回顺序代码，配合 RAII guard 由编译器保证
回滚一定执行。零外部依赖、单线程模型、协程在 `EventDispatcher` 主线程恢复 ——
不引入跨线程同步。

不在范围：多线程协程调度器、`std::execution` sender-receiver、把所有 handler
强制改为协程。单步 handler 仍走回调；只有多步流程才迁移。

---

## 2. 架构

```
┌──────────────────────────────────────────────────────────┐
│ 应用层（saga 协程）                                      │
│   LoginApp::HandleLoginCoro()                            │
│     co_await RpcCall<AuthLoginResult>(...)               │
│     co_await RpcCall<AllocateBaseAppResult>(...)         │
│     co_await RpcCall<PrepareLoginResult>(...)            │
│   每步分配远程资源后用 ScopeGuard 守住回滚路径           │
├──────────────────────────────────────────────────────────┤
│ 协程原语（src/lib/coro/）                                │
│   Task<T> / FireAndForget / CancellationToken            │
│   RpcCall / AsyncSleep / AwaitBgTask / ScopeGuard        │
│            │                                              │
│            ▼                                              │
│   PendingRpcRegistry  键 (MessageID, request_id)         │
│   按 channel 索引；channel 断开 → kReceiverGone          │
├──────────────────────────────────────────────────────────┤
│ 网络层（既有）                                           │
│   InterfaceTable::Dispatch()                             │
│     ├ TryPreDispatch(id, payload) → 命中即消费消息       │
│     └ 否则走原 handler 路径                              │
│   EventDispatcher：timer / poller / frequent-tasks       │
└──────────────────────────────────────────────────────────┘
```

`src/lib/coro/` 全部头文件以 `.h` 结尾；只有 `pending_rpc_registry.cc` 是实现
文件。CMake target `atlas_coro`，链接 `atlas_foundation` 和 `atlas_network`。

---

## 3. 核心类型

### 3.1 `Task<T>`（`task.h`）

惰性协程返回类型。`initial_suspend = suspend_always` 让 Task 在被 `co_await`
之前不执行；`final_suspend` 用 symmetric transfer awaiter 跳回 caller，链式
`co_await` 调用栈深度保持 O(1)。

正常返回值与异常合并存到 `std::variant<monostate, T, exception_ptr>`，
`get_result` 透明重抛。`Task` move-only —— frame 所有权唯一，析构时
`handle_.destroy()` 防止泄漏。

### 3.2 `FireAndForget`（`fire_and_forget.h`）

入站请求的 spawn 入口：每个客户端登录请求 spawn 一个独立协程，无人 `co_await`。
`initial_suspend = suspend_never` 立刻执行到第一个挂起点；`final_suspend =
suspend_never` 让 frame 在协程正常完成时自动销毁。挂起期间 frame 由恢复者
（timer 或 `PendingRpcRegistry`）持有的 `coroutine_handle` 维持。

`unhandled_exception()` 内部 catch 并日志记录，不传播到事件循环 —— 与既有
`SafeInvoke()` 行为一致。

### 3.3 `CancellationToken` / `CancellationSource`（`cancellation.h`）

协作式取消。`Source` 调用 `RequestCancellation()` 时遍历所有注册的回调
（每个 awaiter 注册一个：取消 timer、移除 pending entry、resume 协程），
随后协程在 `co_await` 点收到 `ErrorCode::kCancelled`，`co_return` 时栈展开
触发所有 scope guard。

`CancelRegistration` RAII 析构时反注册，避免 awaiter 销毁后回调悬空。
`Token::OnCancel(...)` 若 source 已取消则立刻同步调用 callback。

### 3.4 `ScopeGuard`（`scope_guard.h`）

通用 RAII 回滚守卫。`Dismiss()` 用于成功路径解除清理；其余路径（异常、提前
`co_return`、token 取消）析构器自动执行。`ATLAS_SCOPE_EXIT` 宏生成匿名实例。

---

## 4. Awaitable 原语

### 4.1 `AsyncSleep`（`async_sleep.h`）

`co_await AsyncSleep(dispatcher, delay, token)` 注册 timer + 取消回调。
返回 `Result<void>`：成功或 `kCancelled`。

### 4.2 `AwaitBgTask`（`await_bg_task.h`）

把 `work` lambda 提交到 `BgTaskManager` 线程池，主线程回调里 resume 协程。
`work` 在 worker 线程执行 —— 不得访问网络对象；resume 在主线程，可安全访问
任何对象。`void` 返回值有特化重载。

### 4.3 `RpcCall`（`rpc_call.h`）

核心 RPC awaiter。流程：

```
RpcCall<Reply>(registry, channel, request, timeout, token)
  ├ await_ready：token 已取消 → 立即返回 kCancelled
  ├ await_suspend：
  │   1. channel.SendMessage(request)；失败 → symmetric transfer 回 caller
  │   2. registry.RegisterPending(reply_id, request_id, on_reply, on_error,
  │                                timeout, channel)
  │   3. token.OnCancel(...) → registry.Cancel(handle)
  │   返回 std::noop_coroutine() 真正挂起
  └ await_resume：返回 Result<Reply>
      ├ 成功：反序列化的 Reply
      ├ 超时：Error{kTimeout}
      ├ 取消：Error{kCancelled}
      └ channel 断开：Error{kReceiverGone}
```

`Reply` 类型必须满足 `RpcReplyMessage` concept（含 `request_id` 字段）。
回调中先 `erase` map entry 再调用 callback，避免回调内部重新 `RegisterPending`
触发迭代器失效。

### 4.4 `PendingRpcRegistry`（`pending_rpc_registry.h/.cc`）

所有协程 RPC 共享一张注册表。键是复合 `(MessageID, request_id)`，值含
`on_reply` / `on_error` 回调、timeout timer、所属 channel。

```cpp
auto RegisterPending(MessageID reply_id, uint32_t request_id,
                     ReplyCallback on_reply, ErrorCallback on_error,
                     Duration timeout, Channel* channel = nullptr) -> PendingHandle;
auto TryDispatch(MessageID id, std::span<const std::byte> payload) -> bool;
void Cancel(PendingHandle handle);
void CancelByChannel(Channel* channel);   // 断开 → kReceiverGone
void CancelAll();                         // 进程关闭
```

`TryDispatch` 由 `InterfaceTable` 的 pre-dispatch hook 调用：从 payload 头 4 字节
读 little-endian `request_id`，命中则取消 timer、`erase` 后调用 `on_reply` 并返
回 true（消息被消费）；否则返回 false 走原 handler 路径。

**协议约定：** 所有可作为 RPC reply 的消息类型，`request_id`（uint32_t）必须是
`Serialize()` 写入的第一个字段。当前 `login_messages.h` / `dbapp_messages.h` /
`baseapp_messages.h` 全部消息满足此约定。

`CancelByChannel` 是 channel 断开时立即清算 —— 比等到默认 10s 超时快；
disconnect callback（`BaseApp` internal/external、`CellApp`、`LoginApp`）触达
即调。

### 4.5 `entity_rpc_reply.h`

通用 entity-RPC 回复消息。`Common::kEntityRpcReply = 102`（见
`network/message_ids.h`）。Reply payload：

```
[request_id u32 LE][error_code i32 LE]
  if error_code != 0: [error_msg VLE-len-prefixed UTF-8]
  if error_code == 0: [body 由调用方 deserializer 反序列化]
```

`SendSuccess(channel, request_id, body)` / `SendFailure(channel, request_id,
code, msg)` 由 receiver 续延 settled 时调用，沿原 channel 写回。
C# 端 `EntityRpcReplyHelpers.SendReplyOnComplete` 调入此路径完成跨语言 reply。

---

## 5. 与 InterfaceTable 的集成

`InterfaceTable` 暴露两个口子（`network/interface_table.h`）：

```cpp
using PreDispatchHook = std::function<bool(MessageID, std::span<const std::byte>)>;
void SetPreDispatchHook(PreDispatchHook hook);
auto TryPreDispatch(MessageID id, std::span<const std::byte> payload) -> bool;
```

`PendingRpcRegistry::TryDispatch` 在 hook 中调用；命中则消息被协程消费，
其余消息走原 handler。`BinaryReader::Data()` 暴露底层 span 让调用方在不消耗
读位置的前提下构造 payload view。

---

## 6. 回滚机制 — 分布式 Saga

登录链跨 4 个进程，Step 3 (PrepareLogin) 分配的远程资源在失败时必须回滚
（BaseApp entity + DBApp checkout）。协程化的策略：**每分配一次远程资源就
紧跟一个 RAII guard，成功路径 `Dismiss()`，其余路径析构器自动回滚。**

```cpp
auto dedup_guard = ScopeGuard([&] { pending_by_username_.erase(username); });
// ... 用户名去重 ...

// Step 1: Auth（无远程资源）
auto auth = co_await RpcCall<AuthLoginResult>(...);
if (!auth || !auth->success) co_return;  // dedup_guard 析构

// Step 2: Allocate（无远程资源）
auto alloc = co_await RpcCall<AllocateBaseAppResult>(...);
if (!alloc || !alloc->success) co_return;

// Step 3: PrepareLogin（分配远程资源）
auto prepare_guard = ScopeGuard([&, addr] {
  if (auto* ch = network().FindChannel(addr)) {
    CancelPrepareLogin cancel{.request_id = rid};
    (void)ch->SendMessage(cancel);
  }
});
auto prep = co_await RpcCall<PrepareLoginResult>(...);
if (!prep || !prep->success) co_return;  // prepare_guard 析构 → 级联回滚

prepare_guard.Dismiss();  // 成功路径解除
// ... 回客户端 LoginResult ...
```

| 失败场景 | 触发链 |
|---------|--------|
| Auth/Allocate 失败 | `dedup_guard` 析构 → 移除 username |
| PrepareLogin 超时/失败 | `prepare_guard` 析构 → `CancelPrepareLogin` → BaseApp 释放 entity → DBApp 释放 checkout |
| 客户端断连 | `cancel_source` 触发 → 所有 awaiter 收 kCancelled → 栈展开触发对应 guard |
| Channel 断开 | `PendingRpcRegistry::CancelByChannel` → `kReceiverGone` reply 立即返回 |
| 进程关闭 | `CancelAll()` → 全部协程退出 → guard 链路全部回滚 |

**兜底机制不变：** BaseApp `cleanup_expired_pending_requests()`、
`rollback_prepared_login_entity()`、DBApp `OnBaseAppDeath()` 提供最终一致性，
即使 `CancelPrepareLogin` 消息因 channel 提前断开未送达，下游也会自动收回
资源。协程化把回滚集中到协程函数内、由编译器保证执行；并未弱化下游兜底。

---

## 7. 错误码

`foundation/error.h` `ErrorCode` 枚举包含：

- `kCancelled` — 协程被取消（用户 token / 进程 shutdown）
- `kReceiverGone` — 等待 reply 的 channel 断开（`CancelByChannel`）
- `kTimeout` — 超时

`error.cc::ErrorCodeName()` 含对应字符串映射。

---

## 8. 设计要点小结

| 决策 | 原因 |
|------|------|
| `Task` 惰性启动 | 与 `FireAndForget` 形成"只挂起 / 立即跑"的明确二分；用户决定何时拉起 |
| symmetric transfer | 链式 `co_await` 不增长调用栈 |
| 单 dispatcher 单线程 | 无锁、无屏障、`co_await` 续延总在主线程 |
| `(MessageID, request_id)` 复合键 | 不同消息类型可独立编号 ID 空间，单类型超时 10s 上限内不会回绕 |
| `request_id` 写在 payload 首字段 | pre-dispatch hook 不需要反序列化整个消息即可路由 |
| `Channel*` 索引 | 断开立即 `kReceiverGone`，避免 10s 卡顿 |
| RAII guard 替代 cleanup | 编译器保证回滚执行；新增失败路径自动覆盖 |
