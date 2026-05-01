# Phase 12: 客户端 SDK

**Status:** 🚧 C# 核心骨架可用（`Atlas.Client` 实体管理 / 回调 /
组件、`Atlas.Client.Desktop` P/Invoke、控制台测试客户端 `src/client/`
登录 + AOI + RPC 全链路已通），AvatarFilter / 高层 `AtlasClient` /
`LoginClient` async 包装 / 独立 `atlas_net_client.dll` 未开工。
**前置依赖:** Phase 9 (LoginApp)、Phase 10 (CellApp AOI)、Script Phase 4
最小共享子集、**Unity Native Network DLL**（[`docs/client/UNITY_NATIVE_DLL_DESIGN.md`](../client/UNITY_NATIVE_DLL_DESIGN.md)
至少 Phase 3 完成，产出 `atlas_net_client`）

**与 Native DLL 设计的分工：**

- **Unity Native DLL** — C++ `atlas_net_client` 动态库，承载 RUDP 传输、
  Login/Auth 状态机、消息收发；C API（P/Invoke）暴露给 C#
- **本文档** — C# 侧 `Atlas.Client`（实体管理、位置插值、Source
  Generator 生成的 RPC 分发、面向游戏代码的高层 API）

## 目标

提供面向游戏代码的 C# 客户端 SDK，使 Unity 可以连接 Atlas 集群，完成
登录、实体同步、RPC 通信和位置插值。

**核心优势：**

- 客户端与服务端共享 `Atlas.Shared`（`SpanReader/Writer`、Entity 基类、
  MailboxTarget、MessageIds）；实体定义、序列化、RPC 代理由
  `Atlas.Generators.Def` 在编译期统一生成
- 客户端复用服务端 C++ RUDP 实现（通过原生 DLL），杜绝 C#/C++ 双实现
  协议漂移

## 子系统状态

| 子系统 | 状态 | 说明 |
|---|---|---|
| `Atlas.Client.csproj`（核心） | ✅ | 含 `ClientEntity / ClientEntityFactory / ClientEntityManager / ClientCallbacks / EntityRegistryBridge / ClientHost / ClientLog`；`Components/` 下 `ClientComponentBase / ClientLocalComponent / ClientReplicatedComponent` |
| Source Generator 客户端集成 | ✅ | `ATLAS_CLIENT` 宏下生成 `ApplyReplicatedDelta / Deserialize` + exposed RPC 发送存根 + `client_methods` 接收分发 |
| `Atlas.Client.Desktop` P/Invoke | ✅ | `ClientNativeApi` 绑 `atlas_engine`：`AtlasLogMessage / SendBaseRpc / SendCellRpc / RegisterEntityType / RegisterStruct / SetNativeCallbacks / GetAbiVersion / ReportClientEventSeqGap` |
| `src/client/` 控制台测试客户端 | ✅ | 600+ 行可工作的登录 / 实体接收 / AOI / RPC dispatch；支持 `--loginapp-host / --username / --password / --assembly`、丢包注入 |
| 独立 `atlas_net_client.dll` + C API | ✅ | `src/lib/net_client/`；ClientSession 异步状态机；`ATLAS_NET_ABI_VERSION = v1.0.0`；ABI 锁定测试 `test_net_client_abi_layout` |
| `Atlas.Client/Native` C# P/Invoke 层 | ✅ | DllImport 绑 `atlas_net_client`；`AtlasNetCallbackBridge` Pattern B（Unity 2022–6.5）；`IAtlasNetEvents` 业务接口 |
| `tools/Atlas.Tools.NetClientDemo` FFI 验证 | ✅ | net9.0 控制台：ABI mismatch 拒绝 + Create / SetCallbacks / Disconnect 幂等 roundtrip |
| `Packages/com.atlas.client/` Unity 包骨架 | ✅ | `package.json` + `Atlas.Client.Unity.asmdef` + `AtlasNetworkManager` MonoBehaviour（Update→Poll，事件转发）；Plugins/ 目录占位待 Phase 6 |
| `AvatarFilter` 位置插值 | ✅ | 8-sample 环形缓冲 + 自适应延迟（`|diff|^curvePower`）+ 线性插值 + `MaxExtrapolation` 外推上限；`Atlas.Client.Tests` 8/8 xunit |
| `LoginClient` async/await 包装 | ⬜ | 未实现；当前由 `AtlasNetworkManager.Login + LoginFinished` event 提供 |
| `AtlasClient` 主入口（`ConnectAsync` / `Update` / 事件） | ⬜ | 未实现；MonoBehaviour 已覆盖 Unity 路径 |

## 架构差异（与最初设计稿不同的点）

- **P/Invoke 拆到 `Atlas.Client.Desktop`**，不放在 `Atlas.Client`。这让
  `Atlas.Client` 保持 netstandard2.1 / Unity 友好（无 `UnityEngine` 引用、
  无平台特定 P/Invoke），桌面 / Unity 两个宿主分别 install backend。
- **协议消息**当前直接消费 `baseapp_messages.h` / `loginapp_messages.h` /
  `dbapp_messages.h` 中的 struct，不另起 C# 侧 `Protocol/` namespace。
- **测试客户端**用 `src/client/` 控制台 exe 而非新增 `atlas_test_client`
  工具；功能等价。

## 关键设计决策

1. **面向 Unity 的 C# API + 共享 C++ 网络栈** — C# 仅承载游戏代码可见的
   高层；传输 + Login/Auth 下沉到 native DLL，直接复用服务端
   `lib/network`，避免双实现漂移。
2. **沿用服务端 RUDP** — 与服务端 `NetworkInterface +
   ReliableUdpChannel` 字节级一致；RUDP 参数（MTU = 1472 / 最大分片 255 /
   延迟 ACK 25ms）不在 C# 镜像。
3. **协议简化** — 初期使用显式 `EntityID` + 全精度位置；不引入
   `IDAlias / PackedXYZ / CacheStamps`。
4. **`AvatarFilter` 延迟自适应**（待实现）— 对齐 BigWorld
   `AvatarFilterHelper`：8 采样环形缓冲 +
   `idealLatency = latencyFrames × interval` +
   `velocity × |diff|^curvePower` 收敛（`curvePower = 2.0`）+ 线性插值 +
   外推限制。
5. **初期不实现** — IDAlias / 位置压缩 / CacheStamps / DataDownload /
   Vehicle / 客户端预测 / 断线重连。

## 剩余工作

- Phase 6 — 跨平台 native 构建（Android arm64 / iOS arm64 / macOS / Linux）
  + Unity Plugins/ 目录填充
- `AvatarFilter` 与 `AtlasNetworkManager.EntityPositionUpdated` 事件流的
  接线（feed Input on receive，Update 时 TryEvaluate）
- `LoginClient` async/await 包装（当前为 event-based）
- 端到端集成测试（`tests/integration/test_client_flow.cpp` 拉真 LoginApp +
  BaseApp + DBApp 集群，驱动完整 Create→Login→Authenticate→RPC→Disconnect）
