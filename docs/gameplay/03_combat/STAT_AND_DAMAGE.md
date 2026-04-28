# 属性与伤害系统（Stat & Damage）

> **用途**：定义 Atlas 实体的属性体系（HP/MP/攻击/防御/速度等）、modifier 聚合规则、StatCache 实现，以及命中确认后的伤害结算 pipeline。
>
> **读者**：工程（必读）、数值策划（必读）、战斗策划（§5、§6 必读）。
>
> **状态**：草案 v0.1 — 待团队评审。
>
> **前置文档**：`OVERVIEW.md`、`BUFF_SYSTEM.md`、`HIT_VALIDATION.md`、`COMBAT_ACTIONS.md`、`DETERMINISM_CONTRACT.md`
>
> **关联文档**：`SKILL_SYSTEM.md`（DSL 引用属性）、`COMBAT_EVENT_ORDERING.md`（伤害事件顺序）

---

## 1. 设计目标与边界

### 1.1 目标

1. **统一属性数据源**：装备、等级、buff、被动技能都通过 modifier 注入同一个 StatCache
2. **可读公式**：策划用 DSL 表达伤害公式，端共享，端同求值
3. **可解释的伤害**：每次伤害可拆解（base / crit / def / res / final），便于平衡与 debug
4. **支持复杂交互**：DoT、护盾、反伤、吸血等通过 buff handler + ModifyDamage 实现
5. **性能可扩展**：5000 实体 × 平均 25 modifier，StatCache 命中查询 < 10 ns
6. **端同确定性**：相同 modifier + 相同伤害输入 → 相同最终伤害

### 1.2 非目标

- **不做实时连续效果**（光环减伤、领域 buff）的特殊算法——通过普通 buff 即可
- **不内置元素相克专用引擎**——元素加成是普通 modifier，不是独立机制
- **不暴露给玩家自由组合公式**——伤害公式策划权威，玩家通过装备/技能选择影响参数

---

## 2. StatType 全集

### 2.1 资源类（玩家可见）

```cpp
enum class StatType : uint16_t {
  // 资源
  CurrentHp = 0,       // 当前 HP
  MaxHp,               // 上限 HP
  CurrentMp,
  MaxMp,
  CurrentStamina,
  MaxStamina,
  
  // 攻击输出
  AtkPower,            // 物理攻击
  MagPower,            // 魔法攻击
  AtkSpeed,            // 攻击速度倍率
  CritChance,          // 暴击率 [0, 1]
  CritDamage,          // 暴击倍率（如 1.5 = +50%）
  HitRate,             // 命中率（PvE 高级怪用，PvP 通常忽略）
  ArmorPenetration,    // 穿甲（百分比 [0, 1]）
  MagPenetration,
  
  // 防御
  Defense,             // 物理防御
  MagResist,           // 魔法抗性
  EvasionRate,         // 闪避率 [0, 1]
  BlockRate,           // 格挡率 [0, 1]
  BlockReduction,      // 格挡时减伤百分比 [0, 1]
  ParryWindow,         // 招架窗口（毫秒）
  
  // 元素抗性
  ResistFire,          // [0, 1]
  ResistIce,
  ResistLightning,
  ResistPoison,
  ResistDark,
  ResistHoly,
  
  // 受伤倍率（最终乘算）
  DamageTakenMul_All,
  DamageTakenMul_Physical,
  DamageTakenMul_Magic,
  DamageTakenMul_Crit,         // 暴击受到的伤害倍率
  DamageDealtMul_All,
  DamageDealtMul_Physical,
  DamageDealtMul_Magic,
  
  // 移动
  MoveSpeed,           // 米/秒
  JumpHeight,
  DashCooldownReduction,
  
  // 技能
  CooldownReduction,   // 冷却缩短倍率 [0, 1]
  CastSpeed,           // 施法速度倍率
  ResourceCostReduction,
  
  // 回复
  HpRegen,             // 每秒
  MpRegen,
  StaminaRegen,
  
  // ===== Flag 类（用 Set Op + 0/1 模拟 bool）=====
  Flag_SuperArmor,
  Flag_Iframe,
  Flag_Stunned,
  Flag_Silenced,
  Flag_Rooted,
  Flag_Blocking,
  Flag_Channeling,
  Flag_Bleeding,
  Flag_Burning,
  Flag_Frozen,
  Flag_Airborne,
  Flag_KnockedDown,
  Flag_Invisible,
  Flag_Untargetable,
  // ... 扩展
  
  Count
};
```

约 **60+ 类型**，预留扩展空间。

### 2.2 Flag 类的特殊性

Flag 类型用 `ModOp.Set` 设值，多 buff 同时影响时 OR 语义：
- 任一 modifier 设为 1 → flag 为 1
- 全部 modifier 设为 0（或无 modifier） → flag 为 0

实现：Set Op for Flag 用 max 而非真正 set：
```csharp
if (stat.IsFlag()) {
  flag_value = max(flag_value, mod.Value);  // 0 or 1
} else {
  // 普通 Set 行为
}
```

### 2.3 隐藏类（系统使用）

不向玩家展示但参与计算：
- `EffectiveLevel`（用于伤害公式中的等级因子）
- `ArmorClass`（决定防御曲线选择）
- `RaceTag`（用于种族克制 modifier）

