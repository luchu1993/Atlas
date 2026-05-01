# NativeApi 架构

> **状态**:✅ 已落地。`atlas_engine` 共享库、`INativeApiProvider`
> 抽象与默认实现、X-macro 单一定义源、主线程模型、TLS / 双 Assembly
> 解决方案均在 `main` 上工作并有测试覆盖;Provider 特化目前仅
> `BaseApp` / `CellApp`,其余进程使用默认实现(详见
> [README.md §7](README.md#7-完成状态))。

C# 脚本层与 C++ 引擎之间的双向调用、进程差异化与主线程语义建立在三件
基础设施之上。本文记录这三条决策的"是什么"与"为什么",以及踩过的两个
平台坑。

## 1. atlas_engine 共享库

`src/lib/*` 各子库以 `OBJECT` 目标编译,聚合为 `atlas_engine` `SHARED`
库;服务端可执行文件(BaseApp / CellApp / DBApp / Reviver)动态链接它。
C# 端 `[LibraryImport("atlas_engine")]` 走标准 DLL 查找,无需
`DllImportResolver` hack。

落地点:

- CMake 目标:`build/<preset>/src/lib/clrscript/Debug/atlas_engine.dll`(或
  Linux `.so`)。
- 导出宏:`src/lib/clrscript/clr_export.h` 定义
  `ATLAS_NATIVE_API = extern "C" + ATLAS_EXPORT`;`ATLAS_ENGINE_EXPORTS`
  由 `atlas_engine` 目标内部设置。
- C# 引用:`src/csharp/Atlas.Runtime/Core/NativeApi.cs` 中
  `private const string LibName = "atlas_engine";`。

## 2. INativeApiProvider 进程级适配

`atlas_*` 导出函数统一通过 `src/lib/clrscript/clr_native_api_defs.h` 的
X-macro 一次性展开:函数声明、函数实现、`INativeApiProvider` 纯虚方法、
`BaseNativeProvider` 默认实现四处自动同步。每个服务端可执行文件在
`ClrHost::Initialize` 之前注册自己的 Provider,`atlas_*` 函数体一行
`GetNativeApiProvider().XXX(...)` 转发。

抽象:`src/lib/clrscript/native_api_provider.h` 中 `class INativeApiProvider`
方法采用 PascalCase,涉及日志/时间/进程前缀/RPC/EntityDefRegistry/数据库
持久化/EntityCreation/AoI 等。

进程差异化示例:

| NativeApi | BaseApp | CellApp | DBApp | LoginApp |
|---|---|---|---|---|
| `SendClientRpc` | 直接通过 ClientProxy 发送 | 转发到对应 BaseApp | 不支持 | 不支持 |
| `SendCellRpc` | 查找目标 CellApp,跨进程路由 | 本地分发给 C# | 不支持 | 不支持 |
| `SendBaseRpc` | 本地分发给 C# | 查找目标 BaseApp,跨进程路由 | 不支持 | 不支持 |
| `GetProcessPrefix` | Base 前缀 | Cell 前缀 | 不分配 EntityId | 不分配 EntityId |
| `RegisterEntityType` | 注册全部信息 | 注册(含 Cell 相关) | 注册(含持久化字段) | 不需要 |

## 3. 主线程模型(AtlasSynchronizationContext)

C# `async/await` 默认把续体调度到线程池;C++ 引擎 API 只能在主 tick 线程
执行。`Atlas.Runtime` 安装自定义 `SynchronizationContext`,把 `Post` 进
`ConcurrentQueue`,`Lifecycle.DoOnTick` 在每帧开头统一 drain,使 await
之后自然回到主线程。

落地点:

- `src/csharp/Atlas.Runtime/Core/AtlasSynchronizationContext.cs` 实现 `Post`
  / `Send` / `ProcessQueue`。
- `src/csharp/Atlas.Runtime/Core/Lifecycle.cs::DoOnTick` 每帧先
  `EngineContext.SyncContext?.ProcessQueue()`,再 `EntityManager.Instance.OnTickAll`。
- `src/csharp/Atlas.Runtime/Core/ThreadGuard.cs` 提供 `SetMainThread()` /
  `EnsureMainThread()` 断言;`Lifecycle.DoEngineInit` 启动时登记主线程。
- `src/csharp/Atlas.ClrHost/Bootstrap.cs` 在握手阶段安装 Context 与 ThreadGuard。

约束:

- 修改引擎状态的 NativeApi 调用方必须已在主线程,可用
  `ThreadGuard.EnsureMainThread()` 断言。
- 子线程上需要触发引擎调用时,通过
  `EngineContext.SyncContext.Post(...)` 转发。

## 4. 平台约束:DLL TLS 隔离

Windows 上 `thread_local` 变量的存储**按模块独立分配**。当
`atlas_engine.dll` 与使用它的可执行文件都静态链接 `atlas_clrscript.lib`
(典型场景:测试 binary),同一线程上 DLL 内的 `t_clr_error` 与 EXE 内的
`t_clr_error` 是两份独立内存。同样的隔离也作用于
`INativeApiProvider* g_provider` 这类全局静态。

后果:C# 经函数指针调用 `clr_error_set()` 写入的是函数指针所属模块的
TLS;C++ 通过本模块的 `HasClrError()` 读取另一份,错误永远查不到。

解决:`atlas_engine.dll` 显式导出查询 / 设置入口,所有调用方都通过它们
操作 DLL 内部状态:

```cpp
ATLAS_NATIVE_API void  AtlasSetNativeApiProvider(void* provider);
ATLAS_NATIVE_API void* AtlasGetClrErrorSetFn();
ATLAS_NATIVE_API void* AtlasGetClrErrorClearFn();
ATLAS_NATIVE_API void* AtlasGetClrErrorGetCodeFn();
ATLAS_NATIVE_API int32_t AtlasHasClrError();
ATLAS_NATIVE_API int32_t AtlasReadClrError(char* buf, int32_t buf_len);
ATLAS_NATIVE_API void    AtlasClearClrError();
```

调用方将 `AtlasGetClrErrorSetFn()` 等返回的地址放入 `ClrBootstrapArgs`
传给 C# `Bootstrap.Initialize()`,`ErrorBridge.SetError` 调用该函数指针
即写入 DLL 的 TLS,C++ 通过 `AtlasHasClrError()` 查询同一份 TLS。生产
进程仅经 import library 链接 `atlas_engine` 时不存在重复 TLS,但混合
链接的测试场景必须使用上述导出函数。

## 5. 平台约束:CLR 双 Assembly 实例

`hostfxr` 的 `load_assembly_and_get_function_pointer` 按路径加载托管程序
集。若 C++ 通过两条不同路径加载了同一 Assembly 的两份文件(例如
`Atlas.Runtime.dll` 既被直接加载,又作为 `Atlas.RuntimeTest.dll` 的
ProjectReference 依赖被自动加载),CLR 可能创建两个独立的 Assembly
实例,各自拥有独立静态字段——`Bootstrap.Initialize()` 设置的
`ErrorBridge.s_setError` 不会被另一实例看到。

解决:测试程序集(`tests/csharp/Atlas.RuntimeTest/`)提供
`CallbackEntryPoint.cs` / `LifecycleTestEntryPoint.cs` 作为 forwarder。
C++ 测试通过 forwarder 启动 bootstrap,而非直接加载 `Atlas.Runtime.dll`,
确保后者只作为依赖被 CLR 加载一次。生产服务端进程的 `clr_bootstrap()`
路径只加载一份 `Atlas.Runtime.dll`,不存在该问题。

## 6. ClrObjectVTable 模式

C++ 调用 `ClrObject` 方法的开销若每次都走 `ClrHost::get_method` 查找
不可接受。Bootstrap 阶段 C# 把 `GCHandleHelper` 的 7 个
`[UnmanagedCallersOnly]` 方法地址(`free_handle / get_type_name /
is_none / to_int64 / to_double / to_string / to_bool`)填入
`ClrObjectVTable`,所有 `ClrObject` 实例共享同一份 vtable,方法调用降为
O(1) 间接跳转。

## 7. 关联文档

- [README.md](README.md) — 脚本层整体分层与 C# / C++ 入口对照表。
- [entity_mailbox_design.md](entity_mailbox_design.md) — Mailbox 路径上 NativeApi 的具体使用。
- [hot_reload.md](hot_reload.md) — `ClrObjectRegistry::ReleaseAll` 与重载边界的 vtable 重绑定。
