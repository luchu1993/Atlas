# Cell 架构设计（Cell Architecture）

> **用途**：定义 Atlas 无缝大世界的空间分区方案——静态 cell 网格、动态分裂合并、CellApp 进程映射、实体归属与迁移、负载均衡。这是支撑 5000 并发 + 100 实体热点的核心架构。
>
> **读者**：工程（必读）、网络工程师（§5、§6 必读）、运维（§7、§9 必读）。
>
> **状态**：草案 v0.1 — 待团队评审。
>
> **前置文档**：`OVERVIEW.md`、`DETERMINISM_CONTRACT.md`、`MOVEMENT_SYNC.md`
>
> **下游文档**：`GHOST_ENTITY.md`（跨 cell 实体）、`DENSITY_ADAPTIVE_NETWORKING.md`（订阅策略）、`PERSISTENCE_DESIGN.md`（实体持久化，待写）

---

## 1. 设计目标与边界

### 1.1 核心目标

1. **5000 并发可承载**：单服务器集群支撑 5000 玩家同时在线
2. **热点弹性**：练级点 / 活动点 100 实体聚集仍流畅（30Hz 副本节奏可降到 20Hz）
3. **无缝体验**：玩家跨 cell 不感知（无 loading screen、无可见 lag）
4. **横向扩展**：增加 CellApp 进程线性提升承载量
5. **故障隔离**：单 CellApp 崩溃不影响其他 cell（最多影响其管辖区域内玩家）

### 1.2 与 BigWorld 的关系

Atlas 继承 BigWorld 的核心架构思想（CellApp / BaseApp / CellAppMgr / BaseAppMgr / Reviver），但针对 MMOARPG 战斗需求做了调整：
- **战斗中实体锁 cell**（避免迁移破坏战斗状态）
- **更激进的 cell 分裂阈值**（单 cell 100 实体而非 500）
- **跨 cell 命中判定**（参见 `HIT_VALIDATION.md §9`）
- **战斗副本 = 特殊的"独占 cell"**（30Hz tick）

### 1.3 非目标

- **不做完全无 cell 模型**（成本爆炸）
- **不动态调整 cell 物理边界形状**（仅在固定网格上分裂/合并）
- **不做跨服务器 cell 迁移**（单服内迁移即可）
- **不为开放世界 PvP 战斗做特殊优化**（无大规模开放 PvP，参见 OVERVIEW §2.3）

---

## 2. 空间分区模型

### 2.1 静态网格

世界划分为固定大小的**网格 cell**（Quadtree 风格）：

```
Cell size: 256 m × 256 m × ∞（高度不分区）
```

16 km × 16 km 地图 = `64 × 64 = 4096` cells。

```
World map (16km × 16km):
  ┌─────┬─────┬─────┬─────┐
  │ 0,0 │ 1,0 │ 2,0 │ ... │
  ├─────┼─────┼─────┼─────┤
  │ 0,1 │ 1,1 │ 2,1 │ ... │
  ├─────┼─────┼─────┼─────┤
  │ ... │     │     │     │
  └─────┴─────┴─────┴─────┘
```

### 2.2 Cell ID 设计

```cpp
struct CellId {
  uint32_t world_id;    // 哪张地图（开放世界主图 / 副本 X）
  int16_t  grid_x;      // 网格 X 坐标
  int16_t  grid_y;      // 网格 Y 坐标
};
```

总计 8 字节，全局唯一。

### 2.3 实体坐标

参见 `DETERMINISM_CONTRACT.md §6`：实体位置 = `(cell_id, float3 local)`，`local` 范围 `[-128, 128]`。

```cpp
struct EntityPosition {
  CellId cell_id;
  float3 local;
};

// 跨 cell 距离计算
float Distance(EntityPosition a, EntityPosition b) {
  float3 a_world_offset = CellOriginOf(a.cell_id) + a.local;
  float3 b_world_offset = CellOriginOf(b.cell_id) + b.local;
  return Length(a_world_offset - b_world_offset);
}
```

注意：跨 cell 不同 `world_id`（开放世界 vs 副本）距离无意义。

### 2.4 高度方向

不在 Y 轴分 cell。原因：
- MMOARPG 玩家活动主要在水平面
- 垂直分区增加复杂度（飞行、跳跃跨高度 cell 触发迁移）
- Y 范围限制在 `[-512, 512]` 已足够

