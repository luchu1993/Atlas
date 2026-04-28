# 命中判定与延迟补偿（Hit Validation & Lag Compensation）

> **用途**：定义 Atlas 中"技能是否命中目标"的判定流程——hitbox 形状扫描、目标过滤、iframe/super armor/block 检查；以及 PvP 公平所需的延迟补偿（lag compensation）算法。
>
> **读者**：工程（必读）、战斗策划（§3、§7 必读，决定 hitbox 形状与窗口）、网络工程（§5、§6 必读）。
>
> **状态**：草案 v0.1 — 待团队评审。
>
> **前置文档**：`OVERVIEW.md`、`MOVEMENT_SYNC.md`、`SKILL_SYSTEM.md`、`COMBAT_ACTIONS.md`、`BUFF_SYSTEM.md`
>
> **下游文档**：`STAT_AND_DAMAGE.md`（命中后的伤害计算）、`COMBAT_EVENT_ORDERING.md`（事件顺序）

---

## 1. 设计目标与边界

### 1.1 目标

1. **服务端权威**：所有命中由服务端裁定，客户端仅做视觉预表现
2. **PvP 公平**：射手感知"我打到了"应得到 ≥ 95% 概率认可（lag compensation）
3. **PvE 流畅**：对 AI 怪物的命中判定无补偿成本
4. **端同确定性**：相同输入下命中结果完全一致（端共享判定函数）
5. **性能可扩展**：单 cell 同时存活 200+ hitbox 在性能预算内

### 1.2 关键指标（来自 OVERVIEW §0）

| 维度 | 数值 |
|---|---|
| Lag compensation 窗口上限 | **200 ms** |
| 服务端位置历史保存 | 1 秒 |
| Hitbox 形状扫描精度 | ≤ 5 cm |
| 单 hitbox 单次扫描 CPU | ≤ 50 μs |
| Favor-the-shooter 容忍 | RTT/2 + 50 ms（最多 200ms） |

### 1.3 非目标

- **不追求 CS:GO 子弹级精度**（200ms 已足够 ARPG）
- **不做客户端权威命中**（速度挂、神准挂会失控）
- **不做物理 raycast**（用自研形状扫描）
- **不做物体阻挡判定**（AoE 穿墙是设计选择，简化网络模型）

---

## 2. 总体流程

### 2.1 命中流程概览

```
玩家释放技能
   ↓
Skill Timeline 触发 SpawnHitbox action（服务端）
   ↓
服务端创建 HitboxInstance，附着到 caster 或世界
   ↓
每 server tick 推进 hitbox：
   ├─ 更新位置（跟随骨骼或独立运动）
   ├─ 候选目标查询（空间分区）
   ├─ 对每个候选执行形状扫描
   │   ├─ PvP 情况下：lag compensation 倒带目标位置
   │   ├─ 形状重叠？
   │   └─ 通过过滤器（敌我、状态）？
   ├─ 命中确认：
   │   ├─ iframe 检查 → 通过则忽略
   │   ├─ Block 检查 → 减伤或反制
   │   ├─ SuperArmor 检查 → 不触发反应
   │   └─ 触发 OnHit DSL（伤害计算见 STAT_AND_DAMAGE.md）
   └─ 广播 Damage 事件给客户端
   ↓
客户端收到 Damage 事件，触发反馈（COMBAT_FEEL.md）
   ↓
Hitbox lifetime 到期或被显式移除 → 销毁
```

### 2.2 客户端仅做视觉预表现

主控玩家释放技能：
- 客户端立即创建本地 hitbox（视觉用）
- **不做命中判定**——视觉 hitbox 只用于 hit spark 占位
- 等服务端 Damage 事件到达，触发完整反馈
- 若服务端无事件回来 = 没打到，客户端不补反馈

---

## 3. Hitbox 形状

### 3.1 支持形状

```cpp
enum class HitboxShape : uint8_t {
  Sphere,         // 全向球
  Cone,           // 锥形（前方扇形）
  Capsule,        // 胶囊（剑刃、长矛）
  Box,            // 长方体（剑气）
  SweepCapsule,   // 沿路径的胶囊（移动攻击）
  Custom,         // 脚本定义（极少用）
};
```

### 3.2 数据结构

```cpp
struct HitboxDef {
  uint16_t      id;
  HitboxShape   shape;
  float         range_m;          // 半径 / 长度
  float         angle_deg;        // 仅 Cone（半角，60° 表示总宽 120°）
  float         width_m;          // Box / Capsule
  float         height_m;         // Box
  float         length_m;         // Capsule
  float3        local_offset;     // 相对附着点
};
```

