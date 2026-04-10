# ScriptPhase 2: C++ ↔ C# 互操作层

> 预估周期: 4–5 周 | 前置依赖: ScriptPhase 1 完成 + [NativeApi 架构基础设施](native_api_architecture.md) 中的方案 A (共享库) 和方案 B (Provider) 就位

---

## 目标

1. 建立高效的双向互操作层: C++ 可调用 C# 方法，C# 可调用 C++ 函数
2. 实现类型编组（Marshal）— blittable 类型零拷贝，字符串/字节数组高效传递
3. 实现托管对象句柄管理（GCHandle）
4. 开发 `Atlas.Generators.Interop` Source Generator 自动生成绑定代码
5. 建立异常桥接机制

## 验收标准 (M2)

- [x] C++ 可调用 C# 静态方法并获取正确返回值 — `ClrStaticMethod<Ret, Args...>` + `test_clr_invoke`
- [x] C# 可通过 `[LibraryImport]` 调用 C++ 导出函数 — `NativeApi.cs` + `test_clr_callback`
- [x] 支持 blittable 类型、字符串、byte[] 的双向传递 — `clr_marshal.hpp` + `test_clr_marshal`
- [x] `Atlas.Generators.Interop` 能从标记属性自动生成绑定代码 — `InteropGenerator` (IIncrementalGenerator)
- [x] C# 异常能被 C++ 侧捕获并转换为 `Error` — `ErrorBridge` + `ClrErrorBuffer` TLS + `test_clr_error`
- [x] GCHandle 生命周期管理正确，无泄漏 — `ClrObjectVTable` + `GCHandleTracker` + `test_clr_object`

> **实现笔记**: 实现过程中发现了 DLL TLS 隔离、CLR 双 Assembly 实例、Source Generator
> 链式处理限制等关键问题，详见 [implementation_notes.md](implementation_notes.md)。

---

## 任务 2.1: 类型编组（Marshal）层

### 新建文件: `src/lib/clrscript/clr_marshal.hpp`, `clr_marshal.cpp`

### 编组策略总表

| C++ 类型 | C# 类型 | 策略 | 拷贝 |
|----------|---------|------|------|
| `bool` | `byte` (1 byte) | blittable, 传 0/1 | 零拷贝 |
| `int32_t` | `int` | blittable | 零拷贝 |
| `int64_t` | `long` | blittable | 零拷贝 |
| `float` | `float` | blittable | 零拷贝 |
| `double` | `double` | blittable | 零拷贝 |
| `Vector3` (3 float) | `Vector3` (blittable struct) | blittable | 零拷贝 |
| `Quaternion` (4 float) | `Quaternion` (blittable struct) | blittable | 零拷贝 |
| `std::string_view` → C# | `byte* data` + `int len` | 指针+长度 | 零拷贝 (pinned) |
| C# `string` → C++ | 回调写入 C++ buffer | 避免 managed alloc | 一次拷贝 |
| `std::vector<byte>` → C# | `byte* data` + `int len` | 指针+长度 | 零拷贝 |
| C# `byte[]` → C++ | `byte* data` + `int len` (pinned) | 指针+长度 | 零拷贝 |
| C++ 对象指针 | `IntPtr` | 不透明句柄 | N/A |
| `ScriptValue` | 变体类型 | 按 `Type` 枚举分发 | 视子类型 |

### 传输结构体定义

```cpp
// C++ 侧 — blittable 传输结构
namespace atlas
{

struct ClrStringRef
{
    const char* data;    // UTF-8 指针
    int32_t length;      // 字节长度（不含 null）
};

struct ClrSpanRef
{
    const std::byte* data;
    int32_t length;
};

struct ClrVector3
{
    float x, y, z;
};

struct ClrQuaternion
{
    float x, y, z, w;
};

} // namespace atlas
```

```csharp
// C# 侧 — 必须与 C++ 布局完全一致
[StructLayout(LayoutKind.Sequential)]
internal readonly struct NativeStringRef
{
    public readonly byte* Data;
    public readonly int Length;

    public ReadOnlySpan<byte> AsSpan() => new(Data, Length);
    public string AsString() => Encoding.UTF8.GetString(Data, Length);
}

[StructLayout(LayoutKind.Sequential)]
public struct Vector3
{
    public float X, Y, Z;
}
```

### 工作点

- [ ] 定义所有 blittable 传输结构体（C++ 和 C# 两侧）
- [ ] 实现 `clr_marshal::to_clr_string(string_view)` → `ClrStringRef`
- [ ] 实现 `clr_marshal::from_clr_string(char*, int)` → `std::string`
- [ ] 实现 `ScriptValue` ↔ CLR 变体的双向转换
- [ ] 编写 `static_assert` 验证结构体大小和对齐与 C# 一致
- [ ] 验证 C++ `bool` (1 byte) 与 C# `byte` 的传递一致性（不用 C# `bool`，因为它在 marshal 中是 4 bytes）

