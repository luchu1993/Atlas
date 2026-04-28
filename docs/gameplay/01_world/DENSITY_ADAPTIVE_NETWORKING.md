# 密度自适应网络（Density-Adaptive Networking）

> **用途**：定义 Atlas 客户端实体订阅与广播策略——按距离分层订阅、按密度动态调频、按战斗状态优先级覆盖。承载 5000 并发玩家与 100 实体热点的关键。
>
> **读者**：工程（必读）、网络工程师（必读）、客户端开发（§7 必读）、运维（§8 必读）。
>
> **状态**：草案 v0.1 — 待团队评审。
>
> **前置文档**：`OVERVIEW.md`、`CELL_ARCHITECTURE.md`、`GHOST_ENTITY.md`、`MOVEMENT_SYNC.md`
>
> **关联文档**：`ENTITY_SNAPSHOT.md`（快照格式细节，待写）、`COMBAT_EVENT_ORDERING.md`（事件通道独立）

---

## 1. 设计目标与边界

### 1.1 问题

参见 `OVERVIEW.md §7` 难点二。简化版：

5000 玩家 / 服务器，热点区域 100 实体。如果对每个客户端**全订阅 + 30 Hz 广播**：
- 单客户端订阅 100 实体 × 30 Hz × 12 字节 = **36 KB/s 下行**
- 5000 玩家 × 36 KB/s = 180 MB/s 服务端总下行
- 单 CellApp 内 100 玩家互相广播 = N² 复杂度

无法承载。**必须分层降频**。

### 1.2 物理事实

玩家**视野内能感知的实体数有限**（认知经济学）：
- 战斗激烈关注 ~5 个目标
- 视觉余光感知 ~15 个
- 远景看到"有人在那" ~30 个
- 更远只是"背景"

**协议设计应该承认这个事实**——近身高精度，远景仅"存在感"。

### 1.3 核心目标

1. **按距离分层订阅**：近身 30 Hz，远景 1 Hz
2. **按密度动态调整**：100 实体热点单实体频率自动降档
3. **战斗事件最高优先**：技能释放/命中走可靠通道，不依赖位置快照
4. **客户端 LOD**：远距离实体视觉简化（比例还原 ≤ 5%）
5. **5000 并发可承载**：单服务器集群带宽 < 100 Mbps

### 1.4 非目标

- **不追求所有实体精确同步**：远景实体 1 m 偏差视觉无差异
- **不实时更新非战斗实体**（NPC 闲逛、装饰物）：极低频
- **不为每个客户端定制订阅**（开销大）：按距离分层批处理

---

## 2. 距离分层订阅

### 2.1 五层模型

每个客户端的视角，订阅的实体按距离分 5 层：

| 层 | 距离范围 | 更新频率 | 数据精度 |
|---|---|---|---|
| **L0 战斗圈** | < 5 m | 30 Hz | 完整：pos + vel + yaw + state + animation |
| **L1 战术感知** | 5 – 15 m | 20 Hz | 完整：pos + vel + yaw + state |
| **L2 视野中** | 15 – 40 m | 10 Hz | pos + yaw + state（无 vel） |
| **L3 视野边缘** | 40 – 80 m | 5 Hz | pos + state（无 yaw 精度） |
| **L4 远景存在感** | 80 – 200 m | 1 Hz | 仅 pos（粗略） |
| **超出 200 m** | — | — | 不订阅 |

### 2.2 副本/Arena 例外

PvP arena（最大 64 m 范围）和小副本：
- 全部订阅 = L0 / L1 范围内
- 30 Hz 全量数据（副本 tick 频率即 30 Hz）
- 不需要分层（实体数 < 16）

### 2.3 重要实体覆盖

某些实体**无视距离强制升级**到更高频订阅：
- 玩家正在攻击的目标（武器锁定）
- 正在攻击玩家的敌人
- 玩家队友（PvP / PvE 配合）
- Boss / 关键 NPC（剧情、机制）

```cpp
SubscriptionTier DecideTier(Entity self, Entity other) {
  float dist = Distance(self, other);
  SubscriptionTier dist_tier = TierByDistance(dist);
  
  // 重要性覆盖
  if (self.target_locked == other) return SubscriptionTier.L0;
  if (other.target_locked == self) return SubscriptionTier.L0;
  if (self.party_id == other.party_id && dist < 200) return Max(dist_tier, L2);
  if (other.IsBoss && dist < 200) return Max(dist_tier, L2);
  
  return dist_tier;
}
```

