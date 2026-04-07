# ScriptPhase 1: .NET 10 运行时嵌入

> 预估周期: 2–3 周 | 前置依赖: ScriptPhase 0 完成

---

## 目标

1. 在 C++ 进程中嵌入 .NET 10 CoreCLR 运行时（通过 `hostfxr`）
2. 实现 C++ → C# 的基本调用: 获取 `[UnmanagedCallersOnly]` 方法的函数指针并调用
3. 配置 Server GC、分层编译等运行时参数
4. 建立 CMake 中 .NET SDK 发现和 C# 项目编译流程
5. 跨平台支持: Windows + Linux

## 验收标准 (M1)

- [ ] C++ 进程能加载 CoreCLR 并成功初始化
- [ ] C++ 可调用 C# `[UnmanagedCallersOnly]` 静态方法并获取正确返回值
- [ ] `runtimeconfig.json` 配置 Server GC 和分层编译
- [ ] CMake 能发现 .NET SDK 并自动编译 C# 测试项目
- [ ] Windows 和 Linux 上均可运行
- [ ] 关闭时 CoreCLR 正常卸载，无泄漏

---

## 任务 1.1: CMake .NET SDK 发现

### 新建文件: `cmake/FindDotNet.cmake`

```cmake
# FindDotNet.cmake
# Locate the .NET SDK and hostfxr library.
#
# Sets:
#   DOTNET_FOUND       - TRUE if .NET SDK was found
#   DOTNET_EXECUTABLE  - Path to 'dotnet' command
#   DOTNET_SDK_DIR     - SDK root directory
#   DOTNET_HOSTFXR_LIB - Path to hostfxr shared library
#   DOTNET_HOSTFXR_INCLUDE - Path to hostfxr headers

find_program(DOTNET_EXECUTABLE dotnet
    HINTS
        "$ENV{DOTNET_ROOT}"
        "$ENV{ProgramFiles}/dotnet"
        "/usr/share/dotnet"
        "/usr/local/share/dotnet"
)

if(NOT DOTNET_EXECUTABLE)
    message(FATAL_ERROR "Could not find 'dotnet' executable. "
            "Set DOTNET_ROOT environment variable.")
endif()

execute_process(
    COMMAND ${DOTNET_EXECUTABLE} --version
    OUTPUT_VARIABLE DOTNET_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
message(STATUS "Found .NET SDK: ${DOTNET_VERSION}")

# Derive SDK root from dotnet executable path
get_filename_component(DOTNET_SDK_DIR "${DOTNET_EXECUTABLE}" DIRECTORY)

# Find hostfxr
if(WIN32)
    set(_hostfxr_name "hostfxr.dll")
    set(_lib_prefix "")
else()
    set(_hostfxr_name "libhostfxr.so")
    set(_lib_prefix "lib")
endif()

# hostfxr lives in: <sdk_root>/host/fxr/<version>/
file(GLOB _hostfxr_versions
    "${DOTNET_SDK_DIR}/host/fxr/*"
)
list(SORT _hostfxr_versions ORDER DESCENDING)
list(GET _hostfxr_versions 0 _hostfxr_version_dir)

find_library(DOTNET_HOSTFXR_LIB
    NAMES hostfxr
    PATHS "${_hostfxr_version_dir}"
    NO_DEFAULT_PATH
)

if(NOT DOTNET_HOSTFXR_LIB)
    message(FATAL_ERROR "Could not find hostfxr library in ${_hostfxr_version_dir}")
endif()

message(STATUS "Found hostfxr: ${DOTNET_HOSTFXR_LIB}")

# hostfxr header (nethost.h / hostfxr.h / coreclr_delegates.h)
# Shipped with the .NET SDK in packs/Microsoft.NETCore.App.Host.*/
file(GLOB _nethost_dirs
    "${DOTNET_SDK_DIR}/packs/Microsoft.NETCore.App.Host.*/*/runtimes/*/native"
)
list(SORT _nethost_dirs ORDER DESCENDING)
if(_nethost_dirs)
    list(GET _nethost_dirs 0 DOTNET_HOSTFXR_INCLUDE)
else()
    # Fallback: use nethost from the SDK
    set(DOTNET_HOSTFXR_INCLUDE "${DOTNET_SDK_DIR}/packs")
endif()

set(DOTNET_FOUND TRUE)

# Helper function: build a C# project with dotnet CLI
function(atlas_build_csharp_project project_dir output_var)
    set(${output_var}_DIR "${CMAKE_BINARY_DIR}/csharp/${project_dir}" PARENT_SCOPE)
    add_custom_target(csharp_${project_dir} ALL
        COMMAND ${DOTNET_EXECUTABLE} build
            "${CMAKE_SOURCE_DIR}/${project_dir}"
            -c $<IF:$<CONFIG:Debug>,Debug,Release>
            -o "${CMAKE_BINARY_DIR}/csharp/${project_dir}"
        COMMENT "Building C# project: ${project_dir}"
    )
endfunction()
```

