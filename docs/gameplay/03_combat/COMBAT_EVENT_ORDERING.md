# 战斗事件排序（Combat Event Ordering）

> **用途**：定义 Atlas 中战斗事件的全局排序规则、tick 内多事件处理流程、客户端事件缓冲与重排机制。这是端同一致性和"互相必杀谁先死"等公平性问题的根基。
>
> **读者**：工程（必读）、网络工程师（§4、§7 必读）、战斗策划（§3、§9 了解）。
>
> **状态**：草案 v0.1 — 待团队评审。
>
> **前置文档**：`OVERVIEW.md`、`DETERMINISM_CONTRACT.md`、`SKILL_SYSTEM.md`、`BUFF_SYSTEM.md`、`HIT_VALIDATION.md`、`STAT_AND_DAMAGE.md`
>
> **关联文档**：`MOVEMENT_SYNC.md`（移动事件）、`COMBAT_FEEL.md`（视觉事件触发）

---

## 1. 设计目标与边界

### 1.1 问题

多个文档反复提到"事件按 server tick 排序"，但没有正式化。这导致：
- 跨子系统的事件顺序不明确（damage 先还是 buff apply 先？）
- 客户端收到乱序事件如何重排不统一
- 同 tick 多个伤害事件谁先生效不一致
- "互相必杀"边界场景行为模糊

本文集中定义这些规则。

### 1.2 目标

1. **唯一时间真相**：所有客户端最终呈现的事件顺序与服务端权威顺序一致
2. **可重现**：相同输入 → 相同事件序列（满足 `DETERMINISM_CONTRACT`）
3. **同 tick 内确定性**：tick 内多事件按数据值排序，不依赖运行时随机因素
4. **级联在 tick 内完成**：不跨 tick 拖延，避免视觉撕裂
5. **客户端可重排**：网络抖动导致的乱序到达可在缓冲后正确呈现

### 1.3 非目标

- **不追求子毫秒精度**：tick 30Hz / 20Hz 已足够，亚 tick 用 μs 偏移
- **不做事件回滚**：客户端按 tick 缓冲后顺序播放，不像 rollback netcode 实时倒带
- **不允许跨 tick 级联**：同 tick 触发的级联必须当 tick 完成

---

## 2. 时间基准

### 2.1 三级时间精度

```
┌────────────────────────────────────────────────┐
│ Tier 1: server_tick (int64)                   │
│   主时间轴，单调递增；副本 30Hz / 开放世界 20Hz │
│                                                │
│ Tier 2: intra_tick_us (uint16)                │
│   亚 tick 偏移；范围 0 .. tick_duration_us-1   │
│   用于格挡窗口、连击窗口等微精度场景             │
│                                                │
│ Tier 3: phase_id + phase_seq                  │
│   tick 内多 phase 处理顺序；用于事件管线编排    │
└────────────────────────────────────────────────┘
```

### 2.2 事件时间戳

每个战斗事件携带：

```csharp
public readonly struct EventTimestamp {
  public readonly long   ServerTick;        // 主时间
  public readonly ushort IntraTickUs;       // 亚 tick μs 偏移
  public readonly byte   PhaseId;           // 处理阶段
  public readonly uint   PhaseSeq;          // phase 内顺序
  public readonly ulong  EventId;           // 全局唯一 uint64
}
```

`EventId` 通过 `(server_tick << 32) | global_counter` 派生，保证唯一且单调。

### 2.3 排序键

对于任意两个事件 A、B，比较顺序：

```csharp
int Compare(in EventTimestamp a, in EventTimestamp b) {
  if (a.ServerTick != b.ServerTick)   return a.ServerTick.CompareTo(b.ServerTick);
  if (a.IntraTickUs != b.IntraTickUs) return a.IntraTickUs.CompareTo(b.IntraTickUs);
  if (a.PhaseId != b.PhaseId)         return a.PhaseId.CompareTo(b.PhaseId);
  if (a.PhaseSeq != b.PhaseSeq)       return a.PhaseSeq.CompareTo(b.PhaseSeq);
  return a.EventId.CompareTo(b.EventId);
}
```

