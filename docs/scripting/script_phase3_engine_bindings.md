# ScriptPhase 3: Atlas 引擎 C# 绑定

> 预估周期: 3–4 周 | 前置依赖: ScriptPhase 2 完成 + [NativeApi 架构基础设施](native_api_architecture.md) 中的线程安全方案就位

---

## 目标

1. 将 Atlas 引擎的核心 API 暴露给 C# 脚本（替代 `atlas_module.cpp`）
2. 实现 `ClrScriptEngine`（`ScriptEngine` 的 .NET 实现）
3. 建立 C# 侧的运行时框架 `Atlas.Runtime`
4. 实现实体生命周期回调机制

## 验收标准 (M3)

- [ ] C# 脚本中可调用 `Atlas.Log.Info()`, `Atlas.Log.Warning()`, `Atlas.Log.Error()`
- [ ] C# 脚本中可读取 `Atlas.Time.ServerTime`, `Atlas.Time.DeltaTime`
- [ ] `ClrScriptEngine` 实现 `ScriptEngine` 接口的完整生命周期
- [ ] C++ 侧可触发 C# 实体的 `OnInit` / `OnTick` / `OnShutdown` 回调
- [ ] 原有 `atlas_module.cpp` 的全部功能在 C# 侧有对应实现

---

## 任务 3.1: `Atlas.Runtime` C# 项目

### 新建目录: `src/csharp/Atlas.Runtime/`

```
src/csharp/Atlas.Runtime/
├── Atlas.Runtime.csproj
├── Core/
│   ├── Bootstrap.cs            # [UnmanagedCallersOnly] 入口点
│   ├── EngineContext.cs         # 全局引擎上下文（初始化状态管理）
│   └── NativeApi.cs            # 底层函数指针绑定（ScriptPhase 4 由 Generator 替代）
├── Log/
│   └── Log.cs                  # Atlas.Log 静态类
├── Time/
│   └── GameTime.cs             # Atlas.Time 静态类
├── Entity/
│   ├── ServerEntity.cs         # 实体基类
│   ├── EntityManager.cs        # 实体注册表 + 生命周期分发
│   └── IEntityLifecycle.cs     # 生命周期接口
└── Events/
    ├── EventBus.cs             # 发布/订阅事件总线
    └── IEventListener.cs       # 事件监听器接口

# 注意: Vector3/Quaternion 统一定义在 Atlas.Shared（ScriptPhase 4），
# Atlas.Runtime 通过引用 Atlas.Shared 使用，不再重复定义
```

### 项目文件: `Atlas.Runtime.csproj`

```xml
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net9.0</TargetFramework>
    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>
    <LangVersion>latest</LangVersion>
    <Nullable>enable</Nullable>
    <RootNamespace>Atlas</RootNamespace>
  </PropertyGroup>

  <ItemGroup>
    <!-- 引用共享库 -->
    <ProjectReference Include="../Atlas.Shared/Atlas.Shared.csproj" />

    <!-- Interop Source Generator -->
    <ProjectReference Include="../Atlas.Generators.Interop/Atlas.Generators.Interop.csproj"
                      OutputItemType="Analyzer"
                      ReferenceOutputAssembly="false" />
  </ItemGroup>
</Project>
```

### EngineContext 定义: `src/csharp/Atlas.Runtime/Core/EngineContext.cs`

```csharp
namespace Atlas.Core;

/// <summary>
/// 全局引擎上下文，管理初始化状态和脚本程序集加载。
/// </summary>
internal static class EngineContext
{
    private static bool _initialized;
    private static ScriptHost? _scriptHost;

    /// <summary>
    /// 自定义 SynchronizationContext（由 Bootstrap.Initialize 设置）。
    /// OnTick 中调用 ProcessQueue() 执行 await 续体。
    /// </summary>
    public static AtlasSynchronizationContext SyncContext { get; set; } = null!;

    public static void Initialize()
    {
        if (_initialized)
            throw new InvalidOperationException("EngineContext already initialized");

        // 从 C++ 获取进程前缀并设置到 EntityManager
        var prefix = NativeApi.GetProcessPrefix();
        EntityManager.Instance.SetProcessPrefix(prefix);

        _initialized = true;
    }

    public static void LoadScriptAssembly(string path)
    {
        _scriptHost = new ScriptHost();
        _scriptHost.Load(path);
    }

    public static void Shutdown()
    {
        if (!_initialized) return;
        _scriptHost?.Dispose();
        _scriptHost = null;
        _initialized = false;
    }
}
```

