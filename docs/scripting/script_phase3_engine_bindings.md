# ScriptPhase 3: Atlas 引擎 C# 绑定

> 预估周期: 3–4 周 | 前置依赖: ScriptPhase 2 完成 + [NativeApi 架构基础设施](native_api_architecture.md) 中的线程安全方案就位 | **状态: 🟡 进行中**

---

## 目标

1. 将 Atlas 引擎的核心 API 暴露给 C# 脚本（替代原 `atlas_module.cpp`）。
2. 实现 `ClrScriptEngine`（`ScriptEngine` 的 .NET 实现）。
3. 建立 C# 侧的运行时框架 `Atlas.Runtime`。
4. 实现实体生命周期回调机制。

## 验收标准 (M3)

- [x] `Atlas.Log.Trace/Debug/Info/Warning/Error/Critical()` 可用。
- [x] `Atlas.Time.ServerTime` / `Atlas.Time.DeltaTime` 可读取。
- [x] `ClrScriptEngine` 实现 `ScriptEngine` 接口完整生命周期。
- [x] C++ 侧可触发 C# 实体的 `OnInit` / `OnTick` / `OnDestroy` 回调。
- [~] 原 `atlas_module.cpp` 的全量能力平移到 C#（日志/时间/进程前缀/RPC/EntityDefRegistry 注册已到位；mailbox、delta 压缩等能力按后续设计文档逐步扩展）。

---

## 已落地组件

### `ClrScriptEngine` — `src/lib/clrscript/clr_script_engine.{h,cc}`

| 阶段 | API | 说明 |
|------|-----|------|
| 配置 | `Configure(const Config&)` | 接收 `runtime_config_path`、`runtime_assembly_path`、可选的 `ClrBootstrapArgs` |
| 初始化 | `Initialize()` | 启动 CoreCLR → 经 `ClrBootstrap` 注入 error bridge / vtable → 绑定 C# 生命周期入口 |
| 生命周期 | `OnInit(bool) / OnTick(float) / OnShutdown()` | 直接调用缓存的 `ClrFallibleMethod<>` |
| 释放 | `Finalize()` | 通过 C# `Lifecycle.EngineShutdown` 收尾 → `ClrHost::Finalize()` |
| 热重载 | `CallHotReload(method)` / `CallHotReload(method, path)` / `Host()` | 专供 `ClrHotReload` 调用 |

缓存的 C# 入口点（均为 `Atlas.Core.Lifecycle, Atlas.Runtime` 下的 `[UnmanagedCallersOnly]` 静态方法）：

- `EngineInit`、`EngineShutdown`
- `OnInit(byte isReload)`、`OnTick(float deltaTime)`、`OnShutdown()`
- 热重载相关入口位于 `Atlas.Hosting.HotReloadManager`：`LoadScripts` / `SerializeAndUnload` / `LoadAndRestore`

### `Atlas.Runtime` C# 项目 — `src/csharp/Atlas.Runtime/`

