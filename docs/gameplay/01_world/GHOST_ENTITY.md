# Ghost Entity 机制（Ghost Entity）

> **用途**：定义 Atlas 跨 cell 实体感知与交互机制——Real / Ghost 副本协议、跨 cell 同步频率、跨 cell RPC 路由、战斗中实体跨 cell 行为。这是 BigWorld 经典机制在 Atlas 中的强化版。
>
> **读者**：工程（必读）、网络工程师（必读）。
>
> **状态**：草案 v0.1 — 待团队评审。
>
> **前置文档**：`OVERVIEW.md`、`CELL_ARCHITECTURE.md`、`HIT_VALIDATION.md`、`MOVEMENT_SYNC.md`
>
> **关联文档**：`COMBAT_EVENT_ORDERING.md`（跨 cell 事件排序）、`DENSITY_ADAPTIVE_NETWORKING.md`（cell 内订阅）

---

## 1. 设计目标与边界

### 1.1 问题

实体 A 在 cell #1，实体 B 在 cell #2（边界相邻）。如果不解决跨 cell：
- A 看不到 B（视觉上不可接受，玩家会"突然出现"）
- A 攻击不到 B（边界处技能命中失效）
- 跨 cell 无法组队、buff、aura 联动

但**让 cell 直接读对方 cell 的实体数据**是反模式——
- 跨进程数据竞态
- 跨网络延迟成 cell 仿真延迟
- 难以保证 cell 仿真的确定性

### 1.2 解决思路

引入 **Ghost** 概念（BigWorld 经典）：
- 每个实体在其权威 cell 称为 **Real**
- 在邻接 cell 留**只读副本**称为 **Ghost**
- Ghost 数据由 Real 周期同步过来
- 邻接 cell 通过 Ghost"看到"另一 cell 的实体

```
实体 A 在 cell #1（Real）
邻接 cell #2、#3、#4 各持有 A 的 Ghost
A 在 cell #2 内的 ghost 是 cell #1 的 A 的副本
```

### 1.3 设计目标

1. **跨 cell 视觉一致**：玩家看到另一 cell 内的实体如同同 cell
2. **跨 cell 战斗可行**：边界处的攻击、技能命中正常工作
3. **同步频率可调**：闲置 cell 低频，战斗 cell 升频
4. **不破坏权威性**：Ghost 是只读，所有写操作走 Real
5. **对客户端透明**：客户端不感知"实体在哪个 cell"

### 1.4 非目标

- **不做实时镜像**：Ghost 总有滞后（按同步频率），可接受
- **不取代客户端 AoI**：Ghost 是服务端进程间机制，客户端订阅是另一层（参见 `DENSITY_ADAPTIVE_NETWORKING.md`）
- **不传所有数据**：Ghost 只持有跨 cell 必需的字段子集

---

## 2. Real vs Ghost

### 2.1 角色定义

```
┌─────────────────────────────────────────────────────────┐
│  Real（权威副本）                                        │
│  - 唯一一个，存在于实体当前所在 cell                      │
│  - 仿真核心：处理输入、推进 timeline、应用伤害             │
│  - 写操作必须经过 Real                                   │
│  - 持有完整状态（所有 stat / buff / inventory ref ...）   │
├─────────────────────────────────────────────────────────┤
│  Ghost（只读副本）                                        │
│  - 多个，存在于邻接 cell                                  │
│  - 同步从 Real 周期推送（每 N ticks）                     │
│  - 仅持有"邻 cell 关心的字段"子集                         │
│  - 跨 cell 操作通过 Ghost 路由到 Real                    │
└─────────────────────────────────────────────────────────┘
```

### 2.2 Ghost 持有的字段

