# 战斗 Action 与 DSL 设计

> **用途**：定义 Atlas 战斗系统的**底层执行原语**——Action 全集、DSL 语法与字节码、内置函数库。SKILL 和 BUFF 系统都构建在本文之上。
>
> **读者**：工程（必读）、战斗策划（§5、§9 必读，理解能用什么"积木"配技能/buff）。
>
> **状态**：草案 v0.1 — 待团队评审。
>
> **前置文档**：`OVERVIEW.md`、`00_foundations/DETERMINISM_CONTRACT.md`
>
> **上层文档**：`SKILL_SYSTEM.md`（用 Action 做 timeline 事件）、`BUFF_SYSTEM.md`（用 Action 做 handler）

---

## 1. 定位与设计目标

### 1.1 这层在哪

```
┌────────────────────────────────────┐
│ SKILL 系统    │  BUFF 系统           │
│  (timeline)   │  (event-driven)     │
└──────┬───────────────┬──────────────┘
       │               │
       ▼               ▼
┌────────────────────────────────────┐
│  Action / DSL 执行器（本文档）      │
│  Action: 原子执行单元                │
│  DSL: 条件表达式 + 目标选择器        │
└──────────────┬─────────────────────┘
               │
               ▼
┌────────────────────────────────────┐
│  Combat Core 基础设施               │
│  Stat/Damage, Hitbox, Movement,    │
│  RNG, Time, Math                    │
└────────────────────────────────────┘
```

Action / DSL 是 SKILL 和 BUFF 的**共用语言**——技能 timeline 触发 Action、buff handler 触发 Action、命中回调走 DSL，全部走同一份执行器。

### 1.2 设计目标

1. **统一**：技能、buff、AI 都用同一套 Action / DSL，避免"三套类似但不一样"的执行路径
2. **数据驱动**：Action 用 Excel 配置参数，DSL 用文本写表达式，**不需要工程改代码就能扩充技能逻辑**
3. **端同**：执行器必须满足 `DETERMINISM_CONTRACT.md`，服务端/客户端逐字节一致
4. **性能**：Action 派发 < 100 ns，DSL 表达式求值 < 1 μs
5. **安全**：DSL 受限（无任意循环、有 gas limit），策划写错不会让服务端死循环
6. **可扩展**：增加新 Action / 新内置函数有清晰流程

### 1.3 非目标

- **不做完整脚本语言**：DSL 故意阉割到只够表达战斗逻辑
- **不支持任意函数定义**：内置函数白名单制，不允许策划写自定义函数
- **不做异步**：所有 Action / DSL 调用瞬时完成（无 await / wait）
- **不暴露给运行时玩家**：DSL 不是"宏"或"自动战斗脚本"系统

---

## 2. Action 系统

### 2.1 Action 是什么

**Action 是一个原子执行单元**，封装"在某时刻对世界做某件事"的最小单位。

特点：
- **瞬时执行**：调用 → 副作用 → 返回，不持续
- **无返回值**：副作用通过 `CombatContext` 传递给后续逻辑
- **数据驱动**：每种 Action 有专属 Excel 表，参数从表中读取
- **复用**：一种 Action 可被多个 Skill / Buff / DSL 引用

### 2.2 Action 接口

```csharp
namespace Atlas.CombatCore.Actions;

public abstract class Action {
  public int        Id;            // 在该 Action 类型表中的主键
  public abstract ActionType Type { get; }
  
  public abstract void Apply(in ActionContext ctx);
  
  // 可选：客户端预测时的视觉钩子（默认 = Apply）
  public virtual void ApplyVisualOnly(in ActionContext ctx) => Apply(ctx);
}

public readonly struct ActionContext {
  public readonly CombatContext   Combat;       // tick / cell / world 等
  public readonly EntityRef       Caster;       // 触发者
  public readonly EntityRef       PrimaryTarget; // 主目标（可空）
  public readonly EventPayload    Payload;      // 触发事件载荷（如 OnDamageTaken 的 damage 信息）
  public readonly SkillInstance?  SourceSkill;  // 源技能实例（若由技能触发）
  public readonly BuffInstance?   SourceBuff;   // 源 buff 实例（若由 buff 触发）
}

public enum ActionType : byte {
  // Animation
  PlayAnim = 1, PlayAnimLoop, SetAnimParam, StopAnim,
  // Hitbox
  SpawnHitbox = 10, RemoveHitbox,
  // Movement
  Dash = 20, Launch, LaunchOther, Pull, Knockback, Teleport, LockMovement, UnlockMovement,
  // State Flag
  SetFlag = 40, ClearFlag,
  // Buff
  ApplyBuff = 50, RemoveBuff, RefreshBuff, DispelBuff, ExtendBuffDuration,
  // Spawn
  SpawnProjectile = 70, SpawnSubSkill, SpawnEntity, DespawnEntity,
  // Resource
  ConsumeMp = 90, ConsumeResource, RestoreMp, RestoreResource,
  // Variable
  SetVar = 110, AddVar,
  // Damage / Heal
  DamageTarget = 130, HealTarget, ModifyDamage,
  // Visual (客户端)
  PlayVFX = 150, PlaySound, CameraShake, HitPause, CameraSlomo, CameraFocus, ShowDamageNumber,
  // Control Flow
  CancelSkill = 200, EndSkill, GotoState, EmitEvent,
  // Conditional
  RunIf = 220, RunForEach,
  // Custom
  CustomScript = 250,
}
```

### 2.3 派发实现