---

## 任务 2.2: 托管对象句柄管理

### 新建文件: `src/lib/clrscript/clr_object.hpp`, `clr_object.cpp`

**说明**: `ClrObject` 是 `ScriptObject` 的 .NET 实现。C++ 侧通过 `GCHandle`（一个不透明句柄）引用 C# 托管对象。

```cpp
class ClrObject final : public ScriptObject
{
public:
    explicit ClrObject(void* gc_handle);
    ~ClrObject() override;

    // 禁止拷贝（GCHandle 是唯一所有权）
    ClrObject(const ClrObject&) = delete;
    ClrObject& operator=(const ClrObject&) = delete;
    ClrObject(ClrObject&& other) noexcept;
    ClrObject& operator=(ClrObject&& other) noexcept;

    // ScriptObject 接口实现
    [[nodiscard]] auto is_none() const -> bool override;
    [[nodiscard]] auto type_name() const -> std::string override;
    [[nodiscard]] auto is_callable() const -> bool override;
    [[nodiscard]] auto get_attr(std::string_view name)
        -> std::unique_ptr<ScriptObject> override;
    [[nodiscard]] auto set_attr(std::string_view name,
                                const ScriptValue& value) -> Result<void> override;
    [[nodiscard]] auto call(std::span<const ScriptValue> args = {})
        -> Result<ScriptValue> override;
    [[nodiscard]] auto as_int() const -> Result<int64_t> override;
    [[nodiscard]] auto as_double() const -> Result<double> override;
    [[nodiscard]] auto as_string() const -> Result<std::string> override;
    [[nodiscard]] auto as_bool() const -> Result<bool> override;
    [[nodiscard]] auto as_bytes() const -> Result<std::vector<std::byte>> override;
    [[nodiscard]] auto to_debug_string() const -> std::string override;

    [[nodiscard]] auto gc_handle() const -> void* { return gc_handle_; }

private:
    void* gc_handle_{nullptr};
};
```

### GCHandle 辅助函数（C# 侧）

```csharp
// C# 侧提供给 C++ 调用的 GCHandle 管理方法
public static class GCHandleHelper
{
    [UnmanagedCallersOnly]
    public static IntPtr Alloc(IntPtr objHandle)
    {
        var obj = GCHandle.FromIntPtr(objHandle).Target;
        var handle = GCHandle.Alloc(obj);
        return GCHandle.ToIntPtr(handle);
    }

    [UnmanagedCallersOnly]
    public static void Free(IntPtr handlePtr)
    {
        var handle = GCHandle.FromIntPtr(handlePtr);
        handle.Free();
    }

    [UnmanagedCallersOnly]
    public static int GetTypeName(IntPtr handlePtr, byte* buffer, int bufferLen)
    {
        var handle = GCHandle.FromIntPtr(handlePtr);
        var typeName = handle.Target?.GetType().Name ?? "null";
        return Encoding.UTF8.GetBytes(typeName, new Span<byte>(buffer, bufferLen));
    }
}
```

### 工作点

- [x] `ClrObject(gc_handle)`: 存储句柄
- [x] `~ClrObject()`: 通过 `ClrObjectVTable::free_handle` 调用 C# `GCHandleHelper.FreeHandle()` 释放
- [x] `get_attr()` / `set_attr()` / `call()`: Phase 2 返回 Error/nullptr 占位；Phase 4 Source Generator 将生成完整实现
- [x] **VTable 模式**: 7 个函数指针（`free_handle`/`get_type_name`/`is_none`/`to_int64`/`to_double`/`to_string`/`to_bool`）在 `ClrBootstrap` 时由 C# `GCHandleHelper` 的 `[UnmanagedCallersOnly]` 方法地址填充，所有 `ClrObject` 共享同一份 vtable。无需等 ScriptPhase 4，无需反射。
- [x] move 语义: 转移句柄所有权，source 置 null
- [x] Debug 模式: `GCHandleTracker` — `std::atomic<int64_t>` alloc/free 计数器，可查 `leak_count()`

### GCHandle 泄漏检测

```cpp
#if ATLAS_DEBUG
class GCHandleTracker
{
public:
    static void on_alloc() { ++alloc_count_; }
    static void on_free() { ++free_count_; }
    static auto leak_count() -> int64_t
    {
        return alloc_count_.load() - free_count_.load();
    }
private:
    static std::atomic<int64_t> alloc_count_;
    static std::atomic<int64_t> free_count_;
};
#endif
```