```cpp
struct GhostEntityState {
  // 标识
  uint64_t entity_id;
  CellId   real_cell_id;       // 指向 Real 所在 cell
  EntityType type;
  
  // 空间
  float3   position;            // cell 内坐标转 world 坐标后存
  float3   velocity;
  uint16_t yaw;
  
  // 战斗相关（粗粒度）
  uint8_t  team_or_faction;
  uint8_t  flag_state;          // iframe / super_armor / dead 等关键 flag
  float    hp_percent;          // 不存绝对值，省带宽
  float    radius;              // 碰撞半径
  
  // 同步元数据
  uint32_t last_sync_tick;      // Real 上次同步给我的 tick
  uint8_t  sync_frequency;      // 当前同步频率档
};
```

总计 ~64 字节/Ghost。

**Real 的字段**多得多（几 KB），但只有以上子集需要 Ghost 同步。

### 2.3 何时使用 Ghost vs Real

| 操作 | 用 Real | 用 Ghost |
|---|---|---|
| 写实体 stat（伤害） | ✅ | ❌（必须路由到 Real） |
| 读位置（距离查询） | ✅ | ✅（Ghost 数据已足） |
| 读 HP（UI 显示） | ✅ | ✅（粗粒度即可） |
| 读队伍（敌我判定） | ✅ | ✅ |
| 触发 buff | ✅ | ❌（路由到 Real） |
| 命中判定 | ✅ | ✅（参见 §6） |
| AI 决策（视觉感知） | ✅ | ✅（AI 把 Ghost 当目标） |

**写永远 Real，读优先 Ghost**。

---

## 3. Ghost 创建与销毁

### 3.1 创建条件

Real 实体进入 cell C 时，**给所有"感知范围内的邻接 cell"创建 Ghost**：

```
Real 在 cell C，AoI 范围 R（如 80m）：
  ↓
  对所有 cell C'（满足 distance(C', C) < R + 256m）：
    在 C' 上创建 Real 的 Ghost
```

例：Real 位于 cell (5,5)，AoI=80m，邻 cell (4,4)/(5,4)/(6,4)/(4,5)/(6,5)/(4,6)/(5,6)/(6,6) 都创建 Ghost（共 8 个）。

### 3.2 销毁条件

- Real 离开邻接范围（移动到远处）
- Real 实体死亡 + 清理
- 邻接 cell 销毁（极少）

### 3.3 同 CellApp 的 Ghost

如果 Real 和邻接 cell 在**同一 CellApp 进程**：
- Ghost 实际是个轻量结构（共享 Real 的内存指针）
- 不需要 IPC 同步
- 性能开销几乎为零

如果不同 CellApp：走完整 Ghost 协议（IPC 同步）。

### 3.4 创建/销毁的协议

```
Cell #1 上 Real entity E 移动接近 cell #2 边界
  ↓
1. Cell #1 检测：E 进入 cell #2 的 ghost 区
  ↓
2. Cell #1 发消息给 #2: "CreateGhost(E, snapshot)"
  ↓
3. Cell #2:
   ├─ 接收并创建 Ghost
   └─ 加入本地 ghost map
  ↓
4. 后续每 N ticks 同步更新（§4）
```

销毁逆向：发 `DestroyGhost(entity_id)`。

---

## 4. Ghost 同步频率

### 4.1 三档频率

按 Real 与 Ghost 实体之间的"战斗紧密度"动态调整：

| 档 | 同步频率 | 触发条件 |
|---|---|---|
| **Idle** | 4 Hz | Real 与邻 cell 实体均无战斗交互 |
| **Aware** | 10 Hz | Real 在战斗中或邻 cell 有玩家在战斗 |
| **Combat** | 30 Hz | Real 与邻 cell 实体有命中判定可能（< 20m） |

### 4.2 决策

每秒重评：

```cpp
SyncFrequency DecideSyncFreq(Entity real, Cell ghostCell) {
  if (real.IsInCombat() || HasNearbyCombatants(real, ghostCell)) {
    if (Distance(real, ghostCell.center) < 20.0f) return SyncFrequency.Combat;
    return SyncFrequency.Aware;
  }
  return SyncFrequency.Idle;
}
```

### 4.3 同步内容

同步消息（packed）：

