# Buff 系统设计（Buff System）

> **用途**：定义 Atlas 中"持续状态 + 事件响应"类效果的统一模型（buff、debuff、状态、aura 等），以及如何与技能系统、属性系统、战斗事件协同。
>
> **读者**：工程（必读）、战斗策划（必读）、数值策划（§5 必读）。
>
> **状态**：草案 v0.1 — 待团队评审。
>
> **前置文档**：`OVERVIEW.md`、`00_foundations/DETERMINISM_CONTRACT.md`、`03_combat/SKILL_SYSTEM.md`
>
> **关联文档**：`STAT_AND_DAMAGE.md`（属性计算）、`COMBAT_EVENT_ORDERING.md`（事件顺序）、`COMBAT_ACTIONS.md`（Action 层）

---

## 1. 设计目标与边界

### 1.1 目标

1. **统一模型**：所有"持续效果"走同一套基础设施——Buff（正面）、Debuff（负面）、状态（stun/iframe）、灵气（aura）、中毒/燃烧（DoT）、装备被动（passive）、灵魂连接（link）等
2. **事件驱动**：Buff 是**状态**而非**时间轴**——在特定事件（被击、暴击、死亡等）发生时响应，而不是"按帧推进"
3. **复用 Skill 基础设施**：与 `SKILL_SYSTEM.md` 共享 Action / DSL 执行器，避免重复造轮子
4. **端同一致**：Buff 生命周期、属性加成、事件触发完全确定性
5. **支持高并发**：单 cell 可能同时存在 10000+ BuffInstance（400 实体 × 25 平均 buff），必须低开销

### 1.2 非目标

- **不做 timeline 驱动的 buff**：持续效果用 duration + periodic tick + hooks 表达
- **不处理 buff 的视觉特效**：VFX/UI 由 OnApply/OnRemove handler 里的 `PlayVFX` action 负责，buff 本身不管渲染
- **不做任意脚本能力**：buff 的 handler 通过白名单 Action/DSL，不允许写任意代码

### 1.3 能力矩阵

| 能力 | 支持方式 |
|---|---|
| 固定时长增益（攻击 +30% 持续 10s） | 基础用法 |
| 永久被动（装备词条） | `DurationMs = -1` |
| 可叠层（中毒 1–5 层） | `MaxStacks > 1`，modifier 按层数缩放 |
| 周期触发（DoT 每秒跳伤害） | `OnTick` handler + `TickIntervalMs` |
| 反应式（被击时反伤） | `OnDamageTaken` handler |
| 触发式（暴击时加 buff） | `OnCrit` handler + `ApplyBuff` action |
| 驱散/免疫 | `DispelType` + dispel action |
| 状态控制（stun/silence/iframe） | flag 型 modifier，damage pipeline 检查 |
| 灵魂连接（一人死亡队友减血） | `OnKill` / `OnDeath` 跨实体 handler |
| 吟唱/充能（看似 timeline） | **仍用 buff 表达**（duration + OnTick + OnExpire） |

---

## 2. 总体架构

### 2.1 模型对比

```
Skill (时间轴驱动)                  Buff (事件驱动)
────────────────                   ────────────────
生命周期  毫秒–秒                   秒–分钟/永久
核心概念  TimelineEvent              StatModifier + EventHandler
推进方式  每 tick 按时间推进事件      事件触发时响应
适合场景  "我这一刻做什么"            "发生 X 时我做 Y"
```

### 2.2 Buff 的三要素

```
┌──────────────────────────────────────────┐
│  BuffDef                                 │
│  ├─ 生命周期元数据                        │
│  │   duration / max_stacks / stack_mode  │
│  ├─ 被动加成 (Modifiers)                  │
│  │   StatMod: +30% atk, -20% speed, ...  │
│  └─ 事件钩子 (Handlers)                   │
│      OnApply: [Action/DSL]                │
│      OnTick: [Action/DSL]                 │
│      OnDamageTaken: [Action/DSL]          │
│      ...                                 │
└──────────────────────────────────────────┘
```

**三者各司其职**：
- **元数据**：由 BuffSystem 管理生命周期
- **Modifiers**：由 StatCache 聚合到实体最终属性
- **Handlers**：事件发生时走 Action/DSL 执行器

### 2.3 共享基础设施

Buff 与 Skill 共享：
- **Action 全集**（`SKILL_SYSTEM.md §5`）
- **DSL 执行器**（`SKILL_SYSTEM.md §6`）
- **确定性契约**（`DETERMINISM_CONTRACT.md`）
- **端同随机源**、时间单位、坐标系统

独有：
- **StatModifier 系统**（本文 §5）
- **事件钩子注册与派发**（本文 §6）
- **叠层语义**（本文 §7）

---

## 3. 数据模型

### 3.1 BuffDef

```csharp
public sealed class BuffDef {
  public int           BuffId;
  public string        Name;
  public BuffCategory  Category;       // Beneficial / Harmful / Neutral / System
  
  public int           DurationMs;     // -1 = 永久
  public int           MaxStacks;      // 1 = 不可叠
  public StackMode     StackMode;      // 见 §7
  public DispelType    DispelType;     // Magic / Physical / Curse / None
  
  public bool          IsHidden;       // UI 不显示（通常 System 类）
  public bool          PersistOnDeath; // 死亡时是否清除
  public int           Priority;       // 同槽位冲突时的优先级
  
  public ImmutableArray<StatModDef>    Modifiers;
  public ImmutableDictionary<BuffEvent, ImmutableArray<HandlerRef>> Handlers;
  
  public int           TickIntervalMs; // OnTick 周期；0 表示无周期触发
  
  public string        IconKey;        // UI 图标 localization key
}

public enum BuffCategory { Beneficial, Harmful, Neutral, System }
public enum DispelType { None, Magic, Physical, Curse, AllMagic, Unique }
```