---

## 3. Modifier 数据结构

### 3.1 ModDef

```csharp
public sealed class StatModDef {
  public StatType Stat;        // 目标属性
  public ModOp    Op;          // 运算类型
  public float    Value;       // 数值
  public bool     PerStack;    // 是否按 buff 层数缩放
  public int      Priority;    // 同 Op 内计算顺序
}

public enum ModOp : byte {
  Add,                 // += value
  MulAdditive,         // 加性百分比：Σ 后乘 base
  MulMultiplicative,   // 乘性百分比：连乘
  Set,                 // 强制赋值
  Cap,                 // 上限钳制
  Floor,               // 下限钳制
}
```

### 3.2 Modifier 来源

| 来源 | 例 |
|---|---|
| 基础属性 | 角色 base HP = level × 100 |
| 等级成长 | `+10 atk_power per level` |
| 装备 | 武器 +20 atk |
| 装备词条 | "暴击 +5%" |
| 天赋 | "+10% 火伤" |
| 套装效果 | "4 件套：crit_chance +15%" |
| 技能被动 | "战吼 buff 期间 atk +30%" |
| 副本 buff | "副本进入：MP +50%" |
| 状态 | "中毒：DamageTakenMul +20%" |

所有来源**统一通过 buff** 进入 StatCache：
- 等级成长 = 永久 buff（`DurationMs=-1`）
- 装备穿戴 = 装备触发的 buff
- 技能 buff = 临时 buff
- 等等

详见 `BUFF_SYSTEM.md §11.4`。

### 3.3 PerStack 缩放

```csharp
float effective_value = mod.PerStack ? mod.Value * buff_instance.Stacks : mod.Value;
```

举例：燃烧 buff，DamageTaken_Fire +5%/层，3 层时 → +15%。

### 3.4 Priority 用途

同 Op 内多个 modifier，按 priority 升序应用：
- 适用于 `Set / Cap / Floor`（顺序敏感）
- `Add / MulAdditive / MulMultiplicative` 内部不敏感（满足结合律）

---

## 4. StatCache 实现

### 4.1 数据结构

```csharp
public sealed class StatCache {
  EntityRef _owner;
  bool _dirty = true;
  float[] _values = new float[(int)StatType.Count];
  
  // 当前实体的所有 modifier 提供者（按权重存储）
  List<IModifierProvider> _providers;
}

public interface IModifierProvider {
  IEnumerable<StatModDef> GetModifiers();
  bool IsActive { get; }
}
```

### 4.2 查询接口

```csharp
public float Get(StatType stat) {
  if (_dirty) Recompute();
  return _values[(int)stat];
}

public bool GetFlag(StatType flag) {
  return Get(flag) > 0.5f;   // float → bool
}
```

**性能保证**：命中缓存 < 10 ns（一次数组访问）。

### 4.3 失效（Invalidate）

```csharp
public void MarkDirty() {
  _dirty = true;
}
```

调用时机：
- BuffInstance Apply / Remove / Stacks 变化
- 装备变化
- 等级提升
- 玩家死亡（基础属性变化）

**关键**：不在每帧调用，仅在事件发生时。

### 4.4 Recompute

```csharp
void Recompute() {
  Array.Clear(_values, 0, _values.Length);
  
  // 先把基础属性写入
  ApplyBase(_owner.Class, _owner.Level, _values);
  
  // 收集所有 active modifier
  var allMods = new List<(StatModDef, int Stacks)>();  // (mod, stacks for PerStack)
  foreach (var p in _providers) {
    if (!p.IsActive) continue;
    int stacks = (p as BuffInstance)?.Stacks ?? 1;
    foreach (var mod in p.GetModifiers()) {
      allMods.Add((mod, stacks));
    }
  }
  
  // 按 stat 分桶
  var bucketsAdd = new Dictionary<StatType, float>();
  var bucketsMulAdd = new Dictionary<StatType, float>();
  var bucketsMulMul = new Dictionary<StatType, float>();
  var bucketsSet = new Dictionary<StatType, List<(float val, int prio)>>();
  var bucketsCap = new Dictionary<StatType, List<(float val, int prio)>>();
  var bucketsFloor = new Dictionary<StatType, List<(float val, int prio)>>();
  
  foreach (var (mod, stacks) in allMods) {
    float v = mod.PerStack ? mod.Value * stacks : mod.Value;
    switch (mod.Op) {
      case ModOp.Add:               bucketsAdd[mod.Stat] = bucketsAdd.GetOrDefault(mod.Stat) + v; break;
      case ModOp.MulAdditive:       bucketsMulAdd[mod.Stat] = bucketsMulAdd.GetOrDefault(mod.Stat) + v; break;
      case ModOp.MulMultiplicative: bucketsMulMul[mod.Stat] = bucketsMulMul.GetOrDefault(mod.Stat, 1.0f) * (1 + v); break;
      // ... Set/Cap/Floor 进 list
    }
  }
  
  // 按公式计算每个 stat
  for (int i = 0; i < (int)StatType.Count; i++) {
    var stat = (StatType)i;
    float base_v = _values[i];                                    // base 值已经在 ApplyBase 写入
    float add = bucketsAdd.GetOrDefault(stat);
    float mul_add_factor = 1 + bucketsMulAdd.GetOrDefault(stat);
    float mul_mul_factor = bucketsMulMul.GetOrDefault(stat, 1.0f);
    
    float final_v = (base_v + add) * mul_add_factor * mul_mul_factor;
    
    // Set/Cap/Floor 按 priority 升序应用
    if (bucketsSet.TryGetValue(stat, out var setList)) {
      foreach (var (val, prio) in setList.OrderBy(s => s.prio)) {
        final_v = val;
      }
    }
    if (bucketsCap.TryGetValue(stat, out var capList)) {
      foreach (var (val, prio) in capList.OrderBy(s => s.prio)) {
        final_v = Math.Min(final_v, val);
      }
    }
    if (bucketsFloor.TryGetValue(stat, out var floorList)) {
      foreach (var (val, prio) in floorList.OrderBy(s => s.prio)) {
        final_v = Math.Max(final_v, val);
      }
    }
    
    _values[i] = final_v;
  }
  
  _dirty = false;
}
```

