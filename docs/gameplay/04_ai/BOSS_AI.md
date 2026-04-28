# Boss AI（Boss Encounter Design）

> **用途**：定义 Atlas 中 Boss 战斗的设计哲学、阶段化机制、招式 telegraph、群组配合（Boss + adds）、与普通怪 AI 的差异。**Boss 战是玩家记住一款 MMO 的原因之一**——这份文档决定 Atlas 能否做出"难忘的战斗体验"。
>
> **读者**：Boss 战斗设计（必读）、动画师（§6 必读）、特效美术（§7 必读）、工程（§4、§9 必读）。
>
> **状态**：草案 v0.1 — 待团队评审。
>
> **前置文档**：`OVERVIEW.md`、`AI_ARCHITECTURE.md`、`SKILL_SYSTEM.md`（custom_handler）、`COMBAT_FEEL.md`
>
> **关联文档**：`AI_LOD.md`（boss 不走标准 LOD）、`SKILL_SYSTEM.md §8`（custom handler 用于 boss）

---

## 1. 设计哲学

### 1.1 Boss 战是 MMO 的"故事高潮"

普通战斗是"日常"，Boss 战是**戏剧**：
- 玩家有期待（前置任务铺垫）
- 战斗有节奏（多阶段）
- 战斗有学习曲线（每次失败都学到东西）
- 战胜后有成就感（不仅是数值奖励）

设计精彩的 Boss 战，玩家会**多年后还记得**——这是 MMO 长生命力的来源。

### 1.2 BDO / DN / WoW 的标杆

参考标杆：
- **WoW 的 Lich King**：多阶段，机制清晰，团队配合
- **BDO 的 Karanda**：飞行 + 地面阶段切换，玩家从地面到空中应对
- **Dragon Nest 的 Manticore**：阶段化技能，每阶段不同 telegraph
- **Final Fantasy XIV** 的 Savage 难度：极致机制配合

Atlas 至少要**做到这个水准**，并争取在某些方面超越。

### 1.3 设计目标

1. **机制清晰**：玩家能看懂 boss 在做什么
2. **可学习**：第一次失败后玩家知道下次怎么改进
3. **多样化**：不同 boss 给不同感觉
4. **戏剧性**：阶段切换有视觉 / 听觉冲击
5. **可重复挑战**：玩家会想"再打一次"

### 1.4 非目标

- **不做 RNG-heavy boss**（运气主导胜负让玩家挫败）
- **不做"练级靶子"** 类 boss（数值堆叠无机制）
- **不复用普通怪 BT 模板**（boss 必须独特）
- **不做秒杀机制**（玩家无反应窗口）

---

## 2. Boss 战的核心要素

### 2.1 必备要素清单

每个 Boss 战必须有：

✅ **多阶段**：至少 2 个阶段，最好 3+
✅ **明显招式 telegraph**：每个攻击有视觉 / 听觉预警
✅ **机制要求**：玩家必须做特定操作（不只是输出）
✅ **难忘瞬间**：至少一个让玩家"哇" 的时刻
✅ **学习曲线**：第 1 次到第 5 次有明显进步空间
✅ **奖励反馈**：击杀有清晰的庆祝（声音、特效、相机）

### 2.2 可选要素

- 召唤物（adds）需要管理
- 阶段切换的过场动画
- 环境互动（boss 砸毁地图 → 出现新机关）
- 团队协调要求（多人副本）
- 时间限制（berserk timer）

### 2.3 反模式

- ❌ 单阶段血厚 boss（无聊）
- ❌ 无 telegraph 的攻击（玩家挫败）
- ❌ 100% 命中的范围攻击（玩家无操作空间）
- ❌ 每次完全一样的行为（没新意）
- ❌ 数值过高 / 过低（无挑战 / 太难）

---

## 3. 阶段化（Phase-Based）设计

### 3.1 标准阶段结构

```
Phase 1: 开场，介绍 boss 主要机制
   ↓ HP 70% 触发
Phase 2: 加入新机制 / 升级现有机制
   ↓ HP 40% 触发
Phase 3: 决战，复杂机制 / 召唤帮手
   ↓ HP 10% 触发
Phase 4 (Optional): 狂暴 / 最终阶段
```