### 3.3 形状扫描函数（端共享 C++）

```cpp
namespace atlas::hit {

bool ShapeOverlap(
  const HitboxInstance& hb,
  float3 hb_origin,           // 世界坐标（服务端）或 cell 内坐标
  uint16_t hb_yaw,            // 朝向
  float3 target_pos,
  float target_radius);       // 目标胶囊半径
}
```

实现按 shape 派发到具体函数：
- `SphereVsCapsule`
- `ConeVsCapsule`
- `CapsuleVsCapsule`
- `BoxVsCapsule`
- `SweepCapsuleVsCapsule`

每种函数**纯函数**，无外部依赖，端同通过。

### 3.4 SweepCapsule 特殊性

挥剑动作：剑从 A 位置扫到 B 位置，扫过的弧线区域都算命中范围。

```cpp
struct SweepCapsule {
  float3 start_pos;
  float3 end_pos;
  float radius;
  // 在 [start, end] 路径上的所有胶囊点都算命中区域
};
```

实现：将 sweep 转化为"在路径上多个采样点上做 capsule 重叠"，通常 3-5 个采样点足够（不是连续微积分）。

### 3.5 Hitbox 跟随策略

```cpp
enum class HitboxAttachMode : uint8_t {
  CasterRoot,         // 跟随 caster 根节点（位置 + 朝向）
  CasterBone,         // 跟随 caster 指定骨骼（剑、拳）
  WorldStatic,        // 世界静态（陷阱、地面 AoE）
  WorldMoving,        // 世界动态（飞行物 hitbox）
  TargetRoot,         // 跟随被指定目标（吸附类技能）
};
```

**CasterBone 模式**的关键约束：
- 服务端**没有 Animator** —— 无法获取真实骨骼位置
- 解决：构建期从 AnimationClip 提取**骨骼轨迹曲线**（每帧 root 偏移）
- 运行时服务端按当前动画 elapsed_ms 查曲线得到偏移
- 客户端用同一曲线，结果一致

数据存 `BoneTrajectories.xlsx`，键 = `(anim_state_id, bone_id)`。

---

## 4. HitboxInstance 生命周期

### 4.1 数据结构

```cpp
class HitboxInstance {
public:
  uint64_t          instance_id;
  uint16_t          def_id;             // → HitboxDef
  
  EntityRef         owner;              // caster
  uint64_t          source_skill_id;    // 来源技能（事件归属）
  
  HitboxAttachMode  attach_mode;
  EntityRef         attach_target;      // CasterBone / TargetRoot 时的目标
  uint16_t          attach_bone_id;
  float3            local_offset;
  
  uint32_t          spawn_tick;
  uint32_t          lifetime_ticks;
  uint32_t          tick_interval;      // 0 = 仅扫一次；>0 = 每 N tick 扫一次
  uint32_t          last_scan_tick;
  
  // 多次命中控制（§8）
  std::vector<EntityRef> already_hit;
  uint8_t           max_hits_per_target;
  
  // 过滤
  TargetFilter      target_filter;
  
  // 命中回调
  uint32_t          on_hit_dsl_ref;     // 命中时执行的 DSL
  uint32_t          damage_dsl_ref;     // 伤害公式 DSL
};
```

### 4.2 创建

由 Skill Timeline 的 `SpawnHitbox` action 触发：

```csharp
public override void Apply(in ActionContext ctx) {
  var def = ctx.Combat.Defs.Hitboxes[HitboxDefId];
  var hb = ctx.World.HitSystem.Spawn(
    owner: ctx.Caster,
    def: def,
    sourceSkill: ctx.SourceSkill?.InstanceId ?? 0,
    attachMode: AttachMode,
    attachBone: AttachBoneId,
    localOffset: LocalOffset,
    lifetimeMs: LifetimeMs,
    onHitDsl: OnHitDslRef,
    damageDsl: DamageDslRef);
}
```

### 4.3 推进与扫描

每 server tick：

