# 技能系统设计（Skill System）

> **用途**：Atlas 战斗系统的"龙骨文档"。定义技能如何表达、如何执行、如何端同、如何被其他系统（buff/movement/animation/AI）集成。
>
> **读者**：工程（必读）、战斗策划（必读）、动画师（§11 相关）、工具开发（§13 相关）。
>
> **状态**：草案 v0.1 — 待团队评审。
>
> **前置文档**：`OVERVIEW.md`、`00_foundations/DETERMINISM_CONTRACT.md`
>
> **下游文档**：`BUFF_SYSTEM.md`、`HIT_VALIDATION.md`、`COMBAT_FEEL.md`、`COMBAT_EVENT_ORDERING.md`、`STAT_AND_DAMAGE.md`

---

## 1. 设计目标与边界

### 1.1 目标

1. **端同一致**：服务端和客户端用同一份代码 + 同一份数据执行技能；伤害、状态变化完全一致
2. **数据驱动**：策划用 Excel 配置 80% 以上技能，无需工程介入
3. **策划可读**：配置结构反映"技能做了什么"而非"参数有哪些"
4. **性能可扩展**：支撑 2000+ 技能 × 400 实体/cell 同时存在的规模
5. **编辑器友好**：Unity Editor 可视化技能时间轴，实时预览
6. **手感保障**：客户端预测响应即时，服务端仲裁权威

### 1.2 非目标

- **不做通用脚本语言**：Atlas DSL 故意受限，不支持任意循环 / 递归 / 不受控的计算
- **不是 UE5 GAS 的克隆**：GAS 适合单客户端权威模型，Atlas 是服务端权威
- **不做图灵完备**：表达力刻意收敛，避免策划写出"像代码一样难懂的技能"
- **不直接处理动画**：动画播放是 Action 的副作用，技能系统不管 IK / blend tree 细节

### 1.3 能力矩阵

| 能力 | 支持方式 |
|---|---|
| 线性单段技能（普攻、简单主动技） | Pure Timeline |
| 蓄力技能（引导 + 释放） | State Machine + Timeline |
| 连招（多段 combo） | State Machine，每段一 state |
| 持续施法（激光、机枪） | State Machine + loop state |
| 多段循环（N 发弹幕） | Loop state + variable |
| 分支条件（连击判定） | DSL 驱动 transition |
| 被击打断 | Wildcard transition (`* → interrupt on: stun`) |
| 子技能召唤（分裂、连锁） | `SpawnSubSkill` action |
| 位移类（冲锋、闪现） | `Dash` / `Launch` action + 服务端权威曲线 |
| 条件伤害（如 on_hit 时冰冻翻倍） | Hitbox 的 `on_hit` DSL/graph |
| Boss 特殊机制 | `custom_handler` 脚本兜底 |

---

## 2. 总体架构

### 2.1 三层 + State Machine

```
┌─────────────────────────────────────────────────────────┐
│ 层 4 (可选): Graph Editor                               │
│   可视化节点图，编译为 DSL                               │
├─────────────────────────────────────────────────────────┤
│ 层 3: State Machine (动态技能才有)                      │
│   States + Transitions + Variables                      │
├─────────────────────────────────────────────────────────┤
│ 层 2: Timeline                                          │
│   线性帧对齐事件序列，每个事件 → Action                 │
├─────────────────────────────────────────────────────────┤
│ 层 1: Action / DSL                                      │
│   原子执行单元（Hitbox, Dash, ApplyBuff...）            │
│   DSL 提供条件表达式和目标选择                          │
└─────────────────────────────────────────────────────────┘
```

### 2.2 设计原则

1. **Timeline 保持线性**：不引入 goto/loop/delay，确定性神圣
2. **动态性上移到 SM**：状态切换、循环、分支都是 SM 的职责
3. **Action 是纯副作用**：接收参数 → 执行 → 输出世界状态变化；不返回值
4. **DSL 瞬时执行**：无 wait/delay，触发即算完；复杂时间控制由 SM 承担
5. **SM 不嵌套**：一层状态机足够；如需子状态就拆成独立技能

### 2.3 执行流程

```
玩家按技能键
  ↓
客户端: 本地预表现（动画 + VFX）+ 发 AbilityRequest 给服务端
  ↓
服务端: 校验（冷却、资源、CC 状态）→ 创建 SkillInstance
  ↓
服务端: 每 tick 推进 SkillInstance:
  ├── 推进当前 State 的 Timeline
  ├── 触发到点的 TimelineEvent → 执行 Action
  ├── 检查 Transitions，处理状态切换
  └── 广播关键事件（技能开始、命中、结束）给相关客户端
  ↓
客户端: 对照 AbilityAck 验证本地预测
  ├── 一致 → 丢弃本地临时状态
  └── 不一致 → 回滚或平滑过渡
  ↓
技能结束 → 销毁 SkillInstance
```

---

## 3. Timeline 模型

### 3.1 数据结构

```csharp
public sealed class TimelineDef {
  public int                          Id;           // 索引用
  public int                          DurationMs;   // 状态内总时长；0 表示无固定时长（等 transition）
  public ImmutableArray<TimelineEventDef> Events;   // 按 TimeMs 升序
}

public abstract class TimelineEventDef {
  public int  TimeMs;           // 自状态 entry 起的毫秒偏移
  public byte ActionType;       // 枚举：Anim/Hitbox/Dash/...
  public int  ActionRef;        // 指向对应 Action 表的 ID
}
```

