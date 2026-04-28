# 动画系统集成（Animation Integration）

> **用途**：定义 Atlas 中动画系统与战斗玩法的交互方式——动画作为玩家主要反馈通道、Animator 状态机的设计哲学、与时间轴/buff/网络的衔接、动画师与策划/工程的协作模式。**这是"超越 BDO 手感"目标的具体落地之一**。
>
> **读者**：动画师（必读）、战斗策划（必读）、客户端工程（必读）、技术美术（§4 必读）。
>
> **状态**：草案 v0.1 — 待团队评审。
>
> **前置文档**：`OVERVIEW.md`、`COMBAT_FEEL.md`、`SKILL_SYSTEM.md`、`UNITY_INTEGRATION.md`
>
> **关联文档**：`MOVEMENT_SYNC.md §8`（动画驱动位移）、`HIT_VALIDATION.md §3.5`（骨骼跟随）

---

## 1. 动画是玩家与战斗的主要交流通道

### 1.1 玩家从动画看到什么

动作 MMO 中，动画**不只是装饰**——它是玩家理解战斗状态的**主要信号**：

| 玩家想知道 | 通过动画看到 |
|---|---|
| 我能否取消当前动作 | 收招姿态、可取消窗口的姿态变化 |
| 敌人是否进入硬直 | 受击 reaction 动画的播放 |
| 敌人即将出大招 | 大招 telegraph（蓄力 / 怒吼） |
| 自己的连招连上了 | 连招衔接姿势 |
| 我打到没打到 | 命中接触帧的动画停顿（hit pause） |
| 我处于 iframe 还是不无敌 | 翻滚动画的姿态、屏幕色调 |
| 我被控制了 | stun / freeze 动画 + 视觉效果 |

如果动画系统做不好，**玩家无法理解游戏状态**——再好的数值也撑不起手感。

### 1.2 BDO 在动画上做对了什么

观察 BDO 的动画特点：
- **每帧都有意义**：起手有蓄力姿态，中段有挥砍重量，收招有顿挫
- **动画与位移融合**：挥剑前冲的弧线感（不是滑步）
- **动画细节传达力量**：肌肉收紧、武器拖尾、布料惯性
- **动画切换清晰**：不会"突然就到收招"，过渡帧讲故事

Atlas 必须**至少匹配**这些。

### 1.3 龙之谷在 PvP 动画上的特点

龙之谷 PvP 动画特点：
- **动作清晰可读**：远端玩家的攻击意图视觉上明确
- **取消窗口姿态变化**：玩家凭视觉判断"现在能续招吗"
- **受击反馈精准**：被击中时有清晰的 stagger 动画（不只是数字）

Atlas PvP 必须**至少匹配**这些。

---

## 2. 总体架构

### 2.1 动画系统在 Atlas 中的位置

```
战斗逻辑（服务端权威）
   ↓ 触发动画事件 (PlayAnim action)
   ↓
客户端 Animator System
   ↓ 调用 Mecanim
Unity Animator
   ↓ 驱动
角色 mesh + skeleton
   ↓ 渲染
玩家屏幕
```

**单向数据流**：战斗逻辑驱动动画，动画**不反向驱动**战斗。

### 2.2 与其他系统的关系

| 系统 | 与动画的关系 |
|---|---|
| **Skill Timeline** | 通过 `PlayAnim` action 决定何时切动画 |
| **Buff** | 状态变化（CC/iframe）通过 SetAnimParam 改 Animator 参数 |
| **Movement** | 移动状态（idle/walk/run）影响 Locomotion layer |
| **Hit Validation** | 命中事件触发 reaction 动画（attacker / victim 双方） |
| **Combat Feel** | Hit pause 调整 Animator.speed |
| **Network** | 远端实体的动画由网络事件驱动 |

---

## 3. Animator State Machine 哲学

### 3.1 核心原则

**Animator 反映状态，不决定状态**。

战斗逻辑判断"角色是否在攻击中" → 通知 Animator → Animator 播放对应动画。
反之"Animator 处于 attack state，所以角色在攻击" 是反模式（Animator 状态可能落后）。

### 3.2 分层结构（Layers）

