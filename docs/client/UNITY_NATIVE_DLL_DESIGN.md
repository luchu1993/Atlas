# Unity Native Network DLL 设计文档

**Status:** ✅ 已落地。当前 C API ABI 为 `0x02050000`: Login/Auth 保持
专用一次性回调,服务端下行消息通过 `on_deliver` 原样透传到 C#。Unity
Plugins 由 `tools/bin/setup_unity_client.{bat,sh}` 为宿主平台 staging,
跨平台产物由 CI workflow 生成。`AtlasNetSetTransportImpairment` 可给当前和后续 RUDP
channel 注入延迟 / 丢包,用于 movement prediction 验收。

**目标:** 把 C++ 网络层抽取为独立 native DLL,Unity 客户端通过 P/Invoke
调用。Unity 客户端 SDK 的高层 C# API(`AtlasClient` / `AvatarFilter` /
`LoginClient` / `AtlasNetworkManager`)在此 DLL 的 C API 之上构建。

**关键决策记录:**

- IL2CPP 回调采用 **Pattern B**(`[MonoPInvokeCallback]` + delegate +
  `Marshal.GetFunctionPointerForDelegate`)。当前 MVP Unity 6 编辑器版本
  (`6000.0.43f1-lilith-2`) 不支持 `[UnmanagedCallersOnly]`;其他 Unity
  runtime 必须先重跑探针。
  迁移到 Pattern A 留待目标 Unity runtime / BCL 支持函数指针后重测;探针保留在 `src/tools/il2cpp_probe/`,
  矩阵详见 [`docs/spike_il2cpp_callback.md`](../spike_il2cpp_callback.md)。
- 依赖解耦:`ProcessType` 落在 `foundation/process_type.{h,cc}`,
  `DatabaseID` 落在 `server/entity_types.h`,`atlas_serialization_binary`
  独立成 target;`atlas_net_client` 只 include `src/server` 的 header-only
  消息定义,链接闭包不含 `atlas_server` / `atlas_db*` / `atlas_entitydef` /
  pugixml。
- 跨平台构建:`CMakePresets.json` 含
  `net-client-{android-arm64, ios-arm64, macos-arm64, linux-x64}`,
  `.github/workflows/atlas-net-client.yml` 矩阵编译 + artifact 30 天保留。

---

## 1. 架构总览

### 1.1 现状

```
当前 C++ 客户端架构:
┌─────────────────────────────────────────────┐
│  ClientApp (C++)                            │
│  ├── ClrScriptEngine (hostfxr 嵌入 CoreCLR) │
│  ├── EventDispatcher (IO 事件循环)           │
│  ├── NetworkInterface (RUDP/TCP/UDP)        │
│  ├── InterfaceTable (消息注册与分发)         │
│  └── ClientNativeProvider (C++↔C# 桥接)     │
├─────────────────────────────────────────────┤
│  Atlas.Client (C# via CoreCLR)              │
│  ├── ClientEntity / EntityManager           │
│  ├── ClientSession / ClientHost             │
│  ├── AtlasNetNative (P/Invoke → atlas_net_client)│
│  └── RPC 分发 + SourceGenerator             │
└─────────────────────────────────────────────┘
```

**问题**: ClientApp 通过 hostfxr 嵌入 CoreCLR，无法直接在 Unity 中使用。Unity 自带 Mono/IL2CPP 运行时，需要的是一个纯 C++ 网络 DLL。

### 1.2 目标架构

```
Unity 客户端架构:
┌─────────────────────────────────────────────┐
│  Unity C# (MonoBehaviour)                   │
│  ├── AtlasNetworkManager                    │
│  │   ├── Update() → AtlasNetPoll()        │
│  │   ├── Login/Auth 调用                     │
│  │   └── 回调分发                            │
│  ├── Atlas.Client (改造后)                   │
│  │   ├── ClientEntity / EntityManager       │
│  │   ├── AtlasNetNative / AtlasNetCallbackBridge│
│  │   └── RPC 分发 + SourceGenerator         │
│  └── Atlas.Shared (直接复用)                 │
│      ├── SpanWriter / SpanReader            │
│      ├── MessageIds / RPC 属性              │
│      └── DataTypes                          │
├─────────────────────────────────────────────┤
│  atlas_net_client.dll (纯 C++ native)       │
│  ├── EventDispatcher (IO 事件循环)           │
│  ├── NetworkInterface (RUDP/TCP/UDP)        │
│  ├── InterfaceTable (消息注册与分发)         │
│  ├── Login/Auth 状态机 (封装)               │
│  └── C API 导出层                           │
│      不包含: CLR, server, entitydef, db     │
└─────────────────────────────────────────────┘
```

### 1.3 核心原则

- **C# 驱动 Tick**: Unity 的 `Update()` 调用 `AtlasNetPoll()` 驱动网络事件循环
- **高层封装 Login/Auth**: DLL 内部管理连接切换和协议状态机，C# 只需调用并等待回调
- **回调上行**: 收到 RPC/实体创建/断线等事件时，DLL 通过注册的函数指针回调 C#
- **零 CLR 依赖**: DLL 不嵌入 CoreCLR，不依赖 hostfxr

---

## 2. 依赖解耦

### 2.1 当前依赖闭包

```
atlas_network
├── atlas_foundation   ✓ 无外部依赖
├── atlas_platform     ✓ 无外部依赖
├── atlas_serialization_binary  ✓ binary_stream
└── ZLIB::ZLIB                  ✓ compression_filter

已清掉的旧依赖:
├── machined_types 已改为直接引用基础层枚举头
├── login_messages.h 不再经 db/idatabase.h 传递 entitydef/
└── baseapp_messages.h 不再经 db/idatabase.h 传递 entitydef/
```

> **文件扩展名**: 项目头文件使用 `.h`, 源文件使用 `.cc` (见 `CLAUDE.md` /
> `.clang-format`)。以下示例保持此约定。

### 2.2 解耦落点

#### 2.2.1 ProcessType 枚举提取

历史上进程类型枚举和大量服务端配置（数据库、认证等）同处一个 server
头，但 `machined_types.h` 只用到进程类型枚举。

**当前状态**: `ProcessType` 已提取到 `foundation/process_type.h`。

```cpp
// src/lib/foundation/process_type.h
#pragma once
#include <cstdint>
#include <string_view>

namespace atlas {

enum class ProcessType : uint8_t {
  kMachined = 0,
  kLoginApp = 1,
  kBaseApp = 2,
  kBaseAppMgr = 3,
  kCellApp = 4,
  kCellAppMgr = 5,
  kDbApp = 6,
  kDbAppMgr = 7,
  kReviver = 8,
  kClient = 9,
};

[[nodiscard]] auto ProcessTypeName(ProcessType type) -> std::string_view;
[[nodiscard]] auto ProcessTypeFromName(std::string_view name) -> Result<ProcessType>;

}  // namespace atlas
```

> **命名说明**: 遵循项目 Google C++ Style（枚举值 `kPascalCase`、
> 函数 PascalCase）。`ProcessTypeName` / `ProcessTypeFromName` 保持同名，
> 调用点无需关心底层归属。

**影响**:
- 服务端配置头通过 `foundation/process_type.h` 使用该枚举
- `network/machined_types.h` 直接 include `foundation/process_type.h`
- `machined_types.h` 对 `server/` 的依赖消除

#### 2.2.2 DatabaseID 提取

`login_messages.h` 和 `baseapp_messages.h` 依赖 `db/idatabase.h` 仅为了 `DatabaseID` 类型别名。

**当前状态**: `DatabaseID` 已位于 `server/entity_types.h`（同处还有
`EntityID` 和 `SessionKey`）。

```cpp
// src/lib/server/entity_types.h
using DatabaseID = int64_t;
inline constexpr DatabaseID kInvalidDBID = 0;
```

**影响**:
- `db/idatabase.h` 改为 `#include "server/entity_types.h"` 获取 DatabaseID
- `login_messages.h` 和 `baseapp_messages.h` 移除 `#include "db/idatabase.h"`
- 消除对 `entitydef/` 的传递依赖

#### 2.2.3 客户端消息头文件

DLL 只需要客户端相关的消息定义（LoginRequest/LoginResult/Authenticate/AuthenticateResult/ClientBaseRpc/ClientCellRpc）。这些消息定义在 `login_messages.h` 和 `baseapp_messages.h` 中，但两个文件还包含大量仅服务端使用的消息。

**当前状态**: 不拆分文件。完成 2.2.1 和 2.2.2 的解耦后，这两个头文件的依赖链为：

```
login_messages.h / baseapp_messages.h
├── network/address.h      ✓ 已在 DLL 中
├── network/message.h      ✓ 已在 DLL 中
├── network/message_ids.h  ✓ 已在 DLL 中
└── server/entity_types.h  ✓ 纯类型头文件，无 .cc
```

依赖干净，可以直接 include。

### 2.3 解耦后的依赖图

```
atlas_net_client.dll
├── atlas_network        (全部 12 个 .cc)
├── atlas_platform       (全部平台相关 .cc)
├── atlas_foundation     (全部 .cc)
├── atlas_movement_sim   (owner predictor parity / input codec)
├── atlas_serialization_binary  (仅 binary_stream.cc)
├── atlas_zlib             (可选, 用于压缩过滤器)
├── server/entity_types.h  (header-only, EntityID/SessionKey/DatabaseID)
├── login_messages.h       (header-only, 登录消息定义)
├── baseapp_messages.h     (header-only, BaseApp 消息定义)
└── 系统库: ws2_32(Win), pthread(Linux)

不包含:
✗ atlas_clrscript (CLR 嵌入)
✗ atlas_server (服务端框架)
✗ atlas_entitydef (实体定义)
✗ atlas_db* (数据库)
✗ atlas_script (脚本抽象)
✗ pugixml, rapidjson (XML/JSON 解析)
```

---

## 3. CMake 构建目标

> 项目使用 CMake 3.28+ 作为唯一构建系统 (见 `CLAUDE.md` 与
> `CMakePresets.json`)。
> 本节约定对齐仓库现有惯例:
> - 所有 target 名加 `atlas_` 前缀 (例: `atlas_network`)
> - 每个库一个子目录, 含单独 `CMakeLists.txt`;
>   由 `src/lib/CMakeLists.txt` 用 `add_subdirectory` 串起
> - 头文件与 `.cc` 平铺在同一目录, 通过
>   `target_include_directories(... PUBLIC "${CMAKE_SOURCE_DIR}/src/lib")` 暴露
> - 动态库用 `add_library(name SHARED ...)`, Unix 下设置
>   `CXX_VISIBILITY_PRESET hidden` 实现精细符号导出
>   (参考 `src/lib/clrscript/CMakeLists.txt` 中 `atlas_engine` 的实现)
> - 平台分支用 `if(WIN32)` / `if(APPLE)` / `if(UNIX AND NOT APPLE)`