```cpp
struct GhostSyncMessage {
  uint64_t entity_id;
  uint32_t real_tick;             // Real 当前 tick
  
  // 增量字段（仅变化的）
  uint8_t  flags;                  // 哪些字段在 payload 中
  // float3 position;               // 若 flag.HasPos
  // float3 velocity;               // 若 flag.HasVel
  // uint16_t yaw;                  // 若 flag.HasYaw
  // uint8_t state_flags;           // 若 flag.HasState
  // float hp_percent;              // 若 flag.HasHp
};
```

只发变化的字段，未变化字段沿用上次值。

### 4.4 频带计算

每 CellApp 跨进程同步带宽估算：
- 单 Ghost 同步消息 ~32 字节（含包头）
- Combat 档：30 Hz × 32 = 960 B/s/ghost
- Aware 档：10 Hz × 32 = 320 B/s/ghost
- Idle 档：4 Hz × 32 = 128 B/s/ghost

单 CellApp 假设 100 邻接 ghost：
- 全 Combat 档：96 KB/s（极端，热点战斗）
- 全 Idle 档：12.8 KB/s（常态）

跨 CellApp IPC 带宽完全可控。

### 4.5 客户端不参与 Ghost

Ghost 是**服务端跨进程机制**，与客户端 AoI 订阅无关。客户端订阅参见 `DENSITY_ADAPTIVE_NETWORKING.md`。

---

## 5. 跨 Cell RPC 路由

### 5.1 问题

CellApp #1 上的 Entity A（Real）想给 Entity B（Ghost）施加 buff。直接写 Ghost = 反模式（Ghost 只读）。

### 5.2 路由机制

```cpp
void Entity::ApplyBuffViaRouting(BuffId buffId, Entity target, ...) {
  if (target.IsGhost()) {
    // 跨 cell RPC
    Cell realCell = target.AsGhost().RealCellId;
    SendCrossCellMessage(realCell, new ApplyBuffRpc {
      target_entity_id = target.id,
      buff_id = buffId,
      source_entity_id = this.id,
      // ...
    });
  } else {
    // 同 cell 直接调用
    target.BuffSystem.Apply(buffId, this, ...);
  }
}
```

### 5.3 跨 cell RPC 协议

```cpp
struct CrossCellRpcMessage {
  CellId   target_cell;
  uint16_t rpc_id;            // ApplyBuff / DamageTarget / EmitEvent / ...
  ByteArray payload;          // 序列化的参数
  uint64_t source_entity_id;  // 发起者
  uint32_t source_tick;       // 发起 tick（用于事件排序）
};
```

### 5.4 时序保证

跨 cell RPC 在**接收 cell 的下一 tick** 处理：

```
Source Cell 在 tick T：
  发送 CrossCellRpc
  
Target Cell 在 tick T+1（最快）：
  接收并应用
```

**最多 1 tick 延迟**（同机器 IPC < 5ms，跨机器 < 50ms）。

### 5.5 RPC 类型清单

| RPC | 用途 |
|---|---|
| `ApplyBuff` | 施加 buff |
| `RemoveBuff` | 移除 buff |
| `DamageTarget` | 跨 cell 伤害 |
| `HealTarget` | 跨 cell 治疗 |
| `KnockBack` | 跨 cell 击退 |
| `RpcSkillStart` | 跨 cell 技能广播 |
| `EmitCombatEvent` | 跨 cell 事件 |

每种 RPC 有 schema 定义（FlatBuffers 或自定义）。

---

## 6. 跨 Cell 命中判定

参见 `HIT_VALIDATION.md §9`。本节展开 Ghost 在其中的角色。

### 6.1 流程