```csharp
public sealed class CellHitSystem {
  List<HitboxInstance> _instances;
  
  public void Tick(CombatContext ctx) {
    long now = ctx.CurrentTick;
    
    for (int i = _instances.Count - 1; i >= 0; i--) {
      var hb = _instances[i];
      
      // lifetime 检查
      if (now >= hb.spawn_tick + hb.lifetime_ticks) {
        Despawn(hb, ctx);
        continue;
      }
      
      // 扫描间隔检查
      if (hb.tick_interval == 0) {
        // 仅扫一次：第一帧扫
        if (now == hb.spawn_tick) DoScan(hb, ctx);
      } else {
        if (now >= hb.last_scan_tick + hb.tick_interval) {
          DoScan(hb, ctx);
          hb.last_scan_tick = now;
        }
      }
    }
  }
  
  void DoScan(HitboxInstance hb, CombatContext ctx) {
    var origin = ComputeOrigin(hb, ctx);    // 跟随骨骼/世界
    var candidates = SpaceQuery.GetCandidatesNear(origin, hb.def.range_m * 2);
    
    foreach (var target in candidates) {
      if (!PassesFilter(hb, target)) continue;
      if (hb.AlreadyHitMaxTimes(target)) continue;
      
      // PvP 走 lag comp，PvE 直接判
      bool hit;
      if (target.IsPlayerControlled && hb.owner.IsPlayerControlled) {
        hit = LagCompCheck(hb, origin, target, ctx);
      } else {
        hit = ShapeOverlap(hb, origin, target.Position, target.Radius);
      }
      
      if (hit) {
        ResolveHit(hb, target, ctx);
      }
    }
  }
}
```

### 4.4 销毁

由 lifetime 到期、`RemoveHitbox` action、caster 死亡（视配置）触发：

```csharp
void Despawn(HitboxInstance hb, CombatContext ctx) {
  // 触发 OnExpire DSL（极少用）
  if (hb.on_expire_dsl_ref != 0) ExecuteDsl(...);
  
  _instances.Remove(hb);
  HitboxInstancePool.Return(hb);  // 对象池
}
```

---

## 5. 延迟补偿（Lag Compensation）

### 5.1 问题

PvP 中 A 攻击 B：
- A 看到的 B 是 ~100ms 前的位置（远端实体延迟渲染，参见 `MOVEMENT_SYNC.md §6`）
- A 在自己屏幕上瞄准并释放技能
- 输入到达服务端又 ~50ms（A 的上行延迟）
- 服务端此刻 B 已在新位置

**不补偿则结果**：A 屏幕上明明打到了，服务端判定"miss"——玩家流失的核心原因。

### 5.2 解决思路

服务端按 A 的"感知时间"**回溯 B 的位置**：

```
A 感知到 B 的时间 = server_now − (A.rtt/2)             [A 上行延迟]
                                  − A.interp_delay      [远端实体显示滞后]
                                  − A.tick_input_age    [输入到 hitbox 触发的延迟]

补偿后 B 位置 = B.position_history.AtTime(perceived_time)
```

**capping**：补偿窗口最多 200ms（防滥用），超过 200ms 的延迟玩家不享受补偿。

### 5.3 位置历史缓冲

每个**玩家控制实体**维护位置历史（NPC 不需要，因为 AI 没有 ping）：

```cpp
struct PositionSample {
  uint32_t tick;
  float3 position;
  uint16_t yaw;
  uint8_t state_flags;     // 是否 iframe / block 等（用于事件回溯一致性）
};

class PositionHistory {
  static constexpr int CAPACITY = 30;   // 1s @ 30Hz 副本，1.5s @ 20Hz 开放
  PositionSample samples_[CAPACITY];
  uint32_t head_ = 0;
  
public:
  void Push(const PositionSample& sample);
  
  // 给定时间戳，返回插值后的位置
  PositionSample InterpolateAt(uint32_t target_tick) const;
};
```

### 5.4 LagCompCheck 实现

```csharp
bool LagCompCheck(HitboxInstance hb, float3 origin, EntityRef target, CombatContext ctx) {
  var attacker = hb.owner;
  
  // 计算补偿时间
  uint perceived_age_ms = 
      (attacker.AvgRttMs / 2) + 
      attacker.RemoteInterpDelayMs + 
      ctx.TickDurationMs;  // 输入到 hitbox 触发的额外 1 tick
  
  perceived_age_ms = Math.Min(perceived_age_ms, 200);  // 200ms 上限
  
  uint compensated_tick = ctx.CurrentTick - (perceived_age_ms / ctx.TickDurationMs);
  
  // 回溯目标位置
  var sample = target.PositionHistory.InterpolateAt(compensated_tick);
  
  // 用回溯位置做形状重叠
  return ShapeOverlap(hb, origin, sample.position, target.Radius);
}
```

### 5.5 Favor-the-shooter

**容忍边界**：当形状扫描结果"接近边界"时，倾向射手胜利：

