# Unity Native Network DLL 设计文档

> **状态**: 草案  
> **日期**: 2026-04-14  
> **目标**: 将 C++ 网络层抽取为独立 native DLL，供 Unity 客户端通过 P/Invoke 调用

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
│  ├── ClientCallbacks ([UnmanagedCallersOnly])│
│  ├── ClientNativeApi (P/Invoke → atlas_engine.dll)│
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
│  │   ├── Update() → atlas_net_poll()        │
│  │   ├── Login/Auth 调用                     │
│  │   └── 回调分发                            │
│  ├── Atlas.Client (改造后)                   │
│  │   ├── ClientEntity / EntityManager       │
│  │   ├── ClientNativeApi (P/Invoke → atlas_net_client)│
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

- **C# 驱动 Tick**: Unity 的 `Update()` 调用 `atlas_net_poll()` 驱动网络事件循环
- **高层封装 Login/Auth**: DLL 内部管理连接切换和协议状态机，C# 只需调用并等待回调
- **回调上行**: 收到 RPC/实体创建/断线等事件时，DLL 通过注册的函数指针回调 C#
- **零 CLR 依赖**: DLL 不嵌入 CoreCLR，不依赖 hostfxr

---

## 2. 依赖解耦

### 2.1 当前依赖图

```
atlas_network
├── atlas_foundation   ✓ 无外部依赖
├── atlas_platform     ✓ 无外部依赖
├── atlas_serialization
│   ├── binary_stream  ✓ 需要
│   ├── xml_parser     ✗ 不需要 (依赖 pugixml)
│   └── json_parser    ✗ 不需要 (依赖 rapidjson)
└── atlas_zlib         ✓ 可选 (压缩过滤器)

问题依赖:
├── network/machined_types.hpp → server/server_config.hpp (ProcessType 枚举)
├── login_messages.hpp → db/idatabase.hpp → entitydef/ (DatabaseID 类型)
└── baseapp_messages.hpp → db/idatabase.hpp → entitydef/
```

### 2.2 解耦方案

#### 2.2.1 ProcessType 枚举提取

`server/server_config.hpp` 包含 `ProcessType` 枚举和大量服务端配置（数据库、认证等），但 `machined_types.hpp` 只用到 `ProcessType`。

**方案**: 将 `ProcessType` 提取到 `foundation/process_type.hpp`。

```cpp
// src/lib/foundation/process_type.hpp (新文件)
#pragma once
#include <cstdint>
#include <string_view>

namespace atlas
{
enum class ProcessType : uint8_t
{
    Machined = 0, LoginApp = 1, BaseApp = 2, BaseAppMgr = 3,
    CellApp = 4, CellAppMgr = 5, DBApp = 6, DBAppMgr = 7, Reviver = 8,
};

[[nodiscard]] auto process_type_name(ProcessType type) -> std::string_view;
[[nodiscard]] auto process_type_from_name(std::string_view name) -> Result<ProcessType>;
}  // namespace atlas
```

**影响**:
- `server/server_config.hpp` 改为 `#include "foundation/process_type.hpp"`
- `network/machined_types.hpp` 改为 `#include "foundation/process_type.hpp"`
- `machined_types.hpp` 对 `server/` 的依赖消除

#### 2.2.2 DatabaseID 提取

`login_messages.hpp` 和 `baseapp_messages.hpp` 依赖 `db/idatabase.hpp` 仅为了 `DatabaseID` 类型别名。

**方案**: 将 `DatabaseID` 提取到 `server/entity_types.hpp`（已存在 `EntityID` 和 `SessionKey`）。

```cpp
// 添加到 src/lib/server/entity_types.hpp
using DatabaseID = int64_t;
inline constexpr DatabaseID kInvalidDBID = 0;
```

**影响**:
- `db/idatabase.hpp` 改为 `#include "server/entity_types.hpp"` 获取 DatabaseID
- `login_messages.hpp` 和 `baseapp_messages.hpp` 移除 `#include "db/idatabase.hpp"`
- 消除对 `entitydef/` 的传递依赖

#### 2.2.3 客户端消息头文件

DLL 只需要客户端相关的消息定义（LoginRequest/LoginResult/Authenticate/AuthenticateResult/ClientBaseRpc/ClientCellRpc）。这些消息定义在 `login_messages.hpp` 和 `baseapp_messages.hpp` 中，但两个文件还包含大量仅服务端使用的消息。

**方案**: 不拆分文件。完成 2.2.1 和 2.2.2 的解耦后，这两个头文件的依赖链变为：

```
login_messages.hpp / baseapp_messages.hpp
├── network/address.hpp      ✓ 已在 DLL 中
├── network/message.hpp      ✓ 已在 DLL 中
├── network/message_ids.hpp  ✓ 已在 DLL 中
└── server/entity_types.hpp  ✓ 纯类型头文件，无 .cpp
```