### 4.5 性能优化

**初始版本**：每次 `Recompute` 重算全部 60+ stat。
- 单次耗时 ~10 μs（30 modifier × 60 stat）
- 实测：每秒触发 < 5 次（远低于每秒一次/per entity）
- 总耗时 < 50 μs/s/entity，可接受

**后期优化（如需）**：
- **Per-stat dirty**：只重算受影响的 stat（modifier 注册时声明影响的 stat 集合）
- **增量更新**：buff apply 时计算 delta，叠加到 cache（无需全部重算）

P0–P2 用初始版本，P4 视性能压测决定是否优化。

---

## 5. 伤害结算 Pipeline

### 5.1 触发源

伤害由以下方式触发：
- Hitbox 命中（参见 `HIT_VALIDATION.md`）
- `DamageTarget` action 直接触发（DoT、反伤）
- 环境（陷阱、毒气）

所有路径汇聚到统一 pipeline。

### 5.2 Pipeline 步骤

```
┌────────────────────────────────────────────────────────────┐
│  Step 0: 命中已确认（hit_validation 通过）                  │
│           已检查 iframe / block / 完美闪避                   │
├────────────────────────────────────────────────────────────┤
│  Step 1: 计算 Base Damage                                  │
│           base = ExecuteDsl(damage_dsl_ref, ctx)            │
│           典型：base = caster.atk × skill_coef              │
├────────────────────────────────────────────────────────────┤
│  Step 2: Crit Roll                                          │
│           is_crit = rand_bool(caster.crit_chance)           │
│           dmg_crit = base × (is_crit ? caster.crit_dmg : 1) │
├────────────────────────────────────────────────────────────┤
│  Step 3: 触发 OnDamageDealt（attacker buff handlers）       │
│           可 emit ModifyDamage(Add/Mul, value)              │
│           可 emit ApplyBuff（如打击同时上 debuff）           │
├────────────────────────────────────────────────────────────┤
│  Step 4: Defense / Resistance                              │
│           dmg_after_def = ApplyDefense(dmg, defense, type) │
│           dmg_after_res = dmg_after_def × (1 - res)         │
├────────────────────────────────────────────────────────────┤
│  Step 5: 触发 OnDamageTaken（victim buff handlers）         │
│           护盾消耗、反伤、吸血等                             │
│           可 emit ModifyDamage / SpawnSubSkill              │
├────────────────────────────────────────────────────────────┤
│  Step 6: DamageTakenMul 聚合                                │
│           dmg_final = dmg × victim.damage_taken_mul_all     │
│                          × victim.damage_taken_mul_<type>   │
├────────────────────────────────────────────────────────────┤
│  Step 7: Floor                                              │
│           dmg_final = max(1, dmg_final)                     │
│           （除非 base = 0，否则保证至少 1 伤害）              │
├────────────────────────────────────────────────────────────┤
│  Step 8: 应用                                                │
│           victim.hp -= dmg_final                            │
│           记录到 DamageEvent（用于反馈）                     │
├────────────────────────────────────────────────────────────┤
│  Step 9: 后续触发                                            │
│           if victim.hp <= 0:                                │
│             TriggerDeath(victim)                            │
│             TriggerOnKill(attacker)                         │
│           TriggerOnHit(attacker)                            │
│           TriggerOnHurt(victim)                             │
└────────────────────────────────────────────────────────────┘
```

### 5.3 Code Skeleton

