# Unity Native Network DLL 设计文档

> **状态**: 草案 v2 (已评审修订)
> **日期**: 2026-04-17
> **目标**: 将 C++ 网络层抽取为独立 native DLL, 供 Unity 客户端通过 P/Invoke 调用
>
> **v2 修订要点**:
> - 新增 §4.0 ABI 约定 (内存所有权 / 版本 / 重入 / 返回码)
> - §4.5 Login/Auth API 精简: SessionKey/baseapp 地址不跨 FFI, authenticate 参数从 4 降为 2
> - §4.5.4 新增 `atlas_net_disconnect` API
> - §4.5.6 状态转换矩阵
> - §5.2.1 SessionKey 生命周期与 `SecureZero` 清除
> - §5.2.2 Callback table 原子替换 + noop sentinel
> - §5.3 澄清客户端 RPC 线格式 (MessageID 位置编码, 无独立 rpc_id 字段)
> - §10 Phase 0: IL2CPP 可行性 Spike (前置所有其他 Phase)
> - §10 各 Phase 细化验证命令 (含 `bazel query` / ABI `static_assert`)
> - §3 / §9.3 构建系统全面 Bazel 化 (target 加 `atlas_` 前缀,
>   `cc_shared_library` 用 `roots`+`exports_filter`, 对齐
>   `docs/BAZEL_MIGRATION.md` 约定)
> - §12 风险表新增 6 项具体风险与缓解

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
├── network/machined_types.h → server/server_config.h (ProcessType 枚举)
├── login_messages.h → db/idatabase.h → entitydef/ (DatabaseID 类型)
└── baseapp_messages.h → db/idatabase.h → entitydef/
```

> **文件扩展名**: 项目头文件使用 `.h`, 源文件使用 `.cc` (见 `CLAUDE.md` /
> `.clang-format`)。以下示例保持此约定。

### 2.2 解耦方案

#### 2.2.1 ProcessType 枚举提取

`server/server_config.h` 包含 `ProcessType` 枚举和大量服务端配置（数据库、认证等），但 `machined_types.h` 只用到 `ProcessType`。

**方案**: 将 `ProcessType` 提取到 `foundation/process_type.h`。

```cpp
// src/lib/foundation/process_type.h (新文件)
#pragma once
#include <cstdint>
#include <string_view>

namespace atlas {

enum class ProcessType : uint8_t {
  kMachined = 0, kLoginApp = 1, kBaseApp = 2, kBaseAppMgr = 3,
  kCellApp = 4, kCellAppMgr = 5, kDBApp = 6, kDBAppMgr = 7, kReviver = 8,
};

[[nodiscard]] auto ProcessTypeName(ProcessType type) -> std::string_view;
[[nodiscard]] auto ProcessTypeFromName(std::string_view name) -> Result<ProcessType>;

}  // namespace atlas
```

> **命名说明**: 遵循项目 Google C++ Style（枚举值 `kPascalCase`、
> 函数 PascalCase）。原 `server/server_config.h` 中已是
> `ProcessTypeName` / `ProcessTypeFromName`，迁移到
> `foundation/process_type.h` 时保持同名，调用点无需改动。

**影响**:
- `server/server_config.h` 改为 `#include "foundation/process_type.h"`
- `network/machined_types.h` 改为 `#include "foundation/process_type.h"`
- `machined_types.h` 对 `server/` 的依赖消除

#### 2.2.2 DatabaseID 提取

`login_messages.h` 和 `baseapp_messages.h` 依赖 `db/idatabase.h` 仅为了 `DatabaseID` 类型别名。

**方案**: 将 `DatabaseID` 提取到 `server/entity_types.h`（已存在 `EntityID` 和 `SessionKey`）。

```cpp
// 添加到 src/lib/server/entity_types.h
using DatabaseID = int64_t;
inline constexpr DatabaseID kInvalidDBID = 0;
```

**影响**:
- `db/idatabase.h` 改为 `#include "server/entity_types.h"` 获取 DatabaseID
- `login_messages.h` 和 `baseapp_messages.h` 移除 `#include "db/idatabase.h"`
- 消除对 `entitydef/` 的传递依赖

#### 2.2.3 客户端消息头文件

DLL 只需要客户端相关的消息定义（LoginRequest/LoginResult/Authenticate/AuthenticateResult/ClientBaseRpc/ClientCellRpc）。这些消息定义在 `login_messages.h` 和 `baseapp_messages.h` 中，但两个文件还包含大量仅服务端使用的消息。

**方案**: 不拆分文件。完成 2.2.1 和 2.2.2 的解耦后，这两个头文件的依赖链变为：

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

## 3. Bazel 构建目标

> 项目使用 Bazel 作为唯一构建系统 (见 `CLAUDE.md` 与 `docs/BAZEL_MIGRATION.md`)。
> 本节约定对齐仓库现有惯例:
> - 所有 `cc_library` target 名加 `atlas_` 前缀 (例: `atlas_network`)
> - 源文件放在 `src/`、头文件放在 `include/`, `hdrs = glob(["include/**/*.h"])`,
>   `includes = ["include"]`
> - `cc_shared_library` 采用 `roots` + `exports_filter` 模式
>   (参考 `atlas_engine` 的实现)
> - 平台分支用 `select({"@platforms//os:windows": [...], ...})`

### 3.1 新增 Target: atlas_net_client

```python
# src/lib/net_client/BUILD.bazel

load("@rules_cc//cc:defs.bzl", "cc_library", "cc_shared_library")

# ---- 核心 cc_library ----
# 编译 Login/Auth 状态机和 C API 入口;
# 通过 cc_shared_library 组合为最终 DLL 之前单独做单元测试
cc_library(
    name = "atlas_net_client_core",
    srcs = [
        "src/client_api.cc",        # C API 导出层
        "src/client_session.cc",    # Login/Auth 状态机
    ],
    hdrs = glob(["include/**/*.h"]),
    includes = ["include"],
    defines = ["ATLAS_NET_CLIENT_EXPORTS"],
    copts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-fvisibility=hidden"],
    }),
    deps = [
        "//src/lib/network:atlas_network",
        "//src/lib/foundation:atlas_foundation",
        "//src/lib/platform:atlas_platform",
        # 关键: 只链接 binary 子集, 不拉入 pugixml / rapidjson (§3.3)
        "//src/lib/serialization:atlas_serialization_binary",
    ],
    visibility = ["//visibility:public"],
)

# ---- 最终共享库 ----
# 参考 src/lib/engine/BUILD.bazel 的 atlas_engine cc_shared_library 模式
cc_shared_library(
    name = "atlas_net_client",
    roots = [":atlas_net_client_core"],
    exports_filter = ["//src/lib/net_client/..."],  # 只导出本目录符号
    user_link_flags = select({
        "@platforms//os:linux":   ["-Wl,--exclude-libs,ALL"],
        "@platforms//os:macos":   ["-Wl,-dead_strip"],
        "//conditions:default":   [],
    }),
    shared_lib_name = select({
        "@platforms//os:windows": "atlas_net_client.dll",
        "@platforms//os:macos":   "libatlas_net_client.dylib",
        "//conditions:default":   "libatlas_net_client.so",
    }),
    # 默认不参与 //... 通配构建, 需显式 bazel build
    tags = ["manual"],
    visibility = ["//visibility:public"],
)

# ---- iOS 静态库变体 ----
# Apple 禁止第三方动态库, Unity iOS 构建走 [DllImport("__Internal")]
cc_library(
    name = "atlas_net_client_static",
    deps = [":atlas_net_client_core"],
    linkstatic = True,
    alwayslink = True,   # Unity 侧按名称查, 避免符号被 gc
    tags = ["manual"],
    visibility = ["//visibility:public"],
)
```