如有大型立体场景（高塔、深井），用单独的 `world_id`。

---

## 3. CellApp 进程模型

### 3.1 CellApp 与 cell 的关系

**多对多**：
- 一个 CellApp **管理多个 cell**（典型 4–16 个）
- 一个 cell **属于唯一一个 CellApp**

```
开放世界主图:
  CellApp #1: 管理 cell {(0,0), (0,1), (1,0), (1,1)}
  CellApp #2: 管理 cell {(2,0), (2,1), (3,0), (3,1)}
  CellApp #3: 管理 cell {(0,2), (0,3), (1,2), (1,3)}
  ...
  
副本：
  CellApp #100: 独占副本 X（单 cell 256m × 256m，无邻接）
```

### 3.2 单 CellApp 内单线程

参考 `OVERVIEW.md §4`：
- CellApp 进程**主线程串行处理**所有自管 cell
- IO（网络 / 数据库）走线程池
- 实体内存仅主线程访问，**无锁**

不同 CellApp 进程间通过 IPC（消息队列）通信。

### 3.3 CellApp 分组

按地图功能划分：
- **开放世界主图**：~16–32 个 CellApp（覆盖 16 km × 16 km）
- **副本池**：~50 个 CellApp，按需分配 / 回收
- **PvP arena 池**：~30 个 CellApp，session 化
- **机器层**：每物理服务器跑 4–8 CellApp 进程

5000 并发预估：
- 开放世界 ~3000 玩家 → 24 CellApp × 平均 125 玩家
- 副本 ~1500 玩家 → 200 副本 × 7.5 玩家
- PvP ~500 玩家 → 80 arena × 6 玩家

### 3.4 CellApp 启动

```
集群启动顺序：
  1. CellAppMgr 启动
  2. 各物理机启动 CellApp 进程
  3. CellApp 注册到 CellAppMgr
  4. CellAppMgr 分配初始 cell 给 CellApp
  5. CellApp 加载地形 / 静态实体 / 持久化数据
  6. 上线，接受玩家
```

### 3.5 CellApp 与 BaseApp 的分工

参见 BigWorld 架构（CLAUDE.md 已述）：

| 进程 | 职责 |
|---|---|
| **CellApp** | 空间仿真：实体位置、AoI、命中判定、buff、movement |
| **BaseApp** | 实体行为：玩家会话、技能逻辑、persistence proxy |
| **DBApp** | 数据库读写：背包、装备、角色档案 |
| **机器代理 machined** | 进程注册、服务发现 |

一个实体的 `Real Cell` 在 CellApp，`Real Base` 在 BaseApp。两者通过 RPC 协作。

### 3.6 故障隔离

CellApp 崩溃：
- Reviver 进程检测，重启 CellApp
- 受影响 cell 由 CellAppMgr 临时分配给其他 CellApp
- 玩家短暂卡顿（5–10 秒），但不掉线
- 实体持久化数据已存 BaseApp / DBApp，不丢失

详见 `09_tools/LOAD_TESTING.md`（待写）的故障演练。

---

## 4. Cell 分裂与合并

### 4.1 触发条件

CellAppMgr 周期监控（每 30 秒）：
- 各 CellApp CPU 使用率
- 各 cell 实体数
- 各 cell tick 耗时

触发**分裂**：
- 单 cell 实体数 > 100（开放世界）/ > 16（副本）
- 单 cell tick 耗时 > 50% 预算（25ms / 50ms 副本，10ms / 50ms 开放）
- 单 CellApp CPU > 70%

触发**合并**：
- 相邻 cell 加起来实体数 < 30
- 持续 5 分钟低负载

### 4.2 分裂方式

**方法 A：按物理边界（不可分裂）**
我们的设计——cell 在世界坐标上是固定的，不能动态切分物理空间。

**方法 B：进程层迁移**
- "分裂"实际是把 cell 从一个 CellApp 迁移到另一个 CellApp
- 物理 cell 边界不变
- 改变的是 cell → CellApp 映射

我们采用**方法 B**。

### 4.3 迁移流程（Cell Handover）