---

## 3. 按层数据精度

### 3.1 L0 战斗圈（< 5 m, 30 Hz）

完整数据：
```cpp
struct EntitySnapshotL0 {
  uint16_t entity_alias;         // AoI 内别名
  PackedPos position;            // 6 字节（cm 精度 delta）
  PackedVel velocity;            // 4 字节
  uint16_t yaw;                  // 全精度
  int8_t pitch;                  // 仅必要时
  uint8_t state_flags;           // grounded / falling / casting / stunned
  uint16_t animator_param_hash;  // 当前动画状态简易标识
};
```

每实体 ~16 字节。

### 3.2 L1 战术感知（5–15 m, 20 Hz）

类似 L0 但**更宽松量化**：
```cpp
struct EntitySnapshotL1 {
  uint16_t entity_alias;
  PackedPos position;            // 5 字节（dm 精度 delta）
  PackedVel velocity;
  uint16_t yaw;
  uint8_t state_flags;
  // 无 animator_param_hash
};
```

每实体 ~12 字节。

### 3.3 L2 视野中（15–40 m, 10 Hz）

```cpp
struct EntitySnapshotL2 {
  uint16_t entity_alias;
  PackedPos position;            // 4 字节
  uint16_t yaw;
  uint8_t state_flags;
  // 无 velocity
};
```

每实体 ~9 字节。

### 3.4 L3 视野边缘（40–80 m, 5 Hz）

```cpp
struct EntitySnapshotL3 {
  uint16_t entity_alias;
  PackedPos position;            // 3 字节（粗 delta）
  uint8_t direction_octant;      // 8 方向中的一个，1 字节
  uint8_t state_flags;
};
```

每实体 ~7 字节。

### 3.5 L4 远景存在感（80–200 m, 1 Hz）

```cpp
struct EntitySnapshotL4 {
  uint16_t entity_alias;
  PackedPos position;            // 3 字节
};
```

每实体 ~5 字节。

### 3.6 客户端 fallback

接收 L2/L3/L4 等低频数据后，客户端按"数据缺失字段"采用默认：
- 无 velocity → 速度 0（停下）
- 无 yaw → 上次 yaw（或默认）
- 无 animator_param → Idle state

视觉上"远处的人感觉静止"，但 1 Hz 推送会及时更新——玩家走近时切换到高层级，数据自动丰富。

---

## 4. 带宽预算

### 4.1 单客户端订阅典型分布

战斗圈玩家（人多场合）订阅：
- L0: 5 实体 × 30 Hz × 16 字节 = 2.4 KB/s
- L1: 10 实体 × 20 Hz × 12 字节 = 2.4 KB/s
- L2: 25 实体 × 10 Hz × 9 字节  = 2.25 KB/s
- L3: 30 实体 × 5 Hz × 7 字节   = 1.05 KB/s
- L4: 40 实体 × 1 Hz × 5 字节    = 0.2 KB/s

**单客户端下行总和 ~ 8.3 KB/s**（仅位置快照部分；战斗事件另算）。

加上事件通道（~3 KB/s）+ 协议头开销（~2 KB/s）：单客户端下行 ~13 KB/s。

### 4.2 服务器整体

5000 玩家 × 13 KB/s = **65 MB/s**（服务器集群下行总和）。

按 5 台物理机计算，每台 13 MB/s = **104 Mbps**——单网卡 1 Gbps 完全可承载。

### 4.3 上行（玩家输入）

InputFrame 60 Hz × 18 字节 × 3 帧冗余 = 3.24 KB/s。

5000 玩家 × 3.24 KB/s = 16 MB/s 服务器总上行。轻松。

### 4.4 极端热点（200v200 PvP）

我们**不做大规模 PvP**（参见 OVERVIEW §2.3），所以无此场景。但 PvE 大型活动可能：
- 100 玩家集中（如世界 boss）
- 各自订阅周围 ~80 实体（含其他玩家 + boss + adds）
- 单客户端下行升至 ~25 KB/s（高但可承受）
- 服务器扩频不够时降级（30→20 Hz）

---

## 5. 优先级快照构造

### 5.1 流程

每 tick 服务端为每个客户端构造个性化快照：