```csharp
public static class ActionDispatcher {
  static readonly Action[] _registry = new Action[256];   // ActionType byte 索引
  
  public static void Register(ActionType type, Action prototype) {
    _registry[(int)type] = prototype;
  }
  
  // 由 SkillTimeline / Buff Handler 调用
  public static void Dispatch(ActionType type, int actionId, in ActionContext ctx) {
    var prototype = _registry[(int)type];
    if (prototype == null) {
      Log.Error($"Unknown ActionType {type}");
      return;
    }
    
    var def = ctx.Combat.Defs.GetActionDef(type, actionId);
    if (def == null) {
      Log.Error($"Action {type}#{actionId} not found");
      return;
    }
    
    def.Apply(ctx);
  }
}
```

**关键纪律**：
- Action 实例**单例**（每种类型一个 prototype），状态全在 `def` 数据里
- 派发用 byte 索引数组而非 `Dictionary`（确定性、性能）
- 未注册的 ActionType 记录错误但不崩溃（线上配置错误不应炸服）

### 2.4 Visual Action（客户端豁免确定性）

某些 Action 是**纯视觉表现**，服务端不执行：
- `PlayVFX`, `PlaySound`, `CameraShake`, `HitPause`, `CameraSlomo`, `CameraFocus`, `ShowDamageNumber`

这些 Action：
- 服务端遇到时**仅广播事件**给相关客户端（不执行）
- 客户端解码事件后调用对应 `ApplyVisualOnly`
- **豁免** `DETERMINISM_CONTRACT.md`（视觉差异不影响游戏状态）

实现：
```csharp
public abstract class Action {
  public virtual bool IsVisualOnly => false;
}

public sealed class CameraShakeAction : Action {
  public override bool IsVisualOnly => true;
  // ... params
}
```

---

## 3. Action 全集（详解）

按类别列出每种 Action 的字段定义。每种 Action 对应一张 Excel 表，表名 = Action 类型 + "Actions.xlsx"（例 `HitboxActions.xlsx`）。

### 3.1 Animation 类

#### `PlayAnim`
| 字段 | 类型 | 说明 |
|---|---|---|
| `anim_state` | string | Animator state 名 |
| `transition_ms` | int | 过渡时长 |
| `play_speed` | float | 播放速度倍率 |

**端表现**：服务端不动，客户端切 Animator state。

#### `PlayAnimLoop`
同上，但 Animator state 必须是循环的（用于 charging / channel 状态）。

#### `SetAnimParam`
| 字段 | 类型 | 说明 |
|---|---|---|
| `param_name` | string | Animator 参数名 |
| `value_int` | int | 整数值（type=Int 时） |
| `value_float` | float | 浮点值（type=Float 时） |
| `value_bool` | bool | 布尔值 |
| `param_type` | enum | Int/Float/Bool/Trigger |

#### `StopAnim`
立即停止当前 Animator 播放，切到 Idle。

### 3.2 Hitbox 类

#### `SpawnHitbox`
| 字段 | 类型 | 说明 |
|---|---|---|
| `hitbox_def_id` | int | 指向 `HitboxDefs.xlsx`（形状、范围） |
| `attach_bone_id` | int | 附着骨骼，0 = caster 根 |
| `local_offset` | vec3 | 局部坐标偏移 |
| `lifetime_ms` | int | 存活时长 |
| `damage_dsl_ref` | string | 伤害公式 DSL 片段 |
| `on_hit_dsl_ref` | string | 命中时执行的 DSL（条件分支） |
| `target_filter` | enum | Enemies/Allies/Both |
| `max_targets` | int | 0 = 不限 |
| `tick_interval_ms` | int | 0 = 仅命中一次；>0 = 周期扫描 |

**HitboxDefs.xlsx**（独立表）：
| 字段 | 含义 |
|---|---|
| `id` | 主键 |
| `shape` | enum: Sphere / Cone / Capsule / Box / SweepCapsule |
| `range_m` | 半径 / 长度 |
| `angle_deg` | 仅 Cone 用 |
| `width_m` | 仅 Box / Capsule |
| `height_m` | 同上 |

#### `RemoveHitbox`
按 `hitbox_id` 提前销毁（用于打断时清理）。

### 3.3 Movement 类

参见 `MOVEMENT_SYNC.md §8`。每种 Movement Action 转化为 `MovementCommand`。

| Action | 主要参数 |
|---|---|
| `Dash` | distance / direction / curve_id / duration_ms / input_policy |
| `Launch` | velocity_xz / velocity_y / gravity / duration_ms |
| `LaunchOther` | target_filter / 同 Launch |
| `Pull` | target_pos / duration_ms |
| `Knockback` | direction / distance / duration_ms |
| `Teleport` | target_pos |
| `LockMovement` | duration_ms |
| `UnlockMovement` | (无参) |

### 3.4 State Flag 类

#### `SetFlag`
| 字段 | 类型 | 说明 |
|---|---|---|
| `flag` | enum | StatType.Flag_* |
| `duration_ms` | int | 持续时长，0 = 永久（仅技能内） |

**注意**：StateFlag 和 Buff 的 Modifier 系统统一——`SetFlag` 实际上是临时 ApplyBuff（持续 `duration_ms` 的 buff，modifier 是 `Flag_X = 1`）。这避免双轨：
- Buff 系统已经管理超级霸体、iframe、cancel_window 等"持续状态"
- `SetFlag` action 是 syntactic sugar，简化短期状态的配置