```
Trigger: CellApp #1 负载过高，决定迁出 cell (5,3)
  ↓
1. CellAppMgr 选择目标 CellApp #2（负载较低）
  ↓
2. CellAppMgr 通知 #1 准备迁出 cell (5,3)
  ↓
3. CellApp #1:
   ├─ 暂停 cell (5,3) 接受新实体进入
   ├─ 序列化 cell 状态：所有实体、buff、技能 instance
   └─ 发送给 CellApp #2
  ↓
4. CellApp #2:
   ├─ 接收并反序列化
   ├─ 实体加入仿真
   └─ 通知 BaseApp 更新 ghost 引用
  ↓
5. CellAppMgr:
   ├─ 更新全局 cell 映射表
   └─ 广播给所有需知 CellApp
  ↓
6. CellApp #1: 销毁 cell (5,3) 本地状态
  ↓
7. 客户端透明：仍然连同一 BaseApp，BaseApp 转发请求到新 CellApp
```

**全程目标**：< 1 秒完成，玩家不感知。

### 4.4 迁移期间的处理

迁移过程实体状态有"短暂双份"：
- 旧 CellApp #1：仍在仿真（直到迁移完成的瞬间）
- 新 CellApp #2：刚加载，未参与仿真

**单时刻只有一个权威**：迁移完成的 tick 边界**原子切换**。

```
Tick T:    CellApp #1 仿真，#2 等待
Tick T+1:  CellApp #2 仿真，#1 不再处理
```

**CellAppMgr 通过分布式锁保证原子性**——同一 cell 不能在两个 CellApp 同时仿真。

### 4.5 战斗状态实体的处理

按 `OVERVIEW.md §5.3` 原则：**战斗中实体锁定 cell 不迁移**。

```cpp
bool CanMigrateCell(CellId cell) {
  for (auto entity : cell.entities) {
    if (entity.IsInCombat() || entity.HasActiveSkillInstance()) {
      return false;  // 暂时不迁移
    }
  }
  return true;
}
```

战斗结束前推迟迁移，避免破坏战斗状态。

如果 cell 持续战斗超过 5 分钟（极少见，通常副本场景），考虑：
- 接受短暂高负载
- 若仍超载，紧急强制迁移（接受短暂战斗状态扰动）

### 4.6 分裂的局限

cell 物理边界 256m × 256m 不可改。如果**单一物理点**聚集 100+ 实体（如世界 boss 战），cell 物理上无法"再切分"。

应对：
- 战斗设计层避免极端聚集（boss 给 AoE 让玩家分散）
- 升级单 CellApp 的硬件（专用机器跑这个 cell）
- 极端情况降低 tick 频率（30Hz → 20Hz）保命

不要试图在工程层做"动态分裂同 cell"——架构成本太高。

---

## 5. 实体归属与迁移

### 5.1 实体归属

每个实体在任一时刻属于唯一一个 cell：

```cpp
class Entity {
  CellId       real_cell_id;   // 当前权威 cell
  float3       local_position;  // cell 内坐标
  EntityState  state;
  // ...
};
```

`real_cell_id` 由其当前世界位置决定（位置 → cell mapping 是确定性函数）。

### 5.2 跨 cell 移动

实体移动跨过 cell 边界时触发**实体迁移**（不同于 cell-CellApp 迁移）：

```
Tick T:
  Entity E.position = (cell_A, local=(127, 0, 50))   # 接近 A 的右边界
  
Tick T+1: 移动了 0.5m
  原始位置 = (cell_A, local=(127.5, 0, 50))
  正常化：发现 local.x > 128，需迁移到 cell_B
  
  转换：
    Entity E.real_cell_id = cell_B
    Entity E.local_position = (-127.5, 0, 50)   # 从 B 的左边界进入
```

迁移流程：
- 同 CellApp 内迁移：仅改 `real_cell_id`，无 IPC
- 跨 CellApp 迁移：通过消息（详见 §5.3）

### 5.3 跨 CellApp 实体迁移

```
Tick T: Entity E 在 cell_A（CellApp #1）
        计算下一帧位置发现要进 cell_B（CellApp #2）
  ↓
1. CellApp #1:
   ├─ 序列化 E 完整状态（位置、HP、buff、active skill）
   └─ 发消息 "EntityMigrating" 到 CellApp #2
  ↓
2. CellApp #2:
   ├─ 反序列化 E
   ├─ 加入本地实体集
   └─ 发回 ack
  ↓
3. CellApp #1:
   ├─ 收到 ack，从本地实体集移除
   └─ 通知 BaseApp 更新 cell ref
  ↓
4. BaseApp:
   └─ 后续 RPC 路由到 CellApp #2
```

**期间状态**：
- T 到 T+1 之间 entity 在两边都存在
- T+1 之后只在 #2

