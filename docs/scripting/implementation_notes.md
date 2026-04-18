# ScriptPhase 2: 实现笔记与经验教训

> 本文档记录 ScriptPhase 2 实现过程中发现的设计偏差、架构问题及解决方案。
> 目的是为后续阶段（3–6）提供参考，避免重复踩坑。

---

## 1. DLL TLS 隔离问题

### 问题描述

`atlas_engine.dll` 和使用它的 C++ 可执行文件（如测试 binary 或未来的服务端进程）
都会静态链接 `atlas_clrscript.lib`。Windows 上，`thread_local` 变量（如
`t_clr_error`）的存储是**按模块独立分配**的——同一线程上，DLL 内部的
`t_clr_error` 和可执行文件内部的 `t_clr_error` 是**两份独立内存**。

影响：

- C# 通过 `ErrorBridge.s_setError`（函数指针）调用 `clr_error_set()` 时，
  写入的是函数指针所在模块的 TLS 副本。
- C++ 调用 `HasClrError()` 时，读取的是当前模块（可执行文件）的 TLS 副本。
- 如果两个函数指针分属不同模块，写入和读取命中不同的 TLS → 错误永远读不到。

同理，`INativeApiProvider` 的全局指针 `g_provider`（`std::atomic<INativeApiProvider*>`）
也是按模块独立存储的。通过 DLL 导出的 `AtlasLogMessage()` 调用
`GetNativeApiProvider()` 读取的是 DLL 内的 `g_provider`；而可执行文件直接调用
`SetNativeApiProvider()` 设置的是可执行文件内的 `g_provider`。

### 解决方案

在 `atlas_engine.dll` 中导出以下函数，确保调用方能操作 DLL 内部的全局状态：

**Provider 注册（全局状态）：**

```cpp
// clr_native_api.hpp
ATLAS_NATIVE_API void AtlasSetNativeApiProvider(void* provider);
```

**Error bridge 函数指针获取（TLS 状态）：**

```cpp
ATLAS_NATIVE_API void* AtlasGetClrErrorSetFn();
ATLAS_NATIVE_API void* AtlasGetClrErrorClearFn();
ATLAS_NATIVE_API void* AtlasGetClrErrorGetCodeFn();
```

**Error bridge 查询（DLL-side TLS）：**

```cpp
ATLAS_NATIVE_API int32_t AtlasHasClrError();
ATLAS_NATIVE_API int32_t AtlasReadClrError(char* buf, int32_t buf_len);
ATLAS_NATIVE_API void    AtlasClearClrError();
```

使用方式：

1. 通过 `AtlasGetClrErrorSetFn()` 获取 DLL 内的 `clr_error_set` 地址
2. 将该地址放入 `ClrBootstrapArgs::error_set` 传给 C# `Bootstrap.Initialize()`
3. C# 异常通过 `ErrorBridge.SetError()` 调用该函数指针 → 写入 DLL 的 TLS
4. C++ 通过 `AtlasHasClrError()` / `AtlasReadClrError()` 查询 DLL 的 TLS

**对生产环境的影响**：在正式的服务端进程中，如果进程通过 import library 链接
`atlas_engine.dll`（而非同时静态链接 `atlas_clrscript`），则不存在 TLS 重复问题。
但测试环境和混合链接场景必须使用上述导出函数。

---

## 2. CLR 双 Assembly 实例问题

### 问题描述

.NET 的 `hostfxr` API `load_assembly_and_get_function_pointer()` 按路径加载托管程序集。
如果 C++ 分别通过两个路径加载了 **同一个 Assembly 的两份文件**（例如
`Atlas.Runtime.dll` 一次从 `src/` 目录直接加载，一次作为 `Atlas.RuntimeTest.dll`
的 ProjectReference 依赖被 CLR 自动加载），CLR 可能创建**两个独立的 Assembly
实例**，各自拥有独立的静态字段。

影响：`Bootstrap.Initialize()` 设置了第一个实例的 `ErrorBridge.s_setError`，
但 `CallbackEntryPoint.ThrowException()` 运行在第二个实例中，看到的
`ErrorBridge.s_setError` 仍为 null。

### 解决方案

在测试程序集（`Atlas.RuntimeTest`）中添加 `RunBootstrap` 转发方法：

```csharp
[UnmanagedCallersOnly]
public static unsafe int RunBootstrap(IntPtr argsPtr, IntPtr vtableOutPtr)
{
    var args = (BootstrapArgs*)argsPtr;
    var vtableOut = (ObjectVTableOut*)vtableOutPtr;
    // 内联 Bootstrap.Initialize 逻辑（因为 [UnmanagedCallersOnly] 方法不能
    // 被其他 C# 代码直接调用）
    ErrorBridge.RegisterNativeFunctions(args->ErrorSet, ...);
    vtableOut->FreeHandle = &GCHandleHelper.FreeHandle;
    ...
    return 0;
}
```

C++ 测试通过 `Atlas.RuntimeTest.dll` 的 `RunBootstrap` 启动 bootstrap，
而不是直接用 `clr_bootstrap(host, runtime_dll)` 单独加载 `Atlas.Runtime.dll`。
这样 `Atlas.Runtime.dll` 只作为 `Atlas.RuntimeTest.dll` 的依赖被加载一次，
所有代码共享同一份 `ErrorBridge` 静态字段。