### 修改 `cmake/AtlasFindPackages.cmake`

```cmake
# 删除:
# find_package(Python3 REQUIRED COMPONENTS Development Interpreter)

# 新增:
include(${CMAKE_SOURCE_DIR}/cmake/FindDotNet.cmake)
```

### 工作点

- [ ] `dotnet` 可执行文件发现（环境变量 + 常见路径）
- [ ] `hostfxr` 库定位（遍历版本目录取最新）
- [ ] `nethost.h` / `hostfxr.h` / `coreclr_delegates.h` 头文件定位
- [ ] `atlas_build_csharp_project()` 辅助函数
- [ ] Windows / Linux 路径差异处理

---

## 任务 1.2: ClrHost 核心实现

### 新建文件: `src/lib/clrscript/clr_host.hpp`

```cpp
#pragma once

#include "foundation/error.hpp"
#include "platform/dynamic_library.hpp"

#include <hostfxr.h>
#include <coreclr_delegates.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace atlas
{

// ============================================================================
// ClrHost — CoreCLR runtime host via hostfxr
// ============================================================================
//
// Lifecycle: initialize() → get_method() ... → finalize()
//
// Thread safety: initialize/finalize must be called from the main thread.
//                get_method is safe after initialization.

class ClrHost
{
public:
    ClrHost() = default;
    ~ClrHost();

    ClrHost(const ClrHost&) = delete;
    ClrHost& operator=(const ClrHost&) = delete;
    ClrHost(ClrHost&&) noexcept;
    ClrHost& operator=(ClrHost&&) noexcept;

    // Initialize CoreCLR with the given runtimeconfig.json.
    [[nodiscard]] auto initialize(
        const std::filesystem::path& runtime_config_path) -> Result<void>;

    // Shut down CoreCLR.
    void finalize();

    // Get a function pointer to a C# [UnmanagedCallersOnly] static method.
    //
    // assembly_path: full path to the .dll
    // type_name: fully qualified type (e.g., "Atlas.Runtime.Bootstrap, Atlas.Runtime")
    // method_name: method name (e.g., "Initialize")
    //
    // Returns a raw function pointer — caller casts to the correct signature.
    [[nodiscard]] auto get_method(
        const std::filesystem::path& assembly_path,
        std::string_view type_name,
        std::string_view method_name) -> Result<void*>;

    [[nodiscard]] auto is_initialized() const -> bool { return initialized_; }

private:
    // Platform-specific: locate and load hostfxr
    [[nodiscard]] auto load_hostfxr() -> Result<void>;

    // hostfxr 共享库（必须存为成员以保持加载状态，否则函数指针悬空）
    std::optional<DynamicLibrary> hostfxr_lib_;

    void* host_context_ = nullptr;     // hostfxr context handle

    // Cached hostfxr API（类型化函数指针，由 load_hostfxr 填充）
    hostfxr_initialize_for_runtime_config_fn fn_init_config_ = nullptr;
    hostfxr_get_runtime_delegate_fn          fn_get_delegate_ = nullptr;
    hostfxr_close_fn                         fn_close_ = nullptr;

    // Cached CoreCLR delegate
    load_assembly_and_get_function_pointer_fn fn_load_assembly_ = nullptr;

#ifdef _WIN32
    // Windows: UTF-8 → UTF-16 转换（hostfxr 使用 wchar_t）
    [[nodiscard]] static auto utf8_to_wstring(std::string_view utf8) -> std::wstring;
#endif

    bool initialized_ = false;
};

} // namespace atlas
```