**典型耗时**：< 50 ms（同机器 IPC < 5ms，跨机器 < 50ms）。

### 5.4 战斗中实体跨 CellApp 怎么办

战斗状态实体不主动跨 cell（被 cell 边界"反弹"或停止移动）。但如果**击退** / **位移技** 被动把它推过去：

**两种策略**：

策略 A：**禁止跨 cell**
- 击退轨迹被 cell 边界阻挡
- 实体停在边界
- 视觉可能怪异（突然停下）

策略 B：**允许跨 cell + 同步迁移**
- 击退过程中实体迁移
- 战斗状态完整迁移到新 CellApp
- 视觉自然

**推荐 B**——MMOARPG 战斗必须容忍这种边界场景。代价是实现复杂度。

### 5.5 玩家断线 / 死亡

- 玩家断线：实体保留在 cell N 秒（30 秒），断线超时则迁移到"离线 cell"或销毁（看设计）
- 玩家死亡：实体保留尸体几秒，复活到指定点（不同 cell）

死亡复活的迁移走标准跨 cell 流程。

---

## 6. CellAppMgr 全局协调

### 6.1 职责

```
CellAppMgr (single instance per server cluster)
  ├─ 维护 cell → CellApp 映射表
  ├─ 监控各 CellApp 负载
  ├─ 决策 cell 迁移
  ├─ 协调副本 / arena 创建/销毁
  ├─ 故障检测 + 恢复
  └─ 全局只读 cell 元数据广播
```

### 6.2 单点风险

CellAppMgr 是**单点**——挂了集群无法继续 cell 调度。应对：
- 主备模式：Standby CellAppMgr 监听主进程心跳，主挂自动接管
- Reviver 进程独立监控
- 主备切换 < 5 秒

仅用于**协调**，不在快路径上——CellApp 可独立运行短时间（5–10 分钟）即使 CellAppMgr 短暂离线。

### 6.3 cell 映射查询

CellApp 之间不直接询问 CellAppMgr "X cell 现在在哪"——这会成为瓶颈。

**广播机制**：
- CellAppMgr 维护映射表
- 任何更新（cell 迁移）后广播 delta 给所有 CellApp
- CellApp 本地缓存映射表
- 无需 round-trip 查询

更新频率：每秒 1 次（远低于业务频率）。

### 6.4 副本 / Arena 调度

```
玩家请求进副本 X:
  ↓
BaseApp 询问 CellAppMgr "请分配一个 CellApp 给副本 X"
  ↓
CellAppMgr:
  ├─ 选取空闲 CellApp（按负载排序）
  ├─ 分配新 cell_id（独占）
  └─ 通知 CellApp 启动副本 cell
  ↓
玩家被传送到 cell
```

副本结束后 CellApp 释放 cell 回到池中。

---

## 7. 性能预算

### 7.1 单 CellApp 承载

假设管理 8 cell（开放世界）：
- 平均 30 实体/cell × 8 = 240 实体
- 单 tick 消耗：
  - Movement 仿真：240 × 20 μs = 4.8 ms
  - 命中判定：100 active hitbox × 30 μs = 3 ms
  - Buff 系统：240 × 10 buff × 1 μs = 2.4 ms
  - 网络广播：~5 ms
  - **合计 ~15 ms**（占 50ms 开放世界 tick 30%）

热点 cell（100 实体）：单 cell 单 tick ~10 ms（独占 50% 预算）→ 触发分裂迁移。

### 7.2 网络带宽

单 CellApp（240 实体）：
- 下行总和（向客户端）：240 × 12 KB/s = 2.9 MB/s（不可怕）
- 跨 CellApp IPC：~500 KB/s（实体迁移 + ghost 同步）

### 7.3 内存

单 CellApp：
- 实体数据：240 × 5 KB = 1.2 MB
- 历史缓冲：240 × 30 samples × 32 bytes = 230 KB
- 索引、AoI 结构：~5 MB
- 地形数据：~100 MB（共享，mmap）
- **合计 ~110 MB**（地形为主）

物理机 32 GB RAM 可跑 ~250 CellApp 进程（远超需要）。

### 7.4 5000 并发整体

| 项 | 数值 |
|---|---|
| 玩家总数 | 5000 |
| 开放世界 CellApp | 25 个 |
| 副本/arena 池 | 100 个 |
| 物理服务器 | 5–8 台（每台 8 进程） |
| 服务器集群带宽 | 100 Mbps + |