或基于时间 / 事件触发，不一定按 HP。

### 3.2 阶段切换的视觉处理

阶段切换是**戏剧时刻**：
- Boss 受重击，进入"无敌"几秒
- 播放 cinematic：boss 怒吼 / 变形 / 召唤
- 配合相机切镜头（参见 `COMBAT_FEEL.md §4.5` Camera Focus）
- 配合震屏 + 强音效
- 玩家屏幕显示阶段提示（"Phase 2: The Awakening"）

让玩家**感觉**到"现在是关键时刻"——而不仅是数字变化。

### 3.3 每阶段的内容

每阶段应**独立有趣**：
- Phase 1 教玩家基本机制（如躲避锥形 AOE）
- Phase 2 升级（同时多个 AOE，需要走位）
- Phase 3 加新机制（召唤 adds 必须先杀）
- Phase 4 狂暴（无机制但伤害巨大，DPS 检查）

不要"每阶段都加东西到所有都同时存在"——玩家会被压垮。

### 3.4 阶段间过渡时间

阶段切换时**给玩家喘息**：
- 1–3 秒过渡时间
- Boss 此时无伤害（吟唱 / 变形）
- 玩家恢复 HP / MP / cooldown
- 重新调整位置 / 队形

让节奏**有起伏**——一直高强度战斗会让玩家疲劳。

---

## 4. Boss AI 实现：custom_handler

### 4.1 为什么用 custom_handler

参见 `SKILL_SYSTEM.md §8`。Boss 的复杂度通常**超出标准 BT** 表达力：
- 复杂阶段管理
- 自定义机制（特殊视觉 / 玩法）
- 剧情对话穿插
- 多变量决策

用脚本（C#）实现，灵活度最高。

### 4.2 BT + custom_handler 混合

实践中：
- **基础 BT** 处理基本行为（追击、攻击、撤退）
- **custom_handler** 控制阶段切换、特殊机制
- 两者**互相调用**

例：Lich King-like Boss
```csharp
public class LichKingBossHandler : ISkillHandler {
  enum Phase { P1_Initial, P2_Adds, P3_Final, P4_Berserk }
  Phase _phase = Phase.P1_Initial;
  
  public void OnTick(BossInstance boss, CombatContext ctx) {
    // 基础 BT 在外部跑（追击、普攻）
    
    // 阶段管理
    if (boss.HpPercent <= 0.7f && _phase == Phase.P1_Initial) {
      EnterPhase2(boss, ctx);
    }
    if (boss.HpPercent <= 0.4f && _phase == Phase.P2_Adds) {
      EnterPhase3(boss, ctx);
    }
    
    // 阶段特殊技能
    switch (_phase) {
      case Phase.P2_Adds: HandleP2(boss, ctx); break;
      case Phase.P3_Final: HandleP3(boss, ctx); break;
      // ...
    }
  }
  
  void EnterPhase2(BossInstance boss, CombatContext ctx) {
    boss.SetInvulnerable(true);
    boss.PlayCinematic("p2_intro");
    boss.SpawnSubSkill("phase_transition_aoe", ...);
    ctx.ScheduleTimer(3000, () => {
      boss.SetInvulnerable(false);
      _phase = Phase.P2_Adds;
      // 召唤 adds
      boss.SpawnEntities("undead_adds", count: 4);
    });
  }
}
```

### 4.3 数据驱动 + 脚本

策划用 Excel 配置：
- 每阶段使用的 skill_id
- HP 阈值
- adds 类型
- 特殊参数

custom_handler 读这些参数，决定具体行为。让策划**自助调试**而不需要工程改代码。

### 4.4 测试与调试

Boss 战测试用：
- **Editor 内单玩家测试模式**：boss 缩短 HP，快速跑完所有阶段
- **录像回放**：详细查看每阶段切换
- **遥测**：上线后看玩家通关率，太低 / 太高都需调整

参见 `09_tools/COMBAT_REPLAY.md`（待写）。

---

## 5. 招式 Telegraph 设计

### 5.1 为什么 Telegraph 关键

