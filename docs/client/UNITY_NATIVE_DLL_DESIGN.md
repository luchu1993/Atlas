# Unity Native Network DLL 设计文档

**Status:** ✅ 已落地。Unity Plugins/ 目录待用 CI artifact 填充后做端到端验证。

**目标:** 把 C++ 网络层抽取为独立 native DLL,Unity 客户端通过 P/Invoke
调用。Phase 12 客户端 SDK 的高层 C# API(`AtlasClient` / `AvatarFilter` /
`LoginClient`)在此 DLL 的 C API 之上构建。

**关键决策记录:**

- IL2CPP 回调采用 **Pattern B**(`[MonoPInvokeCallback]` + delegate +
  `Marshal.GetFunctionPointerForDelegate`)。Unity 2022 至 6.5 全系列均
  不支持 `[UnmanagedCallersOnly]`,迁移到 Pattern A 留待 Unity 6.6+
  (.NET 10)落地后重测;探针保留在 `src/tools/il2cpp_probe/`,
  矩阵详见 [`docs/spike_il2cpp_callback.md`](../spike_il2cpp_callback.md)。
- 依赖解耦:`ProcessType` 落在 `foundation/process_type.{h,cc}`,
  `DatabaseID` 落在 `server/entity_types.h`,`atlas_serialization_binary`
  独立成 target;`network` / `foundation` / `platform` / `serialization`
  四层依赖闭包不再含 `server` / `db` / `entitydef` / pugixml / rapidjson。
- 跨平台构建:`CMakePresets.json` 含
  `net-client-{android-arm64, ios-arm64, macos-arm64, linux-x64}`,
  `.github/workflows/net_client_cross.yml` 矩阵编译 + artifact 30 天保留。

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
│  │   ├── Update() → AtlasNetPoll()        │
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

- **C# 驱动 Tick**: Unity 的 `Update()` 调用 `AtlasNetPoll()` 驱动网络事件循环
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

## 3. CMake 构建目标

> 项目使用 CMake 3.28+ 作为唯一构建系统 (见 `CLAUDE.md` 与
> `CMakePresets.json`, 历史迁移记录见 `0930f66 build: migrate from Bazel to CMake`)。
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

### 3.1 新增 Target: atlas_net_client

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
)

target_compile_definitions(atlas_net_client_core
  PRIVATE ATLAS_NET_CLIENT_EXPORTS
)

target_link_libraries(atlas_net_client_core
  PUBLIC
    atlas_network
    atlas_foundation
    atlas_platform
    # 关键: 只链接 binary 子集, 不拉入 pugixml / rapidjson (§3.3)
    atlas_serialization_binary
    atlas_compiler_options
)

if(UNIX)
  set_target_properties(atlas_net_client_core PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
  )
endif()

# ---- 最终共享库 ----
# 参考 src/lib/clrscript/CMakeLists.txt 的 atlas_engine SHARED 模式
add_library(atlas_net_client SHARED
  client_api.cc
  client_session.cc
)

target_include_directories(atlas_net_client
  PUBLIC "${CMAKE_SOURCE_DIR}/src/lib"
)

target_compile_definitions(atlas_net_client
  PRIVATE ATLAS_NET_CLIENT_EXPORTS
)

target_link_libraries(atlas_net_client
  PRIVATE
    atlas_network
    atlas_foundation
    atlas_platform
    atlas_serialization_binary
    atlas_compiler_options
)

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
  # [I1 修复] macOS Unity Plugin 用 .bundle 扩展名, 本质是动态库
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
  )
  target_compile_definitions(atlas_net_client_static
    PRIVATE ATLAS_NET_CLIENT_EXPORTS
  )
  target_link_libraries(atlas_net_client_static
    PUBLIC atlas_network atlas_foundation atlas_platform
           atlas_serialization_binary atlas_compiler_options
  )
endif()
```

记得在 `src/lib/CMakeLists.txt` (Layer 2) 追加:

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
cmake --preset release -DATLAS_BUILD_NET_CLIENT=ON
cmake --build build/release --target atlas_net_client --config Release
```

若需要可选特性开关 (例如压缩), 使用标准 CMake 模式:
```cmake
option(ATLAS_NET_CLIENT_COMPRESSION "Enable RUDP compression filter" ON)
if(ATLAS_NET_CLIENT_COMPRESSION)
  target_compile_definitions(atlas_net_client_core PRIVATE ATLAS_COMPRESSION=1)
endif()
```

本设计建议: 用 `ATLAS_BUILD_NET_CLIENT` 控制 target 是否生成
(默认 `OFF`, 服务端 CI 矩阵不会误构建); compression 用独立 `option()`。

### 3.3 序列化模块拆分

当前 `src/lib/serialization/CMakeLists.txt` 把 4 个 `.cc` 合入
`atlas_serialization`; 其中 `xml_parser.cc` 和 `json_parser.cc`
引入 pugixml / rapidjson 依赖, Unity 客户端 DLL 不需要。

**方案**: 新增 `atlas_serialization_binary` target (仅 `binary_stream.cc`,
无第三方依赖):

```cmake
# src/lib/serialization/CMakeLists.txt (修改后)

add_library(atlas_serialization_binary STATIC
  binary_stream.cc
)

target_include_directories(atlas_serialization_binary
  PUBLIC "${CMAKE_SOURCE_DIR}/src/lib"
)

target_link_libraries(atlas_serialization_binary
  PUBLIC atlas_foundation atlas_platform atlas_compiler_options
)

# 原 atlas_serialization 保持不变, 服务端继续使用
add_library(atlas_serialization STATIC
  binary_stream.cc
  data_section.cc
  json_parser.cc
  xml_parser.cc
)

target_include_directories(atlas_serialization
  PUBLIC "${CMAKE_SOURCE_DIR}/src/lib"
)

target_link_libraries(atlas_serialization
  PUBLIC atlas_foundation atlas_platform pugixml rapidjson atlas_compiler_options
)
```

> **备选**: 可让 `atlas_serialization` 通过 `target_link_libraries(...
> PUBLIC atlas_serialization_binary)` 并仅新增 data/xml/json 3 个 .cc,
> 避免 `binary_stream.cc` 的重复编译。是否这么做取决于
> `binary_stream.cc` 规模; 若较小, 冗余编译成本可忽略。

#### 3.3.1 传递依赖闭包 — 必须同步调整 atlas_network

> ⚠ **容易踩坑**: 仅仅引入 `atlas_serialization_binary` 目标不足以
> 把 pugixml / rapidjson 挡在 DLL 外。

当前 `src/lib/network/CMakeLists.txt`:

```cmake
target_link_libraries(atlas_network
  PUBLIC atlas_foundation atlas_platform atlas_serialization ZLIB::ZLIB ...
)
```

