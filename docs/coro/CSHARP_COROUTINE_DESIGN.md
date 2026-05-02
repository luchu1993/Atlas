# C# 协程基础设施设计

> 日期: 2026-05-02（设计）/ 2026-05-02（Phase 1-3 落地）
> 状态: ✅ Phase 1-3 已落地（核心类型 / 平台 glue / RPC bridge）；Phase 4 业务迁移待启动
> 关联: [C++ 协程基础设施设计](CPP_COROUTINE_DESIGN.md) | [Phase 12 客户端 SDK](../roadmap/phase12_client_sdk.md) | [脚本架构](../scripting/native_api_architecture.md)

---

## 1. 背景与动机

Atlas 服务端通过 CoreCLR 把 `Atlas.Runtime` 嵌入 C++ 进程；客户端用 Unity，
`Atlas.Client.Unity` 与游戏脚本同样运行在主线程。无论哪一端，绝大多数业务代码
都是 C#，需要表达同样的异步语义:

- 入口 RPC 等待远端回复（登录、跨进程调用、DB 查询）
- 等待若干 tick / 一段游戏时间 / 等待资源加载
- 多个等待并发汇合（`WhenAll` / `WhenAny`）
- 客户端断线、Entity 销毁、玩家退出 → 协作式取消
- 后台线程池工作完成后回主线程继续

`System.Threading.Tasks.Task` 满足语义但成本高: 每次 `async` 调用都堆分配
`AsyncStateMachineBox`、`Task` 对象、`MoveNextRunner`，且 `SynchronizationContext`
post 路径要装箱 `SendOrPostCallback`，`ConfigureAwait(false)` 又破坏了
"恢复必须回主线程" 的不变量。一个稳定 60 fps 的 Unity 客户端、4000 在线的
cellapp，每帧产生几百次 `await` —— 用 `Task` 等于把 GC 压力直接焊死在帧时间上。