### NativeApi 定义: `src/csharp/Atlas.Runtime/Core/NativeApi.cs`

> **注意**: Phase 3 实现手写版本（直接声明 `[LibraryImport]`）。ScriptPhase 4 的 Interop Generator 可替代此文件，
> 由 `[NativeImport]` attribute 自动生成 `[LibraryImport]` 声明和 Span wrapper。

```csharp
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Atlas.Core;

/// <summary>
/// C# → C++ 原生函数调用层。
/// 通过 [LibraryImport] source-generated P/Invoke 直接调用 C++ 导出函数。
/// "atlas_engine" 指向引擎共享库 atlas_engine.dll/.so（详见 native_api_architecture.md §2）。
/// </summary>
internal static unsafe partial class NativeApi
{
    private const string LibName = "atlas_engine";

    // ---- 日志 ----
    [LibraryImport(LibName, EntryPoint = "atlas_log_message")]
    private static partial void LogMessageNative(int level, byte* msg, int len);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void LogMessage(int level, ReadOnlySpan<byte> message)
    {
        fixed (byte* ptr = message)
            LogMessageNative(level, ptr, message.Length);
    }

    // ---- 时间 ----
    [LibraryImport(LibName, EntryPoint = "atlas_server_time")]
    public static partial double ServerTime();

    [LibraryImport(LibName, EntryPoint = "atlas_delta_time")]
    public static partial float DeltaTime();

    // ---- 进程前缀 ----
    [LibraryImport(LibName, EntryPoint = "atlas_get_process_prefix")]
    public static partial byte GetProcessPrefix();

    // ---- RPC 发送 ----
    [LibraryImport(LibName, EntryPoint = "atlas_send_client_rpc")]
    private static partial void SendClientRpcNative(
        uint entityId, uint rpcId, byte target, byte* payload, int len);

    public static void SendClientRpc(
        uint entityId, int rpcId, byte target, ReadOnlySpan<byte> payload)
    {
        fixed (byte* ptr = payload)
            SendClientRpcNative(entityId, (uint)rpcId, target, ptr, payload.Length);
    }

    [LibraryImport(LibName, EntryPoint = "atlas_send_cell_rpc")]
    private static partial void SendCellRpcNative(
        uint entityId, uint rpcId, byte* payload, int len);

    public static void SendCellRpc(
        uint entityId, int rpcId, ReadOnlySpan<byte> payload)
    {
        fixed (byte* ptr = payload)
            SendCellRpcNative(entityId, (uint)rpcId, ptr, payload.Length);
    }

    [LibraryImport(LibName, EntryPoint = "atlas_send_base_rpc")]
    private static partial void SendBaseRpcNative(
        uint entityId, uint rpcId, byte* payload, int len);

    public static void SendBaseRpc(
        uint entityId, int rpcId, ReadOnlySpan<byte> payload)
    {
        fixed (byte* ptr = payload)
            SendBaseRpcNative(entityId, (uint)rpcId, ptr, payload.Length);
    }

    // ---- 实体类型注册 ----
    [LibraryImport(LibName, EntryPoint = "atlas_register_entity_type")]
    private static partial void RegisterEntityTypeNative(byte* data, int len);

    public static void RegisterEntityType(ReadOnlySpan<byte> data)
    {
        fixed (byte* ptr = data)
            RegisterEntityTypeNative(ptr, data.Length);
    }

    [LibraryImport(LibName, EntryPoint = "atlas_unregister_all_entity_types")]
    public static partial void UnregisterAllEntityTypes();
}
```