---

## 任务 2.3: C++ → C# 调用通道

### 新建文件: `src/lib/clrscript/clr_invoke.hpp`, `clr_invoke.cpp`

**说明**: 类型安全的 C# 静态方法调用包装。所有目标方法必须标记 `[UnmanagedCallersOnly]`。

```cpp
// 编译期类型安全的函数指针包装
// 注意: bind() 需要 ClrHost 实例引用来解析函数指针
template <typename Ret, typename... Args>
class ClrStaticMethod
{
public:
    [[nodiscard]] auto bind(
        ClrHost& host,
        const std::filesystem::path& assembly_path,
        std::string_view type_name,
        std::string_view method_name) -> Result<void>
    {
        auto result = host.get_method(
            assembly_path, type_name, method_name);
        if (!result)
            return result.error();
        fn_ = reinterpret_cast<FnPtr>(*result);
        return Result<void>{};
    }

    [[nodiscard]] auto invoke(Args... args) const -> Result<Ret>
    {
        if (!fn_)
            return Error{ErrorCode::ScriptError, "Method not bound"};
        if constexpr (std::is_void_v<Ret>)
        {
            fn_(args...);
            // void 返回值的 C# 方法不通过返回值报错，
            // 调用方需视情况检查 has_clr_error()
            return Result<void>{};
        }
        else
        {
            auto result = fn_(args...);
            // 约定: C# [UnmanagedCallersOnly] 方法返回 int，
            // 0 = 成功, 非 0 = 失败 → 从 TLS 读取异常信息
            if constexpr (std::is_same_v<Ret, int>)
            {
                if (result != 0 && has_clr_error())
                    return read_clr_error();
            }
            return result;
        }
    }

    [[nodiscard]] auto is_bound() const -> bool { return fn_ != nullptr; }

    void reset() { fn_ = nullptr; }

private:
    using FnPtr = Ret(*)(Args...);
    FnPtr fn_{nullptr};
};
```

### 工作点

- [ ] 模板类确保编译期签名匹配
- [ ] `bind()` 接收 `ClrHost&` 引用，调用实例方法 `ClrHost::get_method()` 并缓存函数指针
- [ ] `invoke()` 直接通过函数指针调用，无查找开销
- [ ] `reset()` 清空函数指针（热重载时调用）
- [ ] 支持 `void` 返回值特化

---

## 任务 2.4: C# → C++ 回调通道（LibraryImport 导出函数）

### 新建文件: `src/lib/clrscript/clr_native_api.hpp`, `clr_native_api.cpp`

**设计思路**: C++ 以 `extern "C"` 导出函数，C# 通过 `[LibraryImport("atlas_engine")]` 直接调用。引擎核心构建为共享库 `atlas_engine.dll/.so`（详见 [native_api_architecture.md](native_api_architecture.md) 方案 A），C# P/Invoke 自然从同目录加载。所有 `atlas_*` 导出函数内部委托给 `INativeApiProvider`（方案 B），各进程类型注册不同实现。

### DLL 名称解析

引擎构建为共享库后，C# 标准 P/Invoke 即可找到 `atlas_engine.dll/.so`，
**不再需要** `DllImportResolver` hack（详见 [native_api_architecture.md](native_api_architecture.md) §2.5）。

### C++ 导出宏

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

// 所有导出函数使用 C 链接 + ATLAS_EXPORT
#define ATLAS_NATIVE_API extern "C" ATLAS_EXPORT
```

### C++ 侧导出函数声明

```cpp
// src/lib/clrscript/clr_native_api.hpp
#pragma once

#include "clr_export.hpp"

#include <cstdint>
#include <cstddef>

// ============================================================================
// C# → C++ 导出函数
// ============================================================================
//
// 命名规范: atlas_ 前缀 + snake_case，避免全局符号冲突。
// 所有函数由 C# [LibraryImport("atlas_engine")] 调用。
// C++ → C# 方向使用 ClrStaticMethod + [UnmanagedCallersOnly]，不在此处。

// ---- 日志 ----
ATLAS_NATIVE_API void atlas_log_message(
    int32_t level, const char* msg, int32_t len);

// ---- 时间 ----
ATLAS_NATIVE_API double atlas_server_time();
ATLAS_NATIVE_API float  atlas_delta_time();

// ---- 进程前缀（EntityId 分配） ----
ATLAS_NATIVE_API uint8_t atlas_get_process_prefix();

// ---- RPC 发送 ----
ATLAS_NATIVE_API void atlas_send_client_rpc(
    uint32_t entity_id, uint32_t rpc_id,
    uint8_t target, const uint8_t* payload, int32_t len);