依赖干净，可以直接 include。

### 2.3 解耦后的依赖图

```
atlas_net_client.dll
├── atlas_network        (全部 12 个 .cpp)
├── atlas_platform       (全部平台相关 .cpp)
├── atlas_foundation     (全部 .cpp)
├── atlas_serialization  (仅 binary_stream.cpp)
├── atlas_zlib           (可选, 用于压缩过滤器)
├── server/entity_types.hpp  (header-only, EntityID/SessionKey/DatabaseID)
├── login_messages.hpp       (header-only, 登录消息定义)
├── baseapp_messages.hpp     (header-only, BaseApp 消息定义)
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

### 3.1 新增 Target: atlas_net_client

```cmake
# src/lib/net_client/CMakeLists.txt

add_library(atlas_net_client SHARED
    client_api.cpp          # C API 导出层 (新文件)
    client_session.cpp      # Login/Auth 状态机 (新文件)
)

target_compile_definitions(atlas_net_client PRIVATE
    ATLAS_NET_CLIENT_EXPORTS
)

target_link_libraries(atlas_net_client
    PRIVATE
        atlas_network
        atlas_foundation
        atlas_platform
        atlas_serialization
        $<$<BOOL:${ATLAS_ENABLE_COMPRESSION}>:atlas_zlib>
)

# 只导出 ATLAS_NET_API 标记的符号
if(NOT WIN32)
    target_compile_options(atlas_net_client PRIVATE -fvisibility=hidden)
endif()