### 工作点

- [ ] 创建项目结构和 `.csproj`
- [ ] `AllowUnsafeBlocks=true` 用于 `fixed` 语句和 `byte*` 参数
- [ ] 引用 `Atlas.Generators.Interop`（ScriptPhase 2 创建）
- [ ] **前置依赖**: ScriptPhase 3 启动前必须先完成 ScriptPhase 4 的任务 4.1 (`Atlas.Shared` 项目) + 4.2 (Attribute 定义 + `ReplicationScope`) + 4.3 (`SpanWriter`/`SpanReader`)。这三个任务不依赖 Phase 3 的任何产出，可提前执行
- [ ] `RootNamespace=Atlas` 使所有类型在 `Atlas.*` 命名空间下
- [ ] `NativeApi`: 手写 `[LibraryImport("atlas_engine")]` 声明，高层 wrapper 处理 `ReadOnlySpan<byte>` → `byte*` 转换 + `ThreadGuard.EnsureMainThread()`

---

## 任务 3.2: Bootstrap 入口

### 新建文件: `src/csharp/Atlas.Runtime/Core/Bootstrap.cs`

```csharp
using System.Runtime.InteropServices;

namespace Atlas.Core;

public static class Bootstrap
{
    // 引擎构建为共享库 atlas_engine.dll/.so 后，
    // [LibraryImport("atlas_engine")] 标准 P/Invoke 即可加载，
    // 不再需要 DllImportResolver hack。

    [UnmanagedCallersOnly]
    public static int Initialize()
    {
        try
        {
            // 安装自定义 SynchronizationContext（详见 native_api_architecture.md §4）
            var syncContext = new AtlasSynchronizationContext();
            SynchronizationContext.SetSynchronizationContext(syncContext);
            EngineContext.SyncContext = syncContext;

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

    [UnmanagedCallersOnly]
    public static int Shutdown()
    {
        try
        {
            Log.Info("Atlas C# runtime shutting down");
            EngineContext.Shutdown();
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    [UnmanagedCallersOnly]
    public static unsafe int LoadGameScripts(byte* pathUtf8, int pathLen)
    {
        try
        {
            var path = Encoding.UTF8.GetString(
                new ReadOnlySpan<byte>(pathUtf8, pathLen));
            EngineContext.LoadScriptAssembly(path);
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    [UnmanagedCallersOnly]
    public static int OnInit(byte isReload)
    {
        try
        {
            EntityManager.Instance.OnInitAll(isReload != 0);
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    [UnmanagedCallersOnly]
    public static int OnTick(float deltaTime)
    {
        try
        {
            // 先处理 await 续体（保证在主线程执行），再执行实体 tick
            EngineContext.SyncContext.ProcessQueue();
            EntityManager.Instance.OnTickAll(deltaTime);
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    [UnmanagedCallersOnly]
    public static int OnShutdown()
    {
        try
        {
            EntityManager.Instance.OnShutdownAll();
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }
}
```

### 工作点

- [ ] 每个入口方法遵循 try/catch + `ErrorBridge` 模式
- [ ] `Initialize`: 安装 `AtlasSynchronizationContext` + `ThreadGuard.SetMainThread()`（参见 [native_api_architecture.md](native_api_architecture.md) §4）
- [ ] `OnTick`: 先调 `SyncContext.ProcessQueue()` 处理 await 续体，再调 `EntityManager.OnTickAll()`
- [ ] `Shutdown`: 清理引擎上下文
- [ ] `LoadGameScripts`: 加载用户脚本程序集（路径以 UTF-8 传入）
- [ ] `OnInit` / `OnTick` / `OnShutdown`: 生命周期回调分发到所有实体

---

## 任务 3.3: 实现 `ClrScriptEngine`

### 新建文件: `src/lib/clrscript/clr_script_engine.hpp`, `clr_script_engine.cpp`

