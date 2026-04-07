# 设计: NativeApi 架构基础设施

> 归属阶段: ScriptPhase 1 (CMake 共享库) + ScriptPhase 2 (Provider + 线程安全)
>
> **本文档解决三个阻塞性架构问题，是 ScriptPhase 2–6 所有 NativeApi 调用的基础。**

---

## 1. 问题总览

| # | 问题 | 风险 | 影响范围 |
|---|------|------|---------|
| A | C# `[LibraryImport]` 需要从宿主进程找到 C++ 导出符号，但可执行文件导出符号在 Windows/Linux 上都需要特殊处理 | 高 | 所有 C# → C++ 调用在运行时失败 |
| B | 分布式架构下不同进程类型（BaseApp/CellApp/DBApp）对同一 NativeApi 有不同行为，当前设计无差异化机制 | 高 | RPC 路由错误、功能缺失、静默失败 |
| C | C# `async/await` 续体在线程池线程执行，调用非线程安全的 C++ 引擎 API 导致数据竞争 | 中-高 | 随机崩溃、数据损坏 |

---

## 2. 方案 A: 引擎核心构建为共享库

### 2.1 当前构建模型与问题

```
当前:
  libplatform.a ─┐
  libfoundation.a┤
  libnetwork.a  ─┼── 静态链接 ──→ BaseApp.exe
  libclrscript.a ┤                  (atlas_* 符号在 .exe 中)
  ...           ─┘

  C# [LibraryImport("atlas_runtime")]
    → NativeLibrary.GetMainProgramHandle()
    → GetProcAddress(exe, "atlas_log_message")
    → ❌ 可能找不到（exe 默认不导出符号）
```

**核心矛盾**: `[LibraryImport]` 的标准用法是加载共享库（.dll/.so），而不是从宿主可执行文件查找符号。
虽然 `NativeLibrary.GetMainProgramHandle()` + `DllImportResolver` 技术上可行，
但依赖平台特定的链接器配置（Windows 需 `/WHOLEARCHIVE` 或 `.def`，Linux 需 `-rdynamic`），
且静态库中的 `__declspec(dllexport)` 可能被链接器剥离。

### 2.2 解决方案: 引擎共享库

```
改为:
  libplatform.a ─┐
  libfoundation.a┤
  libnetwork.a  ─┼── 打包为 ──→ atlas_engine.dll/.so
  libclrscript.a ┤                (atlas_* 符号自然导出)
  ...           ─┘
                                   ↑
  BaseApp.exe (薄壳) ─── 动态链接 ─┘
  CellApp.exe (薄壳) ─── 动态链接 ─┘

  C# [LibraryImport("atlas_engine")]
    → 标准 DLL 查找 → atlas_engine.dll/.so
    → GetProcAddress → ✅ 天然可用
```

### 2.3 CMake 改动

```cmake
# ---- 引擎核心共享库 ----
# 将各子库编译为 OBJECT 库（生成 .o/.obj，不独立归档）
add_library(platform OBJECT src/lib/platform/*.cpp)
add_library(foundation OBJECT src/lib/foundation/*.cpp)
add_library(network OBJECT src/lib/network/*.cpp)
add_library(serialization OBJECT src/lib/serialization/*.cpp)
add_library(entitydef OBJECT src/lib/entitydef/*.cpp)
add_library(clrscript OBJECT src/lib/clrscript/*.cpp)
add_library(server_framework OBJECT src/lib/server/*.cpp)
# ...

# 打包为共享库 — atlas_* 导出符号在此生效
add_library(atlas_engine SHARED
    $<TARGET_OBJECTS:platform>
    $<TARGET_OBJECTS:foundation>
    $<TARGET_OBJECTS:network>
    $<TARGET_OBJECTS:serialization>
    $<TARGET_OBJECTS:entitydef>
    $<TARGET_OBJECTS:clrscript>
    $<TARGET_OBJECTS:server_framework>
)

target_compile_definitions(atlas_engine PRIVATE ATLAS_ENGINE_EXPORTS)

# 第三方依赖（MySQL, .NET hostfxr 等）链接到共享库
target_link_libraries(atlas_engine
    PRIVATE hostfxr_import mysql_client
    PUBLIC  Threads::Threads
)

# ---- 服务端可执行文件（薄壳）----
add_executable(BaseApp src/server/baseapp/main.cpp)
target_link_libraries(BaseApp PRIVATE atlas_engine)

add_executable(CellApp src/server/cellapp/main.cpp)
target_link_libraries(CellApp PRIVATE atlas_engine)

add_executable(DBApp src/server/dbapp/main.cpp)
target_link_libraries(DBApp PRIVATE atlas_engine)
```