实现层 `SetFlag` 拆为 `ApplyBuff(internal_flag_buff_id, self, duration_ms)`。

#### `ClearFlag`
对应 `RemoveBuff(internal_flag_buff_id, self)`。

### 3.5 Buff 类

参见 `BUFF_SYSTEM.md`。

| Action | 主要参数 |
|---|---|
| `ApplyBuff` | buff_id / target_filter / duration_override_ms / initial_stacks / source_skill_id |
| `RemoveBuff` | buff_id / target_filter / count |
| `RefreshBuff` | buff_id / target_filter |
| `DispelBuff` | dispel_type_mask / target_filter / count |
| `ExtendBuffDuration` | buff_id / target_filter / by_ms / cap_ms |

### 3.6 Spawn 类

#### `SpawnProjectile`
| 字段 | 类型 | 说明 |
|---|---|---|
| `projectile_def_id` | int | 指向 `ProjectileDefs.xlsx` |
| `spawn_offset` | vec3 | 相对 caster |
| `direction_dsl_ref` | string | 方向计算 DSL（如 `caster.aim_direction`） |
| `velocity` | float | 初速度 |
| `lifetime_ms` | int | 最大存活 |
| `on_hit_skill_id` | int | 命中时触发的子技能 |

#### `SpawnSubSkill`
| 字段 | 类型 | 说明 |
|---|---|---|
| `skill_id` | int | 子技能 |
| `caster_source` | enum | Self / OriginalCaster / Target |
| `position_dsl_ref` | string | 释放位置 DSL |
| `target_dsl_ref` | string | 目标 DSL |

#### `SpawnEntity` / `DespawnEntity`
召唤宠物 / 守卫 / 陷阱用。

### 3.7 Resource 类

#### `ConsumeMp` / `ConsumeResource`
| 字段 | 类型 | 说明 |
|---|---|---|
| `amount_dsl_ref` | string | 消耗量（可基于公式） |
| `target_filter` | enum | Caster / Target |
| `fail_action` | enum | Cancel / Continue |

`fail_action = Cancel` 时不足则技能失败；`Continue` 时不足按 0 处理。

#### `RestoreMp` / `RestoreResource`
对应回复。

### 3.8 Variable 类

#### `SetVar` / `AddVar`
| 字段 | 类型 | 说明 |
|---|---|---|
| `var_name` | string | SkillInstance.Variables 的 key |
| `value_dsl_ref` | string | 值 / 增量计算 DSL |

### 3.9 Damage / Heal 类

#### `DamageTarget`
绕过 hitbox 直接造成伤害（如反伤、DoT）。
| 字段 | 类型 | 说明 |
|---|---|---|
| `target_dsl_ref` | string | 目标选择 DSL |
| `damage_dsl_ref` | string | 伤害公式 |
| `damage_type` | enum | Physical / Magic / True |
| `can_crit` | bool | |
| `is_dot` | bool | 标记为持续伤害（影响某些反应式 buff） |

#### `HealTarget`
对应治疗。

#### `ModifyDamage`
仅在 `OnDamageDealt`/`OnDamageTaken` handler 内有效，**修改正在结算的伤害**：
| 字段 | 类型 | 说明 |
|---|---|---|
| `op` | enum | Add / Mul |
| `value_dsl_ref` | string | 修改量 |

### 3.10 Visual 类（客户端豁免）

#### `PlayVFX`
| 字段 | 类型 | 说明 |
|---|---|---|
| `vfx_id` | string | 资源 key |
| `attach_mode` | enum | World / Bone / Detached |
| `attach_bone_id` | int | 仅 Bone 模式 |
| `local_offset` | vec3 | |
| `lifetime_ms` | int | |
| `scale` | float | |

#### `PlaySound`
| 字段 | 类型 | 说明 |
|---|---|---|
| `sound_id` | string | |
| `volume_mul` | float | |
| `attach_to_caster` | bool | 3D 定位音效 vs 全局 |

#### `HitPause` ★（手感关键）
| 字段 | 类型 | 说明 |
|---|---|---|
| `target_scope` | enum | AttackerOnly / VictimOnly / Both |
| `time_scale` | float | 0.0–1.0（0 = 完全冻结） |
| `duration_ms` | int | 通常 50–80 ms |

详见 `COMBAT_FEEL.md`。

#### `CameraShake`
| 字段 | 类型 | 说明 |
|---|---|---|
| `intensity` | float | 振幅 |
| `duration_ms` | int | |
| `frequency` | float | Hz |
| `falloff_curve` | enum | Linear / Exp / EaseOut |

#### `CameraSlomo`
| 字段 | 类型 | 说明 |
|---|---|---|
| `time_scale` | float | 全局时间缩放（仅本地相机感知） |
| `duration_ms` | int | |
| `ease_in_ms` / `ease_out_ms` | int | |

#### `CameraFocus`
| 字段 | 类型 | 说明 |
|---|---|---|
| `target_dsl_ref` | string | 聚焦目标 |
| `duration_ms` | int | |
| `framing` | enum | TightOnTarget / WideOnGroup / OverShoulder |

#### `ShowDamageNumber`
触发玩家 UI 上跳出伤害数字。

### 3.11 Control Flow 类

#### `CancelSkill`
立即结束当前 SkillInstance。

#### `EndSkill`
正常结束 SkillInstance（区别于 Cancel：会触发"自然完成"事件）。

#### `GotoState`
仅 SM 技能内有效，转到指定 state。