### 新建文件: `src/lib/clrscript/clr_host.cpp`

```cpp
#include "clrscript/clr_host.hpp"
#include "foundation/log.hpp"
#include "platform/dynamic_library.hpp"

// hostfxr API types
#include <nethost.h>
#include <hostfxr.h>
#include <coreclr_delegates.h>

namespace atlas
{

ClrHost::~ClrHost()
{
    if (initialized_)
    {
        finalize();
    }
}

auto ClrHost::initialize(
    const std::filesystem::path& runtime_config_path) -> Result<void>
{
    if (initialized_)
    {
        return Error{ErrorCode::ScriptError, "ClrHost already initialized"};
    }

    // Step 1: Load hostfxr shared library
    auto load_result = load_hostfxr();
    if (!load_result)
    {
        return load_result;
    }

    // Step 2: Initialize CoreCLR with runtime config
    hostfxr_handle ctx = nullptr;
    int rc = fn_init_config_(
        runtime_config_path.c_str(),
        nullptr,        // optional parameters
        &ctx);

    if (rc != 0 || ctx == nullptr)
    {
        return Error{ErrorCode::ScriptError,
            std::format("hostfxr_initialize_for_runtime_config failed: 0x{:08X}", rc)};
    }
    host_context_ = ctx;

    // Step 3: Get the load_assembly_and_get_function_pointer delegate
    void* load_assembly_fn = nullptr;
    rc = fn_get_delegate_(
        ctx,
        hdt_load_assembly_and_get_function_pointer,
        &load_assembly_fn);

    if (rc != 0 || load_assembly_fn == nullptr)
    {
        fn_close_(ctx);
        host_context_ = nullptr;
        return Error{ErrorCode::ScriptError,
            std::format("hostfxr_get_runtime_delegate failed: 0x{:08X}", rc)};
    }
    fn_load_assembly_ = reinterpret_cast<
        load_assembly_and_get_function_pointer_fn>(load_assembly_fn);

    initialized_ = true;
    ATLAS_LOG_INFO("ClrHost: CoreCLR initialized successfully");
    return {};
}

void ClrHost::finalize()
{
    if (!initialized_)
    {
        return;
    }

    if (host_context_)
    {
        fn_close_(host_context_);
        host_context_ = nullptr;
    }

    fn_load_assembly_ = nullptr;
    initialized_ = false;

    ATLAS_LOG_INFO("ClrHost: CoreCLR finalized");
}

auto ClrHost::get_method(
    const std::filesystem::path& assembly_path,
    std::string_view type_name,
    std::string_view method_name) -> Result<void*>
{
    if (!initialized_)
    {
        return Error{ErrorCode::ScriptError, "ClrHost not initialized"};
    }

    void* method_ptr = nullptr;

    // hostfxr API 使用 char_t: Windows = wchar_t, Linux/macOS = char
    // 必须正确处理字符串编码转换
#ifdef _WIN32
    // Windows: 需要 UTF-8 → UTF-16 转换
    auto w_assembly = assembly_path.wstring();
    auto w_type = utf8_to_wstring(type_name);
    auto w_method = utf8_to_wstring(method_name);

    int rc = fn_load_assembly_(
        w_assembly.c_str(),
        w_type.c_str(),
        w_method.c_str(),
        UNMANAGEDCALLERSONLY_METHOD,
        nullptr,
        &method_ptr);
#else
    // Linux/macOS: char_t = char，直接使用 UTF-8
    auto assembly_str = assembly_path.string();
    auto type_str = std::string(type_name);
    auto method_str = std::string(method_name);

    int rc = fn_load_assembly_(
        assembly_str.c_str(),
        type_str.c_str(),
        method_str.c_str(),
        UNMANAGEDCALLERSONLY_METHOD,
        nullptr,
        &method_ptr);
#endif

    if (rc != 0 || method_ptr == nullptr)
    {
        return Error{ErrorCode::ScriptError, std::format(
            "Failed to load method '{}.{}': 0x{:08X}",
            type_name, method_name, rc)};
    }

    return method_ptr;
}

// Windows 辅助函数: UTF-8 → UTF-16 (正确处理非 ASCII 字符)
#ifdef _WIN32
auto ClrHost::utf8_to_wstring(std::string_view utf8) -> std::wstring
{
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0,
        utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring result(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0,
        utf8.data(), static_cast<int>(utf8.size()), result.data(), len);
    return result;
}
#endif

} // namespace atlas
```