```
Layer 0: Locomotion （基础移动）
   ├─ Idle / Walk / Run / Sprint
   ├─ Jump / Fall / Land
   └─ Crouch / Roll
   
Layer 1: Combat Action （战斗动作，覆盖 Locomotion）
   ├─ Attack states（每个技能一组）
   ├─ Block / Parry
   ├─ Hit Reactions
   └─ Death

Layer 2: Upper Body Override （上半身专用）
   ├─ Aim / Cast 维持上肢姿势
   └─ 边跑边吟唱

Layer 3: Facial / Lip Sync （表情）
   └─ 表情、说话嘴型

Layer 4: Additive （叠加层）
   ├─ Breathing
   ├─ Damage flinch（轻微抖动叠加在主动作上）
   └─ Wind effect
```

每层独立 weight 控制。Layer 1 全权重时盖住 Layer 0；Layer 2 仅上肢；Layer 4 永远叠加。

### 3.3 状态命名规范

```
attack_1_warrior_slash       <- Layer 1, 由 Skill Timeline 触发
hit_react_front_slash        <- Layer 1, 由 Damage event 触发
locomotion_idle              <- Layer 0
flinch_slight                <- Layer 4
```

命名规则：`<layer>_<category>_<descriptor>`。便于策划/工程引用。

### 3.4 状态间过渡

每个过渡有：
- **trigger condition**（什么参数变化触发）
- **transition duration**（过渡时长，影响视觉平滑）
- **interrupt source**（哪些状态可打断）
- **exit time**（是否等当前状态自然结束）

精心调过渡是动画质感的关键。仓促过渡 → 玩家觉得"假"。

---

## 4. 网络驱动动画

### 4.1 主控玩家：本地预测

主控玩家释放技能：
1. 客户端**立即**切到攻击 state（本地预测）
2. 同时发输入帧给服务端
3. 服务端确认 → 维持
4. 服务端拒绝 → 客户端切回 Idle（"打空"反馈）

主控的动画响应**没有延迟**——玩家按键即看到动作。

### 4.2 远端实体：事件驱动

远端玩家的动画由服务端广播事件触发：
- `SkillStart` 事件到达 → 客户端切 Animator state
- `SkillStateChange` 事件 → 切到下一 state（如连招）
- `Death` 事件 → 死亡动画
- `BuffApplied(stunned)` → 切到 stun 动画

**渲染时间延迟 ~100ms**（参见 `MOVEMENT_SYNC.md §6.1`），所以远端动画都是"过去 100ms 的动作"。

### 4.3 远端动画的"前摇可见"重要性

由于延迟，远端攻击的"起手帧" 必须**对玩家有阅读价值**：
- BOSS 大招：明显的蓄力 telegraph（玩家看到能反应）
- 普攻：起手 1–2 帧让玩家感知到"他在出招"
- 投射物：发射前的瞄准姿势

如果起手帧太短或太隐蔽，远端玩家"还没看见就被击中"——非常糟糕。

### 4.4 状态同步精度

```
ServerTick T:    远端玩家 X 释放技能
ServerTick T+1:  X 切到 SkillState "attack_2"

客户端按 server tick 顺序播放：
  Render time T:    X 仍在 idle（还没到事件）
  Render time T+5:  X 切到 attack_2（5 ticks 后渲染时间到）
```

**不会出现"远端动画乱序"**——按 server tick 严格排序（参见 `COMBAT_EVENT_ORDERING.md §6`）。

### 4.5 网络抖动下的动画

抖动严重时，事件可能批量到达：
- 客户端 EventBuffer 积累 5 个事件
- 渲染时间到达后**逐个播放**（不会"瞬间播完"）
- 短暂"动画播得快一点"补回来
- 玩家感知：略微卡顿但流畅

绝对禁止"看到事件就跳过中间动画"——会让玩家完全看不懂在打什么。

---

## 5. 帧数据与动画的对齐

### 5.1 时间轴 vs 动画帧

战斗 Timeline 用毫秒（`time_ms`）作为时间单位，Animator 用归一化时间。两者必须对齐。

### 5.2 标准动画帧率

约定：**所有战斗动画 30 FPS 制作**（即使最终 60 FPS 渲染）。

