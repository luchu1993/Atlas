# 属性同步机制设计：DirtyBit vs EventHistory

> 日期: 2026-04-15
> 关联: [BigWorld EventHistory 参考](BIGWORLD_EVENT_HISTORY_REFERENCE.md) | [Phase 10 CellApp](roadmap/phase10_cellapp.md) | [Phase 12 Client SDK](roadmap/phase12_client_sdk.md)

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

### 5.1 可靠 delta 投递

**问题：** Unreliable 投递丢包后，若属性不再变化，客户端永久停留旧值。

**方案：** 每 N 帧（建议 30 帧 ≈ 1 秒）发一次全量快照作为 baseline，客户端以最近收到的 baseline + 后续 delta 合成当前状态。

```
帧 1:  delta [HP dirty] → unreliable
帧 2:  delta [MP dirty] → unreliable
...
帧 30: baseline [全量快照] → reliable
帧 31: delta [HP dirty] → unreliable
```

替代方案：delta 加 sequence number，客户端发现跳号时 NACK 请求重传。但 baseline 方案更简单，对全量快照的序列化已有 `SerializeForOwnerClient()` / `SerializeForOtherClients()` 可复用。

### 5.2 带宽预算

**问题：** 大量实体同时变脏时，帧末 delta 总量可能超过网络承受能力。

**方案：** BaseApp 在 `on_replicated_delta_from_cell` 中加入优先级队列：

```
每帧预算: MAX_BYTES_PER_TICK (如 16KB)

for entity in priority_queue (按距离/重要性排序):
    if budget >= entity.delta_size:
        send(entity.delta)
        budget -= entity.delta_size
    else:
        break  // 延迟到下帧
```

优先级因素：距离（近的优先）、属性类型（位置/HP 优先）、上次发送间隔（越久越优先）。

### 5.3 LatestChangeOnly 标记

**问题：** HP/Position 等高频属性每帧改一次，每帧都序列化发送，浪费带宽。

**方案：** `.def` 文件中增加 `latestOnly="true"` 标记：

```xml
<hp type="INT32" scope="AllClients" latestOnly="true"/>
```

当前 setter 已天然做到"同帧内多次修改只保留最终值"（ClearDirty 在帧末调用）。`latestOnly` 标记配合带宽预算使用：超预算时，标记为 latestOnly 的属性可以安全跳过当前帧，只在下帧发送最新值。

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

## 7. 近期补强实现任务清单

### 依赖关系与实现顺序

```
补强二 (带宽预算)  ──→  补强三 (LatestOnly)     补强一 (Baseline)
   DeltaForwarder       依赖 DeltaForwarder        独立模块
   框架先行              做合并优化                 可并行开发
```

建议顺序: **补强二 → 补强三 → 补强一**

### 7.1 补强二：带宽预算

> 目标：BaseApp 转发 delta 时按每 tick 字节预算限流，超预算的 delta 延迟到下帧。

| # | 任务 | 文件 | 说明 |
|---|---|---|---|
| 2.1 | 新建 DeltaForwarder 模块 | `src/server/baseapp/delta_forwarder.hpp` (新增) `src/server/baseapp/delta_forwarder.cpp` (新增) | `PendingDelta{entity_id, delta, priority, deferred_ticks}`；`enqueue()` 入队；`flush(channel, budget)` 按优先级发送直到预算用完 |
| 2.2 | 集成到 BaseApp | `src/server/baseapp/baseapp.hpp` `src/server/baseapp/baseapp.cpp` `src/server/baseapp/CMakeLists.txt` | `on_replicated_delta_from_cell` 改为 `forwarder.enqueue()`；`on_tick_complete()` 调用 `flush()`；新增 `kDeltaBudgetPerTick = 16KB` |
| 2.3 | Watcher 监控 | `src/server/baseapp/baseapp.cpp` | `baseapp/delta_bytes_sent_total`, `baseapp/delta_bytes_deferred_total`, `baseapp/delta_queue_depth` |
| 2.4 | 单元测试 | `tests/unit/test_delta_forwarder.cpp` (新增) `tests/unit/CMakeLists.txt` | 预算内全发、超预算截断、deferred_ticks 优先级提升、空队列 flush |

### 7.2 补强三：LatestChangeOnly 标记

> 目标：`.def` 中增加 `latest_only="true"` 标记，带宽受限时可安全跳过中间帧只发最新值。

