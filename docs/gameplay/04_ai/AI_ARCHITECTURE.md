# AI 架构（AI Architecture）

> **用途**：定义 Atlas 怪物 / NPC AI 的整体架构、决策模型、群组协作、感知系统。这是"AI 深度超越 BDO"的软目标的核心文档——让玩家感觉 Atlas 的怪物**有脑子**。
>
> **读者**：战斗策划（必读）、AI 设计（必读）、工程（§3、§5、§7 必读）、技术美术（§4 必读）。
>
> **状态**：草案 v0.1 — 待团队评审。
>
> **前置文档**：`OVERVIEW.md`、`SKILL_SYSTEM.md`、`BUFF_SYSTEM.md`、`COMBAT_FEEL.md`
>
> **下游文档**：`AI_LOD.md`（性能分层）、`BOSS_AI.md`（特殊机制）

---

## 1. 设计哲学

### 1.1 AI 服务于玩家体验

Atlas AI 的目的**不是**展示技术——是**让战斗有趣**。

具体表现：
- 怪物动作**可读**（玩家能预判）
- 怪物决策**有逻辑**（不会傻站着挨打）
- 怪物群组**有配合**（不只是"一群独立个体")
- Boss 战**有节奏**（机制清晰，奖励学习）
- 普通怪也有**性格**（哥布林狡诈、骷髅勇猛、巨魔狂躁）

### 1.2 与 BDO 的对比

BDO 怪物特点：
- 大多原地站着等玩家打（"练级靶子"）
- 简单巡逻 + 攻击模式
- 群组中各自独立行动
- Boss 阶段简单（HP 阈值切机制）

这给了 Atlas 拉开差距的机会：
- **群组配合**（合围、互相补位）
- **战术决策**（远程怪后撤、近战怪冲锋）
- **学习行为**（记住玩家的攻击 pattern）
- **多阶段 Boss**（机制 + 节奏 + 戏剧性）

### 1.3 设计目标

1. **行为可读**：玩家能从动作判断怪物意图
2. **变化但稳定**：同种怪物有相似行为 + 个体差异
3. **响应玩家**：玩家走近 / 攻击 / 逃跑都有合理反应
4. **群组协调**：多怪物同战场时有"集团意识"
5. **性能可承载**：100 怪同时仿真不卡服

### 1.4 非目标

- **不做"完美 AI"**：太聪明的 AI 让玩家挫败
- **不做机器学习实时学习**：上线版本固定行为，可调参不可学习
- **不做语音交互**：AI 不是聊天机器人
- **不做物理拟真**（如布料 / 流体反应）：太昂贵

---

## 2. 总体架构

### 2.1 三层模型

```
┌─────────────────────────────────────────────────┐
│  Strategic Layer (战略层)                        │
│  - 群组协作                                       │
│  - 跨实体决策（"我们一起合围")                     │
│  - Boss 阶段控制                                  │
├─────────────────────────────────────────────────┤
│  Tactical Layer (战术层)                         │
│  - 单实体行为树                                   │
│  - 状态机决策（攻击 / 逃跑 / 寻位）                 │
│  - 技能选择                                       │
├─────────────────────────────────────────────────┤
│  Reactive Layer (反应层)                         │
│  - 即时响应（被击中、看到玩家、听到声音）           │
│  - 中断当前动作                                   │
│  - Animator state 切换                            │
└─────────────────────────────────────────────────┘
```

### 2.2 行为树（Behavior Tree）作为战术层

每种怪物有一棵行为树（BT）：
- 节点类型：Sequence / Selector / Parallel / Decorator / Action
- 数据驱动（策划配置，工程实现 action 节点）
- 每 tick 自顶向下评估

例：哥布林士兵的 BT 草图：
```
Selector (优先级)
├── 反应：被攻击且 HP < 30%? → Run Away
├── 反应：盟友死亡 < 5s? → Frenzy Mode (3s 内 +50% 攻速)
├── 战术：玩家在攻击范围内? → Attack
├── 战术：玩家在视野内? → Pursue
└── 巡逻：定时巡逻路径
```