```csharp
bool ShapeOverlapWithTolerance(...) {
  float distance_to_edge = ComputeDistanceToShapeEdge(hb, target);
  
  // 在边界 0.2m 内视为命中（hitbox 半径多容忍 5%）
  return distance_to_edge < 0.2f;
}
```

**仅 PvP 模式启用** favor-the-shooter（PvE 不需要边界倾斜）。

### 5.6 受补偿者的不公平感如何处理

被补偿者的体感："我已经躲开了为什么还被打到"。设计上的处理：

1. **明确告知玩家延迟补偿存在**（教学 + 设置面板说明）
2. **窗口严格 200ms**：超大延迟玩家不享受，不能滥用
3. **iframe 检查在补偿之后**：见 §6——若被补偿瞬间 victim 已进入 iframe，不算命中
4. **视觉反馈解释性**：被打到时弹出"被锁定"图标暗示 lag comp，玩家心理可接受

### 5.7 NPC vs 玩家不对称

| 攻击者 | 受击者 | 是否补偿 |
|---|---|---|
| 玩家 | 玩家 | **是**（PvP） |
| 玩家 | NPC | 否（NPC 没 ping，无需回溯；NPC 也不应"占便宜") |
| NPC | 玩家 | 否（玩家本来就看到 NPC 的真实位置） |
| NPC | NPC | 否 |

仅 PvP 才补偿，简化模型。

### 5.8 移动 hitbox（CasterBone）的补偿

挥剑技能 hitbox 跟随手骨：
- 攻击者的 hitbox origin 由其骨骼位置决定，**这个 origin 不补偿**（攻击者用自己当前位置）
- 受击者的位置补偿（按 §5.4）

简而言之：**只补偿目标位置**，不补偿 hitbox 自身。

---

## 6. iframe / SuperArmor / Block 检查时序

### 6.1 概念区分

| 状态 | 影响 |
|---|---|
| **iframe**（无敌帧） | 完全免疫伤害，不触发受击反应，hitbox 视为穿透 |
| **SuperArmor**（霸体） | 收伤害但不触发受击 stagger（玩家继续动作） |
| **Block**（格挡） | 减伤 + 可能触发反击（特定方向） |
| **Shield**（护盾） | 优先消耗护盾值，护盾未破不触发反应 |

每种都是 buff modifier 上的 flag（参见 `BUFF_SYSTEM.md §11.3`）。

### 6.2 检查顺序

按"先免疫后承伤"顺序：

```
命中确认（形状重叠通过）
   ↓
1. iframe 检查
   if target.flag_iframe { 
     widget_hit_spark = false;
     return; // 完全跳过
   }
   ↓
2. Block 检查
   if target.flag_blocking && IsBlockableDirection(hb, target) {
     // 触发格挡逻辑（§6.5）
     ResolveBlock(hb, target);
     return;
   }
   ↓
3. Shield 检查
   if target.shield_value > 0 {
     ConsumeShield(hb, target);
     // 护盾未破时也跳过 reaction
     return;
   }
   ↓
4. 应用伤害 + reaction（参见 STAT_AND_DAMAGE.md）
   if target.flag_super_armor {
     // 受伤但不触发 stagger
     ApplyDamage(hb, target, no_reaction: true);
   } else {
     ApplyDamage(hb, target, no_reaction: false);
   }
```

### 6.3 iframe 时序的精度

**关键问题**：lag comp 后回溯到的 victim 状态——是用回溯时刻的 iframe 状态，还是当前的？

**答案**：用**回溯时刻的状态**。

```csharp
var sample = target.PositionHistory.InterpolateAt(compensated_tick);
if (sample.state_flags.HasFlag(StateFlag.Iframe)) {
  return;  // 命中时刻 victim 在 iframe，不算命中
}
```

这要求 PositionSample 同时记录 state_flags（见 §5.3 的 sample 结构）。

**注意 PvP 翻滚**：A 释放攻击时 B 还在站立，但 B 在补偿窗口内翻滚进 iframe → 此次攻击**不算命中**（B 应得"我躲开了"反馈）。

### 6.4 Block 方向判定

```csharp
bool IsBlockableDirection(HitboxInstance hb, EntityRef target) {
  float3 attack_dir = (target.Position - hb_origin).Normalized;
  float3 block_facing = ForwardOf(target.yaw);
  
  float dot = Dot(attack_dir, block_facing);
  // dot > 0.5 表示攻击大致从前方来（120° 锥角）
  return dot > 0.5f;
}
```

**Perfect Block**（完美招架）：在格挡开启的前 100ms（数据驱动）算 perfect block，触发反击 buff。