# 输出到 bin/ 目录
set_target_properties(atlas_net_client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
```

### 3.2 CMake 选项

```cmake
option(ATLAS_BUILD_NET_CLIENT "Build Unity/client native networking DLL" OFF)
```

在顶层 `src/lib/CMakeLists.txt` 中：

```cmake
if(ATLAS_BUILD_NET_CLIENT)
    add_subdirectory(net_client)
endif()
```

### 3.3 序列化模块拆分

当前 `atlas_serialization` 编译了 4 个 .cpp 文件，其中 `xml_parser.cpp` 和 `json_parser.cpp` 引入了 pugixml 和 rapidjson 依赖。

**方案**: 将 `binary_stream.cpp` 拆分为独立 target `atlas_serialization_binary`。

```cmake
# src/lib/serialization/CMakeLists.txt (修改)

# 二进制序列化 (无第三方依赖)
add_library(atlas_serialization_binary STATIC binary_stream.cpp)
target_link_libraries(atlas_serialization_binary PUBLIC atlas_foundation atlas_platform)

# 完整序列化 (含 XML/JSON)
add_library(atlas_serialization STATIC
    binary_stream.cpp data_section.cpp xml_parser.cpp json_parser.cpp
)
target_link_libraries(atlas_serialization
    PUBLIC atlas_foundation atlas_platform
    PRIVATE pugixml::pugixml rapidjson
)
```

`atlas_net_client` 只链接 `atlas_serialization_binary`。

---

## 4. C API 定义

### 4.1 导出宏

```cpp
// src/lib/net_client/net_client_export.hpp

#pragma once

#if ATLAS_PLATFORM_WINDOWS
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
typedef struct AtlasNetChannel AtlasNetChannel;

// ABI 版本
ATLAS_NET_CALL uint32_t atlas_net_abi_version(void);

// 错误信息
ATLAS_NET_CALL const char* atlas_net_last_error(AtlasNetContext* ctx);
```

### 4.3 生命周期

```cpp
// 创建网络上下文 (内部创建 EventDispatcher + NetworkInterface)
ATLAS_NET_CALL AtlasNetContext* atlas_net_create(void);

// 销毁 (断开所有连接, 释放资源)
ATLAS_NET_CALL void atlas_net_destroy(AtlasNetContext* ctx);
```

### 4.4 Tick (C# 驱动)

```cpp
// 驱动一次网络事件循环
// - 处理所有待处理的 IO 事件
// - 触发到期的定时器 (重传、心跳等)
// - 触发已注册的回调 (RPC 分发、实体创建等)
// 返回处理的事件数, -1 表示错误
ATLAS_NET_CALL int32_t atlas_net_poll(AtlasNetContext* ctx);
```

Unity 侧用法:
```csharp
void Update() {
    AtlasNet.Poll(_ctx);  // 每帧调用, 驱动整个网络层
}
```

### 4.5 Login/Authenticate (高层封装)

```cpp
// ============================================================================
// 回调类型定义
// ============================================================================

// 登录结果回调
// status: 0=成功, 1=InvalidCredentials, 2=AlreadyLoggedIn, ...
// 成功时 baseapp_host/baseapp_port 有效
typedef void (*AtlasLoginResultFn)(
    uint8_t status,
    const char* baseapp_host,
    uint16_t baseapp_port,
    const char* error_message
);

// 认证结果回调
// 成功时 entity_id 和 type_id 有效
typedef void (*AtlasAuthResultFn)(
    uint8_t success,
    uint32_t entity_id,
    uint16_t type_id,
    const char* error_message
);

// ============================================================================
// 登录流程
// ============================================================================

// 发起登录 (异步, 结果通过回调返回)
// 内部流程: connect_rudp(loginapp) → send LoginRequest → 等待 LoginResult
ATLAS_NET_CALL int32_t atlas_net_login(
    AtlasNetContext* ctx,
    const char* loginapp_host,
    uint16_t loginapp_port,
    const char* username,
    const char* password_hash,
    AtlasLoginResultFn callback
);

// 发起认证 (登录成功后调用, 异步)
// 内部流程: connect_rudp(baseapp) → send Authenticate → 等待 AuthenticateResult
// 通常在 LoginResult 回调中调用
ATLAS_NET_CALL int32_t atlas_net_authenticate(
    AtlasNetContext* ctx,
    const char* baseapp_host,
    uint16_t baseapp_port,
    AtlasAuthResultFn callback
);

// 获取当前连接状态
typedef enum {
    ATLAS_NET_STATE_DISCONNECTED = 0,
    ATLAS_NET_STATE_LOGGING_IN   = 1,
    ATLAS_NET_STATE_AUTHENTICATING = 2,
    ATLAS_NET_STATE_CONNECTED    = 3,
} AtlasNetState;

ATLAS_NET_CALL AtlasNetState atlas_net_get_state(AtlasNetContext* ctx);
```

**内部状态机** (DLL 内部, 不暴露):

```
Disconnected ──login()──→ LoggingIn ──LoginResult(ok)──→ Authenticating
                              │                              │
                         LoginResult(fail)            AuthResult(ok)
                              │                              │
                              ▼                              ▼
                         Disconnected                   Connected
                                                            │
                                                    disconnect/error
                                                            │
                                                            ▼
                                                       Disconnected
```

### 4.6 消息发送

```cpp
// 发送 Base RPC (通过已认证的 BaseApp 连接)
// rpc_id: 由 SourceGenerator 生成的 RPC 标识
// payload: SpanWriter 序列化后的参数数据
ATLAS_NET_CALL int32_t atlas_net_send_base_rpc(
    AtlasNetContext* ctx,
    uint32_t entity_id,
    uint32_t rpc_id,
    const uint8_t* payload,
    int32_t payload_len
);

// 发送 Cell RPC (通过 BaseApp 转发到 CellApp)
ATLAS_NET_CALL int32_t atlas_net_send_cell_rpc(
    AtlasNetContext* ctx,
    uint32_t entity_id,
    uint32_t rpc_id,
    const uint8_t* payload,
    int32_t payload_len
);
```

### 4.7 消息接收回调

```cpp
// ============================================================================
// 回调注册 (在 atlas_net_create 之后, login 之前调用)
// ============================================================================

// RPC 消息回调 (服务端 → 客户端)
typedef void (*AtlasRpcCallbackFn)(
    uint32_t entity_id,
    uint32_t rpc_id,
    const uint8_t* payload,
    int32_t payload_len
);

// 实体创建回调
typedef void (*AtlasEntityCreateFn)(
    uint32_t entity_id,
    uint16_t type_id
);

// 实体销毁回调
typedef void (*AtlasEntityDestroyFn)(
    uint32_t entity_id
);

// 连接断开回调
typedef void (*AtlasDisconnectFn)(
    int32_t reason  // 0=正常, 1=超时, 2=服务端关闭, 3=网络错误
);

// 注册所有回调 (打包为结构体, 类似现有 ClientCallbackTable)
#pragma pack(push, 1)
typedef struct {
    AtlasRpcCallbackFn      on_rpc;
    AtlasEntityCreateFn     on_entity_create;
    AtlasEntityDestroyFn    on_entity_destroy;
    AtlasDisconnectFn       on_disconnect;
} AtlasNetCallbacks;
#pragma pack(pop)

ATLAS_NET_CALL void atlas_net_set_callbacks(
    AtlasNetContext* ctx,
    const AtlasNetCallbacks* callbacks
);
```

### 4.8 日志

```cpp
// 日志回调 (DLL 内部日志转发到 Unity Debug.Log)
typedef void (*AtlasLogFn)(
    int32_t level,      // 0=Trace, 1=Debug, 2=Info, 3=Warning, 4=Error, 5=Critical
    const char* message,
    int32_t message_len
);

ATLAS_NET_CALL void atlas_net_set_log_handler(AtlasLogFn handler);
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

ATLAS_NET_CALL int32_t atlas_net_get_stats(
    AtlasNetContext* ctx,
    AtlasNetStats* out_stats
);
```

### 4.10 完整 API 一览

| 函数 | 方向 | 用途 |
|------|------|------|
| `atlas_net_abi_version` | - | ABI 兼容性检查 |
| `atlas_net_last_error` | - | 获取最后错误信息 |
| `atlas_net_create` | C#→C++ | 创建网络上下文 |
| `atlas_net_destroy` | C#→C++ | 销毁网络上下文 |
| `atlas_net_poll` | C#→C++ | **每帧调用, 驱动网络层** |
| `atlas_net_set_callbacks` | C#→C++ | 注册事件回调 |
| `atlas_net_set_log_handler` | C#→C++ | 注册日志回调 |
| `atlas_net_login` | C#→C++ | 发起登录 |
| `atlas_net_authenticate` | C#→C++ | 发起认证 |
| `atlas_net_get_state` | C#→C++ | 查询连接状态 |
| `atlas_net_send_base_rpc` | C#→C++ | 发送 Base RPC |
| `atlas_net_send_cell_rpc` | C#→C++ | 发送 Cell RPC |
| `atlas_net_get_stats` | C#→C++ | 获取网络统计 |
| `AtlasLoginResultFn` | C++→C# | 登录结果通知 |
| `AtlasAuthResultFn` | C++→C# | 认证结果通知 |
| `AtlasRpcCallbackFn` | C++→C# | RPC 消息分发 |
| `AtlasEntityCreateFn` | C++→C# | 实体创建通知 |
| `AtlasEntityDestroyFn` | C++→C# | 实体销毁通知 |
| `AtlasDisconnectFn` | C++→C# | 连接断开通知 |

---

## 5. DLL 内部实现

### 5.1 AtlasNetContext 结构

```
AtlasNetContext (不透明, C++ 内部)
├── EventDispatcher dispatcher_
├── NetworkInterface network_{dispatcher_}
├── ClientSession session_                    // 新增: 封装 Login/Auth 状态机
│   ├── state_: AtlasNetState
│   ├── loginapp_channel_: Channel*
│   ├── baseapp_channel_: Channel*
│   ├── session_key_: SessionKey
│   ├── player_entity_id_: EntityID
│   ├── player_type_id_: uint16_t
│   ├── login_callback_: AtlasLoginResultFn
│   └── auth_callback_: AtlasAuthResultFn
├── callbacks_: AtlasNetCallbacks             // C# 回调表
├── log_handler_: AtlasLogFn                  // 日志回调
└── last_error_: std::string                  // 最后错误信息
```

### 5.2 ClientSession 状态机

`ClientSession` 是新增的 C++ 类，封装了原来 `ClientApp` 中的 login/authenticate 流程。

核心逻辑从 `client_app.cpp` 提取:
- `login()` → 注册 `LoginResult` typed handler → 连接 LoginApp → 发送 LoginRequest
- `authenticate()` → 注册 `AuthenticateResult` typed handler → 连接 BaseApp → 发送 Authenticate
- 收到响应后触发相应回调
- 认证成功后注册 default handler 用于 RPC 捕获

### 5.3 消息分发流程

```
atlas_net_poll() 调用
  │
  ▼
EventDispatcher::process_once()
  ├── IOPoller::poll() → 读取网络数据
  ├── ReliableUdpChannel → 解包 RUDP → 提取 Bundle
  ├── InterfaceTable::dispatch()
  │   ├── 匹配 Typed Handler (LoginResult, AuthenticateResult)
  │   │   └── ClientSession 内部处理 → 触发 login/auth 回调
  │   └── Default Handler (RPC 消息)
  │       └── 从 Bundle 中提取 entity_id + rpc_id + payload
  │           └── 调用 callbacks_.on_rpc(entity_id, rpc_id, payload, len)
  └── TimerQueue::process() → 重传、心跳等
```

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

```csharp
// Atlas.Client/Native/AtlasNetNative.cs
using System.Runtime.InteropServices;

internal static unsafe partial class AtlasNetNative
{
    private const string LibName = "atlas_net_client";

    // --- 生命周期 ---
    [LibraryImport(LibName)] internal static partial nint atlas_net_create();
    [LibraryImport(LibName)] internal static partial void atlas_net_destroy(nint ctx);

    // --- Tick ---
    [LibraryImport(LibName)] internal static partial int atlas_net_poll(nint ctx);

    // --- 回调 ---
    [LibraryImport(LibName)]
    internal static partial void atlas_net_set_callbacks(nint ctx, AtlasNetCallbacks* callbacks);

    [LibraryImport(LibName)]
    internal static partial void atlas_net_set_log_handler(nint handler);

    // --- Login/Auth ---
    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int atlas_net_login(
        nint ctx, string host, ushort port,
        string username, string passwordHash,
        nint callback);

    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int atlas_net_authenticate(
        nint ctx, string host, ushort port, nint callback);

    [LibraryImport(LibName)]
    internal static partial int atlas_net_get_state(nint ctx);

    // --- 消息 ---
    [LibraryImport(LibName)]
    internal static partial int atlas_net_send_base_rpc(
        nint ctx, uint entityId, uint rpcId, byte* payload, int len);

    [LibraryImport(LibName)]
    internal static partial int atlas_net_send_cell_rpc(
        nint ctx, uint entityId, uint rpcId, byte* payload, int len);

    // --- 诊断 ---
    [LibraryImport(LibName)]
    internal static partial int atlas_net_get_stats(nint ctx, AtlasNetStats* outStats);

    [LibraryImport(LibName)]
    internal static partial uint atlas_net_abi_version();
}
```

### 6.2 回调结构体

```csharp
[StructLayout(LayoutKind.Sequential, Pack = 1)]
internal unsafe struct AtlasNetCallbacks
{
    public nint OnRpc;           // delegate* unmanaged<uint, uint, byte*, int, void>
    public nint OnEntityCreate;  // delegate* unmanaged<uint, ushort, void>
    public nint OnEntityDestroy; // delegate* unmanaged<uint, void>
    public nint OnDisconnect;    // delegate* unmanaged<int, void>
}
```

### 6.3 回调注册 (与现有模式一致)

```csharp
internal static unsafe class AtlasNetCallbackBridge
{
    [UnmanagedCallersOnly]
    static void OnRpc(uint entityId, uint rpcId, byte* payload, int len) { ... }

    [UnmanagedCallersOnly]
    static void OnEntityCreate(uint entityId, ushort typeId) { ... }

    [UnmanagedCallersOnly]
    static void OnEntityDestroy(uint entityId) { ... }

    [UnmanagedCallersOnly]
    static void OnDisconnect(int reason) { ... }

    internal static void Register(nint ctx)
    {
        AtlasNetCallbacks table;
        table.OnRpc = (nint)(delegate* unmanaged<uint, uint, byte*, int, void>)&OnRpc;
        table.OnEntityCreate = (nint)(delegate* unmanaged<uint, ushort, void>)&OnEntityCreate;
        table.OnEntityDestroy = (nint)(delegate* unmanaged<uint, void>)&OnEntityDestroy;
        table.OnDisconnect = (nint)(delegate* unmanaged<int, void>)&OnDisconnect;
        AtlasNetNative.atlas_net_set_callbacks(ctx, &table);
    }
}
```

### 6.4 ClientNativeApi 适配

现有 `ClientNativeApi.cs` 的修改最小化:

| 现有接口 | 变更 |
|----------|------|
| `LibName = "atlas_engine"` | → `"atlas_net_client"` |
| `atlas_send_base_rpc` | → `atlas_net_send_base_rpc` (增加 ctx 参数) |
| `atlas_send_cell_rpc` | → `atlas_net_send_cell_rpc` (增加 ctx 参数) |
| `atlas_set_native_callbacks` | → `atlas_net_set_callbacks` (新结构体) |
| `atlas_log_message` | → 通过日志回调反向调用 Unity Debug.Log |

---

## 7. Unity Package 结构

```
Packages/
└── com.atlas.client/
    ├── package.json
    ├── Runtime/
    │   ├── Atlas.Client.asmdef
    │   ├── Core/
    │   │   ├── AtlasNetworkManager.cs       # MonoBehaviour, 驱动 Tick
    │   │   ├── AtlasNetworkConfig.cs        # ScriptableObject, 连接配置
    │   │   └── AtlasNetworkState.cs         # 连接状态枚举
    │   ├── Native/
    │   │   ├── AtlasNetNative.cs            # P/Invoke 声明
    │   │   ├── AtlasNetCallbackBridge.cs    # [UnmanagedCallersOnly] 回调
    │   │   └── AtlasNetStats.cs             # 统计结构体
    │   ├── Entity/
    │   │   ├── ClientEntity.cs              # 复用, 适配
    │   │   ├── ClientEntityManager.cs       # 复用
    │   │   └── ClientEntityFactory.cs       # 复用
    │   ├── Rpc/
    │   │   └── RpcDispatcher.cs             # RPC 分发 (从 ClientCallbacks 提取)
    │   ├── Shared/                          # 来自 Atlas.Shared
    │   │   ├── Serialization/
    │   │   │   ├── SpanWriter.cs            # 直接复用
    │   │   │   └── SpanReader.cs            # 直接复用
    │   │   ├── Protocol/
    │   │   │   └── MessageIds.cs            # 直接复用
    │   │   └── DataTypes/
    │   │       ├── Vector3.cs               # 或映射到 UnityEngine.Vector3
    │   │       └── EntityRef.cs             # 直接复用
    │   └── Generated/                       # SourceGenerator 输出目录
    │       └── .gitkeep
    ├── Plugins/
    │   ├── Windows/
    │   │   └── x86_64/
    │   │       └── atlas_net_client.dll      # Windows native
    │   ├── Android/
    │   │   ├── arm64-v8a/
    │   │   │   └── libatlas_net_client.so    # Android ARM64
    │   │   └── armeabi-v7a/
    │   │       └── libatlas_net_client.so    # Android ARMv7
    │   ├── iOS/
    │   │   └── libatlas_net_client.a         # iOS 静态库
    │   ├── macOS/
    │   │   └── atlas_net_client.bundle       # macOS
    │   └── Linux/
    │       └── x86_64/
    │           └── libatlas_net_client.so    # Linux
    └── Editor/
        ├── Atlas.Client.Editor.asmdef
        └── AtlasCodeGenMenu.cs              # 手动触发代码生成 (可选)
```

### 7.1 AtlasNetworkManager (核心 MonoBehaviour)

```csharp
public class AtlasNetworkManager : MonoBehaviour
{
    [SerializeField] private AtlasNetworkConfig config;

    private nint _ctx;
    private AtlasNetworkState _state;

    public event Action<uint, ushort> OnEntityCreated;
    public event Action<uint> OnEntityDestroyed;
    public event Action<int> OnDisconnected;

    void Awake()
    {
        _ctx = AtlasNetNative.atlas_net_create();
        AtlasNetCallbackBridge.Register(_ctx);
        // 注册日志转发
        AtlasNetNative.atlas_net_set_log_handler(
            (delegate* unmanaged<int, byte*, int, void>)&LogBridge.OnLog);
    }

    void Update()
    {
        if (_ctx != 0)
            AtlasNetNative.atlas_net_poll(_ctx);
    }

    void OnDestroy()
    {
        if (_ctx != 0)
        {
            AtlasNetNative.atlas_net_destroy(_ctx);
            _ctx = 0;
        }
    }

    public void Login(string username, string passwordHash) { ... }
    public void SendBaseRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload) { ... }
    public AtlasNetStats GetStats() { ... }
}
```

---

## 8. SourceGenerator 集成

> **前置依赖**: DefGenerator 统一重构 (详见 `docs/DEFGEN_CONSOLIDATION_DESIGN.md`)。  
> 重构已完成，`.def` 文件是实体定义的唯一来源，EntityGenerator 和 RpcGenerator 已合并入 DefGenerator 并删除。

### 8.1 Generator 概览 (重构后)

| Generator | 用途 | Unity 客户端需要 |
|-----------|------|-----------------|
| **DefGenerator** | 从 `.def` 生成: RPC stubs, Mailbox, 属性字段/Property/DirtyTracking, Serialization, DeltaSync, Factory, Dispatcher, TypeRegistry | 是 (ATLAS_CLIENT 上下文) |
| **EventGenerator** | 从 `[EventHandler]` 生成: 事件注册/分发 | 是 |
| ~~EntityGenerator~~ | 已合并入 DefGenerator | 已删除 |
| ~~RpcGenerator~~ | 已合并入 DefGenerator | 已删除 |

Unity 客户端编译时定义 `ATLAS_CLIENT` 符号，DefGenerator 自动生成客户端上下文代码:
- client_methods → **Receive** (partial method, 用户实现)
- exposed cell_methods/base_methods → **Send** (自动序列化参数并调用 native DLL)
- 非 exposed 的 cell/base methods → **Forbidden** (编译期阻断)
- 属性字段 → 只生成 scope ≥ OwnClient 的字段 + `ApplyReplicatedDelta`

### 8.2 Unity 中集成 SourceGenerator

1. 将 DefGenerator 和 EventGenerator 编译为 `netstandard2.0` DLL
2. 将 DLL 放入 Unity Package 中 (如 `Packages/com.atlas.client/Analyzers/`)
3. 在 Unity Inspector 中将其标记为 `RoslynAnalyzer`
4. Unity 2022.2+ 自动在编译时执行 Generator
5. `.def` 文件作为 `AdditionalFiles` 引入 (在 `.asmdef` 或 `.csproj` 中配置)

### 8.3 兼容性注意事项

| 项目 | 注意 |
|------|------|
| Target Framework | Generator 必须编译为 `netstandard2.0` |
| Roslyn 版本 | 需匹配 Unity 内置版本 (检查 Unity 发行说明) |
| Span/ref struct | 生成的代码使用 `ref struct` 需要 Unity 2021.2+ |
| 依赖 | Generator DLL 需放入同目录 |
| ATLAS_CLIENT 符号 | 在 Unity 的 Player Settings → Scripting Define Symbols 中添加 |

### 8.4 Atlas.Shared 复用

`Atlas.Shared` 中的序列化/协议代码可以直接作为源文件复用到 Unity Package 中:

- `SpanWriter.cs` / `SpanReader.cs` — 使用 `System.Buffers.BinaryPrimitives`, Unity 支持
- `MessageIds.cs` — 纯常量定义
- `EventBus.cs` — 事件总线
- `EntityRef.cs` — 实体引用

**不再需要的 Attribute** (由 `.def` 文件替代):
- ~~`[Replicated]`~~, ~~`[Persistent]`~~, ~~`[ServerOnly]`~~ — 已删除，属性元数据在 `.def` 中定义
- ~~`[BaseRpc]`~~, ~~`[CellRpc]`~~, ~~`[ClientRpc]`~~ — 已删除，RPC 声明在 `.def` 中定义
- `[Entity("Name")]` — **保留**, 用于关联 C# class 与 `.def` 文件
- `[EventHandler("name")]` — **保留**, EventGenerator 独立使用

**需要适配的部分**:
- `Vector3` / `Quaternion` — Atlas.Shared 定义了自己的，Unity 有 `UnityEngine.Vector3`。方案: 条件编译 `#if UNITY_ENGINE` 提供隐式转换运算符