### 2.3 状态机（State Machine）作为反应层

某些反应不适合行为树（更轻量）：
- **Idle / Alert / Combat / Flee** 大状态切换
- 状态变化触发动画 / 行为树切换

```
StateMachine:
  Idle ──[感知玩家]──> Alert
  Alert ──[玩家 < 攻击范围]──> Combat
  Combat ──[HP < 阈值且 flee_chance > 0]──> Flee
  Flee ──[远离玩家]──> Idle
```

### 2.4 群组协调作为战略层

详见 §6。多怪物属于一个 group，group 协调他们的行动。

### 2.5 与 SKILL_SYSTEM 的关系

AI **使用** Skill 系统：
- AI 决策"我要释放技能 X" → 调用 `SkillSystem.Cast(skill_id, target)`
- 技能本身的执行（timeline / hitbox / damage）走 Skill 系统
- AI 不重新实现技能逻辑

AI 唯一独有的是**决策**：什么时候释放什么技能。

---

## 3. 单怪物的行为树

### 3.1 BT 数据结构

```
BehaviorTreeDef:
  RootNode
  
RootNode 可以是:
  Sequence: 依次执行子节点，全部成功才算成功
  Selector: 依次尝试子节点，第一个成功的算成功
  Parallel: 同时执行多个子节点
  Decorator: 修饰子节点（取反 / 重试 / 限频 / 冷却）
  Action: 叶节点，执行具体行为
  Condition: 叶节点，返回 bool
```

### 3.2 常见 Action 节点

| Action | 用途 |
|---|---|
| `MoveTo(position)` | 寻路到指定位置 |
| `MoveToTarget(entity)` | 追击目标 |
| `FaceTarget(entity)` | 转向目标 |
| `CastSkill(skill_id, target)` | 释放技能 |
| `Wait(duration_ms)` | 等待 |
| `PlayAnim(anim_id)` | 播放动画 |
| `Patrol(path_id)` | 巡逻 |
| `FleeFrom(entity, distance)` | 远离目标 |
| `EmitSound(sound_id)` | 发出声音（可被其他 AI 感知） |
| `BroadcastGroupSignal(signal)` | 通知群组 |

### 3.3 常见 Condition 节点

| Condition | 用途 |
|---|---|
| `HasTarget()` | 是否有目标 |
| `TargetInRange(distance)` | 目标距离 < N |
| `TargetInLineOfSight()` | 与目标视线无遮挡 |
| `HpBelow(percent)` | 自身 HP < % |
| `MpAbove(percent)` | MP 充足 |
| `SkillReady(skill_id)` | 技能 CD 到 |
| `HasStatus(buff_id)` | 自身有 buff |
| `TargetHasStatus(buff_id)` | 目标有 buff |
| `RandomChance(probability)` | 概率分支 |
| `CooldownReady(cooldown_id)` | 内部 cd 到（区别于技能 cd） |

### 3.4 Decorator 节点

| Decorator | 用途 |
|---|---|
| `Inverter` | 取反 |
| `Repeater(N)` | 重复 N 次 |
| `Cooldown(ms)` | 限制此节点重新执行的间隔 |
| `Timeout(ms)` | 超时强制返回失败 |
| `Once` | 仅执行一次（之后忽略） |
| `Random` | 随机化优先级 |

### 3.5 Tick 频率

不是每 server tick 都跑 BT（性能考虑）：
- 战斗中：每 100 ms 跑一次（10 Hz）
- 闲置：每 500 ms 跑一次（2 Hz）
- 远距离 LOD：更低（参见 `AI_LOD.md`）

BT 决策不需要 60 Hz——人类也不会每 16ms 重新决策。

### 3.6 决策与执行的解耦