**完全确定性**——无浮点、无哈希、无指针。

---

## 3. Tick 内处理 Phase

### 3.1 Phase 列表

每个 server tick 服务端按固定 phase 顺序处理：

```
┌────────────────────────────────────────────────────────┐
│ Phase 0: Input & Movement                              │
│   消费 InputFrame → 推进 MovementSimulator             │
│   应用 MovementCommand 曲线                             │
│   位置历史写入                                           │
├────────────────────────────────────────────────────────┤
│ Phase 1: Skill Timeline Advance                        │
│   推进所有 active SkillInstance 的 timeline             │
│   触发到点的 TimelineEvent                              │
│   状态机 transitions 求值                               │
├────────────────────────────────────────────────────────┤
│ Phase 2: Hitbox Spawn & Update                         │
│   新 hitbox 进入扫描列表                                │
│   持续 hitbox 更新 origin 跟随                          │
├────────────────────────────────────────────────────────┤
│ Phase 3: Hitbox Scan & Hit Resolution                  │
│   扫描所有候选目标                                       │
│   过滤 + iframe / block 检查                            │
│   命中确认 → 进入 Damage Pipeline                       │
│   含级联（OnDamageDealt → ModifyDamage 等）             │
├────────────────────────────────────────────────────────┤
│ Phase 4: Damage Apply                                  │
│   伤害应用到 victim.Hp                                  │
│   死亡判定（HP ≤ 0）                                    │
│   死亡事件入死亡队列（不在此 phase 处理）                 │
├────────────────────────────────────────────────────────┤
│ Phase 5: Buff Lifecycle                                │
│   过期 buff 触发 OnExpire                               │
│   周期 buff 触发 OnTick                                 │
│   tick 内 ApplyBuff/RemoveBuff 入队的延后处理           │
├────────────────────────────────────────────────────────┤
│ Phase 6: Death Processing                              │
│   遍历死亡队列：                                         │
│     OnDeath → OnKill → 清 buff → state 切换           │
│   死亡触发的 buff handlers 也在此 phase 完成            │
├────────────────────────────────────────────────────────┤
│ Phase 7: AI Decision                                   │
│   AI 行为树 tick                                        │
│   AI 触发的技能 cast                                    │
├────────────────────────────────────────────────────────┤
│ Phase 8: Snapshot & Broadcast                          │
│   构造客户端快照（位置 / 状态）                          │
│   写入网络发送队列                                       │
└────────────────────────────────────────────────────────┘
```

### 3.2 Phase 间约束

- **Phase 顺序固定**，不允许跳过 / 重排
- **Phase 内事件按 PhaseSeq 升序处理**（PhaseSeq 由处理顺序自然分配）
- 一个 Phase 内允许有多个事件级联触发（同 tick 完成）
- 跨 Phase 的事件流必须显式入队（如 Phase 4 死亡事件入 Phase 6 处理队列）

### 3.3 为什么死亡延后到 Phase 6

避免**死亡顺序不一致问题**：

场景：A 和 B 同一 tick 互相释放致命一击。
- 若 A 的 hitbox 在 Phase 3 早于 B 解析（按 PhaseSeq）：
  - A 的攻击命中 B → B 标记死亡
  - 若立即清 B 的 buff → B 的反击 buff handler 不再触发
  - 但 B 的 hitbox 此时尚未扫描 → 错失 B 的攻击
- 若死亡处理推迟到 Phase 6：
  - 所有 hitbox 在 Phase 3 都被扫描（包括"濒死" entity 的 hitbox）
  - 双方互相伤害结算完毕
  - Phase 6 统一处理死亡，互殴双方都死

**这是设计意图**：动作游戏鼓励"同归于尽" 的紧张感。