### 设计要点

| 决策 | 理由 |
|------|------|
| 使用 `hostfxr` 而非 `coreclr_create_delegate` | `hostfxr` 是 .NET 官方推荐的嵌入方式，支持 runtime config |
| `[UnmanagedCallersOnly]` 方法 | 零 marshaling 开销，调用约定与 C 函数完全一致 |
| 函数指针缓存 | `get_method()` 结果应被调用者缓存，避免重复查找 |
| `void*` 返回 | 调用者根据已知签名 `reinterpret_cast`，保持类型灵活性 |

---

## 任务 1.3: 平台适配层

### 新建文件: `src/lib/clrscript/clr_host_windows.cpp`

```cpp
#include "clrscript/clr_host.hpp"
#include "platform/dynamic_library.hpp"

#include <nethost.h>

#ifdef _WIN32

namespace atlas
{

auto ClrHost::load_hostfxr() -> Result<void>
{
    // Get hostfxr path via nethost
    char_t buffer[MAX_PATH];
    size_t buffer_size = sizeof(buffer) / sizeof(char_t);
    int rc = get_hostfxr_path(buffer, &buffer_size, nullptr);
    if (rc != 0)
    {
        return Error{ErrorCode::ScriptError, std::format(
            "get_hostfxr_path failed: 0x{:08X}", rc)};
    }

    // Load hostfxr.dll — 存为成员，保持库加载状态
    auto lib_result = DynamicLibrary::load(buffer);
    if (!lib_result)
    {
        return Error{ErrorCode::ScriptError, "Failed to load hostfxr.dll: "
            + std::string(lib_result.error().message())};
    }
    hostfxr_lib_ = std::move(*lib_result);

    // Resolve function pointers (类型化，编译期安全)
    auto init_result = hostfxr_lib_->get_symbol<
        hostfxr_initialize_for_runtime_config_fn>(
            "hostfxr_initialize_for_runtime_config");
    auto delegate_result = hostfxr_lib_->get_symbol<
        hostfxr_get_runtime_delegate_fn>(
            "hostfxr_get_runtime_delegate");
    auto close_result = hostfxr_lib_->get_symbol<
        hostfxr_close_fn>("hostfxr_close");

    if (!init_result || !delegate_result || !close_result)
    {
        hostfxr_lib_.reset();
        return Error{ErrorCode::ScriptError, "Failed to resolve hostfxr functions"};
    }

    fn_init_config_ = *init_result;
    fn_get_delegate_ = *delegate_result;
    fn_close_ = *close_result;

    return {};
}

} // namespace atlas

#endif // _WIN32
```

### 新建文件: `src/lib/clrscript/clr_host_linux.cpp`

```cpp
#include "clrscript/clr_host.hpp"
#include "platform/dynamic_library.hpp"

#include <nethost.h>

#ifdef __linux__

namespace atlas
{

auto ClrHost::load_hostfxr() -> Result<void>
{
    char_t buffer[PATH_MAX];
    size_t buffer_size = sizeof(buffer) / sizeof(char_t);
    int rc = get_hostfxr_path(buffer, &buffer_size, nullptr);
    if (rc != 0)
    {
        return Error{ErrorCode::ScriptError, std::format(
            "get_hostfxr_path failed: 0x{:08X}", rc)};
    }

    // 存为成员，保持库加载状态
    auto lib_result = DynamicLibrary::load(buffer);
    if (!lib_result)
    {
        return Error{ErrorCode::ScriptError, "Failed to load libhostfxr.so: "
            + std::string(lib_result.error().message())};
    }
    hostfxr_lib_ = std::move(*lib_result);

    auto init_result = hostfxr_lib_->get_symbol<
        hostfxr_initialize_for_runtime_config_fn>(
            "hostfxr_initialize_for_runtime_config");
    auto delegate_result = hostfxr_lib_->get_symbol<
        hostfxr_get_runtime_delegate_fn>(
            "hostfxr_get_runtime_delegate");
    auto close_result = hostfxr_lib_->get_symbol<
        hostfxr_close_fn>("hostfxr_close");

    if (!init_result || !delegate_result || !close_result)
    {
        hostfxr_lib_.reset();
        return Error{ErrorCode::ScriptError, "Failed to resolve hostfxr functions"};
    }

    fn_init_config_ = *init_result;
    fn_get_delegate_ = *delegate_result;
    fn_close_ = *close_result;

    return {};
}

} // namespace atlas

#endif // __linux__
```

