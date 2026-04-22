# 属性同步机制设计：DirtyBit vs EventHistory

> 初版 2026-04-15 / 最新 2026-04-22（§9 实施汇总；客户端接收侧按 scheme-2 终稿校正）
> 关联: [BigWorld EventHistory 参考](../bigworld_ref/BIGWORLD_EVENT_HISTORY_REFERENCE.md) | [Phase 10 CellApp](../roadmap/phase10_cellapp.md) | [Phase 12 Client SDK](../roadmap/phase12_client_sdk.md) | [Script-Client Smoke Runbook](../stress_test/script_client_smoke.md)
>
> **快速导航**：想看"当前代码是什么样"直接看 [§9 实施现状](#9-实施现状2026-04-22)；§1–§8 是设计决策的背景说明。

---

## 1. 背景

MMO 服务端的属性同步需要解决三个核心问题：

1. **增量同步** — 只发送变化的属性，而非每帧全量
2. **多观察者** — 一个实体被 N 个玩家观察，每人网络状态不同
3. **带宽控制** — 高负载时按优先级裁剪，不能突发大包

BigWorld 使用 **EventHistory**（事件队列 + per-observer 游标）解决这三个问题。Atlas 当前使用 **DirtyBit**（位掩码 + 帧末 delta 序列化）。本文档对比两种方案并给出演进路线。

---

## 2. 两种机制对比

### 2.1 Atlas DirtyBit（当前实现）

```
C# 属性 setter            生成代码                    C++ 网络层
───────────────          ──────────                  ──────────
avatar.Hp = 60  ──→  _dirtyFlags |= Hp
                         │
                    (帧末 sync loop)
                         │
                    IsDirty? → true
                         │
                    SerializeReplicatedDelta()
                    写: [flags=0x01] [hp=60]  ──→  ReplicatedDeltaFromCell
                         │                          (unreliable, msg 2015)
                    ClearDirty()                        │
                                                   BaseApp 转发 → Client
                                                        │
                                                   ApplyReplicatedDelta()
                                                   _hp = 60
```

**关键特征：**
- 每实体一个 `_dirtyFlags` (byte/ushort/uint/ulong，按属性数量自动选型)
- 属性 setter 中 `if (old != value)` 才标脏，同值赋值无开销
- `SerializeReplicatedDelta()` 写 flags + 仅 dirty 字段
- Scope 过滤编译期静态分流：`SerializeForOwnerClient()` / `SerializeForOtherClients()`
- Unreliable 投递，假设"下一帧会覆盖"

### 2.2 BigWorld EventHistory

```
Entity
  ├── lastEventNumber_ (单调递增)
  ├── eventHistory_ (事件链表)
  │     [#12 HP=80] → [#13 Name="xx"] → [#14 HP=60] → [#15 MP=50]
  │
  └── Witness
        └── aoiMap_
              ├── EntityCache(B): lastEvent=#12  → 需发 #13,#14,#15
              ├── EntityCache(C): lastEvent=#14  → 需发 #15
              └── EntityCache(D): lastEvent=#15  → 无需发送
```

**关键特征：**
- 每次属性变更生成带序号的 `HistoryEvent`，保留序列化数据
- 每个观察者维护独立游标 `lastEventNumber_`，只遍历未收到的事件
- per-LOD 游标数组 `lodEventNumbers_[]`，LOD 升级时从历史补发
- `LatestChangeOnly` 优化：高频属性只保留最新事件，O(1) 替换
- 优先级队列 `entityQueue_` 做带宽预算
- `trim()` 周期性裁剪已被所有观察者消费的事件

---

## 3. 优劣分析

### 3.1 DirtyBit 的优势

| 维度 | 说明 |
|---|---|
| **实现简单** | 位运算 + 条件写入，代码生成友好，边界条件少 |
| **内存极低** | 每实体仅 1~8 bytes flags，无序列化数据副本 |
| **无需裁剪** | 没有事件队列，没有 GC/trim 问题 |
| **C# 集成好** | 属性 setter 自动标脏，编译期生成，零运行时开销 |

### 3.2 DirtyBit 的不足

| 维度 | 说明 |
|---|---|
| **多观察者低效** | `OtherClients` 属性需为每个 AOI 观察者分别构造 delta |
| **无带宽调度** | 每帧发全部 dirty 属性，高负载时突发大包 |
| **丢包不可恢复** | Unreliable 投递，属性改一次后长期不再改则客户端永久停留旧值 |
| **LOD 升级无历史** | 实体走近时新暴露的 LOD 层级属性没有历史可回放 |

### 3.3 EventHistory 的优势

| 维度 | 说明 |
|---|---|
| **多观察者高效** | 一次序列化，N 个观察者各自从游标位置读取 |
| **天然可靠** | 游标不前进 = 下帧自动重发，无需额外重传机制 |
| **LOD 补发** | per-LOD 游标在层级变化时自动回放历史事件 |
| **带宽调度** | 配合优先级队列，超预算实体延迟到下帧 |
| **LatestChangeOnly** | 高频属性队列中只保留最新值，减少冗余发送 |

### 3.4 EventHistory 的代价

| 维度 | 说明 |
|---|---|
| **内存重** | 每事件持有序列化数据副本，观察者游标差距大时队列长 |
| **实现复杂** | 链表管理、latestEventPointers、per-LOD 游标、trim 分时间片、Ghost 同步 |
| **Ghost 强耦合** | Real→Ghost 事件编号/顺序/内容必须严格一致 |
| **裁剪微妙** | 太快 → 慢客户端游标失效；太慢 → 内存爆炸 |

---

## 4. 决策：保持 DirtyBit，针对性补强

Atlas 当前阶段不引入完整的 EventHistory 机制。

**理由：**

1. **CellApp/AOI 尚未完整实现** — EventHistory 的核心价值（多观察者游标、LOD 补发、带宽调度）只有在 CellApp + AOI 完整运行时才体现，现在引入是 premature optimization
2. **DirtyBit 与 C# 代码生成契合度高** — setter 标脏 → 帧末序列化 → 清空，流程简洁
3. **EventHistory 复杂度高** — 在脚本层维护事件队列需要大量跨语言同步逻辑，收益比不高

---

## 5. 近期补强项（CellApp 完成前）

### 5.1 丢包恢复：reliable 通道 + baseline 兜底

**问题：** Unreliable 投递丢包后，若属性不再变化，客户端会永久停留旧值。

**落地方案：**
- **reliable 属性（`.def` 声明 `reliable="true"`）** 走 `ReplicatedReliableDeltaFromCell` (msg 2017) → 0xF003 reliable channel，transport 层（RUDP）自动重传。
- **unreliable 属性** 走 `ReplicatedDeltaFromCell` (msg 2015) → 0xF001 unreliable channel，靠 §5.1a 的 baseline pump 作为最终一致性兜底。
- **seq 跳号检测**：每个 reliable delta 带 `event_seq` 前缀，客户端跳号时在 `ClientEntity.NoteIncomingEventSeq` 累加到 `EventSeqGapsTotal`，提供应用层可观测性。

### 5.1a Baseline 架构（BigWorld 对齐）

属性的 "当前状态最终一致" 由 BigWorld 风格的两条路径合力保证：

1. **cell→base opaque backup**（`BackupCellEntity` msg 2018, reliable, 每 50 tick ≈ 1 s）：CellApp 把每个 has-base entity 的 `SerializeEntity` 输出发给 BaseApp，BaseApp 存入 `BaseEntity::cell_backup_data_` 不反序列化。对齐 BigWorld `cellapp/real_entity.cpp:884-906` + `baseapp/base.cpp:1182-1200`。用于 DB 持久化 / offload 迁移 / reviver。

2. **cell→base→client baseline relay**（`ReplicatedBaselineFromCell` msg 2019, reliable, 每 120 tick ≈ 6 s）：CellApp 对每个 has-witness entity 调 `GetOwnerSnapshot` 拿 cell-side `SerializeForOwnerClient` 输出，BaseApp 中继为 `ReplicatedBaselineToClient` (0xF002) 发给 owner client。客户端侧 `ApplyOwnerSnapshot` 直写字段，不触发回调（见 §5.1b）。BaseApp 自己的 `EmitBaselineSnapshots` 保持 no-op —— baseline 源头永远是 cell（唯一有权威 cell-scope 数据的进程）。

两条路径的基础是 generator 按 scope 拆分字段：base partial 只 emit `DATA_BASE`（`base` / `base_and_client`）字段；cell partial 只 emit cell-scope 字段。对齐 BigWorld `lib/entitydef/data_description.ipp:113-131` 的 "data only lives on a base or a cell but not both"。

DB 持久化：`CaptureEntitySnapshot` 在写 DB 前把当前 base bytes + `cell_backup_data_` 拼成 `[magic=0xA7][version=1][base_len u32][base_bytes][cell_len u32][cell_bytes]`；`DecodeDbBlob` 在检出时拆回两段，cell 部分作为下一次 `CreateCellEntity.script_init_data` 喂给新 cell 的 `Deserialize`。保证 cell-scope `persistent="true"` 属性跨 DB 生命周期保留。

各 commit 与源码位点汇总见 [§9.1 / §9.3](#9-实施现状2026-04-22)。

### 5.1b `OnXxxChanged` 触发规则（BigWorld 对齐）

Baseline / 初始快照 **不触发** `OnXxxChanged`，只有运行期 delta 触发。对齐 BigWorld `client/entity.cpp:1124-1133` 把 `isInitialising` 翻成 `!shouldUseCallback` 交给 `common/simple_client_entity.cpp:135-160::propertyEvent`，后者在 `!shouldUseCallback` 下**直接写字段跳过 `set_<propname>` Python 回调**。

| Atlas | BigWorld | 触发 On*Changed？ |
|---|---|---|
| `ApplyOwnerSnapshot` / `ApplyOtherSnapshot`（0xF002 baseline / `kEntityEnter` 初始快照）| `onProperty(isInitialising=true)` | ✗ |
| `ApplyReplicatedDelta`（0xF001 / 0xF003 运行期 delta） | `onProperty(isInitialising=false)` | ✓ |

脚本层面的意义：**baseline 静默恢复**，脚本看不到被 baseline 恢复的字段变化。这是设计不是局限。脚本如果必须观察到 baseline 带来的字段变化，对齐 BigWorld 的做法是**自己读字段值**（周期轮询 `_hp` 等）或**通过 `seqgaps` 推断被吞了多少 event**。

### 5.2 带宽预算（DeltaForwarder，已实施）

**问题：** 大量实体同时变脏时，帧末 delta 总量可能超过网络承受能力。

**当前实现（`src/server/baseapp/delta_forwarder.h/.cc` + `baseapp.cc` 集成点）：**
- `DeltaForwarder` 队列在 BaseApp 侧维护 `PendingDelta{entity_id, delta, deferred_ticks}`。
- `OnReplicatedDeltaFromCell` 入队（同 entity 后到的 delta **整条替换**旧 delta，天然做跨帧积压合并）。
- `OnTickComplete` 按 `kDeltaBudgetPerTick = 16 KB` / tick flush，超预算的留到下 tick，`deferred_ticks` 用于优先级回退。
- Watcher：`baseapp/delta_bytes_sent_total` / `baseapp/delta_queue_depth` 已注册。

**未完成的打磨项**（tail work，非阻塞，见 §7.2）：
- `baseapp/delta_bytes_deferred_total` watcher 尚未注册。
- `PendingDelta.priority` 字段与排序比较器未落地；真实优先级需要 Witness 上下文（距离 / 属性类型 / 最近发送时间），等 Phase 10 CellApp 完成后再填。

**Reliable delta 不走 DeltaForwarder**：`OnReplicatedReliableDeltaFromCell` 直接转发（`baseapp.cc`），因为预算机制本质是"超限就丢"，reliable 不能丢。

### 5.3 LatestChangeOnly 标记（已搁置）

`.def` 加 `latest_only="true"` 标记、带宽受限时合并中间帧的原方案 **不再单独推进**。理由：`DeltaForwarder::Enqueue` 已经对同一 entity 做整条 delta 替换（`delta_forwarder.cc`），覆盖了"跨帧积压合并"的核心诉求。`latest_only` 的差异价值只在 per-property 事件粒度的合并场景才显现，而那要等 §6 远期的 C++ 侧 EventHistory 落地后再评估。归档决策详见 §7.1。

---

## 6. 远期演进（CellApp + AOI 完成后）

当 CellApp 和 AOI 系统完整实现后，引入**混合架构**：

```
C# 脚本层 (DirtyBit)                 C++ CellApp 层 (轻量 EventHistory)
─────────────────                    ──────────────────────────────────
avatar.Hp = 60                       
  → _dirtyFlags |= Hp               
                                     
帧末 SerializeReplicatedDelta()      
  → delta bytes                ──→   收到 delta，生成 HistoryEvent
                                       eventNumber = ++counter
                                       data = delta bytes (零拷贝引用)
                                     
                                     Witness::update() 每帧:
                                       for cache in aoiMap_:
                                         遍历 eventHistory_
                                         按 cache.lastEventNumber 游标
                                         按 detailLevel 过滤
                                         按带宽预算裁剪
                                         发送给客户端
                                         更新游标
```

**核心思路：**
- 属性变更检测仍由 **C# DirtyBit** 驱动 — 保持脚本层简洁
- 多观察者调度由 **C++ EventHistory** 处理 — 利用游标/LOD/带宽控制
- 事件数据直接引用 delta bytes，不做二次序列化 — 避免内存副本
- trim 绑定到帧末或独立协程
- 环形缓冲区替代链表 — cache 友好

### 6.1 与 BigWorld 的差异

| 设计点 | BigWorld | Atlas 混合方案 |
|---|---|---|
| 事件存储 | `list<HistoryEvent*>` 链表 | 环形缓冲区 (cache 友好) |
| 事件编号 | `int32` 单调递增 | `uint64` 避免回绕 |
| 事件数据 | 独立 `char*` 副本 | 引用 delta bytes (零拷贝) |
| 脏检测 | C++ 属性描述符 | C# DirtyBit (代码生成) |
| Ghost 同步 | 流式复制事件到所有 Haunt | 相同设计 |
| 裁剪 | 分时间片周期调用 | 帧末批量裁剪或协程 |
| 内存管理 | new/delete | 对象池或 arena 分配器 |

### 6.2 关键不变量

实现时必须保证：

1. **事件编号严格递增** — 单线程调用或原子操作
2. **游标只前进不后退** — `lastEventNumber_` 更新后不可回退
3. **新进入 AOI 发快照而非回放历史** — 防止历史积压导致带宽爆炸
4. **LatestChangeOnly 不丢失最终值** — 合并旧事件时保留最新数据
5. **Ghost 的 EventHistory 与 Real 保持一致** — 编号、顺序、内容相同

---

## 7. 近期补强清单（归档）

2026-04 之前的补强规划。四个补强项的最终落地见 [§9.1](#91-补强完成度)；这里只保留两条有长期参考价值的设计决策。

### 7.1 补强三 冻结决策

`.def` 的 `latest_only="true"` 标记、配合带宽受限时合并中间帧的原方案 **不再单独推进**。理由：`DeltaForwarder::Enqueue` 已经对同一 entity 做整条 delta 替换（`delta_forwarder.cc`），覆盖了"跨帧积压合并"核心诉求；`latest_only` 的差异价值只在 **per-property 事件粒度合并** 场景才显现，那是 §6 远期 EventHistory 落地后的议题。归档到 §6，等 C++ 侧引入 per-property 合并时再评估。

### 7.2 补强二 尾部工作（live）

`DeltaForwarder` 核心已落地（§5.2），但两个细节可以随时回补，等 Phase 10 CellApp Witness 能提供距离 / 属性类型 / 最近发送时间等上下文时再推进：

| # | 任务 | 文件 | 说明 |
|---|---|---|---|
| 2.5 | `baseapp/delta_bytes_deferred_total` watcher | `src/server/baseapp/baseapp.cc`（紧邻 `delta_bytes_sent_total` 注册块）| 聚合所有 `client_delta_forwarders_` 的 `DeltaForwarder::stats().bytes_deferred` |
| 2.6 | `PendingDelta.priority` 字段 | `delta_forwarder.h/.cc` | 追加 `uint16_t priority{0}`；`Enqueue()` 加参；`Flush()` 排序改 `(priority desc, deferred_ticks desc)`；同 entity 替换时取二者较大值 |
| 2.7 | 调用方传递 priority | `baseapp.cc::OnReplicatedDeltaFromCell` | 占位传 0；真实优先级等 Witness 上下文 |
| 2.8 | 单元测试扩展 | `tests/unit/test_delta_forwarder.cpp` | 高低 priority 排序、合并取大值、priority 相等退化为 deferred_ticks |

---

## 8. 客户端接收侧的对称设计（BigWorld 源码审计）

对照 BigWorld 源码（`common/simple_client_entity.cpp`、`client/entity.cpp`、`lib/connection_model/bw_entity.cpp`），记录 Atlas 客户端消费侧的对齐目标。落地情况见 [§9.2](#92-客户端对齐8-完成度)。

### 8.1 BigWorld 的双通道接收模型

**属性增量通道**（`common/simple_client_entity.cpp:135-163` 的 `propertyEvent()`）：

```cpp
bool propertyEvent(ScriptObject pEntity, const EntityDescription & edesc,
    int propertyID, BinaryIStream & data, bool shouldUseCallback)
{
    ScriptObject pOldValue = king.setOwnedProperty(propertyID, data);  // 先写值并取旧值
    if (shouldUseCallback) {                                            // enter-AoI: false；后续 delta: true
        BW::string methodName = "set_" + pDataDescription->name();
        Script::call(..., PyTuple_Pack(1, pOldValue.get()), ...);       // 自动调 set_<propname>(oldValue)
    }
}
```

两条关键约定：

1. **回调签名 `set_<propname>(oldValue)` 框架自动 invoke** —— 新值已就地写入对象，脚本只需 oldValue 做比较
2. **初始快照 vs 增量由 `shouldUseCallback` 区分** —— enter-AoI 批量下发属性时 `false`（不调回调），后续 delta `true`

**位置通道**（独立消息 `avatarUpdateNoAliasDetailed` / `avatarUpdatePlayerDetailed` / `forcedPosition`）：位置**不走属性系统**、**不触发 Python `set_position`**，只触发 C++ `onPositionUpdated()`。动机：位置高频、需要插值，混入属性回调流会吞 CPU 且语义错位。

Atlas 两条通道的现状实现见 §5.1b（`OnXxxChanged` 只由 delta 触发）与 §9.2（`OnPositionUpdated` 走 volatile 通道独立钩子）。

### 8.2 与 §6 远期架构的关系

§6 规划的 DirtyBit（C#）+ EventHistory（C++ 轻量）混合架构只改**服务端产出侧**。客户端消费侧的对齐与其**正交**：

- **服务端**：DirtyBit → HistoryEvent → per-observer cursor → 写 wire delta
- **客户端**：wire delta → `ApplyReplicatedDelta` 按 bitmap 分发 → 裸赋值 + 回调

不论服务端是否迁 EventHistory，客户端 wire 格式（`kind byte` + `flags bitmap` + `values`）不变，客户端侧实现可复用。

---

## 9. 实施现状（2026-04-22）

§5 – §8 累计规划了 4 个补强、7 个客户端对齐任务，以及多项 BigWorld 架构对齐工作。本节汇总代码现状，避免读者在 §7 / §8 的任务表之间追踪交叉引用。

### 9.1 补强完成度

| 项 | 状态 | 实际落地 |
|---|---|---|
| **补强一：Baseline 快照** | DONE（实现形态偏离原方案）| 原方案让 BaseApp 从 base 侧实例读 `SerializeForOwnerClient` 周期性发 0xF002。M2 落地后 base 侧 cell-scope 字段永远是默认 0，此方案会产生 stale baseline，故改由 CellApp 发。详见 §5.1a —— `ReplicatedBaselineFromCell` (msg 2019) → BaseApp 中继 → `ReplicatedBaselineToClient` (0xF002)，commit `f2dec1e`。`BaseApp::EmitBaselineSnapshots` 保留为 no-op 占位。 |
| **补强二：DeltaForwarder 带宽预算** | DONE（核心）| 核心四项：`src/server/baseapp/delta_forwarder.h/.cc`、`baseapp.cc:628-666` 入队+flush、`baseapp.h:236 kDeltaBudgetPerTick=16 KB`、`tests/unit/test_delta_forwarder.cpp`。tail work（watcher / priority）见 §7.2，非阻塞。 |
| **补强三：LatestChangeOnly 标记** | FROZEN | §7.1 决策：`DeltaForwarder::Enqueue` 已对同 entity 做整条 delta 替换，覆盖"跨帧积压合并"核心诉求；`latest_only` 差异化价值暂无业务需要。归档到 §6 远期。 |
| **补强四：Reliable / Unreliable per-property 分流** | DONE | `.def` 的 `reliable="true"` / `"false"` 标记由 `DeltaSyncEmitter` 拆成 `SerializeReplicatedDeltaReliable` / `SerializeReplicatedDeltaUnreliable`；C++ 侧 `kReplicatedReliableDeltaFromCell` (msg 2017) 与 `kReplicatedDeltaFromCell` (msg 2015) 双路径；reliable 直发（不入 DeltaForwarder 预算），unreliable 走预算限流。验证见 [script_client_smoke.md 场景 2/3](../stress_test/script_client_smoke.md)。 |

### 9.2 客户端对齐（§8）完成度

| 项 | 状态 | 现状 |
|---|---|---|
| `ApplyReplicatedDelta` Client 侧发射 | DONE | `DeltaSyncEmitter::EmitClientApplyReplicatedDelta`；`ctx == Client` 不再 early-return |
| Client setter 语义（scheme-2）| DONE | 见 §5.1b：setter 保持裸赋值，回调只在 `ApplyReplicatedDelta` 里触发（对齐 BigWorld `propertyEvent` 真实行为）|
| `ClientEntity.OnEnterWorld()` | DONE | `Atlas.Client.ClientEntityManager.OnEnter` 在 peer snapshot 应用完毕后调用一次 |
| `ApplyPositionUpdate` + `OnPositionUpdated` | DONE | `ClientEntity.OnPositionUpdated(Vector3)` 虚方法；volatile 通道 `kEntityPositionUpdate` 桥接 |
| C++ 宿主按 envelope kind 桥接 | DONE | `ClientCallbacks.DispatchAoIEnvelope` 按 `CellAoIEnvelopeKind` 分发到 `OnEnter` / `OnLeave` / `DispatchPositionUpdate` / `DispatchPropertyUpdate` |
| Generator DEF008 诊断 | DONE | `ATLAS_DEF008` 对 replicable scope 的 `position` 属性报错；所有 emitter 按 `IsReservedPosition` 跳过 |
| 构建产物验收 | DONE | `samples/client/obj/generated/.../StressAvatar.DeltaSync.g.cs` / `.Properties.g.cs` 符合预期 |

### 9.3 BigWorld 架构对齐（跨补强项）

记录在 §5.1a / §5.1b 的架构对齐工作：

| 工作项 | Commit | 对齐目标 |
|---|---|---|
| M1 PropertyScope 谓词化 | `ce927b6` | `DataDescription::isCellData = !isBaseData` 的单一权威判定 |
| M2 Generator 按 side 分拆 field | `46c70b9` | `data only lives on a base or a cell but not both`（`data_description.ipp:113-131`）|
| L1 `BackupCellEntity` (cell → base opaque bytes) | `3fdbd2d` | `BaseAppIntInterface::backupCellEntity` (`cellapp/real_entity.cpp:884-906`) + Base 代管不反序列化 (`baseapp/base.cpp:1182-1200`) |
| L2 DB blob 拼接 base + cell 两段 | `788330d` | BigWorld DB 持久化组合模型 |
| L4 Baseline pump 迁至 CellApp | `f2dec1e` | 有权威数据的进程发 baseline（BigWorld 没有等价 pump，Atlas 自建以支撑 unreliable 属性恢复）|
| `--drop-transport-ms` RUDP 丢包注入 | `c1f745b` | 验证 reliable retransmit 端到端（BigWorld 有同类能力由 Mercury 提供）|
| `ClientLog` `[t=S.sss]` 事件时间戳 | `aec557f` | 收敛时间机械化断言基础 |

### 9.4 验收运行（2026-04-22 sweep）

4 个场景在 [script_client_smoke.md](../stress_test/script_client_smoke.md) 里都跑通：

| 场景 | 期望 | 实测 |
|---|---|---|
| AoI 广播（50 bare + 2 script, 30s）| `hp ~800-1000 seqgaps=0` | `hp=937 seqgaps=0` |
| 应用层丢包（reliable）| `hp=31 seqgaps=8` | 通过 |
| 传输层丢包（RUDP 重传）| `hp=39 seqgaps=0` | 通过 |
| 应用层丢包（unreliable + baseline 兜底）| 脚本计数与 reliable 相近；字段层 baseline 在 ≤6 s 内收敛 | 通过 |

### 9.5 后续

- §6 远期演进（C++ 轻量 EventHistory + C# DirtyBit 混合）仍未启动，取决于 AoI 负载压测实际瓶颈
- 补强二 tail work（§7.2）可随时补，无阻塞依赖
- 补强三（LatestChangeOnly）保持冻结，等到 C++ 侧引入 per-property 事件合并时再评估
