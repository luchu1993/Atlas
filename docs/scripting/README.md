# C# 脚本层架构

> **状态**:架构主线 ✅ 已落地 / 集成与验证 🚧 进行中(详见 §7)

Atlas 服务端的玩法脚本运行在嵌入式 .NET CoreCLR 上;`Atlas.Generators.Def`
在编译期生成所有反射敏感代码(序列化、脏标记、RPC 分发、Mailbox 代理、
实体工厂、类型注册),使同一套 `Atlas.Shared` 程序集可在 .NET 服务端
和 Unity IL2CPP 客户端共存。

## 1. 目录速览

| 文档 | 内容 |
|---|---|
| [native_api_architecture.md](native_api_architecture.md) | `atlas_engine` 共享库、`INativeApiProvider` 进程级适配、`AtlasSynchronizationContext` 主线程模型、DLL TLS 隔离与 CLR 双 Assembly 实例的踩坑约束 |
| [entity_mailbox_design.md](entity_mailbox_design.md) | BigWorld 风格 `entity.Client.Xxx()` 跨端 RPC 在 Source Generator 下的代码生成与分发 |
| [entity_type_registration.md](entity_type_registration.md) | `.def` → `Atlas.Generators.Def.TypeRegistryEmitter` → `EntityDefRegistry` 的元数据注册链路 |
| [serialization_alignment.md](serialization_alignment.md) | `SpanWriter` / `SpanReader` 与 C++ `BinaryStream` 的字节级兼容约定 |
| [hot_reload.md](hot_reload.md) | `AssemblyLoadContext` 隔离、状态快照格式、`ClrHotReload` + `HotReloadManager` 协作 |

延伸阅读:[`docs/generator/`](../generator/) 收录 `.def` 驱动的 Source Generator
设计;[`docs/property_sync/`](../property_sync/) 收录属性同步与 Component 设计。

## 2. 进程内分层

```
┌────────────────────────────── C# 用户脚本 (热重载) ────────────────────────────┐
│   Atlas.GameScripts.dll      —  实体逻辑、事件订阅、玩家 AI                    │
└──────────────────────────────────────┬─────────────────────────────────────────┘
                                       ▼
┌──────────────────────────── 引擎运行时 (Default ALC) ──────────────────────────┐
│   Atlas.Runtime.dll                                                             │
│   ├── Core/ Lifecycle / NativeApi / RpcBridge / SyncContext / ThreadGuard      │
│   ├── Entity/ ServerEntity / EntityManager / EntityFactory / DeltaHistory      │
│   ├── Hosting/ ScriptHost / ScriptLoadContext / HotReloadManager               │
│   ├── Log + Time + Diagnostics                                                  │
│   Atlas.ClrHost.dll  ←  Bootstrap / ErrorBridge / GCHandleHelper (CoreCLR 桥)   │
│   Atlas.Shared.dll   ←  DataTypes / SpanWriter / SpanReader / Mailbox / RpcId   │
│   Atlas.Generators.Def.dll  ←  编译期 Source Generator(只在 build 时使用)        │
└──────────────────────────────────────┬─────────────────────────────────────────┘
                                       ▼  delegate* unmanaged / [LibraryImport]
┌──────────────────────────── C++ 引擎 (atlas_engine.dll) ───────────────────────┐
│   src/lib/script/         语言无关 ScriptEngine / ScriptValue / ScriptObject    │
│   src/lib/clrscript/      ClrHost / ClrScriptEngine / NativeApi / HotReload     │
│   src/lib/entitydef/      EntityDefRegistry                                     │
└─────────────────────────────────────────────────────────────────────────────────┘
```

`Atlas.Shared` 目标 `netstandard2.1`,在 Unity (Mono / IL2CPP) 上原样使用;
`Atlas.ClrHost` / `Atlas.Runtime` 是 .NET 程序集,只在服务端宿主上加载。
客户端宿主分两层:可被 Unity 直接消费的 `Atlas.Client`(`netstandard2.1`,
仅依赖 `Atlas.Shared`)、桌面 .NET 宿主使用的 `Atlas.Client.Desktop`
(持有 `DesktopBootstrap` + `[LibraryImport("atlas_engine")]`)。

## 3. 关键设计原则

1. **零反射**。所有序列化、脏标记、RPC 分发、Mailbox 代理、实体工厂、
   类型注册由 `Atlas.Generators.Def` 在编译期生成 `partial class` 与
   `[ModuleInitializer]`;运行时不出现 `System.Reflection` /
   `Activator.CreateInstance` / `MethodInfo.Invoke`。

2. **单一引擎共享库**。所有 `src/lib/*` 目标聚合为 `atlas_engine.dll/.so`,
   C# 端 `[LibraryImport("atlas_engine")]` 走标准 DLL 查找,服务端可执行
   文件薄壳化,同时绕开 Windows 上"DLL 与 EXE TLS 副本独立"造成的错误桥
   隔离问题(细节见 native_api_architecture.md)。

