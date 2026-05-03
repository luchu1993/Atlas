# 属性同步

> 关联：[component_design.md](./component_design.md) · [container_property_sync_design.md](./container_property_sync_design.md) · [BigWorld EventHistory 参考](../bigworld_ref/BIGWORLD_EVENT_HISTORY_REFERENCE.md) · [phase10_cellapp.md](../roadmap/phase10_cellapp.md)

Atlas 用 **DirtyBit + audience 拆分** 做属性同步：C# 属性 setter 标 `_dirtyFlags` → CellApp 帧末 `BuildAndConsumeReplicationFrame` 写两份 audience-filtered delta（owner / other） + 两份 audience-filtered snapshot → 通过两条物理通道下发到 BaseApp 中继到 client。这套设计明确**不**是 BigWorld 的 EventHistory（per-observer 游标 + 优先级队列），原因与权衡见 §6。

---

## 1. 通道与消息

| envelope kind | 通道 | Cell→Base msg | Client wire id | 内容 |
|---|---|---|---|---|
| `kEntityEnter` (1) | reliable | `kReplicatedReliableDeltaFromCell` (2017) | `0xF003` | AoI peer 进入 + 初始 other-snapshot |
| `kEntityLeave` (2) | reliable | 同上 | `0xF003` | AoI peer 离开 |
| `kEntityPropertyUpdate` (4) | reliable | 同上 | `0xF003` | 属性 delta（含 `event_seq` 前缀） |
| `kEntityPositionUpdate` (3) | **unreliable** | `kReplicatedDeltaFromCell` (2015) | `0xF001` | 位置/朝向（30 B 定长） |
| baseline | reliable | `kReplicatedBaselineFromCell` (2019) | `0xF002` | owner-snapshot 周期重放 |
| backup | reliable | `kBackupCellEntity` (2018) | — | cell→base opaque blob，用于 DB / offload / reviver |

`reliable` / `unreliable` 在 cellapp 侧由 `Witness` 的两个 `SendFn` 区分（`witness.h:38`），由 `CellApp::AttachWitness` 注入。Cell→Base 之后所有 reliable envelope 直接转 `0xF003`；unreliable envelope 进 `DeltaForwarder`（见 §3）。

注意：`.def` 上的 `reliable="true"` 标志被 generator 写进 `DefEntityTypeRegistry` 的二进制描述符（`TypeRegistryEmitter.cs:132`），但**当前 C# 发射器并不据此分流通道**。所有 property delta 都走 reliable，volatile 通道仅承担 `kEntityPositionUpdate`。`reliable` 标志保留是为支撑后续 per-property 重要性区分。

---

## 2. 服务端发射路径

```
C# 属性 setter            Generator 产物                       帧末 (BuildAndConsumeReplicationFrame)
─────────────             ───────────────                       ────────────────────────────────────
avatar.Hp = 60   ──→  if (_hp != value) {                  ──→  if (!hasEvent && !hasVolatile) return false;
                        var old = _hp;                          if (hasEvent) {
                        _hp = value;                              ++_eventSeq;
                        _dirtyFlags |= ...Hp;                     SerializeForOwnerClient(ownerSnapshot);
                        OnHpChanged(old, value);                  SerializeForOtherClients(otherSnapshot);
                      }                                           SerializeOwnerDelta(ownerDelta);
                                                                  SerializeOtherDelta(otherDelta);
                                                                  _dirtyFlags = None;
                                                                  ClearDirtyComponents();
                                                                }
                                                                if (hasVolatile) ++_volatileSeq;
```

四份 SpanWriter 由 cellapp pump 分配并 reset。Audience 拆分把 `_dirtyFlags` 与编译期常量 `OwnerVisibleMask` / `OtherVisibleMask` 求交，分别投递。

### 2.1 Audience mask（`PropertyScope` → 受众）

| scope | Owner 可见 | Other 可见 |
|---|---|---|
| `OwnClient` | ✓ | – |
| `AllClients` | ✓ | ✓ |
| `CellPublicAndOwn` | ✓ | – |
| `BaseAndClient` | ✓ | – |
| `OtherClients` | – | ✓ |
| `CellPrivate` / `CellPublic` / `Base` | – | – |

由 `PropertyScopeExtensions.IsOwnClientVisible` / `IsOtherClientsVisible` 决定。Generator 把每个 `ReplicatedDirtyFlags` 位按是否落入 mask 编译进 `OwnerVisibleScalarMask` / `OtherVisibleScalarMask` 等常量，运行时 `_dirtyFlags & mask` 一次过滤。

### 2.2 sectionMask 字节布局

每条 audience delta 以 `sectionMask` byte 起头：

```
[u8 sectionMask]                       bit0 = scalar dirty, bit1 = container dirty, bit2 = component dirty
[if bit0] [u8/u16/u32/u64 scalarFlags] [scalar values...]
[if bit1] [u8/u16/u32/u64 containerFlags] [container op-log...]
[if bit2] [u8 activeSlots] [for each slot: u8 slotIdx + per-component delta...]
```

flags 整型宽度按属性数量自动选型（≤8→byte，≤16→ushort，≤32→uint，≤64→ulong）。客户端 `ApplyReplicatedDelta` 反向解码（`DeltaSyncEmitter.EmitClientApplyReplicatedDelta`）。

---

## 3. DeltaForwarder：unreliable volatile 通道的 latest-wins 队列

`src/server/baseapp/delta_forwarder.{h,cc}` —— per-client 队列，承载 `kEntityPositionUpdate` 唯一一类 envelope。