### 2.4 导出宏调整

```cpp
// src/lib/clrscript/clr_export.hpp
#pragma once

#if defined(_WIN32)
    #ifdef ATLAS_ENGINE_EXPORTS
        #define ATLAS_EXPORT __declspec(dllexport)
    #else
        #define ATLAS_EXPORT __declspec(dllimport)
    #endif
#else
    #define ATLAS_EXPORT __attribute__((visibility("default")))
#endif

#define ATLAS_NATIVE_API extern "C" ATLAS_EXPORT
```

### 2.5 C# 侧简化

共享库方案下，不再需要 `DllImportResolver` hack：

```csharp
// 改前: 需要 hack 将 "atlas_runtime" 映射到 exe 自身
static Bootstrap()
{
    NativeLibrary.SetDllImportResolver(
        typeof(Bootstrap).Assembly,
        (name, assembly, searchPath) =>
        {
            if (name == "atlas_runtime")
                return NativeLibrary.GetMainProgramHandle();
            return IntPtr.Zero;
        });
}

// 改后: 标准 P/Invoke，运行时自然从同目录查找 atlas_engine.dll/.so
// Bootstrap 静态构造函数不再需要 DllImportResolver
// [LibraryImport("atlas_engine")] 即可工作
```

NativeApi 的 lib 名称从 `"atlas_runtime"` 改为 `"atlas_engine"`:

```csharp
internal static unsafe partial class NativeApi
{
    private const string LibName = "atlas_engine";

    [LibraryImport(LibName, EntryPoint = "atlas_log_message")]
    private static partial void LogMessageNative(int level, byte* msg, int len);

    // ...
}
```

### 2.6 部署结构

```
deploy/
├── BaseApp.exe             (薄壳, ~50KB)
├── CellApp.exe             (薄壳, ~50KB)
├── DBApp.exe               (薄壳, ~50KB)
├── atlas_engine.dll         (引擎核心, ~2-5MB)
├── dotnet/                  (.NET 10 运行时)
├── scripts/
│   ├── Atlas.Runtime.dll    (C# 引擎运行时)
│   ├── Atlas.Shared.dll     (C# 共享定义)
│   └── Atlas.GameScripts.dll(用户脚本)
└── config/
    └── runtimeconfig.json
```

### 2.7 工作点

- [ ] CMake: 将所有 `src/lib/` 子库改为 `OBJECT` 库
- [ ] CMake: 创建 `atlas_engine` SHARED 库目标，聚合所有 OBJECT 库
- [ ] CMake: 各 server 可执行文件改为动态链接 `atlas_engine`
- [ ] 导出宏: `ATLAS_ENGINE_EXPORTS` 定义控制 `dllexport`/`dllimport`
- [ ] C#: NativeApi 的 LibName 从 `"atlas_runtime"` 改为 `"atlas_engine"`
- [ ] C#: 移除 `DllImportResolver` hack（不再需要）
- [ ] Linux: 验证 `-fvisibility=hidden` + `ATLAS_EXPORT` 仅导出 `atlas_*` 符号
- [ ] CI: 集成测试验证 `dlsym`/`GetProcAddress` 能找到所有 `atlas_*` 符号
- [ ] 文档: 更新 `CLAUDE.md` 中的构建说明