### 3.1 当前 Target: atlas_net_client

```cmake
# src/lib/net_client/CMakeLists.txt

# ---- 核心静态库 ----
# 编译 Login/Auth 状态机和 C API 入口;
# 与最终 SHARED 库分开, 便于单元测试直接链接 static target
add_library(atlas_net_client_core STATIC
  client_api.cc        # C API 导出层
  client_session.cc    # Login/Auth 状态机
)

target_include_directories(atlas_net_client_core
  PUBLIC "${CMAKE_SOURCE_DIR}/src/lib"
         "${CMAKE_SOURCE_DIR}/src/server"
)

target_link_libraries(atlas_net_client_core
  PUBLIC
    atlas_network
    atlas_foundation
    atlas_platform
    atlas_movement_sim
    # 关键: 只链接 binary 子集, 不拉入 pugixml / rapidjson (§3.3)
    atlas_serialization_binary
    atlas_compiler_options
)

target_precompile_headers(atlas_net_client_core REUSE_FROM atlas_foundation)

# ---- 最终共享库 ----
# 参考 src/lib/clrscript/CMakeLists.txt 的 atlas_engine SHARED 模式
add_library(atlas_net_client SHARED
  client_api.cc
  client_session.cc
)

target_include_directories(atlas_net_client
  PUBLIC "${CMAKE_SOURCE_DIR}/src/lib"
         "${CMAKE_SOURCE_DIR}/src/server"
)

target_compile_definitions(atlas_net_client
  PRIVATE  ATLAS_NET_CLIENT_EXPORTS ATLAS_NET_CLIENT_DLL
  INTERFACE ATLAS_NET_CLIENT_DLL
)

target_link_libraries(atlas_net_client
  PRIVATE
    atlas_network
    atlas_foundation
    atlas_platform
    atlas_movement_sim
    atlas_serialization_binary
    atlas_compiler_options
)

target_precompile_headers(atlas_net_client REUSE_FROM atlas_foundation)

if(UNIX)
  set_target_properties(atlas_net_client PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
  )
  target_link_options(atlas_net_client PRIVATE
    $<$<PLATFORM_ID:Linux>:-Wl,--exclude-libs,ALL>
    $<$<PLATFORM_ID:Darwin>:-Wl,-dead_strip>
  )
endif()

# 输出名: atlas_net_client.dll (Win), libatlas_net_client.so (Linux/Android),
#        atlas_net_client.bundle (macOS, Unity Plugin 标准格式)
set_target_properties(atlas_net_client PROPERTIES
  OUTPUT_NAME "atlas_net_client"
)
if(APPLE AND NOT IOS)
  # macOS Unity Plugin uses the .bundle extension, though the target is a dylib.
  set_target_properties(atlas_net_client PROPERTIES
    SUFFIX ".bundle"
    PREFIX ""           # 不要 lib 前缀, Unity 按 LibName 精确查找
  )
endif()

# ---- iOS 静态库变体 ----
# Apple 禁止第三方动态库, Unity iOS 构建走 [DllImport("__Internal")]
# 仅在 iOS 目标构建时定义, 避免污染桌面构建
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  add_library(atlas_net_client_static STATIC
    client_api.cc
    client_session.cc
  )
  target_include_directories(atlas_net_client_static
    PUBLIC "${CMAKE_SOURCE_DIR}/src/lib"
           "${CMAKE_SOURCE_DIR}/src/server"
  )
  target_compile_definitions(atlas_net_client_static
    PRIVATE ATLAS_NET_CLIENT_EXPORTS
  )
  target_link_libraries(atlas_net_client_static
    PUBLIC atlas_network atlas_foundation atlas_platform atlas_movement_sim
           atlas_serialization_binary atlas_compiler_options
  )
endif()

atlas_set_output_dir("" atlas_net_client)
```

`src/lib/CMakeLists.txt` 已注册:

```cmake
add_subdirectory(net_client)
```

### 3.2 构建开关

CMake 原生 `option()` 即可表达 "该 target 是否默认构建":

```cmake
# 顶层 CMakeLists.txt (或 src/lib/net_client/CMakeLists.txt 开头)
option(ATLAS_BUILD_NET_CLIENT "Build Unity native client DLL" OFF)

if(ATLAS_BUILD_NET_CLIENT)
  # 上面的 add_library(atlas_net_client SHARED ...) 仅在开关开启时生效
endif()
```

配置命令:
```bash
# Windows
tools\bin\build.bat release --config-only
cmake -S . -B build/release -DATLAS_BUILD_NET_CLIENT=ON
cmake --build build/release --target atlas_net_client --config Release

# Linux / macOS
tools/bin/build.sh release --config-only
cmake -S . -B build/release -DATLAS_BUILD_NET_CLIENT=ON
cmake --build build/release --target atlas_net_client --config Release
```

`atlas_net_client` 由 `ATLAS_BUILD_NET_CLIENT` 控制（默认 `OFF`，服务端
CI 矩阵不会误构建）。UE / 外部客户端还会随 SDK 构建
`atlas_entitydef_client`；其开关是 `ATLAS_BUILD_ENTITYDEF_CLIENT`，默认跟随
`ATLAS_BUILD_NET_CLIENT`。`atlas_network` 的 compression filter 随网络库一起编译，
当前没有 `ATLAS_NET_CLIENT_COMPRESSION` 这类 net-client 专属 CMake option。

### 3.3 序列化模块拆分

当前 `src/lib/serialization/CMakeLists.txt` 已把 binary stream 拆为
`atlas_serialization_binary`;完整 `atlas_serialization` 复用该 target 并额外链接
`pugixml` / `rapidjson`。Unity 客户端 DLL 只链接 binary 子集。

```cmake
add_library(atlas_serialization_binary STATIC
  binary_stream.cc
)

target_include_directories(atlas_serialization_binary
  PUBLIC "${CMAKE_SOURCE_DIR}/src/lib"
)

target_link_libraries(atlas_serialization_binary
  PUBLIC atlas_foundation atlas_platform atlas_compiler_options
)

add_library(atlas_serialization STATIC
  data_section.cc
  json_parser.cc
  xml_parser.cc
)

target_include_directories(atlas_serialization
  PUBLIC "${CMAKE_SOURCE_DIR}/src/lib"
)

target_link_libraries(atlas_serialization
  PUBLIC atlas_serialization_binary pugixml rapidjson
)
```

#### 3.3.1 传递依赖闭包

`src/lib/network/CMakeLists.txt` 也已改为只公开 `atlas_serialization_binary`：

```cmake
target_link_libraries(atlas_network
  PUBLIC atlas_foundation atlas_platform atlas_serialization_binary
         ZLIB::ZLIB atlas_compiler_options
)
```

`atlas_net_client_core` 和最终 `atlas_net_client` 在此基础上显式链接
`atlas_serialization_binary`，因此 DLL 链接闭包不会经 `atlas_network` 带入
`pugixml` / `rapidjson`。

### 3.4 目录结构

```
src/lib/net_client/
├── CMakeLists.txt
├── net_client_export.h     # ATLAS_NET_API 宏 (§4.1)
├── client_api.h            # C API 声明 (§4.2-4.9)
├── client_api.cc           # C API 实现
├── client_session.h        # Login/Auth 状态机声明 (§5.2)
└── client_session.cc       # 状态机实现
```

(与仓库现有约定一致: `.h` 与 `.cc` 平铺同目录, 不做 `include/` 分层)

对应测试:

```cmake
atlas_add_test(NAME test_net_client_abi_layout
  SOURCES test_net_client_abi_layout.cpp
  DEPS atlas_net_client_core
)

atlas_add_test(NAME test_client_flow
  LABEL integration
  SOURCES test_client_flow.cpp
  DEPS atlas_net_client_core atlas_fake_cluster
)
```

C# 侧 `tests/csharp/Atlas.Client.Tests/ClientSessionTests.cs` 覆盖 `on_deliver`
之后的消息解码、实体生命周期、RPC dispatch 与 space-data 路径。

---

## 4. C API 定义

### 4.0 ABI 约定 (Conventions)

以下契约对全部 C API 函数和回调生效。所有实现与调用者必须遵守；违反将导致未定义行为。

#### 4.0.1 String / Buffer 所有权

所有跨 FFI 的 `const char*` / `const uint8_t*` / `const void*` 均为
**非拥有视图 (non-owning view)**：

**C++ → C# 方向** (回调参数、`AtlasNetLastError` 返回值)
- 指针在**回调/函数返回后立即失效**
- C# 必须在回调内同步复制数据到托管堆
- 不得保存裸指针、不得跨 `AtlasNetPoll()` 周期使用

```csharp
// Pattern B (当前)；未来 runtime 切到 Pattern A 时，仅 attribute 改名。
[MonoPInvokeCallback(typeof(RpcFn))]
static void OnRpc(nint ctx, uint entId, uint rpcId, byte* payload, int len) {
    // ✓ 立即复制
    byte[] copy = new Span<byte>(payload, len).ToArray();
    RpcQueue.Enqueue((entId, rpcId, copy));
    // ✗ 禁止: _savedPtr = payload;
}
```

**C# → C++ 方向** (`AtlasNetSend*`, `AtlasNetLogin` 等入参)
- C# 必须在调用期间保持 pinned，调用返回即可释放
- C++ 必须在函数返回前完成数据复制 (拷入 Bundle 或 `std::string`)

#### 4.0.2 Error 字符串生命周期

- `AtlasNetLastError(ctx)` 返回指针指向 ctx 内部 `std::string`
- 有效期至下一次作用于**同一 ctx** 的任何 API 调用之前
- `AtlasNetGlobalLastError()` (无 ctx 情形) 使用线程局部存储，
  有效期至本线程下一次 Atlas 调用之前

#### 4.0.3 回调可重入性

- 回调内**允许**调用 `AtlasNetSend*`, `AtlasNetGetState`,
  `AtlasNetGetStats`, `AtlasNetLastError`
- 回调内**禁止**调用 `AtlasNetCreate`, `AtlasNetDestroy`,
  `AtlasNetPoll`, `AtlasNetDisconnect`, `AtlasNetLogin`,
  `AtlasNetAuthenticate`, `AtlasNetSetCallbacks`
- `ATLAS_DEBUG` 构建下，禁用组合触发 assert；Release 下返回 `-EBUSY`

#### 4.0.4 Callback Table 初始化