### 3.4 Phase 内级联

Phase 3 / 5 / 6 内允许级联（一个事件触发另一个）。约束（参见 `BUFF_SYSTEM.md §9`）：

- **深度上限 4**
- **单次级联 50 个 handler 上限**
- **同 instance 不重入**
- **超限静默截断 + warning**
- **级联完成才进入下一 phase**

**禁止跨 tick 级联**：本 tick 触发的事件必须本 tick 处理完，不允许"明天再说"。

---

## 4. 排序示例

### 4.1 两个玩家同 tick 互攻

```
Tick 1234567:
  Phase 3 处理：
    EventId 100: A 的 hitbox 命中 B（PhaseSeq=1）
    EventId 101: B 的 hitbox 命中 A（PhaseSeq=2）
  
  排序后处理顺序：
    Event 100 → A.atk × ... → B.Hp -= damage_to_B
                  → if B.Hp <= 0: 入死亡队列（B）
    Event 101 → B.atk × ... → A.Hp -= damage_to_A
                  → if A.Hp <= 0: 入死亡队列（A）
  
  Phase 6:
    死亡队列 [B, A]
    处理 B.Death → A 获得 OnKill
    处理 A.Death → B 已死，OnKill 给已死者无效（按 OnKill handler 实现）
```

**结果**：双方互殴致死。即使 A 的事件 PhaseSeq 早于 B，B 仍能在死亡前完成自己的攻击（因为死亡处理延后到 Phase 6）。

### 4.2 同 tick AoE 击中多人

```
Tick 1234600:
  Phase 3:
    A 的 AoE hitbox 扫描覆盖 [B, C, D]
    EventId 200: hit B (PhaseSeq=1)
    EventId 201: hit C (PhaseSeq=2)
    EventId 202: hit D (PhaseSeq=3)
  
  排序按 EventId:
    200 → B 受伤
    201 → C 受伤
    202 → D 受伤
  
  独立结算，互不影响。
```

**关键**：3 人受到的伤害值**互相独立**。OnDamageDealt 触发的 ModifyDamage 也是逐目标独立累计。

### 4.3 级联 AoE

```
Tick 1234700:
  Phase 3:
    A 的攻击命中 B → DamageTarget.Apply
      → Phase 5 触发 B 的 "thorns" buff OnDamageTaken handler
        → DamageTarget.Apply (反伤 A) 
          → Phase 5 触发 A 的 "lifesteal" buff OnDamageDealt
            → HealTarget.Apply (回复 A)
              → Phase 5 触发 A 的 "overload at full HP" buff
                → ApplyBuff.Apply (给 A 加狂暴)
                  ↓ 深度 = 4，停止级联
```

**深度 4 的级联结束**。第 5 层尝试触发被静默截断 + warning 日志。

### 4.4 多 hitbox 同目标同 tick

技能 X 在帧 5 / 帧 10 / 帧 15 各 spawn 一个 hitbox（多段攻击），如果某 tick 三个 hitbox 都激活（极端情况，极快攻击）：

```
Phase 3:
  EventId 300: hb1 hit B (PhaseSeq=1)
  EventId 301: hb2 hit B (PhaseSeq=2)
  EventId 302: hb3 hit B (PhaseSeq=3)
```

每个 hitbox 独立判定，B 受到三段独立伤害。

---

## 5. 因果链（Causation Chain）

### 5.1 概念

每个事件可选携带"因"列表：触发我的事件们。

```csharp
public sealed class CombatEvent {
  public EventTimestamp Timestamp;
  public EventType Type;
  public EventPayload Payload;
  public ImmutableArray<ulong> CausedBy;   // 因果父事件 IDs
}
```

### 5.2 用途