---

## 8. 配置与运维

### 8.1 配置参数

```yaml
# atlas_cell_config.yaml
world:
  size_m: 16384         # 16 km
  cell_size_m: 256      # 256 m × 256 m
  
cell_app:
  cells_per_app: 8      # 默认每 CellApp 管 8 cell
  max_entities_per_cell: 100  # 触发分裂阈值
  combat_lock: true     # 战斗中不迁移
  
load_balancing:
  check_interval_s: 30
  high_cpu_threshold: 0.7
  low_load_threshold:
    avg_entities_per_cell: 4
    duration_s: 300
```

### 8.2 监控指标

每 CellApp 上报：
- CPU 使用率
- 内存占用
- 单 tick 耗时分布（p50/p95/p99）
- 各 cell 实体数
- IPC 消息率
- 实体迁移次数

CellAppMgr 聚合后产生 dashboard。

### 8.3 告警

- 单 CellApp CPU > 90% 持续 1 分钟 → 紧急告警
- 单 CellApp 崩溃 → P0 告警（Reviver 触发自动恢复）
- cell 迁移失败 → P1 告警
- 内存使用 > 80% → 告警

### 8.4 容量规划

按玩家增长动态扩容：
- 监控玩家高峰并发
- 接近 80% 容量时预警
- 增加物理机 → 拉起新 CellApp → CellAppMgr 自动平衡

无需手动迁移玩家——系统自适应。

---

## 9. 副本与 Arena 特殊处理

### 9.1 副本 = 独占 cell

PvE 副本（4 玩家小队 / 8 人 raid）：
- 单 cell（256m × 256m 足够）
- 单独 CellApp 进程（避免影响开放世界）
- **30Hz tick**（开放世界是 20Hz）
- 无 cell 邻接 → 无 ghost 跨边界

副本生命周期：
- 创建：玩家组队进入时
- 运行：直到玩家全部退出 / 完成 / 失败
- 销毁：5 分钟无人后

### 9.2 PvP Arena = 独占 session

1v1 / 2v2 / 4v4 arena（≤ 8 玩家）：
- 单 cell（小地图，64m × 64m 足够，仍用 256 cell 容器）
- 单独 CellApp 进程
- **30Hz tick** 或更高（视 P3 验证）
- 严格 lag compensation

arena 启动：
- 匹配系统组队完成
- BaseApp 请求 CellAppMgr 分配 arena cell
- 玩家瞬移进入

arena 销毁：
- 比赛结束后立即（玩家踢回外面）

### 9.3 副本/Arena 与开放世界的边界

玩家从开放世界进副本：
- 客户端发"请求进副本"
- BaseApp 协调
- 玩家实体的 BaseApp 状态保留，但 CellApp 状态切换到副本 CellApp
- 客户端切换 AssetBundle（副本场景）
- 体感：短暂 loading（5–10 秒）

退出反向：销毁副本 entity → 在主图复活点重生 → 加载主图 cell。

---

## 10. FAQ 与反模式

### Q1: 256 m × 256 m 是不是太小了？

实际玩家视野 ~50 m。256 m cell 内可容纳整个视野 + 邻接缓冲。够大避免频繁跨边界，够小避免过度拥挤。

数据驱动调整：若实测 cell 边界跨越频繁影响性能，可调到 512 m。但成本是热点更难分裂。

### Q2: 副本用 30Hz 而开放世界用 20Hz，端同会出问题吗？

不会——确定性契约要求**单 session 内 tick 频率不变**（参见 `DETERMINISM_CONTRACT.md §5.5`）。

玩家进副本 = 切到独立 session（独立 RNG seed、独立时间基线）。两个 session 的状态不交叉。

### Q3: 战斗中实体跨 cell 击退怎么处理？

按 §5.4 策略 B：允许跨 cell 同步迁移。实现要点：
- MovementCommand 起点是旧 cell，终点可能在新 cell
- 服务端推进过程中检测越界 → 同步迁移
- 客户端继续显示曲线（位置数据用 cell + local 表达）

实现复杂但用户体验好——**值得投入**。

### Q4: CellAppMgr 单点挂了集群还能跑吗？

短期可以（5–10 分钟）：
- CellApp 各自缓存映射表
- 无新 cell 迁移、无新副本创建
- 已运行的 session 继续

长期不行——必须有备份。Standby CellAppMgr 自动接管。