ATLAS_NATIVE_API void atlas_send_cell_rpc(
    uint32_t entity_id, uint32_t rpc_id,
    const uint8_t* payload, int32_t len);

ATLAS_NATIVE_API void atlas_send_base_rpc(
    uint32_t entity_id, uint32_t rpc_id,
    const uint8_t* payload, int32_t len);

// ---- 实体类型注册 ----
ATLAS_NATIVE_API void atlas_register_entity_type(
    const uint8_t* data, int32_t len);

ATLAS_NATIVE_API void atlas_unregister_all_entity_types();
```

### C++ 侧导出函数实现

> **注意**: 以下为简化的占位实现，展示各函数的基本语义。
> 实际实现中，所有 `atlas_*` 函数应为一行委托到 `get_native_api_provider()`，
> 详见 [native_api_architecture.md](native_api_architecture.md) §3.3。

```cpp
// src/lib/clrscript/clr_native_api.cpp
#include "clrscript/clr_native_api.hpp"
#include "foundation/log.hpp"

// ---- 日志 ----
ATLAS_NATIVE_API void atlas_log_message(
    int32_t level, const char* msg, int32_t len)
{
    std::string_view message(msg, static_cast<size_t>(len));
    // level 值对应 LogLevel 枚举: Info=2, Warning=3, Error=4, Critical=5
    switch (level)
    {
        case 2: ATLAS_LOG_INFO("{}", message); break;
        case 3: ATLAS_LOG_WARNING("{}", message); break;
        case 4: ATLAS_LOG_ERROR("{}", message); break;
        case 5: ATLAS_LOG_CRITICAL("{}", message); break;
        default: ATLAS_LOG_DEBUG("{}", message); break;
    }
}

// ---- 时间 ----
ATLAS_NATIVE_API double atlas_server_time()
{
    // TODO: 调用 Clock::now() 返回 epoch 秒数
    return 0.0;
}

ATLAS_NATIVE_API float atlas_delta_time()
{
    // TODO: 返回上一帧 dt（从引擎主循环中更新）
    return 0.0f;
}

// ---- 进程前缀 ----
ATLAS_NATIVE_API uint8_t atlas_get_process_prefix()
{
    // TODO: 从引擎配置获取当前进程的唯一前缀
    return 0;
}

// ---- RPC 发送 ----
// TODO: 实现对接引擎网络层，序列化 RPC 并发送

// ---- 实体类型注册 ----
// TODO: 实现对接 EntityDefRegistry
```

### C# 侧 LibraryImport 声明（设计概览）

> **注意**: 此处为 API 设计概览，展示 LibraryImport 模式和签名。
> 最终实现（含 private `*Native` + public safe wrapper 模式）参见
> [script_phase3_engine_bindings.md](script_phase3_engine_bindings.md) 任务 3.1 NativeApi。
> ScriptPhase 4 的 `Atlas.Generators.Interop` 可进一步替代手写声明。

```csharp
using System.Runtime.InteropServices;

namespace Atlas.Core;

/// <summary>
/// C# → C++ 原生函数调用层。
/// 通过 [LibraryImport] source-generated P/Invoke 直接调用 C++ 导出函数。
/// "atlas_engine" 为引擎共享库名称（详见 native_api_architecture.md 方案 A）。
/// </summary>
internal static unsafe partial class NativeApi
{
    private const string LibName = "atlas_engine";

    // ---- 日志 ----
    [LibraryImport(LibName, EntryPoint = "atlas_log_message")]
    public static partial void LogMessage(int level, byte* msg, int len);

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
    public static partial void SendClientRpc(
        uint entityId, uint rpcId, byte target,
        byte* payload, int payloadLen);

    [LibraryImport(LibName, EntryPoint = "atlas_send_cell_rpc")]
    public static partial void SendCellRpc(
        uint entityId, uint rpcId,
        byte* payload, int payloadLen);

    [LibraryImport(LibName, EntryPoint = "atlas_send_base_rpc")]
    public static partial void SendBaseRpc(
        uint entityId, uint rpcId,
        byte* payload, int payloadLen);

    // ---- 实体类型注册 ----
    [LibraryImport(LibName, EntryPoint = "atlas_register_entity_type")]
    public static partial void RegisterEntityType(byte* data, int len);

    [LibraryImport(LibName, EntryPoint = "atlas_unregister_all_entity_types")]
    public static partial void UnregisterAllEntityTypes();

    // ---- 高层 wrapper（处理 Span → pointer 转换）----

