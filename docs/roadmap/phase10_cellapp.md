# Phase 10: CellApp — 空间模拟

**Status:** ✅ 完成。`src/server/cellapp/` 已落地完整空间模拟栈：
cellapp / cell / space / cell_entity / witness / aoi_trigger /
ghost_maintainer / real_entity_data / offload_checker / controller_codec /
cellapp_messages / intercell_messages / cellapp_native_provider /
cell_aoi_envelope / cell_bounds。RangeList / RangeTrigger / Controllers
共享库位于 `src/lib/space/`。Phase 11 的 Real/Ghost/Offload 已回填到本
阶段代码（见下文）。Witness 性能优化（distance LOD、demand-based
bandwidth、priority heap）持续在 `docs/optimization/` 跟踪。
**前置依赖:** Phase 8 (BaseApp)、Phase 9 (BaseAppMgr)、DefGenerator
（按受众过滤的 delta API）
**BigWorld 参考:** `server/cellapp/cellapp.hpp`, `entity.hpp`, `witness.hpp`

## 目标

实现 MMO 核心 — 空间模拟引擎。CellApp 负责实时模拟：实体在空间中的
位置管理、RangeList 空间索引、AOI 计算、带宽感知的增量属性同步、
Controller 行为系统。Space / Cell 两级结构 + Real/Ghost 复制 + offload
迁移使得分布式多 CellApp 在本阶段已可工作。

## 类层次

```
CellApp : EntityApp                  — 与 BaseApp 同级
  ├── Space                            — 跨 CellApp 的逻辑空间，拥有所有 CellEntity
  │     └── Cell                       — Space 在本进程的子区域，持非拥有指针
  ├── CellEntity (C++ 薄壳)            — 属性 / 逻辑由 C# 管理；Real XOR Ghost
  │     ├── RealEntityData             — Real 专属：haunt 列表 + 复制状态
  │     └── (real_channel)             — Ghost 专属：指向 Real 所在 CellApp
  ├── RangeList / RangeTrigger         — `src/lib/space/`，完全对齐 BigWorld
  ├── AoITrigger                       — RangeTrigger 子类，驱动 Witness
  ├── Witness                          — AoI 复制管理：LOD + priority heap + 带宽预算
  ├── Controllers                      — MoveTo / Timer / Proximity（`src/lib/space/`）
  ├── GhostMaintainer                  — 每 tick 检查跨边界，diff haunt 列表
  ├── OffloadChecker                   — Real 跨 Cell 边界时发起 OffloadEntity
  └── CellAppNativeProvider            — INativeApiProvider C++ ↔ C# 桥
```

## 关键设计决策

### RangeList / RangeTrigger 完全复用 BigWorld

不简化、不替换。BigWorld 的 RangeList 是经 15 年 MMO 生产验证的核心算法：
双轴独立排序 + 交叉检测实现 2D 区域；`volatile float` 防止编译器优化导致
精度不一致；先扩后缩 trigger shuffle 避免假事件；哨兵节点避免边界检查。
"简化"反而容易引入 bug。

### 属性同步：C# DirtyFlags 替代 C++ eventHistory

| 角色 | BigWorld | Atlas |
|---|---|---|
| 脏标记 | C++ `PropertyOwnerLink` | C# Source Generator `DirtyFlags` |
| 历史记录 | C++ `eventHistory` | C++ `replication_state_`（最新快照 + 有界历史） |
| 序号 | C++ `lastEventNumber_` / `lastVolatileUpdateNumber_` | C++ `EntityCache.last_event_seq` / `last_volatile_seq` |
| Witness 输出 | 直接读 Entity 属性 | 收到 C# 序列化好的 owner / other 双 blob，按观察者状态转发 |

**两种流的语义不可混淆：**

| 流 | 语义 | 落后处理 |
|---|---|---|
| **属性事件流**（`event_seq`） | 有序、累积 | 必须按序补发 delta；断档超出 history 窗口则退回 snapshot/refresh |
| **Volatile 位置流**（`volatile_seq`） | Latest-wins | 直接发送最新绝对位置，丢弃中间帧 |

各 `EntityCache` 独立追踪两个序号；Witness 在 send 时分别处理。

### 位置在 C++ 管理

