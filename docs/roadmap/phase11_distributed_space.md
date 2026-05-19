# Phase 11: 分布式空间 — Real/Ghost + CellAppMgr

**Status:** ✅ 完成。CellAppMgr（注册 / app_id 分配 / BSP 负载均衡 / 死亡
通知）、Real/Ghost 双模 CellEntity、GhostMaintainer / OffloadChecker、9 条
inter-cellapp 消息全部落地并接入 cellapp 主循环。`ShouldOffload` (7006)
**接收端已接线**（cellapp handler 切换 `Cell::should_offload_`，
OffloadChecker 读该标志决定是否跳过本 Cell），但 **CellAppMgr 侧无发送方** —
offload 当前由各 CellApp 本地 OffloadChecker 通过 BSP 几何自然驱动，
mgr 推送链路未连线。
**前置依赖:** Phase 10 (CellApp 单机)、Phase 9 (BaseAppMgr)
**BigWorld 参考:** `server/cellapp/real_entity.hpp`,
`entity_ghost_maintainer.cpp`, `server/cellappmgr/`

## 目标

将单 CellApp 扩展为多 CellApp 分布式空间。一个 Space 可被 BSP 树分区到
多个 CellApp 上；实体跨 Cell 边界时通过 Real/Ghost 机制保持 AOI 可见性；
Offload 实现无缝迁移。

## 关键设计决策

### Ghost 无 C# 实例

Ghost 是纯 C++ 数据容器（`CellEntity(GhostTag, …)` 不分配
`RealEntityData`，存非拥有 `real_channel_` 指针）。BigWorld 用 Python 对象
持属性以支持访问；Atlas 中属性以 blob 形式在 C++ 层流转，Witness 下行时
不解析，因此跨进程不需要再造一个 C# 实例。

| 字段 | 来源 | 语义 |
|---|---|---|
| 位置 / 方向 / on_ground | `GhostPositionUpdate`（latest-wins） | volatile |
| `other_snapshot` | `CreateGhost` 初态 / `GhostSnapshotRefresh` 重灌 | 受众过滤后的 other 视角基线 |
| `other_delta` history | `GhostDelta`（按 `event_seq` 累积） | 可补发 |
| `latest_event_seq` / `latest_volatile_seq` | 同上 | 与 Phase 10 字段对齐 |

跨 Cell 观察者必然是 non-owner（owning client 的 Proxy 固定指向 Real 所在
BaseApp），所以 Ghost 只镜像 other 受众那一路；owner_snapshot / owner_delta
永远落在 Real CellApp。Witness 看到 Ghost 时仍按"每观察者自己的发送进度"
（`last_event_seq` / `last_volatile_seq`）决定发什么。

**代价：** Ghost 上不能跑脚本逻辑，Atlas 的 Controller 全部只在 Real 上
执行（BigWorld 的 GHOST_ONLY Controller 不实现）。

### Offload 复用 Phase 10 的 Serialize / Deserialize

不尝试迁移 GCHandle（CLR 不支持跨进程）。流程：

1. 旧进程 C++ 通过 `SerializeEntity` NativeCallback 触发 C# 序列化
2. C# `ServerEntity.Serialize(ref SpanWriter)` → 完整状态 blob
3. 旧进程 `GCHandle.Free()`，`ConvertRealToGhost(new_real_channel)`
4. 新进程通过 `RestoreEntity` NativeCallback 触发 `EntityFactory.Create()`
   + `ServerEntity.Deserialize(ref SpanReader)`，`ConvertGhostToReal()`

`Serialize` 是完整状态序列化，generator 已覆盖全部 `.def` 属性。**不要**
为 Offload 再造 `SerializeFull / DeserializePersistent` 之类的并行对 —
会分裂"实体状态唯一写入口"的 Phase 10 契约。受众过滤的
`SerializeForOwnerClient / SerializeForOtherClients` 是 AoI 复制路径，
**与 Offload 无关**。

### CellEntity 析构序保留

Phase 10 的析构序（`witness_.reset() → controllers_.StopAll() → 合成
FLT_MAX shuffle → RangeList::Remove`）是 UAF 防护的关键。Phase 11 在
`CellEntity` 上**追加** Real/Ghost 区分字段（`real_data_` /
`real_channel_`），不动 Phase 10 已有字段（`witness_` / `controllers_` /
`range_node_` / `replication_state_` / `script_handle_` / `base_addr_`）。