理由：
- 30 FPS 是 ARPG 行业标准（BDO / DN / Diablo IV）
- 30 FPS 帧数好计算（1 帧 = 33.33 ms）
- 60 FPS 制作多一倍工作量但视觉差异不显著

### 5.3 帧 → 毫秒映射

```
frame_30fps  →  time_ms = frame * 33.33  →  round to int

例：
  frame 5  →  167 ms
  frame 10 →  333 ms
  frame 15 →  500 ms
```

策划在 Excel 配 `time_ms`，Frame Data Editor 同时显示帧数（参见 `09_tools/FRAME_DATA_EDITOR.md §3.4`）。

### 5.4 动画师工作流

```
动画师在 Maya / Blender 制作 30 FPS 动画
  ↓
导入 Unity，AnimationClip 设置 30 FPS
  ↓
在 Frame Data Editor 中查看：
  ├─ 时间轴显示帧数
  ├─ 动画师标记关键帧（hit contact / cancel start / recovery start）
  ├─ 标记自动转 time_ms
  └─ 写入 SkillTimelines.xlsx
  ↓
策划在编辑器内调整数值，看到动画 + 时间轴同步
```

### 5.5 关键帧约定

每个攻击动画必须明确标注：
- `frame_anticipation_end`：起手结束（动作进入 active 段）
- `frame_active_start`：hitbox 应该 spawn 的帧
- `frame_active_end`：hitbox 结束
- `frame_cancel_window_start`：可取消的最早帧
- `frame_recovery_end`：动画结束

由动画师在 Animation Event 中标注，自动同步到 Timeline。

### 5.6 不同等级动画的帧数

参见 `COMBAT_FEEL.md §7.2`：

| 类型 | 推荐帧数 | 毫秒 |
|---|---|---|
| 普攻 | 9-15 帧 | 300-500 ms |
| 重击 | 18-30 帧 | 600-1000 ms |
| Ultimate | 30-60 帧 | 1000-2000 ms |

---

## 6. Root Motion 与位移

### 6.1 不在运行时用 Unity Root Motion

理由：
- 端同破坏（Animator 内部状态不一致）
- 网络同步困难
- 调试不可控

### 6.2 构建期提取曲线

参见 `MOVEMENT_SYNC.md §8` + `09_tools/FRAME_DATA_EDITOR.md §7.2`。

```
Maya 制作动画包含位移（root bone movement）
  ↓
导入 Unity，AnimationClip 启用 root motion
  ↓
工具按钮 "Extract Root Motion"
  ↓
生成 MovementCurve 数据
  ↓
Skill 配置 Dash action 引用 curve_id
  ↓
运行时：
  服务端 + 客户端 都用同一 curve
  Unity Animator 关闭 root motion（防止双重位移）
  Animator 仅播动画 visual
  位置由 MovementCommand 系统驱动
```

### 6.3 玩家感知

玩家**完全感觉不到**这是"曲线驱动而非真 root motion"——视觉上完全一样。

但好处巨大：
- 端同保证（服务端 / 客户端位置完全一致）
- 网络同步免费（不需要传位置流）
- 可调试（曲线可视化）

---

## 7. 受击反馈动画系统

### 7.1 反馈动画的重要性

参见 `COMBAT_FEEL.md §6`。受击动画**不能一刀切**——所有攻击都播同一个 stagger 是反模式。

### 7.2 反应动画矩阵

```
Damage Type × Direction × Victim State → Reaction ID
```

例（部分）：

| Damage Type | Direction | Victim State | Reaction ID | Anim |
|---|---|---|---|---|
| Slash | Front | Standing | 1001 | hit_slash_f |
| Slash | Back | Standing | 1002 | hit_slash_b |
| Slash | Front | Jumping | 1003 | hit_air_b |
| Blunt | Front | Standing | 1010 | hit_blunt |
| Magic | Any | Blocking | 1020 | hit_block_magic |
| ... | ... | ... | ... | ... |

服务端选 reaction_id 在 Damage 事件中带回客户端，客户端切对应 anim。

### 7.3 数量预算