```
对每个 Witness（=玩家客户端的服务端代理）：
  ↓
1. 计算各 ghost / 视野实体到 self 的距离
  ↓
2. 按 §2 规则分配 SubscriptionTier
  ↓
3. 检查每个实体是否到本 tick 的"应同步时刻"
   （L0 每 tick 都同步，L4 每 30 tick 同步一次）
  ↓
4. 收集所有"该同步"的实体，组合为本 tick 快照
  ↓
5. 按层级精度序列化
  ↓
6. 加协议头 + UDP 发送
```

### 5.2 Witness 内部状态

```cpp
struct WitnessEntityState {
  uint64_t entity_id;
  SubscriptionTier current_tier;
  uint32_t last_synced_tick;
  uint32_t alias;                // 客户端用别名
};

class Witness {
  EntityRef owner;                          // 玩家自己
  Dictionary<uint64_t, WitnessEntityState> tracked;
  AliasAllocator alias_pool;                // 别名分配
};
```

### 5.3 别名分配

每个 Witness 维护 entity_id → 16-bit alias 映射：
- 实体进入订阅 → 分配新 alias
- 实体退出订阅 → 回收 alias
- 协议中只发 alias，省 6 字节/实体

### 5.4 同步频率实现

```csharp
bool ShouldSyncThisTick(WitnessEntityState s, long currentTick) {
  int interval_ticks = TierToInterval(s.current_tier);
  return (currentTick - s.last_synced_tick) >= interval_ticks;
}

int TierToInterval(SubscriptionTier tier) {
  return tier switch {
    L0 => 1,    // 30 Hz @ 30 Hz tick
    L1 => 2,    // 15 Hz
    L2 => 3,    // 10 Hz
    L3 => 6,    // 5 Hz
    L4 => 30,   // 1 Hz
  };
}
```

注：开放世界 20 Hz tick 下时间间隔翻倍（L0 仍 1 tick = 50ms）。

### 5.5 快照大小目标

单 tick 单客户端快照：
- 平均：~1 KB（随密度变化）
- 峰值：~3 KB（密集战斗）
- UDP MTU：~1400 字节单包

超过 MTU 拆多包，标记 "split N of M"，客户端组装。

### 5.6 自适应延迟

如果服务端 CPU 跟不上：
- 优先保 L0 / L1（核心战斗体验）
- L4 可跳过 N tick（玩家几乎不感知）

监控指标 `snapshot_construction_lag` 触发降级。

---

## 6. 战斗事件独立通道

### 6.1 为什么独立

位置快照通道（Channel 1，UDP unreliable）：
- 频繁但可丢
- 丢一次下次更新即恢复

战斗事件（技能释放、命中、死亡、buff 变化）：
- **必达，丢失即破坏**：玩家漏看一次伤害事件 = "为什么我血空了？"
- **有时序要求**：先释放后命中
- **不能用快照状态变化推断**：状态变化的"瞬间"才是事件

### 6.2 通道分配

参见 `OVERVIEW.md §6.2`：

| 通道 | 内容 | 可靠性 |
|---|---|---|
| **Channel 1: 快照** | 位置 / 状态 / 动画参数 | UDP unreliable |
| **Channel 2: 战斗事件** | 技能、命中、buff、死亡 | UDP reliable |
| **Channel 3: 持久操作** | 背包、交易 | TCP / 可靠 |

### 6.3 战斗事件不分层

战斗事件**不按距离降频**——所有相关玩家必须收到。
- 可见范围内（200m 内 = L4 距离上限）的所有事件都发
- 超出范围不发（玩家看不到，也不需要事件）

事件量 vs 快照量：典型战斗每秒事件 < 10 个/玩家，远低于快照频率。

### 6.4 事件优先级

特殊事件即使距离远也必发（如范围内 boss 死亡 → 整图玩家通知）：
- 通过特殊 broadcast 通道（不走 Witness）
- 用于全图通告 / 系统消息

---

## 7. 客户端 LOD

### 7.1 视觉简化

客户端按订阅层级简化远端实体渲染：

| 层 | 渲染策略 |
|---|---|
| L0 / L1 | 完整：full mesh + animator + VFX + dynamic lighting |
| L2 | 简化：full mesh + animator（基础 state）+ 减 VFX |
| L3 | 极简：低模 + 静态摆姿势（不播 animator）+ 无 VFX |
| L4 | Billboard / Imposter（2D 贴图） |

### 7.2 切换平滑

实体从 L3 升级到 L2（玩家走近）时：
- 切换 mesh + 启用 animator
- 视觉上一帧切换可能突兀
- 解决：渐进式 fade in（200ms 过渡）