    public static void LogMessage(int level, ReadOnlySpan<byte> message)
    {
        fixed (byte* ptr = message)
            LogMessage(level, ptr, message.Length);
    }

    public static void SendClientRpc(
        uint entityId, uint rpcId, byte target, ReadOnlySpan<byte> payload)
    {
        fixed (byte* ptr = payload)
            SendClientRpc(entityId, rpcId, target, ptr, payload.Length);
    }

    public static void SendCellRpc(
        uint entityId, uint rpcId, ReadOnlySpan<byte> payload)
    {
        fixed (byte* ptr = payload)
            SendCellRpc(entityId, rpcId, ptr, payload.Length);
    }

    public static void SendBaseRpc(
        uint entityId, uint rpcId, ReadOnlySpan<byte> payload)
    {
        fixed (byte* ptr = payload)
            SendBaseRpc(entityId, rpcId, ptr, payload.Length);
    }

    public static void RegisterEntityType(ReadOnlySpan<byte> data)
    {
        fixed (byte* ptr = data)
            RegisterEntityType(ptr, data.Length);
    }
}
```

### 工作点

- [x] 定义 `ATLAS_EXPORT` / `ATLAS_NATIVE_API` 宏 — `clr_export.hpp`
- [x] 实现 `INativeApiProvider` 接口 + `BaseNativeProvider` 基类 — `native_api_provider.hpp/cpp`, `base_native_provider.hpp/cpp`
- [x] C++ 所有 `atlas_*` 导出函数通过 `ATLAS_NATIVE_API_TABLE` X-Macro 一行委托到 `get_native_api_provider()` — `clr_native_api_defs.hpp` 为**单一定义源**
- [x] C# `NativeApi` 使用 `[LibraryImport("atlas_engine")]` 声明所有 P/Invoke 方法 — `NativeApi.cs`
- [x] C# 高层 wrapper 方法处理 `ReadOnlySpan<byte>` → `byte*` + `int` 转换（`fixed` 语句）
- [ ] ~~C# 高层 wrapper 方法添加 `ThreadGuard.EnsureMainThread()` 调用~~ — **推迟至 ScriptPhase 3**，线程安全由 `AtlasSynchronizationContext` 统一处理（参见架构文档 §4）
- [x] `free_gc_handle` 通过 `ClrObjectVTable::free_handle` 函数指针调用 C# `GCHandleHelper.FreeHandle()`（不再使用 `ClrStaticMethod` 缓存）
- [x] **扩展规则**: 新增函数只需编辑 `clr_native_api_defs.hpp`，四个消费文件自动展开（`clr_native_api.hpp/cpp`, `native_api_provider.hpp`, `base_native_provider.hpp/cpp`）。C# 侧对应更新 `NativeApi.cs`
- [x] 所有导出函数名加 `atlas_` 前缀
- [x] **额外导出**: `atlas_set_native_api_provider()` + error bridge 查询函数（解决 DLL TLS 隔离问题，详见 [implementation_notes.md](implementation_notes.md) §1）

---

## 任务 2.5: 异常桥接

### 新建文件: `src/lib/clrscript/clr_error.hpp`, `clr_error.cpp`

**设计**: `[UnmanagedCallersOnly]` 方法不能抛出异常（会导致进程崩溃）。因此每个入口方法内部都必须 try/catch，并将异常信息写入线程本地缓冲区。

### 错误缓冲区

```cpp
// C++ 侧
struct ClrErrorBuffer
{
    int32_t error_code;
    char message[1024];
    int32_t message_length;
};

// 线程本地存储（避免并发冲突）
thread_local ClrErrorBuffer t_clr_error{};

// C++ 侧读取最近的 CLR 错误
auto read_clr_error() -> Error;
void clear_clr_error();
auto has_clr_error() -> bool;
```

```csharp
// C# 侧
internal static class ErrorBridge
{
    [ThreadStatic]
    private static string? _lastError;
    [ThreadStatic]
    private static int _lastErrorCode;

    public static void SetError(Exception ex)
    {
        _lastErrorCode = ex.HResult;
        _lastError = ex.Message;
    }

    [UnmanagedCallersOnly]
    public static int GetLastError(byte* buffer, int bufferLen)
    {
        if (_lastError == null) return 0;
        // 计算需要的字节数，截断到缓冲区大小
        int byteCount = Encoding.UTF8.GetByteCount(_lastError);
        if (byteCount <= bufferLen)
        {
            return Encoding.UTF8.GetBytes(_lastError,
                new Span<byte>(buffer, bufferLen));
        }
        // 缓冲区不够: 截断字符串直到适合
        // 逐字符缩减以确保不截断在 UTF-8 多字节序列中间
        var truncated = _lastError;
        while (Encoding.UTF8.GetByteCount(truncated) > bufferLen && truncated.Length > 0)
            truncated = truncated[..^1];
        return Encoding.UTF8.GetBytes(truncated,
            new Span<byte>(buffer, bufferLen));
    }