### 工作点

- [ ] Windows: `get_hostfxr_path` 使用 `wchar_t` (`char_t` = `wchar_t`)
- [ ] Linux: `get_hostfxr_path` 使用 `char` (`char_t` = `char`)
- [ ] macOS: `char_t` = `char`，与 Linux 逻辑一致，需新建 `clr_host_macos.cpp`（可复用 Linux 实现，仅库名从 `libhostfxr.so` 改为 `libhostfxr.dylib`）
- [ ] `nethost.h` 头文件路径需由 `FindDotNet.cmake` 提供
- [ ] `DynamicLibrary` 实例必须存为 `ClrHost` 成员（`std::optional<DynamicLibrary>`），不可使用局部变量——否则函数返回后库被卸载，缓存的函数指针悬空
- [ ] `get_method()` 中 UTF-8 → `char_t` 转换必须使用正确编码转换（Windows 用 `MultiByteToWideChar`），**禁止**逐字符拷贝 `std::wstring(sv.begin(), sv.end())`
- [ ] 错误处理: 路径不存在、版本不匹配、符号找不到

---

## 任务 1.4: Runtime 配置文件

### 新建文件: `runtime/atlas_server.runtimeconfig.json`

```json
{
  "runtimeOptions": {
    "tfm": "net10.0",
    "framework": {
      "name": "Microsoft.NETCore.App",
      "version": "10.0.0"
    },
    "configProperties": {
      "System.GC.Server": true,
      "System.GC.Concurrent": true,
      "System.GC.DynamicAdaptationMode": 1,
      "System.Runtime.TieredCompilation": true,
      "System.Runtime.TieredCompilation.QuickJit": true,
      "System.Runtime.TieredCompilation.QuickJitForLoops": true,
      "System.Threading.ThreadPool.MinThreads": 4,
      "System.GC.HeapHardLimitPercent": 80
    }
  }
}
```

### GC 配置说明

| 配置项 | 值 | 含义 |
|--------|----|----|
| `System.GC.Server` | `true` | 启用 Server GC: 每个逻辑 CPU 一个 GC 堆，吞吐量更高 |
| `System.GC.Concurrent` | `true` | 并发 GC: Gen2 回收不阻塞应用线程 |
| `System.GC.DynamicAdaptationMode` | `1` | DATAS: 动态自适应 GC，根据应用负载自动调整堆大小和回收频率（.NET 8+） |
| `TieredCompilation` | `true` | 分层编译: 先快速 JIT → 后台优化重编译热路径 |
| `QuickJit` | `true` | 首次 JIT 使用快速模式，减少启动延迟 |
| `QuickJitForLoops` | `true` | 含循环的方法也使用 QuickJit（.NET 10 默认） |
| `ThreadPool.MinThreads` | `4` | 线程池最小线程数，避免冷启动延迟 |
| `HeapHardLimitPercent` | `80` | GC 堆上限为进程可用内存的 80%，预留空间给 C++ 分配 |

---

## 任务 1.5: CMakeLists.txt — clrscript 库

### 新建文件: `src/lib/clrscript/CMakeLists.txt`