### 6.5 Block 处理

```csharp
void ResolveBlock(HitboxInstance hb, EntityRef target, CombatContext ctx) {
  var blockBuff = target.GetBuff(BuffId.Blocking);
  bool isPerfect = (ctx.CurrentTick - blockBuff.ApplyTick) < blockBuff.Def.PerfectWindowTicks;
  
  if (isPerfect) {
    target.GainPosture();  // 体感增加
    target.ApplyBuff(BuffId.PerfectBlockCounter, ctx);  // 反击窗口
    EmitVisualEvent(VisualEventType.PerfectBlock, hb, target);
    EmitAudioEvent(SoundId.PerfectBlock, target.Position);
  } else {
    // 普通格挡：减伤、消耗 stamina
    var dmg = ComputeReducedDamage(hb, target);
    target.Hp -= dmg;
    target.ConsumeStamina(blockBuff.Def.StaminaCost);
    EmitVisualEvent(VisualEventType.Block, hb, target);
  }
  
  // 即使格挡也触发 OnBlock buff handlers
  CombatEvents.Emit(BuffEvent.OnBlock, target);
}
```

### 6.6 SuperArmor 在伤害结算后

SuperArmor 不影响伤害进入，只影响**反应**：
- 伤害正常计算
- 仅跳过 hit reaction animation（玩家继续 anim）
- 仍可触发 buff handlers（包括 OnDamageTaken）

### 6.7 多重判定的优先级

同时具有多个状态时（罕见但可能）：

```
iframe > block > shield > super armor

最终行为按最高优先级状态决定。
```

---

## 7. 目标过滤

### 7.1 TargetFilter 枚举

```cpp
enum class TargetFilter : uint8_t {
  Enemies,            // 仅敌方
  Allies,             // 仅友方（治疗、buff 用）
  Both,               // 不区分
  EnemiesNoSelf,      // 敌方但不含自己（避免误伤）
  AlliesNoSelf,       // 友方但不含自己
  All,                // 包括 self
  PlayersOnly,        // 仅玩家（PvP 限定技能）
  NpcOnly,            // 仅 NPC
};
```

### 7.2 敌我判定

```cpp
bool IsEnemyOf(EntityRef a, EntityRef b) {
  // PvP 副本中：不同队 = 敌
  if (ctx.IsPvpInstance) return a.team != b.team;
  
  // 开放世界：阵营 + faction
  if (a.faction != b.faction) return true;
  
  // PvE 公开 PK 区域：根据玩家 PK 状态
  if (a.IsPlayer && b.IsPlayer && ctx.IsPvpZone) {
    return a.PkFlag || b.PkFlag;
  }
  
  return false;
}
```

### 7.3 跨阵营 buff 区分

某些 hitbox 既造成伤害又施加 debuff，处理两种：
- 对敌方：伤害 + debuff
- 对友方：忽略 / 治疗（看 BuffDef）

实现：hitbox 先过滤敌我，命中后 OnHit DSL 内进一步分支：
```
if target.is_enemy_of(caster) {
  emit DamageTarget(target, ...);
  emit ApplyBuff(target, Slow, 3000);
} else if target.is_ally_of(caster) {
  emit HealTarget(target, ...);
}
```

---

## 8. 多次命中控制

### 8.1 一次性 hitbox（单次扫描）

`tick_interval == 0`：spawn 后第一帧扫描一次，已命中目标加入 `already_hit`，不再命中。

### 8.2 持续 hitbox（周期扫描）

`tick_interval > 0`：每 N tick 扫一次（如激光、毒云）。

```cpp
if (already_hit.contains(target) && hb.max_hits_per_target == 1) continue;

if (already_hit.count(target) >= hb.max_hits_per_target) continue;
```

**典型配置**：
- 激光：`tick_interval=3, max_hits_per_target=999`（持续打）
- 毒云：`tick_interval=10, max_hits_per_target=999`（每 0.5s 跳伤）
- 单段攻击：`tick_interval=0, max_hits_per_target=1`
- 横扫剑（连击）：`tick_interval=0, max_hits_per_target=2`（同一目标可被扫到 2 次）

### 8.3 重置语义

某些 hitbox 跨多个 active 阶段（例如剑挥两下，hitbox 从 frame 5–10 + 20–25 active），中间需要重置 `already_hit`：
- 拆为**两个独立 hitbox** 更清晰，而非用复杂 reset 标志
- 简化判定逻辑

### 8.4 多 hitbox 同 tick 命中同目标