`EnableWitness` / `Add*Controller` / `atlas_set_position` 等路径入口都
`assert(IsReal())` — Ghost 是纯数据镜像，脚本钩子只能活在 Real 上。

### Inter-CellApp 通信用 RUDP

Atlas 服务进程之间已经统一跑在 `NetworkInterface::StartRudpServer()` /
`ConnectRudpNocwnd()` 内部可靠 UDP 基线上。Phase 11 复用，不另起 TCP-only
控制面。`GhostMaintainer` 用 `ChannelResolver` 抽象与 cellapp 进程的
`channels_` 解耦（便于单测）。

### BSP 负载均衡简化

去掉 BigWorld 的多级 `EntityBoundLevels`，用简单负载差驱动：CellAppMgr
`OnTickComplete` 每 `kBalanceTickInterval = 30` ticks 调
`partition.bsp.Balance(kBalanceSafetyBound = 0.9f)`，调整后通过
`UpdateGeometry` (7005) 广播新 BSP 给受影响 CellApp。需要更精细再加
EntityBoundLevels。

### EntityID 跨 CellApp 唯一

CellAppMgr 在 `OnRegisterCellApp()` 中为每个 CellApp 分配
`app_id ∈ [1, 255]`（`kMaxCellAppAppId = 255`），作为 EntityID 编码与负载
上报 / Ghost 寻址的 namespace。集群级 EntityID 由 DBApp `IDClient` 池统一
分配（详见 Phase 7），CellApp 本身不在本地生成 EntityID — 实体随
`CreateCellEntity` (3000) 或 `OffloadEntity` (3110) 携带的 ID 进入
`entity_population_`。

### 信任边界

CellApp 维护 `trusted_baseapps_: unordered_set<Address>`，订阅 machined 的
BaseApp Birth/Death 事件实时维护白名单。`OnClientCellRpcForward` 首先校验
来源 channel 的 internal address 在白名单内；不在白名单的消息直接丢弃，
然后再做 target 是否 Real、RPC 是否 Exposed、direction、source==target
（OWN_CLIENT）等业务校验。CellApp ↔ CellApp 通道（GhostDelta /
OffloadEntity 等）走 RUDP，未额外建白名单。

## 协议

CellApp `3000–3099` 段已被 Phase 10 占用；Phase 11 从 `3100` 起。

### Inter-CellApp（Real ↔ Ghost）

| 消息 | ID | 方向 | 用途 |
|---|---|---|---|
| `CreateGhost` | 3100 | Real → Ghost CellApp | 创建 Ghost；携带 `other_snapshot` 初态 + 序号 + real_cellapp_addr |
| `DeleteGhost` | 3101 | Real → Ghost | 删除 Ghost |
| `GhostPositionUpdate` | 3102 | Real → Ghost | volatile 位置 / 方向（带 `volatile_seq`） |
| `GhostDelta` | 3103 | Real → Ghost | 按 other 受众过滤的 `other_delta` + `event_seq` |
| `GhostSetReal` | 3104 | 新 Real → Ghost | Offload 后通知新 Real 地址 |
| `GhostSetNextReal` | 3105 | 旧 Real → Ghost | Offload 前通知即将迁移 |
| `GhostSnapshotRefresh` | 3106 | Real → Ghost | Ghost `last_event_seq` 超出 history 窗口（kReplicationHistoryWindow = 8）时重灌基线 |

### Offload

| 消息 | ID | 方向 | 用途 |
|---|---|---|---|
| `OffloadEntity` | 3110 | 旧 → 新 CellApp | 完整 Real 数据：persistent_blob、owner/other snapshot、序号、Controller 状态、Haunt 列表、witness 配置 |
| `OffloadEntityAck` | 3111 | 新 → 旧 | Binary 成功标志 |
| `baseapp::CurrentCell` | 2012 | 新 CellApp → BaseApp | 复用 Phase 8 已有入站消息，通知 Base 新 Cell 地址 |

### CellAppMgr