---

## 3. 方案 B: INativeApiProvider 进程级适配

### 3.1 问题分析

Atlas 分布式架构中，同一 NativeApi 在不同进程类型下行为不同：

| NativeApi 函数 | BaseApp | CellApp | DBApp | LoginApp |
|---------------|---------|---------|-------|----------|
| `send_client_rpc` | 持有 ClientProxy，直接发送 | 无客户端连接，转发给 BaseApp | 不支持 | 不支持 |
| `send_cell_rpc` | 查找目标 CellApp，跨进程路由 | 本地分发给 C# | 不支持 | 不支持 |
| `send_base_rpc` | 本地分发给 C# | 查找目标 BaseApp，跨进程路由 | 不支持 | 不支持 |
| `get_process_prefix` | 返回 Base 前缀 | 返回 Cell 前缀 | 不分配 EntityId | 不分配 EntityId |
| `register_entity_type` | 注册（含全部信息） | 注册（含 Cell 相关） | 注册（含持久化字段） | 不需要 |

当前 `clr_native_api.cpp` 是单一实现文件，无法表达这种差异。

### 3.2 Provider 接口

```cpp
// src/lib/clrscript/native_api_provider.hpp
#pragma once

#include <cstddef>
#include <cstdint>

namespace atlas {

/// 抽象接口 — 各进程类型提供不同实现。
/// atlas_* 导出函数全部委托给此接口。
class INativeApiProvider
{
public:
    virtual ~INativeApiProvider() = default;

    // ---- 基础服务 ----
    virtual void log_message(int32_t level,
        const char* msg, int32_t len) = 0;
    virtual double server_time() = 0;
    virtual float delta_time() = 0;

    // ---- 进程标识 ----
    virtual uint8_t get_process_prefix() = 0;

    // ---- RPC 发送 ----
    virtual void send_client_rpc(uint32_t entity_id, uint32_t rpc_id,
        uint8_t target, const std::byte* payload, int32_t len) = 0;
    virtual void send_cell_rpc(uint32_t entity_id, uint32_t rpc_id,
        const std::byte* payload, int32_t len) = 0;
    virtual void send_base_rpc(uint32_t entity_id, uint32_t rpc_id,
        const std::byte* payload, int32_t len) = 0;

    // ---- 实体类型注册 ----
    virtual void register_entity_type(const std::byte* data, int32_t len) = 0;
    virtual void unregister_all_entity_types() = 0;
};

/// 注册当前进程的 Provider（main() 中调用，ClrHost 启动前）
void set_native_api_provider(INativeApiProvider* provider);

/// 获取当前进程的 Provider（atlas_* 导出函数内部调用）
INativeApiProvider& get_native_api_provider();

} // namespace atlas
```

### 3.3 导出函数实现（薄代理）