**关键约束**：
- `Events` 必须按 `TimeMs` 升序；Excel 导入时自动排序，运行时依赖此顺序优化
- `TimeMs` 精度：毫秒级；亚毫秒由 action 内部处理（很少用）
- 同 `TimeMs` 多事件：按 `Events` 数组顺序执行（即 Excel 行顺序）

### 3.2 执行语义

每 tick 由 `TimelineRunner` 推进：

```csharp
public sealed class TimelineRunner {
  TimelineDef _def;
  long _entryTick;
  int _lastFiredEventIdx;
  
  public void Tick(CombatContext ctx) {
    long elapsedMs = (ctx.CurrentTick - _entryTick) * ctx.TickDurationMs;
    
    while (_lastFiredEventIdx + 1 < _def.Events.Length) {
      var nextEvt = _def.Events[_lastFiredEventIdx + 1];
      if (nextEvt.TimeMs > elapsedMs) break;
      
      ActionDispatcher.Dispatch(nextEvt.ActionType, nextEvt.ActionRef, ctx);
      _lastFiredEventIdx++;
    }
  }
  
  public bool IsComplete(CombatContext ctx) {
    if (_def.DurationMs == 0) return false;  // SM 驱动
    long elapsedMs = (ctx.CurrentTick - _entryTick) * ctx.TickDurationMs;
    return elapsedMs >= _def.DurationMs;
  }
}
```

### 3.3 确定性遵从

- Tick 时间单位由 `CombatContext.TickDurationMs` 注入（开放世界 50、副本 33.33→向下取整 33）
- 浮点/随机严格按 `DETERMINISM_CONTRACT.md` §3–4
- Event 触发顺序**严格按数组顺序**，不依赖任何哈希/指针
- 跨 tick 精度：若一 tick 内 `elapsedMs` 跨越多个 event `TimeMs`，**全部按序触发**，不遗漏不乱序

### 3.4 简单技能示例

"普通斩击"——无分支、固定 600ms：

```
TimelineDef:
  DurationMs: 600
  Events:
    [  0] PlayAnim("slash_start")
    [ 50] SetFlag(super_armor, 300ms)
    [150] SpawnHitbox(hb_cone_slash_1)
    [300] SetFlag(cancel_window_open, 200ms)
    [450] ClearFlag(super_armor)
    [600] (隐式 End)
```

执行 2 个 tick（33ms × 2 = 66ms）时：
- `elapsedMs = 66`
- 触发事件索引 0（0ms）和 1（50ms）
- 事件 2（150ms）等下次 tick

---

## 4. State Machine 层

### 4.1 何时需要 SM

**不需要**：80%+ 技能是线性的，直接一条 timeline。
**需要**：
- 持续时长取决于输入（蓄力）
- 次数取决于变量（多段射击）
- 存在分支（连招续接判断）
- 可被打断（被击硬直）
- 持续到某条件（持续施法）

### 4.2 数据结构

```csharp
public sealed class SkillStateMachineDef {
  public string                           InitialState;
  public ImmutableArray<SkillStateDef>    States;
  public ImmutableArray<TransitionDef>    Transitions;
  public ImmutableArray<VariableDef>      Variables;
}

public sealed class SkillStateDef {
  public string           Id;
  public TimelineDef      Timeline;
  public bool             Loop;              // 自动重入
  public ImmutableArray<ActionRef> EntryActions;
  public ImmutableArray<ActionRef> ExitActions;
}

public sealed class TransitionDef {
  public string           FromState;         // "*" 通配
  public TransitionTrigger Trigger;
  public string           ConditionDsl;      // 可选
  public string           ToState;
  public int              Priority;          // 同 tick 多条命中时的优先级
}

public enum TransitionTrigger {
  OnTimelineEnd,        // 当前 state 的 timeline 自然结束
  OnTimeElapsed,        // 基于 condition 判断，每 tick 检查
  OnInput,              // trigger_param = input 名称（"attack" / "dodge"）
  OnCombatEvent,        // trigger_param = 事件类型（"stun" / "knockback"）
  OnVarChanged,         // trigger_param = 变量名
  OnHit,                // 本 skill 的某 hitbox 命中目标
}

public sealed class VariableDef {
  public string  Name;
  public VarType Type;    // Int / Float / Bool / EntityRef
  public string  Default;
}
```

### 4.3 执行流程（每 tick）

```csharp
public sealed class SkillRunner {
  public void Tick(CombatContext ctx) {
    // step 1: advance current state's timeline
    _timelineRunner.Tick(ctx);
    
    // step 2: collect candidate transitions
    var candidates = new List<TransitionDef>();
    foreach (var t in _def.Transitions) {
      if (t.FromState != "*" && t.FromState != _currentState) continue;
      if (!EvaluateTrigger(t.Trigger, ctx)) continue;
      if (!EvaluateCondition(t.ConditionDsl, ctx)) continue;
      candidates.Add(t);
    }
    
    // step 3: resolve by priority
    if (candidates.Count > 0) {
      var chosen = candidates.OrderByDescending(c => c.Priority)
                             .ThenBy(c => c.Id)
                             .First();
      TransitionTo(chosen.ToState, ctx);
    }
    // step 4: handle natural loop
    else if (_currentStateDef.Loop && _timelineRunner.IsComplete(ctx)) {
      TransitionTo(_currentState, ctx);  // 重入自己
    }
  }
  
  void TransitionTo(string newState, CombatContext ctx) {
    RunActions(_currentStateDef.ExitActions, ctx);
    _currentState = newState;
    _currentStateDef = _def.States.First(s => s.Id == newState);
    _entryTick = ctx.CurrentTick;
    _timelineRunner = new TimelineRunner(_currentStateDef.Timeline, _entryTick);
    RunActions(_currentStateDef.EntryActions, ctx);
  }
}
```