- `AtlasNetCreate` 成功后,ctx 内置 noop 回调表;未注册即收消息不会 null 解引用。
- `AtlasNetCallbacks` 当前只有 `on_disconnect` 和 `on_deliver` 两个槽位。
- `AtlasNetSetCallbacks` 复制传入表;字段允许为 `NULL`,DLL 入口处用内部 noop
  替换后保存。
- 回调在 `AtlasNetPoll(ctx)` 线程同步触发;回调内调用
  `AtlasNetSetCallbacks` 仍按 §4.0.3 禁止。
- **所有回调首参均为 `AtlasNetContext* ctx`** — 这是能支持"多 ctx 并发"
  (§4.0.6) 的唯一路径: C# 侧用静态 `Dictionary<nint,IAtlasNetEvents>` 在
  ctx 指针 → 宿主实例之间做映射

> **当前 C ABI 边界:** DLL 只保留断线通知和单一 `on_deliver`;AOI / RPC
> 等服务端下行消息的具体解码由 `Atlas.Client.ClientSession` 完成。

#### 4.0.5 ABI 版本

```cpp
// 每次 ABI-breaking 变更递增；布局: [MAJOR:8][MINOR:8][PATCH:16]
#define ATLAS_NET_ABI_VERSION 0x02050000u   // v2.5.0
```

版本号变更规则:

| 变更类型 | bump | 行为 |
|----------|------|------|
| 函数签名变更 / 结构体布局变更 / 回调签名变更 | MAJOR | C# 必须同步发版 |
| 新增函数 / 新增回调 (结构体尾追加字段不算) | MINOR | 向后兼容，C# 可延后升级 |
| 仅实现修复，不改 ABI | PATCH | 不校验 |

**校验点**: `AtlasNetCreate(expected_abi)` 执行
`caller_major != our_major || caller_minor > our_minor` 检查，
失败返回 NULL 并通过 `AtlasNetGlobalLastError()` 暴露原因。

#### 4.0.6 线程模型

- 每个 `AtlasNetContext` 仅允许被**单一线程**访问（Unity 主线程）
- 多 ctx 并发允许，但不共享任何状态
- 日志 sink (`AtlasNetSetLogHandler`) 是**进程级**全局状态，
  线程安全由 `atlas_foundation` Logger 保证

#### 4.0.7 返回码约定

所有返回 `int32_t` 的 API 遵循:

| 码 | 含义 |
|----|------|
| `0` | 成功 |
| `-EBUSY` (-16) | 状态不允许此调用 |
| `-ENOCONN` (-107) | 无可用连接 |
| `-EINVAL` (-22) | 参数非法 |
| `-ENOMEM` (-12) | 内存分配失败 |
| `-EABI` (-1000, 自定义) | ABI 版本不匹配 (仅 `AtlasNetCreate`) |

### 4.1 导出宏

```cpp
// src/lib/net_client/net_client_export.h

#pragma once

// 使用 _WIN32 而非 ATLAS_PLATFORM_WINDOWS: 让头文件自包含,
// 不依赖上游 build flag (Unity C# 侧的 P/Invoke header gen 会引用它)
#if defined(_WIN32)
#ifdef ATLAS_NET_CLIENT_EXPORTS
#define ATLAS_NET_API __declspec(dllexport)
#else
#define ATLAS_NET_API __declspec(dllimport)
#endif
#else
#define ATLAS_NET_API __attribute__((visibility("default")))
#endif

#define ATLAS_NET_CALL extern "C" ATLAS_NET_API
```

### 4.2 句柄与基础类型

```cpp
// 不透明句柄
typedef struct AtlasNetContext AtlasNetContext;

// ABI 版本查询 (C# 诊断用, 不用于校验)
ATLAS_NET_CALL uint32_t AtlasNetGetAbiVersion(void);

// 错误信息
ATLAS_NET_CALL const char* AtlasNetLastError(AtlasNetContext* ctx);

// 无 ctx 时的错误信息 (AtlasNetCreate 失败时使用, 线程局部)
ATLAS_NET_CALL const char* AtlasNetGlobalLastError(void);
```

### 4.3 生命周期

```cpp
// 创建网络上下文 (内部创建 EventDispatcher + NetworkInterface)。
// expected_abi: 调用方编译时的 ATLAS_NET_ABI_VERSION,
// 与 DLL 内部版本按 §4.0.5 规则校验,失败返回 NULL。
//   - NULL 返回时用 AtlasNetGlobalLastError() 取原因
//   - 成功返回时 ctx 已安装 noop 回调表,立即可安全使用
ATLAS_NET_CALL AtlasNetContext* AtlasNetCreate(uint32_t expected_abi);

// 销毁 (断开所有连接, 清零 SessionKey, 释放资源)
ATLAS_NET_CALL void AtlasNetDestroy(AtlasNetContext* ctx);
```

**C# 使用范式**:

```csharp
public static class AtlasNet {
    const uint kExpectedAbi = 0x02050000u;  // 与 C 头文件同步

    public static nint Create() {
        var ctx = AtlasNetNative.AtlasNetCreate(kExpectedAbi);
        if (ctx == 0) {
            var err = Marshal.PtrToStringUTF8(
                AtlasNetNative.AtlasNetGlobalLastError());
            throw new InvalidOperationException(
                $"atlas_net_client DLL 初始化失败: {err ?? "unknown"}");
        }
        return ctx;
    }
}
```

### 4.4 Tick (C# 驱动)

```cpp
// 驱动一次网络事件循环
// - 处理所有待处理的 IO 事件
// - 触发到期的定时器 (重传、心跳等)
// - 触发已注册的回调 (RPC 分发、实体创建等)
// 返回处理的事件数, -1 表示错误
ATLAS_NET_CALL int32_t AtlasNetPoll(AtlasNetContext* ctx);
```

Unity 侧用法:
```csharp
void Update() {
    AtlasNet.Poll(_ctx);  // 每帧调用, 驱动整个网络层
}
```

### 4.5 Login / Authenticate / Disconnect (高层封装)

> **重要设计决定**: `SessionKey` 永不跨 FFI 边界。DLL 在收到 `LoginResult`
> 时将 SessionKey 和 BaseApp 地址缓存在 `ClientSession` 内部，
> `AtlasNetAuthenticate` 无需由 C# 回传。这一原则:
> - 缩小 SessionKey 的暴露面 (托管堆扫描、调试器、core dump 均无法看到)
> - 消除两侧状态同步负担
> - 简化 API (authenticate 从 4 参降为 2 参)

#### 4.5.1 回调类型

```cpp
// ---- 登录结果回调 ----
// status 枚举: 见 AtlasLoginStatus
// baseapp_host / baseapp_port: 仅供 UI 展示,不需回传给 authenticate
// user_data: AtlasNetLogin 时传入的用户指针,回调原样带回
//            (避免 C# 用静态字段存 MonoBehaviour 句柄)
typedef void (*AtlasLoginResultFn)(
    void*       user_data,
    uint8_t     status,
    const char* baseapp_host,     // 仅当 status==kSuccess 时有效; view 语义
    uint16_t    baseapp_port,
    const char* error_message     // UTF-8, len 由 \0 确定; view 语义
);

typedef enum {
    ATLAS_LOGIN_SUCCESS              = 0,
    ATLAS_LOGIN_INVALID_CREDENTIALS  = 1,
    ATLAS_LOGIN_ALREADY_LOGGED_IN    = 2,
    ATLAS_LOGIN_SERVER_FULL          = 3,
    ATLAS_LOGIN_TIMEOUT              = 4,
    ATLAS_LOGIN_NETWORK_ERROR        = 5,
    ATLAS_LOGIN_DEF_MISMATCH         = 6,
    ATLAS_LOGIN_INTERNAL_ERROR       = 255,
} AtlasLoginStatus;

// ---- 认证结果回调 ----
// 成功时 entity_id / type_id 有效,失败时二者为 0
typedef void (*AtlasAuthResultFn)(
    void*       user_data,
    uint8_t     success,          // 1=成功, 0=失败
    uint32_t    entity_id,
    uint16_t    type_id,
    const char* error_message     // view 语义
);
```

#### 4.5.2 登录 (Login)

```cpp
// 发起登录。异步,结果通过 callback 回传。
// 内部流程:
//   1. 状态机检查 (仅 Disconnected 允许)
//   2. connect_rudp(loginapp_host, loginapp_port) → kLoggingIn
//   3. 构造并发送 LoginRequest (username + password_hash)
//   4. 注册 typed handler <login::LoginResult>
//   5. AtlasNetPoll() 驱动,收到 LoginResult 后:
//      - 解析 SessionKey 和 baseapp_addr,存入 ClientSession 私有字段
//      - 关闭 LoginApp 连接 (已完成使命)
//      - 状态转 kLoginSucceeded
//      - 调用 callback(user_data, status, baseapp_host, baseapp_port, err)
//
// 返回值:
//   0        : 已发起 (结果稍后回调)
//   -EBUSY   : 状态不允许
//   -EINVAL  : 参数非法 (host/port 为空等)
//   -ENOMEM  : 分配失败
ATLAS_NET_CALL int32_t AtlasNetLogin(
    AtlasNetContext*    ctx,
    const char*         loginapp_host,
    uint16_t            loginapp_port,
    const char*         username,
    const char*         password_hash,
    AtlasLoginResultFn  callback,
    void*               user_data        // 透传回 callback, 可为 NULL
);

// 设置 32-byte entity-def SHA-256 digest；随 LoginRequest 发送。
// 应在 AtlasNetLogin 前调用，传入 Atlas.Rpc.EntityDefDigest.Bytes。
ATLAS_NET_CALL int32_t AtlasNetSetEntityDefDigest(
    AtlasNetContext* ctx,
    const uint8_t* data,
    int32_t len
);
```

#### 4.5.3 认证 (Authenticate)

```cpp
// 发起认证。必须在 LoginResult(kSuccess) 回调触发之后调用
// (典型: 在 login 回调内直接调用 authenticate)。
//
// 内部流程:
//   1. 状态机检查 (仅 kLoginSucceeded 允许)
//   2. 从私有字段取 baseapp_addr, connect_rudp() → kAuthenticating
//   3. 构造 Authenticate 消息 (使用私有 session_key_),发送
//   4. 注册 typed handler <baseapp::AuthenticateResult>
//   5. 收到结果后:
//      - 成功: 保存 entity_id/type_id, 状态转 kConnected,
//              安装 RPC default handler, 回调 success=1
//      - 失败: ClearSessionKey(), 状态转 kDisconnected, 回调 success=0
//
// 注意: SessionKey 不作为参数 — DLL 自持 (§5.2.1)
ATLAS_NET_CALL int32_t AtlasNetAuthenticate(
    AtlasNetContext*   ctx,
    AtlasAuthResultFn  callback,
    void*              user_data         // 透传回 callback, 可为 NULL
);
```