```
Atlas.Runtime/
├── Core/
│   ├── Bootstrap.cs              — CLR 层 handshake（接收 BootstrapArgs*、注册 ErrorBridge、
│   │                                 填充 ObjectVTableOut* 7 个函数指针）
│   ├── Lifecycle.cs              — 引擎生命周期 [UnmanagedCallersOnly]：EngineInit/
│   │                                 EngineShutdown/OnInit/OnTick/OnShutdown；内部
│   │                                 DoEngineInit/DoOnTick 等供测试 forwarder 共享
│   ├── EngineContext.cs          — 全局状态（ScriptHost、SyncContext）
│   ├── EngineInfo.cs             — 版本号常量（Atlas.Engine.Name/VersionMajor/VersionMinor）
│   ├── NativeApi.cs              — [LibraryImport("atlas_engine")] 声明 + Span 包装
│   ├── NativeCallbacks.cs        — C++ → C# 回调注册表
│   ├── RpcBridge.cs              — RPC 分发函数指针桥
│   ├── ErrorBridge.cs            — TLS 错误回传（注入 C++ delegate* unmanaged）
│   ├── GCHandleHelper.cs         — ObjectVTableOut 提供的 7 个 [UnmanagedCallersOnly]
│   ├── AtlasSynchronizationContext.cs — 主线程 await 续体队列
│   ├── ThreadGuard.cs            — 主线程断言辅助
│   └── EntityRegistryBridge.cs   — EntityDefRegistry C++ 端注册桥
├── Log/Log.cs                    — Atlas.Log.{Trace,Debug,Info,Warning,Error,Critical}
│                                     级别值与 C++ LogLevel 枚举对齐 (0–5)
├── Time/GameTime.cs              — Atlas.Time.ServerTime / DeltaTime
├── Entity/
│   ├── ServerEntity.cs           — 抽象基类：EntityId / TypeName / Serialize /
│   │                                 Deserialize / SerializeForOwnerClient /
│   │                                 SerializeForOtherClients / OnInit / OnTick / OnDestroy /
│   │                                 SendClientRpc / SendCellRpc / SendBaseRpc
│   ├── EntityManager.cs          — 注册表 + 延迟 create/destroy；OnInitAll/OnTickAll/OnShutdownAll
│   ├── EntityFactory.cs          — 由 DefGenerator 贡献构造器
│   ├── ClientReplicationState.cs — 客户端同步状态
│   └── DeltaHistory.cs           — 属性 delta 历史
├── Hosting/
│   ├── ScriptHost.cs             — AssemblyLoadContext 封装
│   ├── ScriptLoadContext.cs      — isCollectible: true
│   └── HotReloadManager.cs       — SerializeAndUnload / LoadAndRestore 入口
└── Diagnostics/GCMonitor.cs      — DEBUG 下 60 秒周期上报 GC 统计
```

### 关键与文档设计的差异

1. **`Bootstrap` 拆分为 `Bootstrap` + `Lifecycle`**：原始设计把 CLR handshake 和引擎生命周期塞在同一个 `Bootstrap` 类里。实现时发现 handshake 必须先于任何 `[LibraryImport]` 或 `ErrorBridge` 生效，而引擎生命周期则依赖 `EngineContext` 已就绪。拆分后：
   - `Bootstrap.Initialize(BootstrapArgs*, ObjectVTableOut*)` 只负责错误桥注册 + vtable 填充。
   - `Lifecycle.EngineInit/OnTick/...` 负责引擎级生命周期。
   - 每个 `[UnmanagedCallersOnly]` 方法都保持 `try / catch + ErrorBridge.SetError / return -1` 模式。
2. **CLR 双 Assembly 实例问题**：`tests/csharp/Atlas.RuntimeTest` 额外提供 `CallbackEntryPoint.cs` + `LifecycleTestEntryPoint.cs` 作为转发 shim，避免 Bootstrap 被加载两次。详见 [implementation_notes.md](implementation_notes.md) §2。
3. **日志性能**：`Log.Send` 在 ≤ 340 字符时使用 `stackalloc byte[1024]` + `Encoding.UTF8.GetBytes(string, Span<byte>)` 消除分配；超过阈值退化为 `byte[]`。

## 测试覆盖

| 领域 | 测试 |
|------|------|
| `ClrScriptEngine` 生命周期 | `tests/unit/test_clr_script_engine.cpp` |
| C++ → C# 回调 + error bridge | `tests/unit/test_clr_callback.cpp` |
| `EntityManager` 生命周期 / 延迟销毁 | `tests/csharp/Atlas.Runtime.Tests/EntityManagerTests.cs` |
| 实体序列化往返 | `tests/csharp/Atlas.Runtime.Tests/EntitySerializationTests.cs` |
| 脏标记 / delta | `tests/csharp/Atlas.Runtime.Tests/DirtyTrackingTests.cs` |
| 工厂构造 | `tests/csharp/Atlas.Runtime.Tests/EntityFactoryTests.cs` |
| `ScriptHost` 加载/卸载 | `tests/csharp/Atlas.Runtime.Tests/ScriptHostTests.cs` |
| 二进制流对齐 | `tests/csharp/Atlas.Runtime.Tests/SpanWriterReaderTests.cs` |

## 剩余工作

- `atlas_module.cpp` 能力的最后一组清单（某些调试辅助函数、Python 侧的特定查询 API）继续依照 `docs/BIGWORLD_RPC_REFERENCE.md` 与 `docs/PROPERTY_SYNC_DESIGN.md` 的推进节奏补齐。
- `Atlas.Time.DeltaTime` 的高精度时间源（与 CellApp/BaseApp 主循环对齐）待 Phase 3 收尾时统一配置。