### 3.2 BuffInstance

```csharp
public sealed class BuffInstance {
  public long         InstanceId;       // 全局唯一 uint64
  public int          BuffId;
  public EntityRef    Target;
  public EntityRef    Source;           // 施放者（可为空 = 系统）
  public int          SourceSkillId;    // 触发来源技能 ID（0 = 非技能触发）
  
  public long         ApplyTick;
  public long         ExpireTick;       // = ApplyTick + DurationTicks；永久 = Int64.MaxValue
  public int          Stacks;
  public long         NextTickTick;     // 下一次 OnTick 触发的 tick
  
  public Dictionary<string, Variant> Variables;  // buff-scoped，少用
}
```

### 3.3 关键字段说明

- **`Source`** 重要性：决定 damage 归属、击杀计分、是否可被 source 免疫（"你不能被自己的 DoT 杀死"机制）
- **`SourceSkillId`**：便于统计"某技能造成的所有 DoT 伤害"，也用于技能之间的交互（如"此 buff 存在时技能 X 免费")
- **`Variables`**：buff 实例级变量，实例销毁时释放；通常用于"记住 buff 生效前的原值"（某些返还类 buff）

---

## 4. BuffSystem: 每 cell 一个实例

```csharp
public sealed class CellBuffSystem {
  readonly List<BuffInstance> _instances;     // 按 (Target.Id, InstanceId) 排序
  readonly Dictionary<EntityRef, List<BuffInstance>> _byTarget;
  
  public void Tick(CombatContext ctx) {
    long now = ctx.CurrentTick;
    
    // 过期处理：自然结束
    for (int i = _instances.Count - 1; i >= 0; i--) {
      var inst = _instances[i];
      if (inst.ExpireTick <= now) {
        Remove(inst, RemoveReason.Expired, ctx);
      }
    }
    
    // 周期触发 OnTick
    foreach (var inst in _instances) {
      while (inst.NextTickTick <= now && !inst.IsRemoved) {
        TriggerHandler(inst, BuffEvent.OnTick, ctx);
        long intervalTicks = inst.Def.TickIntervalMs / ctx.TickDurationMs;
        inst.NextTickTick += Math.Max(1, intervalTicks);
      }
    }
    
    // 通常外部事件（damage/crit/...）在本 tick 中处理完毕
    // 由 CombatEventSystem 在同 tick 派发
  }
  
  public BuffInstance Apply(int buffId, EntityRef target, EntityRef source, 
                             int durationOverrideMs, int initialStacks, 
                             CombatContext ctx) { /* §7 */ }
  
  public void Remove(BuffInstance inst, RemoveReason reason, CombatContext ctx) {
    TriggerHandler(inst, BuffEvent.OnRemove, ctx);
    if (reason == RemoveReason.Expired) {
      TriggerHandler(inst, BuffEvent.OnExpire, ctx);
    }
    MarkModifiersDirty(inst.Target);
    _instances.Remove(inst);
    _byTarget[inst.Target].Remove(inst);
    inst.IsRemoved = true;
  }
}
```

**确定性保证**：
- `_instances` 维护排序不变量：按 `(target_id, buff_id, instance_id)` 升序
- 遍历顺序固定
- 同 tick 内多事件的触发顺序通过 `CombatEventSystem` 的时间戳裁决（见 `COMBAT_EVENT_ORDERING.md`）

---

## 5. Stat Modifier 系统

### 5.1 为什么 Modifier 独立于 Buff

两个关键考量：
1. **装备、天赋、等级、基础属性**也是 modifier 来源，和 buff 是同一种数据
2. **属性计算需缓存**，每帧重算所有 buff 的加成开销不可接受

抽象出统一的 **StatCache**，Buff 只是其中一个 modifier provider。

### 5.2 StatMod 数据结构

```csharp
public sealed class StatModDef {
  public StatType Stat;        // 目标属性
  public ModOp    Op;          // 运算类型
  public float    Value;       // 数值
  public bool     PerStack;    // 是否按层数缩放
  public int      Priority;    // 计算顺序（同 Op 内）
}

public enum StatType {
  // 基础属性
  MaxHp, MaxMp, AtkPower, MagPower, Defense, MagResist,
  // 战斗属性
  CritChance, CritDamage, HitRate, EvasionRate, BlockRate,
  // 移动属性
  MoveSpeed, AtkSpeed, CastSpeed, CooldownReduction,
  // 资源
  MpRegen, HpRegen,
  // 特殊
  DamageMul_Physical, DamageMul_Magic, DamageTaken_Mul,
  // 标志位 (用 Op=Set, Value=1/0 模拟 bool)
  Flag_SuperArmor, Flag_Iframe, Flag_Silenced, Flag_Rooted, Flag_Stunned,
  // ...
}

public enum ModOp {
  Add,                // += value
  MulAdditive,        // 加性百分比：Σ 后乘基础
  MulMultiplicative,  // 乘性百分比：连乘
  Set,                // 强制赋值（用于 flag）
  Cap,                // 上限限制
  Floor,              // 下限限制
}
```