### 3.2 构建开关

Bazel 不像 CMake 有 `option()`; 项目已有的等价做法是:

a. **tags=["manual"]**: 让 target 默认不参与 `//...` 通配构建,
   开发者需显式 `bazel build //src/lib/net_client:atlas_net_client`。
   本设计首选此法 — net_client 属于 "可选产物", 默认不出现在
   服务端 CI 矩阵中。

b. **--define + config_setting**: 如需更细粒度控制 (例如同一 target
   在不同模式下选择不同实现), 用:

   ```python
   config_setting(
       name = "compression_enabled",
       define_values = {"ATLAS_COMPRESSION": "1"},
   )
   ```

   然后:
   ```bash
   bazel build //src/lib/net_client:atlas_net_client \
       --define=ATLAS_COMPRESSION=1
   ```

本设计建议: compression 作为 `config_setting` + `select` 控制,
其余通过 `manual` tag 控制可见性。

### 3.3 序列化模块拆分

当前 `src/lib/serialization/BUILD.bazel` 把 4 个 `.cc` 合入
`atlas_serialization`; 其中 `xml_parser.cc` 和 `json_parser.cc`
引入 pugixml / rapidjson 依赖, Unity 客户端 DLL 不需要。

**方案**: 新增 `atlas_serialization_binary` target (仅 `binary_stream.cc`,
无第三方依赖):

```python
# src/lib/serialization/BUILD.bazel (修改后)

cc_library(
    name = "atlas_serialization_binary",
    srcs = ["src/binary_stream.cc"],
    hdrs = ["include/serialization/binary_stream.h"],
    includes = ["include"],
    deps = [
        "//src/lib/foundation:atlas_foundation",
        "//src/lib/platform:atlas_platform",
    ],
    visibility = ["//visibility:public"],
)

# 原 atlas_serialization 保持不变, 服务端继续使用
cc_library(
    name = "atlas_serialization",
    srcs = [
        "src/binary_stream.cc",
        "src/data_section.cc",
        "src/xml_parser.cc",
        "src/json_parser.cc",
    ],
    hdrs = glob(["include/**/*.h"]),
    includes = ["include"],
    deps = [
        "//src/lib/foundation:atlas_foundation",
        "//src/lib/platform:atlas_platform",
        "@pugixml",
        "@rapidjson",
    ],
    visibility = ["//visibility:public"],
)
```

> **备选**: 也可让 `atlas_serialization` 以 `atlas_serialization_binary`
> 为 dep 避免 binary_stream.cc 的重复编译。是否这么做取决于
> `binary_stream.cc` 规模; 若较小, 冗余编译成本可忽略。

`atlas_net_client_core` 只依赖 `atlas_serialization_binary`,
这样 DLL 不会携带 pugixml / rapidjson 代码。

### 3.4 目录结构

```
src/lib/net_client/
├── BUILD.bazel
├── include/
│   └── net_client/
│       ├── net_client_export.h     # ATLAS_NET_API 宏 (§4.1)
│       ├── client_api.h            # C API 声明 (§4.2-4.9)
│       └── client_session.h        # Login/Auth 状态机 (§5.2)
└── src/
    ├── client_api.cc               # C API 实现
    └── client_session.cc           # 状态机实现
```

对应单元测试:

```
tests/unit/net_client/
├── BUILD.bazel
├── client_session_test.cc          # mock NetworkInterface (§10 Phase 3 验证 a)
├── abi_layout_test.cc              # sizeof/offsetof static_assert (§10 Phase 3 验证 e)
└── state_machine_test.cc           # §4.5.6 状态矩阵覆盖
```

```
tests/integration/net_client/
├── BUILD.bazel
└── client_flow_test.cc             # 真实 LoginApp + BaseApp + DBApp
```

---

## 4. C API 定义

### 4.0 ABI 约定 (Conventions)

以下契约对全部 C API 函数和回调生效。所有实现与调用者必须遵守；违反将导致未定义行为。

#### 4.0.1 String / Buffer 所有权

所有跨 FFI 的 `const char*` / `const uint8_t*` / `const void*` 均为
**非拥有视图 (non-owning view)**：

**C++ → C# 方向** (回调参数、`atlas_net_last_error` 返回值)
- 指针在**回调/函数返回后立即失效**
- C# 必须在回调内同步复制数据到托管堆
- 不得保存裸指针、不得跨 `atlas_net_poll()` 周期使用

```csharp
[UnmanagedCallersOnly]
static void OnRpc(uint entId, uint rpcId, byte* payload, int len) {
    // ✓ 立即复制
    byte[] copy = new Span<byte>(payload, len).ToArray();
    RpcQueue.Enqueue((entId, rpcId, copy));
    // ✗ 禁止: _savedPtr = payload;
}
```

**C# → C++ 方向** (`atlas_net_send_*`, `atlas_net_login` 等入参)
- C# 必须在调用期间保持 pinned，调用返回即可释放
- C++ 必须在函数返回前完成数据复制 (拷入 Bundle 或 `std::string`)

#### 4.0.2 Error 字符串生命周期

- `atlas_net_last_error(ctx)` 返回指针指向 ctx 内部 `std::string`
- 有效期至下一次作用于**同一 ctx** 的任何 API 调用之前
- `atlas_net_global_last_error()` (无 ctx 情形) 使用线程局部存储，
  有效期至本线程下一次 Atlas 调用之前

#### 4.0.3 回调可重入性

- 回调内**允许**调用 `atlas_net_send_*`, `atlas_net_get_state`,
  `atlas_net_get_stats`, `atlas_net_last_error`
- 回调内**禁止**调用 `atlas_net_create`, `atlas_net_destroy`,
  `atlas_net_poll`, `atlas_net_disconnect`, `atlas_net_login`,
  `atlas_net_authenticate`, `atlas_net_set_callbacks`
- `ATLAS_DEBUG` 构建下，禁用组合触发 assert；Release 下返回 `-EBUSY`

#### 4.0.4 Callback Table 初始化

- `atlas_net_create` 成功后，ctx 内置 noop 回调表，未注册即收消息
  不会 null 解引用
- `atlas_net_set_callbacks` 的结构体字段**不得为 NULL**；未使用的
  字段须填 `atlas_net_noop_*` sentinel (DLL 导出的空实现)
- 热替换：`atlas_net_set_callbacks` 在 poll 外任何时刻可调用，
  ctx 原子切换（使用 `std::atomic<AtlasNetCallbacks*>`）

#### 4.0.5 ABI 版本

```cpp
// 每次 ABI-breaking 变更递增；布局: [MAJOR:8][MINOR:8][PATCH:16]
#define ATLAS_NET_ABI_VERSION 0x01000000u   // v1.0.0
```

版本号变更规则:

| 变更类型 | bump | 行为 |
|----------|------|------|
| 函数签名变更 / 结构体布局变更 / 回调签名变更 | MAJOR | C# 必须同步发版 |
| 新增函数 / 新增回调 (结构体尾追加字段不算) | MINOR | 向后兼容，C# 可延后升级 |
| 仅实现修复，不改 ABI | PATCH | 不校验 |