`PUBLIC` 意味着:下游 (`atlas_net_client_core`) 链 `atlas_network` 时,
CMake 会把 `atlas_serialization`(完整版, 含 pugixml/rapidjson) 也加入
`atlas_net_client_core` 的 `INTERFACE_LINK_LIBRARIES`, 最终打进 DLL。

已验证 (`grep -l 'pugi\|rapidjson' src/lib/network/**`): `atlas_network`
源码只 `#include "serialization/binary_stream.h"`, **没有**任何 xml/json
用法。因此把依赖降级是安全的。

**修复步骤** (必须与 §3.3 同一个 PR 内完成, 否则 §3.3 无效):

1. 修改 `src/lib/network/CMakeLists.txt`:
   ```cmake
   target_link_libraries(atlas_network
     PUBLIC atlas_foundation atlas_platform
            atlas_serialization_binary         # ← 原来是 atlas_serialization
            ZLIB::ZLIB atlas_compiler_options
   )
   ```

2. 全量服务端编译验证 (atlas_network 被服务端各组件链接):
   ```bash
   cmake --preset debug
   cmake --build build/debug --config Debug
   # 预期: 所有原本依赖 atlas_network 的 target (server/loginapp/baseapp/...)
   # 继续编译通过, 因为它们真正需要 xml/json 时会自己再显式依赖 atlas_serialization
   ```

3. 验证 DLL 不再携带 pugixml/rapidjson:
   ```bash
   cmake --build build/release --target atlas_net_client --config Release
   dumpbin /dependents build/release/src/lib/net_client/Release/atlas_net_client.dll
   # 预期: 不出现 pugixml / rapidjson 符号
   # Linux:
   nm -D build/release/.../libatlas_net_client.so | grep -Ei 'pugi|rapid'
   # 预期: 空输出
   ```

4. 若服务端的某个 target (如 `atlas_server` / `atlas_loginapp` / `atlas_baseapp`)
   之前依赖 `atlas_network` 顺带拿到了 `atlas_serialization` 的 xml/json,
   编译会报 `pugi::...` / `rapidjson::...` 未定义 — 给该 target 加显式
   `target_link_libraries(... PUBLIC atlas_serialization)` 补回即可。

`atlas_net_client_core` 在此基础上再显式只依赖 `atlas_serialization_binary`,
DLL 就真正不会携带 pugixml / rapidjson 代码了。

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

对应单元测试 (挂接到现有 `tests/unit/CMakeLists.txt`, 复用
`atlas_add_test` helper):

```cmake
# tests/unit/CMakeLists.txt 追加:

atlas_add_test(NAME test_client_session
  SOURCES test_client_session.cpp     # mock NetworkInterface (§10 Phase 3 验证 a)
  DEPS atlas_net_client_core
)

atlas_add_test(NAME test_net_client_abi_layout
  SOURCES test_net_client_abi_layout.cpp   # sizeof/offsetof static_assert (§10 Phase 3 验证 e)
  DEPS atlas_net_client_core
)

atlas_add_test(NAME test_client_state_machine
  SOURCES test_client_state_machine.cpp    # §4.5.6 状态矩阵覆盖
  DEPS atlas_net_client_core
)
```

```cmake
# tests/integration/CMakeLists.txt 追加:

atlas_add_test(NAME test_client_flow
  LABEL integration
  SOURCES test_client_flow.cpp        # 真实 LoginApp + BaseApp + DBApp
  DEPS atlas_net_client_core atlas_server ...
)
```

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
// Pattern B (当前)；Unity 6.6+ 切到 Pattern A 时，仅 attribute 改名。
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

- `AtlasNetCreate` 成功后, ctx 内置 noop 回调表 (每个字段指向 DLL 内部的
  `AtlasNetNoop<Event>` 静态实现), 未注册即收消息不会 null 解引用
- `AtlasNetSetCallbacks` 的结构体字段允许为 `NULL` — DLL 入口处对每个 NULL
  槽位用对应的内部 noop 替换后再原子写入, C# 侧无需获取 noop 函数指针
  (sentinels 不暴露为导出符号)
- 热替换: `AtlasNetSetCallbacks` 在 poll 外任何时刻可调用,
  ctx 原子切换 (`std::atomic<AtlasNetCallbacks*>`, 读侧 `memory_order_acquire`,
  写侧 `memory_order_release`)
- **所有回调首参均为 `AtlasNetContext* ctx`** — 这是能支持"多 ctx 并发"
  (§4.0.6) 的唯一路径: C# 侧用静态 `Dictionary<nint,AtlasClient>` 在 ctx
  指针 → AtlasClient 实例之间做映射