#### `EmitEvent`
向 `CombatEventSystem` 发射自定义事件，可被其他 buff 监听。

### 3.12 Conditional 类

#### `RunIf`
| 字段 | 类型 | 说明 |
|---|---|---|
| `condition_dsl_ref` | string | 条件 DSL |
| `then_actions_ref` | string | 满足时执行的 action 列表（分号分隔） |
| `else_actions_ref` | string | 不满足时执行的 |

#### `RunForEach`
| 字段 | 类型 | 说明 |
|---|---|---|
| `iter_dsl_ref` | string | 返回 list<entity_ref> 的 DSL |
| `each_actions_ref` | string | 对每个元素执行的 actions |
| `max_iterations` | int | gas limit，默认 32 |

### 3.13 Custom Script

#### `CustomScript`
仅在 custom_handler 模式技能内允许，调用 C# 脚本类。详见 `SKILL_SYSTEM.md §8`。

---

## 4. Target Filter 枚举

多个 Action 用到 `target_filter`：

```csharp
public enum TargetFilter : byte {
  Self,                    // caster 自身
  PrimaryTarget,           // ActionContext.PrimaryTarget
  EventActor,              // 触发事件的主体（如 OnDamageTaken 的 attacker）
  AllEnemiesInRange,       // 半径内所有敌人
  AllAlliesInRange,
  PartyMembers,
  NearestEnemy,
  RandomEnemyInRange,
  EntityById,              // 通过 entity_id 显式指定
  CustomDsl,               // 走 target_dsl_ref 字段
}
```

**简单情况用枚举**（性能好），**复杂情况用 DSL**（灵活）。

---

## 5. DSL 概述

DSL 用于 Action 参数计算、条件判断、目标选择。

### 5.1 设计原则

- **受限**：无任意循环、无递归、无闭包、无变量定义（除局部 `let`）
- **纯**：函数无副作用，结果只取决于输入
- **快**：编译为 bytecode，解释器栈式 VM
- **端同**：服务端 / 客户端 bytecode 完全相同
- **可终止**：每次调用有 gas limit 兜底

### 5.2 何时用 DSL vs Action

| 场景 | 工具 | 例 |
|---|---|---|
| 简单参数（常量） | 直接写值 | `damage = 100` |
| 简单公式（线性） | 直接写值或 DSL | `damage = base * 1.5` |
| 条件分支 | DSL（在 Action 内） 或 RunIf Action | `if target.frozen then 2.5 else 1.5` |
| 多目标循环 | RunForEach Action + DSL 选择器 | `foreach e in all_enemies_in_cone(3, 60)` |
| 复杂连招 | SM 状态机 + transition DSL | 见 SKILL_SYSTEM §4 |
| 大量分支 | 多个独立 Action / SM 状态 | 不要在单个 DSL 写 10 层 if |

---

## 6. DSL 语法

### 6.1 完整 EBNF

```ebnf
program        ::= statement*

statement      ::= let_stmt | assign_stmt | if_stmt | emit_stmt | return_stmt | block

let_stmt       ::= "let" identifier (":" type_name)? "=" expr ";"
assign_stmt    ::= identifier "=" expr ";"
if_stmt        ::= "if" expr block ("else" "if" expr block)* ("else" block)?
emit_stmt      ::= "emit" action_call ";"
return_stmt    ::= "return" expr? ";"
block          ::= "{" statement* "}"

expr           ::= primary | binary_op | unary_op | call | member | conditional

primary        ::= literal | identifier | "(" expr ")"
literal        ::= int_lit | float_lit | bool_lit | "null"
binary_op      ::= expr op expr            // + - * / %  == != < > <= >=  && ||
unary_op       ::= ("!" | "-") expr
call           ::= identifier "(" arg_list? ")"
member         ::= expr "." identifier
                 | expr "." identifier "(" arg_list? ")"
conditional    ::= "if" expr "then" expr "else" expr     // 表达式形式

action_call    ::= action_name "(" arg_list? ")"

arg_list       ::= expr ("," expr)*
type_name      ::= "int" | "float" | "bool" | "vec3" | "entity" | "list"
identifier     ::= [a-z_][a-zA-Z0-9_]*
```

### 6.2 类型系统

| 类型 | 说明 |
|---|---|
| `int` | 32-bit 有符号整数 |
| `float` | IEEE 754 32-bit |
| `bool` | true / false |
| `vec3` | 三个 float |
| `entity` | 实体引用（不透明，仅用于函数传入） |
| `list<T>` | 元素类型一致的列表 |
| `void` | 无返回值（仅 emit） |

**没有 string**：避免运行时分配；标识符通过整数 ID 传递。

### 6.3 类型推断 + 严格

- 编译期推断
- 严格类型：`int` 不自动转 `float`，需显式 `to_float(x)`
- 二元运算两侧类型必须一致

### 6.4 几个完整示例

**简单伤害公式**：
```
return caster.atk_power * 1.5 - target.defense * 0.5;
```

**条件伤害**：
```
let mul = if target.has_status(Frozen) then 2.5 else 1.5;
return caster.atk_power * mul;
```

**复杂 on_hit**：
```
if target.has_status(Burning) {
  emit ApplyBuff(target, Stagger, 500);
  emit ModifyDamage(Mul, 1.3);
}
if caster.is_below_hp_percent(0.3) {
  emit ModifyDamage(Mul, 1.5);
}
```

**目标选择**：
```
return all_enemies_in_cone(caster, 5.0, 60.0);
```

