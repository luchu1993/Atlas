# 设计: NativeApi 架构基础设施

> 归属阶段: ScriptPhase 1 (CMake 共享库) + ScriptPhase 2 (Provider + 线程安全)
>
> **状态（2026-04-18）：✅ 三条方案均已落地**

---

## 1. 三个原始问题

| # | 问题 | 风险 |
|---|------|------|
| A | C# `[LibraryImport]` 需要从宿主进程找到 C++ 导出符号；可执行文件默认不导出 `atlas_*` 符号 | 所有 C# → C++ 调用运行时失败 |
| B | 分布式架构下不同进程类型（BaseApp / CellApp / DBApp）对同一 NativeApi 有不同行为 | RPC 路由错误、功能缺失、静默失败 |
| C | C# `async/await` 续体在线程池线程执行，调用非线程安全的 C++ 引擎 API | 随机崩溃、数据竞争 |

---

## 2. 方案 A：引擎核心构建为共享库

### 结论

所有 `src/lib/*` 子库以 `OBJECT` 目标编译，聚合为 `atlas_engine` `SHARED` 库。`atlas_*` 导出符号在其中自然可见；服务端可执行文件（BaseApp / CellApp / DBApp / Reviver）动态链接 `atlas_engine`。C# 端 `[LibraryImport("atlas_engine")]` 走标准 DLL 查找，无需 `DllImportResolver` hack。

### 实际落地

- CMake 目标：`build/debug/src/lib/clrscript/Debug/atlas_engine.dll`（以及后续 Release/Linux 构建的 `.so`）。
- 导出宏：`src/lib/clrscript/clr_export.h` 定义 `ATLAS_NATIVE_API = extern "C" + ATLAS_EXPORT`；`ATLAS_ENGINE_EXPORTS` 由 `atlas_engine` 目标内部设置。
- C# 侧：`src/csharp/Atlas.Runtime/Core/NativeApi.cs` 的 `private const string LibName = "atlas_engine";`。
- 部署结构（示意）：

  ```
  deploy/
  ├── BaseApp(.exe)            — 薄壳，动态链接 atlas_engine
  ├── CellApp(.exe)
  ├── DBApp(.exe)
  ├── Reviver(.exe)
  ├── atlas_engine.dll/.so     — 引擎核心，导出 atlas_* 符号
  ├── dotnet/                  — .NET 9 运行时
  ├── scripts/
  │   ├── Atlas.Runtime.dll
  │   ├── Atlas.Shared.dll
  │   └── Atlas.GameScripts.dll
  └── config/
      └── atlas_server.runtimeconfig.json
  ```

---

## 3. 方案 B：`INativeApiProvider` 进程级适配

### 结论

`atlas_*` 导出函数统一通过 `clr_native_api_defs.h` 的 X-macro 一次性展开：声明、实现、`INativeApiProvider` 纯虚接口、`BaseNativeProvider` 默认实现四处自动同步。每个服务端可执行文件在 `ClrHost::Initialize` 之前注册自己的 Provider 实现，`atlas_*` 函数体中一行 `GetNativeApiProvider().XXX(...)` 转发。

### 实际落地

- 抽象：`src/lib/clrscript/native_api_provider.h` 定义 `class INativeApiProvider`，方法采用 PascalCase：
  - `LogMessage(int32_t level, const char* msg, int32_t len)`
  - `ServerTime()` / `DeltaTime()`
  - `GetProcessPrefix()`
  - `SendClientRpc(uint32_t entity_id, uint32_t rpc_id, uint8_t target, const std::byte* payload, int32_t len)`
  - `SendCellRpc(...)` / `SendBaseRpc(...)`
  - `RegisterEntityType(const std::byte* data, int32_t len)` / `UnregisterAllEntityTypes()`
  - `WriteToDb(...)` / `GiveClientTo(...)`
  - `CreateBaseEntity(uint16_t type_id, uint32_t space_id)` — scripting-side `EntityFactory.CreateBase`. Witness attachment happens via the client-bind path (see `SetAoIRadius` below); creation no longer carries an AoI radius.
  - `SetAoIRadius(uint32_t entity_id, float radius, float hysteresis)` — mirrors BigWorld's `entity.setAoIRadius`. Forwards `cellapp::SetAoIRadius` to the cell hosting this entity; clamp/Ghost-reject/floor semantics live on the cell side (`Witness::SetAoIRadius` + `CellApp::OnSetAoIRadius`). Only valid on BaseApp; other providers log an error and no-op.
  - `SetNativeCallbacks(const void* native_callbacks, int32_t len)`