```cmake
# atlas_clrscript — .NET CoreCLR embedding and interop

atlas_add_library(atlas_clrscript
    SOURCES
        clr_host.cpp
        $<$<PLATFORM_ID:Windows>:clr_host_windows.cpp>
        $<$<PLATFORM_ID:Linux>:clr_host_linux.cpp>
    DEPS
        atlas_foundation
        atlas_platform
        atlas_script
)

# Link hostfxr
target_link_libraries(atlas_clrscript
    PRIVATE ${DOTNET_HOSTFXR_LIB}
)

# Include hostfxr headers (nethost.h, hostfxr.h, coreclr_delegates.h)
target_include_directories(atlas_clrscript
    PRIVATE ${DOTNET_HOSTFXR_INCLUDE}
)
```

### 修改 `src/lib/CMakeLists.txt`

```cmake
# 删除:
add_subdirectory(pyscript)

# 新增（在 script 之后）:
add_subdirectory(clrscript)
```

---

## 任务 1.6: C# 冒烟测试项目

### 新建目录: `tests/csharp/Atlas.SmokeTest/`

用于验证 CoreCLR 加载和基本的 C++ → C# 调用是否正常工作。

### 新建文件: `tests/csharp/Atlas.SmokeTest/Atlas.SmokeTest.csproj`

```xml
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net10.0</TargetFramework>
    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>
    <EnableDefaultItems>true</EnableDefaultItems>
  </PropertyGroup>
</Project>
```

### 新建文件: `tests/csharp/Atlas.SmokeTest/EntryPoint.cs`

```csharp
using System.Runtime.InteropServices;

namespace Atlas.SmokeTest;

public static class EntryPoint
{
    // C++ calls this to verify CoreCLR is working.
    // Returns 42 as a "hello world" handshake.
    [UnmanagedCallersOnly]
    public static int Ping()
    {
        return 42;
    }

    // Verify basic computation works.
    [UnmanagedCallersOnly]
    public static int Add(int a, int b)
    {
        return a + b;
    }

    // Verify string interop: returns length of a UTF-8 string.
    [UnmanagedCallersOnly]
    public static unsafe int StringLength(byte* utf8Ptr, int byteLen)
    {
        var span = new ReadOnlySpan<byte>(utf8Ptr, byteLen);
        var str = System.Text.Encoding.UTF8.GetString(span);
        return str.Length;
    }
}
```

### C++ 侧冒烟测试

```cpp
// tests/unit/test_clr_host.cpp

#include <gtest/gtest.h>
#include "clrscript/clr_host.hpp"

namespace atlas::test
{

class ClrHostTest : public ::testing::Test
{
protected:
    ClrHost host;

    void SetUp() override
    {
        auto result = host.initialize("runtime/atlas_server.runtimeconfig.json");
        ASSERT_TRUE(result.has_value())
            << "ClrHost init failed: " << result.error().message();
    }

    void TearDown() override
    {
        host.finalize();
    }
};

TEST_F(ClrHostTest, Ping)
{
    auto method = host.get_method(
        "tests/csharp/Atlas.SmokeTest/bin/Atlas.SmokeTest.dll",
        "Atlas.SmokeTest.EntryPoint, Atlas.SmokeTest",
        "Ping");
    ASSERT_TRUE(method.has_value());

    using PingFn = int(*)();
    auto ping = reinterpret_cast<PingFn>(*method);
    EXPECT_EQ(ping(), 42);
}

TEST_F(ClrHostTest, Add)
{
    auto method = host.get_method(
        "tests/csharp/Atlas.SmokeTest/bin/Atlas.SmokeTest.dll",
        "Atlas.SmokeTest.EntryPoint, Atlas.SmokeTest",
        "Add");
    ASSERT_TRUE(method.has_value());

    using AddFn = int(*)(int, int);
    auto add = reinterpret_cast<AddFn>(*method);
    EXPECT_EQ(add(17, 25), 42);
    EXPECT_EQ(add(-1, 1), 0);
    EXPECT_EQ(add(0, 0), 0);
}

TEST_F(ClrHostTest, InitializeTwiceFails)
{
    auto result = host.initialize("runtime/atlas_server.runtimeconfig.json");
    EXPECT_FALSE(result.has_value());
}

TEST_F(ClrHostTest, GetMethodBeforeInitFails)
{
    ClrHost uninit_host;
    auto result = uninit_host.get_method("a.dll", "T, A", "M");
    EXPECT_FALSE(result.has_value());
}

} // namespace atlas::test
```