```cpp
// 必须与 ScriptPhase 0 定义的 ScriptEngine 接口完全一致
class ClrScriptEngine final : public ScriptEngine
{
public:
    // --- ScriptEngine 接口实现 ---
    [[nodiscard]] auto initialize() -> Result<void> override;
    void finalize() override;
    [[nodiscard]] auto load_module(
        const std::filesystem::path& path) -> Result<void> override;
    void on_tick(float dt) override;
    void on_init(bool is_reload = false) override;
    void on_shutdown() override;
    [[nodiscard]] auto call_function(
        std::string_view module_name,
        std::string_view function_name,
        std::span<const ScriptValue> args) -> Result<ScriptValue> override;
    [[nodiscard]] auto runtime_name() const -> std::string_view override
    {
        return "CLR (.NET 9)";
    }

    // --- ClrScriptEngine 特有方法 ---

    // 配置结构体（在 initialize() 之前通过 configure() 设置）
    struct Config
    {
        std::filesystem::path runtime_config_path;
        std::filesystem::path runtime_assembly_path;  // Atlas.Runtime.dll
    };

    [[nodiscard]] auto configure(const Config& config) -> Result<void>;
    [[nodiscard]] auto is_initialized() const -> bool { return initialized_; }

    // --- 热重载支持（ScriptPhase 5 使用）---

    // 调用 C# 侧 [UnmanagedCallersOnly] 方法（通过已绑定的 ClrStaticMethod）
    // name: 方法标识（如 "SerializeAndUnload", "LoadAndRestore"）
    [[nodiscard]] auto call_managed(std::string_view name)
        -> Result<void>;

    // 带参数版本: 传递程序集路径（UTF-8）
    [[nodiscard]] auto call_managed(std::string_view name,
        const std::filesystem::path& assembly_path) -> Result<void>;

    // 释放所有 C++ 侧持有的 ClrObject（GCHandle），热重载卸载前调用
    void release_all_script_objects();

    // 清空脚本方法函数指针缓存（call_cache_ + 脚本相关 ClrStaticMethod）
    void reset_script_method_cache();

    // 重新绑定脚本方法函数指针到新加载的程序集
    [[nodiscard]] auto rebind_script_methods() -> Result<void>;

private:
    Config config_;
    ClrHost host_;

    // 缓存的 Bootstrap 函数指针（C++ → C# 方向，bind 时传入 host_ 引用）
    ClrStaticMethod<int>                     bootstrap_init_;  // 无参数（C# 通过 LibraryImport 直接调 C++）
    ClrStaticMethod<int>                     bootstrap_shutdown_;
    ClrStaticMethod<int, const uint8_t*, int> load_scripts_;
    ClrStaticMethod<int, uint8_t>            on_init_;
    ClrStaticMethod<int, float>              on_tick_;
    ClrStaticMethod<int>                     on_shutdown_;

    // 热重载方法（ScriptPhase 5 绑定）
    ClrStaticMethod<int>                     hot_serialize_and_unload_;
    ClrStaticMethod<int, const uint8_t*, int> hot_load_and_restore_;

    // GCHandle 释放（C++ → C# 方向）
    ClrStaticMethod<void, void*>          free_gc_handle_;

    // call_function() 的动态方法调用缓存
    // 按 "module_name::function_name" 缓存已解析的函数指针
    std::unordered_map<std::string, ClrStaticMethod<int>> call_cache_;

    // C++ 侧 ClrObject 注册表（热重载时需要全部释放）
    ClrObjectRegistry object_registry_;

    bool initialized_{false};
};
```

### `initialize()` 流程