    [UnmanagedCallersOnly]
    public static int GetLastErrorCode() => _lastErrorCode;

    [UnmanagedCallersOnly]
    public static void ClearError()
    {
        _lastError = null;
        _lastErrorCode = 0;
    }
}
```

### [UnmanagedCallersOnly] 方法的标准模式

```csharp
// 每个入口方法都遵循此模式:
[UnmanagedCallersOnly]
public static int SomeMethod(int arg1, float arg2)
{
    try
    {
        // 业务逻辑
        return 0;  // 成功
    }
    catch (Exception ex)
    {
        ErrorBridge.SetError(ex);
        return -1;  // 失败
    }
}
```

```cpp
// C++ 侧调用:
auto result = some_method.invoke(arg1, arg2);
if (*result == -1)
{
    return read_clr_error();  // 从 TLS 读取异常信息
}
```

### 工作点

- [x] `ClrErrorBuffer` 使用 `thread_local` 避免并发冲突 — `clr_error.hpp/cpp`
- [x] C# `ErrorBridge` 使用**静态函数指针字段**（`delegate* unmanaged`），通过 `ClrBootstrapArgs` 从 C++ 注入 `clr_error_set/clear/get_code` 地址。不使用 `[ThreadStatic]`——TLS 在 C++ 侧管理。
- [x] `read_clr_error()`: 从 `t_clr_error` 读取 error_code + message，构造 `Error` 对象，自动清除缓冲区
- [x] 错误消息超过 1024 字节时截断（`std::min(msg_len, kBufSize)`）
- [x] `clear_clr_error()`: `t_clr_error = ClrErrorBuffer{}`
- [x] **⚠️ DLL TLS 隔离**: 需要通过 `atlas_get_clr_error_set_fn()` 获取 DLL 内的函数指针传给 C# — 详见 [implementation_notes.md](implementation_notes.md) §1

---

## 任务 2.6: ~~`Atlas.Generators.Interop` Source Generator~~ → 已移除

> **状态**: 已实现后移除。经过实现验证和方案对比，决定不使用自定义 Source Generator。

### 移除原因

1. **Roslyn 链式限制**: Source Generator 的输出不会被其他 Source Generator 处理。
   `[LibraryImport]` 本身就是 Source Generator，我们生成的 `[LibraryImport] partial`
   方法不会被 `LibraryImportGenerator` 扫描到（CS8795 编译错误）。

2. **Custom Marshaller 限制**: .NET 7+ 的 `[MarshalUsing]` Custom Marshaller 是
   一对一参数映射，无法将 `ReadOnlySpan<byte>` 展开为 `(byte*, int)` 两个原生参数。
   我们的 C++ 函数签名是分离式的 `(const char* msg, int32_t len)`。

3. **收益不足**: 当前 `atlas_*` 导出函数 < 15 个，手写 `[LibraryImport]` + `fixed`
   wrapper 的工作量很小且代码清晰。自定义 Generator 引入了 netstandard2.0 项目、
   增量缓存（EquatableArray）、诊断系统等大量基础设施，维护成本高于收益。

### 最终方案

直接在 `NativeApi.cs` 中使用 `[LibraryImport("atlas_engine")]`：
- **blittable 参数**: 直接声明为 `public static partial` 方法
- **Span 参数**: 手写 private `[LibraryImport]` (byte\* + int) + public wrapper (fixed)
- 这是 .NET 官方推荐的 Span/P/Invoke 模式，IL2CPP 安全

详见 [implementation_notes.md](implementation_notes.md) §3。

### 原设计文档（以下为历史记录）

<details>
<summary>点击展开原始 Task 2.6 设计（已废弃）</summary>


### 属性定义

```csharp
[AttributeUsage(AttributeTargets.Method)]
public sealed class NativeImportAttribute : Attribute
{
    /// <param name="entryPoint">C++ 导出函数名 (如 "atlas_log_message")</param>
    public NativeImportAttribute(string entryPoint) { }

    /// <summary>DLL 名称，默认 "atlas_engine"</summary>
    public string LibraryName { get; set; } = "atlas_engine";
}
```

### 用户使用方式 (输入)

```csharp
public static partial class NativeApi
{
    [NativeImport("atlas_log_message")]
    public static partial void LogMessage(int level, ReadOnlySpan<byte> message);

    [NativeImport("atlas_server_time")]
    public static partial double ServerTime();