**关键纪律**：
- 同 tick 不连续跳转：一次 tick 只做一次 transition，避免级联爆炸
- Wildcard transition（`*`）优先级通常最高，用于"任何状态都能被打断"
- Variables 持续到 SkillInstance 销毁，不跨技能实例共享

### 4.4 蓄力技能示例

"弓箭手三段蓄力":

```
States:
  windup:    Timeline (200ms 起手)
  charging:  Timeline (循环动画，无固定时长)
    事件:
      [   0] PlayAnim_Loop("charge_idle")
      [ 500] SetVar(charge_level = 2); PlayVFX("burst_2")
      [1200] SetVar(charge_level = 3); PlayVFX("burst_3")
  release:   Timeline (释放 + 伤害)
    事件:
      [  0] PlayAnim_Dsl("release_anim_by_level")  ← DSL 根据 charge_level 选动画
      [150] SpawnHitbox_Dsl("damage_formula_by_level")
      [400] SetFlag(cancel_window_open, 200ms)
      [600] (End)

Transitions:
  windup   → charging  OnTimelineEnd            prio 10
  charging → release   OnInput(release)         prio 20
  charging → release   OnTimeElapsed (t≥2000)   prio 10
  charging → interrupt OnCombatEvent(stun)      prio 100

Variables:
  charge_level: int = 1
```

---

## 5. Action 全集

所有 Action 继承自 `ActionDef` 基类，由 `ActionDispatcher` 按 `ActionType` 派发。每种 action 有专属 Excel 表（窄 schema，见 §10）。

### 5.1 Action 分类

| 类别 | Action | 说明 |
|---|---|---|
| **Animation** | `PlayAnim` | 切换 Animator state |
| | `PlayAnimLoop` | 循环播放（用于 charging 等） |
| | `SetAnimParam` | 设置 Animator 参数 |
| **Hitbox** | `SpawnHitbox` | 生成判定框，带 shape / damage / on_hit |
| | `RemoveHitbox` | 提前销毁 hitbox |
| **Movement** | `Dash` | 受控位移（曲线） |
| | `Launch` | 浮空击飞（抛物线） |
| | `LockMovement` | 禁用移动输入 |
| | `UnlockMovement` | 恢复移动输入 |
| **State** | `SetFlag` | 设置状态标志（super_armor/iframe/cancel_window） |
| | `ClearFlag` | 清除标志 |
| **Buff** | `ApplyBuff` | 施加 buff（触发 buff 系统） |
| | `RemoveBuff` | 移除 buff |
| **Spawn** | `SpawnProjectile` | 生成飞行物 |
| | `SpawnSubSkill` | 触发子技能（caster 可以是自己或目标） |
| **Resource** | `ConsumeMp` | 消耗魔法值 |
| | `ConsumeResource` | 通用资源（怒气/能量） |
| **Variable** | `SetVar` | 赋值变量（Dsl 或常量） |
| | `AddVar` | 变量累加 |
| **Damage** | `DamageTarget` | 直接伤害（不经 hitbox） |
| | `HealTarget` | 治疗 |
| **Visual**（客户端） | `PlayVFX` | 粒子特效 |
| | `PlaySound` | 音效 |
| | `CameraShake` | 相机抖动 |
| | `HitPause` | 顿帧（核心打击感） |
| | `CameraSlomo` | 慢镜头 |
| **Control** | `CancelSkill` | 立即结束本技能 |

**Visual 类 action**：
- 服务端广播事件通知客户端执行
- **服务端不执行** visual action（纯客户端表现）
- 因此 Visual action 不受 §3.3 确定性约束

### 5.2 Action 接口

```csharp
public abstract class Action {
  public int Id;
  public abstract ActionType Type { get; }
  public abstract void Apply(SkillContext ctx);
}

public sealed class SpawnHitboxAction : Action {
  public override ActionType Type => ActionType.SpawnHitbox;
  
  public int      HitboxDefId;     // 指向 HitboxDefs 表
  public int      AttachBoneId;    // 附着骨骼（0 = 世界空间，相对 caster）
  public int      LifetimeMs;
  public string   OnHitDslRef;     // 可选：命中时的 DSL 逻辑
  
  public override void Apply(SkillContext ctx) {
    var def = ctx.Defs.Hitboxes[HitboxDefId];
    var instance = ctx.World.SpawnHitbox(
      owner: ctx.Caster,
      def: def,
      attachBone: AttachBoneId,
      lifetime: LifetimeMs);
    
    if (!string.IsNullOrEmpty(OnHitDslRef)) {
      instance.OnHit = ctx.Defs.Dsl[OnHitDslRef];
    }
  }
}
```

### 5.3 新增 Action 的流程

团队约定：添加一个新 `ActionType` 必须**同时**完成：