```csharp
public DamageResult ResolveDamage(in DamageRequest req, CombatContext ctx) {
  var result = new DamageResult { 
    Attacker = req.Attacker, 
    Victim = req.Victim, 
    DamageType = req.Type 
  };
  
  // Step 1: base
  result.BaseDamage = ExecuteDamageDsl(req.DamageDslRef, req.Context);
  
  // Step 2: crit
  var attackerStats = req.Attacker.Stats;
  if (req.CanCrit) {
    var rng = ctx.RngFor(req.Attacker, RandomPurpose.Crit);
    result.IsCrit = rng.NextFloat() < attackerStats.Get(StatType.CritChance);
  }
  result.DamageAfterCrit = result.IsCrit 
    ? result.BaseDamage * attackerStats.Get(StatType.CritDamage) 
    : result.BaseDamage;
  
  // Step 3: OnDamageDealt
  var modifyContext = new DamageModifyContext(result);
  CombatEvents.Emit(BuffEvent.OnDamageDealt, req.Attacker, modifyContext);
  result.ApplyModify(modifyContext);
  
  // Step 4: Defense + Resistance
  var victimStats = req.Victim.Stats;
  result.DamageAfterDefense = ApplyDefense(result.DamageAfterCrit, 
                                            attackerStats, victimStats, 
                                            req.Type);
  result.DamageAfterResist = ApplyResistance(result.DamageAfterDefense,
                                              victimStats, req.Type);
  
  // Step 5: OnDamageTaken
  CombatEvents.Emit(BuffEvent.OnDamageTaken, req.Victim, modifyContext);
  result.ApplyModify(modifyContext);
  
  // Step 6: DamageTakenMul
  result.DamageFinal = result.DamageAfterResist 
    * victimStats.Get(StatType.DamageTakenMul_All)
    * GetTypeSpecificMul(victimStats, req.Type);
  
  // Step 7: Floor
  result.DamageFinal = Math.Max(1, result.DamageFinal);
  if (req.Type == DamageType.Pure) result.DamageFinal = result.BaseDamage;  // 纯粹伤害绕过一切
  
  // Step 8: Apply
  req.Victim.Hp -= result.DamageFinal;
  
  // Step 9: 后续触发（异步进入下一帧的事件队列）
  if (req.Victim.Hp <= 0) {
    QueueEvent(new DeathEvent(req.Victim, req.Attacker));
  }
  QueueEvent(new HitEvent(req.Attacker, req.Victim, result));
  
  return result;
}
```

### 5.4 ModifyDamage 实现

OnDamageDealt / OnDamageTaken handler 中可 `emit ModifyDamage(Add, 100)` 或 `emit ModifyDamage(Mul, 1.5)`。

实现：在 `DamageModifyContext` 中累积修改：

```csharp
public sealed class DamageModifyContext {
  public float CurrentDamage;
  public List<DamageModification> Modifications = new();
  
  public void AddModification(ModOp op, float value) {
    Modifications.Add(new DamageModification(op, value));
  }
  
  public float ApplyAll() {
    float result = CurrentDamage;
    foreach (var mod in Modifications) {
      switch (mod.Op) {
        case ModOp.Add: result += mod.Value; break;
        case ModOp.Mul: result *= mod.Value; break;
      }
    }
    return result;
  }
}
```

handlers 不直接修改 `result.DamageFinal`，而是声明修改意图，pipeline 统一应用。

---

## 6. 伤害类型（DamageType）

### 6.1 枚举

```cpp
enum class DamageType : uint8_t {
  Physical,        // 主流物理（剑、拳、箭）
  Magic,           // 主流魔法
  Fire,            // 火元素
  Ice,
  Lightning,
  Poison,
  Dark,
  Holy,
  True,            // 绕过 Defense / MagResist，但仍受 DamageTakenMul 影响
  Pure,            // 绕过一切（如系统罚伤）—— 极少用
  Bleed,           // 物理 DoT 子类型（穿甲特性）
};
```

### 6.2 Defense 公式

不同 DamageType 用不同减伤公式：

```csharp
float ApplyDefense(float damage, StatCache atk, StatCache vic, DamageType type) {
  switch (type) {
    case DamageType.Physical:
    case DamageType.Bleed:
      return DefenseFormula(damage, atk.Get(StatType.ArmorPenetration), 
                                     vic.Get(StatType.Defense));
    case DamageType.Magic:
    case DamageType.Fire: case DamageType.Ice: 
    case DamageType.Lightning: case DamageType.Poison:
    case DamageType.Dark: case DamageType.Holy:
      return DefenseFormula(damage, atk.Get(StatType.MagPenetration),
                                     vic.Get(StatType.MagResist));
    case DamageType.True:
    case DamageType.Pure:
      return damage;  // 不减
  }
}

float DefenseFormula(float damage, float penetration, float defense) {
  // 经典公式：def_factor = def / (def + K)，K 为常数（高级别衰减）
  const float K = 1000f;
  float effective_def = Math.Max(0, defense * (1 - penetration));
  float def_factor = effective_def / (effective_def + K);
  return damage * (1 - def_factor);
}
```

**`K=1000` 平衡含义**：
- def = 0 → 0% 减伤
- def = K → 50% 减伤
- def = 3K → 75% 减伤
- 接近上限渐缓（避免无敌）

### 6.3 元素抗性（Resistance）

针对 Fire/Ice/Lightning 等元素伤害额外检查 `ResistFire/Ice/...`：

```csharp
float ApplyResistance(float damage, StatCache vic, DamageType type) {
  StatType resistStat = type switch {
    DamageType.Fire => StatType.ResistFire,
    DamageType.Ice => StatType.ResistIce,
    DamageType.Lightning => StatType.ResistLightning,
    DamageType.Poison => StatType.ResistPoison,
    DamageType.Dark => StatType.ResistDark,
    DamageType.Holy => StatType.ResistHoly,
    _ => StatType.Count,
  };
  if (resistStat == StatType.Count) return damage;
  
  float resist = Math.Clamp(vic.Get(resistStat), -1.0f, 1.0f);  // [-100%, +100%]
  return damage * (1 - resist);
}
```

