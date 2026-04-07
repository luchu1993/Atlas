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

- [ ] C++ 可调用 C# 静态方法并获取正确返回值
- [ ] C# 可通过 `[LibraryImport]` 调用 C++ 导出函数
- [ ] 支持 blittable 类型、字符串、byte[] 的双向传递
- [ ] `Atlas.Generators.Interop` 能从标记属性自动生成绑定代码
- [ ] C# 异常能被 C++ 侧捕获并转换为 `Error`
- [ ] GCHandle 生命周期管理正确，无泄漏

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

- [ ] `ClrObject(gc_handle)`: 存储句柄
- [ ] `~ClrObject()`: 调用 C# `GCHandleHelper.Free()` 释放
- [ ] `get_attr()` / `call()`: 通过预注册的 C# 辅助方法实现（避免反射——Source Generator 在 ScriptPhase 4 会生成这些辅助方法，此阶段先用简单的 trampoline）
- [ ] move 语义: 转移句柄所有权，source 置 null
- [ ] Debug 模式: 全局 `GCHandle` alloc/free 计数器，析构时检查平衡

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

- [ ] 定义 `ATLAS_EXPORT` / `ATLAS_NATIVE_API` 宏（参见 [native_api_architecture.md](native_api_architecture.md) §2.4 导出宏）
- [ ] 实现 `INativeApiProvider` 接口 + `BaseNativeProvider` 基类（参见架构文档 §3）
- [ ] C++ 所有 `atlas_*` 导出函数实现为一行委托到 `get_native_api_provider()`
- [ ] C# `NativeApi` 使用 `[LibraryImport("atlas_engine")]` 声明所有 P/Invoke 方法
- [ ] C# 高层 wrapper 方法处理 `ReadOnlySpan<byte>` → `byte*` + `int` 转换（`fixed` 语句）
- [ ] C# 高层 wrapper 方法（除 LogMessage 外）添加 `ThreadGuard.EnsureMainThread()` 调用（参见架构文档 §4）
- [ ] `free_gc_handle` **不在此处**——它是 C++ → C# 方向的调用。C++ `ClrObject` 析构时通过缓存的 `ClrStaticMethod<void, void*>` 调用 C# `GCHandleHelper.Free()`
- [ ] **扩展规则**: 新增函数需同步: ① C++ 导出函数 ② INativeApiProvider 虚方法 ③ C# LibraryImport 声明
- [ ] 确保所有导出函数名加 `atlas_` 前缀避免全局符号冲突

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

- [ ] `ClrErrorBuffer` 使用 `thread_local` 避免并发冲突
- [ ] C# `ErrorBridge` 使用 `[ThreadStatic]` 对应
- [ ] `read_clr_error()`: 调用 C# `GetLastError` + `GetLastErrorCode`，构造 `Error` 对象
- [ ] 错误消息超过 1024 字节时截断
- [ ] `clear_clr_error()`: 在每次成功调用后清除，避免残留

---

## 任务 2.6: `Atlas.Generators.Interop` Source Generator

### 新建目录: `src/csharp/Atlas.Generators.Interop/`

**说明**: 这是第一个 Source Generator，用于自动生成 C# 侧的 Native 函数绑定代码。替代手写的 `[LibraryImport]` 声明和 Span → pointer 转换样板代码。

### 项目结构

```
src/csharp/Atlas.Generators.Interop/
├── Atlas.Generators.Interop.csproj    # netstandard2.0 (Roslyn 要求)
├── Attributes/
│   └── NativeImportAttribute.cs       # [NativeImport]
└── InteropGenerator.cs                # IIncrementalGenerator
```

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

- [ ] 实现 `IIncrementalGenerator` 接口
- [ ] 扫描带 `[NativeImport]` 的 `static partial` 方法
- [ ] 根据参数类型决定生成模式:

| C# 参数类型 | 生成策略 | 说明 |
|-------------|---------|------|
| `int` / `long` / `float` / `double` / `byte` | 直通 | 直接生成 `[LibraryImport]` 为 public 方法 |
| `uint` / `ushort` 等 unsigned | 直通 | 同上 |
| `IntPtr` / `nuint` | 直通 | 同上 |
| `ReadOnlySpan<byte>` | 拆分 | 生成 private `_Method(byte*, int)` + public wrapper with `fixed` |
| `string` | UTF-8 转换 | 生成 private `_Method(byte*, int)` + `Encoding.UTF8.GetBytes` 转换 |
| blittable struct | 直通 | by value 传递 |

- [ ] 全参数 blittable 的方法: 直接生成 `[LibraryImport]` 为 public partial 方法（无需 wrapper）
- [ ] 含 Span/string 参数的方法: 生成 private `[LibraryImport]` + public wrapper
- [ ] 生成编译期诊断:
  - `ATLAS_INTEROP001`: 类未标记 `partial`
  - `ATLAS_INTEROP002`: 方法未标记 `partial`
  - `ATLAS_INTEROP003`: 参数类型不支持（非 blittable 且非 Span/string）
  - `ATLAS_INTEROP004`: `[NativeImport]` entryPoint 重复

### IL2CPP 安全验证

- [ ] 生成的代码全部基于 `[LibraryImport]` (source-generated P/Invoke)，编译期生成 marshalling，无运行时代码生成
- [ ] 无 `Marshal.GetDelegateForFunctionPointer`
- [ ] 无 `Delegate.DynamicInvoke`
- [ ] 无 `Activator.CreateInstance`
- [ ] 无 `MethodInfo.Invoke`

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

### 测试矩阵

| 测试文件 | 核心用例 |
|---------|---------|
| `test_clr_marshal.cpp` | blittable 类型往返; 空字符串/Unicode 字符串/超长字符串; byte[] 空/大; Vector3/Quaternion 精度 |
| `test_clr_object.cpp` | GCHandle 分配/释放; move 后源为空; 双重释放保护; Debug 模式泄漏计数 |
| `test_clr_invoke.cpp` | 绑定成功/调用正确; 绑定不存在方法→Error; 未绑定就调用→Error; void 返回值 |
| `test_clr_callback.cpp` | C# 通过 LibraryImport 调用 C++ atlas_log_message; C# 调用 atlas_server_time; atlas_engine 共享库符号解析正确 |
| `test_clr_error.cpp` | C# 抛异常→C++ 读取 Error; 嵌套异常; 超长消息截断; 清除后无残留 |

### Generator 测试 (C# xUnit)

```
tests/csharp/Atlas.Generators.Interop.Tests/
├── InteropGeneratorTests.cs    # 验证 Generator 输出正确
└── SnapshotTests/              # 快照测试 — 生成代码与期望文件比对
```

| 用例 | 验证内容 |
|------|---------|
| `GeneratesLibraryImport` | 标记 `[NativeImport]` → 生成 `[LibraryImport]` 声明 |
| `BlittableParam_DirectPassthrough` | blittable 参数 → 直接生成 public partial 方法 |
| `SpanParam_GeneratesWrapper` | `ReadOnlySpan<byte>` 参数 → 生成 private P/Invoke + public wrapper with `fixed` |
| `DiagnosticOnNonPartialClass` | 非 partial 类 → `ATLAS_INTEROP001` 诊断 |
| `DiagnosticOnDuplicateEntryPoint` | 重复 entryPoint → `ATLAS_INTEROP004` 诊断 |

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