`position_` / `direction_` 在 C++ `CellEntity`。RangeList shuffle 直接读
`x() / z()`；位置在 C# 会迫使每次 shuffle P/Invoke 回 C#，成本不可接受。
C# 通过 `atlas_set_position()` NativeApi 设置位置；C++ 直接读。

### Real / Ghost / Offload

`CellEntity` 是 Real 与 Ghost 的合体：通过 `real_data_` (Real) 与
`real_channel_` (Ghost) 互斥区分。Real 持 `RealEntityData`（haunt 列表 +
复制序号），Ghost 是被动副本，写操作 log+skip（`cellapp_native_provider.cc`）。
Witness / Controller 只在 Real 上运行。`ConvertRealToGhost` /
`ConvertGhostToReal` 完成 offload 两端的状态切换；`GhostMaintainer` 每
tick 比对 BSP 边界生成 haunt diff，`OffloadChecker` 在 Real 跨 Cell 时
发起 `OffloadEntity` (3110)。

### `INativeApiProvider` 区分 BaseApp 与 CellApp

`SendClientRpc()` 在两个进程中行为不同：

- **BaseApp** — 直接发送到 Proxy 的客户端 Channel
- **CellApp** — 按观察者逐个展开；属性 delta 经 `SelfRpcFromCell`
  (Reliable) 回送，Volatile 位置经 `ReplicatedDeltaFromCell` (Unreliable)
  回送

同一 C# `entity.Client.ShowDamage()` 在不同进程自动路由到正确路径。

### EntityID 全集群唯一

`EntityID` 由 DBApp `IDClient` 在集群范围分配（CellAppMgr 通过 `app_id`
高字节避开冲突），不区分 base / cell 局部 ID。CellApp 单一索引
`entity_population_: EntityID → CellEntity*` 即为生命周期与 RPC 路由的
共同入口；offload 后 ID 不变。

### RPC 安全：双消息 + 四层纵深

CellApp 接收 RPC 拆分为 `ClientCellRpcForward`（客户端发起，REAL_ONLY，
需要 Exposed 校验 + direction 验证 + sourceEntityID 验证）和
`InternalCellRpc`（来自可信 BaseApp，无需校验）两条消息。合并会需要
`is_from_client` 标志，标志本身可被错误设置；拆分对齐 BigWorld
`runExposedMethod` vs `runScriptMethod`，且 Phase 11 引入 Ghost 时两条
都标记 REAL_ONLY，Ghost 透明转发。

四层纵深校验：

| 层 | 位置 | 检查 |
|---|---|---|
| 1 | BaseApp `OnClientCellRpc` | `IsExposed(rpc_id)` |
| 1.5 | BaseApp `OnClientCellRpc` | `direction() == 0x02`（防 Base RPC 走 Cell 通道） |
| 2 | BaseApp `OnClientCellRpc` | 跨实体且非 ALL_CLIENTS |
| 3 | CellApp `OnClientCellRpcForward` | `direction() == 0x02`（纵深） |
| 4 | CellApp `OnClientCellRpcForward` | OWN_CLIENT 时 `source == target` |

## 协议分层

| 段 | 用途 |
|---|---|
| `3000–3023` | CellApp 自有消息：`CreateCellEntity` (3000) / `DestroyCellEntity` (3002) / `ClientCellRpcForward` (3003) / `InternalCellRpc` (3004) / `CreateSpace` (3010) / `DestroySpace` (3011) / `AvatarUpdate` (3020) / `EnableWitness` (3021) / `DisableWitness` (3022) / `SetAoIRadius` (3023) |
| `3100–3111` | Inter-CellApp Real/Ghost 复制与 offload：`CreateGhost` (3100) / `DeleteGhost` (3101) / `GhostPositionUpdate` (3102) / `GhostDelta` (3103) / `GhostSetReal` (3104) / `GhostSetNextReal` (3105) / `GhostSnapshotRefresh` (3106) / `OffloadEntity` (3110) / `OffloadEntityAck` (3111) |
| `2010–2019` | 复用现有 `BaseApp` 入站消息：`CellEntityCreated` (2010) / `CellEntityDestroyed` (2011) / `CurrentCell` (2012) / `CellRpcForward` (2013) / `ReplicatedDeltaFromCell` (2015) / `BroadcastRpcFromCell` (2016) / `ReplicatedReliableDeltaFromCell` (2017) / `BackupCellEntity` (2018) / `ReplicatedBaselineFromCell` (2019)。**2014 已废弃保留**（曾为 `SelfRpcFromCell`） |
| `2022–2027` | BaseApp 外部接口：`ClientBaseRpc` (2022) / `ClientCellRpc` (2023) / `EntityTransferred` (2024) / `CellReady` (2025) / `CellAppDeath` (2026) / `ClientEventSeqReport` (2027) |