### Q5: 1500 个副本同时运行扛得住吗？

按预估副本规模 7.5 人 / 副本，1500 副本 ≈ 11000 玩家——超过 5000 并发上限。

实际：典型时刻 ~100–200 副本同时跑（500 玩家在副本中）。CellApp 池有 50 个空闲就够。

### Q6: 为什么要"战斗中不迁移"，迁移很慢吗？

迁移本身 < 50ms（< 1 tick），技术上可承受。

**但破坏战斗状态**：
- ActiveSkill timeline 跨 CellApp 中断
- Hitbox 跟随骨骼跨进程同步困难
- Buff handler 引用关系断裂

避免迁移战斗实体让架构简单 100 倍，是值得的设计取舍。

### Q7: 如果整张地图 16 km × 16 km 不够呢？

加 `world_id`！多张地图各自独立网格：
- 主城（map_id=1）：8 km × 8 km
- 主图（map_id=2）：16 km × 16 km
- 北方雪原（map_id=3）：12 km × 12 km
- 副本（map_id=100, 101, ...）：各 256 m × 256 m

地图间通过传送切换，无需相邻 cell。

### Q8: 跨 map 玩家能互相看到吗？

不能——不同 `world_id` 的玩家完全隔离。这是设计意图（玩家在不同地图就是物理上不在一起）。

跨 map 唯一交互：通过游戏系统（聊天、邮件、组队邀请等）。

### Q9: cell 迁移期间客户端会感知吗？

不会感知（如果做对）：
- BaseApp 是玩家会话的稳定锚点
- BaseApp 把客户端请求路由到正确 CellApp
- 切换瞬间客户端只感觉 ~50ms 网络抖动（可忽略）
- 实体位置不变（cell 边界不变）

### Q10: 单服 5000 并发实测吗？

P4 阶段必须做大规模压测：
- 100 bot 模拟 → 500 bot → 5000 bot
- 真实战斗节奏（不是站桩）
- 包含副本 + arena
- 持续 2 小时观察稳定性

不通过则架构有问题，必须修。

---

### 反模式清单

- ❌ 在客户端逻辑里假设"我永远在同一 CellApp"（忽略迁移）
- ❌ 实体引用用指针/句柄而非 EntityRef（迁移后失效）
- ❌ 跨 cell 使用本地坐标比较（必须用 world 坐标转换）
- ❌ CellAppMgr 在快路径上参与（应仅协调）
- ❌ 实体数据写两个 CellApp（一定单权威）
- ❌ 战斗中强制迁移（破坏战斗）
- ❌ 自定义不规则 cell 形状（复杂度爆炸）
- ❌ 跨 world 直接交互（应通过游戏系统）

---

## 11. 里程碑

| 阶段 | 交付 |
|---|---|
| P0 末 | 静态 cell 网格；EntityPosition；坐标转换 |
| P1 中 | 单 CellApp 多 cell 管理；同 CellApp 内实体迁移 |
| P2 早 | 跨 CellApp 实体迁移；BaseApp 路由 |
| P2 末 | 副本 / arena 独立 CellApp；动态分配/回收 |
| P3 | 战斗实体跨 cell 击退处理 |
| **P4** | **CellApp 动态迁移（cell handover）；500 实体压力测试** |
| P4+ | 5000 并发集群测试；故障演练 |

---

## 12. 文档维护

- **Owner**：Tech Lead + Network Engineer
- **关联文档**：
  - `OVERVIEW.md`（§7 难点二、六引用本文）
  - `DETERMINISM_CONTRACT.md`（坐标系统）
  - `MOVEMENT_SYNC.md`（实体移动跨 cell）
  - `GHOST_ENTITY.md`（跨 cell 实体感知）
  - `DENSITY_ADAPTIVE_NETWORKING.md`（订阅频率）
  - `HIT_VALIDATION.md §9`（跨 cell 命中判定）
  - `PERSISTENCE_DESIGN.md`（实体持久化，待写）

---

**文档结束。**

**核心纪律重申**：
1. **静态网格 + 动态进程映射**：物理边界不变，进程负载可平衡
2. **战斗实体不迁移**：保护战斗一致性
3. **CellAppMgr 不在快路径**：仅协调，避免单点瓶颈
4. **副本独占 CellApp**：30Hz tick 不影响开放世界
5. **5000 并发是规模目标**：P4 必须压测验证