```
ClrScriptEngine::initialize()
    │
    ├── 1. host_.initialize(config_.runtime_config_path)  // 启动 CoreCLR
    │
    ├── 2. 绑定 Bootstrap 方法                             // C++ → C# 方向
    │   ├── bootstrap_init_.bind(host_, ...)    // Bootstrap.Initialize()
    │   ├── bootstrap_shutdown_.bind(host_, ...)
    │   ├── load_scripts_.bind(host_, ...)
    │   ├── on_init_.bind(host_, ...)
    │   ├── on_tick_.bind(host_, ...)
    │   ├── on_shutdown_.bind(host_, ...)
    │   └── free_gc_handle_.bind(host_, ...)    // C++ → C# GCHandle 释放
    │
    └── 3. bootstrap_init_.invoke()             // 触发 C# 初始化
                                                 // 安装 SynchronizationContext + ThreadGuard
                                                 // C# → C++ 方向通过 [LibraryImport("atlas_engine")] 直接调用
```

> **架构要点**:
> - C# → C++ 方向通过 `[LibraryImport("atlas_engine")]` 直接调用共享库中的导出函数
> - C++ → C# 方向通过 `ClrStaticMethod` + `[UnmanagedCallersOnly]`
> - `atlas_*` 导出函数内部委托给 `INativeApiProvider`，各进程类型注册不同实现
> - 详见 [native_api_architecture.md](native_api_architecture.md)

### 工作点

- [ ] `initialize()`: 按上述流程初始化，`configure()` 必须先于 `initialize()` 调用
- [ ] `finalize()`: 调用 `bootstrap_shutdown_` → `host_.finalize()`
- [ ] `load_module()`: 调用 `load_scripts_`，参数为 `std::filesystem::path`
- [ ] `on_tick()`: 调用缓存的 `on_tick_` 函数指针（每帧调用，必须零查找开销）
- [ ] `free_gc_handle_`: 由 `ClrObject` 析构时调用，方向是 C++ → C#
- [ ] 所有 `invoke()` 返回值检查: 非 0 则从 `ClrError` 读取错误
- [ ] `call_function()` 实现:
  - `module_name` 映射到已加载的 C# 程序集名称（如 `"Atlas.GameScripts"`）
  - `function_name` 映射到 `[UnmanagedCallersOnly]` 静态方法的完全限定名
  - 按 `"module::function"` 缓存到 `call_cache_` 中，首次调用 `host_.get_method()` 解析
  - 返回值通过 `ScriptValue` 编组（Phase 2 `ClrMarshal`）
  - 此方法主要用于脚本间通信和调试工具，热路径（OnTick）不使用
- [ ] `finalize()` 中必须清空 `call_cache_` 和所有 `ClrStaticMethod`，再调用 `host_.finalize()`
- [ ] 初始化失败回滚: 任一步骤失败则 reset 所有已绑定方法 + `host_.finalize()`

---

## 任务 3.4: 日志绑定

### 对应关系 (`atlas_module.cpp` → C#)

| Python 函数 | C++ trampoline | C# API | LogLevel 值 |
|-------------|---------------|--------|------------|
| `atlas.log_info(msg)` | `log_trampoline(2, msg, len)` | `Atlas.Log.Info(msg)` | `Info = 2` |
| `atlas.log_warning(msg)` | `log_trampoline(3, msg, len)` | `Atlas.Log.Warning(msg)` | `Warning = 3` |
| `atlas.log_error(msg)` | `log_trampoline(4, msg, len)` | `Atlas.Log.Error(msg)` | `Error = 4` |
| `atlas.log_critical(msg)` | `log_trampoline(5, msg, len)` | `Atlas.Log.Critical(msg)` | `Critical = 5` |

> **注意**: 级别值必须与 `log.hpp` 中 `LogLevel` 枚举一致: Trace=0, Debug=1, Info=2, Warning=3, Error=4, Critical=5。

### C++ 导出函数实现

> `atlas_log_message` 已在 Phase 2 的 `clr_native_api.cpp` 中定义为 `extern "C" ATLAS_EXPORT` 函数。
> C# 通过 `[LibraryImport("atlas_engine", EntryPoint = "atlas_log_message")]` 直接调用。

### C# 包装: `src/csharp/Atlas.Runtime/Log/Log.cs`