| # | 任务 | 文件 | 说明 |
|---|---|---|---|
| 3.1 | DefModel 增加字段 | `src/csharp/Atlas.Generators.Def/DefModel.cs` | `PropertyDefModel` 新增 `bool LatestOnly` |
| 3.2 | DefParser 解析 | `src/csharp/Atlas.Generators.Def/DefParser.cs` | `ParseProperty()` 读取 `latest_only` 属性 |
| 3.3 | TypeRegistryEmitter 传递到 C++ | `src/csharp/Atlas.Generators.Def/Emitters/TypeRegistryEmitter.cs` | 属性描述符追加 `latest_only` 字段 |
| 3.4 | C++ 存储标记 | `src/lib/entitydef/entity_def_registry.hpp` | `PropertyDesc` 增加 `bool latest_only{false}` |
| 3.5 | DeltaForwarder 利用标记 | `src/server/baseapp/delta_forwarder.cpp` | 多帧积压时只保留 latest_only 属性的最新值 |
| 3.6 | 更新 Avatar.def | `entity_defs/Avatar.def` | `hp` 和 `position` 加 `latest_only="true"` |
| 3.7 | 测试 | C# 解析测试 + `test_delta_forwarder.cpp` 扩展 | 验证解析正确性和合并逻辑 |

### 7.3 补强一：可靠 Baseline 快照

> 目标：每 N 帧（默认 30 帧 ≈ 1s）向客户端发一次全量属性快照（reliable），确保丢包后 1 秒内收敛。

| # | 任务 | 文件 | 说明 |
|---|---|---|---|
| 1.1 | 新增 Baseline 消息 | `src/server/baseapp/baseapp_messages.hpp` `src/lib/network/message_ids.hpp` | `ReplicatedBaselineToClient` (msg 0xF002)，字段: entity_id, scope_tag, snapshot；`Reliable` |
| 1.2 | BaseApp 周期发送 | `src/server/baseapp/baseapp.hpp` `src/server/baseapp/baseapp.cpp` | `on_tick_complete()` 中每 kBaselineInterval 帧遍历有 client 的 Proxy，调 C# 取快照发 reliable 消息 |
| 1.3 | C# scope-filtered 快照回调 | `src/csharp/Atlas.Runtime/Core/NativeCallbacks.cs` `src/server/baseapp/baseapp_native_provider.hpp` | 新增 `GetEntitySnapshot` 回调，区分 owner/other 调用 `SerializeForOwnerClient` / `SerializeForOtherClients` |
| 1.4 | 客户端处理 baseline | `src/client/client_app.cpp` + C# 客户端运行时 | 注册 0xF002 handler；DeltaSyncEmitter 生成 `ApplyBaseline(ref SpanReader)` |
| 1.5 | 测试 | `tests/unit/test_baseline_snapshot.cpp` (新增) `tests/csharp/.../BaselineTests.cs` (新增) | 消息序列化、计数逻辑、属性覆盖正确性 |

---

## 8. 实施审计与后续补强 (2026-04-18)

对 §7 任务清单的代码落地情况做了一次逐项审计。结论：**补强二基本落地，补强一/三完全未开始**，另外识别出一项设计文档未覆盖的正确性风险（Reliable/Unreliable 分流）。本节记录审计结果并给出更新后的补强步骤。

### 8.1 当前实现状态

| 任务 | 状态 | 证据 |
|---|---|---|
| **2.1** DeltaForwarder 模块 | DONE | `src/server/baseapp/delta_forwarder.h/.cc`；`PendingDelta` 含 `entity_id/delta/deferred_ticks`（无 `priority`） |
| **2.2** BaseApp 集成 + 16KB 预算 | DONE | `baseapp.cc:628-638` Enqueue；`baseapp.cc:229,640-666` Flush；`baseapp.h:236` `kDeltaBudgetPerTick` |
| **2.3** Watcher 监控 | PARTIAL | `baseapp.cc:295-301` 注册了 `delta_bytes_sent_total` 和 `delta_queue_depth`；**缺 `delta_bytes_deferred_total`** |
| **2.4** 单元测试 | DONE | `tests/unit/test_delta_forwarder.cpp` (222 行) 覆盖全部场景 |
| **3.1–3.7** LatestChangeOnly | MISSING | 全部 7 项零实现 |
| **1.1–1.5** Baseline 快照 | MISSING | 全部 5 项零实现 |