A 释放 AoE，hitbox 1 和 hitbox 2 同 tick 都覆盖目标 B：
- 各自独立判定（B 被两次伤害）
- 玩家视觉看到两个 hit spark
- 这是设计意图（多重 hit），不是 bug

---

## 9. 跨 Cell 命中处理

### 9.1 边界场景

A 在 cell #1，B 在 cell #2（边界相邻）。A 释放 AoE 覆盖到 B。

按 OVERVIEW §5.3，**战斗中实体锁定 cell**——但攻击瞬间可能跨 cell（B 刚走过去就被打）。

### 9.2 解决：Ghost-aware Hitbox

- A 的 hitbox 在 cell #1 创建
- 扫描时查询当前 cell + 邻接 cell 的实体（包括 Ghost）
- 命中跨 cell 实体时，将事件转发给 B 的 real cell（cell #2）
- cell #2 处理伤害结算（权威）

```csharp
void ResolveHit(HitboxInstance hb, EntityRef target, CombatContext ctx) {
  if (target.RealCellId == ctx.CellId) {
    // 同 cell，直接结算
    LocalHitResolution(hb, target, ctx);
  } else {
    // 跨 cell，转发到 target real cell
    ctx.World.SendCrossCell(target.RealCellId, 
      new CrossCellHitMsg { 
        hb_def_id = hb.def_id, 
        attacker = hb.owner, 
        target = target,
        damage_dsl = hb.damage_dsl_ref,
        ...
      });
  }
}
```

### 9.3 原子性

跨 cell 命中事件通过**消息队列**异步处理：
- A 的 hitbox 检测到 B 命中 → 立即发消息
- B 的 cell 在下一 tick 收到消息 → 应用伤害
- B 客户端可能收到伤害事件比同 cell 慢 ~33ms（一 tick）

**接受这个延迟**——边界处的 1 tick 偏差对手感影响小（0.5 m 内的 hitbox 边缘行为差异）。

### 9.4 边界 bug 的预防

- 服务端跨 cell 消息优先级最高，必须当 tick 处理
- B 的位置历史依然按其 real cell 维护
- 跨 cell 事件**不重复**（attacker 一 tick 内同 hitbox 同 target 只发一次）

详见 `01_world/GHOST_ENTITY.md`（待写）。

---

## 10. PvE 与 PvP 的差异

### 10.1 PvE：简化路径

| 项 | 处理 |
|---|---|
| Lag compensation | 不启用（NPC 不需要） |
| Position history | 玩家维护，NPC 不维护 |
| iframe / block | 仍按规则检查（玩家可能用） |
| Favor-the-shooter | 不启用（无需公平倾斜） |
| 跨 cell | 仍走 Ghost 机制 |

### 10.2 PvP：完整路径

| 项 | 处理 |
|---|---|
| Lag compensation | 启用，200ms 上限 |
| Position history | 双方都维护 |
| iframe 时序 | 按补偿时刻检查 |
| Favor-the-shooter | 启用，5% 边界容忍 |
| 反作弊 | 强化（重复命中、瞬移作弊检测） |

### 10.3 混合场景（PvP arena 内 NPC）

PvP arena 内可能有助战 NPC（如召唤物）：
- NPC 攻击玩家：玩家位置补偿（攻击者无 ping，按当前 tick）
- 玩家攻击 NPC：NPC 不补偿（NPC 永远是当前位置）
- NPC 攻击 NPC：直接判（无补偿）

---

## 11. 客户端预测命中

### 11.1 预测策略

主控释放攻击，客户端**部分预测**：

| 行为 | 预测时机 | 服务端确认作用 |
|---|---|---|
| 挥剑动画 | 立即 | 一致则继续；服务端拒绝则中止 |
| Hit Spark VFX（命中点光斑） | **不预测** | 等服务端 Damage 事件 |
| 伤害数字 | **不预测** | 等服务端 |
| Hit Pause | **不预测** | 等服务端 |
| 受击反应（victim） | 远端实体看到 attacker 挥剑 | 等服务端 Damage 事件后 |

**核心原则**：**反馈层不预测**。宁可"略晚 50ms 反馈"，不要"假反馈再撤回"——撤回视觉反馈是非常糟糕的体验。

### 11.2 训练假人例外

打不会反击的训练假人（练习场景）：
- 客户端可以**完全预测**（hit spark + 伤害数字立即出）
- 因为不存在"服务端拒绝"的可能（除非 caster 自己被状态阻止）
- 提升手感测试的反馈即时性