```csharp
namespace Atlas;

public static class Log
{
    // 级别值与 C++ LogLevel 枚举对齐: Info=2, Warning=3, Error=4, Critical=5
    public static void Info(string message)
    {
        var bytes = Encoding.UTF8.GetBytes(message);
        NativeApi.LogMessage(2, bytes);
    }

    public static void Warning(string message)
    {
        var bytes = Encoding.UTF8.GetBytes(message);
        NativeApi.LogMessage(3, bytes);
    }

    public static void Error(string message)
    {
        var bytes = Encoding.UTF8.GetBytes(message);
        NativeApi.LogMessage(4, bytes);
    }

    public static void Critical(string message)
    {
        var bytes = Encoding.UTF8.GetBytes(message);
        NativeApi.LogMessage(5, bytes);
    }
}
```

### 工作点

- [ ] C++ `atlas_log_message` 导出函数实现（已在 Phase 2 `clr_native_api.cpp` 中定义）
- [ ] C# `Log` 类通过 `NativeApi.LogMessage` 调用
- [ ] UTF-8 编码字符串传递: `Encoding.UTF8.GetBytes` → `ReadOnlySpan<byte>` → `fixed` → P/Invoke
- [ ] **性能优化（后续 TODO）**: 当前 `GetBytes(string)` 每次分配 `byte[]`，高频日志有 GC 压力。改用 `stackalloc` + `Encoding.UTF8.GetBytes(string, Span<byte>)` 消除分配；或使用 .NET 9 `Utf8.TryWrite`

---

## 任务 3.5: 时间系统绑定

### 对应关系

| Python 函数 | C++ trampoline | C# API |
|-------------|---------------|--------|
| `atlas.server_time()` | `server_time_trampoline()` | `Atlas.Time.ServerTime` |
| (新增) | `delta_time_trampoline()` | `Atlas.Time.DeltaTime` |

### C# 包装: `src/csharp/Atlas.Runtime/Time/GameTime.cs`

```csharp
namespace Atlas;

public static class Time
{
    public static double ServerTime => NativeApi.ServerTime();
    public static float DeltaTime => NativeApi.DeltaTime();
}
```

### 工作点

- [ ] `server_time_trampoline`: 调用 `Clock::now()` 返回 epoch 秒数
- [ ] `delta_time_trampoline`: 返回上一帧 dt（从引擎主循环中更新）
- [ ] 属性而非方法——语义更清晰，每次读取都是最新值

---

## 任务 3.6: 实体生命周期框架

### C# 实体基类: `src/csharp/Atlas.Runtime/Entity/ServerEntity.cs`

```csharp
namespace Atlas.Entity;

public abstract class ServerEntity
{
    public uint EntityId { get; internal set; }
    public bool IsDestroyed { get; internal set; }

    /// <summary>
    /// 实体类型名。由 Entity Source Generator 在子类中 override 为编译期常量，
    /// 例如 [Entity("Player")] 会生成: public override string TypeName => "Player";
    /// 用于序列化、工厂创建、热重载状态迁移等场景，避免运行时反射。
    /// </summary>
    public abstract string TypeName { get; }

    protected virtual void OnInit(bool isReload) { }
    protected virtual void OnTick(float deltaTime) { }
    protected virtual void OnDestroy() { }
}
```

### C# 实体管理器: `src/csharp/Atlas.Runtime/Entity/EntityManager.cs`

