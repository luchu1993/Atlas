# Atlas Gameplay 架构总览（北极星）

> **这份文档的用途**：Atlas 项目游戏玩法系统的全局北极星。所有后续详细设计文档都必须与本文对齐；若本文需要更新，先在本文调整后再更新下游文档。
>
> **读者**：项目所有成员（工程、策划、美术、管理）。工程师可作为架构依据，策划/美术可作为能力边界和迭代节奏参照。
>
> **更新节奏**：每次重大架构决策后更新；小迭代季度复核一次。
>
> **状态**：草案 v0.1 — 待团队评审确认。

---

## 1. 项目定位

Atlas 是一款 **MMOARPG**（动作 MMO 角色扮演游戏），服务端由本仓库的分布式多进程框架驱动，客户端使用 Unity。

**参考对标**：
- **黑色沙漠（Black Desert Online, BDO）** — **PvE / 战斗手感**核心对标，目标超越
- **龙之谷（Dragon Nest）** — **PvP 竞技**核心对标，目标达到其水准

**核心规格**：

| 维度 | 规格 |
|---|---|
| 世界结构 | **无缝开放世界**（非 session 化） |
| 单服最大并发 | **5000 玩家** |
| PvE 密集场景 | 热点（练级点/活动点）最多 ~100 实体（玩家+怪物） |
| PvP 形态 | **实例化竞技场**（session-based），1v1 / 2v2 / 4v4 |
| 目标平台 | **桌面**（Windows / Linux Server），不支持移动端 |
| 服务端语言 | C++ 20（核心框架） + C#（CoreCLR 脚本） |
| 客户端引擎 | Unity（C#） |

---

## 2. 质量靶子

### 2.1 硬目标（go/no-go）

两条并列硬目标，P3 里程碑（见 §10）的手感验证覆盖两者，**任一不达标都不进入下一阶段**：

1. **PvE / 战斗手感：超越 BDO** — 单人/组队战斗的打击感、动画流畅度、hit pause、技能衔接
2. **PvP 竞技：达到龙之谷水准** — 1v1/2v2/4v4 小规模竞技的手感、响应、公平性

### 2.2 软目标（努力方向）

- **AI 深度**：练级点怪物行为多样、有群组配合（相比 BDO 的"站桩待砍"怪物是可拉开差距的明确方向）
- **PvP 生态**：ranked 天梯、赛季制、观战系统（龙之谷成熟玩法参考）
- **访问门槛**：新玩家 30 分钟内理解基础战斗、2 小时内掌握基本连招
- **运营稳定**：上线首月无重大 dupe / 经济崩盘事件（对标 New World 反面教材）

### 2.3 明确**不做**的事

- **不做**移动端（避免分散精力与降低画质上限）
- **不做**跨服战斗（单服 5000 已足够覆盖主要玩法）
- **不做**大规模 PvP（国战、攻城、节点战、百人以上团战）——这是 BDO 的主战场但不是我们的
- **不做**完全开放的无限制 PK（分级 PK 制度降低玩家流失；开放世界 PK 受限，主要竞技在 arena）

**注**：虽然不做大规模 PvP，但 PvP 的**公平性和手感要求是最高档**（竞技向——对标龙之谷 ranked 体验）。"不做国战"是规模决策，不等于"PvP 随便做做"。

---

## 3. 核心认知

### 3.1 MMOARPG 是乘法不是加法

传统 MMO 技术栈和 ARPG 战斗技术都有成熟方案。难的是**两者的交集**：

```
MMO 独有问题                ARPG 独有问题
────────────               ────────────
持久化/经济                 动作手感/帧数据
万人尺度/AoI               动画驱动战斗
分布式/分片                实时命中判定
社交/工会                  连招/取消
        ↘             ↙
     ┌──────────────────┐
     │   MMOARPG 交集    │
     │                  │
     │ 动作战斗×万人同框 │
     │ 持久成长×帧级手感 │
     │ 经济系统×实时战斗 │
     │ 跨玩家动作×网络延迟│
     │ 反作弊×开放PK     │
     └──────────────────┘
```

对标 BDO 不只是单独做好 MMO 或 ARPG，**是把交集做到 BDO 现有水平或更好**。项目风险也主要在交集——独立方向都有现成答案。

### 3.2 团队约束导出的架构原则