**对生产环境的影响**：在正式的服务端进程中，`clr_bootstrap()` 加载
`Atlas.Runtime.dll` 作为唯一入口，不存在此问题。此问题仅影响测试环境中
同时存在多个 C# 程序集且都引用 `Atlas.Runtime` 的场景。

---

## 3. Interop Source Generator 的尝试与移除

### 背景

曾实现了 `Atlas.Generators.Interop`（IIncrementalGenerator），用 `[NativeImport]`
属性自动生成 `[LibraryImport]` / `[DllImport]` 声明和 Span wrapper 代码。

### 遇到的问题

1. **Roslyn 链式限制**: Source Generator 的输出不会被其他 Source Generator 处理。
   `[LibraryImport]` 本身就是 Source Generator，我们生成的 `[LibraryImport] partial`
   方法不被 `LibraryImportGenerator` 扫描 → CS8795。改用 `[DllImport] extern`
   绕过了此问题，但丢失了 `[LibraryImport]` 的编译期 marshalling 优势。

2. **Custom Marshaller 限制**: .NET 7+ 的 `[MarshalUsing]` 是一对一参数映射，
   无法将 `ReadOnlySpan<byte>` 展开为 `(byte*, int)` 两个原生参数。C++ 函数签名
   是分离式的 `(const char* msg, int32_t len)`，Custom Marshaller 无法适配。

3. **收益 vs 成本**: 当前 `atlas_*` 导出函数 < 15 个。Generator 引入了
   netstandard2.0 项目、EquatableArray、诊断系统等大量基础设施，维护成本远超手写。

### 最终决策

**移除 `Atlas.Generators.Interop`**，保留手写的 `NativeApi.cs`。

`NativeApi.cs` 已经使用 `[LibraryImport]`（.NET 官方 source-generated P/Invoke），
对 Span 参数用手写 `fixed` wrapper 处理 → `byte* + int`。这是官方推荐的标准模式，
IL2CPP / Unity 2023.1+ 完全兼容，代码清晰且无额外依赖。

### 经验教训

- Source Generator 适合**大量重复模式**（Entity 序列化、RPC 分发等），
  不适合 < 20 个函数的简单声明性任务。
- 自定义 Generator 的 Roslyn 链式限制是**设计时必须验证**的约束，
  不能假设生成的代码会被其他 Generator 处理。
- `[LibraryImport]` + 手写 Span wrapper 是 P/Invoke 的最佳实践，
  没有比它更简洁的官方方案。

---

## 4. ClrObjectVTable 模式（提前到 Phase 2）

### 原始设计

Phase 2 文档（Task 2.2）计划用"简单的 trampoline"实现 `ClrObject` 的方法调用
（`get_attr`、`call` 等），等 ScriptPhase 4 的 Source Generator 生成辅助方法后再优化。

### 实际实现

直接在 Phase 2 引入了 `ClrObjectVTable` 结构体——7 个函数指针在 CLR bootstrap
时一次性注册，所有 `ClrObject` 实例共享同一份 vtable：

```cpp
struct ClrObjectVTable
{
    void     (*free_handle)(void*);
    int32_t  (*get_type_name)(void*, char*, int32_t);
    uint8_t  (*is_none)(void*);
    int32_t  (*to_int64)(void*, int64_t*);
    int32_t  (*to_double)(void*, double*);
    int32_t  (*to_string)(void*, char*, int32_t);
    int32_t  (*to_bool)(void*, uint8_t*);
};
```

**为什么提前实现**：

- 每次 `ClrObject` 方法调用都需要跨 boundary，如果每次都通过 `ClrHost::get_method`
  查找函数指针，开销巨大。
- vtable 模式是 O(1) 的间接调用，完全在 C++ 侧完成指针解引用。
- 不需要等待 ScriptPhase 4 的 Source Generator——vtable 填充在 C# `Bootstrap.Initialize`
  中完成（通过 `&GCHandleHelper.FreeHandle` 等地址运算符获取函数指针）。

---

## 5. ATLAS_NATIVE_API_TABLE X-Macro 模式

### 设计决策

所有 `atlas_*` 导出函数的**单一定义源**在 `clr_native_api_defs.hpp`：

```cpp
#define ATLAS_NATIVE_API_TABLE(X)                        \
    X(void, log_message, (int32_t level, ...), ...)      \
    X(double, server_time, (), ...)                      \
    ...
```

四个消费点自动展开：

1. `clr_native_api.hpp` — 函数声明
2. `clr_native_api.cpp` — 函数实现（委托到 provider）
3. `native_api_provider.hpp` — INativeApiProvider 纯虚方法
4. `base_native_provider.hpp` — 默认实现声明

新增 API 只需编辑 `clr_native_api_defs.hpp` 一个文件。

**注意**：不在 table 中的特殊函数（`AtlasGetAbiVersion`、`AtlasSetNativeApiProvider`、
error bridge 函数）在 `clr_native_api.hpp/cpp` 中手动声明和实现。