P3 阶段最少做：
- 4 种 damage_type × 2 directions × 2 states = **16 种** 基础反应
- + 死亡 / 倒地 / 起身 = **20 种**

P4+ 扩展：
- 加 jumping / air / kneeling / blocking 状态
- 加更多武器类型反应
- 加 boss 专用反应（boss 受击有特殊表现）

最终目标：**每职业 30+ 受击动画**，覆盖 95% 战斗场景。

### 7.4 浮空连击（Juggle）

浮空状态下的反应特殊：
- 普通攻击让浮空者保持空中
- 重击让浮空者下坠
- 浮空过程播 `hit_air_loop` 循环（缓慢下坠）
- 落地时切 `hit_ground_recover`（起身）或 `dead_ground`（死亡）

浮空机制是动作 ARPG 的爽点之一，必须做对。参见 `BUFF_SYSTEM.md`（Airborne flag）+ `MOVEMENT_SYNC.md`（Launch action）。

### 7.5 受击动画的取消

被击中时：
- **普通受击**：硬直，玩家输入被吞
- **iframe 翻滚**：跳过受击动画
- **超级霸体**：受击动画**不播**（动作继续），仅小 flinch 叠加层
- **格挡成功**：切 `hit_blocked` 而非 `hit_xxx`

这些差异让玩家通过动画清楚知道"我刚才挡了 / 闪了 / 硬抗了"——比看屏幕中央的文字提示更直观。

---

## 8. Idle 与个性化

### 8.1 Idle 不是单调

固定一个 idle 动画的角色让人觉得"假"。建议：
- **Breathing layer**：永远叠加微动作（Layer 4 Additive）
- **Idle variations**：每隔 5–10 秒切换次要 idle（伸懒腰、看四周等）
- **武器闲置**：偶尔检查武器、搭弓、拉箭

让角色**有生命**，不是雕像。

### 8.2 Locomotion 自然度

走 / 跑切换：
- Speed param 平滑插值
- 过渡用 Blend Tree（不要硬切）
- 改变方向时身体先转

跳跃 / 落地：
- 起跳前微下蹲（anticipation）
- 落地缓冲（recovery）
- 落到不同高度有不同 reaction

### 8.3 角色个性化

每职业有独特的 idle / locomotion 风格：
- 战士：稳重、武器拖地
- 法师：飘逸、轻盈
- 弓箭手：警觉、低姿态

这些细节让角色**鲜活**——玩家看到剪影就能认出职业。

---

## 9. IK（反向动力学）与外观细节

### 9.1 我们用 IK 的场景

| IK 用途 | 影响玩家体验 |
|---|---|
| **脚部贴地** | 不会"飘在地面"或"穿地" |
| **看向目标** | 远距离敌人在视野时角色头自然转向 |
| **手部抓武器** | 武器换装后手部贴合武器 grip |
| **持物** | 拿物品 / 道具时手贴合 |

### 9.2 IK 与战斗的关系

战斗动画**主要靠 keyframe**，IK 仅辅助：
- 攻击命中位置由 hitbox 决定，不由 IK 决定
- 反应动画 keyframe 制作，不靠 IK 自动生成
- IK 仅在 idle / locomotion / idle blend 等场景生效

### 9.3 性能与 IK

IK 计算有 CPU 成本：
- 每帧 4–8 IK constraints / 角色
- 远距离实体（L3+）禁用 IK 节省性能（参见 `DENSITY_ADAPTIVE_NETWORKING.md §7.3`）

---

## 10. 死亡与 Ragdoll

### 10.1 死亡动画

每职业至少 2 种死亡动画：
- 标准死亡（向后倒）
- 击飞死亡（被重击飞起）

特殊死亡（boss / 剧情）单独制作。

### 10.2 Ragdoll

死亡动画播完后切 ragdoll（物理布偶）：
- 尸体自然倒地（避免穿透地面）
- 受重力影响倒下姿势
- 尸体保留 ~10–20 秒后淡出

Ragdoll 用 Unity Physics（豁免端同）。**不影响 gameplay**——死后的 ragdoll 行为是纯视觉，不影响任何逻辑。

### 10.3 复活动画

复活技能或自动重生：
- 死亡 → 倒地 → 渐隐
- 复活点 → 渐显 + 复活动画
- 视觉过渡 1–2 秒