团队初期 3 人（2 技术），最终 100+（美术 > 策划 >> 技术）。技术长期是瓶颈。由此导出：

1. **数据驱动优先**：策划能改数值就不让工程改代码
2. **工具链投入占比 35–40%**：否则内容产出跟不上
3. **系统接口清晰**：美术/策划产出要能不经工程介入入库
4. **架构能扛人员更替**：文档齐备、规范明确，新人 2 周上手

---

## 4. Day-1 硬决策（锁定，动土即定）

这些决策改变代价极高，已在第 1 天锁定：

| 项目 | 决策 |
|---|---|
| **数值类型** | `float`（IEEE 754 严格模式，禁用 `-ffast-math`） |
| **服务端 tick（开放世界）** | 20 Hz（50 ms/tick） |
| **服务端 tick（副本/竞技场）** | 30 Hz（33 ms/tick） |
| **时间基准** | `int64` tick 序号（从 session 启动为 0 单调递增） |
| **RNG 源** | `xoshiro256**`，种子 = hash(session_id, entity_id, tick, counter) |
| **世界坐标** | `float3`，世界原点居中，单图上限 16 km |
| **Cell 内坐标** | entity.position 存"cell 内相对坐标 + cell_id"，避免远离原点精度丢失 |
| **实体 ID** | `int64`（32 位会耗尽） |
| **技能 ID 空间** | `int32`，按职业分段 |
| **网络协议版本** | 每 packet 带 `proto_ver` byte，握手协商 |
| **保存 schema** | FlatBuffers + 强制版本字段 + 前向兼容规则 |
| **线程模型** | CellApp 单线程 + IO 线程池；cell 内实体访问无锁串行 |
| **C#/C++ 边界** | 移动仿真 C++（native plugin），战斗/技能/buff/AI C#，跨边界调用尽量批处理 |

详细规约见 `00_foundations/DETERMINISM_CONTRACT.md`（待写）。

---

## 5. 核心设计原则（10 条）

每条都是硬约束。所有下游设计文档必须与之一致。

1. **视觉立即响应，数值服务端权威** — 手感的基石。客户端预测表现，服务端仲裁伤害。
2. **事件带时间戳，时间戳即真相** — 一致性的基石。所有战斗事件按 server_tick 排序。
3. **战斗实体锁定在 Cell** — 边界稳定性的基石。战斗中的实体不跨 cell 迁移。
4. **近身高频，远景低频** — 规模的基石。订阅频率随距离分层。
5. **所有物品操作审计，所有经济可回滚** — 安全的基石。
6. **纯函数仿真，确定性契约为先** — 端同的基石。移动/战斗核心计算不依赖外部状态。
7. **工具先于内容** — 产能的基石。系统未建工具就不鼓励大量内容生产。
8. **反作弊是常态不是项目** — 长期的基石。配置专职团队持续运营。
9. **测试矩阵覆盖密度/延迟/丢包/热点** — 质量的基石。
10. **手感验证是 go/no-go** — 项目健康的基石。P3 不达标不进入 P4。

---

## 6. 系统架构总图

```
┌──────────────────────────────────────────────────────────────────┐
│                     GAMEPLAY 层（策划/美术产出）                    │
│   技能表/buff/怪物/关卡/经济/任务/剧情/UI                          │
└──────────────┬───────────────────────────────────────────────────┘
               │  真源 = Excel；Build pipeline → FlatBuffers 字节码
┌──────────────▼───────────────────────────────────────────────────┐
│            COMBAT CORE（端共享 C# 程序集 Atlas.CombatCore.dll）    │
│  ┌────────────┐  ┌──────────┐  ┌────────┐  ┌─────────────┐       │
│  │Skill:      │  │Buff:     │  │AI:     │  │Stat/Damage │       │
│  │ Timeline+SM│  │ 状态+钩子 │  │ 行为树 │  │ 公式        │       │
│  └──────┬─────┘  └────┬─────┘  └────┬───┘  └──────┬──────┘       │
│         └────────────┬┴──────────────┴──────────────┘             │
│                Action / DSL / Graph 执行器                        │
└─────────┬──────────────────────────────────────────┬─────────────┘
          │                                          │
┌─────────▼──────────┐              ┌─────────────── ▼─────────────┐
│  CLIENT (Unity)    │              │  SERVER (Atlas)              │
│                    │              │                              │
│  ┌──────────────┐  │  ◄──协议──►  │  ┌──────────────┐           │
│  │输入预测+和解  │  │              │  │CellApp:       │           │
│  │动画 Mecanim  │  │              │  │ Witness+AoI   │           │
│  │远端插值+外推 │  │              │  │ Cell 分裂     │           │
│  │手感层(hitpause│  │              │  │ 实体仿真      │           │
│  │ camera VFX)  │  │              │  └──────┬────────┘           │
│  └──────────────┘  │              │         │                     │
│  ┌──────────────┐  │              │  ┌──────▼────────┐           │
│  │Native Plugin │  │              │  │BaseApp:       │           │
│  │ (C++共享仿真)│  │              │  │ Entity 行为   │           │
│  └──────────────┘  │              │  └──────┬────────┘           │
└────────────────────┘              │         │                     │
                                    │  ┌──────▼────────┐            │
                                    │  │DBApp:         │            │
                                    │  │ 持久化/事务   │            │
                                    │  └───────────────┘            │
                                    │                              │
                                    │  ┌──────────────────────┐    │
                                    │  │反作弊/遥测/A-B/热更  │    │
                                    │  └──────────────────────┘    │
                                    └──────────────────────────────┘
```