    [NativeImport("atlas_delta_time")]
    public static partial float DeltaTime();

    [NativeImport("atlas_send_client_rpc")]
    public static partial void SendClientRpc(
        uint entityId, uint rpcId, byte target,
        ReadOnlySpan<byte> payload);
}
```

### Generator 输出

```csharp
// <auto-generated by Atlas.Generators.Interop/>
public static partial class NativeApi
{
    // ---- LogMessage: ReadOnlySpan<byte> → byte* + int ----
    [LibraryImport("atlas_engine", EntryPoint = "atlas_log_message")]
    private static unsafe partial void _LogMessage(
        int level, byte* message, int messageLen);

    public static partial void LogMessage(int level, ReadOnlySpan<byte> message)
    {
        unsafe
        {
            fixed (byte* ptr = message)
            {
                _LogMessage(level, ptr, message.Length);
            }
        }
    }

    // ---- ServerTime: blittable 直通 ----
    [LibraryImport("atlas_engine", EntryPoint = "atlas_server_time")]
    public static partial double ServerTime();

    // ---- DeltaTime: blittable 直通 ----
    [LibraryImport("atlas_engine", EntryPoint = "atlas_delta_time")]
    public static partial float DeltaTime();

    // ---- SendClientRpc: 混合参数 ----
    [LibraryImport("atlas_engine", EntryPoint = "atlas_send_client_rpc")]
    private static unsafe partial void _SendClientRpc(
        uint entityId, uint rpcId, byte target,
        byte* payload, int payloadLen);

    public static partial void SendClientRpc(
        uint entityId, uint rpcId, byte target,
        ReadOnlySpan<byte> payload)
    {
        unsafe
        {
            fixed (byte* ptr = payload)
            {
                _SendClientRpc(entityId, rpcId, target, ptr, payload.Length);
            }
        }
    }
}
```

### Generator 实现要点

- [x] 实现 `IIncrementalGenerator` 接口 — `InteropGenerator.cs`
- [x] 使用 `ForAttributeWithMetadataName` 高效扫描 `[NativeImport]` 标记的 `static partial` 方法
- [x] 根据参数类型决定生成模式:

| C# 参数类型 | 生成策略 | 说明 |
|-------------|---------|------|
| `int` / `long` / `float` / `double` / `byte` 等 | 直通 | private `[DllImport] extern` + public 转发 |
| `IntPtr` / `nuint` / unmanaged struct | 直通 | 同上 |
| `ReadOnlySpan<byte>` | 拆分 | private `_Method(byte*, int)` + public wrapper with `fixed` |
| `string` | UTF-8 转换 | private `_Method(byte*, int)` + `Encoding.UTF8.GetBytes` + `fixed` |

- [x] **所有方法**统一生成 private `[DllImport] extern _Method` + public partial wrapper body（包括纯 blittable 方法）
- [x] 生成编译期诊断 ATLAS_INTEROP001–004 — `DiagnosticDescriptors.cs`
- [x] 增量缓存正确性: `EquatableArray<T>` 包装 `ImmutableArray<T>` 实现值语义相等

> **⚠️ 设计偏差**: 原计划生成 `[LibraryImport]` partial 方法，但 Roslyn Source Generator
> 的输出**不会被其他 Source Generator 处理**（`LibraryImportGenerator` 不会扫描我们
> 生成的代码）。因此改用 `[DllImport]` + `static extern`。对全 blittable 参数零开销，
> IL2CPP 安全。详见 [implementation_notes.md](implementation_notes.md) §3。

### IL2CPP 安全验证

- [x] 生成的 P/Invoke 基于 `[DllImport]` + 全 blittable 参数（`byte*`、`int` 等），无运行时 marshaling 开销
- [x] 无 `Marshal.GetDelegateForFunctionPointer`
- [x] 无 `Delegate.DynamicInvoke`
- [x] 无 `Activator.CreateInstance`
- [x] 无 `MethodInfo.Invoke`

</details>

---

## 任务 2.7: C++ / C# 一致性保证

### 方案选择

采用 `[LibraryImport]` 后，C++ 和 C# 的函数签名一致性不再由结构体布局保证。有两种方式确保一致性:

| 方案 | 优点 | 缺点 |
|------|------|------|
| 手写 + 编译期测试 | 简单直接，函数数量少时足够 | 新增函数需手动同步两侧 |
| YAML IDL 代码生成 | 单一定义源，自动生成两侧代码 | 引入工具链依赖 |

**推荐**: 初期手写（当前导出函数 < 15 个），辅以集成测试验证。后续函数增多时可引入 YAML 生成工具。

### 集成测试验证

```cpp
// test_native_api_consistency.cpp
// 验证 C++ 导出函数确实存在且可被调用
TEST(NativeApiConsistency, AllFunctionsExported)
{
    // 通过 dlsym / GetProcAddress 验证所有 atlas_* 符号已导出
    auto handle = DynamicLibrary::load_self();
    ASSERT_TRUE(handle);

    EXPECT_TRUE(handle->get_symbol<void*>("atlas_log_message"));
    EXPECT_TRUE(handle->get_symbol<void*>("atlas_server_time"));
    EXPECT_TRUE(handle->get_symbol<void*>("atlas_delta_time"));
    EXPECT_TRUE(handle->get_symbol<void*>("atlas_get_process_prefix"));
    EXPECT_TRUE(handle->get_symbol<void*>("atlas_send_client_rpc"));
    EXPECT_TRUE(handle->get_symbol<void*>("atlas_send_cell_rpc"));
    EXPECT_TRUE(handle->get_symbol<void*>("atlas_send_base_rpc"));
    EXPECT_TRUE(handle->get_symbol<void*>("atlas_register_entity_type"));
    EXPECT_TRUE(handle->get_symbol<void*>("atlas_unregister_all_entity_types"));
}
```

### 工作点

- [ ] 确保所有 `atlas_*` C++ 导出函数的签名与 C# `[LibraryImport]` 声明参数类型一一对应
- [ ] 集成测试: 通过 `dlsym` / `GetProcAddress` 验证所有符号已导出
- [ ] 后续如函数数量超过 20 个，考虑引入 YAML → C++ header / C# partial class 代码生成

---

## 任务 2.8: 单元测试

### C++ 测试矩阵（全部已实现）

| 测试文件 | 测试数 | 核心用例 |
|---------|--------|---------|
| `test_clr_marshal.cpp` | 41 | blittable 结构体布局/偏移; 字符串/byte[] 往返; ScriptValue 全类型 |
| `test_clr_object.cpp` | 21 | GCHandle 分配/释放; move 语义; vtable 方法调用; 错误传播; Debug 泄漏计数 |
| `test_clr_invoke.cpp` | 12 | 绑定/调用; int error convention; void/float 返回值; move 语义 |
| `test_clr_callback.cpp` | 6 | **CLR 集成测试**: ABI 版本; LogMessage; ServerTime; ErrorBridge 异常传播; GCHandle 生命周期; ProcessPrefix |
| `test_clr_error.cpp` | 12 | TLS 缓冲区读写; 截断; 线程隔离; clear/get_code |
| `test_native_api_provider.cpp` | 14 | Provider 注册/获取; BaseNativeProvider 默认行为 |
| `test_native_api_exports.cpp` | 10 | atlas_engine.dll 符号导出验证（dlsym/GetProcAddress） |
| **总计** | **116** | |

### Generator 测试

Generator 通过 `dotnet build -p:EmitCompilerGeneratedFiles=true` 验证。
C# xUnit 测试（`CSharpGeneratorDriver` 快照测试）计划在 ScriptPhase 6 测试稳定化阶段添加。

---

## 任务依赖图

```
2.1 类型编组 (Marshal) ────────────────┐
                                       │