**多目标循环（在 RunForEach action 里）**：
```
let targets = all_enemies_in_radius(caster, 8.0);
emit RunForEach(targets, [
  ApplyBuff(it, Slow, 3000),
  DamageTarget(it, 50)
]);
```

`it` 是 RunForEach 的隐式迭代变量。

---

## 7. 内置函数库（白名单）

DSL 不允许策划定义函数，只能调用预注册的内置函数。

### 7.1 上下文访问

```
caster: entity              # 当前施法者（ActionContext.Caster）
target: entity              # 主目标（ActionContext.PrimaryTarget）
event_actor: entity         # 事件主体
event_target: entity        # 事件客体
self: entity                # buff handler 中 = 持有者
skill_level: int
charge_level: int           # 仅蓄力技能
combat_tick: int            # 当前 tick 号
```

### 7.2 实体属性查询（`e: entity` 上的方法）

```
e.id: int
e.hp: float
e.hp_max: float
e.hp_percent: float                  # [0, 1]
e.mp: float
e.mp_max: float
e.atk_power: float                   # 走 StatCache（含 buff 加成）
e.mag_power: float
e.defense: float
e.mag_resist: float
e.move_speed: float
e.atk_speed: float
e.crit_chance: float
e.crit_damage: float
e.position: vec3
e.yaw: float
e.is_dead: bool
e.is_player: bool
e.is_npc: bool
e.is_ally_of(other: entity): bool
e.is_enemy_of(other: entity): bool
e.has_status(status_id: int): bool
e.status_stacks(status_id: int): int
e.is_below_hp_percent(p: float): bool
e.distance_to(other: entity): float
```

### 7.3 目标选择器

```
nearest_enemy(caster: entity, range: float): entity
nearest_ally(caster: entity, range: float): entity
all_enemies_in_radius(caster: entity, range: float): list<entity>
all_allies_in_radius(caster: entity, range: float): list<entity>
all_enemies_in_cone(caster: entity, range: float, angle: float): list<entity>
all_in_box(caster: entity, half_extent: vec3, yaw: float): list<entity>
party_members(caster: entity): list<entity>
random_enemy_in_range(caster: entity, range: float): entity   # 走 DeterministicRng
```

### 7.4 数学

```
min(a, b), max(a, b), clamp(x, lo, hi), abs(x)
floor(x), ceil(x), round(x)
lerp(a, b, t), smoothstep(a, b, t)
sin(x), cos(x), tan(x)              # 走 Atlas.CombatCore.Math
sqrt(x), pow(base, exp), exp(x), log(x)
to_float(i: int): float
to_int(f: float): int                # 截断
```

### 7.5 vec3 运算

```
vec3(x, y, z): vec3
v.x, v.y, v.z: float
length(v: vec3): float
length_sq(v: vec3): float
normalize(v: vec3): vec3
dot(a, b): float
cross(a, b): vec3
```

### 7.6 随机（确定性）

```
rand_float(lo: float, hi: float): float
rand_bool(prob: float): bool
rand_int(lo: int, hi_exclusive: int): int
rand_pick(list: list<entity>): entity
```

**实现内部**：调用 `DeterministicRng`，种子由 `(combat_tick, caster.id, dsl_invocation_counter)` 派生（见 `DETERMINISM_CONTRACT.md §4.3`）。

### 7.7 列表操作

```
list.length: int
list.get(i: int): T
list.first(): T                     # 等价于 get(0)
list.is_empty(): bool
list.contains(e): bool
```

**不支持**：filter / map / sort（避免编程门槛过高，简单需求拆成 RunForEach + DSL）。

### 7.8 Action 发射（emit）

`emit ActionName(args...)`——把参数压栈，VM 调用 `ActionDispatcher.Dispatch`。

并非所有 Action 都能在 DSL 里 emit。**只有以下白名单**：

| Action | 可 emit 上下文 |
|---|---|
| `DamageTarget` / `HealTarget` / `ModifyDamage` | on_hit 回调 |
| `ApplyBuff` / `RemoveBuff` | 任意 |
| `SpawnVFX` / `PlaySound` | 任意 |
| `RunIf` / `RunForEach` | 任意 |
| `CameraShake` / `HitPause` | 任意 |
| `EmitEvent` | 任意 |

**禁止**在 DSL 里 emit `SpawnSubSkill` / `Teleport` / `CustomScript`（这些应在 Action 表里直接配，避免 DSL 沦为脚本）。

---

## 8. DSL 字节码 VM

### 8.1 设计

栈式虚拟机，opcode 32-bit 编码：

```
[opcode: 8 bit] [arg: 24 bit]
```

### 8.2 完整 opcode 表