```
Cell #1 上 Entity A 释放技能，hitbox spawn 在 cell #1 边界
hitbox 扫描发现 Ghost B（B 的 Real 在 cell #2）：
  ↓
1. Cell #1 上判定：hitbox 是否覆盖 Ghost B 的位置（Ghost 提供位置）
  ↓
2. 若覆盖，cell #1 不直接应用伤害（Ghost 只读）
  ↓
3. Cell #1 发 CrossCellRpc 给 cell #2: "DamageTarget(B, dmg, ...)"
  ↓
4. Cell #2 在下一 tick 收到：
   ├─ 检查 B 的 Real 当前位置（可能已移动）
   ├─ 重新形状判定（lag comp 后 B 可能不在 hitbox 内）
   └─ 若仍命中，应用伤害
  ↓
5. 触发 OnDamageTaken handler 等
  ↓
6. 反向 RPC：通知 cell #1 "B 已受击" → A 的 OnDamageDealt handler 触发
```

### 6.2 一致性问题

跨 cell 命中**比同 cell 慢 1 tick**：
- 玩家可能感知微小延迟
- 边界处技能"打到 B" 的视觉反馈可能延迟 33ms
- PvE 接受（极少集中边界）
- PvP arena 内**不存在跨 cell**（单 instance）→ 不影响

### 6.3 双重判定

Cell #1 用 Ghost 位置先判（粗），cell #2 用 Real 位置再判（精确）。
- 第一次判减少跨 cell 消息（Ghost 远不在 hitbox → 不发 RPC）
- 第二次判保证准确（Real 位置权威）

少量"第一次判中、第二次未中"的情况是正常（B 已移动），玩家感知"打空"。

---

## 7. 跨 Cell 移动与 Real 迁移

### 7.1 实体跨边界

参见 `CELL_ARCHITECTURE.md §5.2`：实体跨边界时 `real_cell_id` 改变。

```
Tick T:
  Entity E 在 cell #1（Real）
  邻接 cell #2 持有 Ghost
  E 移动到 cell #2 边界

Tick T+1（移动跨边界）:
  Real 从 cell #1 迁移到 cell #2（变 Real）
  Ghost 在 cell #2 销毁
  Ghost 在 cell #1 创建（如果 cell #1 仍在 AoI 内）
  Ghost 在原 cell #2 邻接的其他 cell 也建立
```

**Ghost 与 Real 角色互换**：原来的 Ghost 容器（cell #2）变成 Real，原来的 Real（cell #1）变成 Ghost。

### 7.2 战斗中迁移

按 `OVERVIEW.md §5.3`：战斗中实体不主动迁移。但如果**击退** / **位移技** 推过边界（被动迁移）：

```
Entity E 在 cell #1 处于战斗中
被技能击退跨过 cell #2 边界
  ↓
1. CellApp #1 准备迁移：序列化 E 的完整战斗状态
   ├─ active SkillInstance（如果有）
   ├─ 所有 BuffInstance
   ├─ MovementCommand 状态
   └─ position history（lag comp 用）
  ↓
2. 发给 CellApp #2 + ack
  ↓
3. CellApp #2 接收并恢复战斗状态
  ↓
4. tick T+1: cell #2 上 E 是 Real，继续战斗
   cell #1 上原 Real 数据销毁，可能创建 Ghost（如果还在 #1 的 AoI 内）
```

实现复杂度高但视觉自然。

### 7.3 同步状态时机

**关键**：迁移瞬间所有"待处理"的 RPC 必须正确处理：
- Real 在 cell #1 时收到 cell #X 来的 RPC？→ 仍处理
- Real 已迁到 cell #2，cell #X 还按旧映射发了 RPC？→ cell #1 转发到 cell #2

CellAppMgr 广播映射表更新有延迟（< 1 秒），其间消息可能 misrouted——cell 必须支持 forward。

---

## 8. AoI 与 Ghost 的关系

### 8.1 AoI 的两层

```
┌────────────────────────────────────────┐
│ 客户端 AoI（玩家视角）                  │
│ - 决定客户端订阅哪些远端实体             │
│ - 50–80m 内全订阅，远处分级             │
│ - 详见 DENSITY_ADAPTIVE_NETWORKING.md  │
├────────────────────────────────────────┤
│ Ghost AoI（CellApp 进程间）             │
│ - 决定 cell 之间互相同步哪些实体         │
│ - 每 cell 邻接 8 cells，全部互相 ghost  │
│ - 范围 = max client AoI + 安全 buffer   │
└────────────────────────────────────────┘
```