> **简化记录 (D0 决策后):** 旧版要求 C# 必须从 DLL 拉取 sentinel 函数指针
> 填满每个槽位, 这在 Pattern B (`[MonoPInvokeCallback]` + delegate) 下做不
> 干净——`Marshal.GetFunctionPointerForDelegate` 只能给托管 delegate 取指,
> 不能给 `[LibraryImport]` 声明的 native 入口取指。**改为 NULL 由 DLL 替换**
> 之后, Pattern A 与 Pattern B 共用同一注册路径, 前向兼容代价归零。
> 历史方案见 git log。

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
typedef struct AtlasNetChannel AtlasNetChannel;

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
    const uint kExpectedAbi = 0x01000000u;  // 与 C 头文件同步

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
// 调用后允许重新 AtlasNetLogin() 用新凭证登录。
ATLAS_NET_CALL int32_t AtlasNetDisconnect(
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
```

> **[M3] 非-RPC 业务消息的发送路径**: Phase 12 §2.1 列出的 `EnableEntities`
> (10002)、`AvatarUpdate` (10010)、`Heartbeat` (10003)、`Disconnect` (10020)
> 也是上行业务消息, 不属于"调用某 exposed cell/base 方法"语义, 不应走
> `AtlasNetSendBaseRpc`。方案:
>
> - 这些消息在 Source Generator 里注册为**固定 rpc_id 的系统消息**,
>   C# 层仍调用 `AtlasNetSendBaseRpc(ctx, player_entity_id, kRpcId_EnableEntities,
>   payload, len)`, 由 BaseApp 的 RPC 分发表识别为系统级动作。
> - 这种统一让 DLL 的发送路径只有 2 个入口 (BaseRpc / CellRpc), 无需为每种
>   系统消息加专属 C API。对应的 rpc_id 空间占用在 `Atlas.Shared/MessageIds.cs`
>   里显式编码。
>
> 若后续发现确有高频且不适合走 RPC 通道的消息 (例如高频 AvatarUpdate),
> 再追加 `AtlasNetSendAvatarUpdate(ctx, pos, dir, on_ground)` 这类专属 C API,
> 避免每包都跨 FFI 序列化/复制。

### 4.7 消息接收回调

DLL 是纯传输层。除了一次性的连接断开通知, 所有服务端下行消息（AoI 信封
0xF001 / 0xF002 / 0xF003、RPC 信封 0xF004、login/auth typed 消息以外
的任何 wire id）都经单一 `on_deliver` 把原始 payload 透传到 C#, 由
`Atlas.Client.ClientCallbacks.DeliverFromServer` 完成解码。

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
// (0xF001/0xF002/0xF003 = AoI envelope, 0xF004 = RPC envelope, 其他 =
// 客户端 RPC 直接以 rpc_id 当作 MessageID 发出)。C# 侧 switch 后再解码。
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
| `AtlasNetSetCallbacks` | C#→C++ | 注册事件回调 (原子热替换) |
| `AtlasNetSetLogHandler` | C#→C++ | 注册日志回调 (进程级) |
| `AtlasNetLogin` | C#→C++ | 发起登录 (带 user_data) |
| `AtlasNetAuthenticate` | C#→C++ | 发起认证 (无 host/key 参数,DLL 自持) |
| `AtlasNetDisconnect` | C#→C++ | 关闭连接,保留 ctx 和回调表 |
| `AtlasNetGetState` | C#→C++ | 查询连接状态 (含 LoginSucceeded) |
| `AtlasNetSendBaseRpc` | C#→C++ | 发送 Base RPC |
| `AtlasNetSendCellRpc` | C#→C++ | 发送 Cell RPC |
| `AtlasNetGetStats` | C#→C++ | 获取网络统计 |
| `AtlasLoginResultFn` | C++→C# | 登录结果通知 (user_data + status + baseapp addr) |
| `AtlasAuthResultFn` | C++→C# | 认证结果通知 (user_data + entity_id + type_id) |
| `AtlasDisconnectFn` | C++→C# | **(ctx, reason)** 服务器关闭/超时/踢下线 |
| `AtlasDeliverFromServerFn` | C++→C# | **(ctx, msg_id, payload, len)** 所有非 login/auth 下行消息;C# `ClientCallbacks.DeliverFromServer` 解码 |
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
  - `AtlasNetDisconnect()` → `ClearSessionKey()`
  - `AtlasNetDestroy()` → 随 ctx 析构自动清零
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

`AtlasNetCreate` 在返回前安装一份 noop 表; `AtlasNetSetCallbacks` 用调用者
传入的 ctx-绑定 lambda 把 NULL 字段替换成 noop, 旧表整体替换。

### 5.3 消息分发流程

ClientSession 在 `kConnected` 后注册一个 default handler, 把所有 wire id
原样投到 `on_deliver`。Login / Auth typed 消息照常被它们的 typed handler
拦截; LoggedOff 在 ClientSession 内部映射到 `on_disconnect`。

```
AtlasNetPoll(ctx)
  └── EventDispatcher::ProcessOnce()    [单线程, tick 驱动]
        ├── IOPoller::Poll() → 读包
        ├── ReliableUdpChannel → 解 RUDP → Bundle
        ├── InterfaceTable::Dispatch()
        │   ├── LoginResult / AuthenticateResult → ClientSession typed
        │   │      → login/auth 一次性回调
        │   ├── LoggedOff → callbacks_.on_disconnect(ctx, kLoggedOff)
        │   └── Default Handler (其它一切)
        │         → callbacks_.on_deliver(ctx, msg_id, payload, len)
        └── TimerQueue::Process() → 重传 / 心跳
```

> **线程模型**: `EventDispatcher::ProcessOnce()` (`src/lib/network/event_dispatcher.h:103`)
> 是纯 tick-pumped 同步实现, 不起内部线程, 所以所有回调在 `AtlasNetPoll`
> 的调用线程里同步触发, 满足 §4.0.6 "每 ctx 单线程"。

#### 5.3.1 服务器→客户端 RPC 的线格式

**重要**: 服务器下行的 RPC 消息**不是**一个单独的"ClientRpc"消息带
`rpc_id` 字段；服务器 (BaseApp) 直接将 `rpc_id` 用作消息的 `MessageID`
发出 (见 `src/server/baseapp/baseapp.cc:610-625`
`SendMessage(static_cast<MessageID>(rpc_id), payload)`)。

客户端的 `InterfaceTable` 无法为服务器定义的所有 rpc_id 注册 typed
handler，因此在完成 authentication 后注册一个 **Default Handler**:

```cpp
// 等价于 src/client/client_app.cc:323-337 的现有实现, 扩展出 ctx 透传
network_.InterfaceTable().SetDefaultHandler(
    [this, ctx](const Address&, Channel*, MessageID msg_id, BinaryReader& reader) {
      // msg_id 即 rpc_id (uint16 升 uint32)
      auto remaining = reader.Remaining();
      auto payload = remaining > 0 ? reader.ReadBytes(remaining) : std::nullopt;
      callbacks_.load()->on_rpc(
          ctx,                                                 // [B2] 新增首参
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

> Pattern A / B 共用同一份 `[LibraryImport]` 声明;回调注册的 §6.3 走
> Pattern B(`[MonoPInvokeCallback]` + delegate)。Sentinel 不暴露为 DLL
> 导出,由 DLL 内部替换(§4.0.4)。

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
    [LibraryImport(LibName)] internal static partial uint AtlasNetGetAbiVersion();
    [LibraryImport(LibName)] internal static partial nint AtlasNetLastError(nint ctx);
    [LibraryImport(LibName)] internal static partial nint AtlasNetGlobalLastError();

    // --- 生命周期 ---
    [LibraryImport(LibName)]
    internal static partial nint AtlasNetCreate(uint expectedAbi);

    [LibraryImport(LibName)]
    internal static partial void AtlasNetDestroy(nint ctx);

    // --- Tick ---
    [LibraryImport(LibName)] internal static partial int AtlasNetPoll(nint ctx);

    // --- 回调 ([I2 修复] 返回 int 而非 void) ---
    [LibraryImport(LibName)]
    internal static partial int AtlasNetSetCallbacks(
        nint ctx, AtlasNetCallbacks* callbacks);

    [LibraryImport(LibName)]
    internal static partial void AtlasNetSetLogHandler(nint handler);

    // §4.0.4 简化后：DLL 在 SetCallbacks 入口对 NULL 槽位用内部 noop 替换，
    // C# 不再需要拉 sentinel 导出符号。

    // --- Login/Auth/Disconnect (新 API:无 host/key 参数传回) ---
    [LibraryImport(LibName, StringMarshalling = StringMarshalling.Utf8)]
    internal static partial int AtlasNetLogin(
        nint ctx, string loginappHost, ushort loginappPort,
        string username, string passwordHash,
        nint callback, nint userData);

    [LibraryImport(LibName)]
    internal static partial int AtlasNetAuthenticate(
        nint ctx, nint callback, nint userData);

    [LibraryImport(LibName)]
    internal static partial int AtlasNetDisconnect(nint ctx, int reason);

    [LibraryImport(LibName)]
    internal static partial int AtlasNetGetState(nint ctx);

    // --- 消息 ---
    [LibraryImport(LibName)]
    internal static partial int AtlasNetSendBaseRpc(
        nint ctx, uint entityId, uint rpcId, byte* payload, int len);

    [LibraryImport(LibName)]
    internal static partial int AtlasNetSendCellRpc(
        nint ctx, uint entityId, uint rpcId, byte* payload, int len);

    // --- 诊断 ---
    [LibraryImport(LibName)]
    internal static partial int AtlasNetGetStats(nint ctx, AtlasNetStats* outStats);
}

// 薄封装,把 ABI 校验和错误转异常收拢到一处
public static class AtlasNet {
    const uint kExpectedAbi = 0x01000000u;  // 与 C 头文件同步

    public static nint Create() {
        var ctx = AtlasNetNative.AtlasNetCreate(kExpectedAbi);
        if (ctx == 0) {
            var errPtr = AtlasNetNative.AtlasNetGlobalLastError();
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
// 字段顺序和 Pack 与 C 侧 §4.7 AtlasNetCallbacks 严格一致。
// 任何新增字段只能追加到末尾 (向后兼容, §4.0.5 MINOR bump)。
[StructLayout(LayoutKind.Sequential, Pack = 1)]
internal unsafe struct AtlasNetCallbacks
{
    // 连接事件
    public nint OnDisconnect;           // (nint ctx, int reason) → void
    // 玩家会话
    public nint OnPlayerBaseCreate;     // (ctx, eid, tid, props, len) → void
    public nint OnPlayerCellCreate;     // (ctx, space, pos[3], dir[3], props, len) → void
    public nint OnResetEntities;        // (ctx) → void
    // AOI 实体生命周期
    public nint OnEntityEnter;          // (ctx, eid, tid, pos[3], dir[3], props, len) → void
    public nint OnEntityLeave;          // (ctx, eid) → void
    // 实体状态
    public nint OnEntityPosition;       // (ctx, eid, pos[3], dir[3], on_ground) → void
    public nint OnEntityProperty;       // (ctx, eid, scope, delta, len) → void
    public nint OnForcedPosition;       // (ctx, eid, pos[3], dir[3]) → void
    // RPC
    public nint OnRpc;                  // (ctx, eid, rid, payload, len) → void
}
```

### 6.3 回调注册（Pattern B — 当前路径）

> **D0 决策（[`docs/spike_il2cpp_callback.md`](../spike_il2cpp_callback.md)）：**
> 全部 `[MonoPInvokeCallback]` + delegate + `Marshal.GetFunctionPointerForDelegate`。
> Unity 2022 至 6.5 全系列 IL2CPP 不识别 `[UnmanagedCallersOnly]`，没有第二条
> 路。Unity 6.6+（计划嵌入 .NET 10）落地后再走 §6.3.1 的迁移路径。

> **首参 `nint ctx` 不变** — 所有回调都从 `AtlasNetCallbackBridge.FromCtx(ctx)`
> 查回托管实例，支持多 ctx 并发（§4.0.6）。

```csharp
using System.Runtime.InteropServices;
using AOT;

internal static unsafe class AtlasNetCallbackBridge
{
    // ---- 进程级 ctx 注册表 ------------------------------------------------
    private static readonly ConcurrentDictionary<nint, AtlasClient> _ctxMap = new();
    internal static void Bind(nint ctx, AtlasClient client) => _ctxMap[ctx] = client;
    internal static void Unbind(nint ctx) => _ctxMap.TryRemove(ctx, out _);
    internal static AtlasClient FromCtx(nint ctx) => _ctxMap[ctx];

    // ---- Delegate 类型（与 §4.7 C 端 typedef 一一对应）---------------------
    delegate void DisconnectFn(nint ctx, int reason);
    delegate void PlayerBaseCreateFn(nint ctx, uint eid, ushort tid,
                                     byte* props, int len);
    delegate void PlayerCellCreateFn(nint ctx, uint spaceId,
                                     float px, float py, float pz,
                                     float dx, float dy, float dz,
                                     byte* props, int len);
    delegate void ResetEntitiesFn(nint ctx);
    delegate void EntityEnterFn(nint ctx, uint eid, ushort tid,
                                float px, float py, float pz,
                                float dx, float dy, float dz,
                                byte* props, int len);
    delegate void EntityLeaveFn(nint ctx, uint eid);
    delegate void EntityPositionFn(nint ctx, uint eid,
                                   float px, float py, float pz,
                                   float dx, float dy, float dz,
                                   byte onGround);
    delegate void EntityPropertyFn(nint ctx, uint eid, byte scope,
                                   byte* delta, int len);
    delegate void ForcedPositionFn(nint ctx, uint eid,
                                   float px, float py, float pz,
                                   float dx, float dy, float dz);
    delegate void RpcFn(nint ctx, uint eid, uint rid,
                        byte* payload, int len);

    // ---- 静态实例（GC 必须 pin 住 delegate 直到 ctx 销毁）-----------------
    // process-lifetime 单例：全部 ctx 共享同一份 delegate 实例。函数指针
    // 一次性取出后写入每个 ctx 的 callbacks 表，IL2CPP AOT 把 delegate
    // 编译成稳定 trampoline，函数指针寿命与 process 一致 → 不需要 GCHandle.Alloc。
    static readonly DisconnectFn         s_disconnect       = OnDisconnect;
    static readonly PlayerBaseCreateFn   s_playerBaseCreate = OnPlayerBaseCreate;
    static readonly PlayerCellCreateFn   s_playerCellCreate = OnPlayerCellCreate;
    static readonly ResetEntitiesFn      s_resetEntities    = OnResetEntities;
    static readonly EntityEnterFn        s_entityEnter      = OnEntityEnter;
    static readonly EntityLeaveFn        s_entityLeave      = OnEntityLeave;
    static readonly EntityPositionFn     s_entityPosition   = OnEntityPosition;
    static readonly EntityPropertyFn     s_entityProperty   = OnEntityProperty;
    static readonly ForcedPositionFn     s_forcedPosition   = OnForcedPosition;
    static readonly RpcFn                s_rpc              = OnRpc;

    // ---- 处理函数（payload 在回调返回后失效 → 立即复制，§4.0.1）-----------

    [MonoPInvokeCallback(typeof(DisconnectFn))]
    static void OnDisconnect(nint ctx, int reason)
        => FromCtx(ctx).HandleDisconnect(reason);

    [MonoPInvokeCallback(typeof(PlayerBaseCreateFn))]
    static void OnPlayerBaseCreate(nint ctx, uint eid, ushort tid,
                                   byte* props, int len)
    {
        var copy = len > 0 ? new Span<byte>(props, len).ToArray()
                           : Array.Empty<byte>();
        FromCtx(ctx).Entities.HandlePlayerBaseCreate(eid, tid, copy);
    }

    [MonoPInvokeCallback(typeof(PlayerCellCreateFn))]
    static void OnPlayerCellCreate(nint ctx, uint spaceId,
                                   float px, float py, float pz,
                                   float dx, float dy, float dz,
                                   byte* props, int len)
    {
        var copy = len > 0 ? new Span<byte>(props, len).ToArray()
                           : Array.Empty<byte>();
        FromCtx(ctx).Entities.HandlePlayerCellCreate(spaceId,
            new Vector3(px, py, pz), new Vector3(dx, dy, dz), copy);
    }

    [MonoPInvokeCallback(typeof(ResetEntitiesFn))]
    static void OnResetEntities(nint ctx)
        => FromCtx(ctx).Entities.HandleReset();

    [MonoPInvokeCallback(typeof(EntityEnterFn))]
    static void OnEntityEnter(nint ctx, uint eid, ushort tid,
                              float px, float py, float pz,
                              float dx, float dy, float dz,
                              byte* props, int len)
    {
        var copy = len > 0 ? new Span<byte>(props, len).ToArray()
                           : Array.Empty<byte>();
        FromCtx(ctx).Entities.HandleEntityEnter(eid, tid,
            new Vector3(px, py, pz), new Vector3(dx, dy, dz), copy);
    }

    [MonoPInvokeCallback(typeof(EntityLeaveFn))]
    static void OnEntityLeave(nint ctx, uint eid)
        => FromCtx(ctx).Entities.HandleEntityLeave(eid);

    [MonoPInvokeCallback(typeof(EntityPositionFn))]
    static void OnEntityPosition(nint ctx, uint eid,
                                 float px, float py, float pz,
                                 float dx, float dy, float dz,
                                 byte onGround)
        => FromCtx(ctx).Entities.HandleEntityPosition(eid,
               new Vector3(px, py, pz), new Vector3(dx, dy, dz), onGround != 0);

    [MonoPInvokeCallback(typeof(EntityPropertyFn))]
    static void OnEntityProperty(nint ctx, uint eid, byte scope,
                                 byte* delta, int len)
    {
        var copy = len > 0 ? new Span<byte>(delta, len).ToArray()
                           : Array.Empty<byte>();
        FromCtx(ctx).Entities.HandleEntityProperty(eid, scope, copy);
    }

    [MonoPInvokeCallback(typeof(ForcedPositionFn))]
    static void OnForcedPosition(nint ctx, uint eid,
                                 float px, float py, float pz,
                                 float dx, float dy, float dz)
        => FromCtx(ctx).Entities.HandleForcedPosition(eid,
               new Vector3(px, py, pz), new Vector3(dx, dy, dz));

    [MonoPInvokeCallback(typeof(RpcFn))]
    static void OnRpc(nint ctx, uint eid, uint rid, byte* payload, int len)
    {
        var copy = len > 0 ? new Span<byte>(payload, len).ToArray()
                           : Array.Empty<byte>();
        FromCtx(ctx).RpcDispatcher.Enqueue(eid, rid, copy);
    }

    // ---- 注册到 ctx --------------------------------------------------------
    internal static void Register(nint ctx)
    {
        AtlasNetCallbacks table;
        table.OnDisconnect        = Marshal.GetFunctionPointerForDelegate(s_disconnect);
        table.OnPlayerBaseCreate  = Marshal.GetFunctionPointerForDelegate(s_playerBaseCreate);
        table.OnPlayerCellCreate  = Marshal.GetFunctionPointerForDelegate(s_playerCellCreate);
        table.OnResetEntities     = Marshal.GetFunctionPointerForDelegate(s_resetEntities);
        table.OnEntityEnter       = Marshal.GetFunctionPointerForDelegate(s_entityEnter);
        table.OnEntityLeave       = Marshal.GetFunctionPointerForDelegate(s_entityLeave);
        table.OnEntityPosition    = Marshal.GetFunctionPointerForDelegate(s_entityPosition);
        table.OnEntityProperty    = Marshal.GetFunctionPointerForDelegate(s_entityProperty);
        table.OnForcedPosition    = Marshal.GetFunctionPointerForDelegate(s_forcedPosition);
        table.OnRpc               = Marshal.GetFunctionPointerForDelegate(s_rpc);

        // §4.0.4 简化后允许任一字段为 0; DLL 会在 set 时替换为内部 noop。
        // 上面把全部字段都填了真实 handler, 业务上不需要按事件订阅。
        int rc = AtlasNetNative.AtlasNetSetCallbacks(ctx, &table);
        if (rc != 0) throw new InvalidOperationException(
            $"AtlasNetSetCallbacks failed: {rc}");
    }
}

// ---- Login / Auth 回调 ----
// Login/Auth 单次回调，user_data 是 GCHandle.IntPtr，C++ 透传回来后我们 Free 掉。
internal static unsafe class AtlasNetLoginBridge
{
    delegate void LoginResultFn(nint userData, byte status,
                                byte* baseappHost, ushort baseappPort,
                                byte* errorMessage);

    static readonly LoginResultFn s_callback = OnLoginResult;
    internal static nint Pointer { get; } =
        Marshal.GetFunctionPointerForDelegate(s_callback);

    [MonoPInvokeCallback(typeof(LoginResultFn))]
    static void OnLoginResult(nint userData, byte status,
                              byte* baseappHost, ushort baseappPort,
                              byte* errorMessage)
    {
        var handle = GCHandle.FromIntPtr(userData);
        var mgr = (AtlasNetworkManager)handle.Target!;
        string? host = baseappHost != null
            ? Marshal.PtrToStringUTF8((nint)baseappHost) : null;
        string? err = errorMessage != null
            ? Marshal.PtrToStringUTF8((nint)errorMessage) : null;
        handle.Free();  // 一次性 handle
        mgr.OnLoginCompleted((AtlasLoginStatus)status, host, baseappPort, err);
    }
}

internal static unsafe class AtlasNetAuthBridge
{
    delegate void AuthResultFn(nint userData, byte success,
                               uint entityId, ushort typeId, byte* errorMessage);

    static readonly AuthResultFn s_callback = OnAuthResult;
    internal static nint Pointer { get; } =
        Marshal.GetFunctionPointerForDelegate(s_callback);

    [MonoPInvokeCallback(typeof(AuthResultFn))]
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
}

public enum AtlasLoginStatus : byte {
    Success = 0, InvalidCredentials = 1, AlreadyLoggedIn = 2,
    ServerFull = 3, Timeout = 4, NetworkError = 5, InternalError = 255,
}
```

#### 6.3.1 前向兼容：Unity 6.6+（.NET 10）迁移到 Pattern A

Unity 6.6+ 嵌入 .NET 10 后，`[UnmanagedCallersOnly]` 应当可用。**先重跑
`src/tools/il2cpp_probe/` 在新版本验证**，再开始迁移。

迁移逐回调机械化、无 wire 协议变化、无 DLL 改动。每个 handler 改三处：

```diff
-using AOT;
-
-delegate void RpcFn(nint ctx, uint eid, uint rid, byte* payload, int len);
-static readonly RpcFn s_rpc = OnRpc;
-
-[MonoPInvokeCallback(typeof(RpcFn))]
-static void OnRpc(nint ctx, uint eid, uint rid, byte* payload, int len) { ... }
-
-table.OnRpc = Marshal.GetFunctionPointerForDelegate(s_rpc);
+[UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
+static void OnRpc(nint ctx, uint eid, uint rid, byte* payload, int len) { ... }
+
+table.OnRpc = (nint)(delegate* unmanaged[Cdecl]<nint,uint,uint,byte*,int,void>)&OnRpc;
```

机械步骤：

1. 加 Scripting Define `ATLAS_CALLBACK_PATTERN_A`（用 `#if`/`#endif` 包住
   两份并存，灰度切换）
2. 砍 `using AOT;`
3. 每个 `delegate XxxFn(...)` 类型可以保留也可以删 — Pattern A 不再依赖
4. 每个 `static readonly XxxFn s_xxx = ...;` keep-alive 字段删掉
5. `[MonoPInvokeCallback(typeof(XxxFn))]` → `[UnmanagedCallersOnly(...)]`
6. `Marshal.GetFunctionPointerForDelegate(s_xxx)` → `(nint)&OnXxx` 取址
7. ABI 版本号**不动** — C++ 端看到的函数指针 ABI 完全相同

迁移完成后跑：

- 全部 4 个 Unity 目标的回调矩阵复测
- 桌面 `tools/net_client_demo/` 的 FFI 验证
- ABI layout 单测（`test_net_client_abi_layout.cpp`，§3.1 + §10 Phase 3）

### 6.4 ClientNativeApi 适配

现有 `ClientNativeApi.cs` 的修改最小化:

| 现有接口 | 变更 |
|----------|------|
| `LibName = "atlas_engine"` | → `"atlas_net_client"` |
| `AtlasSendBaseRpc` | → `AtlasNetSendBaseRpc` (增加 ctx 参数) |
| `AtlasSendCellRpc` | → `AtlasNetSendCellRpc` (增加 ctx 参数) |
| `AtlasSetNativeCallbacks` | → `AtlasNetSetCallbacks` (新结构体) |
| `AtlasLogMessage` | → 通过日志回调反向调用 Unity Debug.Log |

---

## 7. Unity SDK 目录结构

仓库内的规范位置在 `src/csharp/Atlas.Client.Unity/`。`tools/setup_unity_client`
build 完后,把整个目录(剔除 csproj / bin / obj)拷贝到用户 Unity 项目的
`Assets/Atlas.Client.Unity/`,native + 托管 dll 摆在子目录 `Plugins/` 下。

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
└── Plugins/                          # setup_unity_client 填充
    ├── Atlas.Client.dll              # 托管,任意平台
    ├── Atlas.Shared.dll              # 托管,任意平台
    ├── Windows/x86_64/atlas_net_client.dll
    ├── Linux/x86_64/libatlas_net_client.so
    ├── macOS/atlas_net_client.bundle
    ├── Android/arm64-v8a/libatlas_net_client.so
    └── iOS/libatlas_net_client.a
```

`ClientEntity` / `ClientEntityManager` / `RpcDispatcher` / `SpanReader` /
`SpanWriter` / `MessageIds` 这些复用自 `Atlas.Client.dll` / `Atlas.Shared.dll`,
以预编译 plugin 形式从 `Plugins/` 引入,不重复源码。

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

        AtlasNetNative.AtlasNetSetLogHandler(
            (nint)(delegate* unmanaged<int, byte*, int, void>)&LogBridge.OnLog);
        AtlasNetCallbackBridge.Register(_ctx);
    }

    void Update()
    {
        if (_ctx != 0)
            AtlasNetNative.AtlasNetPoll(_ctx);
    }

    void OnDestroy()
    {
        if (_ctx != 0)
        {
            AtlasNetNative.AtlasNetDestroy(_ctx);
            _ctx = 0;
            Instance = null!;
        }
    }

    // ---- 公开 API ----

    public void Login(string username, string passwordHash)
    {
        // GCHandle 让 C++ 透传定位回具体实例 (§4.5.2 user_data 参数)
        var h = GCHandle.Alloc(this);
        int rc = AtlasNetNative.AtlasNetLogin(
            _ctx, config.loginappHost, config.loginappPort,
            username, passwordHash,
            AtlasNetLoginBridge.Pointer, GCHandle.ToIntPtr(h));
        if (rc != 0) { h.Free(); throw new InvalidOperationException($"login rc={rc}"); }
    }

    // 通常在 OnLoginFinished 的 Success 分支内调用
    public void Authenticate()
    {
        var h = GCHandle.Alloc(this);
        int rc = AtlasNetNative.AtlasNetAuthenticate(
            _ctx, AtlasNetAuthBridge.Pointer, GCHandle.ToIntPtr(h));
        if (rc != 0) { h.Free(); throw new InvalidOperationException($"auth rc={rc}"); }
    }

    public void Logout() {
        AtlasNetNative.AtlasNetDisconnect(_ctx, (int)AtlasDisconnectReason.Logout);
    }

    public void SendBaseRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload) {
        fixed (byte* p = payload)
            AtlasNetNative.AtlasNetSendBaseRpc(_ctx, entityId, rpcId, p, payload.Length);
    }

    public AtlasNetStats GetStats() {
        AtlasNetStats s;
        AtlasNetNative.AtlasNetGetStats(_ctx, &s);
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

`.def` 文件是实体定义的唯一来源,`Atlas.Generators.Def` 是仓库内
唯一的 Source Generator,Unity 客户端按 `ATLAS_CLIENT` 上下文消费同一份
生成器:

- `client_methods` → **Receive**(partial method,用户实现)
- exposed `cell_methods` / `base_methods` → **Send**(自动序列化参数并调用 native DLL)
- 非 exposed 的 cell/base methods → 编译期阻断
- 属性字段 → 只生成 scope ≥ OwnClient 的字段 + `ApplyReplicatedDelta`

### 8.1 Unity 中集成步骤

1. 将 `Atlas.Generators.Def` 编译为 `netstandard2.0` DLL
2. 将 DLL 放入 SDK 目录(如 `src/csharp/Atlas.Client.Unity/Analyzers/`,setup 脚本会一并复制到用户工程的 `Assets/Atlas.Client.Unity/Analyzers/`)
3. 在 Unity Inspector 中将其标记为 `RoslynAnalyzer`
4. Unity 2022.2+ 自动在编译时执行 Generator
5. `.def` 文件作为 `AdditionalFiles` 引入(在 `.asmdef` 或 `.csproj` 中配置)

### 8.2 兼容性注意事项

| 项目 | 注意 |
|------|------|
| Target Framework | Generator 必须编译为 `netstandard2.0` |
| Roslyn 版本 | 需匹配 Unity 内置版本(检查 Unity 发行说明) |
| Span/ref struct | 生成的代码使用 `ref struct` 需要 Unity 2021.2+ |
| 依赖 | Generator DLL 需放入同目录 |
| `ATLAS_CLIENT` 符号 | 在 Unity Player Settings → Scripting Define Symbols 中添加 |

### 8.3 Atlas.Shared 复用

`Atlas.Shared` 中的序列化 / 协议代码可直接作为源文件复用到 Unity Package:

- `SpanWriter.cs` / `SpanReader.cs` — 基于 `System.Buffers.BinaryPrimitives`
- `MessageIds.cs` — 纯常量定义
- `EntityRef.cs` — 实体引用

属性元数据 / RPC 声明全部由 `.def` 提供;C# 侧仅保留
`[Entity("Name")]` 用来把 partial class 关联到具体的 `.def` 文件。
`Vector3` / `Quaternion` 与 Unity 引擎的同名类型用条件编译
`#if UNITY_ENGINE` 提供隐式转换。

---

## 9. 跨平台考虑

### 9.1 平台矩阵

| 平台 | 库类型 | IO 后端 | 构建工具链 | 注意事项 |
|------|--------|---------|-----------|----------|
| Windows x64 | .dll | WSAPoll | CMake + MSVC (VS 2022) | 主开发平台 |
| Android arm64 | .so | epoll | CMake + Android NDK r25+ toolchain file | Unity IL2CPP |
| Android armv7 | .so | epoll | CMake + Android NDK toolchain file | 渐淘汰, 可选 |
| iOS arm64 | .a (静态) | select | CMake + ios-cmake toolchain | Apple 禁止第三方动态库 |
| macOS arm64 | .bundle | kqueue/select | CMake + AppleClang | 开发调试 (Unity 编辑器);  `.bundle` 是 Unity macOS Plugin 的标准格式, 实际内部是 Mach-O 动态库 |
| Linux x64 | .so | epoll | CMake + GCC/Clang | 服务端/CI |

**Unity 版本兼容性 / C# 回调模式：**

| Unity 区间 | 嵌入 runtime | C# 回调模式 (§6.3) | 备注 |
|---|---|---|---|
| 2022.3 LTS — 6.5 | Mono / 老 .NET 4.x（IL2CPP 同样基于此） | **Pattern B** (`[MonoPInvokeCallback]` + delegate) | 当前主线；`[UnmanagedCallersOnly]` 不支持 |
| 6.6+ (计划) | .NET 10 | **Pattern A** (`[UnmanagedCallersOnly]` + 函数指针) | 落地后须重跑 `src/tools/il2cpp_probe/` 验证再切换；迁移路径见 §6.3.1 |
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

CMake 通过 `-DCMAKE_TOOLCHAIN_FILE=<path>` 驱动交叉编译。前置要求:
- **Android**: NDK 已安装, `$ANDROID_NDK_HOME` 指向 NDK 根目录,
  使用 NDK 自带的 `build/cmake/android.toolchain.cmake`。
  **宿主支持**: Windows / macOS / Linux 均可 — NDK 都有对应平台的 toolchain。
- **iOS / macOS**: 安装 Xcode + CLT; iOS 使用社区维护的
  `ios.toolchain.cmake` (或 Xcode 生成器 + `-G Xcode -DCMAKE_SYSTEM_NAME=iOS`)。
  **宿主限制 (B5)**: **必须在 macOS 上执行**。Xcode 不提供 Windows/Linux 版本,
  `CMAKE_SYSTEM_NAME=iOS` + `-G Xcode` 会在非 macOS 宿主上直接失败
  (找不到 xcode-select / xcodebuild)。项目主开发平台是 Windows, 所以 iOS 产物:
    - 开发期: 让有 macOS 的开发者手动跑 `cmake --preset ios-arm64`
    - 长期: 把 iOS 构建固化到 CI (GitHub Actions `macos-latest` runner 开箱即用)
- 新增 preset (推荐) 封装到 `CMakePresets.json`, 或直接传命令行

构建命令 (target 名 `atlas_net_client` 由 §3.1 定义):

```bash
# Windows x64 (原生构建, 开发主平台)
cmake --preset release -DATLAS_BUILD_NET_CLIENT=ON
cmake --build build/release --target atlas_net_client --config Release

# Linux x64 (服务端 / CI)
cmake --preset release -DATLAS_BUILD_NET_CLIENT=ON
cmake --build build/release --target atlas_net_client

# Android arm64 (Unity 主力 Android 目标)
cmake -S . -B build/android_arm64 \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-24 \
    -DATLAS_BUILD_NET_CLIENT=ON \
    -DATLAS_BUILD_TESTS=OFF -DATLAS_BUILD_CSHARP=OFF \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build/android_arm64 --target atlas_net_client

# Android armv7 (可选, 渐淘汰)
cmake -S . -B build/android_armv7 \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=armeabi-v7a \
    -DANDROID_PLATFORM=android-24 \
    -DATLAS_BUILD_NET_CLIENT=ON \
    -DATLAS_BUILD_TESTS=OFF -DATLAS_BUILD_CSHARP=OFF \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build/android_armv7 --target atlas_net_client

# iOS arm64 — 使用静态库 target (§3.1)
# 原因: Apple 禁止第三方 dylib, Unity iOS 用 [DllImport("__Internal")]
cmake -S . -B build/ios_arm64 -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
    -DATLAS_BUILD_NET_CLIENT=ON \
    -DATLAS_BUILD_TESTS=OFF -DATLAS_BUILD_CSHARP=OFF
cmake --build build/ios_arm64 --target atlas_net_client_static \
    --config Release

# macOS arm64 (开发调试, Unity 编辑器使用)
cmake -S . -B build/macos_arm64 \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DATLAS_BUILD_NET_CLIENT=ON \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build/macos_arm64 --target atlas_net_client
```

建议把这些参数固化到 `CMakePresets.json` (或 `CMakeUserPresets.json`)
里的新增 `configurePresets` 条目, 形如:

```jsonc
{
  "name": "android-arm64",
  "toolchainFile": "$env{ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake",
  "binaryDir": "${sourceDir}/build/android_arm64",
  "cacheVariables": {
    "ANDROID_ABI": "arm64-v8a",
    "ANDROID_PLATFORM": "android-24",
    "CMAKE_BUILD_TYPE": "Release",
    "ATLAS_BUILD_NET_CLIENT": "ON",
    "ATLAS_BUILD_TESTS": "OFF",
    "ATLAS_BUILD_CSHARP": "OFF"
  }
}
```

之后跑 `cmake --preset android-arm64 && cmake --build build/android_arm64
--target atlas_net_client` 即可。

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

> 整套工作已完成,以下仅记录最终的代码 / 构建 / 验收落点。原本的 Phase
> 0–6 实施步骤随着代码 land 一并删除。

| 区块 | 落地 |
|------|------|
| IL2CPP 可行性 Spike | `src/tools/il2cpp_probe/`(probe.cc + Unity ProbeComponent + README);Pattern B 决议见上方"关键决策记录" |
| 依赖解耦 | `foundation/process_type.{h,cc}`、`server/entity_types.h::DatabaseID`、`atlas_serialization_binary` STATIC target;`atlas_network` 闭包零 `server` / `db` / `entitydef` / pugixml / rapidjson |
| C API 导出层 | `src/lib/net_client/`(`client_api.cc` + `client_session.cc`),`atlas_net_client.dll` SHARED + `atlas_net_client_core` STATIC + iOS `_static` 三 target;`test_net_client_abi_layout` 锁 sizeof / offsetof |
| C# P/Invoke | `Atlas.Client/Native/`(DllImport + Pattern B 桥 + `IAtlasNetEvents`);`Atlas.Tools.NetClientDemo`(CoreCLR 控制台)做 FFI roundtrip |
| Unity SDK | `src/csharp/Atlas.Client.Unity/`(asmdef + `AtlasNetworkManager` MonoBehaviour + `tools/setup_unity_client` 一键拷到用户 Unity `Assets/`);`Plugins/` 目录待 CI artifact 填充后做端到端验证 |
| 跨平台构建 | `CMakePresets.json` 含 `net-client-{android-arm64, ios-arm64, macos-arm64, linux-x64}`;`.github/workflows/net_client_cross.yml` 矩阵 + 30 天 artifact |

### 落地映射(供修改时定位)

`src/lib/foundation/process_type.{h,cc}` — `ProcessType` 枚举与
`ProcessTypeName / ProcessTypeFromName` 函数。`server/server_config.h`
与 `network/machined_types.h` 都 include 它。

`src/lib/server/entity_types.h` — `EntityID / SessionKey / DatabaseID`;
`db/idatabase.h`、`login_messages.h`、`baseapp_messages.h` 全部走这一头。

### 关键测试

| 测试 | 文件 | 锁定的不变量 |
|------|------|-------------|
| `test_client_session` | `tests/unit/test_client_session.cpp` | 状态机:Login/Auth 成功、超时、断开、`Disconnect` 幂等;非法状态调用返回 §4.5.6 表上的错误码 |
| `test_client_flow` | `tests/integration/test_client_flow.cpp` | 真实 LoginApp + BaseApp + DBApp 端到端线格式 |
| `test_net_client_abi_layout` | `tests/unit/test_net_client_abi_layout.cpp` | `static_assert` 锁定 `AtlasNetCallbacks` / `AtlasNetStats` 的 `sizeof` / `offsetof`;C# 侧用 `Marshal.SizeOf<>` 双向核对 |
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
- 服务端在 `src/server/loginapp/login_handler.cc` 的 `LoginRequest` 处理路径
  按同一算法比对 (目前实现是直接字符串对比 — Phase 9 已知待补)
- C# 客户端实现见 phase12 §3.6 `HashPassword`; 测试用 `test_login_hash` 单测
  锁定 hex 输出: `SHA-256("alice:hunter2")` = `...` (固定向量, 服务端/客户端共享)

**不做**: bcrypt/argon2 等 slow-hash — 它们的成本在于抗离线爆破, 需要服务端
存储盐后做 slow verify; 本项目认证路径目前还是查表比对, slow-hash 不提供
额外价值。真正的密码存储强化应在 LoginApp 落库层独立设计。

### 11.5 `AtlasNetDisconnect` 幂等性与回调触发 (M5)

§4.5.4 约定:

| 场景 | 行为 |
|------|------|
| 首次调用, 状态非 Disconnected | 关闭 channel / 清状态; USER-initiated 不触发 `on_disconnect`; LOGOUT-initiated 触发 `on_disconnect(ctx, 3)` |
| 首次调用, 状态已为 Disconnected | noop; 返回 0; **不触发 `on_disconnect`** |
| 重复调用 (无论 reason) | noop; 返回 0; **不再触发 `on_disconnect`** (避免 C# 重复收到断线事件) |
| DLL 内部检测到断线 (服务端主动关闭/超时/网络错误) | 自动触发 `on_disconnect(ctx, reason)` 一次, 并切到 Disconnected |

换言之: `on_disconnect` 在一次 "从 Connected/半途 → Disconnected" 转换中
**恰好触发一次**。

---

## 12. 风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| Unity 6.6 嵌入 .NET 10 上线时 Pattern A 仍不工作 | 切换到 Pattern A 失败回滚 | 落地时**先重跑** `src/tools/il2cpp_probe/`;保留 Pattern B 路径以 `#if !ATLAS_CALLBACK_PATTERN_A` 包住,可灰度回退 |
| Unity Roslyn 版本不兼容 SourceGenerator | 编译失败 | 检查 Unity 版本对应 Roslyn 版本, 锁定 Generator 为 netstandard2.0 + Roslyn 4.3 以下 API |
| iOS 静态链接符号冲突 | 链接错误 | `-fvisibility=hidden` 只导出 `atlas_net_*`; 与 Unity 内置 .NET 运行时符号必不冲突 |
| 回调线程安全 | 崩溃/数据竞争 | 所有回调在 `poll()` 内同步触发, 与 Unity 主线程一致 (§4.0.6) |
| Atlas.Shared 的 Vector3 与 UnityEngine.Vector3 冲突 | 编译错误/混淆 | 条件编译或隐式转换运算符 |
| DLL 与 C# 层 ABI 版本不匹配 | 静默数据损坏 / Unity 崩溃 | `AtlasNetCreate(expected_abi)` 强制校验, 失败返回 NULL (§4.0.5) |
| `AtlasNetCallbacks` 布局改动未同步 | Unity 运行时不定期崩溃 | Phase 3 验证 e: 用 `static_assert` 锁 `sizeof`/`offsetof`, 编译期阻断 |
| SessionKey 泄漏 (core dump / 进程 snapshot) | 会话劫持 | `SessionKey` 不跨 FFI, `SecureZero` 清除 (§5.2.1) |
| 用户在非法状态调用 API (重复 login 等) | 状态机损坏 | §4.5.6 矩阵 + 非法调用仅返回错误码, 绝不隐式断开 |
| C# 在回调中递归调用 poll/destroy | 栈溢出 / use-after-free | §4.0.3 明确禁止清单, `ATLAS_DEBUG` 下 assert |
| C# 保存 `AtlasNetLastError` 返回指针跨帧使用 | 悬垂指针读取 | §4.0.1/4.0.2 文档 + code review 检查; C# 层统一封装为 `string` 复制 |
| 服务器发 rpc_id 超过 `uint16` 可表示范围 | RPC 分发失败 | §5.3.2 约束: MessageID 为 uint16; 若将来扩容需同步升级 ABI MAJOR |