### 8.2 补强二：剩余修补任务

| # | 任务 | 文件 | 说明 |
|---|---|---|---|
| 2.5 | 补注册 deferred watcher | `src/server/baseapp/baseapp.cc`（紧邻 line 295-301 已有 watcher 注册块） | 追加 `RegisterWatcher("baseapp/delta_bytes_deferred_total", ...)` 读取 `DeltaForwarder::stats().bytes_deferred`，聚合所有 `client_delta_forwarders_` 求和 |
| 2.6 | PendingDelta 增加 priority | `delta_forwarder.h:54-58` `delta_forwarder.cc:26`（排序比较器） | 追加 `uint16_t priority{0}` 字段；`Enqueue()` 签名加 priority 参数（默认 0）；`Flush()` 排序改为 `(priority desc, deferred_ticks desc)` 字典序；同 entity 替换时**取二者较大值**而非覆盖，避免被低优先级的后续写入降权 |
| 2.7 | BaseApp 调用方传递 priority | `baseapp.cc:628-638` `OnReplicatedDeltaFromCell` | 当前 `OnReplicatedDeltaFromCell` 只收到 `{base_entity_id, delta}`。**暂传 0 作为占位**——真正的"距离/属性类型/上次发送间隔"优先级需要 Witness 上下文，只有 Phase 10 CellApp 完成后才能填入；此任务先确保框架就位 |
| 2.8 | 单元测试扩展 | `tests/unit/test_delta_forwarder.cpp` | 新增用例：(a) 高 priority 插入后先于低 priority 发送；(b) 同 entity 先低后高 priority 时合并保留高 priority；(c) priority 相等时退化为 deferred_ticks 排序 |

### 8.3 补强三：状态调整 — **降级合并**

**审计发现：** `DeltaForwarder::Enqueue` 对同一 entity 已做**整条 delta 替换**（`delta_forwarder.cc:9-20`），这覆盖了 §5.3 所声明的"跨帧积压合并"核心诉求。`latest_only` 标记现阶段的**唯一差异价值**是"区分哪些属性即使积压也必须保留中间帧"——但目前没有任何业务场景需要此区分。

**决定：** 补强三（7 个子任务 3.1–3.7）**不再作为独立任务推进**，保留到 §6 远期混合架构中重新评估。届时若 C++ 侧引入 per-property 事件粒度的合并（而非当前 entity 级替换），再考虑 `.def` 标记。

**行动：** 无需实现。本节作为决策记录归档。

### 8.4 补强四：Reliable/Unreliable per-property 分流（新增，P0）

**问题：** 当前 `ReplicatedDeltaFromCell` 硬编 `MessageReliability::kUnreliable`（`baseapp_messages.h`）。设计文档 §3.2 已承认 DirtyBit "丢包后若属性不再变化，客户端永久停留旧值"。补强一（Baseline 每秒兜底）虽能最终收敛，但**在 1 秒窗口内语义关键属性（HP、状态位、背包变更）可能完全丢失**——玩家可能看到"残血单位变满血"的错觉。

**方案：** 在 `.def` 中加 `reliable="true"` 标记，生成器对可靠属性走独立的 reliable 消息通道，Unreliable 继续承载位置/朝向等高频低重要性属性。与补强一 Baseline 互为双保险（reliable delta 保证语义事件不丢；baseline 保证状态收敛）。