玩家无法读心——必须从**视觉信号**判断 boss 要做什么。
Telegraph 是 boss 战可玩性的**核心**。

### 5.2 Telegraph 类型

按强度 / 时长分类：

| Telegraph | 时长 | 视觉 | 用途 |
|---|---|---|---|
| **Subtle** | 200–400 ms | 微小蓄力姿势 | 普攻 |
| **Medium** | 500–800 ms | 明显蓄力 + 特效 | 中等技能 |
| **Strong** | 1000–2000 ms | 大幅蓄力 + 屏幕提示 + 音效 | 高伤大招 |
| **Cinematic** | 3000+ ms | 全屏特效 + 镜头切换 | 阶段切换 / 必杀 |

### 5.3 视觉信号清单

✅ 蓄力姿势（动画）
✅ 武器位置（剑举高 = 即将下劈）
✅ 身体朝向（决定攻击方向）
✅ 特效（火球凝聚 / 法阵展开）
✅ 地面标记（即将的 AoE 范围）
✅ 屏幕周边脉冲（强招警告）
✅ 音效（特定吟唱）

### 5.4 Telegraph 时长设计

设计原则：玩家**有反应窗口**：
- 短玩家反应时间 ~200 ms
- 普通玩家反应时间 ~400 ms
- Boss 招式 telegraph ≥ 500 ms 才不挫败玩家
- 致命招（一击秒杀）telegraph ≥ 1500 ms

### 5.5 PvE / PvP 不同

PvP 中玩家技能 telegraph 较短（玩家间博弈，长 telegraph 太"假"）。
PvE Boss telegraph 较长（教学 + 容错）。

### 5.6 不同难度档的 Telegraph

| 难度 | Telegraph |
|---|---|
| Normal | 长（玩家轻松学） |
| Hard | 短（紧张感） |
| Nightmare | 极短 + 多招同时 |

通过同一 boss 不同难度版本提供学习曲线。

---

## 6. 标志性招式（Signature Mechanics）

### 6.1 让 boss 难忘

每个 boss 应该有 1–2 个**标志性招式**——玩家会**用它的招式称呼 boss**：
- "啊那个 spinning Karanda"
- "the Lich King who summons valkyr"
- "Thunder Knight 的雷暴脚步"

这要求**机制独特**且**视觉冲击**。

### 6.2 例：剑刃风暴 boss

```
Boss: 双斧剑士
标志性机制 "剑刃风暴":
  - 长 telegraph (2 秒)
  - Boss 旋转双斧形成风暴
  - 全屏 AOE 但有"安全区"
  - 玩家必须找到安全区站位
  - 持续 8 秒，期间 boss 移动慢
  - 结束后 boss 短暂虚弱（玩家输出窗口）
```

这个机制给玩家：
- **学习挑战**：找到安全区需要观察
- **应对方法**：移动 + 站位
- **奖励**：boss 虚弱期可以爆 dps
- **难忘**：旋转双斧风暴的视觉
- **可读名字**：玩家会叫它"剑刃风暴 boss"

### 6.3 设计标志性的方法

提问：
- 这个机制 5 年后玩家还记得吗？
- 这个 boss 与其他怎么区分？
- 玩家会用什么动作 / 名字描述？
- 视觉是否够强？
- 机制是否独特？

如果**任一回答模糊**，机制还不够。

### 6.4 不要每个 boss 都"独特"

Atlas 总共可能 50–100 boss（按副本数）。**每个都做"标志性"不现实**：
- 标志性 boss：~15 个（关键剧情 / 大型副本最终 boss）
- 中等 boss：~50 个（每个有一个亮点机制）
- 普通 boss：~30 个（小副本 boss，机制简单但完整）

资源集中在标志性 boss 上。

---

## 7. 群组配合（Boss + Adds）

### 7.1 召唤物（Adds）的设计

Boss 召唤助手让战斗更复杂：
- **Tank Adds**：玩家必须先杀
- **Healer Adds**：boss 不死则 add 持续治疗
- **Bomb Adds**：自爆造成 AoE，玩家必须风筝
- **Summoner Adds**：召唤更多 add，处理优先

每种 add 类型给玩家**新挑战**。

