# Phase 12: 客户端 SDK

**Status:** ✅ 已落地。`atlas_net_client.dll` 单 `on_deliver` C API
(ABI 0x02000000)、`Atlas.Client` 实体管理 + `AvatarFilter` peer 路径接入
生产、`Atlas.Client.Desktop`（`AtlasClient` / `LoginClient`）+ Unity
`AtlasNetworkManager` 共用 `ClientCallbacks.DeliverFromServer` 解码、
控制台 `atlas_client.exe` 登录 + AoI + RPC 全链路、跨平台 CI 矩阵均已
就绪；FakeCluster 集成测试覆盖 enter/volatile envelope → AvatarFilter
完整 wire path。
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

## LoginClient / AtlasClient

`atlas_net_client.dll` 的 LoginApp / BaseApp 异步握手包成 `AtlasTask`
形状。共享类型（`LoginResult` / `AuthResult` / `LoginFailedException` /
`AuthFailedException`）落 `Atlas.Client/LoginTypes.cs`；`LoginClient` 与
`AtlasClient` 在 `Atlas.Client.Desktop`（CoreCLR）与 `Atlas.Client.Unity`
（Unity / IL2CPP）各有一份对称实现。

```csharp
using var client = new AtlasClient();   // 自带 ManagedAtlasLoop + ManagedRpcRegistry
await client.ConnectAsync("login.example.com", 20013, user, pwHash, ct);
// 主循环每帧 client.Update();   // poll native + drain coro loop
```

- **异常单一路径**：`LoginFailedException(AtlasLoginStatus, message)` /
  `AuthFailedException(message)` / 取消时抛 `OperationCanceledException`。
- **取消语义**：`AtlasCancellationToken` 触发 → 调 `AtlasNetDisconnect` +
  source `TrySetCanceled`；late native callback 看见 inflight slot 为空
  silently drop。
- **并发约束**：单线程模型，同一 `LoginClient` 实例同一时刻只允许一个
  inflight Login / Auth；违规抛 `InvalidOperationException`。
- **生命周期**：`Dispose` 先 `AtlasNetDestroy` 关闭 native ctx，再把任何
  pending source 置 Canceled，最后释放自身 `GCHandle`。`AtlasClient` 在
  `Dispose` 时同样清理它持有的 coro loop + RPC registry（除非 ctor 传
  `installCoroLoop: false`，由 host 自管）。
- **桌面 vs Unity 差异**：API 形态完全一致；两端各自实现 `IAtlasNetEvents`
  并经 `AtlasNetCallbackBridge.Register` 接入 `on_deliver`，再把 payload
  交给共享的 `Atlas.Client.ClientCallbacks.DeliverFromServer` 解码。底层
  P/Invoke trampoline 装载方式不同（桌面 `[UnmanagedCallersOnly]`，
  Unity `[MonoPInvokeCallback]` + 缓存 delegate 防 GC），其余共享。
- **`AtlasTask<T>.FromSource(IAtlasTaskSource<T>, short)`** 是 Atlas.Coro
  暴露给外部 assembly 的公共工厂：让 `LoginClient` 用自己的 source 类型
  包装 PInvoke 完成事件，无需走运行期分配路径。
- **Update 语义差异**：桌面 `AtlasClient.Update()` 同时 Poll +
  ManagedAtlasLoop.Drain；Unity 端 `Update()` 只 Poll，`UnityLoop` 由
  PlayerLoop 自动 tick。两端在 `Update` 里调
  `ClientCallbacks.EntityManager.TickInterpolation(dt)` 推动 peer 实体的
  AvatarFilter 收敛。

## AvatarFilter 数据路径

- 服务端 `Witness::SendEntityUpdate` / `BuildEnterEnvelope` 把
  `Clock::now()` 作为 `serverTime` 直接写进 38B volatile envelope
  （`[u8 kind=3][u32 eid][3f pos][3f dir][u8 og][f64 server_time]`）；
  enter envelope 同样在 on_ground 与 peer snapshot 之间夹一个 `serverTime`。
  服务端只读 `chrono::steady_clock`，不需要把时钟参数沿调用链下传。
- 客户端 `ClientCallbacks.DispatchPositionUpdate` / `DispatchEnter` 解
  `serverTime` 后转给 `ClientEntityManager.ApplyPosition` / `OnEnter`，
  最终落到 `ClientEntity.ApplyPositionUpdate`。
- `ClientEntity` 在第一次收到 peer 样本时 lazy-allocate `Filter`；owner
  实体（`ClientCallbacks.CreateEntity` 路径）置 `IsOwner = true` 跳过
  filter 走 snap 路径。
- 渲染端调 `ClientEntity.TryGetInterpolated(clientTime)` 拉插值结果；
  服务端授权纠正触发 `ResetInterpolation()` 清环重启。