`-1.0` 抗性表示**易伤 100%**（双倍伤害），用于"暴露弱点" debuff。

---

## 7. 暴击、闪避、格挡（Crit / Evasion / Block）

### 7.1 顺序

按命中事件的处理流：

```
HitValidation 已通过 (hitbox 形状重叠 + iframe 检查)
  ↓
Block 检查（在 HIT_VALIDATION.md §6 处理，伤害 pipeline 不重复检）
  ↓
Damage Pipeline 进入：
  ├─ Step 2: Crit Roll（这里）
  └─ ...
```

### 7.2 Crit Roll

```csharp
float crit_chance = caster.Stats.Get(StatType.CritChance);
crit_chance = Math.Clamp(crit_chance, 0, 1);

var rng = ctx.RngFor(caster, RandomPurpose.Crit, hit_event_seq);
bool is_crit = rng.NextFloat() < crit_chance;
```

**RNG 种子**包含 `hit_event_seq`，保证多次随机不同结果（同 tick 多命中场景）。

### 7.3 Crit Damage

```csharp
if (is_crit) {
  damage *= caster.Stats.Get(StatType.CritDamage);  // 通常 1.5–2.5
  
  // 还可以触发"受暴击额外受伤" buff
  damage *= victim.Stats.Get(StatType.DamageTakenMul_Crit);
}
```

### 7.4 Evasion（闪避）—— 通常 PvE only

PvP 中**不用闪避率**（会让玩家"看到攻击命中却 miss"，体验差）。

**PvE**：
- BOSS 可能有"攻击 X% 几率被怪物闪避"
- 在 hit pipeline Step 0.5 加入：
  ```csharp
  float evasion = victim.Stats.Get(StatType.EvasionRate);
  if (rand_bool(evasion)) {
    EmitEvent(EventType.Evaded);
    return;  // 完全 miss
  }
  ```

**PvP 配置标志**：`disable_evasion_in_pvp = true`，arena 内忽略。

### 7.5 Block（格挡）

参见 `HIT_VALIDATION.md §6.4–§6.5`。在 hit pipeline 之前处理，blocked 时不进伤害 pipeline 或带 `ReducedDamageMul` 进 pipeline（看设计）。

### 7.6 Critical Hit Cap

防止暴击率过高破坏平衡：
- 通过 `Cap` modifier：装备给 base = 5%，buff 临时 +30% → cap 在 80%
- 实现：注册一个永久 `Cap modifier` 在 BaseStats provider 里

---

## 8. DoT、HoT、护盾

### 8.1 DoT（Damage over Time）

实现方式：buff + OnTick handler

```
buff "burn":
  duration_ms: 6000
  tick_interval_ms: 500
  on_tick: [DamageTarget(self, dsl="0.2 * source.atk_power", type=Fire)]
```

每 500ms 触发一次 `DamageTarget` action，走完整 damage pipeline（包括元素抗性）。

**source 引用**：DoT 的伤害公式可引用 `source.atk_power`，即施加 buff 的实体的 atk。需要 BuffInstance 持有 source 引用（参见 `BUFF_SYSTEM.md §3.2`）。

**DoT 标志**：`is_dot: true`——某些反应式 buff 可识别"这是 DoT"做不同处理（"不被 DoT 杀死"机制）。

### 8.2 HoT（Heal over Time）

类似 DoT，用 `HealTarget` action：

```
buff "regen":
  duration_ms: 10000
  tick_interval_ms: 1000
  on_tick: [HealTarget(self, amount=100)]
```

`HealTarget` 走简化 pipeline（无 def/res，但有 `HealTakenMul` modifier）：
```csharp
float final = base * victim.Stats.Get(StatType.HealTakenMul);
victim.Hp = Math.Min(victim.Hp + final, victim.Stats.Get(StatType.MaxHp));
```

### 8.3 护盾（Shield）

护盾 = 在 HP 之外的"伤害缓冲"。实现为特殊 buff：

```
buff "magic_shield":
  duration_ms: 10000
  modifiers: [Shield_Magic = 500]  // 抗 500 魔法伤害
  on_damage_taken (filter: type=Magic): [
    AbsorbShield(amount = damage_amount, max = 500)
  ]
```

`AbsorbShield` action：
```csharp
float absorbed = Math.Min(damage, shield_value);
damage -= absorbed;
shield_value -= absorbed;
modifyCtx.CurrentDamage = damage;

if (shield_value <= 0) {
  // 护盾破裂，移除 buff
  RemoveBuff(self, "magic_shield");
  EmitEvent(EventType.ShieldBreak);
}
```

护盾值存哪里？两种方案：
- **存 BuffInstance.Variables**："shield_value: 500"（buff 自管）
- **存 Stat（Shield_Magic）**：通过 modifier 影响

推荐**前者**——护盾是 buff 实例数据，应跟随 buff 生命周期，更清晰。

### 8.4 反伤（Reflect）