服务端识别"假人对象"（特殊 EntityType），客户端按目标类型决定预测档位。

---

## 12. 性能预算

### 12.1 单 hitbox 单次扫描

| 操作 | 预算 |
|---|---|
| 候选查询（空间分区） | ≤ 5 μs |
| 形状重叠（每个候选） | ≤ 1 μs |
| Lag compensation（PvP） | ≤ 3 μs |
| iframe / block 检查 | ≤ 2 μs |
| 单 hitbox 总扫描（含 5 候选） | ≤ 30 μs |

### 12.2 单 cell 单 tick

假设 200 active hitbox + 平均 0.5 扫描/tick（多数 hitbox 只在 spawn 时扫）：
```
200 × 0.5 × 30 μs = 3 ms / tick
```

占副本 33ms 的 9%，可接受。

### 12.3 优化策略

- **空间分区**：使用 grid（cell 内细分 8×8 子格），按子格存实体引用，hitbox 查邻近格
- **候选预筛**：球形包围盒先快速 cull（半径 = hitbox max 范围 + target radius）
- **形状扫描内联**：常用形状（Sphere/Cone/Capsule）的重叠函数手写汇编 / SIMD
- **PositionHistory 查找**：环形数组按 tick 二分查找

### 12.4 监控指标

- 单 cell 平均 active hitbox 数（峰值、平均）
- 单 hitbox 平均扫描耗时
- Lag comp 命中率（"补偿后命中" / "总命中"）
- iframe 拒绝率（被 iframe 救下的攻击数）

---

## 13. 反作弊

### 13.1 命中作弊形式

- **超距离命中**：客户端虚构距离，发"我打到了"——**不存在**，客户端不发命中事件，服务端权威
- **完美瞄准**：自动瞄准最近敌人——**位置作弊层处理**（参见 `MOVEMENT_SYNC.md §10`）
- **完美闪避**：服务端 Damage 事件未到时本地预测出 iframe——**iframe 必须服务端权威**
- **重复命中**：同一 hitbox 命中同一目标多次——**已被 already_hit 集合防御**

### 13.2 服务端检测

```cpp
struct HitAuditLog {
  uint64_t hb_instance_id;
  EntityRef attacker;
  EntityRef target;
  uint32_t tick;
  bool was_lag_compensated;
  uint32_t compensation_age_ms;
  bool result;
};

// 持久化（轮转 1 小时），异常分析
```

异常模式：
- 单玩家命中率 > 90%（正常 60-80%）
- 命中分布异常（all back-stab）
- Lag comp 上限频繁触发（200ms 边缘玩家）
- 同一 hitbox 跨 cell 触发频繁（地理跳跃挂）

---

## 14. FAQ 与反模式

### Q1: 为什么不直接用 Unity Physics 做 hitbox 判定？

- 端同破坏（PhysX 内部浮点不一致）
- 服务端没有 Unity 运行时
- 性能不可控（PhysX overhead）
- 自研 ~1000 行代码完全可控

### Q2: 200ms lag comp 上限是不是太严格？

业界惯例：
- CS:GO: ~200ms
- Source Engine: 1000ms（古老）
- Overwatch: ~250ms
- BDO: ~300ms（曾经，现已收紧）

200ms 覆盖 99% 玩家（中国/北美/欧洲玩家 ping 通常 < 200ms 全程），剩余 1% 玩家不补偿是可接受的设计选择。

### Q3: Favor-the-shooter 5% 边界容忍真的让被打者会愤怒吗？

不会。原因：
- 5% 容忍不是"穿透判定"，只是"贴边时优先射手"
- 被打者的 perceived 命中通常也接近边界（双方视角对称）
- BDO/Dragon Nest 都用类似机制

### Q4: 跨 cell 命中真的需要 Ghost 吗，能否简化？

可以**短期**简化为"每 cell 独立处理，跨 cell 命中丢弃"，但：
- 边界处会出现"打不到"的盲区（玩家明显感知）
- PvP arena 不会跨 cell（单 instance），影响小
- PvE 开放世界要做 Ghost——必须

P3 阶段先做 PvP arena（无跨 cell），P4 上 Ghost 处理 PvE 开放世界。

### Q5: hitbox 跟随骨骼（CasterBone）端同准确吗？

依赖**骨骼轨迹曲线在构建期提取**，端同保证：
- 同 anim_state + 同 elapsed_ms → 同骨骼位置
- 服务端不跑 Animator，但读相同曲线得相同值
- 误差 < 1cm（采样频率 60Hz，曲线插值）

详见 §3.5。