| # | 任务 | 文件 | 说明 |
|---|---|---|---|
| 4.1 | DefModel 增加字段 | `src/csharp/Atlas.Generators.Def/DefModel.cs:37-43` `PropertyDefModel` | 新增 `bool Reliable` |
| 4.2 | DefParser 解析 | `src/csharp/Atlas.Generators.Def/DefParser.cs:102-112` `ParseProperty()` | 读取 `reliable` 属性（默认 false） |
| 4.3 | TypeRegistryEmitter 传递到 C++ | `src/csharp/Atlas.Generators.Def/Emitters/TypeRegistryEmitter.cs:79-88` | 属性描述符追加 `reliable` 字段 |
| 4.4 | C++ 存储标记 | `src/lib/entitydef/entity_type_descriptor.h:47-55` `PropertyDescriptor` | 新增 `bool reliable{false}` |
| 4.5 | 生成器分流序列化 | `src/csharp/Atlas.Generators.Def/Emitters/DeltaSyncEmitter.cs` | 为 reliable 属性与 unreliable 属性生成两份 `_dirtyFlags`（或拆分 mask 位段）；帧末产生两条 delta blob |
| 4.6 | 新增 reliable 消息 ID | `src/lib/network/message_ids.h:82-102` `src/server/baseapp/baseapp_messages.h` | `ReplicatedReliableDeltaFromCell` — **实际分配 msg 2017**（2016 已被 `kBroadcastRpcFromCell` 占用）；`MessageReliability::kReliable`；客户端侧 `DeltaForwarder::kClientReliableDeltaMessageId = 0xF003` |
| 4.7 | BaseApp 双路径 | `baseapp.cc` | `OnReplicatedReliableDeltaFromCell` 直接转发（不入 DeltaForwarder，因为预算用于丢弃场景，reliable 不能丢弃）；Unreliable 路径保持现状 |
| 4.8 | 客户端 handler | client C# runtime | 注册新消息，复用同一 `ApplyReplicatedDelta` 入口（delta blob 格式相同，只是传输通道不同） |
| 4.9 | Avatar.def 标注 | `entity_defs/Avatar.def` | `hp` / 状态类字段加 `reliable="true"`；`position` 保持默认 unreliable |
| 4.10 | 测试 | `tests/unit/test_baseapp_reliable_delta.cpp`（新增）+ C# 生成器测试 | 验证分流正确、reliable 不走预算限流 |

### 8.5 补强一：保持原方案，按 §7.3 执行

补强一任务（1.1–1.5）方案不变，按§7.3 清单原样推进。补强四落地后，补强一仍需保留——两者关注点不同：

- **补强四**：保证"**变化事件**"不丢（语义）
- **补强一**：保证"**当前状态**"最终收敛（快照）

补强一 Baseline 间隔可考虑从 30 帧延长（例如 120 帧 ≈ 4s），因为有补强四兜底后，baseline 只需防御极端场景（客户端错过了 reliable 重传）。

### 8.6 更新后的优先级与实施顺序

```
P0: 补强四 (Reliable 分流)   →  补强一 (Baseline 快照)
    独立模块可并行               依赖补强四稳定后评估间隔

P1: 补强二补齐 (2.5–2.8)        从任意时机插入
    小改动，低风险

P2: 补强三                      冻结，归入 §6 远期
```

**实施顺序建议：**

1. **第一阶段（P0 正确性修复）**：补强四 → 补强一。先做补强四是因为它从根源上减少了补强一需要覆盖的场景；Baseline 间隔的最终确定依赖补强四落地后的实测丢包影响。
2. **第二阶段（P1 观测与调度完善）**：补强二剩余四项（2.5–2.8）。Watcher 是小时级任务；priority 字段框架就位后，等 CellApp Witness 能提供上下文时再填真实优先级。
3. **第三阶段（远期）**：§6 混合架构 + 补强三重新评估，与 Phase 10 CellApp Step 10.5b 一并设计。

### 8.7 验收门槛

每项补强完成时须满足：

| 项 | 验收点 |
|---|---|
| 补强四 | `entity_defs/Avatar.def` 中 `hp` 标注 reliable 后，断网 30 帧再恢复，客户端 HP 与服务端一致（当前会漂移）|
| 补强一 | 客户端错过所有 reliable 消息后，最多 `kBaselineInterval` 帧内状态收敛到服务端 |
| 补强二 (2.5–2.8) | `baseapp/delta_bytes_deferred_total` 在 watcher dump 中可见；`test_delta_forwarder.cpp` 新增 3 个 priority 相关用例全部通过 |

---

## 9. 客户端接收侧的对称设计（2026-04-21，BigWorld 源码审计补齐）

§2–§8 聚焦服务端产出侧。对照 BigWorld 源码（`common/simple_client_entity.cpp`、`client/entity.cpp`、`lib/connection_model/bw_entity.cpp`）+ Atlas 客户端实测产物（`samples/client/obj/generated/Atlas.Generators.Def/Atlas.Generators.Def.DefGenerator/Avatar.*.g.cs`），发现**客户端消费侧存在结构性缺口**，且 §7.3 任务 1.4、§8.4 任务 4.8 中"客户端侧复用 `ApplyReplicatedDelta` 入口"的假设在当前代码里并不成立——Generator 在 Client 模式下根本没发射这个函数。本节把缺口与对齐方案落纸。