3. **Provider 进程级特化**。`atlas_*` 导出函数在 C++ 侧统一委托给当前
   `INativeApiProvider`;BaseApp / CellApp / DBApp / Reviver 各自注册一份
   实现,使 `SendClientRpc` 等方法在不同进程上自动路由到合理的目标。

4. **主线程语义**。安装 `AtlasSynchronizationContext` 后,所有 `await` 续体
   默认回到 Tick 主线程;每帧 `Lifecycle.DoOnTick` 先 drain 队列再驱动实体
   `OnTick`,确保 NativeApi 调用永远在主线程触发。

5. **热重载只换用户脚本**。`Atlas.GameScripts.dll` 加载在可卸载的
   `ScriptLoadContext` 中,引擎运行时与共享库留在 Default ALC;序列化
   快照按实体逐条带 `payloadByteLen` 前缀,允许重载前后类型增删。

## 4. C# / C++ 入口对照

| 方向 | C# 入口 | C++ 入口 |
|---|---|---|
| 引擎握手 | `Atlas.Core.Bootstrap.Initialize(BootstrapArgs*, ObjectVTableOut*)`(in `Atlas.ClrHost`) | `clr_bootstrap()` → `ClrHost::GetMethodAs<>` |
| 引擎生命周期 | `Atlas.Core.Lifecycle.{EngineInit, EngineShutdown, OnInit, OnTick, OnShutdown}` | `ClrScriptEngine::{Initialize, OnInit, OnTick, OnShutdown, Finalize}` |
| C# → C++ | `Atlas.Core.NativeApi.Atlas*` `[LibraryImport]` | `ATLAS_NATIVE_API_TABLE` X-macro 展开的 `atlas_*` 函数 |
| C++ → C# | C++ 端缓存的 `[UnmanagedCallersOnly]` 函数指针 | `Atlas.Generators.Def` 生成的 RPC dispatcher / EntityFactory |
| 错误桥 | `Atlas.Core.ErrorBridge.SetError(ex)`(`Atlas.ClrHost`) | `clr_error_set / clear / get_code`(thread-local;DLL 内导出 `AtlasGetClrError*Fn` 提供函数指针) |
| GCHandle 桥 | `Atlas.Core.GCHandleHelper`(`Atlas.ClrHost`) 提供 7 个 `[UnmanagedCallersOnly]` | `ClrObjectVTable` 一次性注入,所有 `ClrObject` 共享 |
| 热重载 | `Atlas.Hosting.HotReloadManager.{SerializeAndUnload, LoadAndRestore}` | `ClrHotReload::Reload / ProcessPending` |

## 5. 测试矩阵速览

| 层 | 项目 / 文件 | 覆盖 |
|---|---|---|
| C++ 单测 | `tests/unit/test_clr_*` | `ClrHost / ClrMarshal / ClrObject / ClrInvoke / ClrCallback / ClrError / ClrScriptEngine / NativeApi*` 全套生命周期与导出符号校验 |
| C++ 单测 | `tests/unit/test_script_*`、`test_entity_def_registry_*` | 语言无关脚本接口与 `EntityDefRegistry` 注册/RPC/Component/Container |
| C# 单测 | `tests/csharp/Atlas.Runtime.Tests/` | 实体序列化、脏标记、工厂、ScriptHost 加载/卸载、SpanWriter/Reader、Component / 容器同步 |
| C# 单测 | `tests/csharp/Atlas.Generators.Tests/DefGeneratorTests.cs` | DefGenerator 各 Emitter 的快照与诊断 |
| C# 单测 | `tests/csharp/Atlas.Client.Tests/` | 客户端实体注册与回放 |
| 集成 | `tests/csharp/Atlas.RuntimeTest/`、`tests/csharp/Atlas.SmokeTest/` | 测试 forwarder + 冒烟入口,用于 C++ 集成测试加载完整 CLR |

## 7. 完成状态

> 核对于 `main` 分支,2026-05-01。✅ 表示主路径已落地并有测试覆盖,
> 🚧 表示已具备基础设施但尚未在生产路径上接通,⬜ 表示未开始。

### 已落地(✅)