| 用途 | 实现 |
|---|---|
| 调试 | 死亡时可追溯：杀死我的伤害是哪个 hitbox，hitbox 来自哪个技能 |
| 反作弊 | 检测异常事件链（如"伤害事件没有对应 hitbox"） |
| 反馈优化 | 客户端可识别"反伤" 与"原伤害"，分开显示 |
| 平衡分析 | 统计某 buff 引发的 N 阶级联事件 |

### 5.3 何时记录

**默认不记录**（性能开销）。仅在以下场景启用：
- 调试模式 / 测试服
- 上报 bug 后开启 trace
- 反作弊触发审计

生产环境关闭，避免无谓开销。

### 5.4 限制

- `CausedBy` 长度 ≤ 8（足够大多数场景）
- 链深度按级联限制 4
- 不形成环（已通过反重入防御）

---

## 6. 客户端事件接收

### 6.1 接收流程

```
网络层接收 UDP 包
  ↓
反序列化为事件列表
  ↓
按 EventId 去重（重传可能产生重复）
  ↓
插入 EventBuffer（按 EventTimestamp 排序）
  ↓
等待 interp_delay（与远端实体同步）
  ↓
按时间窗口取出事件，触发反馈
```

### 6.2 EventBuffer

```csharp
public sealed class EventBuffer {
  SortedSet<CombatEvent> _pending;   // 按 EventTimestamp 排序
  
  public void Push(CombatEvent evt) {
    _pending.Add(evt);
  }
  
  public List<CombatEvent> PopReadyAt(long render_server_tick) {
    var ready = new List<CombatEvent>();
    foreach (var e in _pending) {
      if (e.Timestamp.ServerTick > render_server_tick) break;
      ready.Add(e);
    }
    foreach (var e in ready) _pending.Remove(e);
    return ready;
  }
}
```

### 6.3 渲染时间

`render_server_tick` 由时间同步协议维护：

```
render_server_tick = client_estimated_server_tick - (interp_delay_ms / tick_ms)
```

约滞后 100–120ms（开放世界 20Hz 下 2–3 tick；副本 30Hz 下 3–4 tick）。

### 6.4 乱序到达处理

UDP 不保证顺序：
- 包 N+1 到达，包 N 还在路上
- EventBuffer 按 EventId 排序，包 N 到达后正确插入到包 N+1 之前
- interp_delay 缓冲让"等等还没到的事件"成为可能

### 6.5 包丢失

- 战斗事件走**可靠通道**（参见 `OVERVIEW.md §6.2`）
- 丢包自动重传，不丢失
- 重传可能造成重复 → 按 EventId 去重

### 6.6 时间过期事件

如果某事件的 `ServerTick` 已经过去（render_server_tick 已经超过它）：
- 仍然处理（避免漏掉）
- 但客户端反馈可能"立即播放"而非按计划时间——边缘情况，玩家可能感知微闪
- 监控指标：`events_processed_late_count` 阈值告警

---

## 7. 跨 Cell 事件排序

### 7.1 问题

A 在 cell #1 释放 AoE 命中 B（在 cell #2）。事件流：

```
Cell #1 Tick T:
  Phase 3: A.hitbox 扫描 → 检测到 B（在邻接 cell ghost 形式）
  跨 cell 消息发送给 Cell #2
  
Cell #2 Tick T+1（接收）:
  应用 B 的伤害事件
  时间戳：ServerTick = T（来自 sender，不是 receiver）
```

**关键**：消息携带的 `ServerTick` 是 **sender 的 T**，不是 receiver 处理时的 T+1。

### 7.2 客户端视角统一

B 的客户端：
- 从 cell #2 的快照流中接收事件
- 事件时间戳 = T（A 发起时刻）
- 客户端 EventBuffer 按 T 排序，与 cell #1 上其他事件一致

**跨 cell 不破坏排序**——所有客户端最终看到事件按全局 ServerTick 顺序播放。

### 7.3 跨 cell 死亡处理

A（cell #1）攻击 B（cell #2），B 死亡：
- Cell #2 的 Phase 6 处理 B 死亡（B 的权威 cell）
- OnKill 事件发回 cell #1 给 A
- 跨 cell 死亡事件可能比同 cell 慢 1 tick