---

## 9. 跨平台考虑

### 9.1 平台矩阵

| 平台 | 库类型 | IO 后端 | 构建工具 | 注意事项 |
|------|--------|---------|----------|----------|
| Windows x64 | .dll | WSAPoll | MSVC/CMake | 主开发平台 |
| Android arm64 | .so | epoll | NDK/CMake | Unity IL2CPP, NDK r25+ |
| Android armv7 | .so | epoll | NDK/CMake | 渐淘汰, 可选 |
| iOS arm64 | .a (静态) | select/kqueue | Xcode/CMake | Apple 禁止第三方动态库 |
| macOS arm64 | .bundle | kqueue/select | Xcode/CMake | 开发调试用 |
| Linux x64 | .so | epoll | GCC/CMake | 服务端/CI |

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

```bash
# Android (NDK toolchain)
cmake -B build-android \
    -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-24 \
    -DATLAS_BUILD_NET_CLIENT=ON \
    -DATLAS_BUILD_SERVER=OFF \
    -DATLAS_BUILD_TESTS=OFF

# iOS (Xcode)
cmake -B build-ios -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DATLAS_BUILD_NET_CLIENT=ON \
    -DATLAS_BUILD_SERVER=OFF \
    -DATLAS_BUILD_TESTS=OFF \
    -DBUILD_SHARED_LIBS=OFF       # 静态库
```