1. 在 `ActionType` 枚举中注册
2. 继承 `Action` 基类实现 C# 类
3. 建对应 Excel sheet，定义字段
4. 更新 codegen 支持该 sheet
5. 在 `ActionDispatcher` 注册 Type → Action 映射
6. 写单元测试
7. 更新本文档 §5.1

**缺一项不允许合并**。CI 静态检查前 4 项，后 3 项 code review 把关。

---

## 6. DSL 层

### 6.1 设计目标

- **策划/程序员都能读**：语法接近自然语言
- **刻意受限**：无任意循环、无副作用函数、无状态
- **端同执行**：编译到 bytecode，服务端/客户端相同解释器
- **适用场景**：
  - Transition condition（`t >= 2000`）
  - Hitbox on_hit 判定（冰冻翻倍）
  - Action 参数计算（damage = atk × formula）
  - Target 选择器（nearest_enemy_in_cone）
  - Timeline event 的可选条件触发

### 6.2 数据类型

- `int`（32-bit）
- `float`（IEEE 754，通过 `Atlas.CombatCore.Math`）
- `bool`
- `entity_ref`（不透明，只能通过函数查询属性）
- `string`（仅作枚举标识符，不支持字符串拼接运算）
- `vec3`

### 6.3 语法

```ebnf
expr       ::= literal | var_ref | func_call | binary_op | unary_op | if_expr
literal    ::= int_lit | float_lit | bool_lit | "null"
var_ref    ::= identifier                             // 本地变量 / 上下文变量
func_call  ::= identifier "(" arg_list ")"
binary_op  ::= expr op expr                           // + - * / % == != < > <= >= && ||
unary_op   ::= ("-" | "!") expr
if_expr    ::= "if" expr "then" expr "else" expr

stmt       ::= var_decl | assign | action_emit | if_stmt
var_decl   ::= "let" identifier "=" expr
assign     ::= identifier "=" expr
action_emit::= action_type "(" arg_list ")"           // e.g. DamageMul(2.5)
if_stmt    ::= "if" expr "then" stmt* ("else" stmt*)?
```

### 6.4 内置函数（白名单）

```
# 上下文访问
caster: entity_ref               # 施法者
target: entity_ref               # 本次 on_hit 的目标
skill_level: int
charge_level: int                # 仅蓄力技能有效

# 实体查询（纯函数）
e.hp: float
e.hp_max: float
e.mp: float
e.atk_power: float
e.defense: float
e.has_status(status_id: int): bool
e.status_stacks(status_id: int): int
e.position: vec3
e.is_dead: bool
e.is_ally_of(other: entity_ref): bool

# 目标选择器
nearest_enemy(caster, range: float): entity_ref
all_enemies_in_cone(caster, range, angle): list<entity_ref>
all_allies_in_radius(caster, range): list<entity_ref>
party_members(caster): list<entity_ref>

# 数学
min(a, b), max(a, b), clamp(x, lo, hi), abs(x)
lerp(a, b, t)
sin(x), cos(x), sqrt(x)         # 走 Atlas.CombatCore.Math

# 随机（必须走 DeterministicRng）
rand_float(lo, hi): float        # 自动派生种子
rand_bool(prob: float): bool

# Action 发射（只在 on_hit / Transition / Action 参数计算场景有效）
DamageMul(mul: float)
ApplyStatus(status_id, duration_ms, stacks?)
SpawnVFX(vfx_id)
# ... 等与 Action 全集对应
```

### 6.5 Bytecode VM

编译到**栈式字节码**，运行时解释：

```
# opcode 示例
PUSH_INT_CONST <value>
PUSH_FLOAT_CONST <value>
LOAD_CTX_VAR <name_id>        # caster, target, skill_level
LOAD_LOCAL <index>
STORE_LOCAL <index>
CALL_FUNC <func_id> <argc>
JUMP_IF_FALSE <offset>
JUMP <offset>
BINARY_ADD, BINARY_SUB, ..., BINARY_EQ, BINARY_LT
UNARY_NEG, UNARY_NOT
EMIT_ACTION <action_type_id>  # 栈顶参数构造 action
RETURN
```

**VM 约束**：
- **栈深上限 64**（防恶意配置）
- **bytecode 长度上限 1024 opcodes**
- **执行步数上限 1000** per DSL 调用（gas limit）
- 超出任一限制：运行时错误，技能被视为 "custom_handler 失败"，服务端拒绝释放

### 6.6 实现组织

```
csharp/Atlas.CombatCore/
├── Dsl/
│   ├── DslParser.cs        // text → AST
│   ├── DslCompiler.cs      // AST → Bytecode
│   ├── Bytecode.cs         // 数据结构
│   ├── DslVm.cs            // 解释器（端同）
│   └── Builtin/            // 内置函数实现
│       ├── EntityQueries.cs
│       ├── TargetSelectors.cs
│       └── MathFunctions.cs
```

**解析/编译在 build 期进行**（codegen 工具），运行时只加载 bytecode 不做 parse，避免解析性能开销。

---

## 7. Graph 层（可选，Phase 3+）

**不在 P0–P2 范围**。视 DSL 使用情况决定是否上。

触发条件（见 OVERVIEW §11.1）：DSL 使用量达到一定规模、策划反馈"想直观地看条件分支"、团队有工具开发资源投入时再启动。

**核心设计**：Graph 只是 DSL 的**可视化 UI 层**，编译目标仍是 DSL bytecode。即：