```cpp
// src/lib/clrscript/clr_native_api.cpp
#include "clrscript/clr_native_api.hpp"
#include "clrscript/native_api_provider.hpp"

namespace atlas {

static INativeApiProvider* g_provider = nullptr;

void set_native_api_provider(INativeApiProvider* provider)
{
    g_provider = provider;
}

INativeApiProvider& get_native_api_provider()
{
    ATLAS_ASSERT(g_provider != nullptr,
        "NativeApi provider not registered. "
        "Call set_native_api_provider() before initializing ClrHost.");
    return *g_provider;
}

} // namespace atlas

// ---- 所有 atlas_* 导出函数 — 一行转发 ----

ATLAS_NATIVE_API void atlas_log_message(
    int32_t level, const char* msg, int32_t len)
{
    atlas::get_native_api_provider().log_message(level, msg, len);
}

ATLAS_NATIVE_API double atlas_server_time()
{
    return atlas::get_native_api_provider().server_time();
}

ATLAS_NATIVE_API float atlas_delta_time()
{
    return atlas::get_native_api_provider().delta_time();
}

ATLAS_NATIVE_API uint8_t atlas_get_process_prefix()
{
    return atlas::get_native_api_provider().get_process_prefix();
}

ATLAS_NATIVE_API void atlas_send_client_rpc(
    uint32_t entity_id, uint32_t rpc_id,
    uint8_t target, const uint8_t* payload, int32_t len)
{
    atlas::get_native_api_provider().send_client_rpc(
        entity_id, rpc_id, target,
        reinterpret_cast<const std::byte*>(payload), len);
}

ATLAS_NATIVE_API void atlas_send_cell_rpc(
    uint32_t entity_id, uint32_t rpc_id,
    const uint8_t* payload, int32_t len)
{
    atlas::get_native_api_provider().send_cell_rpc(
        entity_id, rpc_id,
        reinterpret_cast<const std::byte*>(payload), len);
}

ATLAS_NATIVE_API void atlas_send_base_rpc(
    uint32_t entity_id, uint32_t rpc_id,
    const uint8_t* payload, int32_t len)
{
    atlas::get_native_api_provider().send_base_rpc(
        entity_id, rpc_id,
        reinterpret_cast<const std::byte*>(payload), len);
}

ATLAS_NATIVE_API void atlas_register_entity_type(
    const uint8_t* data, int32_t len)
{
    atlas::get_native_api_provider().register_entity_type(
        reinterpret_cast<const std::byte*>(data), len);
}

ATLAS_NATIVE_API void atlas_unregister_all_entity_types()
{
    atlas::get_native_api_provider().unregister_all_entity_types();
}
```

### 3.4 进程级实现示例

```cpp
// src/server/baseapp/baseapp_native_provider.hpp

#include "clrscript/native_api_provider.hpp"

namespace atlas {

class BaseAppNativeProvider final : public INativeApiProvider
{
public:
    BaseAppNativeProvider(
        ChannelManager& channels,
        ClientProxyManager& clients,
        CellDirectory& cell_dir,
        EntityDefRegistry& registry,
        Clock& clock);

    // ---- 基础服务（所有进程共用逻辑）----
    void log_message(int32_t level,
        const char* msg, int32_t len) override;
    double server_time() override;
    float delta_time() override;

    // ---- BaseApp 特有行为 ----

    uint8_t get_process_prefix() override
    {
        return config_.base_prefix;
    }

    void send_client_rpc(uint32_t entity_id, uint32_t rpc_id,
        uint8_t target, const std::byte* payload, int32_t len) override
    {
        // BaseApp 持有客户端连接 — 直接发送
        auto* proxy = clients_.find(entity_id);
        if (!proxy)
        {
            ATLAS_LOG_WARNING("send_client_rpc: no client proxy for entity {}",
                entity_id);
            return;
        }
        proxy->send_rpc(rpc_id, target, payload, len);
    }

    void send_cell_rpc(uint32_t entity_id, uint32_t rpc_id,
        const std::byte* payload, int32_t len) override
    {
        // BaseApp → CellApp: 通过路由转发
        auto cell_addr = cell_dir_.lookup(entity_id);
        if (!cell_addr)
        {
            ATLAS_LOG_WARNING("send_cell_rpc: no cell for entity {}",
                entity_id);
            return;
        }
        auto bundle = make_cell_rpc_bundle(entity_id, rpc_id, payload, len);
        channels_.send_to(*cell_addr, bundle);
    }

    void send_base_rpc(uint32_t entity_id, uint32_t rpc_id,
        const std::byte* payload, int32_t len) override
    {
        // BaseApp 上收到 base RPC = 本地分发给 C#
        clr_engine_.dispatch_rpc(entity_id, rpc_id, payload, len);
    }

    // ---- 实体注册（所有进程共用逻辑）----
    void register_entity_type(const std::byte* data, int32_t len) override;
    void unregister_all_entity_types() override;

private:
    ChannelManager& channels_;
    ClientProxyManager& clients_;
    CellDirectory& cell_dir_;
    EntityDefRegistry& registry_;
    Clock& clock_;
    ClrScriptEngine& clr_engine_;
};
```