#### 4.5.4 断开连接 (Disconnect)

```cpp
typedef enum {
    ATLAS_DISCONNECT_USER      = 0,  // 主动退出/正常登出
    ATLAS_DISCONNECT_LOGOUT    = 1,  // 准备换账号 (触发 on_disconnect 回调)
    ATLAS_DISCONNECT_INTERNAL  = 2,  // 保留: DLL 内部因错触发 (通常不由用户调)
} AtlasDisconnectReason;

// 关闭当前连接,保留 ctx 和回调表。状态回 Disconnected。
// 幂等: 已断开时再调用返回 0。
// 行为:
//   - 关闭所有活跃 channel (loginapp, baseapp)
//   - ClearSessionKey() 清零敏感状态
//   - 反注册 typed handlers
//   - 如果 reason == LOGOUT,触发 on_disconnect 回调通知上层
//   - 如果 reason == USER,不触发回调 (用户显式退出已知状态)
//
// 注意: AtlasDisconnectReason 是输入枚举; on_disconnect 的 reason 使用事件原因域。
// 调用后允许重新 AtlasNetLogin() 用新凭证登录。
ATLAS_NET_CALL int32_t AtlasNetDisconnect(
    AtlasNetContext*      ctx,
    AtlasDisconnectReason reason);

// 测试用 RUDP 链路损伤。配置会应用到当前 LoginApp/BaseApp channel，
// 也会保留给后续重连创建的新 channel。
ATLAS_NET_CALL int32_t AtlasNetSetTransportImpairment(
    AtlasNetContext* ctx,
    uint32_t one_way_latency_ms,
    uint32_t loss_permyriad,
    uint32_t seed);
```

#### 4.5.5 状态查询

```cpp
typedef enum {
    ATLAS_NET_STATE_DISCONNECTED      = 0,
    ATLAS_NET_STATE_LOGGING_IN        = 1,
    ATLAS_NET_STATE_LOGIN_SUCCEEDED   = 2,   // 已收 LoginResult,等待 authenticate 调用
    ATLAS_NET_STATE_AUTHENTICATING    = 3,
    ATLAS_NET_STATE_CONNECTED         = 4,
} AtlasNetState;

ATLAS_NET_CALL AtlasNetState AtlasNetGetState(AtlasNetContext* ctx);
```

#### 4.5.6 状态转换矩阵

所有 C API 入口**首行**检查 state。非法调用**不**隐式断开、不改状态，
仅返回错误码 (`ATLAS_DEBUG` 下额外 log warn)，保持状态机确定性。

| From \ 调用        | Login    | Authenticate | Send*Rpc  | Disconnect | Destroy |
|-------------------|----------|--------------|-----------|------------|---------|
| Disconnected      | ✓        | `-EBUSY`     | `-ENOCONN`| ✓ (noop)   | ✓       |
| LoggingIn         | `-EBUSY` | `-EBUSY`     | `-ENOCONN`| ✓          | ✓       |
| LoginSucceeded    | `-EBUSY` | ✓            | `-ENOCONN`| ✓          | ✓       |
| Authenticating    | `-EBUSY` | `-EBUSY`     | `-ENOCONN`| ✓          | ✓       |
| Connected         | `-EBUSY` | `-EBUSY`     | ✓         | ✓          | ✓       |

#### 4.5.7 状态机图

```
       ┌─────────────── Disconnected ◄──────────────────┐
       │                     │                          │
       │                AtlasNetLogin()                 │
       │                     ▼                          │
       │                LoggingIn                       │
       │                     │                          │
       │          LoginResult│received                  │
       │           ┌─────────┴──────────┐               │
       │      kSuccess               fail/timeout       │
       │           │                    │               │
       │           ▼                    └──────►────────┤
       │     LoginSucceeded                             │
       │           │                                    │
       │   AtlasNetAuthenticate()                       │
       │           ▼                                    │
       │     Authenticating                             │
       │           │                                    │
       │  AuthResult received                           │
       │      ┌────┴─────┐                              │
       │  success=1    success=0                        │
       │      │           │                             │
       │      ▼           └───────►─────────────────────┤
       │   Connected                                    │
       │      │                                         │
       │   AtlasNetDisconnect() / error / server close  │
       │      │                                         │
       └──────┘                                         │
                                                        │
  (任何状态) AtlasNetDisconnect() ────────────────────┘
```

### 4.6 消息发送

```cpp
// 发送 Base RPC (通过已认证的 BaseApp 连接)
// rpc_id: 由 SourceGenerator 生成的 RPC 标识
// payload: SpanWriter 序列化后的参数数据
ATLAS_NET_CALL int32_t AtlasNetSendBaseRpc(
    AtlasNetContext* ctx,
    uint32_t entity_id,
    uint32_t rpc_id,
    const uint8_t* payload,
    int32_t payload_len
);

// 发送 Cell RPC (通过 BaseApp 转发到 CellApp)
ATLAS_NET_CALL int32_t AtlasNetSendCellRpc(
    AtlasNetContext* ctx,
    uint32_t entity_id,
    uint32_t rpc_id,
    const uint8_t* payload,
    int32_t payload_len
);

#pragma pack(push, 1)
typedef struct AtlasMovementInputFrame {
    uint32_t seq;
    uint32_t input_tick;
    int8_t move_x;
    int8_t move_z;
    uint16_t view_yaw;
    int8_t view_pitch;
    uint16_t buttons;
    uint16_t client_dt_ms;
} AtlasMovementInputFrame;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct AtlasMovementStateFrame {
    float position_x;
    float position_y;
    float position_z;
    float velocity_x;
    float velocity_y;
    float velocity_z;
    float direction_x;
    float direction_y;
    float direction_z;
    uint32_t flags;
    uint32_t last_processed_input_seq;
} AtlasMovementStateFrame;
#pragma pack(pop)

typedef enum AtlasMovementCorrectionTier {
    ATLAS_MOVEMENT_CORRECTION_NONE = 0,
    ATLAS_MOVEMENT_CORRECTION_TIER1 = 1,
    ATLAS_MOVEMENT_CORRECTION_TIER2 = 2,
    ATLAS_MOVEMENT_CORRECTION_SNAP = 3,
} AtlasMovementCorrectionTier;

ATLAS_NET_CALL int32_t AtlasNetSendMovementInput(
    AtlasNetContext* ctx,
    uint32_t target_entity_id,
    const AtlasMovementInputFrame* frames,
    int32_t frame_count
);

ATLAS_NET_CALL int32_t AtlasNetSendMovementCorrectionReport(
    AtlasNetContext* ctx,
    uint32_t target_entity_id,
    uint32_t acked_input_seq,
    uint32_t server_tick,
    float distance_m,
    uint16_t correction_flags
);

ATLAS_NET_CALL int32_t AtlasNetMovementPredictStep(
    const AtlasMovementStateFrame* previous,
    const AtlasMovementInputFrame* input,
    uint32_t server_tick,
    AtlasMovementStateFrame* out_state
);

ATLAS_NET_CALL AtlasMovementCorrectionTier AtlasNetMovementClassifyCorrection(
    float distance_m
);

ATLAS_NET_CALL uint16_t AtlasNetMovementCorrectionFlag(float distance_m);
```

> **非-RPC 业务消息的发送路径**: 高频或协议级上行不复用普通 RPC 语义。
> Movement input 通过 `AtlasNetSendMovementInput` 发送固定布局帧,由 BaseApp
> 做认证实体校验和限流后转发到 CellApp。owner 本地预测通过
> `AtlasNetMovementPredictStep` 复用同一份 `movement_sim` flat query。ack
> replay 后的 correction report 只进入 BaseApp watcher 和可疑升级。普通
> exposed 方法仍走 `AtlasNetSendBaseRpc` / `AtlasNetSendCellRpc`。

### 4.7 消息接收回调

DLL 是纯传输层。除了一次性的连接断开通知, 所有服务端下行消息（AoI 信封
0xF001 / 0xF002 / 0xF003、RPC 信封 0xF004、movement ack / command
0xF005 / 0xF006 / 0xF007,以及 login/auth typed 消息以外的任何 wire id）
都经单一 `on_deliver` 把原始 payload 透传到 C#, 由
`Atlas.Client.ClientSession.DeliverFromServer` 完成解码。

> **设计约束**:
> - 每个回调首参是 `AtlasNetContext* ctx` —— Pattern B 的
>   `[MonoPInvokeCallback]` 与 Pattern A 的 `[UnmanagedCallersOnly]`
>   都无法闭包 this, 没有 ctx 就无法从回调路由回 C# 实例。
> - payload 指针生命周期只到回调返回 (§4.0.1)。
> - 全部在 `AtlasNetPoll(ctx)` 线程同步触发, 不跨线程。
> - NULL 字段在 `AtlasNetSetCallbacks` 内部替换成 noop, 以便测试 / 部分
>   宿主只关心子集时不必显式填 sentinel。

```cpp
// ============================================================================
// 回调注册 (在 AtlasNetCreate 之后, login 之前调用)
// ============================================================================

// 服务器主动关闭或 DLL 检测到断线 (USER-initiated disconnect 不触发此回调)
// reason: 0=服务端关闭, 1=超时, 2=网络错误, 3=LoggedOff (服务端踢下线)
typedef void (*AtlasDisconnectFn)(AtlasNetContext* ctx, int32_t reason);

// 服务端下行的所有非 login/auth 消息: msg_id 是原始 wire id
// (0xF001/0xF002/0xF003 = AoI envelope, 0xF004 = RPC envelope,
// 0xF005/0xF006/0xF007 = movement ack/command)。C# 侧 switch 后再解码。
typedef void (*AtlasDeliverFromServerFn)(
    AtlasNetContext* ctx,
    uint16_t msg_id,
    const uint8_t* payload, int32_t len);

#pragma pack(push, 1)
typedef struct {
    AtlasDisconnectFn          on_disconnect;
    AtlasDeliverFromServerFn   on_deliver;
} AtlasNetCallbacks;
#pragma pack(pop)

// 原子替换。返回码:
//   0        : 成功
//   -EINVAL  : callbacks 为 NULL
//   -EBUSY   : 在回调内调用 (§4.0.3 禁止)
ATLAS_NET_CALL int32_t AtlasNetSetCallbacks(
    AtlasNetContext* ctx,
    const AtlasNetCallbacks* callbacks);
```

### 4.8 日志