### 5.3 计算公式

对每个 `StatType`：

```
Add_pool = base + Σ (Add modifiers)
Mul_additive_factor = 1 + Σ (MulAdditive modifiers)
Mul_multiplicative_factor = ∏ (MulMultiplicative modifiers)

intermediate = Add_pool × Mul_additive_factor × Mul_multiplicative_factor
```

然后应用 `Set / Cap / Floor`（按 Priority 升序）：
```
for each mod in [Set, Cap, Floor] sorted by Priority:
  if Op == Set:    intermediate = mod.Value
  if Op == Cap:    intermediate = min(intermediate, mod.Value)
  if Op == Floor:  intermediate = max(intermediate, mod.Value)

final = intermediate
```

**每个 modifier 的贡献**：若 `PerStack == true`，`effective_value = mod.Value × buff.Stacks`；否则 `= mod.Value`。

### 5.4 StatCache

```csharp
public sealed class StatCache {
  EntityRef _owner;
  bool _dirty;
  Dictionary<StatType, float> _values;
  
  public float Get(StatType stat) {
    if (_dirty) Recompute();
    return _values[stat];
  }
  
  public void MarkDirty() { _dirty = true; }
  
  void Recompute() {
    // 遍历所有 modifier 源（base stats、装备、等级、buff instances on owner）
    // 按 §5.3 公式计算
    _dirty = false;
  }
}
```

### 5.5 失效（Invalidate）时机

`MarkDirty()` 在以下时刻调用：
- BuffInstance 被 Apply / Remove / Refresh
- BuffInstance 的 Stacks 变化
- 装备变化
- 等级变化
- 基础属性重算（较少）

**不每帧失效**（虽然有些 buff 本身是时变的——但 modifier 是阶跃函数，不是连续函数）。

### 5.6 性能目标

- `StatCache.Get()` 命中缓存：< 10 ns
- `Recompute()` 冷启动：< 20 μs（假设 30 active buff，15 种 stat）
- 单 cell 单 tick StatCache 总耗时 < 100 μs

---

## 6. 事件钩子系统

### 6.1 BuffEvent 枚举

```csharp
public enum BuffEvent {
  // 生命周期
  OnApply,            // 首次施加或 StackIndependent 新实例
  OnRemove,           // 任何移除（expired/dispelled/cleansed/replaced）
  OnExpire,           // 自然过期（Remove 子集，仅 duration 到期时）
  OnRefresh,          // StackMode 为 Refresh/RefreshStack 时再次施加
  OnStackChanged,     // Stacks 数量变化（+/-）
  
  // 周期
  OnTick,             // TickIntervalMs 到点触发
  
  // 战斗事件
  OnDamageDealt,      // 本实体造成伤害（作为 attacker）
  OnDamageTaken,      // 本实体受到伤害（作为 victim）
  OnCrit,             // 本实体造成暴击
  OnCritTaken,        // 本实体被暴击
  OnKill,             // 本实体击杀他人
  OnDeath,            // 本实体死亡
  OnHit,              // 本实体技能命中（未必伤害）
  OnHurt,             // 本实体被命中（未必伤害）
  OnHeal,             // 本实体获得治疗
  OnBlock,            // 本实体格挡成功
  OnEvade,            // 本实体闪避成功
  
  // 状态事件
  OnStatusApplied,    // 本实体获得其他 buff
  OnStatusRemoved,    // 本实体失去其他 buff
  OnCcApplied,        // 本实体被 CC（控制）
  
  // 行为事件
  OnSkillCast,        // 本实体释放技能（cast 时）
  OnSkillHit,         // 本实体技能命中（结算时）
  OnMove,             // 本实体开始移动
  OnStop,             // 本实体停止移动
  OnJump,             // 本实体跳跃
  OnLand,             // 本实体落地
}
```

### 6.2 Handler 数据

```csharp
// Handlers 字段在 BuffDef 里
public ImmutableDictionary<BuffEvent, ImmutableArray<HandlerRef>> Handlers;

public sealed class HandlerRef {
  public int    HandlerId;        // 指向 BuffHandlers 表
  public string ConditionDslRef;  // 可选：触发前置条件
  public HandlerFilter Filter;    // 过滤器（例：OnDamageTaken 只对某伤害类型）
}
```

### 6.3 Handler 内容

Handler 由 Action list 构成，执行上下文包含：

```csharp
public struct BuffHandlerContext {
  public BuffInstance Self;
  public EntityRef    EventActor;     // 事件主体（如 OnDamageDealt 的 attacker 即 Self.Target）
  public EntityRef    EventTarget;    // 事件客体（如 OnDamageDealt 的 victim）
  public EventPayload Payload;        // 事件参数（damage amount、crit flag 等）
  public CombatContext Ctx;
}
```

DSL 在 handler 里可访问 `self`、`source`、`target`、`damage.amount` 等。

### 6.4 事件派发流程

```
战斗事件产生（例：A 对 B 造成 100 伤害）
  ↓
CombatEventSystem.Emit(Damage{attacker=A, victim=B, amount=100})
  ↓
对 A 的所有 buff 检查 OnDamageDealt handler
对 B 的所有 buff 检查 OnDamageTaken handler
  ↓
按 (buff.Priority, instance.ApplyTick) 排序后依次触发
  ↓
每个 handler 执行：
  ├─ 评估 ConditionDsl
  ├─ 应用 Filter
  └─ 执行 Action list
  ↓
handler 可能产生新事件（级联）→ 回到 CombatEventSystem
```