BT 决策"释放技能 X" → 但技能本身按其 timeline 推进：
- BT 在某 tick 决定释放
- 调用 `SkillSystem.Cast`
- 技能的 anticipation / active / recovery 自然推进
- BT **下次 tick 不再覆盖**（除非高优先级中断）

这避免"BT 每 tick 都想换技能" 导致的频繁打断。

---

## 4. 怪物个性化

### 4.1 性格 = 行为树参数

不同怪物**复用相似的 BT**，但参数不同：

```yaml
goblin_scout:
  bt_template: "scout_template"
  flee_threshold: 0.3       # HP 30% 撤退
  vision_range: 20m
  skill_pool: [scout_attack, scout_dodge]
  
goblin_warrior:
  bt_template: "warrior_template"
  flee_threshold: 0.0       # 战死不退
  vision_range: 12m
  skill_pool: [warrior_slash, warrior_charge]
```

同一 template 不同参数 → 不同性格的怪物。

### 4.2 性格类别

设计上至少支持：

| 性格 | 特征 |
|---|---|
| Aggressive | 主动追击，HP 低也不退 |
| Cautious | 攻击后撤退，远程优先 |
| Cowardly | 1v1 不打，2+ 才围攻 |
| Berserker | HP 低反而更猛（"狂暴模式"） |
| Tactical | 利用地形 / 召唤帮手 |
| Tank | 站定吸收伤害保护队友 |

### 4.3 个性化通过个体差异

同种怪物个体之间略有差异：
- 巡逻路径不同
- 反应速度小幅波动（0.9–1.1×）
- 偶尔的"个性动作"（伸懒腰、磨牙、看天空）

让玩家**感觉"那个特别凶的哥布林"**而不是"哥布林 #137"。

---

## 5. 感知系统

### 5.1 玩家进入怪物视野

怪物的"视觉":
- 视野范围（如 20m）
- 视野角度（如 120°）
- 视线遮挡（建筑 / 地形）

```python
def CanSee(self_npc, target):
  if Distance(self_npc, target) > self_npc.vision_range:
    return False
  if AngleToTarget(self_npc, target) > self_npc.vision_angle / 2:
    return False
  if HasObstacleBetween(self_npc, target):
    return False
  return True
```

### 5.2 听觉

怪物可以"听到" 远处的声音：
- 玩家攻击 → 触发 `EmitSound(combat_noise, range=30m)`
- 范围内的怪物 → 进入 Alert 状态，开始警觉

### 5.3 触觉（被击中）

被击中是**最强信号**：
- 立即进入战斗状态
- 立即"知道" 攻击者位置
- 不依赖视觉条件

### 5.4 群组共享感知

参见 §6.3——群组成员共享感知信息。一个怪物看到玩家 → 整个群组都警觉。

### 5.5 感知冷却

防止"看一眼忘一眼" 的频繁切换：
- 进入 Alert 后保持至少 5 秒
- 失去目标后保持搜索 10 秒
- 完全失去后才回 Idle

让感知**有惯性**，更像生物。

---

## 6. 群组协作（Group AI）

### 6.1 GroupBrain 模型

每个**怪物群组**有一个虚拟 GroupBrain：
- 记录群组成员
- 协调成员行动
- 决定战术

```
GoblinCamp:
  members: [scout, warrior_1, warrior_2, shaman]
  state: Idle / Combat / Routing
  current_target: player_X (when in combat)
  formation: SurroundTarget / Defensive / Attack
```

### 6.2 GroupBrain 决策

当一个成员发现玩家：
1. 通知 GroupBrain
2. GroupBrain 设定 target
3. 分配每成员角色：
   - shaman → 远程支援（保持距离）
   - warrior → 近战包围
   - scout → 侧翼骚扰

每成员的 BT 受 GroupBrain 影响（GroupBrain 给个体 BT 设变量"我是 flanker"）。

### 6.3 共享感知

群组任一成员发现玩家 → 全员知道：
- shaman 在房子里看不到玩家，但 scout 看到
- shaman 通过群组共享获知玩家位置
- shaman 可以"远程支援"
- 玩家感受：被群组包围，无处藏身