### 7.2 Adds 的 LOD 例外

参见 `AI_LOD.md §2.5`。Boss 的 adds **永远 L0**——玩家正在战斗这群单位，必须保证 AI 完整。

### 7.3 协调感

Boss 和 adds **不只是各自打**，要有协调：
- Boss 召唤 → adds 出现位置围绕 boss
- Boss 受击 → adds 优先攻击伤害最高玩家
- adds 死光 → boss 变愤怒

让玩家感受到"我在打一个**军团** 而不是几个独立单位"。

### 7.4 实现：GroupBrain + custom_handler

详见 `AI_ARCHITECTURE.md §6`。Boss + adds 是一个特殊 group，由 boss 的 custom_handler 控制：
- Boss handler 决定 adds 出现 / 消失
- adds 自己有简化 BT
- GroupBrain 协调 add 的目标选择

---

## 8. Boss 战的视觉 / 听觉

### 8.1 Boss 出场

Boss 出场是**仪式**：
- Boss 房间入场 → 镜头先聚焦 boss（cinematic）
- Boss 起立 / 觉醒动画
- 配合声音：吼声 / 低音 / 战鼓
- 玩家屏幕显示 boss 名字 + 描述

让玩家**感觉这不是普通怪**。

### 8.2 战斗中

boss 战斗的视觉规模高：
- 全屏 VFX 频繁（不滥用）
- 镜头适度参与（重击 hit pause + shake）
- 音乐进入"boss 战" 主题
- HP bar 占据屏幕底部（醒目）

### 8.3 阶段切换

参见 §3.2。是 cinematic 时刻，**舍得花资源**：
- 全屏特效
- 镜头切换
- 强音效
- 屏幕提示

### 8.4 击杀

Boss 死亡是**奖励时刻**：
- Slow motion（参见 `COMBAT_FEEL.md §4.4`）
- 镜头围绕 boss 旋转
- 死亡动画夸张（爆炸 / 坍塌）
- 战利品掉落特效
- 音乐切到胜利主题

让玩家**深刻体验**胜利的喜悦。

### 8.5 失败

玩家死亡 / 团灭：
- 不要嘲讽（boss 不应该"庆祝"杀死玩家——挫败感）
- 显示死亡总结：哪招杀的、哪阶段、玩家 dps 等
- 提供"分析建议"（"试试下次提前躲剑刃风暴"）
- 让玩家**想再来一次**

---

## 9. 性能与端同

### 9.1 Boss 战的特殊负载

Boss 战可能包含：
- 多个 adds（5–20 个）
- 多个同时 hitbox
- 多个 timeline 事件
- 复杂 custom_handler 逻辑

性能压力高于普通战斗。但 boss 战是**少数（玩家 1–8 人）**——总开销不大：
- 8 玩家 × 完整 AI + 战斗 = 约 5 ms / tick
- 占副本 33 ms tick 15%——可接受

### 9.2 端同要求

Boss AI 完全服务端权威：
- custom_handler 在服务端跑
- 客户端只看到结果（动画 / 事件 / 状态变化）
- 端同保证（参见 `DETERMINISM_CONTRACT.md`）

不在客户端预测 boss 的下一招。

### 9.3 副本独占资源

Boss 战通常在副本：
- 独立 CellApp（30 Hz）
- 不影响开放世界
- 可以更"舍得"花资源

---

## 10. 难度调优

### 10.1 难度的来源

Boss 难度来自：
- 机制复杂度
- 数值（HP / 伤害）
- Telegraph 时长（短 = 难）
- 同时机制数量
- 团队配合要求

调难度从机制 / telegraph **优先于**调数值。

### 10.2 多难度版本

每个 boss 可能有：
- Normal：基础体验
- Hard：进阶玩家
- Nightmare：终极挑战
- Mythic（未来）：极限玩家

不同难度**核心机制相同，参数不同**——玩家学一遍受用全部。

### 10.3 难度调优反馈

上线后监控：
- 各难度通关率（Normal 期望 70%，Nightmare 期望 5%）
- 玩家 wipe 频率
- 平均尝试次数
- 玩家 quit 比例（太难就放弃）