```
Graph(.asset) ──[Unity Editor 工具]──► DSL(.txt) ──[build]──► Bytecode(.bin)
```

这保证了无论是否用 Graph，**运行时执行路径唯一**（bytecode 解释器），避免双轨复杂度。

详细设计延后至 `SKILL_GRAPH_EDITOR.md`（未来文档）。

---

## 8. Custom Handler 逃生舱

5–10% 技能天生无法完全数据化（Boss 多阶段、剧情技能、特殊玩法）。提供**脚本钩子**：

```
Skills.xlsx:
skill_id | name       | mode        | custom_handler
1001     | 斩击        | Timeline    | (空)
2500     | 龙王·毁灭   | Script      | DragonKingPhase2Skill
```

Script 模式下：
- **基础数值仍走 Excel**（伤害系数、冷却、MP）
- **执行逻辑走自定义 C# 类**，位于 `src/scripts/skills/`
- 由 CoreCLR 脚本层热加载（支持不停服更新）
- 必须实现 `ISkillHandler`：

```csharp
public interface ISkillHandler {
  void OnCast(SkillInstance inst, CombatContext ctx);
  void OnTick(SkillInstance inst, CombatContext ctx);
  void OnEnd(SkillInstance inst, CombatContext ctx);
  void OnInterrupt(SkillInstance inst, CombatContext ctx);
}
```

**约束**：
- custom_handler 比例**不得超过 15%**，超过说明 Action 库抽象不足
- 每个 custom_handler 必须有单独的单元测试
- 必须严格遵守确定性契约（不可以用作弊"反正我自己控制"）

---

## 9. 运行时生命周期

### 9.1 SkillInstance

```csharp
public sealed class SkillInstance {
  public long     InstanceId;       // 全局唯一（uint64）
  public int      SkillId;
  public EntityRef Caster;
  public EntityRef TargetHint;      // 施法瞬间选定的目标（可为空）
  public int      Level;
  public long     StartTick;
  public string   CurrentState;
  public long     StateEntryTick;
  public Dictionary<string, Variant> Variables;
  
  public TimelineRunner TimelineRunner;
  public ISkillHandler  Handler;    // null = 纯数据技能
}
```

### 9.2 生命周期

```
创建 (Cast)
  ├── 校验冷却、资源、CC 状态、视距
  ├── 扣除 MP / 物品
  ├── 实例化 SkillInstance
  └── 进入 InitialState，触发 EntryActions
Tick
  ├── 推进 Timeline
  ├── 评估 Transitions
  └── 执行到点的 Actions
结束 (End / Cancel / Death)
  ├── 触发当前 state 的 ExitActions
  ├── 清理关联 Hitbox / SubSkill
  └── 销毁 SkillInstance
```

### 9.3 Cell 内管理

```csharp
public sealed class CellSkillSystem {
  List<SkillInstance> _activeSkills;
  
  public void Tick(CombatContext ctx) {
    for (int i = _activeSkills.Count - 1; i >= 0; i--) {
      var inst = _activeSkills[i];
      inst.Tick(ctx);
      if (inst.IsFinished) {
        inst.Cleanup(ctx);
        _activeSkills.RemoveAt(i);
      }
    }
  }
}
```

**关键**：
- **SkillInstance 属于 caster 所在的 cell**
- Caster 跨 cell 时 SkillInstance 随之迁移（**但战斗中实体锁定 cell**，见 OVERVIEW §5.3）
- 因此实践中**SkillInstance 在其生命周期内不跨 cell**，简化了设计
- SubSkill 可以有独立 caster（如 Boss 召唤的 minion 施法），每个 SubSkill 是独立 SkillInstance

---

## 10. Excel Schema 规范

### 10.1 总览

```
data/source/
├── Skills.xlsx              # 总表
├── SkillTimelines.xlsx      # Timeline 事件
├── SkillStates.xlsx         # 动态技能的 State 定义
├── SkillTransitions.xlsx    # SM Transitions
├── SkillVariables.xlsx      # SM Variables
├── actions/
│   ├── HitboxActions.xlsx
│   ├── DashActions.xlsx
│   ├── LaunchActions.xlsx
│   ├── ApplyBuffActions.xlsx
│   ├── SpawnProjectileActions.xlsx
│   ├── SpawnSubSkillActions.xlsx
│   ├── VfxActions.xlsx
│   ├── SoundActions.xlsx
│   ├── HitPauseActions.xlsx
│   ├── CameraShakeActions.xlsx
│   └── ...
└── dsl/
    └── Dsl.xlsx              # DSL 片段库
```

### 10.2 Skills.xlsx

| 字段 | 类型 | 含义 |
|---|---|---|
| `skill_id` | int | 主键 |
| `name` | string | 内部名 |
| `display_name` | string | 本地化 key |
| `category` | enum | Active/Passive/Ultimate |
| `mode` | enum | Timeline/StateMachine/Script |
| `cooldown_ms` | int | 冷却时长 |
| `mp_cost` | int | 魔法消耗 |
| `max_level` | int | 最大等级 |
| `icon` | string | UI 图标 key |
| `custom_handler` | string | 仅 mode=Script 时填 |

### 10.3 SkillTimelines.xlsx