```
buff "thorns":
  on_damage_taken: [DamageTarget(event_actor, dsl="0.2 * incoming_damage", type=Physical)]
```

OnDamageTaken handler 中通过 DSL 引用 `incoming_damage`（事件 payload），对 `event_actor`（attacker）施加反伤。

**关键**：反伤本身**也走完整 pipeline**（attacker 的 def 有效）—— 这避免无限循环吗？参见级联控制（`BUFF_SYSTEM.md §9`）。

---

## 9. 治疗、复活

### 9.1 HealTarget

简化版伤害 pipeline：

```csharp
public HealResult ResolveHeal(in HealRequest req, CombatContext ctx) {
  float base = ExecuteHealDsl(req.HealDslRef, req.Context);
  
  // 治疗也有 crit
  bool is_crit = rng.NextFloat() < req.Caster.Stats.Get(StatType.CritChance);
  float after_crit = is_crit ? base * req.Caster.Stats.Get(StatType.CritDamage) : base;
  
  // OnHealGiven / OnHealReceived
  CombatEvents.Emit(BuffEvent.OnHealGiven, req.Caster);
  CombatEvents.Emit(BuffEvent.OnHealReceived, req.Target);
  
  // HealTakenMul
  float final = after_crit * req.Target.Stats.Get(StatType.HealTakenMul);
  
  // Apply
  float oldHp = req.Target.Hp;
  req.Target.Hp = Math.Min(oldHp + final, req.Target.Stats.Get(StatType.MaxHp));
  
  return new HealResult { ActualHealed = req.Target.Hp - oldHp };
}
```

### 9.2 复活

死亡后特殊机制：
- 普通：被杀后进入 `Dead` 状态，不可被治疗复活
- 复活技能：调用 `Resurrect` action（独立于 Heal）：
  ```csharp
  if (target.IsDead) {
    target.Hp = target.Stats.Get(StatType.MaxHp) * resurrect_pct;
    target.RemoveBuff(BuffId.Dead);
    target.SpawnPosition = target.LastDeathPosition;
    EmitEvent(EventType.Resurrected);
  }
  ```

### 9.3 Lifesteal（吸血）

通过 OnDamageDealt handler：
```
buff "lifesteal":
  on_damage_dealt: [HealTarget(self, dsl="incoming_damage_dealt * 0.1")]
```

吸血**也是 Heal**，受 HealTakenMul 影响。

---

## 10. 死亡处理

### 10.1 死亡触发

```csharp
if (victim.Hp <= 0 && !victim.IsDead) {
  victim.IsDead = true;
  victim.Hp = 0;
  
  // 1. 触发 victim 的 OnDeath buff handlers
  CombatEvents.Emit(BuffEvent.OnDeath, victim);
  
  // 2. 若有 OnDeath handler 治疗 victim 让其复活，跳过后续
  if (victim.Hp > 0) {
    victim.IsDead = false;
    return;
  }
  
  // 3. 触发 attacker 的 OnKill buff handlers
  CombatEvents.Emit(BuffEvent.OnKill, attacker, /* killed = */ victim);
  
  // 4. 清除 victim 的 !PersistOnDeath buff
  victim.BuffSystem.RemoveAllNonPersistent();
  
  // 5. 进入 Dead state
  victim.Animator.PlayAnim("death");
  victim.Movement.Disable();
  
  // 6. 死亡事件广播
  EmitEvent(EventType.Death { victim, attacker, kill_skill });
}
```

### 10.2 OnDeath 时机

**OnDeath handler 在标记 Dead 之后但清除 buff 之前触发**：
- handler 仍可读取 victim 身上的 buff 状态
- handler 可以 emit `HealTarget(self, ...)` 救自己
- 仅当 handler 全部执行完仍 `Hp <= 0` 才真正死亡

这支持"虚弱抗性"、"涅槃重生"、"死亡咒诅" 等机制。

### 10.3 PvP 死亡 vs PvE 死亡

PvP arena 内：
- 玩家进入 "Spectator" 状态
- 等队友复活或一局结束
- 不掉落物品

PvE 开放世界：
- 玩家死亡后选择就近复活点 / 城镇复活
- 装备耐久度损耗
- 经验损失（看设计）

死亡处理走相同 pipeline，差异在 `Resurrect` 的具体规则。

---

## 11. 事件触发（OnHit / OnHurt / OnKill）

### 11.1 时机

```
Damage pipeline Step 9（应用伤害后）：
  Emit OnHit (attacker)        → 攻击者收到事件
  Emit OnHurt (victim)         → 受击者收到事件
  if Crit:
    Emit OnCrit (attacker)
    Emit OnCritTaken (victim)
  if victim.Hp <= 0:
    Emit OnDeath (victim)
    Emit OnKill (attacker)
```

### 11.2 事件 payload

```csharp
struct HitEventPayload {
  EntityRef Attacker;
  EntityRef Victim;
  DamageType Type;
  float DamageBase;
  float DamageAfterCrit;
  float DamageFinal;
  bool IsCrit;
  bool WasBlocked;
  bool WasShielded;
  uint64 SourceSkillId;
  uint64 SourceHitboxId;
}
```

handler 通过 `event_payload.damage_final` DSL 引用使用。