数据指导调整。

### 10.4 难度调优 hot fix

发现 boss 数据失衡（上线后玩家反馈卡 boss）：
- 紧急 hotfix（参见 `10_liveops/HOTFIX_PLAYBOOK.md`，待写）
- 通常调数值（伤害 / HP）
- 重大问题（机制 bug）需要版本升级

---

## 11. 标志性 Boss 设计案例

### 11.1 案例 1：龙王（剧情 Boss）

```
Phase 1: 飞行打击
  - 龙在空中飞行
  - 玩家用远程攻击 / 攀爬抓钩
  - 龙释放火球 AOE（Strong telegraph）
  - 标志：玩家必须读地面火球落点

Phase 2: 地面缠斗
  - 龙降落，进入近战
  - 龙咬 / 尾巴扫 / 翅膀扇
  - 玩家躲招式 + 输出
  - 标志：龙的尾巴是 1 周角的扫动

Phase 3: 怒吼狂暴
  - 龙血量 < 20%
  - HP 不可减（无敌 5 秒）+ 怒吼
  - 然后无敌结束 + 双倍伤害
  - 必杀招"地狱火"全屏 AOE 必须躲
  - 标志：致命的"龙息" 招式

击杀:
  - 龙坍塌坠落
  - 镜头特写
  - 玩家获得"屠龙者" 称号
```

### 11.2 案例 2：练级点 Boss（小型 Boss）

```
"森林之主"（练级点最终 boss）

Phase 1: 普通战斗
  - 树根攻击（近战）
  - 召唤树精（adds）
  - 玩家管理 adds + 输出 boss

Phase 2: HP < 50%
  - boss 入土消失 5 秒（不可见）
  - 地面出现小标记追玩家
  - 5 秒后地下伸出根刺攻击玩家位置
  - 玩家需要持续移动
  - 标志：地下伏击机制

击杀: 简单（无 cinematic，普通副本不舍得花资源）
```

### 11.3 案例 3：竞技场 Boss（PvP 不适用，仅 PvE）

略——PvP 不打 Boss。

---

## 12. Boss 战的开发工作流

### 12.1 设计阶段

1. **需求**：策划提出 boss 设计稿（背景 / 招式 / 阶段 / 难度）
2. **review**：动画 / 美术 / 工程 review，评估可行性
3. **核心机制原型**：先做 1 个标志性招式，验证体验
4. **完整阶段**：逐阶段实现
5. **调优**：数值 + telegraph + 节奏

### 12.2 工程任务

- 实现 custom_handler（C# 脚本）
- 配置 BT 基础部分
- 适配 cinematic 系统
- 集成 telegraph 系统

### 12.3 美术任务

- Boss 模型 + 动画（30+ 动画通常）
- Add 模型 + 动画
- VFX（每招式独立）
- Boss 房间环境

### 12.4 音效任务

- Boss 主题音乐
- 招式音效（每招独立）
- 阶段切换音
- 击杀音效

### 12.5 工作量估算

每个标志性 boss：
- 设计 + 编写：1–2 周
- 美术：4–6 周（动画 + VFX + 模型）
- 音效：1–2 周
- 工程实现：2–3 周（custom_handler + 测试）
- 调优：2–4 周
- **总计：8–12 周** / 一名标志性 boss

15 个标志性 boss × 10 周 = 150 周 / 团队并行 3 boss = 约 1 年。

---

## 13. FAQ 与玩家关注问题

### Q1: Atlas 的 Boss 比 BDO 更难吗？

不**更难**，但**更精彩**。难度针对玩家分布调（Normal 70% 通关）。
精彩度通过机制设计 + 视觉冲击实现。

### Q2: 可以单人挑战 Boss 吗？

视 boss 类型：
- 普通副本 Boss：可（4 人组队推荐）
- 大型副本 Boss：必须组队
- World Boss：开放世界，多玩家共战
- 单人挑战 Boss：未来可能（如龙之谷的"挑战"模式）

### Q3: Boss 死亡后再次复活时间？

副本 Boss：副本重置（每日 / 每周）。
World Boss：固定刷新（每 6 小时）。
剧情 Boss：仅一次（通过即不再）。