不要"瞬间出现"——破坏沉浸感。

---

## 11. 表情与对话动画

### 11.1 PvE 剧情场景

剧情对话需要：
- 嘴型同步（lip sync）
- 表情变化（喜怒哀乐）
- 手势

这些用 Layer 3 Facial。

### 11.2 PvP 嘲讽

玩家可触发预设嘲讽动作（按键 emote）：
- 招手、鼓掌、嘲讽、跳舞
- 取代当前 idle，循环播放
- 受到攻击立即打断

社交玩法之一，对体验有积极影响。

---

## 12. 动画师与策划/工程的协作流程

### 12.1 新技能从零到完成

```
策划提出技能需求（设计文档：动作 / 数值 / 反馈）
  ↓
动画师参与 review，提出动作可行性建议
  ↓
动画师做 anticipation / action / recovery 三段毛坯
  ↓
策划在 Frame Data Editor 中预览，调整时间轴
  ↓
动画师精修关键帧（调表情、拖尾、武器轨迹）
  ↓
特效美术加 VFX
  ↓
音效设计加 audio
  ↓
策划平衡数值，跑测试
  ↓
P3 盲测验证手感
```

不是动画师"独立"产出——必须**和策划/工程紧密协作**。

### 12.2 动画师的工具支持

- **Frame Data Editor**：可视化时间轴 + 实时预览
- **Animation Notify**：在 AnimationClip 上标注关键帧（自动转 time_ms）
- **Pose Library**：常用姿态库
- **Reference Video**：BDO / DN / 其他游戏关键动画的视频参考

### 12.3 沟通约定

每周一次动画 / 战斗 review：
- 看本周新做的动画
- 玩家视角播放，互相提建议
- 确定下周优先级

避免"动画师做完 → 策划试用 → 重新做"的浪费循环——前期 align 期望。

---

## 13. 动画质量基线

### 13.1 P3 验证

参见 `MILESTONE_COMBAT_FEEL.md §3.3`：动画质感占评分 20% 权重。

P3 通过标准（盲测）：
- 动画细节相比 BDO 不输
- 过渡平滑（玩家不感知 hitch）
- 反应动画清晰（玩家能从动画读出战斗状态）
- Locomotion 自然（不滑步）

### 13.2 持续监控

P4+ 上线后定期 review：
- 玩家反馈"动画很假"的频率
- 新职业动画与已有职业风格一致
- 性能（IK / Animator 复杂度）

每月一次动画质量复盘会。

---

## 14. FAQ 与反模式

### Q1: 自己做动画 vs 动捕（mocap）？

**核心动作走 mocap**（攻击 / 移动），**特殊动作 keyframe**（如夸张大招）。

mocap 优势：动作自然，符合人体力学。劣势：成本高、需要后期清理。

mocap 完成后**动画师手工调整**——加重量感 / 加表情 / 调时机。

### Q2: 风格化 vs 写实？

参考对标——BDO / DN 都是**写实偏酷炫**风格（不是日式动漫，不是写实模拟）。

Atlas 走类似路线：写实人体比例 + 风格化动作（动作幅度 1.2–1.5 倍真人）。

### Q3: 动画 mesh 需要多少多边形？

主角色（玩家可见的近距离）：30k–50k tri。
怪物：依据复杂度 10k–30k。
远景实体：LOD 自动切到低模（< 5k）。

### Q4: 同时多少个角色播完整动画无卡顿？

性能目标：**80 个角色**同屏（含玩家 + NPC）全 Animator + IK，60 FPS。

超过则进入 LOD（远端简化）。

### Q5: 动画师改一个动画后，端同会破吗？

只要动画修改不改变`time_ms`（关键帧时机），不影响端同——动画是纯客户端视觉。

时机改了（如把 `frame_active_start` 从 5 改 7），策划必须更新 Excel `time_ms`，DataBuild 重生成。

### Q6: 一个角色多少 Animator state 合理？

经验：
- Locomotion 层：~10 state
- Combat 层：每个技能 ~2–3 state（warmup / active / recovery），60 技能 = 150 state
- 全 Layer 加一起：~200 state / 角色