| 字段 | 类型 | 含义 |
|---|---|---|
| `skill_id` | int | 外键 |
| `state_id` | string | 状态名（非 SM 技能为 `default`） |
| `time_ms` | int | 事件时间偏移 |
| `action_type` | enum | Hitbox/Dash/PlayAnim/... |
| `action_ref` | int | 指向对应 action 表的 ID |
| `condition_dsl_ref` | string | 可选 DSL 条件（不满足则跳过） |

### 10.4 SkillStates.xlsx

| 字段 | 类型 | 含义 |
|---|---|---|
| `skill_id` | int | 外键 |
| `state_id` | string | 状态名 |
| `duration_ms` | int | 0 = 等 transition |
| `loop` | bool | 自动重入 |
| `entry_actions_ref` | string | 多个 ref 分号分隔 |
| `exit_actions_ref` | string | 同上 |

### 10.5 SkillTransitions.xlsx

| 字段 | 类型 | 含义 |
|---|---|---|
| `skill_id` | int | 外键 |
| `from_state` | string | `*` = 通配 |
| `trigger_type` | enum | OnTimelineEnd/OnInput/OnCombatEvent/... |
| `trigger_param` | string | 如 `attack` / `stun` |
| `condition_dsl_ref` | string | 可选额外条件 |
| `to_state` | string | 目标状态 |
| `priority` | int | 同 tick 冲突时 |

### 10.6 Action 表通用规则

- 每种 `ActionType` 一张表
- 主键：`<action_type>_id`（如 `hitbox_id`, `dash_id`）
- 字段：该 action 所需参数
- 外键必校验（如 `damage_coef_formula` 指向 Dsl.xlsx 的 id）

### 10.7 校验规则（Build pipeline 强制）

- 所有外键必须存在
- `time_ms` 在 Timeline 内不得重复过多（同 time_ms 超过 8 个事件告警）
- Dynamic skill 必须有 SkillStates 且包含 `InitialState`
- 所有 DSL 片段必须编译通过
- `custom_handler` 引用必须在 `src/scripts/skills/` 找到对应类

**违例阻塞 build**，不是警告。

---

## 11. 与其他系统的集成

### 11.1 Buff 系统

**接口**：
- Skill → Buff: 通过 `ApplyBuff` / `RemoveBuff` action
- Buff → Skill: 通过 Buff 的 handler 调用 `SpawnSubSkill` action

**共享基础设施**：
- 都使用相同的 Action / DSL 执行器
- 都遵循相同的确定性契约

**界限**：
- Skill 的 timeline 只描述技能本身，不描述后续 buff 持续期
- Buff 不拥有 timeline（事件驱动）
- 详见 `BUFF_SYSTEM.md`

### 11.2 Movement 系统

**接口**：
- `Dash` / `Launch` action 转化为 **MovementCommand** 发给实体的 MovementController
- MovementCommand 描述位移曲线（端共享数据）
- 服务端权威执行，客户端预测

**约束**：
- Skill 内不直接改 `entity.position`，必须走 MovementCommand
- Caster 被 CC（stun / root）时，MovementCommand 可能被 suppress（由 Movement 系统决策）
- 详见 `02_sync/MOVEMENT_SYNC.md`

### 11.3 命中判定 & Lag Compensation

- `SpawnHitbox` 产生 Hitbox 实体，进入 `HitValidation` 系统
- Hitbox 在 lifetime 内每 tick 扫描目标
- PvP 语境下 Hitbox 扫描走延迟补偿路径（详见 `02_sync/LAG_COMPENSATION.md`）
- 命中时触发 Hitbox 的 `OnHit` DSL（参数包含 target、damage_scale 等）

### 11.4 Animation（Unity 客户端）

**单向**：
- Skill timeline 的 `PlayAnim` action → 发送 `AnimCommand` 给客户端 Animator
- Client 收到 → 切 Animator state
- **Animation Notify 不回驱动 Skill 事件**（动作-网络解耦原则）

**动画驱动位移**：
- 构建期从 AnimationClip 提取位移曲线（root motion），存为 curve data
- Skill 的 `Dash` action 引用该曲线
- 运行时服务端 + 客户端用同一曲线，天然同步

### 11.5 AI

- AI 是 skill 的**消费者**，通过 `SkillSystem.Cast(caster, skill_id, target)` 释放
- AI 决策逻辑在行为树 / 状态机（独立系统，见 `04_ai/`）
- Boss 特殊技能走 custom_handler 获得最大灵活度

### 11.6 网络同步

**三类事件广播**：
- `SkillStart { instance_id, caster, skill_id, target, level, start_tick, seed }`
- `SkillStateChange { instance_id, new_state, entry_tick }`
- `SkillEnd { instance_id, reason }`

**客户端预测**：
- 客户端立即创建本地 SkillInstance
- 服务端 `SkillStart` ack 后对照
- 一致 → 继续；不一致 → 服务端权威替换

详见 §12 客户端预测。

---

## 12. 客户端预测

### 12.1 策略分层

**Tier 1: 无预测（最保守）**
- 超长 Boss 技能（> 2s 前摇）
- 远端其他玩家释放的技能
- 等服务端事件到达才开始播
- 优点：绝对一致；缺点：有延迟感

**Tier 2: 乐观预测（默认）**
- 自己释放的普通技能
- 客户端立即播放动画、特效、音效
- 命中反馈等服务端 ack
- 服务端拒绝时回滚动画