本方案设计 `AtlasTask` / `AtlasTask<T>` —— **服务端与客户端共用、稳态零 GC、
单线程模型**的协程基础设施，对标 [Cysharp/UniTask](https://github.com/Cysharp/UniTask)
（Unity 端零分配 Task）和 [egametang/ET](https://github.com/egametang/ET) 的
`ETTask`（服务端单线程协程）。

### 1.1 设计原则

1. **服务端 / 客户端 API 完全一致** —— 相同的 `AtlasTask` 类型、相同的
   awaitable、相同的取消语义。差异（计时器底层、PlayerLoop 钩子）藏在
   `AtlasLoop` 抽象后面。
2. **稳态零 GC** —— `AtlasTask` 是 `readonly struct`，`AsyncMethodBuilder`
   是 `struct`，awaiter 是 `struct`；状态后端 (`IAtlasTaskSource<T>`) 池化复用，
   首次 `await` 才分配，完成后归还到池。
3. **单线程模型** —— 所有 `await` 续延都在主线程恢复（服务端 dispatcher 主线程，
   客户端 Unity 主线程）。无锁、无内存屏障；`ConfigureAwait` 在这里没有意义。
4. **协作式取消是一等公民** —— 每个 `AtlasTask` 都接受 `AtlasCancellationToken`，
   层级取消（父 token 取消时所有子 token 自动取消）默认开启。
5. **与 C++ 协程一致语义** —— 服务端的 C# RPC 协程通过 native bridge 复用
   `src/lib/coro/PendingRpcRegistry`，C++ 与 C# 共享同一套 (MessageID, request_id)
   匹配机制；C# 端只是注册 `IAtlasTaskSource` 替代 C++ 的 `coroutine_handle`。
6. **不复刻 .NET BCL 不必要的部分** —— 不支持多线程并发 await（永远主线程恢复）、
   不支持 `ConfigureAwait`、不支持 `TaskScheduler`。这些选择换来更小的状态机、
   更短的代码路径、更可预测的性能。

### 1.2 不在本方案范围内

- 多线程任务调度器（保持单主线程模型）
- `IAsyncEnumerable<T>` / `await foreach` —— 第一阶段不实现，后续若有诉求按
  UniTask 的 `IUniTaskAsyncEnumerable` 思路追加
- 异步 LINQ —— 同上
- 客户端 / 服务端共用同一份 DLL —— 两端有各自的 platform glue（`AtlasLoop`），
  但 `AtlasTask` 类型本身在 `Atlas.Shared` 中定义，dll 二进制兼容

---

## 2. 架构总览

```
┌──────────────────────────────────────────────────────────────┐
│                       业务脚本层                              │
│                                                              │
│   public async AtlasTask<int> LoadProfile(...)                │
│   {                                                          │
│       var profile = await db.GetProfileRpc(dbid, ct);         │
│       await AtlasTask.Delay(100, ct);                         │
│       (var a, var b) = await AtlasTask.WhenAll(taskA, taskB); │
│       return profile.Level;                                   │
│   }                                                          │
├──────────────────────────────────────────────────────────────┤
│                  协程类型层 (Atlas.Shared.Coro)              │
│                                                              │
│  ┌──────────────────┐  ┌────────────────────────────────┐    │
│  │ AtlasTask        │  │ AsyncAtlasTaskMethodBuilder    │    │
│  │ AtlasTask<T>     │  │ AsyncAtlasTaskMethodBuilder<T> │    │
│  │ (readonly struct)│  │ (struct, lazy alloc)           │    │
│  └─────────┬────────┘  └──────────────┬─────────────────┘    │
│            │                          │                      │
│  ┌─────────▼──────────────────────────▼─────────────┐        │
│  │  IAtlasTaskSource / IAtlasTaskSource<T>          │        │
│  │  AtlasTaskCompletionSourceCore<T> (pooled)       │        │
│  │  Status / Token / Continuation                    │        │
│  └─────────┬─────────────────────────────────────────┘        │
├────────────┼─────────────────────────────────────────────────┤
│            ▼  Awaitable 原语 (Atlas.Shared.Coro.Awaitables)  │
│  Delay  Yield  WhenAll  WhenAny  WaitUntil  FromBgWork  Rpc  │
├──────────────────────────────────────────────────────────────┤
│              取消传播 (Atlas.Shared.Coro.Cancellation)       │
│  AtlasCancellationToken / AtlasCancellationSource             │
│  + 父子层级、+ 与 native CancellationSource 桥接 (server)     │
├──────────────────────────────────────────────────────────────┤
│                Platform glue (IAtlasLoop)                    │
│  ┌──────────────────────┐  ┌──────────────────────────────┐  │
│  │ Atlas.Runtime        │  │ Atlas.Client.Unity           │  │
│  │ (server)             │  │ (client)                     │  │
│  │                      │  │                              │  │
│  │ • C++ EventDispatcher│  │ • UnityEngine.PlayerLoop     │  │
│  │   timers via P/Invoke│  │ • Time.deltaTime / unscaled  │  │
│  │ • PendingRpcRegistry │  │ • UniTask-style PlayerLoop   │  │
│  │   bridge → C++       │  │   injection (PreUpdate slot) │  │
│  │ • BgTaskManager 转发 │  │ • Job-system off-thread      │  │
│  └──────────────────────┘  └──────────────────────────────┘  │
└──────────────────────────────────────────────────────────────┘
```

---

## 3. 核心类型

### 3.1 `AtlasTask` / `AtlasTask<T>` —— 协程返回类型

**文件:** `src/csharp/Atlas.Shared/Coro/AtlasTask.cs`,
`src/csharp/Atlas.Shared/Coro/AtlasTask.Generic.cs`

`AtlasTask` 与 `AtlasTask<T>` 都是 `readonly struct`，**不在堆上分配**。每个
task 持有指向后端 source 的弱引用 + 一个 short token（防止 ABA 复用）；空 task
（已同步完成）甚至不持有 source。

```csharp
[AsyncMethodBuilder(typeof(AsyncAtlasTaskMethodBuilder<>))]
[StructLayout(LayoutKind.Auto)]
public readonly struct AtlasTask<T>
{
    private readonly IAtlasTaskSource<T>? source_;
    private readonly T result_;
    private readonly short token_;

    // 已同步完成路径 — 无 source 分配
    public AtlasTask(T result)
    {
        source_ = null;
        result_ = result;
        token_ = 0;
    }

    public AtlasTask(IAtlasTaskSource<T> source, short token)
    {
        source_ = source;
        result_ = default!;
        token_ = token;
    }

    public AtlasTaskStatus Status =>
        source_ is null ? AtlasTaskStatus.Succeeded : source_.GetStatus(token_);

    public Awaiter GetAwaiter() => new(this);

    public readonly struct Awaiter : ICriticalNotifyCompletion
    {
        private readonly AtlasTask<T> task_;

        public Awaiter(AtlasTask<T> task) => task_ = task;

        public bool IsCompleted =>
            task_.source_ is null ||
            task_.source_.GetStatus(task_.token_) != AtlasTaskStatus.Pending;

        public T GetResult()
        {
            if (task_.source_ is null) return task_.result_;
            return task_.source_.GetResult(task_.token_);
        }

        public void OnCompleted(Action continuation) =>
            UnsafeOnCompleted(continuation);

        public void UnsafeOnCompleted(Action continuation)
        {
            if (task_.source_ is null)
            {
                continuation();
                return;
            }
            task_.source_.OnCompleted(SAction, continuation, task_.token_);
        }

        private static readonly Action<object?> SAction = static o => ((Action)o!)();
    }
}
```

**关键设计决策:**

| 决策 | 原因 |
|------|------|
| `readonly struct` | Task 本身零分配；状态在 source 中 |
| 同步完成短路径 | `AtlasTask.FromResult(x)` 不分配 source |
| `short token_` | 池化 source 复用时区分新旧借出，避免悬空 await |
| 无 `Status: Faulted` 字段 | 异常通过 `IAtlasTaskSource.GetResult` 重抛，与 ValueTask 对齐 |
| `ICriticalNotifyCompletion` only | C# 编译器生成的状态机走 `UnsafeOnCompleted`，跳过 ExecutionContext 复制（这是 0 GC 的关键之一） |

`AtlasTask`（无返回值）与 `AtlasTask<T>` 共享同一套 source 接口；非泛型版的
source 直接实现 `IAtlasTaskSource`，泛型版实现 `IAtlasTaskSource<T>`。

### 3.2 `IAtlasTaskSource<T>` 与 `AtlasTaskCompletionSourceCore<T>`

**文件:** `src/csharp/Atlas.Shared/Coro/IAtlasTaskSource.cs`,
`src/csharp/Atlas.Shared/Coro/AtlasTaskCompletionSourceCore.cs`

```csharp
public interface IAtlasTaskSource
{
    AtlasTaskStatus GetStatus(short token);
    void OnCompleted(Action<object?> continuation, object? state, short token);
    void GetResult(short token);
}

public interface IAtlasTaskSource<T> : IAtlasTaskSource
{
    new T GetResult(short token);
}

public enum AtlasTaskStatus : byte
{
    Pending   = 0,
    Succeeded = 1,
    Faulted   = 2,
    Canceled  = 3,
}
```

`AtlasTaskCompletionSourceCore<T>` 是 awaitable 实现者复用的脏活模板（直接借鉴
UniTask 的 `UniTaskCompletionSourceCore<T>` —— 经过 5 年生产环境验证）:

```csharp
public struct AtlasTaskCompletionSourceCore<T>
{
    private T result_;
    private object? error_;          // null | Exception | OperationCanceledException
    private short version_;
    private bool hasUnhandledError_;
    private int completedCount_;     // 0 = pending, 1 = completed
    private Action<object?>? continuation_;
    private object? continuationState_;

    public short Version => version_;

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public void Reset()
    {
        ReportUnhandledError();
        unchecked { version_++; }
        completedCount_ = 0;
        result_ = default!;
        error_ = null;
        hasUnhandledError_ = false;
        continuation_ = null;
        continuationState_ = null;
    }

    public bool TrySetResult(T result)
    {
        if (Interlocked.Increment(ref completedCount_) != 1) return false;
        result_ = result;
        SignalCompletion();
        return true;
    }

    public bool TrySetException(Exception ex) { /* 同上模式 */ }
    public bool TrySetCanceled(CancellationToken ct) { /* 同上模式 */ }

    public AtlasTaskStatus GetStatus(short token)
    {
        ValidateToken(token);
        if (completedCount_ == 0) return AtlasTaskStatus.Pending;
        return error_ switch
        {
            null                          => AtlasTaskStatus.Succeeded,
            OperationCanceledException    => AtlasTaskStatus.Canceled,
            _                             => AtlasTaskStatus.Faulted,
        };
    }

    public T GetResult(short token)
    {
        ValidateToken(token);
        hasUnhandledError_ = false;
        if (error_ != null)
        {
            if (error_ is OperationCanceledException oce) throw oce;
            ExceptionDispatchInfo.Capture((Exception)error_).Throw();
        }
        return result_;
    }

    public void OnCompleted(Action<object?> cont, object? state, short token)
    {
        ValidateToken(token);
        if (Volatile.Read(ref completedCount_) != 0)
        {
            cont(state);   // 已完成，立即在调用线程执行（即主线程）
            return;
        }
        continuationState_ = state;
        if (Interlocked.CompareExchange(ref continuation_, cont, null) is { } prev)
            throw new InvalidOperationException("source already has continuation");
    }

    private void SignalCompletion()
    {
        var cont = Interlocked.Exchange(ref continuation_, AtlasTaskCompletedSentinel.Action);
        if (cont is null || ReferenceEquals(cont, AtlasTaskCompletedSentinel.Action)) return;
        cont(continuationState_);
    }
}
```

**`completedCount_` 用 `Interlocked` 而非简单赋值的原因:** 唯一的多线程入口是
backend awaitable 在后台线程完成时调用 `TrySetResult` —— 例如 `FromBgWork`。
这时 `Reset` / `OnCompleted` 都已经在主线程执行完，`Interlocked.Increment`
保证 "完成动作只跑一次"，且 release 语义把 result 写入对所有线程可见。
单纯主线程路径下分支预测器会保持热路径不变，无明显开销。

### 3.3 `AsyncAtlasTaskMethodBuilder<T>`

**文件:** `src/csharp/Atlas.Shared/Coro/AsyncAtlasTaskMethodBuilder.cs`

`async` 方法的零分配关键。延迟 source 分配 —— 同步走完不分配；在第一次
真正挂起时才从池借出 box，把状态机搬进去。

```csharp
public struct AsyncAtlasTaskMethodBuilder<T>
{
    private AtlasTaskSourceBox<T>? box_;       // 延迟分配
    private T result_;                          // 同步路径直接赋值
    private bool hasResult_;

    public static AsyncAtlasTaskMethodBuilder<T> Create() => default;

    public AtlasTask<T> Task =>
        box_ is null
            ? (hasResult_ ? new AtlasTask<T>(result_) : AtlasTask<T>.CompletedDefault)
            : new AtlasTask<T>(box_, box_.Version);

    public void Start<TStateMachine>(ref TStateMachine sm)
        where TStateMachine : IAsyncStateMachine
        => sm.MoveNext();   // 不复制 ExecutionContext

    public void SetResult(T result)
    {
        if (box_ is null) { result_ = result; hasResult_ = true; return; }
        box_.SetResult(result);
    }

    public void SetException(Exception ex)
    {
        EnsureBox();
        box_!.SetException(ex);
    }

    public void AwaitOnCompleted<TAwaiter, TStateMachine>(
        ref TAwaiter awaiter, ref TStateMachine sm)
        where TAwaiter : INotifyCompletion
        where TStateMachine : IAsyncStateMachine
    {
        var box = EnsureBox();
        box.SetStateMachine(ref sm);
        awaiter.OnCompleted(box.MoveNextAction);
    }

    public void AwaitUnsafeOnCompleted<TAwaiter, TStateMachine>(
        ref TAwaiter awaiter, ref TStateMachine sm)
        where TAwaiter : ICriticalNotifyCompletion
        where TStateMachine : IAsyncStateMachine
    {
        var box = EnsureBox();
        box.SetStateMachine(ref sm);
        awaiter.UnsafeOnCompleted(box.MoveNextAction);
    }

    public void SetStateMachine(IAsyncStateMachine _) { /* no-op, struct builder */ }

    private AtlasTaskSourceBox<T> EnsureBox()
        => box_ ??= AtlasTaskSourceBox<T>.Pool.Rent();
}
```

`AtlasTaskSourceBox<T>` 是池化的 reference type，内部封装
`AtlasTaskCompletionSourceCore<T>` + 状态机 + 一个缓存的
`Action MoveNextAction`（第一次借出时构造，之后 reuse）。`Pool` 是 thread-static
+ 队列上限的双层池，借出 / 归还都是非分配的。

**与 BCL `AsyncTaskMethodBuilder` 的对比:**

| 项 | BCL `AsyncTaskMethodBuilder<T>` | `AsyncAtlasTaskMethodBuilder<T>` |
|----|--------------------------------|----------------------------------|
| 同步完成 | 走 `Task.FromResult` 路径，部分缓存 | 完全零分配 |
| 异步完成 | 分配 `AsyncStateMachineBox`、`Task` | 从池借 `AtlasTaskSourceBox` |
| ExecutionContext | 复制 | 跳过（单线程模型） |
| 完成后 | Task 留给 GC | Box 归还池 |

### 3.4 `AtlasCancellationToken` / `AtlasCancellationSource`

**文件:** `src/csharp/Atlas.Shared/Coro/Cancellation/AtlasCancellationSource.cs`

设计目标:
1. 父子层级 —— 父取消，所有子 source 一并取消
2. 零分配链接 —— 单子 token 场景不堆分配链接节点
3. 与 native `atlas::CancellationSource` 双向互通（服务端）—— Channel 断连
   时 native 触发取消，C# 协程立即收到；C# 主动取消时同步通知 native

```csharp
public sealed class AtlasCancellationSource : IDisposable
{
    private CancellationCoreState state_;       // struct, 内嵌
    private IntPtr nativeHandle_;                // server only

    public AtlasCancellationToken Token => new(this, state_.Version);

    public bool IsCancellationRequested => state_.Cancelled;

    public void Cancel() => state_.RequestCancel(nativeHandle_);

    /// <summary>
    /// 创建一个子 source，父取消时自动级联。
    /// 父已取消则返回的 source 立刻处于取消状态。
    /// </summary>
    public AtlasCancellationSource CreateLinked() { /* ... */ }

    /// <summary>Server only: 绑定已存在的 native CancellationSource。</summary>
    internal void AttachNative(IntPtr handle) { /* ... */ }
}

public readonly struct AtlasCancellationToken : IEquatable<AtlasCancellationToken>
{
    private readonly AtlasCancellationSource? source_;
    private readonly short version_;

    public bool IsCancellationRequested =>
        source_ is not null && source_.IsCancellationRequested;

    public CancelRegistration Register(Action<object?> callback, object? state)
    {
        if (source_ is null) return default;
        return source_.RegisterCallback(callback, state, version_);
    }

    public void ThrowIfCancellationRequested()
    {
        if (IsCancellationRequested)
            throw new OperationCanceledException();
    }
}

public readonly struct CancelRegistration : IDisposable
{
    private readonly AtlasCancellationSource? source_;
    private readonly long id_;
    public void Dispose() => source_?.Unregister(id_);
}
```

**与 BCL `CancellationToken` 的差异:**

| 项 | `CancellationToken` | `AtlasCancellationToken` |
|----|---------------------|--------------------------|
| 注册 callback | 分配 `CancellationTokenRegistration` | 借 list slot，归还时回收 |
| Linked source | `CreateLinkedTokenSource(params)` 总分配 | 单父零分配；多父走 array pool |
| 取消传播 | 走 `ExecutionContext` | 直接同步调用（主线程） |
| Native 桥 | 无 | 服务端通过 `AttachNative` 双向同步 |

**Native 桥（仅服务端）:** 当 C# 协程要 `await rpc_call` 或被 Channel 断连
联动取消，需要让 C++ `CancellationSource::request_cancellation()` 直接驱动
C# `AtlasCancellationSource`。`Atlas.Runtime` 提供:

```csharp
[LibraryImport("atlas_engine")]
private static partial IntPtr atlas_coro_cancellation_create_linked(IntPtr parent);

[LibraryImport("atlas_engine")]
private static partial void atlas_coro_cancellation_request(IntPtr handle);

[UnmanagedCallersOnly]
private static void OnNativeCancel(IntPtr managed_gc_handle)
{
    var src = (AtlasCancellationSource)GCHandle.FromIntPtr(managed_gc_handle).Target!;
    src.OnNativeCancellationRequested();
}
```

C++ 端 `clrscript/cancellation_bridge.{h,cc}` 在 `request_cancellation()`
被调用时通过 reverse-PInvoke 唤起 `OnNativeCancel`。所有跨边界数据通过
`GCHandle` 持久化，C# 析构时调用 native 释放。

---

## 4. Awaitable 原语

### 4.1 `AtlasTask.Delay` —— 定时器等待

**文件:** `src/csharp/Atlas.Shared/Coro/Awaitables/Delay.cs`

```csharp
public static class AtlasTaskExtensions
{
    public static AtlasTask Delay(int milliseconds, AtlasCancellationToken ct = default)
        => DelayAwaitable.Create(milliseconds, ct);
}
```

底层实现走 `IAtlasLoop.RegisterTimer` —— 服务端落到 C++
`EventDispatcher::add_timer`（通过 PInvoke），客户端落到 PlayerLoop 的
`PreUpdate` 阶段累计 `Time.unscaledDeltaTime`。`DelayAwaitable` 是池化对象，
单次 await 周期内借 / 还一次。

### 4.2 `AtlasTask.Yield` —— 让出当前帧

```csharp
public static YieldAwaitable Yield() => default;       // 下一 tick 恢复
public static YieldAwaitable NextFrame() => default;   // 客户端别名
```

`YieldAwaitable` 是 zero-size struct，直接调用 `IAtlasLoop.PostNextFrame(action)`
入队（队列本身在 loop 内复用）。零分配。

### 4.3 `AtlasTask.WhenAll` / `AtlasTask.WhenAny`

```csharp
public static AtlasTask WhenAll(AtlasTask t1, AtlasTask t2);
public static AtlasTask<(T1, T2)> WhenAll<T1, T2>(AtlasTask<T1> t1, AtlasTask<T2> t2);
public static AtlasTask<T[]> WhenAll<T>(IEnumerable<AtlasTask<T>> tasks);
public static AtlasTask<int> WhenAny(params AtlasTask[] tasks);
```

实现要点:
- `WhenAll(t1, t2)` 直接构造一个 2-槽 source，避免 array 分配
- 任一子 task 报异常 → 聚合报告（保留所有异常用于诊断），但 `await` 重抛第一个
- `WhenAny` 取消其他子 task 的资源（注册 `OnCompleted` 时存子 source list，
  胜出时调用其余的取消 hook）

### 4.4 `AtlasTask.WaitUntil` / `AtlasTask.WaitWhile`

```csharp
public static AtlasTask WaitUntil(Func<bool> predicate, AtlasCancellationToken ct = default);
public static AtlasTask WaitUntil<TState>(Func<TState, bool> predicate, TState state,
    AtlasCancellationToken ct = default);
```

每帧 PreUpdate 阶段 poll predicate；带 state 重载避免闭包分配（关键热路径
模式）。这是 ET / UniTask 都暴露的实用 API，对 UI / 游戏逻辑极友好。

### 4.5 `AtlasTask.FromBgWork` —— 线程池等待

```csharp
public static AtlasTask FromBgWork(Action work);
public static AtlasTask<T> FromBgWork<T>(Func<T> work);
```

服务端: 走 C++ `BgTaskManager::add_task`（与 C++ `await_bg_task` 同款），
work 在 worker 线程跑，`SignalCompletion` 由主线程的 `FrequentTask` 回调触发
—— `IAtlasTaskSource` 状态变更和续延执行都在主线程。

客户端: 走 `ThreadPool.UnsafeQueueUserWorkItem`（无 ExecutionContext 复制），
完成后 `IAtlasLoop.PostMainThread(continuation)`。

`work` lambda 必须不访问 Unity API / Entity 状态 —— 这是与 `Task.Run` 一致的
约定。文档化 + analyzer 检查（后续）。

### 4.6 `AtlasTask.Rpc` —— 远端调用等待（核心）

**文件:** `src/csharp/Atlas.Shared/Coro/Awaitables/Rpc.cs`,
`src/csharp/Atlas.Runtime/Coro/RpcRegistry.cs`（server 实现）,
`src/csharp/Atlas.Client/Coro/RpcRegistry.cs`（client 实现）

服务端的 C# 调用 native RPC（例如调用 DBApp）必须复用 C++ 侧的
`PendingRpcRegistry`，否则同一 reply 会被 C# 和 C++ 两套注册表竞争消费。

设计:

```csharp
// 由代码生成器生成的 Mailbox proxy 调用此入口
public static AtlasTask<TResult> AwaitRpc<TResult>(
    Mailbox target, MessageId requestId, ReadOnlySpan<byte> requestBody,
    int timeoutMs, AtlasCancellationToken ct = default)
    where TResult : struct, IAtlasMessage
{
    // 1. 借池化 source
    var source = AtlasRpcSource<TResult>.Pool.Rent();

    // 2. 主线程下注册到 native PendingRpcRegistry
    //    bridge 把 (reply_id, request_id) 关联到 GCHandle.ToIntPtr(source)
    AtlasNativeCoro.RegisterPending(
        replyId: TResult.MessageId,
        requestId: requestId.Value,
        timeoutMs: timeoutMs,
        sourceHandle: GCHandle.ToIntPtr(source.GcHandle));

    // 3. 通过同一 Mailbox 通道发送请求字节流（与现有路径一致）
    target.Send(requestBody);

    // 4. 取消注册
    if (ct.IsCancellationRequested)
        source.SetCanceled();
    else
        source.AttachCancel(ct);

    return source.Task;
}
```

C++ 侧 `clrscript/rpc_bridge.cc` 在收到 reply / 超时 / 取消时通过 reverse-PInvoke
回调 C#:

```cpp
// C++ -> C#: 反序列化前不发生，C# 拿到完整 payload 自行反序列化
extern "C" void atlas_coro_rpc_complete(IntPtr source_handle,
                                         CompletionStatus status,
                                         const std::byte* payload, size_t len);
```

C# 端 `AtlasRpcSource<TResult>.Complete(...)` 调用 `IAtlasMessage.Deserialize`
（生成器生成）填充 result，归还 source 到池。**整个路径无 byte[] 分配** ——
payload buffer 来自 native，C# 用 `Span<byte>` 视图反序列化到 struct。

客户端不走这条路径 —— 客户端 RPC 已经全在 C# 侧（参考
`Atlas.Client.NetClient`），直接维护一份 C# `RpcRegistry` 即可，键和超时
逻辑一一对应 C++ 实现。

---

## 5. 与 C++ 协程的统一语义

C# 协程与 C++ 协程共享同一套异步模型 —— 都是 stackless，都在主线程恢复，
都通过 `(MessageID, request_id)` 匹配 RPC reply，都用 RAII / `using` 触发
回滚。这意味着登录链等 saga 流程可以**任意切换实现语言而行为不变**。

| 概念 | C++ | C# |
|------|-----|----|
| 协程返回 | `Task<T>` / `FireAndForget` | `AtlasTask<T>` / `async void` 替代为 `Forget()` 扩展 |
| 等待 RPC | `co_await rpc_call<Reply>(...)` | `await mailbox.SomeRpc(...)` |
| 取消传播 | `CancellationToken` | `AtlasCancellationToken` |
| 回滚 guard | `ScopeGuard` + `dismiss()` | `AtlasScopeGuard` (struct) + `Dismiss()` 或 `try/finally` + `using` |
| RPC 注册表 | `PendingRpcRegistry` | C# rent 一个 source，注册到同一 native registry（服务端）/ C# registry（客户端） |
| 超时 | `Duration` 参数 | `int timeoutMs` |
| 错误返回 | `Result<T,E>` | 抛 `AtlasException` 或返回 `AtlasResult<T>`（见 §6） |

### 5.1 服务端 RPC 共享同一注册表

C++ 协程 RPC 与 C# 协程 RPC 不冲突，因为:
- C++ 注册时 `sourceHandle` 是 `coroutine_handle<>`
- C# 注册时 `sourceHandle` 是 `GCHandle.ToIntPtr(...)`
- registry 内部存 `void*` + tag 标识来源，dispatch 时分别调用 C++ 续延或
  reverse-PInvoke

`MessageID` 编号空间已经全局唯一（见
`src/csharp/Atlas.Shared/Protocol/MessageIds.cs`），生成器保证 C++ / C# 两侧
完全对齐。

---

## 6. 异常 vs Result 风格

C# 一侧默认采用**异常传播**（与 Task / UniTask 一致），但提供
`AtlasResult<T, E>` 可选返回类型，让对热路径敏感的代码避免抛异常的开销:

```csharp
// 抛异常风格（默认，business code）
public async AtlasTask<Profile> LoadProfile(long dbid, AtlasCancellationToken ct)
{
    var raw = await Db.GetProfileRpc(dbid, ct);   // 网络 / 反序列化错误抛出
    return Profile.FromRaw(raw);
}

// Result 风格（hot path / 服务端 RPC handler）
public async AtlasTask<AtlasResult<Profile, RpcError>> LoadProfileNoThrow(...)
{
    var rawResult = await Db.GetProfileRpcNoThrow(dbid, ct);
    if (rawResult.IsError) return rawResult.Error;
    return Profile.FromRaw(rawResult.Value);
}
```

服务端 ScriptApp 内的请求处理（如 `LoginHandler`）默认走 Result 风格 ——
上游 C++ 已经走 `Result<T,E>`，跨边界保持风格统一更易诊断。客户端
gameplay 代码默认走异常 —— Unity 程序员的肌肉记忆。

---

## 7. 平台 glue: `IAtlasLoop`

```csharp
public interface IAtlasLoop
{
    void PostNextFrame(Action<object?> cb, object? state);
    long RegisterTimer(int milliseconds, Action<object?> cb, object? state);
    void CancelTimer(long handle);
    void PostMainThread(Action<object?> cb, object? state);
    bool IsMainThread { get; }
    long CurrentFrame { get; }
}

public static class AtlasLoop
{
    public static IAtlasLoop Current { get; private set; } = NullLoop.Instance;
    public static void Install(IAtlasLoop loop) => Current = loop;
}
```

服务端 `Atlas.Runtime` 在 `EngineContext.Initialize` 中安装
`ServerLoop`（PInvoke 到 EventDispatcher）；客户端 `Atlas.Client.Unity`
在 `ClientHost.Initialize` 中安装 `UnityLoop`（PlayerLoop 注入，方法照搬
UniTask 的 `PlayerLoopHelper`）。

`AtlasSynchronizationContext` 保留作为 `Task` 互操作兼容层（第三方库返回
`Task` 时仍能走主线程恢复），不再是协程基础设施的核心组件。

---

## 8. 池化策略

| 池 | 范围 | 上限 | 备注 |
|----|------|------|------|
| `AtlasTaskSourceBox<T>` | per-T，thread-static + global queue | 256 / 类型 | 由 builder 借还，承载状态机和 source |
| `AtlasRpcSource<TReply>` | per-reply 类型 | 64 / 类型 | RPC 等待专用，含 cancel registration slot |
| `DelayAwaitable` | 全局 | 256 | 一次 await 借还一次 |
| `WhenAllSource<T>` | per-arity 2 / 3 / 4，泛型其余走 array | 64 | 头部 arity 单独优化避免 array 分配 |
| `CancellationCallbackNode` | 全局 | 1024 | 注册 cancel callback 时借出 |

所有池都是 lock-free（thread-static 主存储 + 全局 ConcurrentQueue 兜底），
**借还都是 O(1) 且零分配**。池上限超出时 fallback 到 `new` —— 不抛异常、
不阻塞，因为业务代码不应该被池压力影响正确性。

GC 监控通过现有 `GCMonitor`（`src/csharp/Atlas.Runtime/Diagnostics/GCMonitor.cs`）
持续观测；CI 跑 stress test 验证稳态 0 GC（gen0 不增长）。

---

## 9. 文件清单

### 9.1 新增文件

```
src/csharp/Atlas.Shared/Coro/
  AtlasTask.cs                                    # 非泛型 AtlasTask
  AtlasTask.Generic.cs                            # AtlasTask<T>
  AtlasTaskStatus.cs                              # 枚举
  IAtlasTaskSource.cs                             # 后端接口
  AtlasTaskCompletionSourceCore.cs                # 实现模板
  AtlasTaskSourceBox.cs                           # 池化 box（builder 用）
  AsyncAtlasTaskMethodBuilder.cs                  # struct builder
  AtlasTaskExtensions.cs                          # FireAndForget / .Forget()

  Awaitables/
    DelayAwaitable.cs
    YieldAwaitable.cs
    WhenAllAwaitable.cs                           # 2/3/4 + array overload
    WhenAnyAwaitable.cs
    WaitUntilAwaitable.cs
    FromBgWorkAwaitable.cs

  Cancellation/
    AtlasCancellationToken.cs
    AtlasCancellationSource.cs
    CancellationCoreState.cs                      # struct
    CancelRegistration.cs

  Pool/
    TaskPool.cs
    PoolStats.cs

  Result/
    AtlasResult.cs                                # AtlasResult<T,E>

src/csharp/Atlas.Runtime/Coro/
  ServerLoop.cs                                   # IAtlasLoop 服务端实现
  AtlasNativeCoro.cs                              # PInvoke 入口
  AtlasRpcSource.cs                               # RPC 专用 source
  CancellationBridge.cs                           # native ↔ managed 取消桥

src/csharp/Atlas.Client/Coro/
  ClientLoop.cs                                   # 桌面客户端 IAtlasLoop
  ClientRpcRegistry.cs                            # 客户端 RPC 注册表

src/csharp/Atlas.Client.Unity/Coro/
  UnityLoop.cs                                    # PlayerLoop 注入
  UnityLoopRunners.cs                             # PreUpdate / PostLateUpdate slot

src/lib/clrscript/
  cancellation_bridge.h
  cancellation_bridge.cc
  rpc_bridge_clr.h
  rpc_bridge_clr.cc

tests/csharp/Atlas.Runtime.Tests/Coro/
  AtlasTaskTests.cs                               # 同步 / 异步路径
  CancellationTests.cs                            # 单 / 多级取消
  WhenAllTests.cs / WhenAnyTests.cs
  DelayTests.cs / YieldTests.cs
  RpcTests.cs                                     # 服务端 RPC 集成
  GcAllocationTests.cs                            # 0 GC 验证（用 GCMonitor）
```

### 9.2 修改文件

```
src/csharp/Atlas.Runtime/Core/EngineContext.cs    # 安装 ServerLoop
src/csharp/Atlas.Runtime/Core/AtlasSynchronizationContext.cs
                                                   # 退化为兼容层（互操作 Task）
src/csharp/Atlas.Client/ClientHost.cs              # 安装 ClientLoop
src/csharp/Atlas.Client.Unity/UnityProfilerBackend.cs
                                                   # （仅相邻处保持一致）
src/lib/clrscript/clrscript_engine.{h,cc}          # 注册 reverse-PInvoke 入口
src/lib/coro/pending_rpc_registry.{h,cc}           # 增加 source tag 区分 C++ / C#
tests/csharp/CMakeLists.txt                        # 加入新测试
```

### 9.3 CMake 改动

`Atlas.Shared` / `Atlas.Runtime` / `Atlas.Client*` csproj 自动 glob 新文件，
无需 CMake 改动；只有 `clrscript` 库的 `cancellation_bridge.cc` /
`rpc_bridge_clr.cc` 需要加进 `src/lib/clrscript/CMakeLists.txt`。

---

## 10. 落地状态

| 范围 | 状态 | 主要文件 |
|------|------|---------|
| 核心类型 / Builder / Cancellation / Yield / Delay / WaitUntil / AtlasResult | ✅ | `Atlas.Shared/Coro/` |
| Pool + TestLoop | ✅ | `Atlas.Shared/Coro/Pool/`、`Atlas.Shared/Coro/Testing/` |
| WhenAll / WhenAny / FromBgWork | ✅ | `Atlas.Shared/Coro/Awaitables/` |
| ManagedAtlasLoop（Server / Desktop）+ DesktopLifecycle | ✅ | `Atlas.Shared/Coro/Hosting/`、`Atlas.Client.Desktop/` |
| UnityLoop（PlayerLoop 注入） | ✅ | `Atlas.Client.Unity/Coro/` |
| `IAtlasRpcRegistry` / `AtlasRpcSource<T>` / `ManagedRpcRegistry` | ✅ | `Atlas.Shared/Coro/Rpc/` |
| `coro_bridge` + `INativeApiProvider::CoroRegister/Cancel` + BaseApp 集成 | ✅ | `src/lib/clrscript/coro_bridge.{h,cc}`、`src/server/baseapp/` |
| `NativeRpcRegistry` PInvoke 包装 | ✅ | `Atlas.Runtime/Coro/NativeRpcRegistry.cs` |
| `AtlasRpcRegistryHost` + `AtlasRpc.Await<T>` 手动 RPC 入口 | ✅ | `Atlas.Shared/Coro/Rpc/AtlasRpc.cs` |
| `AtlasShutdownToken` + Lifecycle 装载 | ✅ | `Atlas.Shared/Coro/Cancellation/AtlasShutdownToken.cs` |
| 取消桥（per-entity native ↔ managed CancellationSource） | ⏳ 待启动 | — |
| Entity-RPC reply 协议 + 生成器 `AtlasTask<TReply>` 代理 | ⏳ 待启动 | `Atlas.Generators.Def/` |
| `FromBgWork` lambda safety analyzer | ⏳ 待启动 | — |

**测试覆盖:** `Atlas.Tests.Coro.*` 70 用例；`tests/unit/test_coro_bridge` 6 用例
（成功 / 取消 / 超时 / null fn / 多 entry / 取消后丢弃）。

### 10.1 当前可用模式

在生成器升级落地前，业务脚本调用进程间已有 `MessageID` 的 RPC（如 BaseApp →
DBApp）可直接用 `AtlasRpc.Await<T>`：

```csharp
public async AtlasTask<Profile> LoadProfile(long dbid, AtlasCancellationToken ct = default)
{
    var requestId = AtlasRpc.NextRequestId();
    var task = AtlasRpc.Await<Profile>(
        replyId: DbApp.LoadProfileResultId,
        requestId: requestId,
        deserializer: static span => Profile.Deserialize(span.Slice(4)),  // skip request_id 头
        timeoutMs: 10_000, ct: ct);

    SendLoadProfileRequest(dbid, requestId);   // 业务方自己发包，request_id 写头 4 字节
    return await task;
}
```

进程关闭时所有 await 协程自动取消：调用方传 `AtlasShutdownToken.Token` 即可。

### 10.2 Entity-RPC reply 协议设计

今天 `<base_methods>` / `<cell_methods>` / `<client_methods>` 都是 fire-and-forget。
本节给出支持 `await mailbox.Method(...)` 形态的完整方案 —— 涵盖 .def 语法、错误模型、
entity 生命周期事件、wire 格式、生成器形态、兼容性方案，以及落地分阶段。

#### 10.2.1 设计原则

| 原则 | 含义 |
|---|---|
| 单一错误模型 | 框架 / 业务 / 远端异常都进 `RpcReply<T>` 字段，永不需 `try/catch` |
| 接收方写法保持自然 | 用户 `partial` 方法返回 `AtlasTask<RpcReply<T>>`，业务错误 `return RpcReply.Fail(...)` |
| Lifecycle-aware | 不假装跨进程迁移状态机；offload / destroy / hot-reload 显式 cancel + 业务重试 |
| 兼容性 | netstandard2.1 / Unity 2022 (C# 9) / IL2CPP AOT 全栈，无 `static abstract`、无运行期反射 |
| 复用现有基建 | `PendingRpcRegistry` / `coro_bridge` / `AtlasRpcSource` / `RpcArgCodec` / `.def` 解析全部沿用 |

#### 10.2.2 `.def` 语法

`<method>` 加 `reply` 属性，类型规则与 `arg type=` 完全一致：

```xml
<entity name="Avatar">
  <base_methods>
    <method name="UseItem" exposed="own_client">           <!-- fire-and-forget, 不变 -->
      <arg name="itemId" type="int32"/>
    </method>

    <method name="GetLevel" reply="int32"/>                <!-- primitive reply -->

    <method name="GetTitles" reply="list[string]"/>        <!-- container reply -->

    <method name="LoadProfile" reply="ProfileResult">      <!-- struct reply -->
      <arg name="dbid" type="int64"/>
    </method>

    <method name="QueryStats" exposed="own_client" reply="StatsReply">  <!-- exposed + reply -->
      <arg name="kind" type="int32"/>
    </method>
  </base_methods>

  <client_methods>
    <method name="ShowDamage">                              <!-- client_methods 禁止 reply -->
      <arg name="amount" type="int32"/>
    </method>
  </client_methods>
</entity>
```

**校验规则（生成器 emit error）：**

- `reply` 类型规则 = `<arg type=...>` 类型规则（primitive / struct / `list[T]` / `dict[K,V]` / `EntityRef`）
- `<client_methods>` 下不允许 `reply`（client 是接收方而非发起方）
- `<base_methods>` / `<cell_methods>` 都允许 `reply`，可与 `exposed` 共存
- 同一 `<method>` 不能多次 `reply`
- reply 引用的 struct 必须前向兼容（新版仅在末尾追加字段）

#### 10.2.3 错误模型：统一 `RpcReply<T>`

调用方拿到的永远是 `RpcReply<T>`，**单分支模式覆盖所有失败**：

```csharp
public readonly struct RpcReply<T>
{
    private readonly T _value;
    public int Error { get; }                  // 0 = success
    public string? ErrorMessage { get; }

    public bool IsOk             => Error == 0;
    public bool IsBusinessError  => Error > 0;
    public bool IsFrameworkError => Error < 0;

    public T Value => IsOk ? _value
        : throw new InvalidOperationException(
            $"RpcReply.Value on error: code={Error} msg={ErrorMessage}");

    public bool TryGetValue(out T value) { value = _value; return IsOk; }

    public static RpcReply<T> Ok(T v)                    => new(v, 0, null);
    public static RpcReply<T> Fail(int code, string? m)  => new(default!, code, m);
    private RpcReply(T v, int e, string? m) { _value = v; Error = e; ErrorMessage = m; }

    // 接收方语法糖: return RpcReply<T>.Ok(value) 直接转成 AtlasTask
    public static implicit operator AtlasTask<RpcReply<T>>(RpcReply<T> reply)
        => AtlasTask<RpcReply<T>>.FromResult(reply);
}

public static class RpcErrorCodes
{
    public const int Timeout          = -1;
    public const int Cancelled        = -2;   // 含 offload / destroy / hot-reload
    public const int RemoteException  = -3;   // 接收方未捕获异常
    public const int ReceiverGone     = -4;   // entity / channel / process 没了
    public const int SendFailed       = -5;   // 发送侧失败
    public const int MethodNotFound   = -6;   // 接收方 .def 版本不识别该 method
    public const int PayloadMalformed = -7;   // SpanReader 越界 / 格式错
    // 业务侧用 > 0
}
```

**框架错误消息一律使用 interned literal**（避免 `enum.ToString()` 每次分配）：

```csharp
internal static class RpcFrameworkMessages
{
    public const string Timeout         = "framework: timeout";
    public const string Cancelled       = "framework: cancelled";
    public const string ReceiverGone    = "framework: receiver gone";
    public const string SendFailed      = "framework: send failed";
    public const string MethodNotFound  = "framework: method not found";
    public const string PayloadMalformed = "framework: payload malformed";
}
```

调用方代码（**永不 `try/catch`**）：

```csharp
var r = await mb.LoadProfile(dbid);
if (r.IsOk) Use(r.Value);
else if (r.IsBusinessError) {
    switch (r.Error) {
        case BizCodes.NotFound: ShowDialog("not found"); break;
        case BizCodes.Banned:   KickPlayer();            break;
    }
} else {
    if (r.Error == RpcErrorCodes.Timeout) Retry();
    else Log.Warn($"rpc framework error: {r.Error} {r.ErrorMessage}");
}
```

接收方代码（业务错误**显式返回**，不 throw）：

```csharp
public partial class Avatar : ServerEntity
{
    public partial AtlasTask<RpcReply<Profile>> LoadProfile(long dbid)
    {
        if (!_db.Has(dbid))
            return RpcReply<Profile>.Fail(BizCodes.NotFound, "dbid not found");
        return RpcReply<Profile>.Ok(_db.Load(dbid));
    }

    // async 形态: partial 实现可加 async 修饰
    public async partial AtlasTask<RpcReply<Profile>> LoadProfile(long dbid)
    {
        if (!_db.Has(dbid)) return RpcReply<Profile>.Fail(BizCodes.NotFound, "no");
        var raw = await _db.LoadAsync(dbid);
        return RpcReply<Profile>.Ok(new Profile { ... });
    }
}
```

派发器对**未捕获**异常翻译成 `RemoteException`（截断 `ex.ToString()` 至 512 字符
防止 GC 噩梦）；业务错误用户用 `RpcReply.Fail(positiveCode, msg)` 显式返回，
零异常分配。

#### 10.2.4 Entity 生命周期事件 → 协程取消

`ServerEntity` 暴露 `LifecycleCancellation` token，**三种事件**触发 cancel：

1. **Offload**：CellApp entity 跨 cell 迁移前，`EntityManager.OnEntityWillOffload` 钩子触发
2. **Destroy**：entity 被销毁（玩家下线、despawn）前
3. **Hot reload**：`HotReloadManager.SerializeAndUnload` 前必须 drain 所有 in-flight RPC

```csharp
public abstract class ServerEntity
{
    private readonly AtlasCancellationSource _lifecycle = new();
    public AtlasCancellationToken LifecycleCancellation => _lifecycle.Token;

    internal void TriggerLifecycleCancellation() => _lifecycle.Cancel();
    // 由 EntityManager 在 offload / destroy / hot-reload 钩子调
}
```

业务代码处理（与其他失败**完全对称**）：

```csharp
public partial AtlasTask<int> ComputeStuff()
{
    var r = await SomeRpc(ct: this.LifecycleCancellation);
    if (r.IsFrameworkError && r.Error == RpcErrorCodes.Cancelled) {
        // entity offload / destroy / reload 中, 这次失败
        return 0;
    }
    if (r.IsOk) return r.Value * 2;
    return -1;
}
```

| 进程 | offload? | destroy 频率 | hot reload? | 策略 |
|---|---|---|---|---|
| `BaseApp` entity | ❌ | 玩家下线触发 | ✅ | LifecycleCancellation 仅响应 destroy + hot reload |
| `CellApp` entity | ✅ 频繁 | 频繁 | ✅ | LifecycleCancellation 三态全覆盖 |
| `LoginApp` 等 ManagerApp | ❌ | n/a | ✅ | 仅 hot reload 触发 |

**Hot reload 不是事后清理 —— 必须 drain 完才能卸载 assembly**，否则状态机引用旧
`ScriptLoadContext` 的类型，resume 时崩。`HotReloadManager.SerializeAndUnload`
入口加：

```csharp
public static async AtlasTask<int> SerializeAndUnload(...)
{
    // 1. 通知所有 entity 即将 reload
    foreach (var e in EntityManager.Instance.All())
        e.TriggerLifecycleCancellation();
    // 2. drain pending registry (有界等待, 否则强制丢)
    await DrainRpcRegistry(maxWaitMs: 5000);
    // 3. 序列化、卸载
    ...
}
```

#### 10.2.5 协议层

| 项 | 内容 |
|---|---|
| MessageID | `13` `EntityRpcReply`（保持 1-byte 编码段，与现有 1-12 顺延） |
| 请求 payload | `[request_id: u32 LE][args...]`；rpc_id 顶位 `kReplyBit` 标记 expects-reply |
| Reply payload | `[request_id: u32 LE][error_code: i32 LE]`<br>`if error_code != 0:` `[error_msg: VLE-len-prefixed UTF-8]`<br>`if error_code == 0:` `[reply_data: T]` |
| rpc_id `kReplyBit` | rpc_id 高位空闲段（`[direction:2 \| typeIndex:14 \| method:8]` 的 8 个 unused bits 取最高位） |
| 反向路由 | server↔server 走反向 entity channel；server→client 走该 client 的 external channel |
| Channel 断开处理 | `PendingRpcRegistry` 按 channel 索引；channel 断开立即 cancel 该 channel 上 pending → `RpcErrorCodes.ReceiverGone` reply（避 10s 卡顿） |

#### 10.2.6 兼容性方案：`errorMapper` delegate

`static abstract` 接口成员在 netstandard2.1 / Unity 2022 / IL2CPP **不可用**。
改成显式传 `errorMapper` delegate 给 source：

```csharp
public sealed class AtlasRpcSource<T> : ...
{
    public delegate T SpanDeserializer(ref SpanReader reader);   // 框架已解 request_id + error_code
    public delegate T ErrorMapper(int code, string message);

    public void Start(IAtlasRpcRegistry registry, int replyId, uint requestId,
        SpanDeserializer deserializer, ErrorMapper errorMapper,
        int timeoutMs = AtlasRpc.DefaultTimeoutMs,
        AtlasCancellationToken ct = default) { /* ... */ }

    public void OnReply(ReadOnlySpan<byte> payload)
    {
        var r = new SpanReader(payload);
        var requestId = r.ReadUInt32();        // 验证用
        var errorCode = r.ReadInt32();
        if (errorCode != 0) {
            var msg = r.ReadString();
            _core.TrySetResult(_errorMapper!(errorCode, msg));
            return;
        }
        try { _core.TrySetResult(_deserializer!(ref r)); }
        catch (Exception ex) {
            _core.TrySetResult(_errorMapper!(
                RpcErrorCodes.PayloadMalformed, ex.Message));
        }
    }

    public void OnError(RpcCompletionStatus status)
    {
        var (code, msg) = status switch {
            RpcCompletionStatus.Timeout   => (RpcErrorCodes.Timeout,    RpcFrameworkMessages.Timeout),
            RpcCompletionStatus.Cancelled => (RpcErrorCodes.Cancelled,  RpcFrameworkMessages.Cancelled),
            _                             => (RpcErrorCodes.SendFailed, RpcFrameworkMessages.SendFailed),
        };
        _core.TrySetResult(_errorMapper is not null
            ? _errorMapper(code, msg)
            : default!);
    }
}
```

每个 reply 类型 T 由生成器一次性 emit `static` lambda（**零分配**）：

```csharp
private static readonly AtlasRpcSource<RpcReply<Profile>>.ErrorMapper s_LoadProfileErrorMapper =
    static (code, msg) => RpcReply<Profile>.Fail(code, msg);

private static readonly AtlasRpcSource<RpcReply<Profile>>.SpanDeserializer s_LoadProfileDeserializer =
    static (ref SpanReader r) => RpcReply<Profile>.Ok(Profile.Deserialize(ref r));

public AtlasTask<RpcReply<Profile>> LoadProfile(long dbid,
    AtlasCancellationToken ct = default,
    int timeoutMs = AtlasRpc.DefaultTimeoutMs)
{
    var requestId = AtlasRpc.NextRequestId();
    var src = AtlasRpcSource<RpcReply<Profile>>.Rent();
    src.Start(AtlasRpcRegistryHost.Current, MessageIds.EntityRpcReply, requestId,
        s_LoadProfileDeserializer, s_LoadProfileErrorMapper, timeoutMs, ct);
    SendRequest(0x300102 | RpcId.kReplyBit, requestId, dbid);
    return src.Task;
}
```

手动 API 用户（不走生成器）用 `RpcReplyHelpers<T>`：

```csharp
public static class RpcReplyHelpers
{
    public static AtlasRpcSource<RpcReply<T>>.ErrorMapper For<T>()
        => Cache<T>.ErrorMapper;

    private static class Cache<T>
    {
        public static readonly AtlasRpcSource<RpcReply<T>>.ErrorMapper ErrorMapper =
            static (code, msg) => RpcReply<T>.Fail(code, msg);
    }
}
```

`Cache<T>` 是泛型 nested static class，每个具化 T 自动一次性 init。

#### 10.2.7 GC / 性能修补

`NativeRpcRegistry` 当前每次 `GCHandle.Alloc(callback)` 一次堆分配。5000 在线 ×
5 RPC/s = 25k GCHandle/s 持续小对象压力。引入 `GCHandlePool`（thread-static）：

```csharp
internal static class GCHandlePool
{
    [ThreadStatic] private static Stack<GCHandle>? s_pool;

    public static IntPtr Rent(object target)
    {
        var pool = s_pool;
        if (pool is { Count: > 0 }) {
            var gch = pool.Pop();
            gch.Target = target;
            return GCHandle.ToIntPtr(gch);
        }
        return GCHandle.ToIntPtr(GCHandle.Alloc(target, GCHandleType.Normal));
    }

    public static void Return(IntPtr handle)
    {
        var gch = GCHandle.FromIntPtr(handle);
        gch.Target = null;
        var pool = s_pool ??= new Stack<GCHandle>(64);
        if (pool.Count < 256) pool.Push(gch);
        else gch.Free();
    }
}
```

`NativeCallbacks.OnRpcComplete` 改 `gch.Free()` → `GCHandlePool.Return(managedHandle)`。

#### 10.2.8 异常情况覆盖（offload 之外）

| 类别 | 触发 | 框架处理 |
|---|---|---|
| Entity destroy 中 in-flight | 调用方 / 接收方 entity 被 `Destroy()` | `LifecycleCancellation` 触发 → `Cancelled` reply；接收方 entity 已 destroy 时 dispatcher 回 `ReceiverGone` |
| Hot reload 中 in-flight | `HotReloadManager.SerializeAndUnload` | 先 cancel 所有 entity 的 LifecycleCancellation，drain 5s，再卸载 assembly |
| Channel 断开 | RUDP / TCP peer 进程崩 | `PendingRpcRegistry` 按 channel 索引；断开立即 cancel 该 channel pending → `ReceiverGone`（避 10s 默认 timeout） |
| 进程崩溃 / reviver 重启 | machined `DeathNotification` | 同 channel 断开路径 |
| `.def` 版本错配（rolling deploy） | caller v2 / receiver v1 调到不存在方法 | dispatcher 回 `MethodNotFound` |
| Payload 格式错位 | 字段数 / 类型不一致 | SpanReader 越界 → `PayloadMalformed` |
| 重入 RPC（A 调 B，B 反向调 A） | 业务依赖循环 | 单线程模型保证无真并发；用户须接受逻辑并发或自加锁字段 |
| Late reply（timeout 后到达） | 网络抖动 / RUDP 重传 | registry 找不到 entry，silently drop（既有行为） |
| `request_id` 32 位回绕 | 50 天 @ 10k req/s | 10s 超时上限保证不会冲突 |
| Duplicate reply | RUDP 重传 | 第一次 dispatch 后 entry 已删，重复包 silently drop |

#### 10.2.9 实现层文件清单

```
src/csharp/Atlas.Shared/Coro/Rpc/
  RpcReply.cs                  # struct RpcReply<T> + implicit op + Ok/Fail
  RpcErrorCodes.cs             # 静态类: 框架预留负数码
  RpcFrameworkMessages.cs      # interned literal 错误消息
  RpcReplyHelpers.cs           # Cache<T>.ErrorMapper 工厂
  AtlasRpcSource.cs            # 加 ErrorMapper 重载, SpanDeserializer 改 ref SpanReader
  AtlasRpc.cs                  # 加 OrThrow 扩展

src/csharp/Atlas.Runtime/Coro/
  NativeRpcRegistry.cs         # 用 GCHandlePool
  GCHandlePool.cs              # thread-static GCHandle 池

src/csharp/Atlas.Runtime/Entity/
  ServerEntity.Lifecycle.cs    # LifecycleCancellation token

src/csharp/Atlas.Runtime/Hosting/
  HotReloadManager.cs          # SerializeAndUnload 前 drain RPC

src/csharp/Atlas.Generators.Def/
  DefParser.cs                 # 加 reply="..." 属性解析 + 校验
  DefTypeHelper.cs             # reply 类型走与 arg 同套
  Emitters/MailboxEmitter.cs   # emit AtlasTask<RpcReply<T>> 签名 + static delegates
  Emitters/DispatcherEmitter.cs# emit 接收方派发 + 异常翻译 + 反向 reply 写回

src/lib/coro/
  pending_rpc_registry.{h,cc}  # 加 channel 索引: 按 channel 删 pending

src/lib/network/
  message_ids.h                # 加 EntityRpcReply = 13

src/server/baseapp/baseapp.cc  # 加 EntityRpcReply 反向 channel 路由
src/server/cellapp/cellapp.cc  # 同上 + offload 触发 LifecycleCancellation
src/csharp/Atlas.Shared/Protocol/MessageIds.cs  # 加 EntityRpcReply = 13
```

#### 10.2.10 落地分阶段

| 阶段 | 范围 | 依赖 |
|---|---|---|
| **P1** | `RpcReply<T>` / `RpcErrorCodes` / `RpcFrameworkMessages` / `RpcReplyHelpers` 类型层 + 单元测试 | — |
| **P2** | `AtlasRpcSource<T>` 加 `ErrorMapper` 重载 + `SpanDeserializer` 改 `ref SpanReader` + `OrThrow` 扩展 + 测试 | P1 |
| **P3** | `GCHandlePool` + `NativeRpcRegistry` 切换 | — |
| **P4** | `ServerEntity.LifecycleCancellation` + EntityManager destroy / offload 钩子 | — |
| **P5** | `HotReloadManager` drain pending RPC | P4 |
| **P6** | C++: MessageID `EntityRpcReply` 注册 + payload 编解码 + `pre_dispatch_hook` 路由 | — |
| **P7** | C++: `BaseApp::SendEntityRpcReply` / `CellApp::SendEntityRpcReply` 反向 channel 解析 + offload 钩子调入 C# | P6 + P4 |
| **P8** | `PendingRpcRegistry` 加 channel 索引 + channel 断开批量 cancel | P6 |
| **P9** | `DefParser` 解析 `reply="..."` + 校验 | — |
| **P10** | `MailboxEmitter` emit `AtlasTask<RpcReply<T>>` 代理 | P1 + P2 + P9 |
| **P11** | `DispatcherEmitter` emit 接收方派发 + 异常翻译 + 反向 reply 写回 | P10 + P7 + P8 |
| **P12** | analyzer: CellApp entity 内 `await` 必须显式 ct；`RpcReply<T>` 未检查 `IsOk` 即访问 `.Value` 警告 | 任何时间 |

P1–P9 各自隔离可单独 land；P10–P11 是生成器联动，需一起做；P12 独立。

---

## 11. 风险与缓解

### 11.1 状态机泛型膨胀

**风险:** 每个 `async AtlasTask<T>` 方法都生成一份独立状态机；
`AsyncAtlasTaskMethodBuilder<T>` 的泛型实例化数量可能很大，影响 IL 大小和
JIT 时间。

**缓解:** UniTask 用同样模式在数百万行 Unity 项目中稳定使用，证明可控。
`AtlasTaskSourceBox<T>` 通过工厂闭包池化，泛型实例只 sizeof(T) 决定，
不影响运行时内存碎片。

### 11.2 Box 池化的 ABA / token 错配

**风险:** Box 借出 → 完成 → 归还 → 立即被另一 async 借出 —— 旧 awaiter
若仍持有引用，在新借出后调用 `GetResult` 会拿到错误数据。

**缓解:** `short token_` 在每次 `Reset` 时 +1。`GetStatus` / `GetResult` /
`OnCompleted` 都校验 token，token 错配抛 `InvalidOperationException`。
UniTask 5 年生产验证此模式安全。

### 11.3 lambda 闭包仍然分配

**风险:** `AtlasTask.WaitUntil(() => x.IsReady)` 闭包捕获 `x` 仍分配 closure
对象，破坏 0 GC。

**缓解:** 提供 `WaitUntil<TState>(Func<TState,bool>, TState)` 重载，热路径
显式传 state；analyzer 在 `AtlasTask.*` 调用点提示没有走带 state 的重载。
ET 框架同样处理。

### 11.4 与第三方库的 Task 互操作

**风险:** 第三方库（例如 Newtonsoft.Json 或 Unity 的某些 API）只返回
`Task` / `ValueTask`，无法零分配。

**缓解:** 提供 `task.ToAtlasTask()` 适配器（一次性分配，返回后续 0 GC）；
明确文档化"协程 0 GC 仅适用于业务代码内部"。

### 11.5 Unity IL2CPP / AOT

**风险:** Unity 客户端用 IL2CPP 时，泛型反射 / 动态 dispatch 有限制。

**缓解:** 所有泛型实例由生成器（`Atlas.Generators.Def`）显式 emit
`AtlasTask<TReply>` 实例化点，IL2CPP 在 AOT 编译时即可看到全部具化类型。
`AsyncAtlasTaskMethodBuilder<T>` 不依赖反射。

### 11.6 reverse-PInvoke 性能

**风险:** native → managed 调用（用于 cancellation / RPC reply 触达 C#）
有约 200ns 开销，高频热路径可能放大。

**缓解:**
- RPC reply 频率受网络限制（每秒 < 10k），开销可接受
- 取消是低频路径
- 批量 timer 触发 / batch reply 走单次 PInvoke + 内部分发（如有需要在
  Phase 4 优化）
- C++ 与 C# 在同进程同主线程，PInvoke 不涉及切换；measured 在 win10/x64
  约 80–200ns，验证后再决定是否优化

---

## 12. 与 ET / UniTask 的关系

**借鉴点:**
- UniTask: `IAtlasTaskSource<T>` 接口、`AtlasTaskCompletionSourceCore<T>`、
  builder 延迟分配、PlayerLoop 注入方式
- ET: 单线程模型、层级 cancellation 设计、Result 风格的 RPC 错误处理

**差异点:**
- UniTask 主要为 Unity 客户端优化，没有 native 互通；Atlas 服务端是 C++ +
  CoreCLR，必须有 `PendingRpcRegistry` 桥接
- ET 服务端用了类似但更简化的 `ETTask`，无 `IValueTaskSource` 风格的精细
  pool；Atlas 借 UniTask 的 source pool 让两端同步受益
- Atlas 强调 "C++ 与 C# 协程同语义"，把 C++ 已落地的设计（saga RAII、
  request_id 约定、超时 timer 同源）映射到 C# —— ET / UniTask 都不需要
  考虑这层