```cpp
// 日志回调 (DLL 内部日志转发到 Unity Debug.Log)
typedef void (*AtlasLogFn)(
    int32_t level,      // 0=Trace, 1=Debug, 2=Info, 3=Warning, 4=Error, 5=Critical
    const char* message,
    int32_t message_len
);

ATLAS_NET_CALL void AtlasNetSetLogHandler(AtlasLogFn handler);
```

### 4.9 诊断

```cpp
// 网络统计信息
#pragma pack(push, 1)
typedef struct {
    uint32_t rtt_ms;          // 当前 RTT (毫秒)
    uint32_t bytes_sent;      // 累计发送字节
    uint32_t bytes_recv;      // 累计接收字节
    uint32_t packets_lost;    // 累计丢包数
    uint32_t send_queue_size; // 待发送队列大小
    float    loss_rate;       // 丢包率 (0.0~1.0)
} AtlasNetStats;
#pragma pack(pop)

ATLAS_NET_CALL int32_t AtlasNetGetStats(
    AtlasNetContext* ctx,
    AtlasNetStats* out_stats
);
```

### 4.10 完整 API 一览

| 函数 | 方向 | 用途 |
|------|------|------|
| `AtlasNetGetAbiVersion` | - | ABI 版本查询 (诊断用) |
| `AtlasNetLastError` | - | 获取 ctx 内最后错误 (view 语义,§4.0.2) |
| `AtlasNetGlobalLastError` | - | 无 ctx 时的错误 (TLS,仅用于 create 失败后) |
| `AtlasNetCreate` | C#→C++ | 创建网络上下文 (带 ABI 校验,§4.0.5) |
| `AtlasNetDestroy` | C#→C++ | 销毁网络上下文 |
| `AtlasNetPoll` | C#→C++ | **每帧调用, 驱动网络层** |
| `AtlasNetSetCallbacks` | C#→C++ | 注册事件回调并填充 noop |
| `AtlasNetSetLogHandler` | C#→C++ | 注册日志回调 (进程级) |
| `AtlasNetLogin` | C#→C++ | 发起登录 (带 user_data) |
| `AtlasNetSetEntityDefDigest` | C#→C++ | 设置 32-byte entity-def digest，随登录请求发送 |
| `AtlasNetAuthenticate` | C#→C++ | 发起认证 (无 host/key 参数,DLL 自持) |
| `AtlasNetDisconnect` | C#→C++ | 关闭连接,保留 ctx 和回调表 |
| `AtlasNetSetTransportImpairment` | C#→C++ | 测试用 RUDP 延迟 / 丢包注入 |
| `AtlasNetGetState` | C#→C++ | 查询连接状态 (含 LoginSucceeded) |
| `AtlasNetSendBaseRpc` | C#→C++ | 发送 Base RPC |
| `AtlasNetSendCellRpc` | C#→C++ | 发送 Cell RPC |
| `AtlasNetSendMovementInput` | C#→C++ | 发送玩家预测输入帧 |
| `AtlasNetSendMovementCorrectionReport` | C#→C++ | 上报 owner replay 后的纠正等级 |
| `AtlasNetMovementPredictStep` | C#→C++ | 使用共享 movement_sim 推进本地预测 |
| `AtlasNetMovementClassifyCorrection` | C API utility | 按距离计算 owner correction tier；托管侧当前用 `MovementCorrection.Classify` |
| `AtlasNetMovementCorrectionFlag` | C API utility | 按距离生成 movement correction flag；托管侧当前用 `MovementCorrection.FlagFor` |
| `AtlasNetGetStats` | C#→C++ | 获取网络统计 |
| `AtlasLoginResultFn` | C++→C# | 登录结果通知 (user_data + status + baseapp addr) |
| `AtlasAuthResultFn` | C++→C# | 认证结果通知 (user_data + entity_id + type_id) |
| `AtlasDisconnectFn` | C++→C# | **(ctx, reason)** 服务器关闭/超时/踢下线 |
| `AtlasDeliverFromServerFn` | C++→C# | **(ctx, msg_id, payload, len)** 所有非 login/auth 下行消息;C# `ClientSession.DeliverFromServer` 解码 |
| `AtlasLogFn` | C++→C# | 日志转发 |

---

## 5. DLL 内部实现

### 5.1 AtlasNetContext 结构

```
AtlasNetContext (不透明, C++ 内部)
└── atlas::net_client::ClientSession          // `AtlasNetContext` 直接继承
    ├── EventDispatcher dispatcher_{"net_client"}
    ├── NetworkInterface network_{dispatcher_}
    ├── state_: AtlasNetState (含 kLoginSucceeded)
    ├── loginapp_channel_: Channel*           // 仅 LoggingIn 期间非空
    ├── baseapp_channel_: Channel*            // Authenticating 后非空
    ├── session_key_: std::array<uint8_t,32>  // DLL 内部持有, 不暴露
    ├── baseapp_addr_: Address                // LoginResult 后缓存
    ├── player_entity_id_: EntityID
    ├── player_type_id_: uint16_t
    ├── login_callback_: AtlasLoginResultFn + user_data
    ├── auth_callback_:  AtlasAuthResultFn  + user_data
    ├── callbacks_: AtlasNetCallbacks         // NULL 字段替换为 noop
    ├── entity_def_digest_: std::array<uint8_t,32>
    └── last_error_: std::string              // ctx 内最后错误信息
```

日志/全局错误不在 ctx 内:
- `g_log_handler`: 进程级 `std::atomic<AtlasLogFn>` (§4.8)
- `global_last_error`: `thread_local std::string` (仅 create 失败路径写入)

### 5.2 ClientSession 状态机

这里的 `ClientSession` 指 C++ native session,位置为
`src/lib/net_client/client_session.{h,cc}`;托管侧另有
`Atlas.Client.ClientSession` 负责实体、space-data 和 RPC 解码。

核心逻辑沿用桌面 client 的登录 / 认证语义（参考
`src/client/client_app.cc` 的 `Login()` 和 `Authenticate()`），但 DLL 版本改为
异步状态机：

- `login()`:
  1. 状态检查 (仅 Disconnected 允许), 失败返回 `-EBUSY`
  2. `network_.ConnectRudp(loginapp_addr)` → 保存到 `loginapp_channel_`
  3. 构造 `login::LoginRequest`, `SendMessage()`
  4. `RegisterTypedHandler<login::LoginResult>(on_login_result)`
  5. 状态转 `kLoggingIn`
  6. 返回 0 (异步, 结果在 on_login_result 中回调)
- `on_login_result()`:
  - `memcpy(session_key_.data(), msg.session_key.bytes, 32)` — 存私有字段
  - `baseapp_addr_ = msg.baseapp_addr` — 缓存
  - `loginapp_channel_->Close(); loginapp_channel_ = nullptr` — 关闭 LoginApp 连接
  - `UnregisterTypedHandler<login::LoginResult>()` — 清理
  - 状态转 `kLoginSucceeded`
  - `login_callback_(user_data, status, ip_str, port, err)`
- `authenticate()`:
  1. 状态检查 (仅 `kLoginSucceeded` 允许)
  2. `network_.ConnectRudp(baseapp_addr_)` → `baseapp_channel_`
  3. 构造 `baseapp::Authenticate{session_key: session_key_}`, 发送
  4. `RegisterTypedHandler<baseapp::AuthenticateResult>(on_auth_result)`
  5. 状态转 `kAuthenticating`
- `on_auth_result()`:
  - 成功: 保存 entity_id/type_id, 状态转 `kConnected`,
    安装 `SetDefaultHandler` 用于 RPC 捕获, 回调 `success=1`
  - 失败: `ClearSessionKey()`, 关闭 channel, 状态回 `kDisconnected`, 回调 `success=0`
  - 无论成败: `UnregisterTypedHandler<baseapp::AuthenticateResult>()`

#### 5.2.1 SessionKey 持有规则

- **产生**: `on_login_result()` 从 payload 中将 32 字节 SessionKey
  `memcpy` 到 `session_key_`（`std::array<uint8_t, 32>`）
- **使用**: `authenticate()` 构造 `baseapp::Authenticate` 消息时从
  `session_key_` 复制进消息
- **销毁时机**:
  - `on_auth_result(success=false)` → 立即 `ClearSessionKey()`
  - `AtlasNetDisconnect()` → `ClearSessionKey()`
  - `AtlasNetDestroy()` → 随 ctx 析构自动清零
  - 成功登录后保留，为未来的断线重连预留
- **禁止跨 FFI**: SessionKey 永不穿越 C API; C# 不感知其存在
- **内存**: 析构与显式清零都通过 `SecureZero` 覆写 `session_key_`,
  防止普通优化路径移除清零操作

当前实现把 `SecureZero` 放在 `client_session.cc` 的匿名 namespace,用
`volatile unsigned char*` 覆写 `session_key_`;析构、认证失败、超时、
disconnect 都会调用 `ClearSessionKey()`。

#### 5.2.2 Callback Table 安装

`AtlasNetCreate` 在返回前安装一份 noop 表;`AtlasNetSetCallbacks` 复制
调用者传入的 `AtlasNetCallbacks`,并把 NULL 字段替换成内部 noop。
回调表仍遵循每 ctx 单线程 owner 约束,不支持在回调栈内重入改表。

### 5.3 消息分发流程

native `ClientSession` 在 `kConnected` 后注册 default handler, 把大多数
wire id 原样投到 `on_deliver`。Login / Auth typed 消息由一次性 handler
拦截;`EntityTransferred` / `CellReady` 因为固定长度 body 使用 typed handler
重包后投递到 `on_deliver`;`AtlasNetDisconnect(LOGOUT)` 映射到
`on_disconnect(reason=3)`，这里的 `3` 是 logged-off 事件原因，不是
`AtlasDisconnectReason.LOGOUT` 的枚举值。

```
AtlasNetPoll(ctx)
  └── EventDispatcher::ProcessOnce()    [单线程, tick 驱动]
        ├── IOPoller::Poll() → 读包
        ├── ReliableUdpChannel → 解 RUDP → Bundle
        ├── InterfaceTable::Dispatch()
        │   ├── LoginResult / AuthenticateResult → ClientSession typed
        │   │      → login/auth 一次性回调
        │   ├── EntityTransferred / CellReady → typed handler → on_deliver
        │   └── Default Handler (其它一切)
        │         → callbacks_.on_deliver(ctx, msg_id, payload, len)
        └── TimerQueue::Process() → 重传 / 心跳
```

> **线程模型**: `EventDispatcher::ProcessOnce()` (`src/lib/network/event_dispatcher.cc`)
> 是纯 tick-pumped 同步实现, 不起内部线程, 所以所有回调在 `AtlasNetPoll`
> 的调用线程里同步触发, 满足 §4.0.6 "每 ctx 单线程"。