**Tier 3: 完整预测（高手感技能）**
- 普攻、连招起手
- 本地完整执行 Timeline 包括 hitbox 生成（仅视觉）
- 命中仍等服务端 ack
- 这是手感最好的档位，但回滚最复杂

**选择**：每个技能在 Skills.xlsx 里配 `prediction_tier` 字段。

### 12.2 预测回滚

若服务端拒绝（超出冷却、MP 不足、被 CC 等）：
- 发 `SkillReject { reason }`
- 客户端：
  - 立即停止动画（Animator 切 `Idle`）
  - 清理预测生成的 VFX / Hitbox
  - 提示玩家（UI 闪烁 / 音效）
- 刻意**不走 `visual_smoother` 插值**——玩家需要感知"你没放出来"

### 12.3 预测和解（非拒绝但有偏差）

- 服务端 `SkillStart` ack 包含：`start_tick`、`seed`、`variables_init`
- 客户端本地 instance 对照：
  - `start_tick` 偏差 ≤ 2 tick：接受，更新本地 start_tick
  - 偏差 > 2 tick：视为状态严重不同步，硬重置
- `seed` 必须匹配，否则随机部分（如暴击）会分叉

### 12.4 远端技能表现

**接收流程**：
- `SkillStart` 到达，带 `start_tick`
- 转换为本地时间：`local_time_for_event = start_tick × tick_ms - interp_delay`
- 技能表现的起点延迟到该 local 时间（通常比"现在"还晚 ~100ms）
- 之后按 server 广播的事件表现

**这保证了远端技能和远端实体位置在同一"过去时间"上**，视觉一致。

---

## 13. 调试与测试

### 13.1 单元测试

每个 Action / DSL 函数 / State Machine 模式有单元测试，例如：

```csharp
[Test]
public void SpawnHitbox_DealsDamageOnEnemyInRange() {
  var ctx = CombatContext.CreateTest();
  var caster = ctx.SpawnEntity(hp: 100, atk: 100);
  var target = ctx.SpawnEntity(hp: 100, defense: 50);
  ctx.SetPosition(target, caster.Position + Forward * 2.0f);
  
  var hb = ActionLibrary.SpawnHitbox(shape: Cone(range: 3, angle: 60), damage_mul: 1.0f);
  hb.Apply(ctx.ForCaster(caster));
  
  ctx.Tick(1);  // 推进一 tick 让 hitbox 扫描
  
  Assert.AreEqual(50, target.Hp);  // 100 - 100 × 50% = 50
}
```

### 13.2 端同测试（强制）

每新增 Action / DSL 函数：
- 服务端仿真与客户端仿真跑 1000 次随机场景
- 最终状态 diff 必须为空
- 违反 → PR 不得合并

### 13.3 Timeline 可视化

Unity Editor 工具（详见 `09_tools/FRAME_DATA_EDITOR.md`）：
- 读取 SkillTimeline 数据
- 时间轴 Gantt 图展示所有事件
- 实时预览：在 Editor 内播放技能，看 Animator / VFX / Hitbox 同步
- 帧扫描：单帧步进观察

### 13.4 战斗回放

- 录制：`SkillSystem` 记录每个 instance 的 `(start_tick, state_changes, random_seeds, inputs)` 事件流
- 回放：加载记录 → 以同种子重建 instance → 验证状态复现
- 用于：bug 复现、平衡 review、反作弊审计

---

## 14. 性能预算

### 14.1 目标

- 单 cell 同时 ≤ 300 active SkillInstance（估算：400 实体 × 平均 0.75 技能在身）
- 单 tick SkillSystem 耗时：**≤ 2 ms**（占 33ms tick 的 6%）
- 单 Timeline event 执行：**≤ 5 μs**

### 14.2 优化策略

- Event 分派走 **开关索引 + 函数指针**，避免 vtable / reflection
- Action 实例**不分配 GC**（C# 中大量用 struct + `in` 参数）
- Hitbox 扫描用**空间分区** + **预计算候选列表**
- Variables 用 **struct-of-arrays**（热点按 layout 访问）
- DSL bytecode 解释器尽量 branchless

### 14.3 监控指标

- 每 cell 每秒 skill instance 数量（峰值、平均）
- 每 tick SkillSystem.Tick 耗时分布
- DSL 执行次数 / 平均 opcode 数
- Action 类型分布（用哪些多、哪些少）

---

## 15. FAQ 与反模式

### Q1: 为什么不用 UE5 GAS 的思路？

GAS 是为 UE5 Replication 设计的，假设"客户端是游戏的一部分权威"。Atlas 是严格服务端权威，UE5 客户端预测回滚机制不能复用。另外我们客户端是 Unity，GAS 是 UE5 C++ 代码，更不可移植。

**吸收 GAS 思路**：标签（tag）系统、效果（GameplayEffect）编排——这些我们用 State Flag + Buff 覆盖。

### Q2: DSL 为什么不用 Lua / Python?

- **确定性**：Lua/Python 跨平台浮点不保证一致
- **性能**：脚本语言解释开销大
- **安全**：可无限循环、深度递归
- **策划门槛**：语法对非程序员仍不友好

自研 DSL 刻意限制（无循环、有 gas limit、白名单函数），换来上述问题的全部解决。

### Q3: Timeline 事件太多影响性能吗？

典型技能 10–30 事件，执行耗时全部 < 50 μs。2000 技能 × 400 实体也只是"Excel 数据量大"而非"运行时压力大"——运行时只看激活的 instance。