2.2 GCHandle 管理 (ClrObject) ─────────┤
                                       │
2.3 C++ → C# 调用 (ClrInvoke) ────────┤
                                       ├─── 2.8 单元测试
2.4 C# → C++ 回调 (函数表) ───────────┤
                                       │
2.5 异常桥接 (ClrError) ──────────────┤
                                       │
2.6 Interop Generator ─── 2.7 C++ 配套工具
```

**建议执行顺序**: 2.1 → 2.3 + 2.4 (并行) → 2.2 → 2.5 → 2.6 + 2.7 (并行) → 2.8

---

## 风险与缓解

| 风险 | 概率 | 影响 | 缓解措施 |
|------|------|------|---------|
| C++ `bool` 与 C# `bool` 大小不一致 | 高 | 中 | C# 侧使用 `byte` 替代 `bool`; 或 `[MarshalAs(UnmanagedType.U1)]` |
| GCHandle 泄漏导致内存增长 | 中 | 高 | Debug 模式 alloc/free 计数; 定期报告 leak_count |
| Source Generator 调试困难 | 中 | 中 | 使用 `Debugger.Launch()` 附加; 生成文件 emit 到磁盘检查 |
| 结构体布局 C++/C# 不匹配 | 中 | 高 | 双侧 `static_assert`/`Debug.Assert` sizeof 校验 |
| Windows 可执行文件符号导出 | 低 | 中 | `.def` 文件或 `__declspec(dllexport)` 确保 `atlas_*` 符号可被 C# 找到 |