#### 5.3.1 服务器→客户端 RPC 的线格式

服务器下行 RPC 统一走 `kClientRpcMessageId`（0xF004）
`ClientRpcEnvelope`，body 为 `[u32 entity_id][u32 rpc_id][u64 trace_id][args]`
（见 `src/server/baseapp/baseapp_messages.h` 的 `ClientRpcEnvelope` 和
`BaseApp::RelayRpcToClient`）。

`atlas_net_client` C API 不解析 envelope；它在 authentication 后注册 default
handler，把 0xF001..0xF007 的 raw wire id 与 payload 投到 `on_deliver`。
托管 `Atlas.Client.ClientSession` 收到 0xF004 后解出 entity / rpc / trace 并调
`DispatchRpc`。桌面 client 的 `ClientApp::RegisterMessageHandlers` 也按同一
envelope 语义转给 native provider:

```cpp
if (msg_id == baseapp::kClientRpcMessageId) {
  auto entity_id = reader.Read<EntityID>();
  auto rpc_id = reader.Read<uint32_t>();
  auto trace_id = reader.Read<uint64_t>();
  auto args = reader.ReadBytes(reader.Remaining());
  OnRpcMessage(*entity_id, *rpc_id, *trace_id, args->data(),
               static_cast<int32_t>(args->size()));
}
```

约束:
- DLL default handler 只在 `kConnected` 状态注册；`disconnect()` 时反注册。
- `entity_id` 来自 `ClientRpcEnvelope`，不再假定总是当前玩家实体。
- payload 指针在回调返回后失效 (§4.0.1)

#### 5.3.2 为什么 native C API 只做 raw deliver？

- C API ABI 保持窄接口，只承诺 wire id + payload；新增客户端消息不需要扩展
  callback table。
- `Atlas.Client.ClientSession` 统一解 AoI、space-data、RPC 和 movement ack，
  Unity / desktop / integration tests 共用同一托管解码路径。
- 原生桌面 client 需要直连脚本 runtime 时，可以在自己的 default handler 中解
  `ClientRpcEnvelope` 后转给 native provider。

### 5.4 日志转发

DLL 启动时注册一个自定义 `LogSink`，将所有日志通过 `AtlasLogFn` 回调转发:

```cpp
class CallbackLogSink : public LogSink {
    AtlasLogFn handler_;
public:
    void write(LogLevel level, std::string_view category,
               std::string_view message, const std::source_location&) override
    {
        if (handler_)
            handler_(static_cast<int32_t>(level), message.data(),
                     static_cast<int32_t>(message.size()));
    }
};
```

---

## 6. C# P/Invoke 层

### 6.1 AtlasNetNative.cs

`src/csharp/Atlas.Client/Native/AtlasNetNative.cs` 采用 `DllImport`,以便
`netstandard2.1`、Unity Mono 和 IL2CPP 共用同一份源码。当前关键入口如下:

```csharp
public static unsafe class AtlasNetNative
{
    public const uint AbiVersion = 0x02050000u;

#if UNITY_IOS && !UNITY_EDITOR
    private const string LibName = "__Internal";
#else
    private const string LibName = "atlas_net_client";
#endif

    [DllImport(LibName)] public static extern uint AtlasNetGetAbiVersion();
    [DllImport(LibName)] public static extern IntPtr AtlasNetLastError(IntPtr ctx);
    [DllImport(LibName)] public static extern IntPtr AtlasNetGlobalLastError();
    [DllImport(LibName)] public static extern IntPtr AtlasNetCreate(uint expectedAbi);
    [DllImport(LibName)] public static extern void AtlasNetDestroy(IntPtr ctx);
    [DllImport(LibName)] public static extern int AtlasNetPoll(IntPtr ctx);
    [DllImport(LibName)] public static extern AtlasNetState AtlasNetGetState(IntPtr ctx);
    [DllImport(LibName)] public static extern int AtlasNetLogin(
        IntPtr ctx, string loginappHost, ushort loginappPort, string username,
        string passwordHash, IntPtr callback, IntPtr userData);
    [DllImport(LibName)] public static extern int AtlasNetAuthenticate(
        IntPtr ctx, IntPtr callback, IntPtr userData);
    [DllImport(LibName)] public static extern int AtlasNetSetEntityDefDigest(
        IntPtr ctx, byte* data, int len);
    [DllImport(LibName)] public static extern int AtlasNetDisconnect(
        IntPtr ctx, AtlasDisconnectReason reason);
    [DllImport(LibName)] public static extern int AtlasNetSetCallbacks(
        IntPtr ctx, ref AtlasNetCallbacks callbacks);
    [DllImport(LibName)] public static extern int AtlasNetSetTransportImpairment(
        IntPtr ctx, uint oneWayLatencyMs, uint lossPermyriad, uint seed);
    [DllImport(LibName)] public static extern int AtlasNetSendBaseRpc(
        IntPtr ctx, uint entityId, uint rpcId, byte* payload, int len);
    [DllImport(LibName)] public static extern int AtlasNetSendCellRpc(
        IntPtr ctx, uint entityId, uint rpcId, byte* payload, int len);
    [DllImport(LibName)] public static extern int AtlasNetSendMovementInput(
        IntPtr ctx, uint targetEntityId, AtlasMovementInputFrame* frames,
        int frameCount);
    [DllImport(LibName)] public static extern int AtlasNetSendMovementCorrectionReport(
        IntPtr ctx, uint targetEntityId, uint ackedInputSeq, uint serverTick,
        float distanceM, ushort correctionFlags);
    [DllImport(LibName)] public static extern int AtlasNetMovementPredictStep(
        AtlasMovementStateFrame* previous, AtlasMovementInputFrame* input,
        uint serverTick, AtlasMovementStateFrame* outState);
    [DllImport(LibName)] public static extern void AtlasNetSetLogHandler(IntPtr handler);
    [DllImport(LibName)] public static extern int AtlasNetGetStats(
        IntPtr ctx, out AtlasNetStats stats);
}
```

`Create()` 薄封装统一传入 `AbiVersion`,并在 `AtlasNetCreate` 返回 null 时用
`AtlasNetGlobalLastError()` 生成清晰异常。Login/Auth 仍是一次性 callback +
`userData` 透传;当前 `AtlasNetworkManager` 保存 delegate 字段来维持 callback
生命周期。

### 6.2 回调结构体

```csharp
// Layout pinned by tests/unit/test_net_client_abi_layout.cpp.
[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct AtlasNetCallbacks
{
    public IntPtr OnDisconnect;
    public IntPtr OnDeliver;
}
```

`OnDeliver` 是当前 C API 的唯一服务端消息入口。DLL 不再把 AOI enter/leave、
position、property、RPC 拆成多个 typed callbacks;C# `ClientSession` 负责按
wire `msgId` 解码。

### 6.3 回调注册（Pattern B — 当前路径）

`AtlasNetCallbackBridge` 用进程级 `ConcurrentDictionary<IntPtr,IAtlasNetEvents>`
把 native ctx 映射回托管宿主。两个 static delegate 作为 GC keep-alive;
Unity 编译时用 `[UnityEngine.AOT.MonoPInvokeCallback]` 标注同一组 handler。

```csharp
public static unsafe class AtlasNetCallbackBridge
{
    private static readonly ConcurrentDictionary<IntPtr, IAtlasNetEvents> CtxMap = new();

    public delegate void DisconnectFn(IntPtr ctx, int reason);
    public delegate void DeliverFn(IntPtr ctx, ushort msgId, byte* payload, int len);

    private static readonly DisconnectFn SDisconnect = OnDisconnect;
    private static readonly DeliverFn    SDeliver    = OnDeliver;

    public static void Register(IntPtr ctx, IAtlasNetEvents events)
    {
        CtxMap[ctx] = events;
        var table = new AtlasNetCallbacks
        {
            OnDisconnect = Marshal.GetFunctionPointerForDelegate(SDisconnect),
            OnDeliver    = Marshal.GetFunctionPointerForDelegate(SDeliver),
        };
        int rc = AtlasNetNative.AtlasNetSetCallbacks(ctx, ref table);
        if (rc != AtlasNetReturnCode.Ok) throw new InvalidOperationException(
            $"AtlasNetSetCallbacks failed: rc={rc}");
    }

    public static void Unregister(IntPtr ctx) => CtxMap.TryRemove(ctx, out _);

#if UNITY_5_3_OR_NEWER
    [UnityEngine.AOT.MonoPInvokeCallback(typeof(DisconnectFn))]
#endif
    private static void OnDisconnect(IntPtr ctx, int reason)
        => FromCtx(ctx)?.OnDisconnect(reason);

#if UNITY_5_3_OR_NEWER
    [UnityEngine.AOT.MonoPInvokeCallback(typeof(DeliverFn))]
#endif
    private static void OnDeliver(IntPtr ctx, ushort msgId, byte* payload, int len)
        => FromCtx(ctx)?.OnDeliver(msgId, MakeSpan(payload, len));
}
```

`AtlasNetworkManager` implements `IAtlasNetEvents`: `OnDisconnect` raises the Unity
`Disconnected` event, and `OnDeliver` calls `Session.DeliverFromServer(msgId, payload)`.
Generated RPC helpers reach native through `ClientHost.SendBaseRpcHandler` /
`SendCellRpcHandler`, both wired by `AtlasNetworkManager.WireClientHostBridges()`.

### 6.4 Pattern A 前向兼容

目标 Unity runtime 的函数指针路径可用后,先重跑 `src/tools/il2cpp_probe/`
验证 `[UnmanagedCallersOnly]`。迁移只影响 `OnDisconnect` / `OnDeliver` 两个
函数指针的取得方式;C ABI、wire 协议和 `AtlasNetCallbacks` 布局不变,因此
ABI 版本号不因 Pattern A 迁移而变化。

---

## 7. Unity SDK 目录结构

仓库内的规范位置在 `src/csharp/Atlas.Client.Unity/`。
`tools/bin/setup_unity_client.{bat,sh}` build 完后,把 runtime SDK 内容同步到用户
Unity 项目的 `Assets/Atlas.Client.Unity/`,并剔除
`Atlas.Client.Unity.csproj`、`bin/`、`obj/` 和 `.gitkeep`;native + 托管 dll
摆在子目录 `Plugins/` 下。