Ghost AoI **总是不小于客户端 AoI**——确保客户端订阅时本地有 Ghost 数据可用。

### 8.2 Ghost 是上游

客户端订阅依赖 Ghost：
- 客户端在 cell #2 内，订阅了 cell #1 的实体 X
- cell #2 必须有 X 的 Ghost 数据，才能响应订阅
- 没 Ghost → 无法广播给客户端

### 8.3 性能联动

如果某 cell 没有玩家观察，**邻 cell 的 Ghost 没有意义**——可以降低同步频率。

但实践中所有 cell 都可能有 NPC 巡逻 / AI 感知，Ghost 通常都有用。简化处理：始终维护 Ghost。

---

## 9. 性能预算

### 9.1 同步消息数量

单 CellApp 管 8 cell，每 cell 平均 30 实体：
- 每 cell 邻接 8 个其他 cell（含同 CellApp 内部和跨 CellApp）
- 每 cell 创建 Ghost 数：30 实体 × 8 邻接 = 240 ghost 同步关系
- 单 CellApp 总 ghost 关系：8 cells × 240 = 1920

每秒消息：
- Idle 档（4 Hz）：1920 × 4 = 7680 msg/s
- Combat 档（30 Hz）：1920 × 30 = 57600 msg/s

实际不全 Combat 档，估算：
- 总同步开销：~10000 msg/s 平均
- IPC 开销：< 5 ms / s（千分之 5）

### 9.2 同 CellApp 内 Ghost

不需要 IPC 同步（共享内存）。仅记录"哪些 cell 含 ghost"，O(1) 访问。

跨 CellApp Ghost 占主要开销。

### 9.3 内存

每 Ghost 64 字节，1920 ghost = 120 KB / CellApp。可忽略。

### 9.4 监控

- 单 CellApp ghost 总数
- IPC 消息率
- IPC 消息延迟（p99 < 10ms）
- 跨 cell RPC 失败率（应 < 0.01%）

---

## 10. FAQ 与反模式

### Q1: Ghost 跟 client AoI subscription 是一回事吗？

**不是**。Ghost 是**服务端进程间**机制，client subscription 是**服务端→客户端**广播。

Ghost 让 cell #1 知道 cell #2 有 entity X 存在；
client subscription 让玩家客户端知道 X 存在。

两者互补：客户端需要 X 的位置 → CellApp 必须有 X 的 Ghost 数据 → 通过 Ghost 同步获取。

### Q2: 为什么 BigWorld 用 Ghost，CryENGINE/Unreal 不用？

CryENGINE / Unreal 经典是**单进程多线程**架构（一个 server 跑所有），没有跨进程实体边界问题，不需要 Ghost。

BigWorld 是**多进程分布式**架构，必须有 Ghost。Atlas 继承这个模型。

### Q3: Ghost 数据滞后会导致命中"鬼影"吗？

可能——Ghost 比 Real 滞后 30–250ms（按频率）。判定 Ghost 命中后路由到 Real，Real 重新判可能 miss。

PvE 边界场景**可接受**（玩家偶尔感知"应该打中却没"）。
PvP arena **不存在跨 cell**，无此问题。

### Q4: Ghost 能不能完全省略，cell 间直接共享内存？

理论上可以（仅同 CellApp 内）。但跨 CellApp 共享内存：
- 跨进程同步原语复杂（mutex / semaphore）
- 一个 CellApp 崩溃影响共享内存
- 无法跨机器扩展

Ghost 是为**网络分布式**设计，比共享内存更通用。

### Q5: Combat 档 30 Hz 同步会不会比 cell 内仿真还快？

不会——cell 内仿真也是 30 Hz（副本）。Ghost 同步频率 ≤ 仿真频率，不可能更快。

只有 Idle 档（4 Hz）远低于仿真频率，节省带宽。

### Q6: Ghost 能调用 Real 的方法吗？