**传输可靠性约束：**

- `ReplicatedReliableDeltaFromCell` (2017) — **Reliable**：有序属性 delta、owner 基线、reliable=false 字段的快照补包
- `ReplicatedDeltaFromCell` (2015) — **Unreliable**：Volatile 位置 / 朝向（latest-wins）
- `BroadcastRpcFromCell` (2016) — **Unreliable**：广播 ClientRpc
- `CellRpcForward` (2013) — **Reliable**：BaseApp ↔ CellApp 服务端 RPC

属性 delta 必须按 `event_seq` 有序到达，**必须走 Reliable**。

### `0xF001` 过渡协议

Phase 10 首阶段通过 `BaseApp::FlushClientDeltas()` 用
`DeltaForwarder::kClientDeltaMessageId = 0xF001` 把 `client_delta_forwarders_`
中的 payload 发给客户端，载荷内含 `CellAoIEnvelope`：

```cpp
enum class CellAoIEnvelopeKind : uint8_t {
    EntityEnter = 1, EntityLeave = 2,
    EntityPositionUpdate = 3, EntityPropertyUpdate = 4,
};
struct CellAoIEnvelope {
    CellAoIEnvelopeKind kind;
    EntityID public_entity_id;     // 当前阶段使用 base_entity_id
    std::vector<std::byte> payload;
};
```

**约束：** `DeltaForwarder` 是 latest-wins，不能承载有序属性 delta。
属性 delta 改走 `ReplicatedReliableDeltaFromCell` (2017)（Reliable，BaseApp
侧直达 client channel，不经 `DeltaForwarder`）；Volatile 位置走
`ReplicatedDeltaFromCell` (2015) → `DeltaForwarder` → `0xF001`。

Phase 12 落地真实客户端协议后，由 BaseApp 在下行前解包 `CellAoIEnvelope`；
当前 client 仍以 `0xF001` opaque envelope 路径接收（`src/client/client_app.cc`）。

## Tick 内并发 / 重入约束

CellApp 单线程：`OnStartOfTick → Updatables → (C# OnTick →
PublishReplicationFrame) → OnTickComplete(TickControllers → TickWitnesses)`。
Controller 可改变位置 → `RangeList::Shuffle` → `AoITrigger::OnEnter/OnLeave`
→ `Witness::AddToAoi/RemoveFromAoi`。约束：

| # | 规则 | 理由 |
|---|---|---|
| 1 | `TickControllers()` 完全结束才 `TickWitnesses()` | Controller 改位会在 Witness 遍历中改 `aoi_map_`，迭代失效 |
| 2 | `Witness::Update()` 期间禁止修改自己的 `aoi_map_`；只允许 mark `GONE`，末尾 compaction 擦除 | mid-iteration erase 留悬垂键 |
| 3 | `priority_queue_` 元素是稳定键（`EntityID` 或 `EntityCache` arena 索引），不存 `unordered_map::value_type*` | rehash 后 bucket 迁移更难测试 |
| 4 | `CellEntity` 析构序：`witness_.reset()` → `controllers_.StopAll()` → 从 `range_list_` 移除 | Witness 拥有 RangeList 的 AoITrigger 节点 |
| 5 | tick_witnesses 期间禁止从 C# 回调走 `space.find_entity` 销毁实体 | 同 #2，需延迟到 tick 末尾 |
| 6 | `replication_state_.history` 写在 PublishReplicationFrame，读在 `Witness::SendEntityUpdate`，两阶段不重叠 | 单线程严格前后序，无需互斥 |

**实体销毁级联：**

- 被观察实体销毁：`Space::RemoveEntity()` 广播给所有持有该 entity 的
  Witness 设 `cache.flags |= GONE`；实际擦除在各 Witness 下次 compaction；
  `RangeList::Remove()` 同步执行。