```
src/csharp/Atlas.Client.Unity/
├── Atlas.Client.Unity.asmdef        # Unity 编译单元
├── Atlas.Client.Unity.csproj        # IDE-only mirror, 不进 Unity
├── README.md
├── AtlasClient.cs                    # 高层 connect/auth wrapper
├── AtlasNetworkManager.cs            # MonoBehaviour 入口
├── LoginClient.cs                    # 登录 / 鉴权流程
├── UnityLogBackend.cs                # Atlas.Diagnostics.Log → Debug.Log
├── UnityProfilerBackend.cs           # Atlas.Diagnostics.Profiler → ProfilerMarker
├── UnityConversions.cs               # Atlas↔Unity Vector3/Quaternion 扩展
├── Coro/
│   └── UnityLoop.cs                  # PlayerLoop 驱动 Atlas 协程
├── Runtime/
│   ├── AtlasUnityFramePump.cs        # app-owned frame dispatcher
│   └── AtlasEntityViewRegistry.cs    # session-owned view lifecycle helper
└── Plugins/                          # setup_unity_client 填充
    ├── Atlas.Client.dll              # 托管,任意平台
    ├── Atlas.Shared.dll              # 托管,任意平台
    ├── Windows/x86_64/atlas_net_client.dll
    ├── Windows/x86_64/mimalloc.dll      # Debug 用 mimalloc-debug.dll
    ├── Linux/x86_64/libatlas_net_client.so
    ├── Linux/x86_64/libmimalloc.so      # Debug 用 libmimalloc-debug.so
    ├── macOS/atlas_net_client.bundle
    ├── Android/arm64-v8a/libatlas_net_client.so
    └── iOS/libatlas_net_client_static.a
```

`ClientEntity` / `ClientEntityManager` / `RpcDispatcher` / `SpanReader` /
`SpanWriter` / `MessageIds` 这些复用自 `Atlas.Client.dll` / `Atlas.Shared.dll`,
以预编译 plugin 形式从 `Plugins/` 引入,不重复源码。

### 7.1 AtlasNetworkManager (核心 MonoBehaviour)

`AtlasNetworkManager` 是当前 Unity 场景入口,同时实现 `IAtlasNetEvents`。
它负责创建 native ctx、注册 `AtlasNetCallbackBridge`、把 generated RPC helper
连接到 `ClientHost`,并在 `Update()` 中驱动 `AtlasNetPoll` 与 `ClientSession.Tick`。

```csharp
public sealed class AtlasNetworkManager : MonoBehaviour, IAtlasNetEvents
{
    [SerializeField] private string loginappHost = "127.0.0.1";
    [SerializeField] private ushort loginappPort = 20018;

    public event Action<AtlasLoginStatus, string?>? LoginFinished;
    public event Action<bool, uint, ushort, string?>? AuthFinished;
    public event Action<int>? Disconnected;

    public ClientSession Session { get; private set; } = ClientCallbacks.DefaultSession;
    public AtlasNetState State =>
        _ctx == IntPtr.Zero ? AtlasNetState.Disconnected : AtlasNetNative.AtlasNetGetState(_ctx);

    private IntPtr _ctx;
    private AtlasNetNative.LoginResultDelegate? _loginCallback;
    private AtlasNetNative.AuthResultDelegate? _authCallback;

    private void Awake()
    {
        _ctx = AtlasNetNative.Create();
        AtlasNetCallbackBridge.Register(_ctx, this);
        WireClientHostBridges();
    }

    private void Update()
    {
        if (_ctx == IntPtr.Zero) return;
        AtlasNetNative.AtlasNetPoll(_ctx);
        Session.Tick(Time.deltaTime);
    }

    public void Configure(string host, ushort port)
    {
        loginappHost = host;
        loginappPort = port;
    }

    public void ConfigureSession(ClientSession session)
    {
        Session = session ?? throw new ArgumentNullException(nameof(session));
        if (_ctx != IntPtr.Zero) WireClientHostBridges();
    }

    public int Login(string username, string passwordHash) { ... }
    public int Authenticate() { ... }
    public int Logout() { ... }
    public int SendBaseRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload) { ... }
    public int SendCellRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload) { ... }
    public bool TryGetStats(out AtlasNetStats stats) { ... }
    public bool TryGetInterpolatedTransform(uint entityId,
        out Vector3 pos, out Vector3 dir, out bool onGround) { ... }

    void IAtlasNetEvents.OnDisconnect(int reason) => Disconnected?.Invoke(reason);
    void IAtlasNetEvents.OnDeliver(ushort msgId, ReadOnlySpan<byte> payload)
        => Session.DeliverFromServer(msgId, payload);
}
```

`AtlasClient` / `LoginClient` 仍提供 coroutine-friendly connect/auth wrapper;
它们是 managed convenience layer,不取代 `AtlasNetworkManager` 的 Unity 场景入口。

---

## 8. SourceGenerator 集成

`.def` 文件是实体定义的唯一来源，`Atlas.Generators.Def` 仍是 C# 端唯一
Source Generator。当前 Unity 集成不在 Unity Editor 内 staging Roslyn
analyzer；`setup_unity_client` 只复制 `Atlas.Shared.dll`、`Atlas.Client.dll`
和 native plugin。游戏侧 typed entity assembly 先通过普通 `dotnet build`
生成，再作为预编译 DLL 放进 Unity。

MVP 的当前路径由 `tools/bin/setup_mvp_unity.{bat,sh}` 负责：

1. 构建 `samples/mvp/Atlas.Mvp.Client/Atlas.Mvp.Client.csproj`
2. `Atlas.Generators.Def` 在该项目构建期间消费 `entity_defs/*.def`
3. 产物 `Atlas.Mvp.Client.dll` 被复制到
   `samples/mvp/UnityClient/Assets/Plugins/Atlas.Mvp/`
4. Unity 通过 asmdef / plugin 引用这个预生成 DLL，而不是在 Editor 编译期
   直接运行 generator

当前生成代码覆盖：

- `client_methods` → receive partial method，由游戏代码实现
- exposed `cell_methods` / `base_methods` → send stub，序列化参数并调用 native DLL
- 非 exposed 的 cell/base methods → 编译期阻断
- 属性字段 → 按 scope 生成客户端可见字段、setter 和 change hook
- `position` 字段 → 按 `ATLAS_DEF008` 保留为 volatile envelope 路径，不进入
  普通属性 delta

如果以后需要让第三方 Unity 项目在 Editor 内直接跑 generator，再补
`Assets/Atlas.Client.Unity/Analyzers/`、`RoslynAnalyzer` import settings 和
`.def` AdditionalFiles 流程；这不是当前 SDK 的发布路径。

### 8.1 Atlas.Shared / Atlas.Client 复用

`Atlas.Client.Unity.asmdef` 通过 `precompiledReferences` 引用
`Atlas.Shared.dll` 和 `Atlas.Client.dll`。这些 DLL 由 `setup_unity_client`
从 `src/csharp/*/bin/<config>/netstandard2.1/` staging 到
`Assets/Atlas.Client.Unity/Plugins/`；Unity 不直接复制这些项目的源文件。

常用类型仍来自同一源码项目:

- `Atlas.Shared.Serialization.SpanWriter` / `SpanReader`
- `Atlas.Shared.Protocol.MessageIds`
- `Atlas.Shared.DataTypes.EntityRef`
- `Atlas.Client.ClientEntity` / `ClientSession` / `AvatarFilter`

属性元数据 / RPC 声明全部由 `.def` 提供；游戏侧 typed assembly 通过
`[Entity("Name")]` partial class 关联到具体 `.def` 文件。

---

## 9. 跨平台考虑

### 9.1 平台矩阵

| 平台 | 库类型 | IO 后端 | 构建工具链 | 注意事项 |
|------|--------|---------|-----------|----------|
| Windows x64 | .dll | WSAPoll | CMake + MSVC (VS 2026 / VS 2022 17.14+) | 主开发平台 |
| Android arm64 | .so | epoll | `net-client-android-arm64` + Android NDK | Unity IL2CPP |
| iOS arm64 | .a (静态) | select | `net-client-ios-arm64` + Xcode | Apple 禁止第三方动态库 |
| macOS arm64 | .bundle | select | `net-client-macos-arm64` + AppleClang | 开发调试 (Unity 编辑器);  `.bundle` 是 Unity macOS Plugin 的标准格式, 实际内部是 Mach-O 动态库 |
| Linux x64 | .so | epoll | `net-client-linux-x64` + GCC/Clang | 服务端/CI |

**Unity 版本兼容性 / C# 回调模式：**

| Unity 区间 | 嵌入 runtime | C# 回调模式 (§6.3) | 备注 |
|---|---|---|---|
| 当前 MVP Unity 6 (`6000.0.43f1-lilith-2`) | Unity Mono / IL2CPP 托管运行时 | **Pattern B** (`[MonoPInvokeCallback]` + delegate) | 当前主线；`[UnmanagedCallersOnly]` 不支持 |
| Unity 2022.3 LTS | Unity Mono / IL2CPP 托管运行时 | **Pattern B** (`[MonoPInvokeCallback]` + delegate) | 兼容目标；切换项目前需重跑 IL2CPP 探针 |
| 未来目标 Unity runtime | 待实测 | **Pattern A 候选** (`[UnmanagedCallersOnly]` + 函数指针) | 须重跑 `src/tools/il2cpp_probe/` 验证再切换；迁移路径见 §6.4 |
| Atlas 最低支持 | 2022.3 LTS | — | 与项目 Unity 客户端目标一致 |

### 9.2 iOS 静态库处理

iOS 不允许加载第三方 `.dylib`，必须编译为 `.a` 静态库。

Unity 处理方式:
- 将 `.a` 放入 `Plugins/iOS/`
- Unity 构建时自动链接进最终二进制
- P/Invoke `[DllImport("__Internal")]` 替代库名

需要条件编译:
```csharp
#if UNITY_IOS && !UNITY_EDITOR
    private const string LibName = "__Internal";
#else
    private const string LibName = "atlas_net_client";
#endif
```

### 9.3 CMake 交叉编译

CMake 通过 platform preset 驱动交叉编译。前置要求:
- **Android**: NDK 已安装, `$ANDROID_NDK_HOME` 指向 NDK 根目录,
  preset 使用 NDK 自带的 Android CMake toolchain 文件。
  **宿主支持**: Windows / macOS / Linux 均可 — NDK 都有对应平台的 toolchain。
- **iOS / macOS**: 安装 Xcode + CLT; iOS preset 使用 Xcode 生成器 +
  `CMAKE_SYSTEM_NAME=iOS`。
  **宿主限制 (B5)**: **必须在 macOS 上执行**。Xcode 不提供 Windows/Linux 版本,
  iOS/macOS preset 带 Darwin host 条件。项目主开发平台是 Windows, 所以 iOS 产物:
    - 开发期: 让有 macOS 的开发者手动跑 `cmake --preset net-client-ios-arm64`
    - 长期: 把 iOS 构建固化到 CI (GitHub Actions `macos-latest` runner 开箱即用)