### 6.1 层间契约

- **Gameplay 层 → Combat Core**：通过 Excel 产出数据（FlatBuffers 字节码），不直接调用代码
- **Combat Core → Client/Server**：Atlas.CombatCore.dll 作为共享程序集，两端各自加载、各自实例化
- **Client ↔ Server**：通过版本化协议通信；战斗事件和位置快照走不同通道

### 6.2 三条网络通道（职责严格分离）

| 通道 | 承载内容 | 可靠性 | 频率 | 典型延迟预算 |
|---|---|---|---|---|
| **Channel 1: 位置/状态快照** | 实体位置、朝向、速度、状态标志 | UDP unreliable | 开放世界 20Hz / 战斗区 30Hz | 可丢 |
| **Channel 2: 战斗事件** | 技能释放、命中、buff 变化、死亡 | UDP reliable + 时间戳 | 事件驱动 | 必达，有序 |
| **Channel 3: 持久操作** | 背包、交易、存档、公会、市场 | TCP / 强可靠 | 极低频 | 可承受 100 ms+ |

**纪律**：不能用通道 1 传战斗事件（丢了就错），不能用通道 3 传位置（延迟太高），不能用通道 2 传高频位置（带宽爆）。

---

## 7. 七个关键技术难点

按项目杀伤力排序，每个对应未来独立详细文档。

### 难点一：网络延迟下的手感保持
- **本质**：玩家感知的"延迟"不等于 RTT，是"预测失败时的反馈质量"
- **Atlas 策略**：视觉立即响应、数值服务端权威、hit spark 预播、伤害数字等 ack
- **相关文档**：`03_combat/COMBAT_FEEL.md`, `05_client/CLIENT_PREDICTION.md`

### 难点二：PvE 热点密度 与 PvP 竞技公平性（双重挑战）
- **范围修订**：原先考虑的"200v200 大规模 PvP"已从项目范围移除（见 §2.3）。此难点现在聚焦两个具体场景：
  - **PvE 密集**：热点区域（练级点、世界事件）~100 实体聚集
  - **PvP 竞技**：8 人以内 arena 但要求**最高帧精度和公平性**
- **PvE 策略**：动态 cell 分裂 + 优先级快照分层 + 客户端 LOD + 重要事件专线
- **PvP 策略**：独立质量档（专用 CellApp、潜在 60Hz tick、严格 lag compensation、重反作弊）
- **相关文档**：`01_world/CELL_ARCHITECTURE.md`, `01_world/DENSITY_ADAPTIVE_NETWORKING.md`, `02_sync/LAG_COMPENSATION.md`

### 难点三：战斗事件跨客户端一致性
- **本质**：没有绝对"同时"，只有"服务端 tick 序列"才是真相
- **Atlas 策略**：事件带严格时间戳 + 客户端缓冲 100ms 后重排序消费 + 本地预测不倒放只补偿
- **相关文档**：`03_combat/COMBAT_EVENT_ORDERING.md`