**级联控制**：见 §9。

### 6.5 OnTick 的帧对齐

`TickIntervalMs` 不一定是 tick 时长的整数倍（比如 500ms 间隔，tick 33ms）。对齐策略：

```
intervalTicks = max(1, round(TickIntervalMs / TickDurationMs))
NextTickTick = ApplyTick + intervalTicks
```

副本（30Hz，33.33ms）下 500ms 间隔 ≈ 15 tick（实际 499.95ms），误差 < 1%，可接受。

**刷新策略**：Refresh 类 stack 重施加时，`NextTickTick` 可选重置或保持，由 BuffDef 配置字段 `TickResetOnRefresh: bool` 决定。

---

## 7. 叠层语义（Stack Semantics）

### 7.1 StackMode 枚举

```csharp
public enum StackMode {
  Replace,              // 移除旧实例，加新实例（重置 duration 与 stacks）
  Refresh,              // 保留 stacks，刷新 duration
  RefreshStack,         // stacks += 1 (封顶 MaxStacks)，duration 刷新
  StackIndependent,     // 每次施加创建独立实例（多计时器）
  Ignore,               // 已存在则忽略新施加
  Extend,               // duration += 新 duration（封顶 2 × DurationMs）
}
```

### 7.2 Apply 流程（核心决策树）

```csharp
public BuffInstance Apply(int buffId, EntityRef target, EntityRef source,
                           int durationOverrideMs, int initialStacks,
                           CombatContext ctx) {
  var def = GetDef(buffId);
  var existing = FindExisting(target, buffId);  // 按 mode 匹配
  
  if (existing == null) {
    return CreateFresh(def, target, source, durationOverrideMs, initialStacks, ctx);
  }
  
  switch (def.StackMode) {
    case StackMode.Replace:
      Remove(existing, RemoveReason.Replaced, ctx);
      return CreateFresh(def, target, source, durationOverrideMs, initialStacks, ctx);
    
    case StackMode.Refresh:
      existing.ExpireTick = ctx.CurrentTick + DurationToTicks(durationOverrideMs, ctx);
      if (def.TickResetOnRefresh) RecomputeNextTick(existing, ctx);
      TriggerHandler(existing, BuffEvent.OnRefresh, ctx);
      return existing;
    
    case StackMode.RefreshStack:
      existing.ExpireTick = ctx.CurrentTick + DurationToTicks(durationOverrideMs, ctx);
      int oldStacks = existing.Stacks;
      existing.Stacks = Math.Min(existing.Stacks + initialStacks, def.MaxStacks);
      if (existing.Stacks != oldStacks) {
        MarkModifiersDirty(existing.Target);
        TriggerHandler(existing, BuffEvent.OnStackChanged, ctx);
      }
      TriggerHandler(existing, BuffEvent.OnRefresh, ctx);
      return existing;
    
    case StackMode.StackIndependent:
      return CreateFresh(def, target, source, durationOverrideMs, initialStacks, ctx);
    
    case StackMode.Ignore:
      return existing;  // 无操作
    
    case StackMode.Extend:
      long maxExpire = ctx.CurrentTick + DurationToTicks(def.DurationMs * 2, ctx);
      long proposedExpire = existing.ExpireTick + DurationToTicks(durationOverrideMs, ctx);
      existing.ExpireTick = Math.Min(proposedExpire, maxExpire);
      TriggerHandler(existing, BuffEvent.OnRefresh, ctx);
      return existing;
  }
}
```

### 7.3 "FindExisting" 的匹配规则

需要明确"什么算同一个 buff"：

| 匹配维度 | 默认行为 | 覆盖方式 |
|---|---|---|
| 同 BuffId | 必须 | 不可覆盖 |
| 同 Target | 必须 | 不可覆盖 |
| 同 Source | **不要求** | 配置 `MatchBySource=true` 时要求 |

**`MatchBySource`** 的典型用途：
- `true`：PvP 里"A 对 B 施毒"和"C 对 B 施毒"是两个独立实例（避免 C 覆盖 A）
- `false`：大多数 PvE 怪物 debuff 不关心来源

### 7.4 Modifier 在叠层时的计算

每个 BuffInstance 的 modifier 生效值 = `mod.Value × (mod.PerStack ? Stacks : 1)`。

**对于 StackIndependent**：多个实例的 modifier 各自生效、**独立累加**（多实例时 stat 计算会被多个 modifier 贡献）。

### 7.5 Dispel 与叠层交互

Dispel（驱散）动作：
- 按 `DispelType` 匹配；`Unique` 类型不可驱散
- 驱散 1 层（StackIndependent 场景为 1 实例；Stack 场景为 stacks -= 1）
- 驱散到 0 时 Remove

---

## 8. 生命周期

### 8.1 创建路径

1. **技能释放**：Skill 的 `ApplyBuff` action 调用 `BuffSystem.Apply`
2. **Buff 触发**：其他 buff 的 handler 中 `ApplyBuff`
3. **装备系统**：实体装备物品 → 自动施加装备 buff（`DurationMs=-1`）
4. **系统施加**：如进入副本给予"副本 buff"
5. **脚本施加**：`custom_handler` 中调用