当前仓库已把客户端 SDK 交叉编译参数固化到 `CMakePresets.json`:

```bash
# Android arm64 (Unity 主力 Android 目标)
cmake --preset net-client-android-arm64
cmake --build --preset net-client-android-arm64

# iOS arm64 — 使用静态库 target (§3.1)
cmake --preset net-client-ios-arm64
cmake --build --preset net-client-ios-arm64

# macOS arm64 (开发调试, Unity 编辑器使用)
cmake --preset net-client-macos-arm64
cmake --build --preset net-client-macos-arm64

# Linux x64
cmake --preset net-client-linux-x64
cmake --build --preset net-client-linux-x64
```

Unity 侧对 iOS 使用 `[DllImport("__Internal")]`, `.a` 归档会被
Xcode 主工程直接链接入最终二进制 (见 §7 Plugins/iOS 目录)。

### 9.4 IO 后端选择

| 平台 | 推荐后端 | 说明 |
|------|----------|------|
| Windows | WSAPoll | 已实现，客户端连接数少，WSAPoll 足够 |
| Linux/Android | epoll | 已实现 |
| iOS/macOS | select | 已实现。kqueue 可选 (未实现)，但 select 对单连接客户端足够 |
| io_uring | 不用于客户端 | 仅适合服务端高并发场景 |

---

## 10. 落地概览

> 整套工作已完成,以下仅记录最终的代码 / 构建 / 验收落点。

| 区块 | 落地 |
|------|------|
| IL2CPP callback probe | `src/tools/il2cpp_probe/`(probe.cc + Unity ProbeComponent + README);Pattern B 决议见上方"关键决策记录" |
| 依赖解耦 | `foundation/process_type.{h,cc}`、`server/entity_types.h::DatabaseID`、`atlas_serialization_binary` STATIC target；`atlas_net_client` 只 include `src/server` 的 header-only 消息定义，链接闭包不含 `atlas_server` / `atlas_db*` / `atlas_entitydef` / pugixml |
| C API 导出层 | `src/lib/net_client/`(`client_api.cc` + `client_session.cc`),`atlas_net_client.dll` SHARED + `atlas_net_client_core` STATIC + iOS `_static` 三 target;`test_net_client_abi_layout` 锁 sizeof / offsetof |
| C# P/Invoke | `Atlas.Client/Native/`(DllImport + Pattern B 桥 + `IAtlasNetEvents`);`Atlas.Tools.NetClientDemo`(CoreCLR 控制台)做 FFI roundtrip |
| Unity SDK | `src/csharp/Atlas.Client.Unity/`(asmdef + `AtlasNetworkManager` MonoBehaviour + `tools/bin/setup_unity_client.{bat,sh}` 同步 runtime SDK 到用户 Unity `Assets/`) |
| 跨平台构建 | `CMakePresets.json` 含 `net-client-{android-arm64, ios-arm64, macos-arm64, linux-x64}`;`.github/workflows/atlas-net-client.yml` 矩阵 + 30 天 artifact |

### 落地映射(供修改时定位)

`src/lib/foundation/process_type.{h,cc}` — `ProcessType` 枚举与
`ProcessTypeName / ProcessTypeFromName` 函数。`server/server_config.h`
与 `network/machined_types.h` 都 include 它。

`src/lib/server/entity_types.h` — `EntityID / SessionKey / DatabaseID`;
`db/idatabase.h`、`login_messages.h`、`baseapp_messages.h` 全部走这一头。

### 关键测试

| 测试 | 文件 | 锁定的不变量 |
|------|------|-------------|
| `test_client_flow` | `tests/integration/test_client_flow.cpp` | 真实 LoginApp + BaseApp + DBApp 端到端线格式 |
| `test_net_client_abi_layout` | `tests/unit/test_net_client_abi_layout.cpp` | `static_assert` 锁定 `AtlasNetCallbacks` / `AtlasNetStats` 的 `sizeof` / `offsetof`;C# 侧用 `Marshal.SizeOf<>` 双向核对 |
| `ClientSessionTests` | `tests/csharp/Atlas.Client.Tests/ClientSessionTests.cs` | C# 下行消息解码、实体生命周期、RPC dispatch、space-data 路径 |
| FFI roundtrip | `Atlas.Tools.NetClientDemo`(CoreCLR 控制台) | `Create(abi)` → `Login` → `Authenticate` → 1 条 RPC → `Disconnect` → `Destroy`,验证 `user_data` 透传与 ABI 不匹配时的清晰异常 |

---

## 11. 协议兼容性备忘

### 11.1 线格式要求

以下格式必须与服务端严格匹配，任何不一致都会导致通信失败:

| 项目 | 格式 | 参考 |
|------|------|------|
| 字节序 | **小端序** (Little-Endian) | `binary_stream.h` endian 转换 |
| PackedInt | `<0xFE`: 1B, `0xFE`: 3B, `0xFF`: 5B | `binary_stream.h` WritePackedInt |
| 字符串 | PackedInt(len) + UTF-8 字节 | `binary_stream.h` WriteString |
| SessionKey | 32 字节不透明数据 | `server/entity_types.h` `SessionKey` |
| MessageID | uint16_t | `network/message.h` `MessageID` |
| EntityID | uint32_t | `server/entity_types.h` `EntityID` |

### 11.2 关键消息 ID

| 消息 | ID | 方向 | 长度 |
|------|-----|------|------|
| LoginRequest | 5000 | Client → LoginApp | 可变 |
| LoginResult | 5001 | LoginApp → Client | 可变 |
| Authenticate | 2020 | Client → BaseApp | 固定 32B |
| AuthenticateResult | 2021 | BaseApp → Client | 可变 |
| ClientBaseRpc | 2022 | Client → BaseApp | 可变 |
| ClientCellRpc | 2023 | Client → BaseApp | 可变 |

### 11.3 RUDP 线格式

```
[1B flags][4B seq?][4B ack?][4B ack_bits?][4B frag?] + payload
MTU = 1472, 最大分片 255, 延迟 ACK 25ms
```

DLL 内部复用现有 `ReliableUdpChannel` 实现，C# 层无需感知 RUDP 细节。

### 11.4 password_hash 算法 (I4)

`AtlasNetLogin` 的 `password_hash` 字段由客户端在调用前计算。为避免服务端
拿到明文密码, 哈希必须在**客户端**侧做, DLL 只作透传。

**算法**:

```
password_hash = Base64( SHA-256( username + ":" + password ) )
```

- 输入: UTF-8 字节序列 `username + ":" + password`
- 输出: 32 字节摘要, Base64 编码 (44 字符含填充)
- 为什么拼 username: 盐化, 防止两个用户相同密码得到相同 hash

**两端对齐验证**:
- 服务端在 `src/server/loginapp/loginapp.cc` 的 `LoginRequest` 处理路径比对
  客户端传入的 hash 字符串。
- C# 调用方在进入 `AtlasNetLogin` 前计算 `passwordHash`;DLL 只作透传。

**不做**: bcrypt/argon2 等 slow-hash — 它们的成本在于抗离线爆破, 需要服务端
存储盐后做 slow verify; 本项目认证路径目前还是查表比对, slow-hash 不提供
额外价值。真正的密码存储强化应在 LoginApp 落库层独立设计。

### 11.5 `AtlasNetDisconnect` 幂等性与回调触发 (M5)

§4.5.4 约定:

| 场景 | 行为 |
|------|------|
| 首次调用, 状态非 Disconnected | 关闭 channel / 清状态; USER-initiated 不触发 `on_disconnect`; LOGOUT-initiated 触发 `on_disconnect(ctx, 3)`，其中 `3` 是 logged-off 回调原因 |
| 首次调用, 状态已为 Disconnected | noop; 返回 0; **不触发 `on_disconnect`** |
| 重复调用 (无论 reason) | noop; 返回 0; **不再触发 `on_disconnect`** (避免 C# 重复收到断线事件) |
| DLL 内部检测到断线 (服务端主动关闭/超时/网络错误) | 自动触发 `on_disconnect(ctx, reason)` 一次, 并切到 Disconnected |

换言之: `on_disconnect` 在一次 "从 Connected/半途 → Disconnected" 转换中
**恰好触发一次**。

---

## 12. 风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| 未来 Unity runtime 上 Pattern A 仍不工作 | 切换到 Pattern A 失败回滚 | 落地时**先重跑** `src/tools/il2cpp_probe/`;保留 Pattern B 路径以 `#if !ATLAS_CALLBACK_PATTERN_A` 包住,可灰度回退 |
| Unity Roslyn 版本不兼容 SourceGenerator | 编译失败 | 检查 Unity 版本对应 Roslyn 版本, 锁定 Generator 为 netstandard2.0 + Roslyn 4.3 以下 API |
| iOS 静态链接符号冲突 | 链接错误 | `-fvisibility=hidden` 只导出 `atlas_net_*`; 与 Unity 内置 .NET 运行时符号必不冲突 |
| 回调线程安全 | 崩溃/数据竞争 | 所有回调在 `poll()` 内同步触发, 与 Unity 主线程一致 (§4.0.6) |
| Atlas.Shared 的 Vector3 与 UnityEngine.Vector3 冲突 | 编译错误/混淆 | 条件编译或隐式转换运算符 |
| DLL 与 C# 层 ABI 版本不匹配 | 静默数据损坏 / Unity 崩溃 | `AtlasNetCreate(expected_abi)` 强制校验, 失败返回 NULL (§4.0.5) |
| `AtlasNetCallbacks` 布局改动未同步 | Unity 运行时不定期崩溃 | `test_net_client_abi_layout` 用 `static_assert` 锁 `sizeof`/`offsetof`, 编译期阻断 |
| SessionKey 泄漏 (core dump / 进程 snapshot) | 会话劫持 | `SessionKey` 不跨 FFI, `SecureZero` 清除 (§5.2.1) |
| 用户在非法状态调用 API (重复 login 等) | 状态机损坏 | §4.5.6 矩阵 + 非法调用仅返回错误码, 绝不隐式断开 |
| C# 在回调中递归调用 poll/destroy | 栈溢出 / use-after-free | §4.0.3 明确禁止清单, `ATLAS_DEBUG` 下 assert |
| C# 保存 `AtlasNetLastError` 返回指针跨帧使用 | 悬垂指针读取 | §4.0.1/4.0.2 文档 + code review 检查; C# 层统一封装为 `string` 复制 |
| `ClientRpcEnvelope` 字段顺序在 C++ / C# / UE 任一端漂移 | RPC 分发失败或 trace 丢失 | §5.3.1 固定 `[entity_id][rpc_id][trace_id][args]`; wire contract 变更需同步三端测试 |