### 难点四：经济系统 dupe 防御
- **本质**：任何竞态都是通胀源，不能消灭但必须能侦测+回滚
- **Atlas 策略**：高价值操作走强一致通道、item uuid 唯一、审计日志不可删、5 分钟对账
- **相关文档**：`07_persistence/TRANSACTION_MODEL.md`, `07_persistence/AUDIT_AND_RECONCILIATION.md`

### 难点五：帧数据时代的内容产能
- **本质**：工具产能决定项目产能，25 职业 × 60 技能的规模硬碰硬
- **Atlas 策略**：Unity Editor 帧数据编辑器 + 战斗回放 + 相似技能模板复用 + CI 自动化 QA
- **相关文档**：`09_tools/FRAME_DATA_EDITOR.md`, `06_data/DATA_PIPELINE.md`

### 难点六：Cell 边界与跨进程一致性
- **本质**：开放世界最 bug 多的地方，BigWorld 有基础方案但需要加强
- **Atlas 策略**：战斗状态不迁移 + Ghost 升级到战斗频率 + 命中结算在 target 的 real cell
- **相关文档**：`01_world/GHOST_ENTITY.md`

### 难点七：Action PvP 反作弊
- **本质**：是持续战争而非一次任务，必须配专职团队长期运营
- **Atlas 策略**：服务端权威一切 + 行为异常 ML + 蜜罐 + 审计回滚 + Ban wave
- **相关文档**：`08_security/ANTI_CHEAT_STRATEGY.md`, `10_liveops/BAN_WAVE_PROCESS.md`

---

## 8. 三条贯穿始终的张力

无法消除，只能平衡。每个设计决策都在这三条张力的某一端上。

```
张力 A: 响应性 ←──────────→ 权威性
        客户端预测            服务端仲裁
        平衡点：视觉预测、数值权威

张力 B: 一致性 ←──────────→ 可用性
        强事务保护经济         低延迟保护手感
        平衡点：战斗最终一致、经济强一致、分通道

张力 C: 规模   ←──────────→ 质量
        万人同框              帧级精度
        平衡点：距离分层、近身高精度、远景存在感
```

**团队在评审设计时应随时追问**：这个方案偏向哪一端？代价是什么？是否和"10 条原则"以及质量靶子一致？

---

## 9. 分期路线图

### P0：地基（1–2 月）
**交付物**：空世界两人移动同步能跑通。
- 确定性契约文档 + 代码实现
- 网络协议 + 版本化
- 时间/坐标/RNG 规范
- Entity 模型骨架
- Native plugin 骨架 + P/Invoke 接口

### P1：移动全栈（2–3 月）
**交付物**：4 人在封闭房间里互相看到对方流畅移动 + 0 延迟手感。
- 输入预测、和解、远端 Hermite 插值
- Lag compensation 原型
- 延迟/丢包模拟框架
- 基础 UI（能操作即可）

### P2：战斗核心（3–4 月）
**交付物**：一个测试职业 + 5 个技能；能打怪、能 PK、数值正确。
- Timeline + State Machine 运行时
- Buff 系统
- 命中判定 + lag compensation
- Data pipeline（Excel → FlatBuffers + codegen）
- 1 个测试职业内容

### P3：手感打磨 ★go/no-go
**交付物**：一个完整职业 × 20 技能打磨到"能爽"；**PvE 和 PvP 两个场景**盲测均通过标准。
- Hit pause / hit stop
- Camera effects (shake / slomo / framing)
- Animation-driven movement 曲线
- Frame data 编辑器 v1
- **两场景独立测试**：
  - PvE：打训练假人 / 打小怪 / 打 Boss
  - PvP：1v1 arena 对战
- **未达标不进入 P4，宁可推迟 3 个月继续打磨**

### P4：规模（2–3 月）
**交付物**：500 实体（100 玩家 + 400 AI）同图流畅。
- Cell 动态分裂
- AI 架构 + LOD
- Ghost entity 跨 cell
- 100 bot 压力测试 + 密度自适应验证

### P5：MMO 特性（持续）
**交付物**：可公开测试的游戏雏形。
- 开放世界内容 / 副本 / 多 PvP 模式
- 持久化 / 经济 / 交易 / 社交
- 任务 / 剧情 / 升级曲线
- 反作弊 / live ops / 运营工具

**总时长**：P0–P4 合计 10–15 个月，P5 持续。

---

## 10. 里程碑门控