- 观察者本身销毁：按析构序 #4 进行；`Witness::~Witness` 自动从 RangeList
  移除 AoITrigger 节点，对 aoi_map_ 中剩余实体**不发 EntityLeave**（客户端
  channel 由 BaseApp 在 Proxy 解绑时整体回收）。
- 观察者掉线：`BaseApp::UnbindClient` 自动向 cell 发 `DisableWitness`，
  witness 立即释放；handler 对未知 entity / 无 witness 的情况 log-warn 返回。

## 性能基线

| 指标 | 定义 | Phase 10 目标 |
|---|---|---|
| Tick 时长 | `ServerApp::Tick()` 单次 wall-clock | p50 < 20ms / p99 < 50ms @ 10Hz / 1000 实体 |
| RangeList shuffle CPU | 聚合耗时 | 占单 tick CPU < 20% |
| Witness tick CPU | `TickWitnesses()` 聚合耗时 | 占单 tick CPU < 50% |
| 单观察者带宽 | 经 `ReplicatedReliableDeltaFromCell + ReplicatedDeltaFromCell` 出口字节 | < 4 KB/tick（≈ 40 KB/s @ 10Hz） |
| history 内存 | `Σ replication_state.history × sizeof(ReplicationFrame)` | < 4 MB / Space / 1000 实体 @ window=8 |
| snapshot fallback 率 | fallback 触发 / 总发送 | < 1%（稳态） |

`max_packet_bytes` 测度位置：Witness 构造 `ReplicatedReliableDeltaFromCell::payload`
的**内层**字节数（即 `CellAoIEnvelope` 序列序列），不含 header 与网络 frame；
deficit 以"溢出字节"计，下 tick 扣预算。压测用例放在
`tests/integration/test_cellapp_perf.cpp`（默认不跑）。

## AvatarUpdate 安全策略

Phase 10 采取**不做复杂反作弊但保留安全上限**：

| 校验 | Phase 10 处理 |
|---|---|
| 位置源合法性 | BaseApp 只接受该 Proxy 持有实体的上行（`FindProxyByChannel()`） |
| 单 tick 位移上限 | CellApp 在 `OnAvatarUpdate` 中按 `delta.length() > kMaxSingleTickMove` 拒绝并 WARN；`kMaxSingleTickMove = max_speed × dt × 2`，默认 50m/tick |
| NaN/Inf | 反序列化后立即 `std::isfinite()` 三分量校验，失败 drop + WARN |
| 权威性 | 客户端位置直接信任（除上限）；服务端权威移动 Phase 11+ |
| 频次限制 | Phase 10 不做；统一 rate limiter 走 machined 层（Phase 11+） |

## 待跟进 follow-up

### 已合并的结构性变更

- Real / Ghost / Offload 已与 Phase 10 同步落地（`ghost_maintainer`、
  `real_entity_data`、`offload_checker`，3100–3111 消息段）
- `ClientCellRpcForward` 信任边界：信封带 `source_forwarding_cellapp` +
  machined peer 白名单
- `Space::Tick` silent compaction 已移除
- EntityID 跨 CellApp 唯一：CellAppMgr 分配 `app_id` 作为高字节
- `SelfRpcFromCell` (2014) 已删除；reliable owner 流统一走
  `ReplicatedReliableDeltaFromCell` (2017)
- `SerializeReplicatedDelta*` legacy API 已移除；只剩
  `BuildAndConsumeReplicationFrame`

### 仍待处理

- 客户端 RPC 速率预算尚无统一 rate limiter（建议走 machined 层）
- `Witness::HandleAoIEnter` 在 `try_emplace` 时不覆盖刚设的 `kGone` flag
  （peer 同 tick 内 leave-then-re-enter 的冗余 Enter，无害但浪费带宽）
- `EntityRangeListNode::owner_data_` 用 `void*` + `reinterpret_cast`；契约
  由注释维护，可换为 `IRangeListOwner*` 强类型接口
- `Witness::Update` priority heap 每 tick `std::make_heap`，AoI 极大
  （数千 peers）时考虑 incremental 维护（详见
  [optimization/incremental_priority_queue.md](../optimization/incremental_priority_queue.md)，
  当前 0.40 % CPU，未达触发线）
- `DeltaSyncEmitter` 在所有 audience mask 都没命中脏位时仍写 1-byte
  flags 头；short-circuit `if flags == 0 return` 跳过整段序列化