```csharp
namespace Atlas.Entity;

public sealed class EntityManager
{
    public static EntityManager Instance { get; } = new();

    private readonly Dictionary<uint, ServerEntity> _entities = new();

    // 分布式 EntityId: 高 8 位为进程前缀，低 24 位为本地自增序列
    // processPrefix 由 C++ 引擎导出的 atlas_get_process_prefix() 函数提供
    // 每个 BaseApp/CellApp 进程拥有唯一前缀，确保全局唯一
    private byte _processPrefix;
    private uint _localSeq = 1;

    /// <summary>
    /// 由 Bootstrap.Initialize 调用，设置进程前缀（从 C++ 引擎获取）
    /// </summary>
    internal void SetProcessPrefix(byte prefix) => _processPrefix = prefix;

    private uint AllocateEntityId()
    {
        if (_localSeq >= 0x0100_0000)
            throw new InvalidOperationException("EntityId local sequence overflow");
        return ((uint)_processPrefix << 24) | _localSeq++;
    }

    // 延迟创建/销毁队列 — 避免在遍历 _entities 时修改字典
    private readonly List<ServerEntity> _pendingCreates = new();
    private readonly List<uint> _pendingDestroys = new();

    public T Create<T>() where T : ServerEntity, new()
    {
        var entity = new T { EntityId = AllocateEntityId() };
        if (_iterating)
        {
            _pendingCreates.Add(entity);
        }
        else
        {
            _entities[entity.EntityId] = entity;
        }
        return entity;
    }

    internal void OnInitAll(bool isReload)
    {
        _iterating = true;
        foreach (var entity in _entities.Values)
            entity.OnInit(isReload);
        ProcessPendingDestroys();
    }

    internal void OnTickAll(float deltaTime)
    {
        _iterating = true;
        foreach (var entity in _entities.Values)
        {
            if (!entity.IsDestroyed)
                entity.OnTick(deltaTime);
        }
        ProcessPendingDestroys();
    }

    internal void OnShutdownAll()
    {
        foreach (var entity in _entities.Values)
            entity.OnDestroy();
        _entities.Clear();
    }

    /// <summary>
    /// 将销毁延迟到当前遍历结束后执行。
    /// 在 OnTick/OnInit 回调中调用 Destroy() 时自动进入延迟队列。
    /// </summary>
    private bool _iterating;

    public void Destroy(uint entityId)
    {
        if (_iterating)
        {
            _pendingDestroys.Add(entityId);
            return;
        }
        DestroyImmediate(entityId);
    }

    private void DestroyImmediate(uint entityId)
    {
        if (_entities.Remove(entityId, out var entity))
        {
            entity.OnDestroy();
            entity.IsDestroyed = true;
        }
    }

    private void ProcessPendingDestroys()
    {
        _iterating = false;
        foreach (var entity in _pendingCreates)
            _entities[entity.EntityId] = entity;
        _pendingCreates.Clear();
        foreach (var id in _pendingDestroys)
            DestroyImmediate(id);
        _pendingDestroys.Clear();
    }

    /// <summary>获取所有实体（热重载序列化时使用）</summary>
    internal IReadOnlyCollection<ServerEntity> GetAllEntities()
        => _entities.Values;

    /// <summary>注册已有实体（热重载反序列化恢复时使用）</summary>
    internal void Register(ServerEntity entity)
    {
        _entities[entity.EntityId] = entity;
    }

    /// <summary>清除所有实体（热重载卸载前调用，不触发 OnDestroy）</summary>
    internal void Clear() => _entities.Clear();
}
```

### 工作点

- [ ] `ServerEntity` 虚方法: `OnInit`, `OnTick`, `OnDestroy`
- [ ] `EntityManager` 管理实体注册表
- [ ] `OnTickAll`: 遍历所有实体调用 `OnTick`，跳过 `IsDestroyed` 实体
- [ ] **延迟销毁**: `OnTick`/`OnInit` 回调中调用 `Destroy()` 时进入延迟队列，遍历结束后统一执行，避免 `InvalidOperationException`
- [ ] C++ `ClrScriptEngine::on_tick(dt)` → `Bootstrap.OnTick(dt)` → `EntityManager.OnTickAll(dt)`
- [ ] 后续 ScriptPhase 4 的 Entity Generator 会替代手写的遍历为编译期生成的 switch

---

## 任务 3.7: 常量绑定

### 对应关系