### Q6: 玩家 Block 时碰到不可格挡攻击（unblockable）怎么办？

技能配置 `is_unblockable: bool`。Hit pipeline 跳过 Block 检查：
```csharp
if (hb.def.is_unblockable) {
  // 跳过 Block 检查
} else {
  if (target.flag_blocking && IsBlockableDirection(...)) {
    ResolveBlock(...);
    return;
  }
}
```

视觉上 unblockable 攻击需有红色高亮预警（设计要求）。

### Q7: 多人围殴一个目标，目标的位置历史会不会被多人都"看到"不同的过去版本？

会——每个攻击者用自己的 perceived_time 回溯到不同 tick 的 victim 位置。这是设计意图（每人公平地享受补偿），不是 bug。

### Q8: 如果两个玩家"互相必杀"（同 tick 互相致命一击），谁先死？

按 `COMBAT_EVENT_ORDERING.md` 规则：
- 同 tick 多事件按 (server_tick, attacker_id, hitbox_instance_id) 三级排序
- 排序靠前者先结算
- 后者结算时若已死亡 = 攻击无效

实际上玩家几乎不会真正同 tick 互击，但规则需要明确。

### Q9: 命中判定时若 caster 已死亡？

- Hitbox 仍存活到 lifetime 结束（设计选择，避免"死前最后一击不算"）
- 但 hitbox 已无 owner 修饰符——伤害用 hitbox 创建时缓存的 modifier 快照
- 即"死人之拳"机制

### Q10: 服务端拒绝命中（lag comp 后判定 miss），客户端怎么知道？

- **服务端不发"miss"通知**（带宽浪费）
- 客户端**预期一定时间内收到 Damage 事件**（如 200ms）
- 超时未收到 = 没打到，本地视觉自然消退（hit spark 本来就没预测）
- 玩家感知：动画播完了但没数字弹出 = 打空

### 反模式清单

- ❌ 让客户端发送"我命中了" 给服务端（作弊向）
- ❌ Hit Spark / 伤害数字客户端预测（撤回视觉是糟糕体验）
- ❌ 用 Unity Physics 做服务端命中判定（端同破坏）
- ❌ Lag comp 窗口设 > 300ms（被 lag comp 的玩家受不了）
- ❌ iframe 检查在 lag comp 之前（错误时序导致"我躲了还被打"）
- ❌ Hitbox 跟随骨骼但骨骼数据不端共享（双方位置不同）
- ❌ 跨 cell 命中直接读对方 cell 实体（应通过消息）
- ❌ 多次命中处理用复杂 reset 而非拆多个 hitbox

---

## 15. 里程碑

| 阶段 | 交付 |
|---|---|
| P0 末 | HitboxInstance 数据结构；Sphere / Cone 重叠函数 |
| P1 中 | Capsule / Box / SweepCapsule 完整形状库 |
| P2 初 | 完整 hitbox 生命周期；多次命中控制；目标过滤 |
| P2 中 | iframe / Block / SuperArmor 检查时序 |
| **P2 末** | **Lag compensation 完整实现；PvP 1v1 命中公平验证** |
| P3 早 | Favor-the-shooter；位置历史精度调优 |
| P3 中 | 与 COMBAT_FEEL 的反馈集成 |
| P4+ | 跨 cell 命中（Ghost-aware） |

---

## 16. 文档维护

- **Owner**：Tech Lead + Network Engineer
- **关联文档**：
  - `OVERVIEW.md`（§7 难点二、三引用本文）
  - `MOVEMENT_SYNC.md`（PositionHistory 详细实现）
  - `SKILL_SYSTEM.md`（SpawnHitbox action 来源）
  - `BUFF_SYSTEM.md`（iframe / SuperArmor / Block 状态来源）
  - `STAT_AND_DAMAGE.md`（命中后伤害计算）
  - `COMBAT_FEEL.md`（命中事件触发的反馈）
  - `COMBAT_EVENT_ORDERING.md`（同 tick 多命中排序）
  - `01_world/GHOST_ENTITY.md`（跨 cell 实体）
  - `08_security/ANTI_CHEAT_STRATEGY.md`（命中作弊检测）

---

**文档结束。**

**核心纪律重申**：
1. **服务端权威一切**：客户端永不裁决命中
2. **PvP 才补偿，PvE 不补偿**：成本与收益匹配
3. **iframe 时序按补偿时刻**：闪避公平的关键
4. **反馈不预测**：宁可略晚不要撤回
5. **跨 cell 走 Ghost**：边界处不取捷径
