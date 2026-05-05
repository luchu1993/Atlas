# C# 协程基础设施

> 状态：✅ 已落地
> 关联：[C++ 协程基础设施](cpp_coroutine_design.md) | [脚本架构](../scripting/native_api_architecture.md)

---

## 1. 动机

服务端 `Atlas.Runtime` 在嵌入式 CoreCLR 上运行；客户端 `Atlas.Client.Unity`
在 Unity 主线程运行。两端业务代码几乎都是 C#，且需要相同的异步语义：
入口 RPC 等待远端回复、等待若干 tick / 一段游戏时间、`WhenAll`/`WhenAny`
汇合、客户端断线 / Entity 销毁触发协作式取消、后台线程池工作完成回主线程。

`System.Threading.Tasks.Task` 满足语义但每次 `async` 调用都堆分配
`AsyncStateMachineBox` + `Task` + `MoveNextRunner`，`SynchronizationContext`
post 装箱 `SendOrPostCallback`，`ConfigureAwait(false)` 又破坏"恢复必须回主线
程"的不变量。60 fps 客户端、5000 在线 cellapp 每帧几百次 `await` —— 用
`Task` 等于把 GC 压力焊死在帧时间上。

`AtlasTask` / `AtlasTask<T>` 是稳态零 GC、单线程、服务端与客户端共用的协程
返回类型，对标 [Cysharp/UniTask](https://github.com/Cysharp/UniTask) 与
[egametang/ET](https://github.com/egametang/ET) `ETTask`。

设计原则：

1. 服务端 / 客户端 API 完全一致，平台差异（计时器、PlayerLoop）藏在
   `IAtlasLoop` 后面
2. 稳态零 GC：`AtlasTask` / builder / awaiter 都是 struct；状态后端
   `IAtlasTaskSource<T>` 池化复用
3. 单线程模型：所有续延在主线程恢复，无锁、无 `ExecutionContext` 复制
4. 协作式取消是一等公民，层级取消默认开启
5. 与 C++ 协程同语义 —— 服务端 RPC 复用 C++ 侧 `PendingRpcRegistry`，
   两端共享 `(MessageID, request_id)` 匹配
6. 不复刻不必要的 BCL 表面：无多线程并发 await、无 `ConfigureAwait`、无
   `TaskScheduler`

不在范围：多线程 scheduler、`IAsyncEnumerable<T>`、异步 LINQ。

---

## 2. 架构

```
┌──────────────────────────────────────────────────────────────┐
│ 业务脚本                                                     │
│   public async AtlasTask<int> LoadProfile(...)               │
│   var r = await mailbox.GetLevel(ct: this.LifecycleCancellation);│
├──────────────────────────────────────────────────────────────┤
│ 协程类型层（Atlas.Shared/Coro/）                             │
│   AtlasTask / AtlasTask<T>           readonly struct         │
│   AsyncAtlasTaskMethodBuilder<T>     struct，懒 box          │
│   IAtlasTaskSource<T>                池化后端                │
│   AtlasTaskCompletionSourceCore<T>   token-safe 状态核       │
├──────────────────────────────────────────────────────────────┤
│ Awaitable                                                    │
│   Delay  Yield  WhenAll  WhenAny  WaitUntil  FromBgWork      │
│   AtlasRpc.Await<T>                                          │
├──────────────────────────────────────────────────────────────┤
│ Cancellation                                                 │
│   AtlasCancellationSource / Token  (managed)                 │
│   AtlasShutdownToken               (process-wide)            │
├──────────────────────────────────────────────────────────────┤
│ Platform glue                                                │
│  ┌──────────────────────┐  ┌──────────────────────────┐      │
│  │ Server / Desktop     │  │ Unity                    │      │
│  │ ManagedAtlasLoop     │  │ UnityLoop (PlayerLoop)   │      │
│  │ NativeRpcRegistry    │  │ ManagedRpcRegistry       │      │
│  │   → coro_bridge      │  │                          │      │
│  │   → PendingRpcReg.   │  │                          │      │
│  └──────────────────────┘  └──────────────────────────┘      │
└──────────────────────────────────────────────────────────────┘
```

`AtlasTask` 类型本身在 `Atlas.Shared/Coro/` 下，目标 `netstandard2.1`，
二进制兼容 .NET 服务端与 Unity 2022 IL2CPP 客户端。

---

## 3. 核心类型

### 3.1 `AtlasTask` / `AtlasTask<T>`

`readonly struct`，标 `[AsyncMethodBuilder(typeof(AsyncAtlasTaskMethodBuilder<>))]`。
非泛型 `AtlasTask` 复用 `IAtlasTaskSource<AtlasUnit>` 后端，省一份非泛型接口。
同步完成路径（`FromResult` / `FromException` / `FromCanceled`）不持有 source；
异步路径用 `short _token` 字段防止池化 source 复用时 ABA。Awaiter 实现
`ICriticalNotifyCompletion`，状态机走 `UnsafeOnCompleted` 跳过
`ExecutionContext` 复制。

### 3.2 `IAtlasTaskSource<T>` / `AtlasTaskCompletionSourceCore<T>`

```csharp
public interface IAtlasTaskSource<out T>
{
    AtlasTaskStatus GetStatus(short token);
    void OnCompleted(Action<object?> continuation, object? state, short token);
    T GetResult(short token);
    short Version { get; }
}
```

核心不变量两条：

- **ABA 防护**：`Reset()` 递增 `_version`；`GetStatus` / `GetResult` /
  `OnCompleted` 校验 token，过期 token 抛 `InvalidOperationException`
- **Pending → Completed 恰好一次**：`Interlocked.Increment` 排队第一个赢的
  写入 result 后 release 给所有线程。多线程入口仅限后台 worker 完成回写
  （如 `FromBgWork`）；纯主线程路径分支预测器保持热路径不变

### 3.3 `AsyncAtlasTaskMethodBuilder<T>`

零分配关键。两条路径：

- **同步走完**：result / exception 暂存在 builder（struct）字段，不借 box
- **第一次挂起**：`EnsureBox<TStateMachine>(ref sm)` 从 `TaskPool<T>` 借出
  `AtlasTaskSourceBox<TStateMachine, T>`，状态机以 `TStateMachine` 原生
  类型存放（双泛型避免装箱）

| 项 | BCL `AsyncTaskMethodBuilder<T>` | `AsyncAtlasTaskMethodBuilder<T>` |
|----|---------------------------------|----------------------------------|
| 同步完成 | `Task.FromResult`，部分缓存 | 零分配 |
| 异步完成 | 分配 `AsyncStateMachineBox` + `Task` | 借 `AtlasTaskSourceBox` |
| ExecutionContext | 复制 | 跳过（单线程模型） |
| 完成后 | Task 留给 GC | Box 归还池 |

### 3.4 `AtlasCancellationToken` / `AtlasCancellationSource`

```csharp
public sealed class AtlasCancellationSource : IDisposable
{
    public AtlasCancellationToken Token { get; }
    public bool IsCancellationRequested { get; }
    public void Cancel();
    public AtlasCancellationSource CreateLinked();
}

public readonly struct AtlasCancellationToken
{
    public static AtlasCancellationToken None => default;
    public bool IsCancellationRequested { get; }
    public bool CanBeCanceled { get; }
    public CancelRegistration Register(Action<object?> callback, object? state);
    public void ThrowIfCancellationRequested();
}
```

主线程模型 —— 所有状态访问无锁；取消传播是直接同步调用，不走
`ExecutionContext`。`CreateLinked()` 单父零分配。`AtlasShutdownToken` 是进程
级一次性广播，host bootstrap 时 `Install`。

| 项 | BCL `CancellationToken` | `AtlasCancellationToken` |
|----|---------------------|--------------------------|
| 注册 callback | 分配 `CancellationTokenRegistration` | 借 list slot，归还回收 |
| Linked source | `CreateLinkedTokenSource(params)` 总分配 | 单父零分配 |
| 取消传播 | 走 `ExecutionContext` | 直接同步调用（主线程） |

**Native lifecycle 桥：** `ServerEntity.LifecycleCancellation`（§6.3）在
destroy / shutdown / hot-reload 时由 C# 侧主动 cancel；CellApp offload 之前
由 C++ `CellApp::TickOffloadChecker` 通过 `NativeCallbackTable.entity_lifecycle_cancel`
反向调入 C# 触发同一 source。Channel 断开侧由 C++
`PendingRpcRegistry::CancelByChannel` 覆盖 —— 该 channel 上所有 pending
立即收 `kReceiverGone` reply，无需经 managed cancel。

---

## 4. Awaitable 原语

| 原语 | 行为 |
|---|---|
| `AtlasTask.Delay(ms, ct)` | 服务端 / 桌面端走 .NET ThreadPool Timer → `_mainQueue` 回主线程；Unity 端 PlayerLoop 累计 `Time.unscaledDeltaTime` |
| `AtlasTask.Yield()` | 通过 `IAtlasLoop.PostNextFrame` 把续延入主线程队列，下一帧恢复，零分配 |
| `AtlasTask.WhenAll` | 2 / 3-arity（独立 source 类型避免 array）+ `IEnumerable` 兜底 |
| `AtlasTask.WhenAny` | 返回胜出索引（带 `T` 时 `(index, value)`）；`Interlocked.CompareExchange` 单赢者，落败错误 silently drop |
| `AtlasTask.WaitUntil` | 每帧 poll predicate；`WaitUntil<TState>(...)` 重载显式传 state 避免闭包，热路径必须用 |
| `AtlasTask.FromBgWork` | `ThreadPool.UnsafeQueueUserWorkItem` 跑 worker，结果通过 `IAtlasLoop.PostMainThread` 回主线程；`work` lambda 不得访问 Unity API / Entity 状态 |

### 4.1 RPC 等待

不变量：服务端 C# 调用 native RPC（如 BaseApp → DBApp）**必须复用** C++ 侧
`PendingRpcRegistry` —— 同一 reply 不能被两套注册表竞争消费。

抽象通过 `IAtlasRpcRegistry`：

| 实现 | 用途 | 后端 |
|------|------|------|
| `NativeRpcRegistry` | 服务端 | PInvoke 到 C++ `coro_bridge`，复用 `PendingRpcRegistry` |
| `ManagedRpcRegistry` | 客户端 / 测试 | 纯 C# 字典 + .NET Timer |

`AtlasRpcSource<T>` 是池化 source。Reply payload 协议固定为
`[request_id u32][error_code i32]([error_msg vle-string]|[body])`，框架自己
消费 header 后把 `SpanReader` 交给 user deserializer。所有失败路径走
`ErrorMapper` 返回 `RpcReply<T>`，**永不抛** —— 调用方天然单分支。

业务侧主入口由生成器 emit 的 mailbox 代理（§6）：

```csharp
var r = await avatar.Base.LoadProfile(dbid, ct: this.LifecycleCancellation);
if (r.IsOk) Use(r.Value);
else if (r.IsBusinessError) HandleBiz(r.Error);
else if (r.Error == RpcErrorCodes.Timeout) Retry();
```

手动入口（已有非 entity-rpc 协议的 `MessageID`）：

```csharp
var requestId = AtlasRpc.NextRequestId();
var task = AtlasRpc.Await<Profile>(
    replyId: MessageIds.EntityRpcReply, requestId,
    deserializer: static (ref SpanReader r) => RpcReply<Profile>.Ok(Profile.Deserialize(ref r)),
    timeoutMs: 10_000, ct: ct);
SendLoadProfileRequest(dbid, requestId);
return await task;
```

C++ 侧 `clrscript/coro_bridge.cc` 在 reply / 超时 / 取消时通过函数指针回调 C#
`OnRpcComplete`，把 `GCHandle` 还原成 `IAtlasRpcCallback`。`GCHandle` 由
`Atlas.Runtime/Coro/GCHandlePool` 池化（`[ThreadStatic] Stack<GCHandle>`，
bounded 256），5000 在线 × 5 RPC/s 持续负载 GCHandle 分配速率近零。Payload
buffer 来自 native，C# 用 `ReadOnlySpan<byte>` 视图反序列化 —— 整条路径无
`byte[]` 分配。

---

## 5. 与 C++ 协程同语义

两端共享同一异步模型 —— 都是 stackless、都在主线程恢复、都通过
`(MessageID, request_id)` 匹配 RPC reply、都用 RAII / `using` 触发回滚。
登录链等 saga **可任意切换实现语言而行为不变**。

| 概念 | C++ | C# |
|------|-----|----|
| 协程返回 | `Task<T>` / `FireAndForget` | `AtlasTask<T>` / `Forget()` 扩展 |
| 等待 RPC | `co_await RpcCall<Reply>(...)` | `await mailbox.SomeRpc(...)` |
| 取消传播 | `CancellationToken` | `AtlasCancellationToken` |
| 回滚 guard | `ScopeGuard` + `Dismiss()` | `try/finally` + `using` |
| RPC 注册表 | `PendingRpcRegistry` | C# rent source 注册到同一 native registry（服务端）/ C# registry（客户端） |
| 错误返回 | `Result<T,E>` | 抛 `AtlasException` 或返回 `AtlasResult<T>` / `RpcReply<T>` |

服务端 RPC 共享同一 native 注册表 —— `PendingRpcRegistry` 不区分调用来源，
注册接口只接受 success / failure 两个 callable。C++ 协程注册时传 `co_await`
续延 lambda；C# 注册时由 `coro_bridge` 包一层 lambda 把 managed `GCHandle`
和 reply payload 转给 C# 函数指针。`MessageID` 全局唯一（见
`Atlas.Shared/Protocol/MessageIds.cs`），生成器保证两侧对齐。

---

## 6. Entity-RPC reply 协议

`<base_methods>` / `<cell_methods>` 上 `<method reply="...">` 让
`await mailbox.Method(...)` 成为一等公民 —— 调用方拿 `AtlasTask<RpcReply<T>>`，
接收方写 partial 返回 `RpcReply<T>.Ok / Fail`，框架负责 wire 编解码、续延
reply 写回、错误翻译、LifecycleCancellation 联动。

设计原则：

- **单一错误模型** —— 框架 / 业务 / 远端异常都进 `RpcReply<T>` 字段，永不
  需 `try/catch`
- **接收方写法自然** —— `partial` 返回 `AtlasTask<RpcReply<T>>`，业务错误
  `return RpcReply<T>.Fail(...)`
- **Lifecycle-aware** —— 不假装跨进程迁移状态机；offload / destroy /
  hot-reload 显式 cancel + 业务重试
- **兼容性** —— netstandard2.1 / Unity 2022 (C# 9) / IL2CPP AOT 全栈，无
  `static abstract`、无运行期反射
- **复用现有基建** —— `PendingRpcRegistry` / `coro_bridge` /
  `AtlasRpcSource` / `RpcArgCodec` / `.def` 解析全部沿用

### 6.1 `.def` 语法

`<method>` 加 `reply` 属性，类型规则与 `arg type=` 完全一致：

```xml
<method name="GetLevel" reply="int32"/>
<method name="GetTitles" reply="list[string]"/>
<method name="LoadProfile" reply="ProfileResult">
  <arg name="dbid" type="int64"/>
</method>
<method name="QueryStats" exposed="own_client" reply="StatsReply">
  <arg name="kind" type="int32"/>
</method>
```

校验规则（生成器 emit error）：

- `reply` 类型规则 = `<arg type=...>` 类型规则（primitive / struct /
  `list[T]` / `dict[K,V]` / `EntityRef`）
- `<client_methods>` 下不允许 `reply`（client 是接收方而非发起方）
- 同一 `<method>` 不能多次 `reply`
- reply 引用的 struct 必须前向兼容（新版仅在末尾追加字段）

### 6.2 `RpcReply<T>` 错误模型

调用方拿到的永远是 `RpcReply<T>`，**单分支模式覆盖所有失败**：

```csharp
public readonly struct RpcReply<T>
{
    public int Error { get; }                 // 0 = success
    public string? ErrorMessage { get; }
    public bool IsOk             => Error == 0;
    public bool IsBusinessError  => Error > 0;
    public bool IsFrameworkError => Error < 0;
    public T Value { get; }                   // 非 Ok 抛 InvalidOperationException
    public bool TryGetValue(out T value);

    public static RpcReply<T> Ok(T v);
    public static RpcReply<T> Fail(int code, string? m);

    // Receiver: return RpcReply<T>.Ok(x) 直接转 AtlasTask
    public static implicit operator AtlasTask<RpcReply<T>>(RpcReply<T> reply);
}

public static class RpcErrorCodes
{
    public const int Timeout          = -1;
    public const int Cancelled        = -2;   // 含 offload / destroy / hot-reload
    public const int RemoteException  = -3;   // 接收方未捕获异常
    public const int ReceiverGone     = -4;   // entity / channel / process 没了
    public const int SendFailed       = -5;
    public const int MethodNotFound   = -6;   // 接收方 .def 版本不识别
    public const int PayloadMalformed = -7;
    // 业务错误用 > 0
}
```

框架错误消息一律走 `RpcFrameworkMessages` interned literal，避免每次
`enum.ToString()` 分配。派发器对**未捕获**异常翻译成 `RemoteException`
（截断 `ex.ToString()` 至 512 字符防 GC 噩梦）；业务错误用户用
`RpcReply.Fail(positiveCode, msg)` 显式返回，零异常分配。

`OrThrow` 扩展用于把非 Ok reply 转 `RpcException`（throw 风格）：

```csharp
int level = await avatar.Base.GetLevel().OrThrow();
```

**默认风格：** C# 业务代码默认走异常传播（与 Task / UniTask 一致）。
`AtlasResult<T, TError>` 是可选返回类型，给热路径或跨边界一侧使用 ——
服务端 ScriptApp 的请求处理与上游 C++ `Result<T, E>` 风格保持一致。
Entity-RPC 是 C# 协程层**唯一不抛异常**的路径。

### 6.3 Lifecycle 取消

`ServerEntity` 暴露 `LifecycleCancellation` token，**三种事件**触发 cancel：

| 事件 | 触发 |
|---|---|
| Offload | CellApp entity 跨 cell 迁移前，`EntityManager.OnEntityWillOffload` |
| Destroy | entity 被销毁（玩家下线、despawn）前 |
| Hot reload | `HotReloadManager.SerializeAndUnload` 前必须 drain 所有 in-flight RPC |

```csharp
public abstract class ServerEntity
{
    private readonly AtlasCancellationSource _lifecycle = new();
    public AtlasCancellationToken LifecycleCancellation => _lifecycle.Token;
    internal void TriggerLifecycleCancellation() => _lifecycle.Cancel();
}
```

业务代码处理（与其他失败完全对称）：

```csharp
var r = await SomeRpc(ct: this.LifecycleCancellation);
if (r.IsFrameworkError && r.Error == RpcErrorCodes.Cancelled) return 0;
if (r.IsOk) return r.Value * 2;
return -1;
```

| 进程 | offload? | destroy 频率 | hot reload? |
|---|---|---|---|
| `BaseApp` entity | ❌ | 玩家下线触发 | ✅ |
| `CellApp` entity | ✅ 频繁 | 频繁 | ✅ |
| `LoginApp` 等 ManagerApp | ❌ | n/a | ✅ |

**Hot reload 必须 drain 完才能卸载 assembly** —— 否则状态机引用旧
`ScriptLoadContext` 的类型，resume 时崩。`HotReloadManager.SerializeAndUnload`
在 serialize 之前 trigger 所有 entity 的 LifecycleCancellation，drain
`ManagedAtlasLoop` 续延队列至空（最多 16 轮），再走原 serialize / clear /
unload 路径。

### 6.4 协议层

| 项 | 内容 |
|---|---|
| MessageID | `Common::kEntityRpcReply = 102`（`network/message_ids.h`）；C# 镜像 `MessageIds.EntityRpcReply` |
| 请求 payload | `[request_id u32 LE][args...]`；rpc_id 顶位 `kReplyBit` 标记 expects-reply |
| Reply payload | `[request_id u32 LE][error_code i32 LE]`<br>`if error_code != 0:` `[error_msg VLE-len-prefixed UTF-8]`<br>`if error_code == 0:` `[body]` |
| `kReplyBit` | bit 31（`RpcIdEncoder.kReplyBit = 1u << 31`）；slot_idx 缩为 7 bit（bits 24–30，最大 127 component slot） |
| 反向路由 | dispatcher 接受 `IntPtr replyChannel` 参数，由 `BaseApp/CellApp` 收消息时透传 `Channel*`；接收方续延 settled 后 `NativeApi.SendEntityRpc{Success,Failure}` 沿同一 channel 回写 |
| Channel 断开处理 | `PendingRpcRegistry::CancelByChannel(Channel*)`；BaseApp internal/external + CellApp + LoginApp 在 disconnect callback 中调用，立即 cancel 该 channel pending → `RpcErrorCodes.ReceiverGone` |

### 6.5 异常情况覆盖

| 类别 | 触发 | 框架处理 |
|---|---|---|
| Entity destroy 中 in-flight | 调用方 / 接收方 entity `Destroy()` | `LifecycleCancellation` → `Cancelled`；接收方已 destroy 时 dispatcher 回 `ReceiverGone` |
| Hot reload 中 in-flight | `HotReloadManager.SerializeAndUnload` | 先 cancel 所有 entity LifecycleCancellation，drain 续延队列至空，再卸载 |
| Channel 断开 | RUDP / TCP peer 进程崩 | `PendingRpcRegistry` 按 channel 索引；断开立即 cancel 该 channel pending → `ReceiverGone` |
| 进程崩溃 / reviver 重启 | machined `DeathNotification` | 同 channel 断开路径 |
| `.def` 版本错配 | caller v2 / receiver v1 调到不存在方法 | dispatcher 回 `MethodNotFound` |
| Payload 格式错位 | 字段数 / 类型不一致 | SpanReader 越界 → `PayloadMalformed` |
| 重入 RPC（A 调 B，B 反向调 A） | 业务依赖循环 | 单线程模型保证无真并发；用户须接受逻辑并发或自加锁字段 |
| Late reply（timeout 后到达） | 网络抖动 / RUDP 重传 | registry 找不到 entry，silently drop |
| `request_id` 32 位回绕 | 50 天 @ 10k req/s | 10s 超时上限保证不会冲突 |
| Duplicate reply | RUDP 重传 | 第一次 dispatch 后 entry 已删，重复包 silently drop |

### 6.6 Analyzer

`RpcReplyValueAccessAnalyzer`（`ATLAS_RPC001`）：访问 `RpcReply<T>.Value`
之前必须检查 `IsOk`，否则编译期警告。

---

## 7. 平台 glue：`IAtlasLoop`

`IAtlasLoop` 抽象出主线程队列 + timer + 帧计数（`Atlas.Shared/Coro/IAtlasLoop.cs`）。
`AtlasLoop.Current` 是进程级单实例，host bootstrap 时 `Install`。

| 实现 | 用途 | 后端 |
|------|------|------|
| `ManagedAtlasLoop` | 服务端 / 桌面客户端 | .NET ThreadPool Timer + `ConcurrentQueue`，宿主每帧 `Drain()` |
| `UnityLoop` | Unity 客户端 | PlayerLoop 注入（思路照搬 UniTask `PlayerLoopHelper`） |
| `TestLoop` | 单测 | 完全确定性手动推进，不依赖真实时钟 |

---

## 8. 池化策略

所有可池化对象走单一 `TaskPool<T>`（`Atlas.Shared/Coro/Pool/TaskPool.cs`）
—— `[ThreadStatic] Stack<T>`，`MaxSize = 256`，借/还 O(1)，单线程模型下无锁。
当前接入：

| 类型 | 用途 |
|------|------|
| `AtlasTaskSourceBox<TStateMachine, T>` | builder 挂起时承载状态机 + source |
| `AtlasRpcSource<T>` | 单次 RPC 等待 |
| `WaitUntilAwaitable` | predicate poll |

池满 fallback 到 `new` —— 不抛、不阻塞。其他 awaitable（`WhenAll` /
`WhenAny` / `BgWorkSource` / `DelayAwaitable`）每次 await 直接 `new`，
后续若 profiler 显示热点再挂入池。

GC 验证靠 `tests/csharp` 里的 stress 用例 + `Atlas.Runtime.Diagnostics`
里的运行期监控。

---

## 9. 已知限制

- **CellApp 端发起 entity-rpc**：`CellAppNativeProvider::CoroRegisterPending`
  仍是基类 no-op，cellapp 内的 `await someEntity.Base.Method(...)` 注册不到
  native registry。接收 reply 路径（cellapp 作为被叫方）已通；**主动 await
  仅 BaseApp / LoginApp 可用**。
- **Analyzer 规则**：仅落地 `ATLAS_RPC001`。"CellApp entity 内 await 必须显
  式传 ct"、"`FromBgWork` lambda safety" 规则未实现。

---

## 10. 与 ET / UniTask 的关系

借鉴：UniTask 的 `IAtlasTaskSource<T>` 接口形态、source pool 模式、PlayerLoop
注入；ET 的单线程模型与层级 cancellation。差异：UniTask 仅 Unity 客户端，无
native 互通；Atlas 服务端必须经 `PendingRpcRegistry` 桥接 C++。ET `ETTask`
无精细 source pool；Atlas 借 UniTask 的 source pool 让两端同步受益。Atlas
独有"C++ 与 C# 协程同语义" —— saga RAII、`request_id` 约定、超时 timer 同源
都来自 C++ 侧已落地的设计。