**校验点**: `atlas_net_create(expected_abi)` 执行
`caller_major != our_major || caller_minor > our_minor` 检查，
失败返回 NULL 并通过 `atlas_net_global_last_error()` 暴露原因。

#### 4.0.6 线程模型

- 每个 `AtlasNetContext` 仅允许被**单一线程**访问（Unity 主线程）
- 多 ctx 并发允许，但不共享任何状态
- 日志 sink (`atlas_net_set_log_handler`) 是**进程级**全局状态，
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
| `-EABI` (-1000, 自定义) | ABI 版本不匹配 (仅 `atlas_net_create`) |

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
typedef struct AtlasNetChannel AtlasNetChannel;

// ABI 版本查询 (C# 诊断用, 不用于校验)
ATLAS_NET_CALL uint32_t atlas_net_abi_version(void);

// 错误信息
ATLAS_NET_CALL const char* atlas_net_last_error(AtlasNetContext* ctx);

// 无 ctx 时的错误信息 (atlas_net_create 失败时使用, 线程局部)
ATLAS_NET_CALL const char* atlas_net_global_last_error(void);
```

### 4.3 生命周期

```cpp
// 创建网络上下文 (内部创建 EventDispatcher + NetworkInterface)。
// expected_abi: 调用方编译时的 ATLAS_NET_ABI_VERSION,
// 与 DLL 内部版本按 §4.0.5 规则校验,失败返回 NULL。
//   - NULL 返回时用 atlas_net_global_last_error() 取原因
//   - 成功返回时 ctx 已安装 noop 回调表,立即可安全使用
ATLAS_NET_CALL AtlasNetContext* atlas_net_create(uint32_t expected_abi);

// 销毁 (断开所有连接, 清零 SessionKey, 释放资源)
ATLAS_NET_CALL void atlas_net_destroy(AtlasNetContext* ctx);
```

**C# 使用范式**:

```csharp
public static class AtlasNet {
    const uint kExpectedAbi = 0x01000000u;  // 与 C 头文件同步