```cpp
enum class Op : uint8_t {
  // 立即数
  PUSH_INT_CONST,        // arg = const_table_idx (int32 池)
  PUSH_FLOAT_CONST,      // arg = const_table_idx (float 池)
  PUSH_BOOL_TRUE,
  PUSH_BOOL_FALSE,
  PUSH_NULL,
  
  // 变量
  LOAD_LOCAL,            // arg = local idx
  STORE_LOCAL,           // arg = local idx
  LOAD_CTX,              // arg = ctx_var_id（caster, target, ...）
  
  // 算术
  ADD_I, ADD_F,
  SUB_I, SUB_F,
  MUL_I, MUL_F,
  DIV_I, DIV_F,
  MOD_I,
  NEG_I, NEG_F,
  
  // 比较
  EQ_I, EQ_F, EQ_B, EQ_E,    // E = entity
  NE_I, NE_F, NE_B, NE_E,
  LT_I, LT_F,
  LE_I, LE_F,
  GT_I, GT_F,
  GE_I, GE_F,
  
  // 逻辑
  AND_B, OR_B, NOT_B,
  
  // 类型转换
  TO_FLOAT, TO_INT,
  
  // 函数调用
  CALL_FN,               // arg = func_id
  CALL_METHOD,           // arg = method_id (entity / vec3 / list 上的方法)
  
  // 控制流
  JUMP,                  // arg = relative offset
  JUMP_IF_FALSE,
  JUMP_IF_TRUE,
  
  // Action 发射
  EMIT_ACTION,           // arg = action_type_id；栈顶是参数 struct
  
  // 字段访问
  GET_MEMBER,            // arg = member_id
  
  // 结束
  RETURN,
  RETURN_VOID,
};
```

总计 ~40 opcode。

### 8.3 VM 实现

```cpp
class DslVm {
  std::array<Variant, 64> stack_;
  uint32_t sp_ = 0;
  uint32_t pc_ = 0;
  uint32_t gas_remaining_ = 1000;
  
public:
  Variant Execute(const Bytecode& code, const ActionContext& ctx) {
    pc_ = 0;
    sp_ = 0;
    
    while (pc_ < code.ops.size()) {
      if (--gas_remaining_ == 0) {
        Log.Warn("DSL gas limit exceeded");
        return Variant::Null();
      }
      
      Op op = code.ops[pc_].op;
      uint32_t arg = code.ops[pc_].arg;
      pc_++;
      
      switch (op) {
        case Op::PUSH_INT_CONST: stack_[sp_++] = code.int_consts[arg]; break;
        case Op::ADD_I: { auto b = stack_[--sp_]; stack_[sp_-1] = stack_[sp_-1].i + b.i; break; }
        case Op::CALL_FN: ExecuteBuiltin(arg, ctx); break;
        case Op::EMIT_ACTION: EmitAction(arg, ctx); break;
        case Op::RETURN: return stack_[--sp_];
        // ...
      }
    }
    return Variant::Null();
  }
};
```

### 8.4 资源限制

| 限制 | 上限 | 超限处理 |
|---|---|---|
| 栈深 | 64 | 编译期拒绝 |
| Bytecode 长度 | 1024 ops | 编译期拒绝 |
| 单次执行 gas | 1000 ops | 运行时停止，返回 null + warning |
| `RunForEach` 迭代数 | 32（默认，可配） | 运行时截断 |

**Gas 设计原因**：恶意 / 错误的 DSL 不能让服务端死循环。1000 op 足以覆盖 99% 合理用例。

### 8.5 性能

实测预算：
- 简单求值（5 ops）：< 100 ns
- 中等表达式（30 ops）：< 500 ns
- 复杂分支（100 ops）：< 2 μs
- gas 上限附近（1000 ops）：< 20 μs

战斗 tick 单次 DSL 调用 < 5 μs 平均（远低于 33ms tick 预算）。

---

## 9. DSL 编译流水线

### 9.1 在 Build 期进行

```
data/source/Dsl.xlsx
       ↓
[Excel reader]
       ↓
DSL 文本片段（每片段唯一 dsl_ref ID）
       ↓
[Lexer + Parser]
       ↓
AST
       ↓
[Type checker]
       ↓
[Optimizer]
       ↓
[Bytecode emitter]
       ↓
data/generated/dsl.bytecode（FlatBuffers，按 dsl_ref 索引）
```

### 9.2 错误处理

**编译失败 = build 失败**：
- 语法错误：行号 + 错误类型
- 类型错误：变量 / 函数签名不匹配
- 未知函数：调用了非白名单函数
- 资源超限：bytecode > 1024、栈深 > 64

策划必须在提交前通过 build 校验。CI 失败阻塞合并。

### 9.3 优化通道

简单优化：
- 常量折叠（`2 * 3` → `6`）
- 死代码消除（`if false then ...`）
- 短路求值（`a && false` → `false`）

不做：
- 内联（DSL 没有函数定义）
- 循环优化（DSL 不允许循环）

### 9.4 运行时加载

`data/generated/dsl.bytecode` 在 session 启动时加载到内存：
- 服务端 CoreCLR 直接 mmap
- Unity 客户端打包到 AssetBundle，启动时读

加载时**不再 parse**，运行时只解释。

---

## 10. Action ↔ DSL 交互

### 10.1 Action 参数走 DSL

每种 Action 的字段可以是：
- 常量（直接写值）
- DSL 引用（写 `dsl_ref` 字段，运行时求值）

例 `DamageTarget`：
```
target_dsl_ref: "primary_target"     ← 求值返回 entity
damage_dsl_ref: "atk_minus_def"      ← 求值返回 float
```

Action `Apply()` 实现按需调用 DSL VM 求值。

### 10.2 DSL 内 emit Action

```
if target.has_status(Frozen) {
  emit ModifyDamage(Mul, 2.5);
  emit ApplyBuff(target, Shatter, 1000);
}
```

VM 遇到 `EMIT_ACTION` opcode 时调用 `ActionDispatcher.Dispatch`。当前 `ActionContext` 自动传入。

### 10.3 RunForEach 的循环

```
let targets = all_enemies_in_cone(caster, 5.0, 60.0);
emit RunForEach(targets, [DamageTarget(it, 100), ApplyBuff(it, Burn, 3000)]);
```