| Python 常量 | C# 对应 |
|-------------|---------|
| `atlas.VERSION_MAJOR = 0` | `Atlas.Engine.VersionMajor` |
| `atlas.VERSION_MINOR = 1` | `Atlas.Engine.VersionMinor` |
| `atlas.ENGINE_NAME = "Atlas"` | `Atlas.Engine.Name` |

### C#: `src/csharp/Atlas.Runtime/Core/EngineInfo.cs`

```csharp
namespace Atlas;

public static class Engine
{
    public const int VersionMajor = 0;
    public const int VersionMinor = 1;
    public const string Name = "Atlas";
}
```

### 工作点

- [ ] 版本号与 `CMakeLists.txt` 中的 `project(Atlas VERSION 0.1.0)` 保持一致
- [ ] 后续考虑从 C++ 侧传递版本号，而非硬编码

---

## 任务 3.8: 单元 / 集成测试

### C++ 侧测试

| 测试文件 | 核心用例 |
|---------|---------|
| `test_clr_script_engine.cpp` | `initialize()` → `on_init()` → `on_tick()` → `on_shutdown()` → `finalize()` 完整生命周期 |
| `test_clr_script_engine.cpp` | `load_module()` 成功 / 不存在路径→Error |
| `test_clr_script_engine.cpp` | 未 initialize 就调用 on_tick → Error |

### C# 侧测试: `tests/csharp/Atlas.RuntimeTests/`

| 测试类 | 核心用例 |
|--------|---------|
| `LogTests` | `Log.Info` 调用 NativeApi（mock 验证） |
| `EntityManagerTests` | Create → OnTickAll → Destroy → 验证回调顺序 |
| `EntityManagerTests` | 销毁后再 Tick 不崩溃 |
| `BootstrapTests` | Initialize 设置全局状态 → Shutdown 清理 |

### 集成测试

| 测试文件 | 验证内容 |
|---------|---------|
| `test_engine_lifecycle.cpp` | C++ 启动引擎 → C# 初始化 → Tick 100帧 → 关闭 → 内存无泄漏 |

---

## 任务依赖图

```
3.1 Atlas.Runtime 项目结构 ─────┐
                                │
3.2 Bootstrap 入口 ─────────────┤
                                │
3.3 ClrScriptEngine ────────────┤
        │                       │
3.4 日志绑定 ───────────────────┤
        │                       ├── 3.8 测试
3.5 时间系统绑定 ───────────────┤
        │                       │
3.6 实体生命周期 ───────────────┤
                                │
3.7 常量绑定 ───────────────────┘
```

**建议执行顺序**: 3.1 → 3.2 → 3.3 → 3.4 + 3.5 (并行) → 3.6 → 3.7 → 3.8 → 3.9

---

## 新增任务: C++ / C# 序列化格式对齐

> 源自架构讨论中关于"引擎底层序列化怎么做"的问题，已独立成文。

### 任务 3.9: 序列化对齐基础设施

**详细设计文档**: [serialization_alignment.md](serialization_alignment.md)

确保 C# `SpanWriter`/`SpanReader` 与 C++ `BinaryWriter`/`BinaryReader` 字节级兼容：

- [ ] 实现 `SpanWriter` — `ref struct`，基于 `ArrayPool<byte>`，小端格式
  - 全部基本类型 + `WritePackedUInt32` + `WriteString` + `WriteVector3` / `WriteQuaternion`
- [ ] 实现 `SpanReader` — `ref struct`，对称的读取方法
  - `ReadPackedUInt32` 与 C++ `read_packed_int` 逐字节对齐
- [ ] C++ 导出函数扩展: `atlas_send_client_rpc`, `atlas_send_cell_rpc`, `atlas_send_base_rpc`（已在 Phase 2 声明）
- [ ] 跨语言往返测试: C++ 写 → C# 读 / C# 写 → C++ 读
  - 覆盖全部类型 + packed int 临界值 (254/255/256) + 大数据
- [ ] C# 侧单元测试: 往返、边界值、ArrayPool 归还、自动扩容