### Q4: PvP 中遇到 Boss 吗？

PvP arena 不混 Boss。PvP 是纯玩家对战。

仅 PvE 才有 Boss。

### Q5: Boss 战时其他玩家可以加入吗？

副本：组队加入（开始前）。
World Boss：开放加入（路过的人可以一起打）。
但 World Boss 通常不允许"半路加入"——已经开始了不能加（保持公平）。

### Q6: Boss 会"作弊"吗（看到我隐身 / 穿墙）？

不会——Boss 与普通怪一样按规则感知。
某些 boss 有"侦测隐身"技能（明确告知玩家），是设计意图。

### Q7: 如果 Boss 被卡 bug 怎么办？

紧急 hotfix（数小时内）。
重大 bug 临时关闭副本，避免玩家被坑（白白浪费时间）。

### Q8: Boss 战时会掉线怎么办？

副本机制：
- 玩家断线 → 等待 30 秒
- 如果重连 → 继续战斗
- 如果不返回 → 副本结束（其他人可继续）

让玩家不因为短暂断线**完全失去进度**。

### Q9: 玩家可以"看攻略" 学 Boss 吗？

可以——攻略不影响游戏体验。
Atlas 不阻止外部攻略，但**游戏内**也提供：
- Boss 机制提示（重要招式 telegraph 时屏幕提示）
- 击败后看回放（学习自己 / 他人的处理）

### Q10: Boss 战与 PvP 排位都能让我们成为高手玩家吗？

对——Atlas 的"高级玩家"应该擅长两方面：
- PvE Boss 战（机制 / 配合）
- PvP arena（操作 / 反应）

两者**核心技能（手感 / 游戏理解）相通**，但要求不同。

---

### 反模式清单

- ❌ Boss 没 telegraph 直接秒杀（玩家挫败）
- ❌ 一阶段血厚 boss（无聊）
- ❌ Boss 与普通怪一样的 BT（缺乏特殊感）
- ❌ 阶段切换无视觉提示（玩家不知道发生了什么）
- ❌ 标志性招式与其他招式视觉相同（玩家无法识别）
- ❌ 击杀无奖励反馈（成就感缺失）
- ❌ Boss 战伤害玩家无反馈（玩家不知道为什么死）
- ❌ 同一 boss 在不同副本完全相同（缺乏新意）
- ❌ Boss 走 LOD（机制可能丢失）
- ❌ 所有 Boss 都有"标志性招式"（资源不够，质量稀释）

---

## 14. 里程碑

| 阶段 | 交付 |
|---|---|
| P3 早 | custom_handler 框架；首个 Boss 原型 |
| P3 中 | 1 个标志性 Boss 完整（多阶段 / telegraph / 击杀反馈） |
| P3 末 | Boss 战盲测：玩家 4.0+ 评分 |
| P4 早 | 5 个标志性 Boss + 10 个普通 Boss |
| P4+ | 持续扩展；多难度版本 |

---

## 15. 文档维护

- **Owner**：Boss 战斗设计 Lead + Tech Lead + 美术 Lead 三方共担
- **关联文档**：
  - `OVERVIEW.md`（§2 软目标"AI 深度"）
  - `AI_ARCHITECTURE.md`（基础 AI 框架）
  - `AI_LOD.md`（boss 不走标准 LOD）
  - `SKILL_SYSTEM.md §8`（custom_handler）
  - `COMBAT_FEEL.md §4`（cinematic camera）
  - `ANIMATION_INTEGRATION.md`（boss 动画）
  - `09_tools/COMBAT_REPLAY.md`（boss 战回放，待写）

---

**文档结束。**

**核心纪律重申**：
1. **Boss 战是 MMO 的故事高潮**：值得花资源做好
2. **多阶段 + telegraph + 标志性招式**：三大要素缺一不可
3. **custom_handler 是 boss 的特权**：但要严格审核（占比 < 15%）
4. **难度通过机制不通过数值**：好的难度让玩家学习
5. **击杀反馈必须强烈**：奖励玩家的努力
6. **15 个标志性 + 50 个中等 + 30 个普通**：资源分配清晰
