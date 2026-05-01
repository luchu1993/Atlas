# Phase 11: 分布式空间 — Real/Ghost + CellAppMgr

**Status:** ✅ 主线完成。CellAppMgr、Real/Ghost 双模 CellEntity、
GhostMaintainer / OffloadChecker、9 条 inter-cellapp 消息全部落地并接入
cellapp 主循环。剩余收尾：CellAppMgr 的 `ShouldOffload` 广播尚未在
rebalance 时调度（消息结构与 handler 已就位）。
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
`range_node_` / `replication_state_` / `script_handle_` /
`base_addr_` / `base_entity_id_`）。

`EnableWitness` / `Add*Controller` / `atlas_set_position` 等路径入口都
`assert(IsReal())` — Ghost 是纯数据镜像，脚本钩子只能活在 Real 上。

### Inter-CellApp 通信用 RUDP

Atlas 服务进程之间已经统一跑在 `NetworkInterface::StartRudpServer()` /
`ConnectRudpNocwnd()` 内部可靠 UDP 基线上。Phase 11 复用，不另起 TCP-only
控制面。`GhostMaintainer` 用 `ChannelResolver` 抽象与 cellapp 进程的
`channels_` 解耦（便于单测）。

### BSP 负载均衡简化

去掉 BigWorld 的多级 `EntityBoundLevels`，用简单负载差驱动：
`TickLoadBalance()` 每 30 ticks（≈1s）调 `partition.bsp.Balance(safety_bound = 0.9)`，
广播新 geometry 给受影响的 peers。需要更精细再加 EntityBoundLevels。

### EntityID 跨 CellApp 唯一

CellAppMgr 在 `OnRegisterCellApp()` 中分配 `app_id ∈ [1, 255]` 作为
EntityID 高字节；CellApp 本地使用低 24 位单调递增。Phase 10 的
`next_entity_id_` 本地分配模式已被替换。最大 255 个 CellApp。

### 信任边界

`ClientCellRpcForward` 信封含 `source_forwarding_cellapp` 字段；CellAppMgr
维护可信 peer 白名单（基于 machined 注册），CellApp 在 `OnClientCellRpcForward`
中校验。

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
| `RegisterCellApp` / `RegisterCellAppAck` | 7000 / 7001 | 注册 + `app_id` 分配 + 配置回包 |
| `InformCellLoad` | 7002 | CellApp → Mgr | 负载 / entity_count 上报 |
| `CreateSpaceRequest` | 7003 | BaseApp / 脚本 → Mgr | 创建 Space |
| `AddCellToSpace` | 7004 | Mgr → CellApp | 分配 Cell |
| `UpdateGeometry` | 7005 | Mgr → CellApp | BSP 树 / Cell 边界更新 |
| `ShouldOffload` | 7006 | Mgr → CellApp | 启用 / 禁用迁移（消息已定义；尚未在 rebalance 路径调用） |
| `baseapp::CellAppDeath` | — | Mgr → BaseApp | CellApp 死亡时通知 BaseApp 触发备份恢复 |

## CellApp 死亡处理

`OnCellAppDeath()` 把 BSP 叶节点重新指向幸存 CellApp，并通过
`baseapp::CellAppDeath` 通知 BaseApp 触发实体备份恢复。备份恢复路径
本身仍属 Phase 13 范围。

## 收尾 TODO

- [ ] CellAppMgr 在 BSP rebalance 后调度 `ShouldOffload` 广播（消息和
      handler 已就位，缺调用点）
- [ ] `EntityRangeListNode::owner_data_` 仍是 `void*` + `reinterpret_cast`
      （契约由 `AoITrigger::OnEnter/OnLeave` 注释维护）；可换为
      `IRangeListOwner*` 强类型接口