### 9.1 BigWorld 的双通道接收模型

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
2. **初始快照 vs 增量由 `shouldUseCallback` 区分** —— enter-AoI 批量下发属性时 `false`（不调回调，避免脚本在 `onEnterWorld` 之前被乱序通知），后续 delta `true`

**位置通道**（独立消息 `avatarUpdateNoAliasDetailed` / `avatarUpdatePlayerDetailed` / `forcedPosition`，`lib/connection/common_client_interface.hpp:122-152`）：

```
ServerConnection::avatarUpdateXxx()
  → BWEntities::handleEntityMoveWithError()
  → BWEntity::onMoveFromServer()            (bw_entity.cpp:562-588)
  → movement filter (插值 / 外推 / 时间戳追赶)
  → setSpaceVehiclePositionAndDirection()
  → onPositionUpdated()                      (bw_entity.cpp:624-653, 纯 C++ 钩子)
```

位置**不走属性系统**、**不触发 Python `set_position`**，只触发 C++ `onPositionUpdated()`。脚本要观察移动走独立入口（`Entity.onMove()` 等）。设计动机：位置高频、需要插值，混入属性回调流会吞 CPU 且语义错位。

### 9.2 Atlas 当前客户端接收现状（实测）

| 环节 | BigWorld | Atlas 实测 | 证据 |
|---|---|---|---|
| 属性 delta 解码 | `propertyEvent(propertyID, ...)` 按 ID 分发 | **无**：Generator 在 Client 模式直接跳过 | `src/csharp/Atlas.Generators.Def/Emitters/DeltaSyncEmitter.cs:19-20`：`if (ctx == ProcessContext.Client) return null;` |
| setter 自动回调 | 自动调 `set_<propname>(oldValue)` | **不调**：Client 分支 setter 仅 `_field = value` | `PropertiesEmitter.cs:111-140` 的 `EmitProperty`：`withDirtyTracking=true` 分支（server）触发 `OnXxxChanged`；`else` 分支（client，`ctx != Client` 才算 replicable）只生成裸赋值 |
| 初始快照 vs 增量 | `shouldUseCallback` 布尔参数 | **无区分**：只有 `Deserialize()`（enter-AoI 全量），且直写 `_field = reader.ReadXxx()` 绕过 setter | `samples/client/obj/generated/.../Avatar.Serialization.g.cs` |
| 位置高频同步 | 独立消息 + filter + `onPositionUpdated()` | 位置与属性共走 `kReliableDeltaWireId=0xF003` AoI 信封，首字节 `CellAoIEnvelopeKind` 区分；客户端无 filter、无独立位置钩子 | `src/tools/world_stress/main.cc:447-497` 当前只对信封头字节计数 |

`§7.3` 任务 1.4 与 `§8.4` 任务 4.8 均假设客户端"复用同一 `ApplyReplicatedDelta` 入口"，该入口当前**未生成**，两任务的客户端侧落地需先补齐此前置依赖。

### 9.3 对齐目标

**P0 — 属性 delta 对称解码**

扩 `DeltaSyncEmitter` 在 `ctx == ProcessContext.Client` 时生成对称的 `ApplyReplicatedDelta(ref SpanReader reader)`：按服务端写出的 `(flags byte[s])+(dirty values)` 位图格式读，逐字段：

```csharp
public void ApplyReplicatedDelta(ref SpanReader reader)
{
    var flags = (ReplicatedDirtyFlags)reader.ReadByte();
    if ((flags & ReplicatedDirtyFlags.Hp) != 0)
    {
        var old = _hp;
        _hp = reader.ReadInt32();
        OnHpChanged(old, _hp);
    }
    // ...
}
```

注意：客户端不维护 `_dirtyFlags` 字段（没有写端诉求），只把 flags 当一次性 bitmap 用于解码分发。

**P0 — 客户端 setter 触发回调**

修 `PropertiesEmitter.cs:121-137`：让 `ctx == ProcessContext.Client` 的 setter 也走"`if (old != new)` → 写字段 → 调 `OnXxxChanged(old, new)`"的形式，**但不设置任何 dirty bit**（客户端没有脏标记）。这样无论是 wire delta 写入还是脚本本地赋值都会触发回调，与 BigWorld `set_<propname>(old)` 语义对齐。

