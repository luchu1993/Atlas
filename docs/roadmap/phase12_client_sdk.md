# Phase 12: 客户端 SDK

> 前置依赖: Phase 9 (LoginApp), Phase 10 (CellApp AOI), Script Phase 4 (Atlas.Shared 最小共享子集),
> **Unity Native Network DLL** (`docs/UNITY_NATIVE_DLL_DESIGN.md`, 至少 Phase 3 完成, 产出 `atlas_net_client`)
> BigWorld 参考: `lib/connection/server_connection.hpp`, `lib/connection/client_interface.hpp`, `lib/connection/avatar_filter_helper.hpp`
>
> **与 `UNITY_NATIVE_DLL_DESIGN.md` 的分工**:
> - **Unity Native DLL**: C++ `atlas_net_client` 动态库 — 负责 RUDP 传输、Login/Auth 状态机、消息收发, C API (P/Invoke) 暴露给 C#。
> - **本文档 (Phase 12)**: C# 侧客户端 SDK (当前代码位于 `src/csharp/Atlas.Client/`, 命名空间 `Atlas.Client`) —
>   实体管理、位置插值、Source Generator 生成的 RPC 分发、面向游戏代码的高层 API。

---

## 目标

提供面向游戏代码的 C# 客户端 SDK, 使 Unity 可以连接 Atlas 集群, 完成登录、实体同步、RPC 通信和位置插值。
底层网络栈 (RUDP + Login/Auth 状态机) 由 C++ 原生 DLL `atlas_net_client` 提供, C# 侧通过 P/Invoke 消费其 C API。

**核心优势:**
- 客户端与服务端共享 `Atlas.Shared` 库 (`SpanReader/Writer`、Entity 基类、MailboxTarget、MessageIds),
  实体定义、序列化代码、RPC 代理在编译期由 `Atlas.Generators.Def` 统一生成。
- 客户端与服务端共享 C++ RUDP 实现 (通过原生 DLL), 杜绝 C#/C++ 双实现协议漂移。

## 验收标准

- [ ] `Atlas.Client` 可发布为 NuGet 包 (含各平台 `atlas_net_client` 原生库), Unity 可引用
- [ ] 客户端可登录 Atlas 集群 (LoginApp → BaseApp 完整流程)
- [ ] 客户端可接收 AOI 内实体的创建/销毁/属性更新
- [ ] 客户端可调用 exposed cell/base 方法 (编译期类型安全)
- [x] 客户端可接收 `client_methods` RPC 调用 (DefGenerator 分发, 通过 `ClientCallbacks.ClientRpcDispatcher`)
- [ ] `AvatarFilter` 位置插值工作平滑 (延迟自适应)
- [ ] 断线检测可工作, 提供重连 API
- [ ] 带宽统计可查询
- [ ] 独立的命令行测试客户端 (`atlas_test_client`) 可验证全部功能
- [ ] 全部新增代码有单元测试

## 状态表 (2026-04-18)