```cpp
// src/server/cellapp/cellapp_native_provider.hpp

class CellAppNativeProvider final : public INativeApiProvider
{
    // ...

    void send_client_rpc(...) override
    {
        // CellApp 无直接客户端连接 — 转发给 BaseApp 中转
        auto base_addr = base_dir_.lookup(entity_id);
        if (!base_addr)
        {
            ATLAS_LOG_WARNING("send_client_rpc: no base for entity {}",
                entity_id);
            return;
        }
        auto bundle = make_client_rpc_forward_bundle(
            entity_id, rpc_id, target, payload, len);
        channels_.send_to(*base_addr, bundle);
    }

    void send_cell_rpc(...) override
    {
        // CellApp 上收到 cell RPC = 本地分发给 C#
        clr_engine_.dispatch_rpc(entity_id, rpc_id, payload, len);
    }

    void send_base_rpc(...) override
    {
        // CellApp → BaseApp: 通过路由转发
        auto base_addr = base_dir_.lookup(entity_id);
        auto bundle = make_base_rpc_bundle(entity_id, rpc_id, payload, len);
        channels_.send_to(*base_addr, bundle);
    }
};
```

### 3.5 不支持操作的处理

对于 DBApp、LoginApp 等不需要 RPC 的进程，提供一个默认 Provider 基类：

```cpp
// src/lib/clrscript/base_native_provider.hpp

/// 基础 Provider — 提供日志/时间/注册等通用实现，
/// RPC 发送默认为 log error + no-op。
/// 各进程的 Provider 继承此基类，只 override 需要的方法。
class BaseNativeProvider : public INativeApiProvider
{
public:
    // ---- 通用实现（所有进程共享）----
    void log_message(int32_t level,
        const char* msg, int32_t len) override
    {
        std::string_view message(msg, static_cast<size_t>(len));
        switch (level)
        {
            case 2: ATLAS_LOG_INFO("{}", message); break;
            case 3: ATLAS_LOG_WARNING("{}", message); break;
            case 4: ATLAS_LOG_ERROR("{}", message); break;
            case 5: ATLAS_LOG_CRITICAL("{}", message); break;
            default: ATLAS_LOG_DEBUG("{}", message); break;
        }
    }

    double server_time() override { return clock_.now_seconds(); }
    float delta_time() override { return clock_.delta_time(); }

    void register_entity_type(const std::byte* data, int32_t len) override
    {
        registry_.register_type(data, len);
    }

    void unregister_all_entity_types() override
    {
        registry_.unregister_all();
    }

    // ---- RPC 默认为不支持 ----
    void send_client_rpc(...) override
    {
        ATLAS_LOG_ERROR("send_client_rpc not supported on this process type");
    }

    void send_cell_rpc(...) override
    {
        ATLAS_LOG_ERROR("send_cell_rpc not supported on this process type");
    }

    void send_base_rpc(...) override
    {
        ATLAS_LOG_ERROR("send_base_rpc not supported on this process type");
    }

    uint8_t get_process_prefix() override
    {
        ATLAS_LOG_ERROR("get_process_prefix not supported on this process type");
        return 0;
    }

protected:
    BaseNativeProvider(EntityDefRegistry& registry, Clock& clock)
        : registry_(registry), clock_(clock) {}

    EntityDefRegistry& registry_;
    Clock& clock_;
};
```

### 3.6 服务端进程启动流程