- 注册：同文件内 `void SetNativeApiProvider(INativeApiProvider*)` / `INativeApiProvider& GetNativeApiProvider()`。未注册时断言终止（编程错误，非可恢复失败）。
- 默认实现：`src/lib/clrscript/base_native_provider.{h,cc}` — 日志/时间等通用逻辑。
- 进程特化：`src/server/baseapp/baseapp_native_provider.{h,cc}` 提供 BaseApp 版本；CellApp / DBApp / Reviver 按需扩展。
- 导出函数：`src/lib/clrscript/clr_native_api.{h,cc}` 由 `clr_native_api_defs.h` 的 X-macro 展开。

### 进程差异化示意

| NativeApi | BaseApp | CellApp | DBApp | LoginApp |
|-----------|---------|---------|-------|----------|
| `SendClientRpc` | 持有 ClientProxy，直接发送 | 转发到对应 BaseApp | 不支持 | 不支持 |
| `SendCellRpc` | 查找目标 CellApp，跨进程路由 | 本地分发给 C# | 不支持 | 不支持 |
| `SendBaseRpc` | 本地分发给 C# | 查找目标 BaseApp，跨进程路由 | 不支持 | 不支持 |
| `GetProcessPrefix` | Base 前缀 | Cell 前缀 | 不分配 EntityId | 不分配 EntityId |
| `RegisterEntityType` | 注册（含全部信息） | 注册（含 Cell 相关） | 注册（含持久化字段） | 不需要 |

---

## 4. 方案 C：线程安全（`AtlasSynchronizationContext`）

### 结论

C# `async/await` 默认把续体调度到线程池线程，但 C++ 引擎 API（RPC 发送、实体操作等）只能在主 tick 线程执行。安装自定义 `SynchronizationContext`，把 `Post` 进队列，`OnTick` 的开头统一 drain 一次，使得 await 之后自然回到主线程。

### 实际落地

- `src/csharp/Atlas.Runtime/Core/AtlasSynchronizationContext.cs` 实现 `Post` / `Send` 将续体排队到 `ConcurrentQueue`，并提供 `ProcessQueue()` 方法。
- `src/csharp/Atlas.Runtime/Core/Lifecycle.cs::DoOnTick` 中每帧先 `EngineContext.SyncContext?.ProcessQueue()`，再执行 `EntityManager.Instance.OnTickAll(deltaTime)`。
- `src/csharp/Atlas.Runtime/Core/ThreadGuard.cs` 提供 `SetMainThread()` / `EnsureMainThread()` 断言辅助，`Lifecycle.DoEngineInit` 启动时登记主线程。
- `src/csharp/Atlas.Runtime/Core/Bootstrap.cs` 在 CLR handshake 阶段安装 Context 与 ThreadGuard。

### 使用约束

- NativeApi 中凡会修改引擎状态的方法，调用方必须已在主线程（可通过 `ThreadGuard.EnsureMainThread()` 断言）。
- `await` 前后若存在引擎调用，应让 await 回到主线程（默认行为，因为当前 `SynchronizationContext` 就是 `AtlasSynchronizationContext`）。
- 子线程（I/O、线程池）上若需执行引擎调用，用 `SyncContext.Post(...)` 转发。

---

## 5. 关联文档

- [script_phase1_dotnet_host.md](script_phase1_dotnet_host.md) — 共享库 + `ClrHost` 的落地细节。
- [script_phase2_interop_layer.md](script_phase2_interop_layer.md) — `atlas_*` 导出函数、`NativeApi.cs`、`ClrObjectVTable`、异常桥接。
- [implementation_notes.md](implementation_notes.md) — DLL TLS 隔离、CLR 双 Assembly 实例、X-macro 等实现中踩坑的具体处理。