`RunForEach` Action 内部：
```cpp
public override void Apply(in ActionContext ctx) {
  var targets = EvalDsl(IterDslRef, ctx);  // list<entity>
  
  int max = MaxIterations > 0 ? MaxIterations : 32;
  for (int i = 0; i < targets.Count && i < max; i++) {
    var iterCtx = ctx.WithIterator(targets[i]);  // 把 it 绑定为 targets[i]
    foreach (var actionRef in EachActionsRef) {
      ActionDispatcher.Dispatch(actionRef.Type, actionRef.Id, iterCtx);
    }
  }
}
```

---

## 11. Excel Schema

### 11.1 Action 表通用约定

每种 Action 类型一张表：
```
data/source/actions/
├── HitboxActions.xlsx
├── DashActions.xlsx
├── ApplyBuffActions.xlsx
├── DamageTargetActions.xlsx
├── PlayVFXActions.xlsx
├── HitPauseActions.xlsx
├── CameraShakeActions.xlsx
└── ...
```

字段命名约定：
- 主键：`<action_type>_id`（int），如 `hitbox_id`, `dash_id`
- DSL 引用字段：以 `_dsl_ref` 结尾
- 时间字段：以 `_ms` 结尾

### 11.2 DSL 表

```
Dsl.xlsx
| dsl_ref | content              | return_type | notes |
| burn_dot_dmg | "0.2 * caster.atk_power * stacks" | float | 燃烧 DoT 公式 |
| pick_nearest | "nearest_enemy(caster, 5.0)"      | entity | |
```

**长 DSL** 单独存为 `data/source/dsl/*.dsl` 文本文件，Dsl.xlsx 引用文件路径。

### 11.3 校验

Build pipeline 强制：
- 所有 `_dsl_ref` 引用在 Dsl 表存在
- DSL 编译通过（语法、类型、资源）
- Action 类型枚举值在 §2.2 定义中
- 外键正确（如 `hitbox_def_id` 在 HitboxDefs.xlsx 存在）

---

## 12. 添加新 Action 的流程

团队约定**严格 7 步**，缺一不可：

1. **设计评审**：在 Action 类型枚举前**先讨论**是否真的需要新 Action（很多需求其实组合现有 Action 就够）
2. **更新 `ActionType` 枚举**（§2.2），分配新值
3. **实现 `Action` 子类**：参数 + `Apply()` 逻辑
4. **建 Excel 表**：定义字段
5. **更新 codegen**：让 build 工具能从 Excel 读到对应表生成 C# 数据
6. **写单元测试 + 端同测试**
7. **更新本文档 §3.x**

CI 自动检查前 5 项；后 2 项 code review 把关。

**审批层级**：
- 简单参数化扩展（如 `SpawnVFX2` 加一个 scale 参数）：1 工程师 review
- 新 Action 类型：Tech Lead + Combat Designer 联合 review
- 改 ActionType 枚举值：仅 Tech Lead，**且必须在新版本而非热更**

---

## 13. 测试与端同

### 13.1 单元测试

每种 Action / 每个内置函数有单元测试：

```csharp
[Test]
public void DamageTarget_DealsExpectedAmount() {
  var ctx = TestCombatContext.Create();
  var attacker = ctx.SpawnEntity(atk: 100);
  var victim = ctx.SpawnEntity(hp: 1000, def: 50);
  
  var action = new DamageTargetAction {
    DamageDslRef = "atk_minus_def",
    DamageType = DamageType.Physical,
  };
  action.Apply(ctx.For(attacker, victim));
  
  Assert.AreEqual(950, victim.Hp);
}
```

### 13.2 端同测试

每次 PR 跑：
- 1000 次随机场景生成 → 服务端 / 客户端 各跑一次
- 状态 diff 必须为空
- 性能不退化超过 5%

### 13.3 模糊测试（DSL VM）

- 随机生成 bytecode 序列（合法格式）
- 验证 VM 永远不崩溃 / 不死循环
- gas limit 总能终止

---

## 14. 性能预算

### 14.1 Action 派发

- 单次 `ActionDispatcher.Dispatch`：< 100 ns（数组索引 + 虚函数）
- 单次 Action `Apply` 平均：< 5 μs

### 14.2 DSL VM

- 简单求值（< 10 op）：< 200 ns
- 中等求值（< 50 op）：< 1 μs
- gas 上限（1000 op）：< 20 μs

### 14.3 整体

战斗 tick 单 cell 总 Action / DSL 耗时：
- 假设 100 active SkillInstance × 平均 0.5 event/tick × 5 μs = 250 μs
- 加上 buff handler 派发：~500 μs / tick
- 占副本 33ms tick 的 ~1.5%，富余

---

## 15. FAQ 与反模式

### Q1: DSL 没有 string 类型，如何写"如果敌人是 boss"？

通过 Tag / Flag 体系：实体有 `tags: list<int>`（boss / elite / minion），DSL 用 `target.has_tag(TAG_BOSS)`。Tag id 是整数，不需要 string。

### Q2: 为什么 emit 白名单不允许 `SpawnSubSkill`？

避免 DSL 沦为脚本。子技能调度是 Action 表层面的事，应在 Excel 里明确配置（便于策划查找"谁触发了谁"），不要藏在 DSL 文本里。

### Q3: DSL 性能会成为瓶颈吗？

不会。即使 1000 次/秒调用 × 5μs = 5ms/秒，远低于单 tick 33ms 预算。极端用例（boss 释放 AoE 命中 50 个目标各跑 on_hit DSL）也只有几 ms。