```cpp
// src/server/baseapp/main.cpp

int main(int argc, char* argv[])
{
    // 1. 初始化引擎基础服务
    auto config = load_config(argc, argv);
    auto clock = Clock();
    auto channels = ChannelManager(config.network);
    auto clients = ClientProxyManager();
    auto cell_dir = CellDirectory(channels);
    auto registry = EntityDefRegistry();

    // 2. 创建并注册 BaseApp 专用 Provider
    BaseAppNativeProvider provider(channels, clients, cell_dir, registry, clock);
    atlas::set_native_api_provider(&provider);

    // 3. 初始化 CLR 脚本引擎（此后 C# 调用 atlas_* 导出函数
    //    → 路由到 BaseAppNativeProvider 的实现）
    ClrScriptEngine engine;
    engine.configure({
        .runtime_config_path = config.dotnet_runtime_config,
        .runtime_assembly_path = config.runtime_dll_path,
    });
    engine.initialize();

    // 4. 主循环
    while (running)
    {
        clock.tick();
        channels.poll();
        engine.on_tick(clock.delta_time());
    }

    engine.finalize();
    return 0;
}
```

### 3.7 工作点

- [ ] 定义 `INativeApiProvider` 抽象接口
- [ ] 实现 `BaseNativeProvider` 基类（日志/时间/注册通用逻辑 + RPC 默认 no-op）
- [ ] 实现 `BaseAppNativeProvider`: RPC 发送（直接/路由）、进程前缀
- [ ] 实现 `CellAppNativeProvider`: RPC 发送（本地/转发）、空间管理
- [ ] 实现 `DBAppNativeProvider`: 仅注册+持久化，无 RPC（继承 BaseNativeProvider 即可）
- [ ] `clr_native_api.cpp`: 所有 `atlas_*` 导出函数改为一行委托
- [ ] `set_native_api_provider` 在 `ClrHost::initialize()` 前断言已注册
- [ ] 各 server `main.cpp` 在启动时注册对应 Provider
- [ ] 单元测试: Mock Provider 验证 atlas_* 函数正确转发
- [ ] 集成测试: 在错误进程类型上调用 RPC → 日志告警且不崩溃

---

## 4. 方案 C: 线程安全保证

### 4.1 问题分析

Atlas 引擎主循环是单线程设计。C++ 引擎 API（日志除外）不是线程安全的。
但 C# 天然鼓励异步编程：

```csharp
protected override async void OnTick(float dt)
{
    var data = await LoadFromDBAsync();   // await 后续体在线程池线程执行
    NativeApi.SendClientRpc(...);         // ⚠️ 从非主线程调用！数据竞争！
}
```

需要两层防御:
1. `SynchronizationContext` — 让 `await` 续体自动回到主线程
2. Debug 线程守卫 — 万一用户绕过 await（如 `Task.Run`），立即报错

### 4.2 AtlasSynchronizationContext

```csharp
// src/csharp/Atlas.Runtime/Core/AtlasSynchronizationContext.cs

using System.Collections.Concurrent;

namespace Atlas.Core;

/// <summary>
/// 自定义 SynchronizationContext — 确保 await 续体在主线程执行。
/// 原理与 Unity 的 UnitySynchronizationContext 相同。
/// </summary>
internal sealed class AtlasSynchronizationContext : SynchronizationContext
{
    private readonly ConcurrentQueue<WorkItem> _queue = new();
    private readonly int _mainThreadId;

    public AtlasSynchronizationContext()
    {
        _mainThreadId = Environment.CurrentManagedThreadId;
    }

    /// <summary>
    /// 异步回调投递（await 续体通过此方法排队）。
    /// </summary>
    public override void Post(SendOrPostCallback d, object? state)
    {
        _queue.Enqueue(new WorkItem(d, state, null));
    }

    /// <summary>
    /// 同步回调。主线程直接执行，非主线程排队并阻塞等待。
    /// </summary>
    public override void Send(SendOrPostCallback d, object? state)
    {
        if (Environment.CurrentManagedThreadId == _mainThreadId)
        {
            d(state);
            return;
        }

        using var signal = new ManualResetEventSlim(false);
        _queue.Enqueue(new WorkItem(d, state, signal));
        signal.Wait();
    }

    /// <summary>
    /// 在主线程 tick 中调用 — 逐个执行所有排队的 await 续体。
    /// 位于 EntityManager.OnTickAll 之前，确保续体中的引擎 API 调用安全。
    /// </summary>
    public int ProcessQueue()
    {
        int processed = 0;
        while (_queue.TryDequeue(out var item))
        {
            try
            {
                item.Callback(item.State);
            }
            catch (Exception ex)
            {
                Log.Error($"Unhandled exception in async continuation: {ex.Message}");
            }
            finally
            {
                item.Signal?.Set();
            }
            processed++;
        }
        return processed;
    }

    private readonly record struct WorkItem(
        SendOrPostCallback Callback, object? State,
        ManualResetEventSlim? Signal);
}
```