### 6.4 阵型与位置

群组成员**有相对位置**：
- 标准阵型：tank 前 / dps 中 / heal 后
- 战斗中按阵型分布
- 一个成员死亡 → GroupBrain 重新分配位置

### 6.5 动态行为

GroupBrain 可触发战术：
- HP 低的群组 → 撤退集合 → 重整后再战
- 玩家强势 → 召唤援军（GroupBrain 决定，不是单个怪决定）
- Boss 出现 → 群组成员臣服 / 协助

### 6.6 群组规模上限

每群组最多 ~12 个成员：
- 大于 12 协调复杂
- 战场视觉混乱（玩家看不清谁是谁）
- 性能压力

更多怪物分多个群组。

---

## 7. 性能预算

### 7.1 目标规模

热点 PvE 场景：50 怪物 + 30 玩家。

### 7.2 单怪物每 tick 开销

```
BT tick: ~10–30 μs
感知扫描: ~5 μs
群组同步: ~2 μs
状态机: ~1 μs
总计: ~20–40 μs / 怪物 / 决策周期
```

战斗中决策周期 100 ms（10 Hz）：
- 50 怪 × 40 μs / 100 ms = 0.02 ms/ms = 2% CPU

非常充裕。

### 7.3 寻路（Pathfinding）

寻路最贵：
- 单次寻路 1–10 ms（依据距离 / 障碍）
- 50 怪同时寻路 → 不可能每 tick 都跑

策略：
- 寻路结果缓存（仅在目标变化时重算）
- 寻路任务分散到多 tick
- 简单环境用直线（无寻路）

### 7.4 视线检测

视线 raycast：< 5 μs。
但 50 怪 × 30 玩家 × 视线检测 = 1500 次 / 决策周期。

策略：
- 仅检查"在视野范围内" 的目标
- 共享 raycast 结果（同一 tick 多查询走缓存）

### 7.5 LOD

详见 `AI_LOD.md`。远距离怪物 AI 简化（甚至冻结）大幅省 CPU。

---

## 8. 玩家与 AI 的交互体验

### 8.1 战斗的节奏感

设计上，AI 应该让战斗**有起伏**：
- 玩家初见 AI → AI 警戒 / 试探（开始紧张）
- 战斗中段 → AI 全力（紧张高潮）
- AI 受伤 → 行为变化（如逃跑 / 狂暴）
- 玩家胜利 → AI 死亡的清晰反馈

不要"AI 启动后就一直一个状态打到死"——枯燥。

### 8.2 可预测但有变化

玩家应该能**学习** AI 行为：
- 哥布林会在玩家背后偷袭
- 法师怪物受到威胁会瞬移
- 巨魔每攻击 3 下喘息一次

但不能完全可预测（每次都一样 → 太死板）：
- 加入 ~20% 的随机
- 多种 attack pattern 之间随机切换
- 偶尔"惊喜动作"（小概率技能）

平衡点：玩家能预判**主体**，但仍有**意外**。

### 8.3 AI 的"愚蠢"也是设计

不要做"完美 AI" —— 玩家会感觉挫败。
适当"留破绽"：
- AI 不会立即从远程切换到近战（即使最佳决策应该这么做）
- AI 受击后有短暂"硬直" 让玩家有连击机会
- AI 不会针对玩家"完美预测"翻滚

让 AI 像**有生命的存在**而非冷冰冰的算法。

### 8.4 AI 与队友配合

PvE 副本中玩家组队，AI 应**意识到群体威胁**：
- 4 玩家一起攻击同一目标 → AI 知道是被多目标攻击
- AI 选择"AOE 反击"或"逃跑寻找单独目标"
- 让群组战斗有策略

### 8.5 AI 反应的可读性

玩家能从 AI 动作读出意图：
- AI 起手 = 即将攻击（玩家可闪避）
- AI 转向 = 切换目标
- AI 后退 = 准备远程或撤退
- AI 蹲下 = 蓄力大招