### Q4: 为什么 `RunForEach` 限 32 次迭代？

防止"打中 200 个怪一锁服 100ms"。32 是经验上限：
- 大多数 AoE 敌人数 < 20
- 极端 boss 战 ~30 召唤物
- 32 ≥ 实际需求 + 安全余量

需要更多时显式配置 `max_iterations`，但要 review。

### Q5: 客户端能 emit 服务端的 Action 吗？

**不能**。客户端只执行 `IsVisualOnly = true` 的 Action（VFX/audio/camera）。所有改世界状态的 Action 必须服务端执行。

客户端预测时本地执行的是"自己复刻一份相同 Action"，输出仅限本地视觉，不写回任何状态（StatCache / EntityState 都是只读视图）。

### Q6: DSL 能调用其他 DSL 吗？

**不能直接**。但可以通过 Action 间接：
```
emit DamageTarget(target, dsl_ref="formula_a");
emit DamageTarget(target, dsl_ref="formula_b");
```
两个 DSL 各自独立调用。

不支持 DSL 内嵌套调用 DSL，避免栈溢出 + 调用图复杂。

### Q7: Action 的客户端预测和服务端执行结果不一致怎么办？

按 Action 类型分类处理：
- **位置类**（Dash 等）：走 `MOVEMENT_SYNC.md` 的和解机制
- **数值类**（DamageTarget）：客户端不预测伤害数字（等服务端推送）
- **状态类**（ApplyBuff）：客户端只预测自施 buff，他人施加等推送
- **视觉类**（VFX / Camera）：客户端权威，无需对齐

### Q8: 如何调试 DSL 错误？

工具支持：
- 编辑期：DSL 编辑器（VS Code 插件）实时语法 / 类型检查
- 运行时：DSL VM 异常时 dump 完整调用栈到日志（包含 dsl_ref / pc / 局部变量）
- 回放：Combat Replay 系统记录每次 DSL 调用，可单步重播

### Q9: 新内置函数怎么加？

类似新 Action 的 7 步流程：
1. 设计评审（是否能用现有函数组合）
2. 在内置函数注册表加条目
3. 实现 C# 函数（端共享）
4. 加单元测试
5. 更新 §7 文档

注意：新内置函数会进入字节码 ABI，不能轻易删除（旧 bytecode 引用会失效）。

### Q10: Action 数量预期到多少？

合理上限 ~80 种。超过这个数说明抽象不够好，需要回顾合并。

实际经验：
- 启动 P2 时 ~20 种基础 Action
- 上线时 ~50 种
- 长期 stable 在 60–80 种

---

### 反模式清单

- ❌ 把复杂逻辑全塞 DSL（导致难读，应拆 Action）
- ❌ 在 DSL 里做大量数据查询（性能浪费，应预计算到 Action 参数）
- ❌ 为单个技能添加新 Action 类型（应用 custom_handler）
- ❌ 跨 Action 类型共享字段（每种 Action 自己的 schema）
- ❌ 在客户端 Visual Action 里写权威逻辑（混淆责任）
- ❌ DSL 内 emit 不在白名单的 Action（破坏分层）
- ❌ 用 `RunForEach` 处理玩家 input（玩家输入由 Movement 系统处理）
- ❌ 同一 dsl_ref 多处复制粘贴（应抽取共用）
- ❌ DSL 里依赖求值副作用顺序（保持纯函数风格）
- ❌ 新加 Action 不写测试（端同 bug 不及早发现 = 灾难）

---

## 16. 里程碑

| 阶段 | 交付 |
|---|---|
| P0 末 | Action 基础设施 + Dispatcher；5 种基础 Action（PlayAnim, SpawnHitbox, ApplyBuff, DamageTarget, Dash） |
| P1 中 | DSL parser + bytecode VM；20 个内置函数 |
| P2 初 | 完整 Action 全集（§3 所有项）；DSL 编译流水线 |
| P2 中 | RunIf / RunForEach；端同测试覆盖 100% Action |
| P2 末 | 性能优化（< 1.5% tick 占用） |
| P3 | Visual Action 完整：HitPause / CameraShake / CameraSlomo（配合手感） |
| P4+ | DSL 编辑器（VS Code 插件）；策划友好工具 |

---

## 17. 文档维护

- **Owner**：Tech Lead + Combat Designer
- **关联文档**：
  - `SKILL_SYSTEM.md`（Timeline / SM 用 Action）
  - `BUFF_SYSTEM.md`（Handler 用 Action）
  - `STAT_AND_DAMAGE.md`（DamageTarget / HealTarget 详细公式）
  - `HIT_VALIDATION.md`（SpawnHitbox 详细机制）
  - `MOVEMENT_SYNC.md`（Dash / Launch 等位移 Action）
  - `COMBAT_FEEL.md`（HitPause / Camera Action 设计）
  - `DETERMINISM_CONTRACT.md`（执行器必须遵循）

---

**文档结束。**

**核心纪律重申**：
1. **Action 是积木，不是脚本**：策划用预定义积木拼装，不是写程序
2. **DSL 受限是优势不是劣势**：限制让端同、性能、可维护都受益
3. **emit 白名单不可绕过**：每加一个白名单条目要全员 review
4. **Visual Action 严格客户端独占**：服务端只广播事件
5. **测试覆盖 100%**：Action / 内置函数 / DSL 必须自动化端同测试