### 11.3 OnHit / OnHurt 的常见用途

| 用途 | 实现方式 |
|---|---|
| 暴击吸血 | OnHit handler，filter is_crit=true |
| 受击回怒 | OnHurt handler，回复怒气资源 |
| 击杀回血 | OnKill handler，HealTarget(self, ...) |
| 击杀触发额外攻击 | OnKill handler，SpawnSubSkill |
| 受击触发护盾 | OnHurt handler，ApplyBuff(self, shield, ...) |

---

## 12. 回归测试与平衡 dashboard

### 12.1 单元测试

每个 modifier 类型 + 每个 DamageType + 每个事件触发都有单元测试：

```csharp
[Test]
public void Damage_Physical_AppliesDefense() {
  var atk = CreateEntity(atk_power: 100);
  var vic = CreateEntity(defense: 1000);  // 50% 减伤 (def=K)
  
  var result = DamageSystem.Resolve(new DamageRequest {
    Attacker = atk,
    Victim = vic,
    DamageDsl = "100",
    Type = DamageType.Physical,
    CanCrit = false,
  });
  
  Assert.AreEqual(50, result.DamageFinal, delta: 1);
}
```

### 12.2 端同测试

服务端 / 客户端各跑同一 DamageRequest 序列，结果必须一致：
- DamageBase / DamageAfterCrit / DamageFinal 完全相等
- IsCrit 一致（RNG 端同）
- 事件触发顺序一致

### 12.3 平衡 dashboard

实时统计上线后玩家：
- 各职业 DPS（伤害每秒）
- 各技能使用率
- 暴击率分布
- 死亡来源（哪些技能/buff 杀人最多）
- 平均战斗时长（PvE Boss / PvP arena）

数据驱动调整 modifier 和公式系数，不靠"感觉"。

---

## 13. 数值热更

### 13.1 修改 modifier 数值

数值策划改 Excel 中 buff modifier 的 value，run codegen，热更到服务器：
- 已存在的 BuffInstance：可选保持旧值或升级到新值（`hot_reload_strategy`）
- 新施加的：使用新值
- StatCache：所有受影响实体 mark dirty，下次 Get 重算

### 13.2 修改伤害公式 DSL

修改 `damage_dsl_ref` 的内容，重新编译 bytecode：
- 新发起的 Damage 用新公式
- 进行中的 hitbox（已 spawn）：仍用其 spawn 时的 dsl_ref（不动态 hot reload，避免战斗中半截切换）

### 13.3 上线后调整原则

不要在战斗 session 进行中改数值。最佳实践：
- 副本结束后下载新配置
- 玩家进新 session 才生效
- 长 session（开放世界）：滚动重启服务器，热更新

---

## 14. 性能预算

### 14.1 StatCache

| 操作 | 预算 |
|---|---|
| `Get(stat)` 命中缓存 | < 10 ns |
| `MarkDirty()` | < 1 ns |
| `Recompute()` | < 20 μs |
| 单 entity 平均触发 Recompute 频率 | < 5/s |
| 单 entity 总 Stat 系统耗时 | < 100 μs/s |

### 14.2 Damage Pipeline

| 操作 | 预算 |
|---|---|
| 单次 `ResolveDamage` | < 30 μs |
| OnDamageDealt / OnDamageTaken 平均 handler 数 | < 5 |
| 单次伤害事件总处理（含级联） | < 200 μs |

### 14.3 单 cell 单 tick

假设：
- 400 实体
- 平均每 tick 10 次 Damage 事件（战斗 hot zone）
- 每事件 200 μs

= 2 ms / tick (占 33ms 副本 tick 6%)，可接受。

---

## 15. FAQ 与反模式

### Q1: 为什么 Defense 用 `def / (def + K)` 而非线性减伤？

线性减伤（`damage - defense`）会让低伤害技能完全无效（一刀连皮都破不开）。
百分比公式（`def / (def + K)`）让所有伤害都有效，只是高 def 时减伤更多——更平衡。

K 值的设计：
- K = 角色满级时合理 def 的中位数
- 玩家穿基础装备 def ≈ 0.3K，30% 减伤
- 满装备 def ≈ 2K，67% 减伤
- 不可能超过 100%（lim = 1）

### Q2: 暴击率超过 100% 怎么处理？

通过 Cap modifier 防止：装备最大 +60%，buff 最大 +30%，cap 在 90%。
若没 cap 出错超过 100%，rng < 1.0 始终成立 = 100% 暴击。逻辑上仍工作但破坏平衡。

策划必须在每个 stat 配 cap（写在基础属性表）。

### Q3: 为什么 OnDamageDealt 在 Defense 之前，OnDamageTaken 在之后？

设计意图：
- **OnDamageDealt**（attacker 视角）：影响"我打多少"，应用 def 之前修改原始伤害
- **OnDamageTaken**（victim 视角）：影响"我吃多少"，应用 def 之后修改最终承受

例：
- "破甲攻击" buff（attacker）：MulDamage by 1.3，应在 def 之前 → 攻击穿透增强
- "护盾减伤" buff（victim）：absorb damage，应在 def 之后 → 实际承受减少

### Q4: Pure damage 真的绕过一切？包括 iframe？