| 子系统 | 状态 | 说明 |
|------|------|------|
| 工程骨架 `Atlas.Client.csproj` | ✅ | 位于 `src/csharp/Atlas.Client/`, 目标框架 `net9.0`, 引用 `Atlas.Shared` |
| `ClientEntity` 基类 + `ClientEntityFactory` + `ClientEntityManager` | ✅ | 注册/创建/销毁 + `SendCellRpc/SendBaseRpc` 入口已就位 |
| `ClientNativeApi` P/Invoke | 🚧 | 已声明对 `atlas_engine` 的 `AtlasLogMessage / AtlasSendBaseRpc / AtlasSendCellRpc / AtlasRegisterEntityType / AtlasSetNativeCallbacks / AtlasGetAbiVersion`; 还未绑定到独立的 `atlas_net_client.dll`, 无登录/Auth/Poll/诊断 API |
| `ClientCallbacks` (C++ → C# 回调表) | ✅ | `DispatchRpc / CreateEntity / DestroyEntity` 通过 `[UnmanagedCallersOnly]` 静态绑定 |
| `ClientEntityRegistryBridge` | ✅ | 供 DefGenerator 生成代码调用 `RegisterEntityType` |
| `atlas_net_client` 独立 DLL + C API | ⬜ | `src/lib/connection/` 目前只有占位 CMake, 未见 DLL 目标 |
| `LoginClient` async/await 登录包装 | ⬜ | 未实现 |
| 协议消息 payload 编解码 (`Protocol/`) | ⬜ | 目前仅能通过 `ClientCallbacks` 收到 RPC, 没有 `EntityEnter / PositionUpdate / PropertyUpdate` 等消息处理 |
| `AvatarFilter` 位置插值 | ⬜ | 未实现 (全仓无该类型) |
| `AtlasClient` 主入口 (ConnectAsync / Update / 事件) | ⬜ | 未实现 |
| `atlas_test_client` 命令行工具 | ⬜ | 未实现 |
| Atlas.Client 单元 / 集成测试 | ⬜ | `tests/csharp/` 下暂无 `Atlas.Client.Tests` |

---

## 1. BigWorld 架构分析与 Atlas 适配 (设计背景) ⬜

保留 BigWorld 参考 — 关键适配决策:
- 语言: C# 高层 SDK + C++ `atlas_net_client` 网络栈 (避免 C#/C++ 双实现)
- 传输: 原生 DLL 内 RUDP, C# 侧 P/Invoke
- 登录: DLL 内 Login/Auth 状态机, C# 侧 `async/await` 包装; `SessionKey` 不跨 FFI
- 实体选择: 每条消息显式 `EntityID` (不用 BigWorld `selectEntity` 隐式状态)
- 位置插值: `AvatarFilter` 对齐 BigWorld `AvatarFilterHelper` (8 采样环形缓冲 + 延迟自适应)
- 属性同步: `ApplyReplicatedDelta()` Source Generator 产物
- RPC: `RpcDispatcher.DispatchClientRpc()` 编译期分发
- 不实现: `IDAlias`、PackedXYZ、`CacheStamps` (初期)

## 1.3 Atlas.Shared 共享架构 ✅ (骨架)

当前 `Atlas.Shared` 已含: `SpanReader/Writer`、`Entity/Attributes/IEntityDef/VolatileInfo`、`Protocol/IRpcTarget + MessageIds`、`Rpc/IRpcProxy + MailboxTarget`、`DataTypes`、`Events`。客户端项目通过 `ProjectReference` 消费; 无 `netstandard2.1` 多框架拆分 — 当前统一为 `net9.0`。

---

## 2. 协议设计 ⬜

当前代码中除 Phase 9 的 `LoginRequest/LoginResult` 与 `Authenticate/AuthenticateResult` 外, Phase 12 的其它目标协议
(`ClientBaseRpc/ClientCellRpc/EnableEntities/AvatarUpdate/Heartbeat/Disconnect` 以及服务端下行的
`CreateBasePlayer/CreateCellPlayer/EntityEnter/EntityLeave/EntityPositionUpdate/EntityPropertyUpdate/ClientRpcCall/ResetEntities/ForcedPosition/LoggedOff`) 尚未作为 wire 消息落地。

注:
- Phase 10 首阶段 AOI 下行仍可能经由外层 `0xF001` + 内层 `CellAoIEnvelope` 的过渡协议; Phase 12 对外暴露稳定 SDK 协议前, 需要由 BaseApp 解包成真实客户端消息。
- `BroadcastRpcFromCell` 作为 AOI 广播优化路径, 需等 BaseApp 支持 observer fan-out 后才适用。

消息字段草案 (`CreateBasePlayer / CreateCellPlayer / EntityEnter / EntityLeave / EntityPositionUpdate / EntityPropertyUpdate / ClientRpcCall / ForcedPosition / AvatarUpdate / Disconnect`) 详见历史版本 / BigWorld 参考, 此处不再展开 —— 待服务端协议落地后同步补回。

---

## 3. 核心模块设计

### 3.1 AtlasClient — 主入口 ⬜

设计目标类 `AtlasClient : IDisposable`:
- `ConnectAsync(loginHost, loginPort, user, pw)` 驱动 `AtlasNetCreate → Login → Authenticate`
- `Update(dt)` 调 `AtlasNetPoll` + `AvatarFilter.UpdateLatency`
- 属性: `IsConnected / PlayerId / Entities / BytesReceived / BytesSent`
- 事件: `Connected / Disconnected / ForcedPositionReceived`
- `SendExposedRpc(entityId, rpcId, payload)` / `SendAvatarUpdate(pos, dir, onGround)`
- 进程级 `AtlasNetCallbackBridge` 维护 `ctx → AtlasClient` 映射

**当前状态**: 未实现; RPC 发送目前由 `ClientEntity.SendCellRpc/SendBaseRpc` 直接走 `ClientNativeApi`, 无主入口聚合。

### 3.2 ClientEntityManager ✅ (最小版本)

当前实现只提供 `Get / Register / Destroy / Count`。设计文档中的 `PlayerEntity / AllEntities / PlayerBaseCreated / PlayerCellCreated / EntityEntered / EntityLeft / EntityUpdated / EntityMoved / EntitiesReset` 事件, 以及
`HandleCreateBasePlayer / HandleEntityEnter / HandlePositionUpdate / HandlePropertyUpdate / HandleForcedPosition / HandleResetEntities` 消息处理 — **全部未实现**, 等 §2 协议落地后补齐。

### 3.3 ClientEntity ✅ (最小版本)

当前 `ClientEntity`:
- `EntityId / IsDestroyed / TypeName`
- `Deserialize(ref SpanReader)` (由生成器实现)
- `OnInit / OnDestroy`
- `SendCellRpc / SendBaseRpc` (走 `ClientNativeApi`)

未实现 (设计目标):
- `ServerPosition / ServerDirection / OnGround`
- `FilteredPosition / FilteredDirection` (依赖 `AvatarFilter`)
- `IsPlayer`
- 与 `ServerEntity` 共享 `SharedEntity` 引用 (当前客户端继承 `ClientEntity` 而非 `ServerEntity`)

### 3.4 AvatarFilter — 位置插值 ⬜

对齐 BigWorld `AvatarFilterHelper`: 环形缓冲 (8 采样) + 延迟自适应 (`idealLatency = latencyFrames × interval`, 实际延迟按 `velocity × |diff|^curvePower` 收敛) + 线性插值 + 外推限制。
**未实现**; 全仓无该类型。

### 3.5 Native DLL 绑定层 🚧

**当前状态**: `ClientNativeApi` 绑定到 `atlas_engine` 库 (统一引擎 DLL), 已导出:
- `AtlasLogMessage`
- `AtlasSendBaseRpc / AtlasSendCellRpc`
- `AtlasRegisterEntityType`
- `AtlasSetNativeCallbacks` (安装 `ClientCallbackTable { DispatchRpc, CreateEntity, DestroyEntity }`)
- `AtlasGetAbiVersion`

**缺失** (设计目标为独立 `atlas_net_client.dll`):
- `AtlasNetCreate / AtlasNetDestroy / AtlasNetPoll`
- `AtlasNetLogin / AtlasNetAuthenticate / AtlasNetDisconnect`
- `AtlasNetSetCallbacks` (完整回调集: `on_disconnect / on_player_base_create / on_player_cell_create / on_entity_enter / on_entity_leave / on_entity_position / on_entity_property / on_forced_position / on_reset_entities`)
- `AtlasNetGetState / AtlasNetGetStats / AtlasNetLastError`
- `AtlasNetCallbackBridge` 按 `ctx` 反查 `AtlasClient` 的桥接

`src/lib/connection/CMakeLists.txt` 目前仅为占位 (`# atlas_connection — placeholder`), 独立的 `atlas_net_client` DLL 目标也未建立。

### 3.6 LoginClient ⬜

`async/await` 包装 `AtlasNetLogin / AtlasNetAuthenticate` 的 `callback + user_data` 风格 C API。依赖 `GCHandle` + `[UnmanagedCallersOnly]` trampoline + `TaskCompletionSource`。
**未实现**。

---

## 4. 端到端流程 ⬜

完整流程 (登录 → CreateBasePlayer → EnableEntities → CreateCellPlayer → EntityEnter → AvatarUpdate/PositionUpdate/PropertyUpdate/RPC) 尚未端到端打通;
当前只完成 `Authenticate / AuthenticateResult` 之后的 RPC 收发骨架 (基于 `ClientCallbacks.DispatchRpc`)。

---

## 5. Source Generator 客户端集成 ✅ (已就位, 使用中)

`Atlas.Generators.Def` 已根据 `ATLAS_CLIENT / ATLAS_BASE / ATLAS_CELL` 宏生成不同方向的代码:
- `ATLAS_CLIENT`: `ApplyReplicatedDelta() / Deserialize()` + exposed RPC 发送存根 + `client_methods` 接收分发 (`ClientRpcDispatcher` 回调)
- `ATLAS_BASE / ATLAS_CELL`: `SerializeReplicatedDelta()` + `client_methods` 发送存根 + `cell/base_methods` 接收分发

客户端接收 `client_methods` RPC 的链路 (`ClientCallbacks.DispatchRpc → ClientRpcDispatcher.Invoke(entity, rpcId, reader)`) 已可用。

---

## 6. 实现步骤

| Step | 内容 | 状态 |
|------|------|------|
| 12.1 | Native DLL P/Invoke 绑定 (完整 `AtlasNet*` C API) | 🚧 已有最小骨架 (`ClientNativeApi` 绑 `atlas_engine`), 需扩展为 `atlas_net_client` 完整 API |
| 12.2 | 业务消息编解码 (`MessageCodec / ClientMessages / ServerMessages`) | ⬜ |
| 12.3 | `LoginClient` async/await 包装 | ⬜ |
| 12.4 | `ClientEntityManager + ClientEntity` 扩展 (事件、消息处理) | 🚧 骨架可用, 事件 / 消息处理未实现 |
| 12.5 | `AvatarFilter` | ⬜ |
| 12.6 | `AtlasClient` 主入口 | ⬜ |
| 12.7 | Source Generator 客户端条件 | ✅ (`Atlas.Generators.Def` 已支持 `ATLAS_CLIENT`) |
| 12.8 | `atlas_test_client` 命令行工具 | ⬜ |
| 12.9 | 集成测试 | ⬜ |

---

## 7. 文件清单汇总

已存在:
```
src/csharp/Atlas.Client/
├── Atlas.Client.csproj                 ✅
├── ClientEntity.cs                     ✅
├── ClientEntityFactory.cs              ✅
├── ClientEntityManager.cs              ✅ (最小)
├── ClientNativeApi.cs                  🚧 (绑 atlas_engine, 缺 AtlasNet* API)
├── ClientCallbacks.cs                  ✅
└── EntityRegistryBridge.cs             ✅
```

待创建:
```
src/lib/net_client/                     ⬜ (由 UNITY_NATIVE_DLL_DESIGN 产出)
src/csharp/Atlas.Client/
├── AtlasClient.cs                      ⬜ Step 12.6
├── LoginClient.cs                      ⬜ Step 12.3
├── AvatarFilter.cs                     ⬜ Step 12.5
├── Native/
│   ├── AtlasNetNative.cs               ⬜ Step 12.1 (独立 DLL 绑定)
│   ├── AtlasNetCallbacks.cs            ⬜
│   └── AtlasNetCallbackBridge.cs       ⬜
└── Protocol/                           ⬜ Step 12.2
    ├── MessageCodec.cs
    ├── ClientMessages.cs
    └── ServerMessages.cs

src/tools/atlas_test_client/            ⬜ Step 12.8
tests/csharp/Atlas.Client.Tests/        ⬜ Step 12.9 (目前无该测试工程)
tests/integration/test_client_sdk.cpp   ⬜
```

---

## 8. 依赖关系与执行顺序

```
UNITY_NATIVE_DLL_DESIGN Phase 0–3   ← 前置 (IL2CPP Spike + 依赖解耦 + C API 导出)
    │
Step 12.1 (Native 绑定)  ─┐
Step 12.2 (业务消息)      │── Step 12.3 / 12.4 ── Step 12.6 ── Step 12.8 ── Step 12.9
Step 12.5 (AvatarFilter) ─┘
Step 12.7 (Source Generator) ✅ 已完成
```

---

## 9. BigWorld 对照 ⬜ (参考)

保留 C++ `ServerConnection` ↔ Atlas `AtlasClient + atlas_net_client.dll` 的对照表 (见历史版本): C# 面向 Unity + 原生 DLL 复用服务端 RUDP; 单一 `EntityPositionUpdate` 取代 24 种 `avatarUpdate` 变体; `enterAoI + requestEntityUpdate + createEntity` 合并为 `EntityEnter`; `CacheStamps / IDAlias` 初期不实现。

---

## 10. 关键设计决策 ✅ (已确立)

1. **面向 Unity 的 C# API + 共享的 C++ 网络栈** — C# 仅承载游戏代码能看到的高层, 传输 + Login/Auth 下沉到 `atlas_net_client.dll`, 直接复用服务端 `lib/network`, 避免双实现漂移。
2. **沿用服务端 RUDP (由原生 DLL 承载)** — 与服务端 `NetworkInterface + ReliableUdpChannel` 字节级一致; RUDP 参数 (MTU=1472 / 最大分片 255 / 延迟 ACK 25ms) 无需在 C# 镜像。
3. **协议简化** — 初期使用显式 `EntityID` + 全精度位置; 不引入 `IDAlias / PackedXYZ / CacheStamps`, 等 AOI 压测数据稳定后再决定。
4. **AvatarFilter 延迟自适应** — 保留 BigWorld 核心算法: 8 采样环形缓冲 + `idealLatency = latencyFrames × interval` + `velocity × |diff|^curvePower` 收敛 (`curvePower = 2.0`)。
5. **初期不实现**: IDAlias / 位置压缩 / CacheStamps / DataDownload / Vehicle / 客户端预测 / 断线重连 — 按 AOI 压测与业务需求再推进。