**P0 — 初始快照 vs 增量分流**

- `Deserialize()`（enter-AoI 全量快照）**维持直写字段**、**不触发回调**，对齐 BigWorld `shouldUseCallback=false` 语义
- 新增 `ClientEntity.OnEnterWorld()` 虚方法，框架在快照应用完毕后统一调用一次，供脚本做相对"当前状态"的整体初始化
- `ApplyReplicatedDelta()`（增量）**触发每个变化属性的 `OnXxxChanged`**，对齐 `shouldUseCallback=true`

**P0 — Position 走独立通道**

Position 在 `.def` 仍是 `scope="all_clients"` 属性（与 BigWorld `Volatile` 标志呼应），但客户端接收端**不与普通属性同路**：

- `0xF003` 信封 `kind=kEntityPositionUpdate`（volatile 通道）桥接到 `ClientEntity.ApplyPositionUpdate(Vector3 newPos)` 专用钩子
- `ApplyPositionUpdate` 直写 `_position` 字段（不经属性 setter）、调 `OnPositionUpdated(Vector3 newPos)` ——**不是** `OnPositionChanged(old, new)`，避免混入属性回调链
- `kind=kEntityPropertyUpdate`（event 通道）中若再带 Position 位：要么服务端保证 Position 永不进入 event 通道（与 §6.1 volatile/event 分流一致），要么客户端 `ApplyReplicatedDelta` 在位图中跳过 Position（Generator 层面校验）

**P2 — 客户端 movement filter（远期）**

BigWorld filter（时间戳追赶 + 外推）是完整客户端必需，stress 场景不需要。留给 Unity 客户端集成时再设计，现阶段 `OnPositionUpdated` 直接落到 `_position` 即可。

### 9.4 与 §6 远期架构的关系

§6 规划的 DirtyBit（C#）+ EventHistory（C++ 轻量）混合架构只改**服务端产出侧**。本节补齐**客户端消费侧**，两者**正交**：

- **服务端**：DirtyBit → HistoryEvent → per-observer cursor → 写 wire delta
- **客户端**：wire delta → `ApplyReplicatedDelta` 按 bitmap 分发 → setter + 回调

不论服务端是否迁 EventHistory，客户端 wire 格式（`kind byte` + `flags bitmap` + `values`）可保持不变，本节的客户端侧实现复用。

### 9.5 实施任务清单

| # | 任务 | 文件 | 依赖 |
|---|---|---|---|
| 9.1 | `DeltaSyncEmitter` 增加 Client 对称发射 `ApplyReplicatedDelta` | `src/csharp/Atlas.Generators.Def/Emitters/DeltaSyncEmitter.cs:19-20` | 无 |
| 9.2 | `PropertiesEmitter` Client 分支 setter 触发 `OnXxxChanged`（不设 dirty bit） | `src/csharp/Atlas.Generators.Def/Emitters/PropertiesEmitter.cs:111-140` | 无 |
| 9.3 | `ClientEntity` 基类增 `OnEnterWorld()` 虚方法 | `src/csharp/Atlas.Client/ClientEntity.cs` | 无 |
| 9.4 | `ClientEntity` 基类增 `ApplyPositionUpdate(Vector3)` + `OnPositionUpdated(Vector3)` 钩子 | `src/csharp/Atlas.Client/ClientEntity.cs` | 无 |
| 9.5 | C++ 客户端宿主把 `0xF003` 信封按 kind 桥到 `ClientCallbacks.DispatchRpc` / `ApplyReplicatedDelta` / `ApplyPositionUpdate` | `src/client/client_native_provider.cc`，`src/csharp/Atlas.Client/ClientCallbacks.cs` | 9.1/9.4 |
| 9.6 | Generator 诊断：Position 不得出现在 `ApplyReplicatedDelta` 位图中（或自动跳过） | `src/csharp/Atlas.Generators.Def/DefDiagnosticDescriptors.cs` | 9.1 |
| 9.7 | 验收：`samples/client/` 构建后产物多出 `Avatar.DeltaSync.g.cs`（只含 `ApplyReplicatedDelta`）；`Avatar.Properties.g.cs` setter 调用 `OnXxxChanged` | 构建验证 | 9.1/9.2 |

任务 9.1–9.4 是 §7.3 / §8.4 中所有"客户端侧"落地的**前置基础**，优先做完再推补强一 / 补强四的客户端部分。