**iframe 优先级最高**，依然挡 Pure。Pure 仅绕过 def/res/DamageTakenMul 等"减伤层"。

实际上 Pure 几乎不用——通常用 True 即可（绕过 def/res 但仍受 DamageTakenMul）。

### Q5: DoT 的 source 死了，DoT 还继续吗？

默认继续——除非 buff 配置 `RemoveOnSourceDeath: true`。

继续的合理性：毒已经施加在身上，毒源死亡不影响毒效。

### Q6: 治疗会触发暴击吗？

会。治疗暴击是合理设计（让奶妈也有刺激）。配置：`heal_can_crit: bool` 在 HealTarget action。

### Q7: 单次伤害最大值有上限吗？

两种 cap：
- **`MaxDamagePerHit`** stat：单次伤害不超过此值（用于"无敌怪"机制）
- **`OneHitKillProtection`** flag：HP > 30% 时单次伤害不能直接致死（"一血保护"）

都通过普通 stat / flag 实现。

### Q8: 为什么 modifier 计算分 Add/MulAdd/MulMul 三类？

经典 RPG 模型：
- **Add**：装备/天赋固定加成（"+20 atk"）
- **MulAdditive**：% 增益按和叠加（"30%+50% = 80% 增益"），更宽松
- **MulMultiplicative**：% 增益按乘叠加（"1.3 × 1.5 = 95% 增益"），更紧

不同 buff 用不同 ModOp 即可表达"叠加方式不同"，灵活且符合玩家直觉。

### Q9: PvP 中 stat 是否生效（装备词条）？

**部分生效**：
- PvP 装备词条系数化（如 PvE 装备伤害 ×0.7 在 PvP）——通过 PvP-specific modifier 实现
- 玩家进 PvP arena 自动触发"PvP 平衡 buff"，附加大量 modifier 调整数值

详见 `09_tools/AB_TESTING.md`（待写）"PvP 平衡机制"章节。

### Q10: 一次性大量目标的伤害会拉爆性能吗？

200 目标 AoE 单次伤害：
- 200 × 30 μs = 6 ms（一次性）
- 偶发，可接受
- 若每 tick 都打 200 目标（持续 AoE），需要 RunForEach max_iterations 控制

---

### 反模式清单

- ❌ 直接改 `entity.Hp`（应走 ResolveDamage / ResolveHeal）
- ❌ 在 hot path 反复调 `Recompute()`（应靠 dirty flag 缓存）
- ❌ 在 Damage handler 里改 attacker/victim 的 hp（应通过 ModifyDamage 或 emit Heal）
- ❌ 服务端跑伤害公式时使用 `Math.Sin/Cos`（端同破坏，应用 `Atlas.CombatCore.Math`）
- ❌ Crit roll 用全局 RNG（端同破坏，应用 RngFor）
- ❌ Defense/Resistance 公式硬编码常数 K（应配置在数值表）
- ❌ 死亡判定在伤害结算之前（应在之后，避免少结算事件）
- ❌ 没 OnDeath handler 触发的死亡处理（应统一进 pipeline）
- ❌ DoT tick 不走完整 pipeline（应走，否则 DoT 不受 def 影响 = 平衡灾难）
- ❌ Stat Cap 缺失，导致暴击率/移速等溢出（必须配 Cap）

---

## 16. 里程碑

| 阶段 | 交付 |
|---|---|
| P0 末 | StatType / StatCache 基础；Modifier 类型完整 |
| P1 中 | Recompute 公式；基础 Damage pipeline |
| P2 初 | OnDamageDealt / OnDamageTaken 集成；Crit；Defense/Resistance |
| P2 中 | DoT / HoT / Shield；元素抗性 |
| P2 末 | 端同测试覆盖；平衡测试基线职业 |
| P3 | 与 COMBAT_FEEL 集成（伤害数字、暴击 VFX） |
| P4+ | PvP 平衡 modifier；ranked 数据 dashboard |

---

## 17. 文档维护

- **Owner**：Tech Lead + 数值策划 Lead
- **关联文档**：
  - `OVERVIEW.md`
  - `BUFF_SYSTEM.md`（modifier 来源 + 事件钩子）
  - `HIT_VALIDATION.md`（命中触发到本系统）
  - `COMBAT_ACTIONS.md`（DamageTarget / HealTarget / ModifyDamage action）
  - `SKILL_SYSTEM.md`（DSL 中引用 stat）
  - `COMBAT_EVENT_ORDERING.md`（OnHit/OnKill/OnDeath 顺序）
  - `DETERMINISM_CONTRACT.md`
  - `COMBAT_FEEL.md`（伤害数字、暴击反馈）

---

**文档结束。**

**核心纪律重申**：
1. **Stat 只有一个真源 StatCache**：所有读必须经过它
2. **Modifier 来源统一为 buff**：装备/天赋/状态都包装为 buff
3. **Damage pipeline 严格 9 步**：不允许跳过、不允许穿插
4. **OnHit / OnDamageTaken 的修改通过 ModifyDamage**：不直接改最终值
5. **Pure damage 不要滥用**：99% 场景用 Physical/Magic/True 已足够
6. **平衡看 dashboard 不靠感觉**：上线后用真实数据迭代