### 9.4 IO 后端选择

| 平台 | 推荐后端 | 说明 |
|------|----------|------|
| Windows | WSAPoll | 已实现，客户端连接数少，WSAPoll 足够 |
| Linux/Android | epoll | 已实现 |
| iOS/macOS | select | 已实现。kqueue 可选 (未实现)，但 select 对单连接客户端足够 |
| io_uring | 不用于客户端 | 仅适合服务端高并发场景 |

---

## 10. 实施步骤

### Phase 1: 依赖解耦 (预计 1-2 天)

**目标**: 让 network 模块可以不依赖 server/db/entitydef 独立编译。

1. **提取 ProcessType**
   - 创建 `src/lib/foundation/process_type.hpp`
   - 将 `ProcessType` 枚举及 `process_type_name()`/`process_type_from_name()` 从 `server/server_config.hpp` 移到新文件
   - `server/server_config.hpp` 改为 include 新文件
   - `network/machined_types.hpp` 改为 include 新文件

2. **提取 DatabaseID**
   - 在 `server/entity_types.hpp` 中添加 `DatabaseID` 类型别名
   - `db/idatabase.hpp` 改为从 `entity_types.hpp` 获取 DatabaseID
   - `login_messages.hpp` 和 `baseapp_messages.hpp` 移除 `#include "db/idatabase.hpp"`，确认只需 `#include "server/entity_types.hpp"`