```
PendingDelta { entity_id, delta bytes, deferred_ticks, priority }
```

- 同一 entity 后到的 delta **整条替换**前一条（latest-wins）。位置流的语义允许这样做。
- `Flush(channel, kDeltaBudgetPerTick = 16 KB)` 按 priority desc / deferred_ticks desc 排序后取前 N 条（`baseapp.h`）。
- `kMaxDeferredTicks = 120` 兜底强发，避免低优先级 entity 永远饿死。
- watcher：`baseapp/delta_bytes_sent_total` / `baseapp/delta_bytes_deferred_total` / `baseapp/delta_queue_depth`（`baseapp.cc:382-395`）。

不变量：**累积型状态**（HP、库存、event_seq）**禁**走 DeltaForwarder —— latest-wins 会吞中间帧。Cumulative 一律走 reliable path（`OnReplicatedReliableDeltaFromCell` 直发，`baseapp.cc:878`）。

---

## 4. Baseline：丢包/AoI-enter 收敛兜底

不可靠通道的位置流可以靠下一帧覆盖收敛；reliable property 通道理论不丢，但客户端**进入 AoI 后**仍需要拿到当前权威状态。两条路径合力保证最终一致：

### 4.1 周期 baseline（cell→base→client，每 ~6 s）

`CellApp::TickClientBaselinePump` 对每个 has-witness 的 entity 调 `SerializeForOwnerClient`，发 `kReplicatedBaselineFromCell` (2019)。BaseApp `OnReplicatedBaselineFromCell` 透传为 `0xF002` 给 owner client。`BaseApp::EmitBaselineSnapshots` 保留为 no-op：cell 是 cell-scope 字段唯一权威，base 侧 cell-scope 字段恒为默认零，从 base 拉 baseline 会发出 stale 数据。

客户端 `ClientEntity.ApplyOwnerSnapshot` 直写 backing field，**不**触发 `OnXxxChanged`。

### 4.2 Cell→Base 持久化备份（每 ~1 s）

`kBackupCellEntity` (2018)：cell 发 `SerializeEntity` 全量 bytes，BaseApp `SetCellBackupData` 存为 opaque blob，不反序列化。用途：

- DB 写盘 — `[magic 0xA7][version 1][base_len u32][base_bytes][cell_len u32][cell_bytes]` 拼接为单 blob，DBApp 不解析
- offload 迁移 / reviver — 直接喂给新 cell 的 `Deserialize`

字段在 base 与 cell 上**互斥**（`PropertyScope.IsBase()` xor `IsCell()`）—— generator 按 side 拆 `Serialize`/`Deserialize` 范围，对齐 BigWorld `data_description.ipp:113-131` "data only lives on a base or a cell but not both"。

---

## 5. 客户端接收侧：BigWorld 对齐的回调语义

对照 BigWorld `common/simple_client_entity.cpp::propertyEvent()`：初始快照与 baseline **不**触发 setter callback；只有 delta 触发。

| 路径 | 是否触发 `OnXxxChanged` |
|---|---|
| `Deserialize`（初始快照 / 进入 AoI） | ✗（直写字段，末尾调 `OnEnterWorld()`） |
| `ApplyOwnerSnapshot`（baseline 0xF002 / 重新进 AoI） | ✗ |
| `ApplyOtherSnapshot`（`kEntityEnter` 邻居快照） | ✗ |
| `ApplyReplicatedDelta`（运行期 delta） | ✓ |
| `ApplyPositionUpdate`（volatile） | 仅触发 `OnPositionUpdated`，**不**走属性回调链 |

`ClientEntityManager.OnEnter` 在 `ApplyOtherSnapshot` 完毕后调一次 `OnEnterWorld()`，脚本在此基于完整状态做整体初始化。这是设计取舍——baseline 静默恢复，脚本如要观察被 baseline 吞下的字段变化，须自行轮询字段或追踪 `event_seq` gap（`ClientEntity.NoteIncomingEventSeq` 累加 `EventSeqGapsTotal` 并 relay 到服务端 watcher `baseapp/client_event_seq_gaps_total`）。

---

## 6. 不做：BigWorld 风格的 EventHistory

EventHistory（事件队列 + per-observer 游标 + LOD 补发 + 优先级队列）解决"多观察者高效 + 天然可靠 + LOD 补发 + 带宽调度"。Atlas 暂不引入：

- 价值集中在多观察者 + LOD + 带宽调度场景，需要 CellApp + AoI 完整压力数据来定位真实瓶颈
- C# DirtyBit 与代码生成契合好，跨语言维护事件队列代价高
- 内存代价（每事件持序列化副本）与 trim 时机敏感

后续若 AoI 压测出现"unreliable 不够 + reliable 太重"的中间地带，候选方案是 **C# DirtyBit + C++ 侧轻量 EventHistory 混合架构**：检测留 C#，多观察者调度迁 C++，事件直接引用 delta bytes，环形缓冲替链表。届时客户端 wire 格式（sectionMask + flags + values）不变。

---

## 7. 可观测性

| watcher | 含义 |
|---|---|
| `baseapp/delta_bytes_sent_total` | unreliable channel 已发字节 |
| `baseapp/delta_bytes_deferred_total` | DeltaForwarder 因预算延后字节 |
| `baseapp/delta_queue_depth` | DeltaForwarder 队列深度 |
| `baseapp/reliable_delta_bytes_sent_total` | reliable channel 已发字节 |
| `baseapp/client_event_seq_gaps_total` | 客户端 reliable delta 跳号累计 |