    public static nint Create() {
        var ctx = AtlasNetNative.atlas_net_create(kExpectedAbi);
        if (ctx == 0) {
            var err = Marshal.PtrToStringUTF8(
                AtlasNetNative.atlas_net_global_last_error());
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
ATLAS_NET_CALL int32_t atlas_net_poll(AtlasNetContext* ctx);
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
> `atlas_net_authenticate` 无需由 C# 回传。这一原则:
> - 缩小 SessionKey 的暴露面 (托管堆扫描、调试器、core dump 均无法看到)
> - 消除两侧状态同步负担
> - 简化 API (authenticate 从 4 参降为 2 参)

#### 4.5.1 回调类型

```cpp
// ---- 登录结果回调 ----
// status 枚举: 见 AtlasLoginStatus
// baseapp_host / baseapp_port: 仅供 UI 展示,不需回传给 authenticate
// user_data: atlas_net_login 时传入的用户指针,回调原样带回
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
//   5. atlas_net_poll() 驱动,收到 LoginResult 后:
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
ATLAS_NET_CALL int32_t atlas_net_login(
    AtlasNetContext*    ctx,
    const char*         loginapp_host,
    uint16_t            loginapp_port,
    const char*         username,
    const char*         password_hash,
    AtlasLoginResultFn  callback,
    void*               user_data        // 透传回 callback, 可为 NULL
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
ATLAS_NET_CALL int32_t atlas_net_authenticate(
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
// 调用后允许重新 atlas_net_login() 用新凭证登录。
ATLAS_NET_CALL int32_t atlas_net_disconnect(
    AtlasNetContext*      ctx,
    AtlasDisconnectReason reason);
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

ATLAS_NET_CALL AtlasNetState atlas_net_get_state(AtlasNetContext* ctx);
```

#### 4.5.6 状态转换矩阵

所有 C API 入口**首行**检查 state。非法调用**不**隐式断开、不改状态，
仅返回错误码 (`ATLAS_DEBUG` 下额外 log warn)，保持状态机确定性。

| From \ 调用        | login    | authenticate | send_*rpc | disconnect | destroy |
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
       │                  login()                       │
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
       │   authenticate()                               │
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
       │   disconnect() / error / server close          │
       │      │                                         │
       └──────┘                                         │
                                                        │
  (任何状态) atlas_net_disconnect() ────────────────────┘
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
| `atlas_net_abi_version` | - | ABI 版本查询 (诊断用) |
| `atlas_net_last_error` | - | 获取 ctx 内最后错误 (view 语义,§4.0.2) |
| `atlas_net_global_last_error` | - | 无 ctx 时的错误 (TLS,仅用于 create 失败后) |
| `atlas_net_create` | C#→C++ | 创建网络上下文 (带 ABI 校验,§4.0.5) |
| `atlas_net_destroy` | C#→C++ | 销毁网络上下文 |
| `atlas_net_poll` | C#→C++ | **每帧调用, 驱动网络层** |
| `atlas_net_set_callbacks` | C#→C++ | 注册事件回调 (原子热替换) |
| `atlas_net_set_log_handler` | C#→C++ | 注册日志回调 (进程级) |
| `atlas_net_login` | C#→C++ | 发起登录 (带 user_data) |
| `atlas_net_authenticate` | C#→C++ | 发起认证 (无 host/key 参数,DLL 自持) |
| `atlas_net_disconnect` | C#→C++ | 关闭连接,保留 ctx 和回调表 |
| `atlas_net_get_state` | C#→C++ | 查询连接状态 (含 LoginSucceeded) |
| `atlas_net_send_base_rpc` | C#→C++ | 发送 Base RPC |
| `atlas_net_send_cell_rpc` | C#→C++ | 发送 Cell RPC |
| `atlas_net_get_stats` | C#→C++ | 获取网络统计 |
| `atlas_net_noop_rpc` 等 | 内部 | sentinel 空回调 (set_callbacks 占位) |
| `AtlasLoginResultFn` | C++→C# | 登录结果通知 (user_data + status + baseapp addr) |
| `AtlasAuthResultFn` | C++→C# | 认证结果通知 (user_data + entity_id + type_id) |
| `AtlasRpcCallbackFn` | C++→C# | RPC 消息分发 |
| `AtlasEntityCreateFn` | C++→C# | 实体创建通知 |
| `AtlasEntityDestroyFn` | C++→C# | 实体销毁通知 |
| `AtlasDisconnectFn` | C++→C# | 连接断开通知 (LOGOUT/超时/服务端关闭/错误) |
| `AtlasLogFn` | C++→C# | 日志转发 |

---

## 5. DLL 内部实现

### 5.1 AtlasNetContext 结构

```
AtlasNetContext (不透明, C++ 内部)
├── EventDispatcher dispatcher_
├── NetworkInterface network_{dispatcher_}
├── ClientSession session_                    // 封装 Login/Auth 状态机
│   ├── state_: AtlasNetState (含 kLoginSucceeded)
│   ├── loginapp_channel_: Channel*           // 仅 LoggingIn 期间非空
│   ├── baseapp_channel_: Channel*            // Authenticating 后非空
│   ├── session_key_: std::array<uint8_t,32>  // DLL 内部持有, 不暴露
│   ├── baseapp_addr_: Address                // LoginResult 后缓存
│   ├── player_entity_id_: EntityID
│   ├── player_type_id_: uint16_t
│   ├── login_callback_: AtlasLoginResultFn + user_data
│   └── auth_callback_:  AtlasAuthResultFn  + user_data
├── callbacks_: std::atomic<AtlasNetCallbacks>  // 原子热替换, 初始为 noop 表
├── abi_version_: uint32_t                    // 保存 create 时传入的版本 (诊断用)
└── last_error_: std::string                  // ctx 内最后错误信息
```

日志/全局错误不在 ctx 内:
- `log_handler_`: 进程级 `std::atomic<AtlasLogFn>` (§4.8)
- `global_last_error`: `thread_local std::string` (仅 create 失败路径写入)

### 5.2 ClientSession 状态机

`ClientSession` 是新增的 C++ 类，封装了原来 `ClientApp` 中的 login/authenticate 流程。

核心逻辑从 `client_app.cc` 提取 (参考 `src/client/client_app.cc:210-294` 的
`Login()` 和 `Authenticate()`):

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
  - `atlas_net_disconnect()` → `ClearSessionKey()`
  - `atlas_net_destroy()` → 随 ctx 析构自动清零
  - 成功登录后保留，为未来的断线重连 (Phase 2) 预留
- **禁止跨 FFI**: SessionKey 永不穿越 C API; C# 不感知其存在
- **内存**: 析构与显式清零都用 `SecureZeroMemory` 或等价物,
  防止 core dump / 进程 snapshot 泄漏

```cpp
// src/lib/platform/secure_memory.h (新增)
#pragma once
#include <cstddef>

namespace atlas {
// 保证不被编译器优化掉的零填充 (防止敏感数据残留)
inline void SecureZero(void* p, std::size_t n) {
#if defined(_WIN32)
  ::SecureZeroMemory(p, n);
#elif defined(__STDC_LIB_EXT1__)
  ::memset_s(p, n, 0, n);
#elif defined(__GLIBC__) || defined(__APPLE__)
  ::explicit_bzero(p, n);
#else
  volatile unsigned char* vp = static_cast<volatile unsigned char*>(p);
  while (n--) *vp++ = 0;
#endif
}
}  // namespace atlas

// src/lib/net_client/client_session.h
class ClientSession {
  std::array<uint8_t, 32> session_key_{};
  // ...
 public:
  ~ClientSession() { ClearSessionKey(); }
  void ClearSessionKey() { SecureZero(session_key_.data(), 32); }
};
```

#### 5.2.2 Callback Table 原子热替换

`callbacks_` 使用 `std::atomic<AtlasNetCallbacks>` (若结构体可平凡)
或 `std::atomic<std::shared_ptr<AtlasNetCallbacks>>`。

`atlas_net_create` 成功返回前必须已写入一份 noop 表:

```cpp
// src/lib/net_client/client_api.cc
static void NoopRpc(uint32_t, uint32_t, const uint8_t*, int32_t) {}
static void NoopEntityCreate(uint32_t, uint16_t) {}
static void NoopEntityDestroy(uint32_t) {}
static void NoopDisconnect(int32_t) {}

AtlasNetContext* atlas_net_create(uint32_t expected_abi) {
  // ... ABI 校验 ...
  auto* ctx = new AtlasNetContext{/* ... */};
  AtlasNetCallbacks noop{
    .on_rpc = &NoopRpc,
    .on_entity_create = &NoopEntityCreate,
    .on_entity_destroy = &NoopEntityDestroy,
    .on_disconnect = &NoopDisconnect,
  };
  ctx->callbacks_.store(noop, std::memory_order_release);
  return ctx;
}
```

同时导出 `atlas_net_noop_*` sentinel,让 C# 可以显式填写未使用字段,
`set_callbacks` 见到 NULL 字段直接返回 `-EINVAL`。

### 5.3 消息分发流程

```
atlas_net_poll() 调用
  │
  ▼
EventDispatcher::ProcessOnce()
  ├── IOPoller::Poll() → 读取网络数据
  ├── ReliableUdpChannel → 解包 RUDP → 提取 Bundle
  ├── InterfaceTable::Dispatch()
  │   ├── 匹配 Typed Handler (LoginResult, AuthenticateResult)
  │   │   └── ClientSession 内部处理 → 触发 login/auth 回调
  │   └── Default Handler (RPC 消息, 见下方注解)
  │       └── 将 MessageID 升级为 uint32_t rpc_id
  │           └── 调用 callbacks_.on_rpc(entity_id, rpc_id, payload, len)
  └── TimerQueue::Process() → 重传、心跳等
```

#### 5.3.1 服务器→客户端 RPC 的线格式

**重要**: 服务器下行的 RPC 消息**不是**一个单独的"ClientRpc"消息带
`rpc_id` 字段；服务器 (BaseApp) 直接将 `rpc_id` 用作消息的 `MessageID`
发出 (见 `src/server/baseapp/baseapp.cc:610-625`
`SendMessage(static_cast<MessageID>(rpc_id), payload)`)。

客户端的 `InterfaceTable` 无法为服务器定义的所有 rpc_id 注册 typed
handler，因此在完成 authentication 后注册一个 **Default Handler**:

```cpp
// 等价于 src/client/client_app.cc:323-337 的现有实现
network_.InterfaceTable().SetDefaultHandler(
    [this](const Address&, Channel*, MessageID msg_id, BinaryReader& reader) {
      // msg_id 即 rpc_id (uint16 升 uint32)
      auto remaining = reader.Remaining();
      auto payload = remaining > 0 ? reader.ReadBytes(remaining) : std::nullopt;
      callbacks_.load().on_rpc(
          player_entity_id_,
          static_cast<uint32_t>(msg_id),
          payload ? reinterpret_cast<const uint8_t*>(payload->data()) : nullptr,
          payload ? static_cast<int32_t>(payload->size()) : 0);
    });
```

约束:
- Default Handler 只在 `kConnected` 状态注册; `disconnect()` 时反注册
- `entity_id` 总是填当前玩家 `player_entity_id_`; 多实体 RPC (Aoi 实体)
  由上层 C# 在 payload 中自行解析
- payload 指针在回调返回后失效 (§4.0.1)

#### 5.3.2 为什么不在 Bundle 中加显式 rpc_id 字段？

- 省 2-4 字节/包 (每秒数百 RPC 时可观)
- 与服务器现有协议兼容, 不必改 BaseApp 发送逻辑
- `MessageID` 空间 (uint16, 65536 个) 对客户端 RPC 足够

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

> **前置**: 若 P0-1 IL2CPP Spike (§10 Phase 0) 结论为方案 A 可行,则使用
> 下方 `[UnmanagedCallersOnly]` + 函数指针风格。若 Spike 结论为必须
> 退回方案 B,则所有回调改为 `[MonoPInvokeCallback] delegate + GCHandle`
> 模式,Marshal.GetFunctionPointerForDelegate 取 nint。

```csharp
// Atlas.Client/Native/AtlasNetNative.cs
using System.Runtime.InteropServices;

internal static unsafe partial class AtlasNetNative
{
#if UNITY_IOS && !UNITY_EDITOR
    private const string LibName = "__Internal";
#else
    private const string LibName = "atlas_net_client";
#endif

    // --- 版本 / 错误 ---
    [LibraryImport(LibName)] internal static partial uint atlas_net_abi_version();
    [LibraryImport(LibName)] internal static partial nint atlas_net_last_error(nint ctx);
    [LibraryImport(LibName)] internal static partial nint atlas_net_global_last_error();

    // --- 生命周期 ---
    [LibraryImport(LibName)]
    internal static partial nint atlas_net_create(uint expectedAbi);

    [LibraryImport(LibName)]
    internal static partial void atlas_net_destroy(nint ctx);

    // --- Tick ---
    [LibraryImport(LibName)] internal static partial int atlas_net_poll(nint ctx);

    // --- 回调 ---
    [LibraryImport(LibName)]
    internal static partial int atlas_net_set_callbacks(
        nint ctx, AtlasNetCallbacks* callbacks);

    [LibraryImport(LibName)]
    internal static partial void atlas_net_set_log_handler(nint handler);

    // --- Login/Auth/Disconnect (新 API:无 host/key 参数传回) ---
    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int atlas_net_login(
        nint ctx, string loginappHost, ushort loginappPort,
        string username, string passwordHash,
        nint callback, nint userData);

    [LibraryImport(LibName)]
    internal static partial int atlas_net_authenticate(
        nint ctx, nint callback, nint userData);

    [LibraryImport(LibName)]
    internal static partial int atlas_net_disconnect(nint ctx, int reason);

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
}

// 薄封装,把 ABI 校验和错误转异常收拢到一处
public static class AtlasNet {
    const uint kExpectedAbi = 0x01000000u;  // 与 C 头文件同步

    public static nint Create() {
        var ctx = AtlasNetNative.atlas_net_create(kExpectedAbi);
        if (ctx == 0) {
            var errPtr = AtlasNetNative.atlas_net_global_last_error();
            var err = errPtr == 0 ? "unknown"
                                  : Marshal.PtrToStringUTF8(errPtr);
            throw new InvalidOperationException(
                $"atlas_net_client DLL 初始化失败 (ABI 预期={kExpectedAbi:X8}): {err}");
        }
        return ctx;
    }
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

### 6.3 回调注册

```csharp
internal static unsafe class AtlasNetCallbackBridge
{
    // ---- 事件回调 (消息/实体/断开) ----
    // 所有指针/字节数据在本方法返回后失效 (§4.0.1), 必须立即复制。
    [UnmanagedCallersOnly]
    static void OnRpc(uint entityId, uint rpcId, byte* payload, int len) {
        byte[] copy = len > 0 ? new Span<byte>(payload, len).ToArray()
                              : Array.Empty<byte>();
        RpcDispatcher.Enqueue(entityId, rpcId, copy);
    }

    [UnmanagedCallersOnly]
    static void OnEntityCreate(uint entityId, ushort typeId) {
        ClientEntityManager.Instance.HandleCreate(entityId, typeId);
    }

    [UnmanagedCallersOnly]
    static void OnEntityDestroy(uint entityId) {
        ClientEntityManager.Instance.HandleDestroy(entityId);
    }

    [UnmanagedCallersOnly]
    static void OnDisconnect(int reason) {
        AtlasNetworkManager.Instance.HandleDisconnect(reason);
    }

    internal static void Register(nint ctx)
    {
        AtlasNetCallbacks table;
        table.OnRpc = (nint)(delegate* unmanaged<uint, uint, byte*, int, void>)&OnRpc;
        table.OnEntityCreate = (nint)(delegate* unmanaged<uint, ushort, void>)&OnEntityCreate;
        table.OnEntityDestroy = (nint)(delegate* unmanaged<uint, void>)&OnEntityDestroy;
        table.OnDisconnect = (nint)(delegate* unmanaged<int, void>)&OnDisconnect;
        int rc = AtlasNetNative.atlas_net_set_callbacks(ctx, &table);
        if (rc != 0) throw new InvalidOperationException(
            $"atlas_net_set_callbacks failed: {rc}");
    }
}

// ---- Login / Auth 回调 ----
// 注意: Login/Auth 回调通过 nint 指针单独传入, 不走 callbacks 结构体。
// user_data 是 GCHandle 的 IntPtr, 让 C++ 透传回来, 便于定位 caller。
internal static unsafe class AtlasNetLoginBridge
{
    [UnmanagedCallersOnly]
    static void OnLoginResult(nint userData, byte status,
                              byte* baseappHost, ushort baseappPort,
                              byte* errorMessage)
    {
        // 通过 userData (GCHandle) 取回原始 AtlasNetworkManager 实例
        var handle = GCHandle.FromIntPtr(userData);
        var mgr = (AtlasNetworkManager)handle.Target!;
        string? host = baseappHost != null
            ? Marshal.PtrToStringUTF8((nint)baseappHost) : null;
        string? err = errorMessage != null
            ? Marshal.PtrToStringUTF8((nint)errorMessage) : null;
        handle.Free();  // 一次性 handle
        mgr.OnLoginCompleted((AtlasLoginStatus)status, host, baseappPort, err);
    }

    internal static nint Pointer =>
        (nint)(delegate* unmanaged<nint, byte, byte*, ushort, byte*, void>)&OnLoginResult;
}

internal static unsafe class AtlasNetAuthBridge
{
    [UnmanagedCallersOnly]
    static void OnAuthResult(nint userData, byte success,
                             uint entityId, ushort typeId, byte* errorMessage)
    {
        var handle = GCHandle.FromIntPtr(userData);
        var mgr = (AtlasNetworkManager)handle.Target!;
        string? err = errorMessage != null
            ? Marshal.PtrToStringUTF8((nint)errorMessage) : null;
        handle.Free();
        mgr.OnAuthCompleted(success != 0, entityId, typeId, err);
    }

    internal static nint Pointer =>
        (nint)(delegate* unmanaged<nint, byte, uint, ushort, byte*, void>)&OnAuthResult;
}

public enum AtlasLoginStatus : byte {
    Success = 0, InvalidCredentials = 1, AlreadyLoggedIn = 2,
    ServerFull = 3, Timeout = 4, NetworkError = 5, InternalError = 255,
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
public unsafe class AtlasNetworkManager : MonoBehaviour
{
    public static AtlasNetworkManager Instance { get; private set; }

    [SerializeField] private AtlasNetworkConfig config;

    private nint _ctx;

    public event Action<AtlasLoginStatus, string?> OnLoginFinished;
    public event Action<bool, uint, ushort, string?> OnAuthFinished;
    public event Action<uint, ushort> OnEntityCreated;
    public event Action<uint> OnEntityDestroyed;
    public event Action<int> OnDisconnected;

    void Awake()
    {
        Instance = this;
        _ctx = AtlasNet.Create();   // 内部带 ABI 校验, 失败抛异常 (§6.1)

        AtlasNetNative.atlas_net_set_log_handler(
            (nint)(delegate* unmanaged<int, byte*, int, void>)&LogBridge.OnLog);
        AtlasNetCallbackBridge.Register(_ctx);
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
            Instance = null!;
        }
    }

    // ---- 公开 API ----

    public void Login(string username, string passwordHash)
    {
        // GCHandle 让 C++ 透传定位回具体实例 (§4.5.2 user_data 参数)
        var h = GCHandle.Alloc(this);
        int rc = AtlasNetNative.atlas_net_login(
            _ctx, config.loginappHost, config.loginappPort,
            username, passwordHash,
            AtlasNetLoginBridge.Pointer, GCHandle.ToIntPtr(h));
        if (rc != 0) { h.Free(); throw new InvalidOperationException($"login rc={rc}"); }
    }

    // 通常在 OnLoginFinished 的 Success 分支内调用
    public void Authenticate()
    {
        var h = GCHandle.Alloc(this);
        int rc = AtlasNetNative.atlas_net_authenticate(
            _ctx, AtlasNetAuthBridge.Pointer, GCHandle.ToIntPtr(h));
        if (rc != 0) { h.Free(); throw new InvalidOperationException($"auth rc={rc}"); }
    }

    public void Logout() {
        AtlasNetNative.atlas_net_disconnect(_ctx, (int)AtlasDisconnectReason.Logout);
    }

    public void SendBaseRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload) {
        fixed (byte* p = payload)
            AtlasNetNative.atlas_net_send_base_rpc(_ctx, entityId, rpcId, p, payload.Length);
    }

    public AtlasNetStats GetStats() {
        AtlasNetStats s;
        AtlasNetNative.atlas_net_get_stats(_ctx, &s);
        return s;
    }

    // ---- Bridge 回调入口 ----
    internal void OnLoginCompleted(AtlasLoginStatus status, string? host, ushort port, string? err)
        => OnLoginFinished?.Invoke(status, err);
    internal void OnAuthCompleted(bool success, uint entityId, ushort typeId, string? err)
        => OnAuthFinished?.Invoke(success, entityId, typeId, err);
    internal void HandleDisconnect(int reason)
        => OnDisconnected?.Invoke(reason);
}

public enum AtlasDisconnectReason : int { User = 0, Logout = 1, Internal = 2 }
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

| 平台 | 库类型 | IO 后端 | 构建工具链 | 注意事项 |
|------|--------|---------|-----------|----------|
| Windows x64 | .dll | WSAPoll | Bazel + MSVC | 主开发平台 |
| Android arm64 | .so | epoll | Bazel + rules_android_ndk | Unity IL2CPP, NDK r25+ |
| Android armv7 | .so | epoll | Bazel + rules_android_ndk | 渐淘汰, 可选 |
| iOS arm64 | .a (静态) | select | Bazel + rules_apple | Apple 禁止第三方动态库 |
| macOS arm64 | .bundle | kqueue/select | Bazel + rules_apple | 开发调试用 |
| Linux x64 | .so | epoll | Bazel + GCC/Clang | 服务端/CI |

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

### 9.3 Bazel 交叉编译

Bazel 通过 `--platforms` 与 `--config` 驱动交叉编译。前置要求:
- `MODULE.bazel` 已注册 `rules_android_ndk` (Android) 和
  `rules_apple` / `apple_support` (iOS/macOS) 依赖
- `.bazelrc` 定义各目标的 `--config` 条目 (见下方参考)

Bazel 构建命令 (target 名 `atlas_net_client` 由 §3.1 定义):

```bash
# Windows x64 (原生构建, 开发主平台)
bazel build //src/lib/net_client:atlas_net_client

# Linux x64 (服务端 / CI)
bazel build //src/lib/net_client:atlas_net_client \
    --config=linux_x86_64

# Android arm64 (Unity 主力 Android 目标)
bazel build //src/lib/net_client:atlas_net_client \
    --config=android_arm64

# Android armv7 (可选, 渐淘汰)
bazel build //src/lib/net_client:atlas_net_client \
    --config=android_armv7

# iOS arm64 — 使用静态库 target (§3.1)
# 原因: Apple 禁止第三方 dylib, Unity iOS 用 [DllImport("__Internal")]
bazel build //src/lib/net_client:atlas_net_client_static \
    --config=ios_arm64

# macOS arm64 (开发调试, Unity 编辑器使用)
bazel build //src/lib/net_client:atlas_net_client \
    --config=macos_arm64
```

`.bazelrc` 片段参考 (具体 platform 定义在 `//platforms:BUILD.bazel`):

```
# --- Android NDK (rules_android_ndk) ---
build:android_arm64 --platforms=//platforms:android_arm64
build:android_arm64 --android_platforms=//platforms:android_arm64
build:android_armv7 --platforms=//platforms:android_armv7
build:android_armv7 --android_platforms=//platforms:android_armv7

# --- iOS / macOS (rules_apple) ---
build:ios_arm64     --platforms=@build_bazel_apple_support//configs:ios_arm64
build:ios_arm64     --apple_platform_type=ios
build:macos_arm64   --platforms=@build_bazel_apple_support//configs:darwin_arm64

# --- Linux 交叉 (若从 Windows/macOS 构建 Linux so, 可选) ---
build:linux_x86_64  --platforms=@platforms//cpu:x86_64+@platforms//os:linux
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

## 10. 实施步骤

### Phase 0: IL2CPP 可行性 Spike (预计 1 天) ⚠ 必须先于其他 Phase

**目标**: 在投入深度重构前, 确认 Unity IL2CPP 能正确触发 C# 函数指针回调。
这直接决定移动端方案: 若 `[UnmanagedCallersOnly]` 在 IL2CPP 下失效,
§6.3 的全部回调桥接代码需要重写为 `[MonoPInvokeCallback] + delegate`。

**产出** (独立分支, 不合入主干):

1. **最小 native 探测库** `atlas_il2cpp_probe`
   - 只导出 2 个符号:
     ```cpp
     extern "C" ATLAS_NET_API void probe_set_callback(void (*cb)(int32_t));
     extern "C" ATLAS_NET_API void probe_fire(int32_t value);
     ```
   - 用同一 Bazel toolchain 交叉编译 Windows / Android-arm64 / iOS-arm64 三份

2. **Unity 测试工程** (Unity 2022.3 LTS 作为下限基准)
   - 场景中挂载 `ProbeComponent`, 调 `probe_set_callback(&OnFire)` 然后 `probe_fire(42)`
   - 预期 Debug.Log "fired with 42"
   - 四个目标分别出包验证:
     - Editor Mono (基线)
     - Standalone Windows IL2CPP
     - Android arm64 IL2CPP
     - iOS arm64 IL2CPP (静态库 + `[DllImport("__Internal")]`)

3. **两套方案对比代码**
   ```csharp
   // 方案 A: [UnmanagedCallersOnly] + 函数指针 (.NET 5+ 原生风格)
   [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
   static void OnProbeA(int v) { Debug.Log($"A fired {v}"); }

   // 方案 B: MonoPInvokeCallback + delegate (IL2CPP 兼容风格)
   delegate void ProbeDelegate(int v);
   [MonoPInvokeCallback(typeof(ProbeDelegate))]
   static void OnProbeB(int v) { Debug.Log($"B fired {v}"); }
   ```

**决策门槛**:

| Spike 结果 | 行动 |
|-----------|------|
| 方案 A 在全部 4 个目标都触发回调 | 采用 A（符合当前设计文档）, 进入 Phase 1 |
| 方案 A 仅 Mono/.NET 可用, IL2CPP 需要 B | 设计文档 §6.3 全部回调改 delegate + GCHandle 模式, 然后进入 Phase 1 |
| 两方案均失败 | 提升 Unity 最低版本 (2023 LTS), 重跑; 仍失败则改走 reverse P/Invoke + 自定义 trampoline |

**产物**:
- `docs/spike_il2cpp_callback.md` 记录矩阵结果和最终选型
- 若选方案 B, 同步更新本设计文档 §6.1 / §6.3
- 设计文档 §9.1 兼容性矩阵增列 "Unity 最低版本" 和 "C# 回调模式"

### Phase 1: 依赖解耦 (预计 1-2 天)

**目标**: 让 network 模块可以不依赖 server/db/entitydef 独立编译。

1. **提取 ProcessType**
   - 创建 `src/lib/foundation/process_type.h` (项目使用 `.h` 不是 `.hpp`)
   - 将 `ProcessType` 枚举及 `ProcessTypeName()` / `ProcessTypeFromName()`
     从 `src/lib/server/server_config.h` 移到新文件 (命名保持 PascalCase)
   - `server/server_config.h` 改为 include 新文件
   - `network/machined_types.h` 改为 include 新文件

2. **提取 DatabaseID**
   - 在 `src/lib/server/entity_types.h` 中添加 `DatabaseID` 类型别名
   - `src/lib/db/idatabase.h` 改为从 `entity_types.h` 获取 DatabaseID
   - `src/server/loginapp/login_messages.h` 和 `baseapp_messages.h` 移除
     `#include "db/idatabase.h"`, 确认只需 `#include "server/entity_types.h"`

3. **验证** (每一步都必须过)

   a. 目标独立编译:
      ```bash
      bazel build //src/lib/network:atlas_network
      ```

   b. 依赖图检查 — 用 `bazel query` 列出传递依赖, 确认
      `atlas_network` 不再闭包到 `atlas_server` / `atlas_db` / `atlas_entitydef`:
      ```bash
      bazel query "deps(//src/lib/network:atlas_network)" --output=package \
          | sort -u
      # 预期输出只含: src/lib/foundation, src/lib/platform,
      # src/lib/serialization (及可选 third_party/zlib)
      # 禁止出现: src/lib/server, src/lib/db, src/lib/entitydef
      ```

      进阶, 用 `somepath` 定位残留路径:
      ```bash
      bazel query "somepath(//src/lib/network:atlas_network, \
                            //src/lib/server:atlas_server)"
      # 期望输出: Empty results.
      ```

   c. 全量测试不回退:
      ```bash
      bazel test //tests/unit:all --test_output=errors
      bazel test //tests/integration:all --test_output=errors
      ```

   d. 格式检查:
      ```bash
      clang-format --dry-run --Werror <修改的 .h/.cc 文件>
      ```

### Phase 2: 序列化模块拆分 (预计 0.5 天)

1. **创建 `atlas_serialization_binary` target**
   - 修改 `src/lib/serialization/BUILD.bazel`
   - 拆出 `cc_library(name = "serialization_binary", srcs = ["binary_stream.cc"], ...)`,
     不链接 pugixml/rapidjson
   - 现有 `serialization` target 保持不变 (继续包含全部 4 个 .cc)

2. **验证**
   - 独立编译:
     ```bash
     bazel build //src/lib/serialization:atlas_serialization_binary
     ```
   - 最小 smoke test: 在 `tests/unit/serialization/BUILD.bazel` 添加
     `test_binary_stream_only` target 仅依赖 `atlas_serialization_binary`,
     能编译使用 `BinaryReader / BinaryWriter` 的 hello_world.cc
   - 依赖不回溯到 pugixml / rapidjson:
     ```bash
     bazel query "deps(//src/lib/serialization:atlas_serialization_binary)" \
         | grep -E "pugixml|rapidjson"
     # 期望: 无任何匹配
     ```

### Phase 3: C API 导出层 (预计 3-4 天)

1. **创建目录和文件**
   - `src/lib/net_client/` 目录
   - `net_client_export.h` — 导出宏定义 (§4.1)
   - `client_session.h` / `client_session.cc` — Login/Auth 状态机
     (从 `src/client/client_app.cc:210-294` 提取并异步化)
   - `client_api.h` — C API 头文件 (公开接口)
   - `client_api.cc` — C API 实现
   - `BUILD.bazel` — `cc_shared_library` 构建配置

2. **实现 ClientSession** (§5.2 详细行为)
   - 封装 `EventDispatcher` + `NetworkInterface`
   - 实现 Login 状态机 (ConnectRudp → SendMessage<LoginRequest> → Handle LoginResult)
   - 实现 Auth 状态机 (ConnectRudp → SendMessage<Authenticate> → Handle AuthenticateResult)
   - 实现 RPC Default Handler (§5.3.1)
   - `SecureZero` 清除 SessionKey (§5.2.1)

3. **实现 C API**
   - 生命周期: `atlas_net_create(abi)` / `atlas_net_destroy`
   - Tick: `atlas_net_poll`
   - Login/Auth/Disconnect: §4.5
   - 消息: `atlas_net_send_base_rpc` / `atlas_net_send_cell_rpc`
   - 回调: `atlas_net_set_callbacks` (原子替换) / `atlas_net_set_log_handler`
   - 诊断: `atlas_net_get_stats` / `atlas_net_get_state` / `atlas_net_last_error`
   - Sentinel: `atlas_net_noop_rpc` 等 (§5.2.2)

4. **验证** — 三类测试必须齐备

   a. 单元测试 (`tests/unit/net_client/client_session_test.cc`)
      - 用 `MockNetworkInterface` 驱动 `ClientSession` 状态机
      - 覆盖用例:
        - Login 成功 → LoginSucceeded
        - Login 超时 → Disconnected
        - Login 期间服务端关闭 → on_disconnect 触发
        - Auth 成功 → Connected
        - Auth 失败 → ClearSessionKey 被调用, 状态 Disconnected
        - 状态矩阵 (§4.5.6) 所有非法调用返回预期错误码
        - Disconnect 幂等性
      ```bash
      bazel test //tests/unit/net_client:client_session_test --test_output=all
      ```

   b. 集成测试 (`tests/integration/net_client/client_flow_test.cc`)
      - 启动真实 LoginApp + BaseApp + DBApp (复用现有 integration fixture)
      - 驱动完整 C API: `atlas_net_create` → `login` → `authenticate` → RPC → `disconnect` → `destroy`
      - 验证端到端线格式兼容性
      ```bash
      bazel test //tests/integration/net_client:client_flow_test --test_output=errors
      ```

   c. FFI 验证 (独立 .NET 控制台 demo, 不依赖 Unity)
      - `tools/net_client_demo/Program.cs` — 最小可运行 demo
      - 构建与运行:
        ```bash
        bazel build //src/lib/net_client:atlas_net_client
        dotnet run --project tools/net_client_demo/net_client_demo.csproj
        ```
      - 手工验证 P/Invoke 路径: `Create(abi)` → `Login` → `Authenticate`
        → 收 1 条 RPC → `Disconnect` → `Destroy`
      - 确认回调从 C++ → C# 正常触发, GCHandle/user_data 正确回传
      - ABI 版本不匹配时 `Create()` 抛异常而非崩溃 (故意用错版本号验证)

   d. 导出符号清单审核 (Windows)
      ```bash
      bazel build //src/lib/net_client:atlas_net_client
      dumpbin /exports bazel-bin/src/lib/net_client/atlas_net_client.dll \
          | grep "atlas_net_"
      # 对照 §4.10 表, 确认所有 API 均已导出且无多余符号
      ```

      Linux/macOS 对应:
      ```bash
      nm -D --defined-only bazel-bin/src/lib/net_client/libatlas_net_client.so \
          | grep atlas_net_
      ```

   e. ABI 回归防护: 在 `tests/unit/net_client` 下加一个
      `abi_layout_test.cc`, 对每个跨 FFI 结构体做 `static_assert` 锁定
      `sizeof` / `offsetof`:
      ```cpp
      static_assert(sizeof(AtlasNetCallbacks) == 32);
      static_assert(offsetof(AtlasNetCallbacks, on_rpc) == 0);
      static_assert(offsetof(AtlasNetCallbacks, on_entity_create) == 8);
      // ...
      ```
      这样任何破坏 ABI 的修改都在编译期爆出, 而不是运行期 Unity 崩溃。

### Phase 4: C# P/Invoke 适配 (预计 2-3 天)

1. **创建 AtlasNetNative.cs** (§6.1)
   - 所有 P/Invoke 声明, 使用 `[LibraryImport]` (source-generated marshalling)
   - 反映新 API: `atlas_net_create(uint expectedAbi)` 等
   - iOS 条件编译 `LibName = "__Internal"`
   - 顶部 `AtlasNet.Create()` 薄封装统一做 ABI 校验 + 异常

2. **创建 AtlasNetCallbackBridge.cs** (§6.3)
   - `[UnmanagedCallersOnly]` 事件回调 (OnRpc/OnEntityCreate/OnEntityDestroy/OnDisconnect)
   - 单独的 `AtlasNetLoginBridge` / `AtlasNetAuthBridge` 处理 user_data + GCHandle
   - 所有回调内立即复制指针数据 (§4.0.1)

3. **适配 ClientNativeApi.cs**
   - 改为调用 `atlas_net_client` 而非 `atlas_engine`
   - 增加 `nint ctx` 参数传递; ctx 由 `AtlasNetworkManager` 管理生命周期

4. **验证**
   - Phase 3 §4.c 的 `tools/net_client_demo/` 控制台项目走通完整流程
   - 回调从 C++ → C# 正常触发, `user_data` 透传回 `GCHandle.FromIntPtr` 可解回原始对象
   - ABI 版本不匹配时 `Create()` 抛出清晰异常

### Phase 5: Unity Package 集成 (预计 2-3 天)

> **前置**: Phase 0 Spike 结论已落地; 若选方案 B (delegate), Phase 4
> 的 bridge 代码走 delegate 路径, 本阶段保持不变。

1. **创建 Package 目录结构** (§7)
   - `package.json` 配置
   - `Runtime/` 和 `Editor/` 目录
   - Assembly Definition 文件 (`Atlas.Client.asmdef`)

2. **集成 C# 代码**
   - 复制 Atlas.Shared 序列化/协议代码
   - 复制 Atlas.Client 实体/RPC 代码
   - 创建 `AtlasNetworkManager` MonoBehaviour (§7.1)

3. **集成 Native 插件**
   - 放置各平台 DLL/SO/A 文件 (由 Phase 6 交叉编译产出)
   - 配置 Unity 平台过滤器
   - iOS `__Internal` 条件编译 (§9.2)

4. **SourceGenerator 集成** (§8)
   - 编译 Generator 为 netstandard2.0
   - 放入包中标记为 RoslynAnalyzer
   - 验证生成代码正确

5. **验证**
   - Unity 工程中引用包, 场景挂载 `AtlasNetworkManager`
   - 连本地服务端跑完整流程 (login → authenticate → RPC → logout)
   - ABI 不匹配时 `AtlasNet.Create()` 抛异常而非崩溃

### Phase 6: 跨平台构建 (预计 2-3 天)

1. **Android NDK 构建**
   - 在 `MODULE.bazel` 中注册 `rules_android_ndk`
   - `.bazelrc` 增加 `--config=android_arm64` / `--config=android_armv7` (§9.3)
   - 命令:
     ```bash
     bazel build //src/lib/net_client:atlas_net_client --config=android_arm64
     ```
   - 产物位置: `bazel-bin/src/lib/net_client/libatlas_net_client.so`
   - 验证:
     ```bash
     # 检查 ABI (ARM64)
     file bazel-bin/src/lib/net_client/libatlas_net_client.so
     # 预期: "ELF 64-bit LSB shared object, ARM aarch64, ..."
     # 符号导出检查
     nm -D --defined-only bazel-bin/.../libatlas_net_client.so | grep atlas_net_
     ```

2. **iOS 构建**
   - 在 `MODULE.bazel` 中注册 `rules_apple` + `apple_support`
   - 使用 `atlas_net_client_static` target 产出 `.a` (§9.3)
   - 命令:
     ```bash
     bazel build //src/lib/net_client:atlas_net_client_static \
         --config=ios_arm64
     ```
   - 产物位置: `bazel-bin/src/lib/net_client/libatlas_net_client_static.a`
   - 验证: 将 `.a` 放入 Unity `Plugins/iOS/`, Unity iOS 构建出包后
     `.ipa` 内 symbol table 包含 `_atlas_net_create` 等

3. **CI 集成** (可选)
   - 在 GitHub Actions 或等价 CI 上注册矩阵构建任务
   - 产物统一打包 (tar/zip) 上传到 release artifacts

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

---

## 12. 风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| IL2CPP 不支持 `[UnmanagedCallersOnly]` + 函数指针 | Android/iOS 回调失效, Unity 崩溃 | **Phase 0 Spike (§10) 强制前置**; fallback 到 `MonoPInvokeCallback` + delegate + GCHandle |
| Unity Roslyn 版本不兼容 SourceGenerator | 编译失败 | 检查 Unity 版本对应 Roslyn 版本, 锁定 Generator 为 netstandard2.0 + Roslyn 4.3 以下 API |
| iOS 静态链接符号冲突 | 链接错误 | `-fvisibility=hidden` 只导出 `atlas_net_*`; 与 Unity 内置 .NET 运行时符号必不冲突 |
| 回调线程安全 | 崩溃/数据竞争 | 所有回调在 `poll()` 内同步触发, 与 Unity 主线程一致 (§4.0.6) |
| Atlas.Shared 的 Vector3 与 UnityEngine.Vector3 冲突 | 编译错误/混淆 | 条件编译或隐式转换运算符 |
| DLL 与 C# 层 ABI 版本不匹配 | 静默数据损坏 / Unity 崩溃 | `atlas_net_create(expected_abi)` 强制校验, 失败返回 NULL (§4.0.5) |
| `AtlasNetCallbacks` 布局改动未同步 | Unity 运行时不定期崩溃 | Phase 3 验证 e: 用 `static_assert` 锁 `sizeof`/`offsetof`, 编译期阻断 |
| SessionKey 泄漏 (core dump / 进程 snapshot) | 会话劫持 | `SessionKey` 不跨 FFI, `SecureZero` 清除 (§5.2.1) |
| 用户在非法状态调用 API (重复 login 等) | 状态机损坏 | §4.5.6 矩阵 + 非法调用仅返回错误码, 绝不隐式断开 |
| C# 在回调中递归调用 poll/destroy | 栈溢出 / use-after-free | §4.0.3 明确禁止清单, `ATLAS_DEBUG` 下 assert |
| C# 保存 `atlas_net_last_error` 返回指针跨帧使用 | 悬垂指针读取 | §4.0.1/4.0.2 文档 + code review 检查; C# 层统一封装为 `string` 复制 |
| 服务器发 rpc_id 超过 `uint16` 可表示范围 | RPC 分发失败 | §5.3.2 约束: MessageID 为 uint16; 若将来扩容需同步升级 ABI MAJOR |