| 模块 | 关键代码 |
|---|---|
| 语言无关脚本抽象 | `src/lib/script/` (`script_engine.h` / `script_value` / `script_object` / `script_events`) |
| `atlas_engine` 共享库 + 导出宏 | `src/lib/clrscript/clr_export.h`、CMake `OBJECT` 聚合目标 |
| CoreCLR 嵌入(Win + Linux) | `clr_host.{h,cc}` + `clr_host_windows.cc` + `clr_host_linux.cc` |
| CLR 握手 / 错误桥 / GCHandle 桥 | `Atlas.ClrHost`(`Bootstrap.cs` / `ErrorBridge.cs` / `GCHandleHelper.cs`) + `clr_bootstrap.{h,cc}` + `clr_error.{h,cc}` |
| Marshal / Object / Invoke / VTable | `clr_marshal.{h,cc}` / `clr_object.{h,cc}` + `clr_object_registry.{h,cc}` / `clr_invoke.h` |
| `ATLAS_NATIVE_API_TABLE` X-macro | `clr_native_api_defs.h` + `clr_native_api.{h,cc}` |
| `INativeApiProvider` + 默认实现 | `native_api_provider.{h,cc}` + `base_native_provider.{h,cc}` |
| BaseApp / CellApp Provider 特化 | `src/server/baseapp/baseapp_native_provider.*`、`src/server/cellapp/cellapp_native_provider.*` |
| `ClrScriptEngine` 全生命周期 | `clr_script_engine.{h,cc}` + `Atlas.Runtime/Core/Lifecycle.cs` |
| 主线程模型 | `AtlasSynchronizationContext.cs` + `ThreadGuard.cs` |
| Atlas.Runtime: Entity / Hosting / Log / Time / Diagnostics | `src/csharp/Atlas.Runtime/` |
| Atlas.Shared: SpanWriter/Reader / DataTypes / Mailbox / RpcId | `src/csharp/Atlas.Shared/` |
| `Atlas.Generators.Def` 全套 emitter | `Properties / Serialization / DeltaSync / Factory / Mailbox / RpcStub / Dispatcher / RpcId / TypeRegistry / Component / EntityComponentAccessor / Struct / StructRegistry` 等 16 个 |
| EntityDefRegistry | `src/lib/entitydef/`(C# 注册路径 + DBApp `RegisterFromBinaryFile` 已接通) |
| 客户端宿主分层 | `Atlas.Client`(`netstandard2.1`)+ `Atlas.Client.Desktop`(.NET)+ `Atlas.Client.Unity`(asmdef + IL2CPP defines) |
| 离线工具 | `Atlas.Tools.DefDump`(`.def` → `entity_defs.bin`) |
| 示例脚本 | `samples/base/`(Account / Avatar)、`samples/client/`(StressAvatar + Component) |
| 热重载基础设施 | `clr_hot_reload.{h,cc}` + `file_watcher.{h,cc}` + `Hosting/{ScriptHost, ScriptLoadContext, HotReloadManager}.cs` |
| 热重载接入 + 测试 | `ScriptApp::Init` 读 `ServerConfig.enable_hot_reload` 等字段构造 `ClrHotReload`,`OnTickComplete` 驱动 `Poll/ProcessPending`;集成测试 `tests/integration/test_hot_reload.cpp` |
| C++ 测试(脚本相关 ~12 个文件) | `tests/unit/test_clr_*` + `test_script_*` + `test_native_api_*` + `test_entity_def_registry_*` |
| C# 测试 | `Atlas.Runtime.Tests` 18 个文件、`Atlas.Generators.Tests` 5 个文件、`Atlas.Client.Tests` |
| 集成 forwarder / 冒烟 | `Atlas.RuntimeTest` + `Atlas.SmokeTest` |

### 进行中(🚧)

| 项 | 现状 / 缺什么 |
|---|---|
| 其余进程的 Provider | 仅 `BaseApp` / `CellApp` 已实现;`DBApp` / `Reviver` / `DBAppMgr` 当前依赖 `BaseNativeProvider` 默认实现,差异化能力(如 `WriteToDb` 真正落库)尚未接通 |
| 端到端引擎生命周期测试 | 缺 `tests/integration/test_engine_lifecycle.*`(C++ 启动 → C# 初始化 → Tick N 帧 → 关闭) |
| Unity IL2CPP 端到端验收 | `Atlas.Client.Unity.asmdef` 已就位,但 `link.xml` + iOS/Android IL2CPP build + 序列化/RPC 往返尚未验证 |

### 未开始(⬜)

| 项 | 备注 |
|---|---|
| 10K 实体 1 小时稳定性压测 | 监控 RSS / GC 暂停,验收无泄漏 |
| `BenchmarkDotNet` 基准 | 调用延迟、序列化、`EntityFactory`、`RpcDispatch`、10K Tick |
| GC 暂停 p99 < 5 ms 验证 | `runtime/atlas_server.runtimeconfig.json` 已启用 Server GC + DATAS,缺持续负载下的 p99 数据 |
| `delegate* unmanaged` < 100 ns / string < 500 ns 延迟基准 | 同上,需要对应测试台 |

## 8. 部署形态

```
deploy/
├── BaseApp / CellApp / DBApp / Reviver        — 薄壳可执行,动态链接 atlas_engine
├── atlas_engine.dll/.so                        — 引擎核心,导出 atlas_* 符号
├── dotnet/                                     — .NET 运行时(随构建打包)
├── scripts/
│   ├── Atlas.Runtime.dll
│   ├── Atlas.Shared.dll
│   ├── Atlas.ClrHost.dll
│   └── Atlas.GameScripts.dll                   — 用户脚本,可热重载
└── runtime/atlas_server.runtimeconfig.json     — Server GC + DATAS + 分层编译
```