**接受这个延迟**——跨 cell 战斗本来就是边界情况，1 tick 偏差对手感影响小。

---

## 8. 死亡时刻的精细化

### 8.1 死亡时刻定义

实体的 "死亡时刻" = `(ServerTick, IntraTickUs)`，对应**致命一击事件的时间戳**。

```csharp
struct DeathInfo {
  EntityRef Victim;
  EntityRef Attacker;     // 致命一击的施加者
  ulong KillingHitEventId;
  EventTimestamp DeathTime;
  DamageType KillingDamageType;
}
```

### 8.2 同 tick 多次伤害取最后一击

A 和 B 都在 tick T 攻击 C，且都致命：
- 按 EventId 顺序：先 A 的事件，后 B 的事件
- A 的伤害命中 → C.Hp 降到 50
- B 的伤害命中 → C.Hp 降到 -10（已死）
- DeathInfo.Attacker = B（最后一击）

**Last hit wins**——这是动作游戏惯例。

### 8.3 OnKill 时机

死亡处理在 Phase 6（参见 §3.3）。Phase 6 内：
- 遍历死亡队列（FIFO 顺序）
- 对每个死亡：
  - 触发 victim 的 OnDeath
  - 若 OnDeath 治愈复活（Hp > 0），从队列移除
  - 否则触发 attacker 的 OnKill
  - 清非永久 buff
  - 进入 Dead state

### 8.4 OnKill 给已死者的处理

A 杀死 B 的同时被 B 反击致死：
- Phase 6 先处理 B 死亡 → 触发 A 的 OnKill（A 此时还活着）
- Phase 6 处理 A 死亡 → 触发 B 的 OnKill（B 已经死了）

**B 的 OnKill 仍然触发**，因为 OnKill 是 attacker 的 buff handler，handler 检查的是 attacker 的状态而非 victim 的。即"我在死前杀了你"算 kill。

但 OnKill handler 内若尝试操作（如治疗自己），需要 self.IsAlive 检查（DSL 自动加，详见 `BUFF_SYSTEM.md`）。

---

## 9. 移动事件排序

### 9.1 移动事件

| 事件 | 时机 |
|---|---|
| `EntityMoved` | Phase 0 推进 movement 后 |
| `EntityTeleported` | MovementCommand teleport |
| `EntityFell` | 高度变化超阈值 |
| `EntityCellChanged` | 跨 cell |

### 9.2 移动事件不进 EventBuffer

移动状态走**位置快照通道**（不可靠 UDP，每 tick），与战斗事件分离：
- 客户端用 Hermite 插值平滑展示
- 不需要"按 EventId 排序"
- 移动事件本身不会乱序（每 tick 一帧最新位置）

战斗事件（hit / damage / buff）才进 EventBuffer。

### 9.3 移动与战斗的同步

战斗事件的位置参考是**事件时间戳对应的位置**：
- `LagCompensation` 已处理（位置历史）
- 客户端渲染战斗事件时用渲染时间对应的位置（远端实体的当前插值位置）

不需要事件本身携带位置——位置由实体当前展示位置决定。

---

## 10. 事件类型清单

### 10.1 完整事件类型