### 8.2 移除路径

枚举所有移除原因：

```csharp
public enum RemoveReason {
  Expired,        // 自然过期
  Dispelled,      // 被驱散
  Cleansed,       // 被清除（包括友方清除 debuff）
  Replaced,       // StackMode.Replace 时新实例取代
  Killed,         // Target 死亡且 !PersistOnDeath
  SourceDead,     // Source 死亡且 BuffDef.RemoveOnSourceDeath==true
  Manual,         // 脚本主动移除
  EquipRemoved,   // 装备移除类（限装备 buff）
  SystemAdjusted, // 系统调整（重载、热更）
}
```

**OnRemove handler** 在所有路径触发；**OnExpire handler** 仅在 `Expired` 触发（用于区别"自然消失 vs 被打断"）。

### 8.3 Target 死亡处理

Target 死亡时：
```
if (buff.Def.PersistOnDeath) {
  // 保留，常见于"复活后重新激活"的 buff
} else {
  Remove(buff, RemoveReason.Killed, ctx);
}
```

### 8.4 Source 死亡处理

Source 死亡时：
- 默认 buff 保留（已经施加的毒、debuff 不会因施毒者死亡而消失）
- 可配置 `RemoveOnSourceDeath=true` 改变行为（用于"宠物 aura 随主人消失"）

### 8.5 跨 Cell 处理

**原则**：Buff 绑定在 Target 上，随 Target 所在 Cell 存在。

- Target 跨 Cell 迁移时，BuffInstance 完整迁移（数据序列化随行）
- BuffInstance 的 Source 字段是 EntityRef（可能指向另一 Cell 的实体）
- Source 被 dispose 或跨服消失：`Source` 置为 `EntityRef.Null`，buff 不自动移除（除非 `RemoveOnSourceDeath`）

**战斗中实体锁 Cell**（OVERVIEW §5.3）避免了 Cell 切换与 buff 的并发复杂度。

---

## 9. 级联触发控制

### 9.1 问题

Buff handler 可能触发其他 buff：

```
A 被击中
  → A 身上的 "反伤 aura" OnDamageTaken handler 触发
  → 对 attacker 造成伤害
  → attacker 身上的 "吸血 passive" OnDamageDealt handler 触发
  → attacker 回复 HP
  → attacker 身上的 "治疗时回蓝" buff OnHeal handler 触发
  → attacker MP 回复
  → attacker 身上的 "满蓝时过载" buff OnResourceChanged handler 触发
  → ...
```

无限级联会 lockup cell tick。必须有保护。

### 9.2 控制机制

```csharp
public struct BuffEventContext {
  public int Depth;                           // 当前触发深度
  public HashSet<long> InstancesInChain;     // 本次级联中已触发的 buff instances
  
  public const int MaxDepth = 4;
  public const int MaxTotalHandlers = 50;    // 单次级联总 handler 上限
  
  public bool CanTrigger(BuffInstance inst) {
    if (Depth >= MaxDepth) return false;
    if (InstancesInChain.Contains(inst.InstanceId)) return false;  // 反重入
    if (InstancesInChain.Count >= MaxTotalHandlers) return false;
    return true;
  }
}
```

### 9.3 规则

1. **反重入**：同一 BuffInstance 在一次级联中只能触发一次
2. **深度限制**：链式调用最多 4 层
3. **总数限制**：单次级联最多 50 个 handler
4. **超限处理**：记录 warning 日志，丢弃后续触发（不报错，不中断战斗）
5. **同 tick 批处理**：所有级联在**同一 tick 内完成**，不跨 tick

### 9.4 为什么选 4 层

经验数据：
- 1 层：直接响应
- 2 层：buff A 触发 buff B
- 3 层：复杂套娃（反伤→吸血→回蓝）
- 4 层：极限场景，超出即为设计问题

调高上限不会"更好"，只会让 bug 更难发现。4 是经验平衡点。

### 9.5 违规诊断

每次超限：
```
WARN: Buff cascade limit exceeded
  Trigger chain: [reflect_aura → lifesteal_passive → mp_on_heal → overload_at_full_mp → ...]
  Truncated at: overload_at_full_mp
  CellId: 42, Tick: 1234567
```

运营监控面板统计 top-10 超限 buff 组合，策划针对性修改。

---

## 10. Excel Schema

### 10.1 总览

```
data/source/
├── Buffs.xlsx              # 总表
├── BuffModifiers.xlsx      # 被动加成
├── BuffHandlers.xlsx       # 事件钩子
└── (复用 Skill 的 Action 表和 DSL 表)
```

### 10.2 Buffs.xlsx

| 字段 | 类型 | 含义 |
|---|---|---|
| `buff_id` | int | 主键 |
| `name` | string | 内部名 |
| `display_name` | string | 本地化 key |
| `category` | enum | Beneficial/Harmful/Neutral/System |
| `duration_ms` | int | -1 = 永久 |
| `max_stacks` | int | 最大层数 |
| `stack_mode` | enum | Replace/Refresh/RefreshStack/StackIndependent/Ignore/Extend |
| `match_by_source` | bool | 查找 existing 时是否匹配 source |
| `dispel_type` | enum | None/Magic/Physical/Curse/Unique |
| `tick_interval_ms` | int | 0 = 无周期触发 |
| `tick_reset_on_refresh` | bool | Refresh 时是否重置 NextTick |
| `persist_on_death` | bool | Target 死亡时是否保留 |
| `remove_on_source_death` | bool | Source 死亡时是否移除 |
| `is_hidden` | bool | UI 不显示 |
| `priority` | int | 同槽位冲突时优先级 |
| `icon_key` | string | UI 图标 |