3. **验证**
   - 确保 `atlas_network` target 不再传递依赖 `atlas_server` / `atlas_db` / `atlas_entitydef`
   - 全量编译和测试通过

### Phase 2: 序列化模块拆分 (预计 0.5 天)

1. **创建 `atlas_serialization_binary` target**
   - 修改 `src/lib/serialization/CMakeLists.txt`
   - 仅包含 `binary_stream.cpp`，不链接 pugixml/rapidjson
   - 现有 `atlas_serialization` target 保持不变

2. **验证**
   - `atlas_serialization_binary` 独立编译通过
   - `atlas_network` 链接 `atlas_serialization_binary` 而非 `atlas_serialization`（可选，也可保持现状仅在 net_client 中区分）

### Phase 3: C API 导出层 (预计 3-4 天)

1. **创建目录和文件**
   - `src/lib/net_client/` 目录
   - `net_client_export.hpp` — 导出宏定义
   - `client_session.hpp/.cpp` — Login/Auth 状态机（从 `client_app.cpp` 提取逻辑）
   - `client_api.hpp` — C API 头文件（公开接口定义）
   - `client_api.cpp` — C API 实现
   - `CMakeLists.txt` — 构建配置

2. **实现 ClientSession**
   - 封装 `EventDispatcher` + `NetworkInterface`
   - 实现 Login 状态机（connect → send LoginRequest → handle LoginResult）
   - 实现 Auth 状态机（connect → send Authenticate → handle AuthenticateResult）
   - 实现 RPC 默认处理器（捕获所有 RPC 消息 → 回调 C#）