```csharp
public enum CombatEventType : ushort {
  // Skill 生命周期
  SkillStart = 1,
  SkillStateChange,
  SkillEnd,
  SkillCancelled,
  SkillRejected,
  
  // Hitbox
  HitboxSpawn = 20,
  HitboxRemove,
  HitDetected,           // 命中确认（lag comp 通过）
  HitMissed,             // 闪避 / Block / iframe
  
  // Damage / Heal
  DamageDealt = 40,
  HealReceived,
  ShieldAbsorbed,
  ShieldBroken,
  
  // Buff
  BuffApplied = 60,
  BuffRemoved,
  BuffStackChanged,
  BuffRefreshed,
  BuffTickFired,
  
  // Death
  Death = 80,
  Resurrected,
  Kill,                  // attacker 视角
  
  // CC
  CcApplied = 100,
  CcRemoved,
  
  // Stat
  StatChanged = 120,     // 仅重大变化（HP 跨阈值）
  
  // Visual triggers
  HitPauseTrigger = 140,
  CameraShakeTrigger,
  CameraSlomoTrigger,
  ShowDamageNumber,
  
  // Sound
  PlaySound = 160,
  
  // Chat / System
  CombatLog = 180,       // 战斗日志（PvP 复盘用）
}
```

### 10.2 客户端 vs 服务端

| 事件类别 | 服务端权威 | 客户端预测 |
|---|---|---|
| Skill 生命周期 | 是 | 部分（自施时） |
| Hit Detected | 是 | 否 |
| Damage | 是 | 否（不预测数字） |
| Buff | 是 | 部分（自施时） |
| Death | 是 | 否 |
| Visual triggers | 服务端发起 | 客户端执行 |
| Sound | 同上 | 同上 |

---

## 11. 性能与监控

### 11.1 性能预算

| 操作 | 预算 |
|---|---|
| 单事件入 EventBuffer | < 100 ns（B-tree 插入） |
| 单 tick 排序事件数 | < 1000（极端值） |
| Phase 间过渡 | < 10 μs |
| 单 tick 总事件处理 | < 5 ms |

### 11.2 监控指标

- 单 tick 事件总数（峰值、平均）
- Phase 各阶段耗时
- 级联深度分布（>3 应告警）
- 客户端事件迟到率（render time 已过的事件比例）
- EventBuffer 堆积大小（>200 应告警）

---

## 12. FAQ 与反模式

### Q1: 为什么要用 IntraTickUs 而不是简单的事件 seq？

某些机制（格挡窗口、parry）有亚 tick 精度需求：
- 格挡窗口 100ms 跨越 2-3 个 tick
- 需要知道"输入是在窗口的 50ms 内还是 80ms"做完美 parry 判定

IntraTickUs 提供这种精度。普通事件不强制使用。

### Q2: 同 tick 多次伤害的 RNG 种子是否一致？

**不一致**——每次伤害的 RNG 种子包含 `event_seq`：
```
seed = hash(session_id, attacker_id, tick, RandomPurpose.Crit, event_seq)
```

`event_seq` 不同 → 暴击 roll 结果不同。这避免"同 tick 多次必然全暴 / 全不暴"的失衡。

### Q3: 客户端事件晚到很久（>500ms）怎么办？

策略：
- 仍然处理（避免漏关键事件如 Death）
- 但视觉反馈打折（不播 hit pause 等"沉浸式"特效）
- 写日志，超过阈值上报玩家网络问题

### Q4: 跨 cell 事件比同 cell 慢 1 tick，PvP 公平吗？

PvP arena 在单 instance 内，**不存在跨 cell**——所有玩家在同 cell。

PvE 跨 cell 战斗（开放世界 boss 横跨边界）确实慢 1 tick。这种场景：
- 1 tick = 33ms（副本）或 50ms（开放世界），不显著
- 通常不会持续跨 cell（boss 移动慢于战斗节奏）
- 接受不完美

### Q5: Death 事件延后到 Phase 6 会不会造成"看到 HP=0 但还在攻击"？

**会**，但客户端处理：
- HP 显示从权威状态读
- 客户端可以看到 victim HP=0 但还在挥剑（Phase 6 还没处理）
- ~33ms 后 Death 事件到达，victim 切到死亡动画

**视觉一致性问题**：
- 客户端可以**预测死亡**（HP=0 立即播放死亡动画）
- 但服务端权威，预测错（被复活技能拉起来）需要回滚——少见但需处理

简单做法：客户端不预测，等 Death 事件。33ms 视觉延迟可接受。