### 10.3 BuffModifiers.xlsx

| 字段 | 类型 | 含义 |
|---|---|---|
| `buff_id` | int | 外键 |
| `stat` | enum | 目标 StatType |
| `op` | enum | Add/MulAdditive/MulMultiplicative/Set/Cap/Floor |
| `value` | float | 数值 |
| `per_stack` | bool | 是否按层数缩放 |
| `priority` | int | 同 Op 内的计算顺序 |

示例：
```
buff_id | stat       | op              | value | per_stack | priority
2003    | atk_power  | MulAdditive     | 0.30  | false     | 0
2003    | atk_speed  | MulAdditive     | 0.20  | false     | 0
2003    | defense    | MulAdditive     | -0.15 | false     | 0
```

### 10.4 BuffHandlers.xlsx

| 字段 | 类型 | 含义 |
|---|---|---|
| `buff_id` | int | 外键 |
| `event` | enum | BuffEvent 枚举 |
| `handler_ref` | string | 指向 Action/DSL 片段的 ID |
| `condition_dsl_ref` | string | 可选额外条件 |
| `filter` | enum | HandlerFilter（如 OnDamageTaken_MagicOnly） |
| `priority` | int | 同事件多 handler 的触发顺序 |

示例（燃烧 DoT）：
```
buff_id | event       | handler_ref          | condition_dsl_ref    | filter | priority
2001    | OnTick      | burn_damage_formula  |                      |        | 0
2001    | OnApply     | vfx_burn_apply       |                      |        | 0
2001    | OnRemove    | vfx_burn_fade        |                      |        | 0
2001    | OnHurt      | extinguish_if_water  | target.in_water      |        | 10
```

### 10.5 校验规则（Build pipeline）

- 所有 `handler_ref` 必须存在
- `stack_mode = StackIndependent` 时 `max_stacks` 忽略（告警）
- `tick_interval_ms > 0` 时必须有 OnTick handler（否则告警）
- `per_stack = true` 时 `stack_mode` 不能是 StackIndependent（语义冲突，error）
- 循环依赖检测：buff A 的 handler 直接 apply buff B，buff B 的 handler 直接 apply buff A → warning

---

## 11. 与其他系统集成

### 11.1 Skill 系统

**Skill → Buff**：
- `ApplyBuff(buff_id, target_filter, duration_ms, stacks)` action
- `RemoveBuff(buff_id, target_filter)` action
- `DispelBuff(dispel_type_mask, target_filter)` action（驱散）

**Buff → Skill**：
- Handler 中 `SpawnSubSkill(skill_id, caster, target, position)` action
- 例：燃烧 buff 的 OnTick 触发子技能"火焰爆裂"

### 11.2 Damage 系统

Damage pipeline 查询 Target 的 modifier：
- `defense` 影响减伤
- `damage_taken_mul` 影响最终伤害
- `flag_iframe` 时整个 damage 被忽略

Damage pipeline 触发 buff 事件：
- 计算前：OnDamageDealt（attacker 的 buff）→ 可能修改 damage
- 应用后：OnDamageTaken（victim 的 buff）→ 可能反伤、吸血等
- 暴击时：OnCrit / OnCritTaken

### 11.3 CC（控制）系统

控制效果（stun/root/silence/fear）都是 buff：
```
Stun buff:
  modifiers: [Flag_Stunned = 1]
  duration_ms: 3000

实体行动前检查: if (stat.Get(Flag_Stunned) > 0) { suppress action }
```

Iframe 同理：
```
Iframe buff:
  modifiers: [Flag_Iframe = 1]
  duration_ms: 500

Damage pipeline: if (victim.stat.Get(Flag_Iframe) > 0) { damage = 0; return; }
```

### 11.4 装备系统

装备 = 永久 buff：
```
穿装备时: ApplyBuff(equipment_buff_id, self, self, duration_ms=-1)
脱装备时: RemoveBuff(equipment_buff_id, self)
```

装备 buff 特点：
- `DurationMs = -1`
- `StackMode = StackIndependent`（多件装备可叠）
- `Category = System`
- `IsHidden = true`（不在战斗 UI 显示，但可在装备面板查看）

### 11.5 AI 系统

AI 决策考虑自身 buff 状态：
- 检查 `Flag_Stunned`、`Flag_Silenced`：技能无法释放
- 检查自身 HP 低时激活"撤退"行为树分支
- Boss 可根据 Debuff 层数调整行为（"被冰冻 3 层时释放狂暴"）

### 11.6 动画系统（Unity 客户端）

Buff 的视觉通过 OnApply/OnRemove handler 中的 `PlayVFX`、`SetAnimParam` action 驱动：
```
OnApply: PlayVFX("burn_loop", attach: chest)
         SetAnimParam("is_burning", true)
OnRemove: PlayVFX("burn_fade", attach: chest)
          SetAnimParam("is_burning", false)
```

**不通过 Animator state** 驱动 buff 逻辑——动画只是"表现"。

### 11.7 UI