### 4.3 安装与集成

```csharp
// Bootstrap.Initialize() 中安装
[UnmanagedCallersOnly]
public static int Initialize()
{
    try
    {
        // 安装自定义 SynchronizationContext — await 续体自动回到主线程
        var syncContext = new AtlasSynchronizationContext();
        SynchronizationContext.SetSynchronizationContext(syncContext);
        EngineContext.SyncContext = syncContext;

        // 记录主线程 ID（线程守卫用）
        ThreadGuard.SetMainThread();

        EngineContext.Initialize();
        Log.Info("Atlas C# runtime initialized");
        return 0;
    }
    catch (Exception ex)
    {
        ErrorBridge.SetError(ex);
        return -1;
    }
}
```

```csharp
// Bootstrap.OnTick() 中处理队列
[UnmanagedCallersOnly]
public static int OnTick(float deltaTime)
{
    try
    {
        // 先处理所有 await 续体（它们可能调用引擎 API）
        EngineContext.SyncContext.ProcessQueue();

        // 再执行实体 tick
        EntityManager.Instance.OnTickAll(deltaTime);
        return 0;
    }
    catch (Exception ex)
    {
        ErrorBridge.SetError(ex);
        return -1;
    }
}
```

### 4.4 效果验证

安装 SynchronizationContext 后，以下代码自动安全：

```csharp
protected override async void OnTick(float dt)
{
    var data = await LoadFromDBAsync();   // 线程池执行 IO
    // ↓ await 续体被 Post 到 AtlasSynchronizationContext 队列
    // ↓ 下一帧 ProcessQueue() 在主线程执行
    NativeApi.SendClientRpc(...);         // ✅ 主线程，安全
    this.Health = data.Hp;                // ✅ 主线程，安全
}
```

### 4.5 ThreadGuard — Debug 模式第二层防御

对于绕过 await 直接使用 `Task.Run` 的场景：

```csharp
// src/csharp/Atlas.Runtime/Core/ThreadGuard.cs

using System.Diagnostics;
using System.Runtime.CompilerServices;

namespace Atlas.Core;

internal static class ThreadGuard
{
    private static int _mainThreadId;

    internal static void SetMainThread()
        => _mainThreadId = Environment.CurrentManagedThreadId;

    /// <summary>
    /// Debug 模式下验证调用在主线程。
    /// [Conditional("DEBUG")] 确保 Release 构建零开销。
    /// </summary>
    [Conditional("DEBUG")]
    internal static void EnsureMainThread(
        [CallerMemberName] string? caller = null)
    {
        if (Environment.CurrentManagedThreadId != _mainThreadId)
        {
            throw new InvalidOperationException(
                $"NativeApi.{caller}() must be called from the main thread. " +
                $"Current thread: {Environment.CurrentManagedThreadId}, " +
                $"main thread: {_mainThreadId}. " +
                $"Use 'await' to automatically return to the main thread.");
        }
    }
}
```