### Q6: 一个事件触发的级联里有 50 个 handler，会不会卡 tick？

50 handler × ~5 μs = 250 μs，远低于 33ms tick 预算。**不卡**。

但 50 已是上限，超过表示设计有问题（buff 链过长），需要重新设计。

### Q7: 客户端如何知道事件的 PhaseSeq？

服务端构造事件时分配 PhaseSeq，写入序列化字段。客户端读取后用于排序。

PhaseSeq 是 uint32，每 tick 重置（不需要全局唯一）。

### Q8: 因果链记录开启时性能影响多大？

每事件多 8 个 ulong = 64 字节。生产环境 1000 事件/tick × 64 = 64 KB。带宽和内存都可接受。

但**默认关闭**——99% 玩家不需要因果链，开启徒增带宽。仅 debug 模式 / 反作弊审计开启。

### Q9: 客户端能不能用本地时钟而非 server tick 排序？

**不能**。本地时钟有抖动，会破坏事件顺序。

所有客户端事件排序必须用 server tick + IntraTickUs。本地时钟仅用于"何时取出 ready 事件"。

### Q10: 互相必杀场景在 PvP arena 里多见吗？

不多见，但存在。常出现于：
- 双方残血同时按必杀键
- 反伤 buff 触发死亡
- 自爆类技能

设计处理（§4.1）让双方都死，这是动作游戏的"双杀"经典体验，玩家感知公平。

---

### 反模式清单

- ❌ 用 wall clock（DateTime.Now）排序事件（端同破坏）
- ❌ 用 GetHashCode 作为排序辅助键（非确定）
- ❌ Phase 内修改其他 entity 的状态（应通过事件 emit）
- ❌ Phase 7 后再 emit 事件（必须在 Phase 8 前完成）
- ❌ 让客户端用 Time.time 取出事件（应用 server tick）
- ❌ 跨 tick 级联（必须当 tick 完成）
- ❌ 让 EventBuffer 无限堆积（设监控阈值）
- ❌ 在 Phase 5 处理死亡（应延后到 Phase 6）
- ❌ Death 后 OnKill 被忽略（应触发即使 attacker 也死）
- ❌ 自定义事件不带 server tick 时间戳（破坏全局排序）

---

## 13. 里程碑

| 阶段 | 交付 |
|---|---|
| P0 末 | EventTimestamp / Compare 函数；EventBuffer 骨架 |
| P1 末 | Phase 列表实现；Phase 间过渡 |
| P2 中 | 完整级联控制；同 tick 多事件正确处理 |
| **P2 末** | **端同测试覆盖：1000 场景双端事件序列一致** |
| P3 | 客户端缓冲与重排；视觉触发同步 |
| P4+ | 跨 cell 事件流；因果链调试工具 |

---

## 14. 文档维护

- **Owner**：Tech Lead + Network Engineer
- **关联文档**：
  - `OVERVIEW.md`（§7 难点三引用本文）
  - `DETERMINISM_CONTRACT.md`（排序键规范）
  - `SKILL_SYSTEM.md`（Skill 事件触发时机）
  - `BUFF_SYSTEM.md`（buff 事件 + 级联控制）
  - `HIT_VALIDATION.md`（hit 事件来源）
  - `STAT_AND_DAMAGE.md`（damage 事件）
  - `MOVEMENT_SYNC.md`（移动事件分离）
  - `COMBAT_FEEL.md`（visual trigger 事件）

---

**文档结束。**

**核心纪律重申**：
1. **server_tick 是唯一时间真相**：所有排序基于它
2. **Phase 顺序固定**：8 个 phase 不允许重排或跳过
3. **Death 延后到 Phase 6**：保证互殴公平
4. **级联同 tick 完成**：不允许跨 tick 拖延
5. **客户端排序按 server 时间戳**：本地时钟不参与排序
6. **EventBuffer 缓冲 ~100ms**：吸收网络抖动