UI 订阅 BuffSystem 事件：
- `BuffApplied` → 增加图标
- `BuffRemoved` → 移除图标
- `BuffStackChanged` → 更新数字
- `BuffRefreshed` → 刷新计时条
- `BuffTick` → 可选的小动画（如 DoT 跳伤害时图标闪烁）

---

## 12. 客户端同步

### 12.1 同步策略：事件驱动

Buff 状态**不进每帧快照**（见 `OVERVIEW.md §6.2` 三通道）——走独立事件通道：

```
可靠事件（Channel 2）:
  BuffApplied { entity_id, buff_id, instance_id, source, duration_ms, stacks, apply_tick }
  BuffRemoved { entity_id, instance_id, reason }
  BuffStackChanged { entity_id, instance_id, new_stacks }
  BuffRefreshed { entity_id, instance_id, new_expire_tick }
```

**不同步**：
- Modifier 详情（客户端读 BuffDef 自行计算）
- Handler 内部状态（服务端权威）

### 12.2 客户端本地 BuffInstance

客户端有**只读副本**：
```csharp
public sealed class ClientBuffInstance {
  public long InstanceId;
  public int  BuffId;
  public long ApplyTick;
  public long ExpireTick;
  public int  Stacks;
  
  // 客户端用于 UI 倒计时、VFX 追踪
  public GameObject VfxInstance;
}
```

客户端按收到的事件更新副本；VFX/UI 由本地维护。

### 12.3 预测

**只预测自施 buff**（玩家技能里的 ApplyBuff 作用于自己）：
- 客户端立即创建临时 BuffInstance，播放 OnApply 的 VFX
- 服务端 ack 后确认或修正
- 他人施加的 buff（被打中中毒、友方给护盾）等服务端推送

**不预测其他 buff**：预测复杂度远大于 50ms 延迟的用户可感度。

### 12.4 时间校准

客户端 buff UI 显示的剩余时长 = `(ExpireTick - client_estimated_server_tick) × tick_ms`。

`client_estimated_server_tick` 由时间同步协议维护（见 `NETWORK_PROTOCOL.md`），每 2 秒一次校准样本，EMA 平滑。

---

## 13. 性能考量

### 13.1 规模预算

- 单 cell 最大 BuffInstance 数：10000（400 实体 × 25 平均）
- 单 tick BuffSystem.Tick 耗时：**≤ 3 ms**
- 单事件派发耗时（含级联）：**≤ 100 μs 平均，1 ms p99**

### 13.2 优化策略

**数据布局**：
- `_instances` 用 `List<BuffInstance>`，连续内存，缓存友好
- `_byTarget` 索引加速 "某实体的所有 buff" 查询
- 热字段（`ExpireTick`, `NextTickTick`, `Stacks`）集中放

**过期扫描**：
- 每 tick 扫过期可用 priority queue 优化到 O(k + log n)（k = 本 tick 过期数）
- 但实测直接线性扫 10000 条目 < 500 μs，不必过早优化

**Handler 触发**：
- Handler 按 BuffEvent 分桶（`Dictionary<BuffEvent, List<(BuffInstance, HandlerRef)>>`）
- 事件发生时直接查桶，不遍历所有 buff

**StatCache**：
- Dirty flag 驱动，避免每帧重算
- 极少 miss（大多数 tick 没 buff 变化）

**GC 压力**：
- BuffInstance 用对象池（ReturnToPool 替代 `new`）
- Handler context 用 struct，栈分配

### 13.3 监控指标

- 每 cell BuffInstance 总数（峰值、平均）
- BuffSystem.Tick 耗时分布
- 每种 BuffEvent 触发频率
- 级联超限统计（见 §9.5）
- StatCache 命中率

---

## 14. FAQ 与反模式

### Q1: 吟唱/充能类看似 timeline，真的用 buff 表达？

是。典型例子：

**吟唱（3 秒间歇性生成弹幕，结束时爆炸）**：
```
buff "channel_bolt":
  duration_ms: 3000
  tick_interval_ms: 500
  on_apply:   PlayVFX("channel_loop"); LockMovement()
  on_tick:    SpawnProjectile(bolt)
  on_expire:  SpawnAoE(explosion); UnlockMovement()
  on_remove:  UnlockMovement(); StopVFX("channel_loop")  ← 被打断时不爆炸
```

比 timeline 表达清晰——"被打断不爆炸"是事件驱动天然支持。

### Q2: 某效果既需要 buff 又需要 skill timeline，怎么拆？

判断标准：
- 有**帧精度要求**（如某帧生成 hitbox）→ 放技能 timeline
- 无帧精度，只响应事件 → 放 buff

常见分工：
- 技能释放时的爆发伤害 → 技能 timeline
- 后续的 DoT / 减速 → 技能中 `ApplyBuff` 施加 buff

### Q3: Buff 能改 Buff 吗（例如一个 Buff 延长其他 Buff 的持续时间）？

可以，通过 Action：
```
buff "time_warp":
  on_apply:
    ExtendBuffDuration(target, all_beneficial, by_ms=5000)
```

`ExtendBuffDuration` 是 Action 全集里的一种，调用 `BuffSystem.Extend`。

**限制**：不能在 OnTick 里修改自己的 duration（会造成无限延长）。CI 检查 handler 对自身的引用。

### Q4: 客户端为什么不预测"中毒 DoT"的伤害数字显示？