| 消息 | ID | 方向 | 用途 |
|---|---|---|---|
| `RegisterCellApp` / `RegisterCellAppAck` | 7000 / 7001 | 双向 | 注册 + `app_id` 分配 + 配置回包 |
| `InformCellLoad` | 7002 | CellApp → Mgr | 负载 / entity_count 上报 |
| `CreateSpaceRequest` | 7003 | BaseApp / 脚本 → Mgr | 创建 Space |
| `SpaceCreatedResult` | 7007 | Mgr → 请求方 | 创建结果回包 |
| `AddCellToSpace` | 7004 | Mgr → CellApp | 分配 Cell |
| `UpdateGeometry` | 7005 | Mgr → CellApp | BSP 树 / Cell 边界更新（rebalance 后广播） |
| `ShouldOffload` | 7006 | Mgr → CellApp | 接收端已接线（切换 `Cell::should_offload_`），但 mgr 侧无发送方 — 由本地 OffloadChecker 自然驱动 |
| `baseapp::CellAppDeath` | 2026 | Mgr → BaseApp | CellApp 死亡时通知 BaseApp 触发备份恢复，含 `dead_addr` + rehome 列表 |

## CellApp 死亡处理

`OnCellAppDeath()` 把 BSP 叶节点重新指向幸存 CellApp，并通过
`baseapp::CellAppDeath` 通知 BaseApp 触发实体备份恢复。备份恢复路径
本身仍属 Phase 13 范围。

## 收尾 TODO

- [ ] 决定 `ShouldOffload` (7006) 的去留 — 当前 offload 完全由本地
      OffloadChecker 在 BSP 几何更新后自然触发，mgr 推送路径未连线；要么
      接入 rebalance 调度，要么在协议层面删除该消息
- [ ] `EntityRangeListNode::owner_data_` 仍是 `void*` + `reinterpret_cast`
      （契约由 `AoITrigger::OnEnter/OnLeave` 注释维护）；可换为
      `IRangeListOwner*` 强类型接口
- [ ] CellApp ↔ CellApp 通道（GhostDelta / OffloadEntity）尚未基于
      machined CellApp 注册做白名单；当前依赖 RUDP 内网假设
- [ ] **Real→Ghost 时未清理 C# 实例**（详见下节"已知缺口"）

## 已知缺口: Real→Ghost 时未清理 C# 实例

### 现象

`tools/bin/run_mvp_cluster.bat`（默认 4 cellapp + 600 NPC）跑 4 分钟时
每 cellapp 的 stdout 累计 20 MB 日志，~280K 条以下三类 WARNING：

- `atlas_publish_replication_frame on Ghost entity_id=N — rejected`
- `atlas_set_position on Ghost entity_id=N — rejected`
- `atlas_set_direction on Ghost entity_id=N — rejected`

伴随 `Slow tick: 1230ms (expected 50.0ms)` — 单 tick 拖到 25x，client
端表现为 sync 卡顿、AoI 内 NPC 间歇可见。

### 根因

`### Ghost 无 C# 实例` 这条设计决策在协议 / C++ 数据层已落实
（`CellEntity` Ghost 不分配 `RealEntityData`、`real_channel_` 持非拥有
指针、AoI 复制仅走 other 受众），但 **C# 侧没有对应的清理路径**：

1. NPC 跨 BSP 边界 → `OffloadChecker::Compute` 把它放进 ops
2. `TickOffloadChecker` 调 `op.entity->ConvertRealToGhost(peer)` —
   **只翻 C++ 状态**：清 witness、清 controllers、清 real_data_
3. C++ 侧没有 `destroy_script_entity_fn` callback（client provider 有
   `ClientDestroyEntityFn`，cellapp provider 没有对应物，见
   `cellapp_native_provider.h`）
4. C# `EntityManager._entities` 仍持有该实体的 GCHandle
5. 下一个 cellapp tick，`EntityManager.OnTickAll` 遍历 `_entities`，
   只判断 `entity.IsDestroyed`，**不识别 Ghost 状态** →
   `NpcAiComponent.OnTick` 继续运行，调 `Owner.SetPosition` /
   `Owner.SetDirection` / 末尾 generator-emitted `PublishReplicationFrame`
6. 三个 native callback 在 C++ 端的 `IsReal()` 守门处全部被拒，每个
   都打一条 WARNING

600 NPC × 20Hz × 一段时间内有相当比例处于 Real-在-别处 / Ghost-在-本地
= 几万条/秒，cellapp 的事件循环大部分 CPU 时间花在写日志和 native
callback 拒绝上 → 复制管线被拖慢 → 客户端 sync 间歇丢帧。

Avatar 不受这条影响因为它不会自走（owner-authoritative 在
client，cell 侧只接 ReportPos）。同时 Avatar 在 25eeb7e 已修了"Ghost
姿态过期"的 pingpong，留下的 NPC 部分只能靠这条独立任务收尾。

