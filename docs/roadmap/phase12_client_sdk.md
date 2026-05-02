# Phase 12: 客户端 SDK

**Status:** ✅ 主线完成（`atlas_net_client.dll` C API、`Atlas.Client`
实体管理 / 回调 / 组件、`Atlas.Client.Desktop` P/Invoke、`AvatarFilter`
位置插值、控制台测试客户端 `src/client/` 登录+AOI+RPC 全链路、Unity
`com.atlas.client` 包骨架、跨平台 CI 矩阵均已落地）；剩余高层
`LoginClient` / `AtlasClient` async 包装 ⬜。
**前置依赖:** Phase 9 (LoginApp)、Phase 10 (CellApp AOI)、脚本层
`Atlas.Shared` + Source Generator（[`docs/scripting/`](../scripting/)）

**分工：** 传输与 Login/Auth 状态机由 C++ `atlas_net_client` 承载，C
API 通过 P/Invoke 暴露给 C#；本文档负责 C# 侧 `Atlas.Client`（实体管理、
位置插值、Source Generator RPC 分发、面向游戏代码的高层 API）。

## 目标

提供面向游戏代码的 C# 客户端 SDK，使 Unity 可以连接 Atlas 集群，完成
登录、实体同步、RPC 通信和位置插值。

**核心优势：**

- 客户端与服务端共享 `Atlas.Shared`（`SpanReader/Writer`、Entity 基类、
  MailboxTarget、MessageIds）；实体定义、序列化、RPC 代理由
  `Atlas.Generators.Def` 在编译期统一生成
- 客户端复用服务端 C++ RUDP 实现（通过原生 DLL），杜绝 C#/C++ 双实现
  协议漂移

## 关键架构

- `atlas_net_client.dll`（`src/lib/net_client/`）承载 ClientSession
  异步状态机，C API ABI 由 `ATLAS_NET_ABI_VERSION` 锁定，`Atlas.Client` /
  Unity / 控制台客户端共用同一份传输实现。
- P/Invoke 层下沉到 `Atlas.Client.Desktop`，`Atlas.Client` 保持
  netstandard2.1 / Unity 友好（无 `UnityEngine` 引用、无平台 P/Invoke），
  桌面与 Unity 通过各自 backend 接入。
- 协议结构直接消费 `baseapp_messages.h` / `loginapp_messages.h` /
  `dbapp_messages.h`，不另起 C# 侧 `Protocol/` namespace。
- Source Generator 在 `ATLAS_CLIENT` 宏下生成 `ApplyReplicatedDelta` /
  `Deserialize` / exposed RPC 发送存根 / `client_methods` 接收分发，
  服务端 / 客户端共用同一套 def 描述。

## 关键设计决策

1. **面向 Unity 的 C# API + 共享 C++ 网络栈** — C# 仅承载游戏代码可见的
   高层；传输 + Login/Auth 下沉到 native DLL，直接复用服务端
   `lib/network`，避免双实现漂移。
2. **沿用服务端 RUDP** — 与服务端 `NetworkInterface +
   ReliableUdpChannel` 字节级一致；RUDP 参数（MTU = 1472 / 最大分片 255 /
   延迟 ACK 25ms）不在 C# 镜像。
3. **协议简化** — 初期使用显式 `EntityID` + 全精度位置；不引入
   `IDAlias / PackedXYZ / CacheStamps`。
4. **`AvatarFilter` 延迟自适应** — 对齐 BigWorld `AvatarFilterHelper`：
   8 采样环形缓冲 + `idealLatency = latencyFrames × interval` +
   `velocity × |diff|^curvePower` 收敛（`curvePower = 2.0`）+ 线性插值 +
   外推限制。
5. **初期不实现** — IDAlias / 位置压缩 / CacheStamps / DataDownload /
   Vehicle / 客户端预测 / 断线重连。

## 剩余工作

- `LoginClient` async/await 包装（当前由 `AtlasNetworkManager.Login +
  LoginFinished` event 提供）
- `AtlasClient` 高层主入口（`ConnectAsync` / `Update` / 事件），统一
  桌面 / Unity 调用路径
- 端到端集成测试（`tests/integration/test_client_flow.cpp` 拉真 LoginApp +
  BaseApp + DBApp 集群，驱动完整 Create→Login→Authenticate→RPC→Disconnect）