每个 P 结束有明确的 go/no-go 检查：

| 里程碑 | 门控标准 | 不达标处理 |
|---|---|---|
| P0 | 端同仿真 diff 测试 10k 帧 0 漂移 | 重写仿真核心直到过关 |
| P1 | 4 人 150ms 模拟延迟下手感可接受 | 调优预测和解，极端情况可加输入缓冲 |
| P2 | 5 技能平衡数值端同，伤害偏差 <1% | 修复 data pipeline 或战斗公式 |
| **P3** | **盲测 8 人双场景评分：PvE 手感 ≥ 4.0/5.0（对比 BDO）+ PvP 手感 ≥ 4.0/5.0（对比龙之谷）** | **推迟，继续打磨，直到达标** |
| P4 | 500 实体 30Hz 下服务端 CPU <70% | 降级 cell 配置或优化热路径 |

---

## 11. 文档地图

所有详细设计文档放在 `docs/gameplay/` 下，按主题目录组织：

```
docs/gameplay/
├── OVERVIEW.md                          ← 本文（北极星）
├── MILESTONE_COMBAT_FEEL.md             ← P3 手感验证细则（next）
├── 00_foundations/
│   ├── DETERMINISM_CONTRACT.md          ★ 早期必写
│   ├── NETWORK_PROTOCOL.md
│   ├── COORDINATE_AND_TIME.md
│   └── ENTITY_MODEL.md
├── 01_world/
│   ├── CELL_ARCHITECTURE.md
│   ├── GHOST_ENTITY.md
│   └── DENSITY_ADAPTIVE_NETWORKING.md
├── 02_sync/
│   ├── MOVEMENT_SYNC.md
│   ├── ENTITY_SNAPSHOT.md
│   └── LAG_COMPENSATION.md
├── 03_combat/
│   ├── COMBAT_ACTIONS.md
│   ├── SKILL_SYSTEM.md
│   ├── BUFF_SYSTEM.md
│   ├── STAT_AND_DAMAGE.md
│   ├── HIT_VALIDATION.md
│   ├── COMBAT_FEEL.md                   ★ BDO 超越的秘密
│   └── COMBAT_EVENT_ORDERING.md
├── 04_ai/
│   ├── AI_ARCHITECTURE.md
│   ├── AI_LOD.md
│   └── BOSS_AI.md
├── 05_client/
│   ├── UNITY_INTEGRATION.md
│   ├── ANIMATION_INTEGRATION.md
│   └── CLIENT_PREDICTION.md
├── 06_data/
│   ├── DATA_PIPELINE.md
│   ├── LOCALIZATION.md
│   └── CONTENT_STANDARDS.md
├── 07_persistence/
│   ├── DB_ARCHITECTURE.md
│   ├── TRANSACTION_MODEL.md
│   ├── AUDIT_AND_RECONCILIATION.md
│   └── SAVE_SCHEMA.md
├── 08_security/
│   ├── ANTI_CHEAT_STRATEGY.md
│   ├── AUTH_AND_SESSION.md
│   └── DATA_VALIDATION.md
├── 09_tools/
│   ├── FRAME_DATA_EDITOR.md             ★ 产能关键
│   ├── COMBAT_REPLAY.md
│   ├── TELEMETRY_AND_METRICS.md
│   └── LOAD_TESTING.md
└── 10_liveops/
    ├── DEPLOYMENT.md
    ├── AB_TESTING.md
    ├── HOTFIX_PLAYBOOK.md
    └── BAN_WAVE_PROCESS.md
```

**★ 标记** 是超越 BDO 级别的关键文档，其他 MMO 也有同类但这些是 Atlas 核心差异化投入。

### 11.1 写作顺序

初期 3 人团队，按依赖关系推进：

1. `OVERVIEW.md`（本文）
2. `MILESTONE_COMBAT_FEEL.md`（P3 go/no-go 判定细则）
3. `00_foundations/DETERMINISM_CONTRACT.md`（技术硬核基础）
4. `03_combat/SKILL_SYSTEM.md`（前几轮讨论已成熟）
5. `03_combat/BUFF_SYSTEM.md`
6. `02_sync/MOVEMENT_SYNC.md`
7. 并行启动 `09_tools/FRAME_DATA_EDITOR.md`（工具规划）
8. 其他按实现阶段展开