### 工作点

- [ ] C# 项目可由 CMake 自动构建 (`atlas_build_csharp_project`)
- [ ] `Ping()` 返回 42 验证基本链路
- [ ] `Add(a, b)` 验证参数传递
- [ ] `StringLength()` 验证 `unsafe` 指针互操作
- [ ] 错误场景: 重复初始化、未初始化就调用

---

## 任务 1.7: hostfxr API 类型参考

以下是 ScriptPhase 1 用到的 hostfxr API 调用链，供开发参考：

```
┌──────────────────────────────────────────────────────────────┐
│ 1. get_hostfxr_path()                                        │
│    → 定位 hostfxr.dll / libhostfxr.so 的路径                  │
│                                                               │
│ 2. LoadLibrary / dlopen(hostfxr_path)                        │
│    → 动态加载 hostfxr                                         │
│                                                               │
│ 3. hostfxr_initialize_for_runtime_config(config, ...)        │
│    → 用 runtimeconfig.json 初始化 CoreCLR                     │
│    → 返回 hostfxr_handle (上下文)                              │
│                                                               │
│ 4. hostfxr_get_runtime_delegate(ctx,                         │
│        hdt_load_assembly_and_get_function_pointer, &delegate) │
│    → 获取 load_assembly_and_get_function_pointer 回调          │
│                                                               │
│ 5. load_assembly_and_get_function_pointer(                   │
│        assembly_path, type_name, method_name,                │
│        UNMANAGEDCALLERSONLY_METHOD, ...)                      │
│    → 加载 C# 程序集 → 找到 [UnmanagedCallersOnly] 方法        │
│    → 返回原生函数指针                                          │
│                                                               │
│ 6. fn_ptr(args...)                                           │
│    → 直接调用，等同于调用 C 函数                                │
│                                                               │
│ 7. hostfxr_close(ctx)                                        │
│    → 关闭 CoreCLR 上下文                                       │
└──────────────────────────────────────────────────────────────┘
```

---

## 任务 1.8: 单元测试矩阵

| 测试文件 | 核心用例 | 前置条件 |
|---------|---------|---------|
| `test_clr_host.cpp` | `initialize()` → `Ping()` → `finalize()` 完整生命周期 | .NET SDK 已安装 |
| `test_clr_host.cpp` | `Add(a, b)` 参数传递正确性 | C# SmokeTest 已编译 |
| `test_clr_host.cpp` | `StringLength()` 指针互操作 | |
| `test_clr_host.cpp` | 重复 `initialize()` → Error | |
| `test_clr_host.cpp` | 未初始化 `get_method()` → Error | |
| `test_clr_host.cpp` | 不存在的方法 → Error | |
| `test_clr_host.cpp` | `finalize()` 后 `get_method()` → Error | |

### CMakeLists.txt 新增

```cmake
atlas_add_test(test_clr_host
    SOURCES test_clr_host.cpp
    DEPS atlas_clrscript
)
add_dependencies(test_clr_host csharp_tests_csharp_Atlas.SmokeTest)
```

---

## 任务依赖图

```
1.1 FindDotNet.cmake ─────────────────────┐
                                           │
1.2 ClrHost 核心实现 ──────────────────────┤
        │                                  │
1.3 平台适配 (Windows / Linux) ────────────┤
                                           │
1.4 runtimeconfig.json ────────────────────┤
                                           │
1.5 clrscript CMakeLists ─────────────────┤
                                           │
1.6 C# 冒烟测试项目 ─────────────────────┤
                                           │
1.7 (参考文档，无代码产出) ────────────────┤
                                           │
1.8 C++ 单元测试 ─────────────────────────┘
```

**建议执行顺序**: 1.1 → 1.5 → 1.4 → 1.2 + 1.3 (并行) → 1.6 → 1.8

**预计代码变更量**: C++ 新增 ~600 行 (ClrHost + 平台适配 + 测试)；C# 新增 ~60 行 (SmokeTest)；CMake 新增 ~80 行