### 设计选项

**选项 A：Real→Ghost 时销毁 C# 实例，Ghost→Real 时重建**

跟 BigWorld 模型对齐（Ghost 没有 Python 对象），也跟现有"Ghost 是纯数据
镜像"的设计宣言一致。

C++ 改动：
- `cellapp_native_provider` 加 `destroy_script_entity_fn`（签名同
  `ClientDestroyEntityFn`），注册到 callback table
- `ConvertRealToGhost` 在翻状态**之前**调一次，把 C# 实例销毁
- `OnOffloadEntity` 走 promote-Ghost 分支时（已有 Ghost），不能依赖
  `_entities` 还有那个实体；统一走 `restore_entity_fn` +
  `msg.persistent_blob` 重建（fresh-create 分支已经这么做）

C# 改动：
- `EntityManager` 增加 `Destroy(uint entityId)` 入口，从 `_entities`
  摘除并触发 `OnDestroy`
- `restore_entity_fn` 入口确保即使该 id 已存在也走 destroy → recreate
  路径（否则 promote-Ghost 时残留旧实例）

代价：每次 NPC 跨界都跑一次 OnDestroy / RestoreEntity，比 B 方案重；
Controller 状态、event 订阅会跟 C# 实例一起没掉，但 Phase 11 的 Offload
设计本来就把 Controller 状态打进 OffloadEntity blob（`SerializeControllersForMigration`），
target 端 RestoreEntity 后从 blob 重建——这条契约已经在 Avatar 那里跑通。

**选项 B：C# 加 IsGhost 标志，OnTickAll 跳过 Ghost**

最轻：C++ 在 Real↔Ghost 转换时通过 callback 翻 C# 端的
`ServerEntity._isGhost`，`EntityManager.OnTickAll` 多一道
`if (entity.IsGhost) continue;` 守门，`PublishReplicationFrame` / Cell
RPC dispatch 在 C# 端也加守门（避免到 C++ 才被拒）。

代价：违背"Ghost 没有 C# 实例"的宣言，C# 实例和 GCHandle 一直挂着，
内存/GC 压力跨界后不释放；NpcAiComponent 的本地 `_target` / `_rng`
状态在 Ghost 期间停滞，等迁回来再用，可能跟 Real 端实际状态不一致。

**选项 C：纯 C++ 抑制（不动 C# 协议）**

直接在 `cellapp_native_provider` 的 `SetEntityPosition` /
`SetEntityDirection` / `PublishReplicationFrame` 里把 Ghost 拒绝路径从
WARNING 降到 DEBUG（或限频）。**不修问题、只压日志**，cellapp 仍然在
做大量无效 native trap。仅作为"修不了主因时的减噪 patch"，不推荐
作为主线解。

### 推荐方案 + 落地步骤

走选项 A，分两个 PR：

**PR 1：C++ → C# 销毁 callback**
1. `clrscript`：callback table 加 `destroy_script_entity_fn` 字段，
   注册同 `restore_entity_fn` 的 marshalling 风格
2. `cellapp_native_provider`：透出
   `destroy_script_entity_fn()` getter；`Cell::Init` 沿用既有 wiring
3. `Atlas.Runtime`：导出 `EntityManager.DestroyById(uint)` 给 native
   表绑定
4. 单测：`tests/integration/test_cellapp_handlers.cpp` 加一个
   "ConvertRealToGhost calls destroy_script_entity_fn" 用例

**PR 2：Offload 路径接线**
1. `CellApp::TickOffloadChecker` 在 `ConvertRealToGhost(peer)` **之前**
   触发 destroy_script_entity_fn（既有 `entity_lifecycle_cancel_fn`
   保持，二者顺序：cancel → destroy → C++ 翻 Ghost）
2. `CellApp::OnOffloadEntity` 的 promote-Ghost 分支统一走
   `restore_entity_fn`（之前只 fresh-create 分支用），保证目的端必有
   一个新建的 C# 实例
3. 验证：`tools/bin/run_mvp_cluster.bat` + 6 个 pingpong bot 跑
   60s，每 cellapp stdout 应回落到几百 KB 量级、零
   `atlas_publish_replication_frame on Ghost — rejected`、`Slow tick:`
   不再出现

完成后 README 的 Phase 11 状态可保持 ✅，Ghost-无-C#-实例的设计承诺第一次
在 MVP-scale 负载下成立。