### 11.2 文档规范

- **篇幅**：初期阶段每份 300–600 行；团队扩张后按需加深
- **结构**：目标 / 核心决策 / 接口 / 实现要点 / 测试 / FAQ
- **工程/非工程分层**：技术细节与设计意图分段，便于不同读者抓重点
- **可追溯**：每个决策标注"为什么选 A 不选 B"，方便后期判断是否需要重评

---

## 12. 团队 Scaling 考量

### 3 人阶段（现在）
- 文档**极简**，保持迭代速度
- 所有决策可口头讨论 + 短文档记录
- 工具可以粗糙，能用就行
- 重点：**锁定硬决策**（Day-1 决策、10 条原则）

### 10–20 人阶段
- 文档**加深**，接口规范明确
- 模块负责制，每个工程师主导 1–2 个模块
- 工具投入开始密集（专职 1 人做工具）
- 重点：**接口与责任划分**

### 50+ 人阶段
- 文档**严格评审**，每份有 Owner + Reviewer
- 反作弊、live ops、QA 各成独立团队
- 工具团队 5–10 人
- 重点：**流程与协作**

### 永远不变的原则
- **技术决策文档化**：口头约定会随人员流动丢失
- **工具优先**：任何阶段工程都不能轻视工具
- **原则 §5 的 10 条** 贯穿始终，不因规模变化而妥协

---

## 13. 约定与禁区

### 约定
- 所有游戏逻辑时间单位用 **server tick**，不用 wall clock
- 所有随机必须走项目 RNG 接口，不用 `rand()` / `System.Random`
- 所有物品操作必须写**审计日志**（持久化 + 不可删）
- 所有新协议包必须带 **proto_ver**
- 所有跨 cell 交互必须走 **Ghost entity 机制**，不直接跨进程读写

### 禁区
- **禁止**客户端发送"我在位置 X"的绝对位置（只发输入帧）
- **禁止**在战斗逻辑里用 `Time.time` / `DateTime.Now` / `std::chrono::now()`
- **禁止**用 `[SerializeField]` / ScriptableObject 作为跨引擎数据源（Unity 特有）
- **禁止**在同一 cell 内用多线程修改实体状态（单线程串行）
- **禁止**为了"临时紧急"跳过审计日志 / 反作弊校验
- **禁止**在 P3 手感验证通过前大规模加技能（会污染手感基线）

---

## 14. 下一步

本文确认后，**顺序推进**：

1. `MILESTONE_COMBAT_FEEL.md` — 定义 P3 "盲测 4.0/5.0" 是如何度量的，测试流程、场景、评分标准
2. `00_foundations/DETERMINISM_CONTRACT.md` — 锁定浮点/RNG/时间/坐标的技术细节
3. `03_combat/SKILL_SYSTEM.md` — 把前几轮讨论的 Timeline + State Machine 正式化
4. 并行工作：工具团队起步 `09_tools/FRAME_DATA_EDITOR.md`

---

## 附录 A：术语表

| 术语 | 含义 |
|---|---|
| Cell | 服务端空间分区单位，由 CellApp 持有和仿真 |
| Ghost | 实体在相邻 cell 的只读副本，用于跨 cell 交互 |
| Witness | 负责向一个玩家客户端推送 AoI 内实体快照的服务端对象 |
| Timeline | 技能的线性帧事件序列，确定性执行 |
| SM | State Machine，技能/战斗状态切换控制 |
| Action | Timeline/Buff 的原子执行单元（DamageAction、DashAction 等） |
| Buff | 事件驱动的持续状态，含 modifier 和 handler |
| Hit Pause | 击中时两端动画短暂冻结（50–80ms），增强打击感 |
| Lag Compensation | 服务端按射手 RTT 回溯目标位置进行命中判定 |
| CoreCLR | .NET 运行时，Atlas 服务端嵌入它执行 C# 脚本 |
| P/Invoke | C# 调用 C++ native plugin 的跨语言机制 |
| AoI | Area of Interest，实体感知范围，决定订阅哪些其他实体 |
| FBS | FlatBuffers，零拷贝二进制序列化格式，用于 Excel 导出产物 |

---

**文档结束。**

**批注**：任何对本文的修改（新增 / 变更 / 删除）都应该在 git commit message 中明确标注 "OVERVIEW updated: ..."，方便追溯。