在 NativeApi wrapper 中调用（以 SendClientRpc 为例）：

```csharp
public static void SendClientRpc(
    uint entityId, int rpcId, byte target, ReadOnlySpan<byte> payload)
{
    ThreadGuard.EnsureMainThread();
    fixed (byte* ptr = payload)
        SendClientRpcNative(entityId, (uint)rpcId, target, ptr, payload.Length);
}
```

> **例外**: `atlas_log_message` 不需要线程守卫 — 日志系统通常是线程安全的，
> 且从后台线程写日志是合理需求。

### 4.6 工作点

- [ ] 实现 `AtlasSynchronizationContext`（`Post`/`Send`/`ProcessQueue`）
- [ ] `Bootstrap.Initialize()` 中安装 SynchronizationContext + 记录主线程 ID
- [ ] `Bootstrap.OnTick()` 中先调 `ProcessQueue()` 再调 `EntityManager.OnTickAll()`
- [ ] 实现 `ThreadGuard.EnsureMainThread()`（`[Conditional("DEBUG")]`）
- [ ] NativeApi 所有 wrapper 方法（除 `LogMessage` 外）添加 `ThreadGuard.EnsureMainThread()` 调用
- [ ] 单元测试: 从 Task.Run 中调用 NativeApi → Debug 模式抛出 `InvalidOperationException`
- [ ] 集成测试: `async void OnTick` 中使用 `await Task.Delay` → 续体在主线程执行
- [ ] 文档: 在用户面向的 API 文档中说明"所有引擎 API 必须在主线程调用，await 自动保证这一点"

---

## 5. 三个方案的执行顺序与依赖

```
ScriptPhase 0                ScriptPhase 1                   ScriptPhase 2
清理 Python                  .NET 10 嵌入                     C++ ↔ C# 互操作层
                                │
                                ├── 方案 A: CMake 共享库 ←─── atlas_* 导出函数需要宿主
                                │   (与 Phase 1 并行)
                                │
                                └───────────────────────┐
                                                        │
                             方案 B: Provider 接口 ──────┤── ScriptPhase 2 Task 2.4
                             (ScriptPhase 2 首要任务)    │
                                                        │
                             方案 C: 线程安全 ───────────┤── ScriptPhase 2/3
                             (纯 C# 增量改动)            │
                                                        ▼
                                                   ScriptPhase 3+
                                                   (依赖以上三个方案)
```

| 方案 | 最早可开始 | 阻塞 | 工作量 |
|------|-----------|------|--------|
| C (线程安全) | ScriptPhase 2 启动时 | 不阻塞其他工作 | ~2 天 |
| B (Provider) | ScriptPhase 2 Task 2.4 | 阻塞 RPC 发送实现 | ~3 天 |
| A (共享库) | ScriptPhase 1 并行 | 阻塞 C# P/Invoke 集成测试 | ~3-5 天 |

**推荐顺序**: C → B → A（风险递减，A 如时间紧可暂用平台特定 linker flag 过渡）

---

## 6. 风险与缓解

| 风险 | 概率 | 缓解 |
|------|------|------|
| 共享库重构导致链接错误 | 中 | 增量迁移: 先验证一个 server exe + shared lib，再推广 |
| Provider 虚函数调用开销 | 极低 | 每次 NativeApi 调用增加一次虚调用（~1ns），远小于 P/Invoke 开销（~10-30ns） |
| SynchronizationContext 队列延迟 | 低 | await 续体延迟到下一帧执行（最多一帧，16-33ms），对游戏逻辑可接受 |
| 用户在 Task.Run 中调用引擎 API | 中 | Debug 模式 ThreadGuard 立即报错 + 文档明确说明 |
| 日志从后台线程调用 | 常见 | LogMessage 不加线程守卫，日志系统本身线程安全 |