超过 500 state 难维护。如果太多，考虑用 Sub-State Machine 组织。

### Q7: 动画性能优化点？

按重要性：
1. **LOD**：远距离禁用 Animator（最大节省）
2. **IK 限制**：只在视野中实体启用
3. **Animator 优化**：减少 state 复杂度、降低 bone 数
4. **Culling**：屏幕外角色不更新 Animator

### Q8: PvP 中如何让玩家"读懂"对手动画？

每职业**核心 5 个技能**有强 telegraph：
- 蓄力姿势明显
- 武器位置预示攻击方向
- 持续 ≥ 200ms（玩家有反应窗口）

非核心技能可以更隐蔽（高手才能预判）。

### Q9: 玩家自定义角色（捏脸）会影响战斗动画吗？

不会——捏脸只动 mesh，不动 skeleton。所有动画基于标准骨骼，捏脸后动画依然适配。

但需注意：极端体型（巨胖 / 极瘦）可能让动画看起来怪异。系统给出"风格化"约束（参见角色创建限制）。

### Q10: 动画工作量预估？

每职业（25 职业）：
- 60 技能 × 1 动画/技能 = 60 动画
- 30 受击反应
- 20 idle / locomotion
- 5 死亡 / 复活 / 重生
- = ~115 动画 / 职业

× 25 职业 = ~2900 动画

每动画平均 1 周制作（含 mocap + 清理 + 调整），共 3000 周——团队规模 8 动画师并行 = 8 年。

实际**复用 + 模块化**降低 50–70% 工作量。每职业 75% 复用通用动画（idle / locomotion 等），仅 25% 独占战斗动画。预估 ~2 年（4 动画师）。

---

### 反模式清单

- ❌ 所有受击都用同一个 stagger 动画
- ❌ Animator 状态决定战斗逻辑（应反向）
- ❌ 运行时用 Unity Root Motion（端同破坏）
- ❌ Idle 动画完全静止（角色像雕像）
- ❌ 远端动画起手帧太短（玩家来不及反应）
- ❌ 战斗动画忽视 anticipation / recovery（动作干瘪）
- ❌ 角色个性混同（剪影分不出职业）
- ❌ 只动 keyframe 不调过渡（切换硬切）
- ❌ 动画 LOD 不做（性能拉爆）
- ❌ 不参考标杆（孤立闭门造车）

---

## 15. 里程碑

| 阶段 | 交付 |
|---|---|
| P1 末 | Animator 系统集成；基础 locomotion + idle |
| P2 中 | Skill timeline 触发动画；远端事件驱动 |
| P2 末 | 1 职业完整 20 技能动画；基础受击反应 |
| **P3** | **完整反应矩阵；动画质感 ≥ 4.0/5.0 盲测** |
| P3 末 | Hit pause / camera / VFX / animation 联动打磨完成 |
| P4+ | 多职业动画扩展；持续打磨 |

---

## 16. 文档维护

- **Owner**：动画 Lead + Tech Lead + Combat Designer Lead 三方共担
- **关联文档**：
  - `OVERVIEW.md`（§2 软目标"动画质感"）
  - `MILESTONE_COMBAT_FEEL.md`（动画质感评分维度）
  - `COMBAT_FEEL.md`（hit pause / hit reaction 配合）
  - `SKILL_SYSTEM.md`（PlayAnim action）
  - `BUFF_SYSTEM.md`（状态对动画的影响）
  - `MOVEMENT_SYNC.md §8`（动画驱动位移）
  - `HIT_VALIDATION.md §3.5`（骨骼跟随）
  - `09_tools/FRAME_DATA_EDITOR.md §7`（动画师工作流）

---

**文档结束。**

**核心纪律重申**：
1. **动画反映状态，不决定状态**：单向数据流
2. **每个技能必须有 anticipation / action / recovery 三段**：动作不能干瘪
3. **远端动画起手帧 ≥ 200ms 重要**：玩家需要反应窗口
4. **受击反应必须细分**：通用 stagger 是反模式
5. **动画师与策划/工程紧密协作**：避免"做完才发现不对"
6. **超越 BDO 在细节里**：每帧都有意义