不能——Ghost 是只读数据结构，无方法。所有"操作"通过跨 cell RPC 请求 Real 执行。

### Q7: Ghost 同步丢包怎么办？

跨 CellApp 是**进程间通信（IPC）**，通常基于本地 socket / 共享内存队列，**不丢包**（有重传 / 队列阻塞）。

跨机器走 TCP / 可靠 UDP，也不丢包。

仅极端故障（机器宕机）才会丢失，但那时 CellApp 也挂了，超出 Ghost 范畴。

### Q8: 战斗实体跨 cell 击退时，邻接 cell 的客户端怎么看到？

视觉上：客户端看到的远端实体（Ghost 推送）会平滑跨过 cell 边界（位置数据是 world 坐标，cell 边界对客户端透明）。

服务端：实体 Real 从 cell #1 迁到 cell #2，但客户端订阅是连续的（BaseApp 路由）。

### Q9: Ghost 影响命中判定的延迟比 lag compensation 还严重吗？

不会。Ghost 同步延迟（最坏 250ms Idle）只在"未发生战斗"时；一旦战斗发生升到 Combat 档（33ms 延迟，1 tick 等价于副本本身的时间精度）。

跨 cell 战斗的延迟主要是 RPC 路由（1 tick），与同 cell 战斗的延迟相同档次。

### Q10: 客户端能感知"我跨 cell 了"吗？

不能——cell 是服务端架构概念。客户端只看到：
- 我的角色位置（world 坐标）
- 视野内的其他实体
- 网络协议层不暴露 cell_id

唯一可能感知的是**短暂网络抖动**（cell handover 期间 ~50ms），但这与正常网络抖动难以区分。

---

### 反模式清单

- ❌ 直接修改 Ghost 数据（Ghost 只读）
- ❌ 跨 cell 直接读 Real 数据（应通过 Ghost 或 RPC）
- ❌ 让客户端发"我在 cell X" 给服务端（cell 是服务端概念）
- ❌ Ghost 同步频率与 cell 仿真频率不一致（应 ≤ 仿真频率）
- ❌ 在 Ghost 上调用业务方法（应路由到 Real）
- ❌ 用 Ghost 作为持久化数据源（Real 才是权威）
- ❌ 大量 RPC 跨 cell（应批量打包）
- ❌ Ghost 数据序列化用 schema 不固定的格式（升级会破坏兼容）

---

## 11. 里程碑

| 阶段 | 交付 |
|---|---|
| P1 末 | 同 CellApp 内 cell-cell 共享（无 IPC） |
| P2 中 | 跨 CellApp Ghost 同步协议 |
| P2 末 | 跨 cell RPC 路由（ApplyBuff / DamageTarget） |
| P3 | 战斗中跨 cell 击退处理 |
| **P4** | **完整 Ghost AoI 自适应频率；性能压测** |
| P4+ | 故障演练；CellAppMgr 映射表 hot reload |

---

## 12. 文档维护

- **Owner**：Tech Lead + Network Engineer
- **关联文档**：
  - `OVERVIEW.md`
  - `CELL_ARCHITECTURE.md`（Cell 物理分区）
  - `MOVEMENT_SYNC.md`（实体跨 cell 移动）
  - `HIT_VALIDATION.md §9`（跨 cell 命中判定）
  - `BUFF_SYSTEM.md`（buff 跨 cell 路由）
  - `DENSITY_ADAPTIVE_NETWORKING.md`（client AoI vs Ghost AoI）
  - `COMBAT_EVENT_ORDERING.md`（跨 cell 事件排序）

---

**文档结束。**

**核心纪律重申**：
1. **Real 唯一权威，Ghost 只读副本**：写永远走 Real
2. **Ghost 频率自适应**：4 / 10 / 30 Hz 三档动态切换
3. **跨 cell 操作走 RPC**：禁止直接读写 Ghost
4. **战斗中可跨 cell 击退**：复杂但用户体验值得
5. **Ghost 与 client AoI 不是一回事**：内层服务端机制，外层客户端订阅