3. **实现 C API**
   - 生命周期: create/destroy
   - Tick: poll
   - Login/Auth: 高层封装
   - 消息: send_base_rpc, send_cell_rpc
   - 回调: set_callbacks, set_log_handler
   - 诊断: get_stats, get_state, last_error

4. **验证**
   - 编译出 `atlas_net_client.dll`
   - 导出符号检查 (`dumpbin /exports` on Windows)
   - 编写简单的 C 测试程序验证 API 可调用

### Phase 4: C# P/Invoke 适配 (预计 2-3 天)

1. **创建 AtlasNetNative.cs**
   - 所有 P/Invoke 声明
   - 使用 `[LibraryImport]` (source-generated marshalling)

2. **创建 AtlasNetCallbackBridge.cs**
   - `[UnmanagedCallersOnly]` 回调函数
   - 回调内部分发到 `ClientEntityManager` / `RpcDispatcher`

3. **适配 ClientNativeApi.cs**
   - 改为调用 `atlas_net_client` 而非 `atlas_engine`
   - 增加 `nint ctx` 参数传递

4. **验证**
   - 在独立 .NET 控制台项目中测试 P/Invoke 调用
   - 确认回调从 C++ → C# 正常触发

### Phase 5: Unity Package 集成 (预计 2-3 天)