降级反向：fade out 简化版本。

### 7.3 动画 LOD

L1+ 实体播放完整 Animator state machine（所有 layer / IK）。
L2 简化：仅主 layer，无 IK。
L3 完全不播 Animator（静态姿势）。

### 7.4 VFX LOD

L2+ 完整 VFX。
L3 距离 ≥ 40 m，只播主特效，去除尾迹/光晕。
L4 通常无 VFX（除非全图特殊事件）。

### 7.5 Audio LOD

3D 空间音效衰减按距离自然处理。L4 距离 ≥ 80 m，多数音效已可忽略。

详见 `COMBAT_FEEL.md §9`。

---

## 8. 热点处理

### 8.1 场景

100 实体集中在一 cell（练级点 / 世界 boss）：
- 单客户端订阅 ~80 实体（实际可见的）
- 服务端构造 80 实体快照
- 单 tick 快照大小 ~1.5 KB
- 30 Hz 推送 = 45 KB/s 单客户端
- 100 玩家 × 45 KB/s = 4.5 MB/s 单 CellApp

仍可承载，但接近极限。

### 8.2 自动降级策略

若某客户端 CPU/带宽接近上限：

```cpp
void AdaptForCongestion(Witness w) {
  if (w.OutboundBandwidth > THRESHOLD) {
    // 降级 L1 频率：20 → 15 Hz
    // 提高 L2 距离阈值：15m → 25m（更多实体进 L2 而非 L1）
    // L4 频率：1 Hz → 0.5 Hz
  }
}
```

逐步降级避免过度反应。

### 8.3 地理分布的好处

5000 玩家不可能全聚一处。世界设计应：
- 多副本入口分散
- 多练级点
- 多任务地点
- 公会基地分布

避免出现"所有玩家都在主城广场"的单点拥堵。

### 8.4 单 cell 不能容纳时

100 实体已是单 cell 上限。超出时：
- CellAppMgr 触发 cell 迁移到独占 CellApp（参见 `CELL_ARCHITECTURE.md §4`）
- 从单 CellApp 多 cell 切到独占模式
- 释放 CPU 给本 cell 仿真

---

## 9. 监控与运维

### 9.1 关键指标

每个 CellApp 上报：
- 单 Witness 平均订阅实体数
- 各层订阅数分布（L0 / L1 / ... 占比）
- 单 tick 快照构造耗时（p50/p95/p99）
- 单客户端下行带宽分布
- 自动降级触发次数

每客户端（采样上报）：
- 接收快照延迟
- 丢包率
- 客户端 fps
- LOD 切换频率

### 9.2 告警

- 单 CellApp 总下行 > 80 Mbps → 告警
- 单 Witness 订阅 > 100 实体 → 告警（异常密度）
- 自动降级在 5 分钟内触发 > 3 次 → 紧急告警
- 客户端丢包率 > 5% → 跟踪是否网络问题

### 9.3 调优杠杆

运维可调参数：
- 各层距离阈值（5m / 15m / 40m / 80m）
- 各层频率（30 / 20 / 10 / 5 / 1 Hz）
- 自动降级阈值
- 别名池大小（默认 65535 = uint16）

不停服 hot reload 配置（不影响进行中战斗）。

---

## 10. FAQ 与反模式

### Q1: 为什么用距离分层而不是优先级队列？

距离分层简单且符合直觉：
- 距离是确定的物理量
- 分层规则清晰（玩家可理解）
- 实现复杂度低

优先级队列虽然灵活但：
- 每 tick 重排序开销
- 难以预测客户端体验

实践证明分层 + 重要性 override 已足够。

### Q2: 32 m 与 40 m 的实体差很大吗？为什么用 40 m 边界？

经验值：
- 玩家视野约 60 m（FOV 60° 角度）
- 40 m 是"清晰可辨" 的距离上限
- 40 m → 80 m 是"可见但模糊"
- > 80 m 是"远景"

边界选 40 m / 80 m 与玩家感知拐点对齐。可微调（35 / 75 等），但相差 5 m 玩家体感差异不大。

### Q3: 单玩家订阅 100 实体时 RAM 占用？

每订阅项 ~32 字节（state + alias + tier）。
100 项 × 32 = 3.2 KB / 玩家。
5000 玩家 × 3.2 KB = 16 MB / 服务器集群。

可忽略。

### Q4: L4 1 Hz 远景实体跳变明显吗？