**可以显示 "中毒中" 的图标**，但**不预测伤害数字**：
- 伤害计算可能被服务端修正（attacker 的其他 modifier、victim 的 iframe）
- 预测失误后回滚伤害数字是糟糕体验
- 延迟 50ms 显示伤害数字玩家可接受

### Q5: StackIndependent 的多个实例叠 Modifier 不会爆炸吗？

会随实例数线性增长，但实际场景有限（多毒源 PK 最多 3–4 层）。若设计上需要极多实例（如"每被击一次加 1 层"），应改用 `RefreshStack + MaxStacks`。

### Q6: Buff 能跨 session / 重登陆保留吗？

分三类：
- **战斗内临时 buff**（攻击+30% 持续 30s）：下线即清除
- **半永久 buff**（副本内开局 buff，持续整个副本）：随 session 销毁
- **装备/天赋 buff**：脱离后重新施加（这类 `DurationMs=-1` 由装备/天赋系统管理）

这些区别写在 BuffDef 的 `persist_scope` 字段（Session/Permanent/Temp）。

### Q7: 两个 buff 的 Modifier 冲突（一个加 30% 速，一个减 50% 速），怎么处理？

**都生效，按公式叠加**：
```
MulAdditive_factor = 1 + 0.30 + (-0.50) = 0.80
```
速度变为 base × 80%。

**不是**"谁施加的后谁覆盖"——那是 StackMode 的事，Modifier 层面不做覆盖语义（除 `Set` 特殊）。

### Q8: 如果 buff 的 DSL 公式因为参数变化需要热更，怎么办？

**重新加载 BuffDef**：
- 已存在的 BuffInstance 仍持有旧 Def 引用（不改变已生效结果的稳定性）
- 新施加的按新 Def
- 允许配置 `hot_reload_strategy`：`KeepOld` 或 `UpgradeAll`
- 生产环境倾向 `KeepOld`，热补丁期间并存

### Q9: OnDeath 的 Target 已经死了，还能触发吗？

**能**。死亡不等于实体立即销毁——BuffSystem 先触发 OnDeath handler 后再清理：
```
实体 HP 降为 0 → 标记死亡
  → 触发 Target 身上所有 OnDeath handler
  → 触发 Attacker 身上所有 OnKill handler
  → 清除 !PersistOnDeath 的 buff
  → Entity 进入 dead state
```

OnDeath handler 常用于"死亡爆炸"、"最后一击给盟友 buff"等。

### Q10: PvP 中敌对 buff 的反馈如何？

与 PvE 区别不大，但有两点强化：
- **UI 明显**：敌对 CC/debuff 必须在屏幕中心提示（不是角色头顶）
- **网络优先级**：CC 事件走事件通道最高优先级，避免"被眩晕但客户端不知道"

### 反模式清单

- ❌ 在 buff handler 里做复杂计算后"改 attack_power 一次"（应用 Modifier）
- ❌ Buff 里套 Timeline（用 duration + OnTick + OnExpire）
- ❌ 用大量 `StackIndependent` 代替层数机制（性能浪费）
- ❌ 在 OnTick 里修改自身 duration（无限延长）
- ❌ 依赖 buff 触发顺序（跨实例的事件顺序由 `COMBAT_EVENT_ORDERING` 裁决）
- ❌ 用 buff 表达"一次性即时效果"（那是 skill action 的事）
- ❌ 客户端预测他人施加的 buff（复杂度高，不偿失）
- ❌ 把 UI 显示逻辑放进 buff handler（UI 应订阅事件，不反向驱动）

---

## 15. 里程碑

| 阶段 | 交付 |
|---|---|
| P0 末 | BuffDef / BuffInstance 数据结构；StatCache 骨架 |
| P1 中 | Modifier 系统完整；基础 buff 生效（+atk、+speed） |
| P2 初 | Handler 系统；OnApply/OnRemove/OnTick 生效 |
| P2 中 | 叠层语义（6 种 StackMode）完整；级联控制 |
| P2 末 | 20+ 测试 buff（DoT、CC、Aura、反伤、吸血...）；端同测试 |
| P3 | 配合手感：CC 命中反馈、buff 图标 UI |
| P4+ | 装备 buff 集成；天赋 buff 集成；boss buff 脚本化 |

---

## 16. 文档维护

- **Owner**：Tech Lead + 数值策划 Lead
- **评审频率**：新增 BuffEvent、新增 StatType、StackMode 语义变更时
- **关联文档**：
  - `SKILL_SYSTEM.md`（§5 共享 Action 层）
  - `STAT_AND_DAMAGE.md`（Modifier 详细计算）
  - `COMBAT_EVENT_ORDERING.md`（事件派发顺序）
  - `HIT_VALIDATION.md`（Iframe 检查）
  - `COMBAT_FEEL.md`（CC 反馈视觉）
  - `DETERMINISM_CONTRACT.md`（所有 handler 执行必须遵循）

---

**文档结束。**

**核心纪律重申**：
1. **Buff 是状态不是流程**：持续效果用 Modifier + Handler 表达，不借 Timeline
2. **共享 Action 层**：别为 buff 另造一套执行器
3. **级联必须有界**：4 层 / 50 个 handler / 同 tick 内完成
4. **StatCache 是单一真源**：不要绕过直接访问 buff modifier
5. **客户端轻量**：只存状态副本，逻辑全在服务端