### Q4: Buff 给的属性加成如何影响 Skill 内的伤害公式？

`caster.atk_power` 这个 DSL 变量在求值时**走 StatCache**（见 `STAT_AND_DAMAGE.md`），已经包含所有 active buff 的 modifier 聚合。Skill 无需知道 buff 细节。

### Q5: 施法打断后 Exit Actions 会触发吗？

**会**。无论是自然结束、中断、玩家取消还是死亡，Exit Actions 都触发（除非 caster 死亡时该 action 涉及操作 caster 本身，由 action 内部判断空引用）。

这是为什么 Exit Actions 适合放**清理类逻辑**（停止 loop anim、清理 UI、结算资源返还）。

### Q6: 同时激活多个同技能 instance（比如多段冲刺）怎么办？

默认 **同一 caster + 同一 skill_id 只能有 1 个 active instance**，第二次 Cast 被拒绝或替换（配置决定）。

如果设计需要多 instance（如 AoE 召唤物）：
- 用 `SpawnSubSkill`，每个 SubSkill 是独立 skill_id 和独立 instance
- 不要靠"允许多 instance"走捷径

### Q7: Timeline 的 time_ms 不能表达"第 N 帧"吗？

原则上毫秒是绝对时间，跟帧率解耦。但 Unity 动画通常按帧设计（30fps / 60fps），建议：
- 策划/动画师在 Unity 里按帧调试动画
- Excel 里填 `frame_at_30 × 33.33 → 向上取整 ms`（工具自动）
- 未来可增加 `time_frames@30` 列作为辅助输入，codegen 转 ms

### Q8: 能不能在 Timeline 里写"如果 target 是冰冻状态则触发额外 action"?

**可以**，但**不直接在 Timeline 层**，而是通过：
- Action 层：`SpawnHitbox` 的 `OnHitDslRef` 里写 DSL 判断
- SM 层：`ConditionalBranch` transition（如果技能本身要分支）

Timeline 事件的可选 `condition_dsl_ref` 字段支持简单情况。

### Q9: 技能动画里的 root motion 能用吗？

**不用 Unity 运行时 root motion**（非确定），而是：
- 在构建期从 AnimationClip 提取 root motion 为曲线数据
- Skill Dash action 引用该曲线
- 运行时服务端+客户端都用同一曲线数据
- 表现上看起来像 root motion，实际是 curve-driven movement

### Q10: 我发现一个 Action 组合不了我想要的效果，怎么办？

先问：
1. 能否用 `SpawnSubSkill` 拆分成两个技能？
2. 能否用已有 Action + DSL 组合覆盖？
3. 确实缺失的是通用能力，且未来会反复用？—— 加新 Action 类型
4. 是纯粹个例？—— 用 custom_handler

**禁止**：为单一技能临时加 Action 字段污染通用表。

### 反模式清单

- ❌ 把技能逻辑放 custom_handler 图省事（应先考虑 Action 组合）
- ❌ Timeline 里加 goto/loop/wait 事件类型（违反线性原则，用 SM）
- ❌ 在 DSL 里放大量 if/else（应拆成 SM transitions 或独立技能）
- ❌ Action 里做"如果 X 则 Y 否则 Z"条件（应由 DSL 或上游 Action 决策）
- ❌ Timeline event time_ms 精度到 0.1ms（没意义）
- ❌ 用 custom_handler 是为了"性能优化"（99% 情况下 Action 层足够快）
- ❌ Buff 的持续效果放进技能 Timeline（buff 有专门系统）
- ❌ 通过全局变量在技能间传递状态（用 SM Variables 或 Buff）

---

## 16. 里程碑

| 阶段 | 交付 |
|---|---|
| P0 末 | TimelineDef / SkillRunner / 最小 Action 集（5 种）运行 |
| P1 末 | 与 Movement 系统集成；Dash/Launch action 端同 |
| P2 中 | 完整 Action 全集（§5.1 所有项）；Excel pipeline；5 个测试技能 |
| P2 末 | Buff 集成；DSL v1；20 个技能；端同自动测试 |
| P3 | 手感打磨，可能新增 Action（hit pause, camera 等） |
| P4+ | custom_handler 机制；特殊 boss 技能；工具 v1 |

---

## 17. 文档维护

- **Owner**：Tech Lead
- **关联文档**：
  - `OVERVIEW.md`（§7 难点一、二、三引用本文）
  - `DETERMINISM_CONTRACT.md`（所有执行必须遵循）
  - `BUFF_SYSTEM.md`（共享 Action 层）
  - `MOVEMENT_SYNC.md`（Dash/Launch 对接）
  - `HIT_VALIDATION.md`（Hitbox 判定）
  - `COMBAT_FEEL.md`（Visual action 设计：hit pause/camera）
  - `FRAME_DATA_EDITOR.md`（Unity Editor 时间轴工具）

---

**文档结束。**

**核心纪律重申**：
1. **Timeline 线性神圣**：动态性上移到 SM，不要"聪明的 Timeline"
2. **Action 是语言**：新增 Action 慎重，但建立共识后大胆扩展
3. **确定性优先**：所有执行路径遵守 `DETERMINISM_CONTRACT`
4. **custom_handler 是逃生舱不是高速通道**：比例必须 < 15%
5. **数据驱动**：能在 Excel 表达的就不写代码