实测：1 Hz 推送 + Hermite 插值 + 客户端外推（参见 `MOVEMENT_SYNC.md §6`）后，远景 80m+ 实体视觉上"小幅平滑漂移"——不影响游戏体验。

如果 L4 实体突然"瞬移"几米，玩家可能注意但不影响游戏（因为它远，不影响战斗）。

### Q5: 切层时插值会出问题吗（如 L3 → L0）？

会需要一次"插值算法切换"。处理：
- 客户端检测层级变化
- 升级层（更详细数据）：直接用新数据，平滑过渡 200ms
- 降级层：保持当前插值状态，按低频数据继续

视觉上几乎无感。

### Q6: PvP arena 不分层，5000 并发会走分层吗？

PvP arena ≤ 8 玩家，**单 instance 内全订阅**。但服务器跑多个 arena：
- 每个 arena 是独立 CellApp
- arena 间互不订阅
- 每个 arena 5000 中的几个玩家，单 arena 数据量与孤立小副本一样

5000 并发主要在开放世界，分层在那里发挥作用。

### Q7: 客户端能选择"高画质，订阅更多"吗？

不能——服务端控制订阅，避免：
- 客户端发"我要更多" → 带宽攻击
- 不同客户端订阅不一致 → 难以统计
- 复杂度增加

服务端权威决定订阅，玩家只能调"渲染层"（LOD），不能调"订阅层"。

### Q8: 重要性 override 优先级冲突怎么处理？

按 §2.3 的规则按层取最高（`Max`）：
- 队友 + 远 + Boss → 取 max(L2, L4) = L2
- 锁定目标 + 远 → 取 L0

简单 max 操作，不需复杂调度。

### Q9: 自动降级会影响 PvP 公平吗？

PvP arena 不会触发降级（小规模、稳定）。开放世界 PvP 也是小规模 PK，对带宽影响低。

降级仅影响 PvE 拥挤场景，且降的是 L4 远景（不影响战斗判定）。

### Q10: 5000 并发是上限吗？

是当前架构目标。理论上限可更高（CellApp 横向扩展），但：
- 需要更多物理机
- 需要更复杂运维
- 经济性 / 体验权衡

OVERVIEW 设定 5000，实际能力可能 8000–10000。超出后需要分服。

---

### 反模式清单

- ❌ 让客户端订阅所有可见实体（无视距离）
- ❌ 所有实体一律 30 Hz（无分层）
- ❌ 战斗事件混进位置快照通道（丢包 = 玩家黑屏）
- ❌ 客户端选择订阅范围（应服务端权威）
- ❌ 重要性 override 不限制（导致退化为全订阅）
- ❌ 别名池耗尽不处理（应回收 / 重用）
- ❌ 跨 cell 客户端订阅未通过 Ghost（数据不存在）
- ❌ LOD 切换无过渡（视觉突兀）

---

## 11. 里程碑

| 阶段 | 交付 |
|---|---|
| P1 末 | 单层全订阅原型；30Hz 数据通道 |
| P2 中 | L0 / L1 / L2 三层；按距离分层订阅 |
| P2 末 | 完整 5 层订阅；别名分配 |
| P3 早 | 客户端 LOD（mesh / VFX / animator） |
| P3 中 | 优先级 override（队友、boss、目标锁定） |
| **P4** | **热点 100 实体压测；自动降级；5000 并发实测** |
| P4+ | 监控 dashboard；动态调优 |

---

## 12. 文档维护

- **Owner**：Tech Lead + Network Engineer + Client Engineer
- **关联文档**：
  - `OVERVIEW.md`（§7 难点二引用本文）
  - `CELL_ARCHITECTURE.md`（cell 内 Witness 服务）
  - `GHOST_ENTITY.md`（Ghost AoI 上游）
  - `MOVEMENT_SYNC.md §6`（远端实体插值消费快照）
  - `COMBAT_EVENT_ORDERING.md`（事件通道独立性）
  - `ENTITY_SNAPSHOT.md`（快照格式细节，待写）
  - `09_tools/LOAD_TESTING.md`（5000 并发压测，待写）

---

**文档结束。**

**核心纪律重申**：
1. **距离分层 + 重要性 override**：兼顾性能与体验
2. **战斗事件独立通道**：必达不丢，与位置快照分开
3. **远景实体仅"存在感"**：不追求精确同步
4. **客户端 LOD 配套**：网络节流 + 渲染节流双管齐下
5. **5000 并发是工程目标**：P4 必须实测验证