这要求 AI 行为与**动画 telegraph** 紧密绑定（参见 `ANIMATION_INTEGRATION.md §4.3`）。

---

## 9. AI 的内容产出

### 9.1 数据驱动设计

策划负责：
- BT 树形结构（用编辑器或 YAML 配）
- 性格参数（视野范围、HP 阈值、技能池）
- 群组配置

工程负责：
- BT action / condition 节点实现
- 寻路 / 视线 / 群组协调框架
- 性能优化

### 9.2 BT 编辑器

未来工具（参见 `09_tools/` 待写）：
- 可视化 BT 编辑器
- 实时调试（看哪个分支被命中）
- 参数调整 + 预览

P0–P2 用 YAML / Excel 配置；P3+ 上工具。

### 9.3 BT 模板复用

每种"原型怪"做一棵 BT 模板：
- Aggressive Melee
- Cautious Ranged
- Tank
- Berserker
- Tactical (with summons)
- ...

具体怪物 = 模板 + 参数。

工作量：~10 BT 模板 + 数百怪物配置 = 大幅减少新增怪物的成本。

### 9.4 怪物生态

地图填充考量：
- 同区域多种怪物 + 多种 BT
- 群组分布（不是单怪散落）
- 怪物等级分布
- 资源 / 怪物比例

详见 `world/MapDesign.md`（待写）。

---

## 10. 调试与可观察

### 10.1 AI 调试 UI

Editor 内开启时：
- 每怪物头顶显示 state
- BT 当前活跃节点高亮
- 视野范围 / 听觉范围 wireframe
- 决策日志（最近 5 个决策）

帮助策划 / 工程理解 AI 在做什么。

### 10.2 AI 录像

战斗回放工具（参见 `09_tools/COMBAT_REPLAY.md`，待写）：
- 录制 AI 决策流
- 可看到每次 BT tick 选了哪个分支
- 可看到为什么没攻击（cd? out of range? no LOS?）

### 10.3 玩家反馈

上线后收集反馈：
- "这个怪太蠢" / "这个怪太强"
- "练级点 boss 机制不清晰"
- "群组怪打不过"

针对性调参，不停服热更。

---

## 11. FAQ 与玩家关注问题

### Q1: AI 会用机器学习吗？

不会——上线版本固定行为：
- 机器学习不可控（玩家行为变化 → AI 失衡）
- 端同破坏（学习的权重不一致）
- 玩家无法学习"如何打 AI"（AI 一直变）

我们用**精心设计的固定行为 + 个体差异**，让 AI 看起来"像活的"但实际可预测。

### Q2: AI 会"想着"杀玩家吗？

不会有意识——AI 只是按规则决策。但**设计上**让玩家**感觉**有意识：
- 选择最弱的玩家攻击（"狡猾"）
- 群组合围（"协作"）
- HP 低狂暴（"愤怒"）

这些都是规则，不是意识。但视觉上让玩家产生情感反应。

### Q3: PvP 中怪物 AI 加入会破坏平衡吗？

PvP arena **不混入 AI 怪物**——保持纯玩家对战。

例外：1v1 mirror 模式中可能有"训练假人"，但那是被动靶子，不主动攻击。

### Q4: 一只怪物可以"知道"几公里外发生了什么吗？

不能——感知有距离限制（视野 20m / 听觉 30m）。
群组共享感知也仅限群组成员（通常 < 50m 范围）。

跨地图通讯不存在。

### Q5: BOSS 战 AI 比普通怪复杂多少？

参见 `BOSS_AI.md`。BOSS：
- 多阶段（每阶段独立 BT）
- 自定义 mechanics（custom_handler）
- 通常单独 BT，不复用模板
- 工作量是普通怪的 5–10 倍

### Q6: AI 数量上限多少？

按 §7.2 估算 50 怪 + 30 玩家无压力。