1. **创建 Package 目录结构**
   - `package.json` 配置
   - `Runtime/` 和 `Editor/` 目录
   - Assembly Definition 文件

2. **集成 C# 代码**
   - 复制 Atlas.Shared 序列化/协议代码
   - 复制 Atlas.Client 实体/RPC 代码
   - 创建 `AtlasNetworkManager` MonoBehaviour

3. **集成 Native 插件**
   - 放置各平台 DLL/SO/A 文件
   - 配置 Unity 平台过滤器
   - iOS `__Internal` 条件编译

4. **SourceGenerator 集成**
   - 编译 Generator 为 netstandard2.0
   - 放入包中标记为 RoslynAnalyzer
   - 验证生成代码正确

5. **验证**
   - Unity 工程中引用包
   - 场景中挂载 AtlasNetworkManager
   - 连接本地服务端测试完整流程

### Phase 6: 跨平台构建 (预计 2-3 天)

1. **Android NDK 构建**
   - CMake toolchain 配置
   - arm64-v8a 和 armeabi-v7a
   - 验证 .so 可在 Unity Android 构建中加载

2. **iOS 构建**
   - CMake Xcode 生成
   - 静态库编译
   - 验证 Unity iOS 构建中链接正常

3. **CI 集成** (可选)
   - GitHub Actions 或其他 CI 自动构建各平台产物
   - 产物打包上传

---

## 11. 协议兼容性备忘

### 11.1 线格式要求

以下格式必须与服务端严格匹配，任何不一致都会导致通信失败:

| 项目 | 格式 | 参考 |
|------|------|------|
| 字节序 | **小端序** (Little-Endian) | `binary_stream.hpp:endian::to_little()` |
| PackedInt | `<0xFE`: 1B, `0xFE`: 3B, `0xFF`: 5B | `binary_stream.hpp:write_packed_int()` |
| 字符串 | PackedInt(len) + UTF-8 字节 | `binary_stream.hpp:write_string()` |
| SessionKey | 32 字节不透明数据 | `entity_types.hpp:SessionKey` |
| MessageID | uint16_t | `message.hpp:MessageID` |
| EntityID | uint32_t | `entity_types.hpp:EntityID` |

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

---

## 12. 风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| Unity Roslyn 版本不兼容 SourceGenerator | 编译失败 | 检查 Unity 版本对应 Roslyn 版本，锁定 Generator 依赖 |
| iOS 静态链接符号冲突 | 链接错误 | 使用 `-fvisibility=hidden`，只导出 `atlas_net_*` 符号 |
| 回调线程安全 | 崩溃/数据竞争 | 所有回调在 `poll()` 内同步触发，与 Unity 主线程一致 |
| Atlas.Shared 的 Vector3 与 UnityEngine.Vector3 冲突 | 编译错误/混淆 | 条件编译或隐式转换运算符 |
| DLL 与服务端 ABI 版本不匹配 | 静默数据损坏 | `atlas_net_abi_version()` 启动时校验 |