实际上限：~100 怪 + 50 玩家（80% CPU 使用）。
更多需要降级 LOD 或拆分场景。

### Q7: 怪物寻路会卡 bug 吗？

寻路 bug 是 MMO 经典问题。我们的对策：
- 良好的导航网格（NavMesh）—— 设计要求
- 寻路失败 fallback（如直线移动）
- 卡住自动 teleport（"我卡死了，传回出生点"）
- 玩家可报告卡 bug 怪物

完美解决困难，但通过工程 + 测试控制到 < 1% 概率。

### Q8: AI 行为是确定性的吗（端同）？

是——AI 行为完全服务端权威，不在客户端跑：
- 客户端只看到怪物的位置 + 动画 + 攻击事件
- 不知道 AI 内部决策
- 端同不会出现 AI 行为不一致

### Q9: AI 会不会针对玩家"个人"？

不会——AI 选择 target 基于：
- 距离最近
- HP 最低（aggro）
- threat 最高（治疗 / 伤害）

不会"记住"特定玩家，下次还专门针对。但会让"治疗 / 输出最高的玩家"被攻击概率高——这是合理设计。

### Q10: 玩家能"解谜" 出 AI 行为模式吗？

应该可以——这是设计意图：
- 玩家观察怪物 → 学习 pattern
- 利用 pattern 高效战斗
- 资深玩家比新手更厉害

但不是"完全可预测"——加入 20% 随机让经验玩家也有惊喜。

---

### 反模式清单

- ❌ AI 决策频率太高（每 tick 都跑 BT，浪费 CPU）
- ❌ 一种怪物一棵独立 BT（应模板化）
- ❌ AI 完美预测玩家（玩家挫败）
- ❌ AI 决策与动画脱节（玩家无法读出意图）
- ❌ 群组成员各自独立（破坏"群体意识"）
- ❌ AI 状态变化没动画反馈（玩家不知道发生了什么）
- ❌ 寻路失败硬编码"卡住"（应有 fallback）
- ❌ 端同破坏（用 Unity 物理 / 时钟做决策）
- ❌ 让玩家觉得"AI 作弊"（看不见的攻击 / 完美命中）
- ❌ AI 复杂度无 LOD（远处也跑全 BT）

---

## 12. 里程碑

| 阶段 | 交付 |
|---|---|
| P1 末 | BT 框架；Idle / Combat 状态机 |
| P2 中 | 完整 BT 节点库；3 种怪物原型 |
| P2 末 | 群组协调（GroupBrain）；感知系统 |
| **P3** | **5 种性格的怪物完整调试；玩家盲测体验** |
| P4 早 | AI LOD（参见 AI_LOD.md） |
| P4 中 | Boss AI 框架（参见 BOSS_AI.md） |
| P4+ | BT 编辑器；持续迭代 |

---

## 13. 文档维护

- **Owner**：战斗策划 Lead + AI 工程师 + Tech Lead
- **关联文档**：
  - `OVERVIEW.md`（§2 软目标"AI 深度"）
  - `AI_LOD.md`（性能分层）
  - `BOSS_AI.md`（特殊机制）
  - `SKILL_SYSTEM.md`（AI 释放技能）
  - `BUFF_SYSTEM.md`（AI 受 CC 影响决策）
  - `MOVEMENT_SYNC.md`（AI 移动）
  - `ANIMATION_INTEGRATION.md`（AI 动画 telegraph）
  - `09_tools/COMBAT_REPLAY.md`（AI 录像调试，待写）

---

**文档结束。**

**核心纪律重申**：
1. **AI 服务玩家体验，不展示技术**：让战斗有趣是唯一标准
2. **行为树 + 群组 + 感知**：三层模型简洁有力
3. **数据驱动**：策划用模板 + 参数，少改代码
4. **可读性 > 复杂度**：玩家能看懂 AI 在做什么
5. **不做"完美 AI"**：留破绽让玩家有成就感
6. **超越 BDO 在群组配合和 Boss 阶段**：差异化竞争点
