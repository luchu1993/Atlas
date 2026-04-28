# 移动同步设计（Movement Sync）

> **用途**：定义 Atlas 中玩家主控角色移动、远端实体移动表现、技能驱动位移的同步机制。保证主控手感无感延迟、远端角色流畅自然、服务端权威防作弊。
>
> **读者**：工程（必读）、战斗策划（§8 技能位移必读）、客户端开发（Unity 侧，§11 必读）。
>
> **状态**：草案 v0.1 — 待团队评审。
>
> **前置文档**：`OVERVIEW.md`、`00_foundations/DETERMINISM_CONTRACT.md`、`03_combat/SKILL_SYSTEM.md`
>
> **下游文档**：`ENTITY_SNAPSHOT.md`（远端实体广播）、`LAG_COMPENSATION.md`（命中延迟补偿）、`01_world/CELL_ARCHITECTURE.md`（跨 cell 移动）

---

## 1. 设计目标与边界

### 1.1 三个不同的问题

移动同步不是**一个**问题，是**三个独立问题**，混在一起解会互相拖累：

| 问题 | 主要诉求 | 权威方 | 主要策略 |
|---|---|---|---|
| **A. 主控本地预测** | 0 延迟手感 | 客户端先行，服务端权威 | 输入帧 + 预测 + 和解 |
| **B. 远端实体插值** | 平滑视觉 | 服务端快照 | Hermite 插值 + 自适应外推 |
| **C. 技能位移** | 端同一致的曲线 | 服务端 | 构建期提取曲线，端同执行 |

**本文按这三条线各自展开**；它们共享底层仿真库但表现层策略完全不同。

### 1.2 核心指标（来自 OVERVIEW §0 精度预算）

| 维度 | 上限 |
|---|---|
| 主控预测误差（服务端仲裁前） | < 0.3 m（技能判定不受影响） |
| 远端渲染位置误差 | < 0.5 m |
| 延迟补偿窗口 | ≤ 200 ms |
| 软纠正阈值 | [0.3, 1.5] m |
| 硬 snap 阈值 | ≥ 5 m（击退/传送/位移技） |
| 服务端 Tick | 开放世界 20Hz，副本 30Hz |

### 1.3 非目标

- **不追求 CS:GO 级公平**（0 ping 差异）——MMO 语境下不可能，也不必要
- **不做客户端权威位置**——玩家发位置 = 速度挂/瞬移挂失控
- **不做完整物理模拟**（Havok/PhysX）——2.5D 胶囊 + 高度图足够（见 OVERVIEW §9.1）
- **不做 Root Motion 运行时**——非确定性，构建期提曲线

---

## 2. 架构总图

```
┌──────────────────────────────────────────────────────────────────┐
│                        Client (Unity)                            │
│                                                                  │
│   ┌────────────────┐   ┌──────────────┐   ┌──────────────────┐  │
│   │Input 采集       │   │Predictor     │   │Remote            │  │
│   │60Hz            │──►│ (C++)        │   │Interpolator      │  │
│   │                │   │ local loop   │   │ (Hermite)        │  │
│   └────────────────┘   └──────┬───────┘   └────────┬─────────┘  │
│                               │                    │            │
│                    ┌──────────▼─────────┐          │            │
│                    │AtlasMovement        │◄─────────┘            │
│                    │Controller           │                       │
│                    │ (写 transform)      │                       │
│                    └─────────────────────┘                       │
└───────────────────────────┬──────────────────────────────────────┘
                            │
                  Channel 1: UDP unreliable 位置快照 (20/30Hz)
                  Channel 2: UDP reliable 输入帧 + 事件
                            │
┌───────────────────────────▼──────────────────────────────────────┐
│                        Server (Atlas)                            │
│                                                                  │
│   ┌────────────────┐   ┌──────────────┐   ┌──────────────────┐  │
│   │Input Processor │   │Movement      │   │Snapshot           │  │
│   │ (接输入帧,     │──►│Simulator     │──►│Broadcaster        │  │
│   │  去重,校验)    │   │ (C++)        │   │ (AoI 过滤, 压缩)  │  │
│   └────────────────┘   └──────┬───────┘   └──────────────────┘  │
│                               │                                  │
│                    ┌──────────▼─────────┐                        │
│                    │History Buffer       │                        │
│                    │ (500ms 位置历史,    │                        │
│                    │  用于 lag comp)     │                        │
│                    └─────────────────────┘                        │
└──────────────────────────────────────────────────────────────────┘
```

**核心约束**：
- `Movement Simulator` 的核心函数 `apply_input()` 是**纯函数**，端共享 C++ 代码
- 服务端和客户端同 `apply_input()` 保证和解机制有效
- Unity 只通过 P/Invoke 调用 native plugin，不在 C# 重实现仿真逻辑

---

## 3. 输入帧协议（客户端 → 服务端）

### 3.1 为什么不发位置

**永远不让客户端发送"我在位置 X"**：
- 发位置 = 移动权威让给客户端
- 速度挂、瞬移挂几乎无法根治
- MMO 经济 + PvP 不能容忍此漏洞

客户端发**输入帧**，服务端用同一 `apply_input()` 推导位置——服务端永远是位置的唯一真源。

### 3.2 InputFrame 数据结构

```cpp
struct InputFrame {
  uint32_t  seq;              // 单调递增序号（和解 anchor）
  uint16_t  client_dt_ms;     // 客户端实际帧耗时
  int8_t    move_x;           // 移动方向 X 分量 (-127..127 映射 [-1,1])
  int8_t    move_z;           // 移动方向 Z 分量
  uint16_t  view_yaw;         // 朝向（全 16 位精度）
  int8_t    view_pitch;       // 俯仰（-90..90 映射）
  uint16_t  buttons;          // 按键 bitmask（跳跃、冲刺、技能 1-8）
  uint64_t  client_time_us;   // 客户端时间戳（时间同步用）
};
```

**字段设计理由**：
- `move_x/z` 用 int8 而非 float：减半带宽，精度足够（客户端摇杆/键盘输入本身是离散的）
- `view_yaw` 全精度：射击方向和技能朝向对精度敏感
- `buttons` 用 bitmask：同一帧按多键（技能 + 冲刺）可同时表达
- `client_time_us` 帮助服务端估算往返延迟，用于 jitter 处理

单帧大小 = **18 字节**。60Hz 发送 = 1.08 KB/s 上行。

### 3.3 冗余策略

**每个 UDP 包含最近 3 帧**：`[seq, seq-1, seq-2]`。

- 丢 1 包：下一包的 redundant 帧补上，无损
- 丢 2 包连续：前 2 包作为上一包的 redundant 仍补上
- 丢 3 包连续：才有输入丢失，服务端按"无输入"推进一步

**带宽代价**：3 帧 × 18 字节 = 54 字节/包，60Hz = 3.24 KB/s 上行。在桌面网络环境下忽略。

### 3.4 服务端接收处理

```cpp
void InputProcessor::OnPacketReceived(Packet& pkt, CombatContext& ctx) {
  for (auto& frame : pkt.input_frames) {
    if (frame.seq <= last_processed_seq_) continue;  // 已处理，去重
    if (frame.seq > last_processed_seq_ + MAX_SEQ_GAP) continue;  // gap 太大，可能是作弊
    
    // 加入待处理队列（按 seq 排序）
    pending_inputs_.push(frame);
  }
}

void InputProcessor::Tick(CombatContext& ctx) {
  // 每 tick 消费一个或多个输入帧（根据 client_dt 推进）
  int budgetMs = ctx.TickDurationMs;
  while (budgetMs > 0 && !pending_inputs_.empty()) {
    auto& frame = pending_inputs_.top();
    int useMs = std::min((int)frame.client_dt_ms, budgetMs);
    
    simulator_.ApplyInput(entity_, frame, useMs, ctx);
    
    budgetMs -= useMs;
    if (useMs >= frame.client_dt_ms) {
      last_processed_seq_ = frame.seq;
      pending_inputs_.pop();
    }
  }
  
  // 剩余预算无输入：按"无输入"推进（减速、重力）
  if (budgetMs > 0) {
    simulator_.ApplyNoInput(entity_, budgetMs, ctx);
  }
}
```

**`client_dt_ms` 校验**：
- 上限 100ms（防止作弊者用大 dt 跳过时间）
- 下限 4ms（防止刷快速输入）
- 超出范围 → 丢弃该帧并记录作弊日志

---

## 4. Movement Simulator（端共享 C++）

### 4.1 接口

```cpp
namespace atlas::movement {

struct MovementState {
  float3 position;     // cell 内相对坐标
  float3 velocity;
  uint16_t yaw;
  int8_t pitch;
  uint32_t flags;      // grounded / jumping / falling / dashing ...
  
  // 命令叠加层（§8）
  CommandOverride active_command;
  uint32_t command_elapsed_ms;
};

// 纯函数：输入状态 + 输入帧 + dt → 新状态
// 不依赖任何外部状态，不做任何 IO
MovementState ApplyInput(
  const MovementState& prev,
  const InputFrame& input,
  uint16_t dt_ms,
  const MovementConfig& config,
  const TerrainSample& terrain);

// 无输入时推进（减速、重力、摩擦）
MovementState ApplyNoInput(
  const MovementState& prev,
  uint16_t dt_ms,
  const MovementConfig& config,
  const TerrainSample& terrain);

}
```

### 4.2 `MovementConfig`（端共享数据）

```cpp
struct MovementConfig {
  float max_speed;              // m/s
  float acceleration;           // m/s²
  float deceleration;
  float turn_rate;              // rad/s
  float jump_impulse;
  float gravity;
  float air_control;            // [0,1]
  
  float capsule_radius;
  float capsule_height;
  
  // 技能限制
  bool can_move_while_casting;
  bool can_turn_while_casting;
};
```

从 Excel 加载，端共享（通过 `Atlas.CombatCore.Data`）。

### 4.3 `TerrainSample`（查询接口）

服务端与客户端各自实现：

```cpp
struct TerrainSample {
  std::function<float(float3)> height_at;
  std::function<bool(float3, float3)> collides_with_wall;
  // ... 其他空间查询
};
```

**关键约束**：
- 服务端 `TerrainSample` 查询权威地形数据
- 客户端 `TerrainSample` 必须查询**完全相同的数据**（即地形数据端共享）
- 禁止客户端额外查询 Unity `Physics.Raycast` 而服务端不查——会导致漂移

### 4.4 确定性契约遵从

`ApplyInput` 严格遵循 `DETERMINISM_CONTRACT.md`：
- 所有浮点用 `/fp:strict`，不用 `-ffast-math`
- 三角函数走 `Atlas.CombatCore.Math::Sin/Cos`（软件实现）
- 整数运算不依赖 overflow 未定义行为
- 不调用任何 `rand()` / 系统时钟

---

## 5. 主控客户端预测

### 5.1 每帧流程

```
1. 采集输入 → InputFrame{seq=N}
2. 立即本地仿真：state_N = apply_input(state_{N-1}, input_N, dt)
3. 存入 ring buffer: history[N] = {input_N, state_N, predicted_server_tick}
4. 发送 InputFrame（含冗余）
5. 写入 Unity transform（渲染当前 state）
```

**渲染位置 = 当前仿真位置 + visual_offset**（见 §5.4 平滑机制）。

### 5.2 History Buffer

```cpp
struct InputHistoryEntry {
  uint32_t seq;
  InputFrame input;
  MovementState predicted_state;
  uint32_t predicted_server_tick;
};

// Ring buffer，容量 120 帧（2 秒 @ 60Hz 覆盖最坏 RTT）
class PredictionHistory {
  InputHistoryEntry buffer_[120];
  uint32_t head_;  // 最新写入位置
  
  void Append(const InputHistoryEntry& e);
  InputHistoryEntry* FindBySeq(uint32_t seq);
  void TruncateBefore(uint32_t seq);   // 丢弃 seq 以前的历史
};
```

### 5.3 服务端 Ack 协议

**Ack 频率**：非每帧 ack，否则下行带宽无法承受。策略：
- 每 3 tick（100-150ms）发一次"状态回馈"
- Ack 内容：
  ```cpp
  struct StateAck {
    uint32_t acked_input_seq;      // 已处理的最高输入 seq
    uint32_t server_tick;
    MovementState authoritative_state;
  };
  ```

### 5.4 和解（Reconciliation）

客户端收到 `StateAck` 后：

```cpp
void Predictor::OnStateAck(const StateAck& ack) {
  auto* entry = history_.FindBySeq(ack.acked_input_seq);
  if (!entry) return;  // 太旧，已被 truncate
  
  float err = Distance(entry->predicted_state.position, 
                       ack.authoritative_state.position);
  
  if (err < SOFT_THRESHOLD_LOW) {      // < 0.3 m
    // Tier 1: 无感，悄悄纠正内部状态
    entry->predicted_state = ack.authoritative_state;
    ReplayFromSeq(ack.acked_input_seq + 1);
    // 不改 visual_offset_，玩家无感
  }
  else if (err < SOFT_THRESHOLD_HIGH) { // [0.3, 1.5] m
    // Tier 2: 视觉平滑补偿
    float3 oldRenderPos = GetRenderPosition();
    entry->predicted_state = ack.authoritative_state;
    ReplayFromSeq(ack.acked_input_seq + 1);
    visual_offset_ = oldRenderPos - GetCurrentSimPosition();
    visual_offset_tau_ = 300;  // ms
  }
  else if (err < HARD_THRESHOLD) {     // [1.5, 5] m
    // Tier 3: 快速视觉过渡
    float3 oldRenderPos = GetRenderPosition();
    entry->predicted_state = ack.authoritative_state;
    ReplayFromSeq(ack.acked_input_seq + 1);
    visual_offset_ = oldRenderPos - GetCurrentSimPosition();
    visual_offset_tau_ = 100;  // ms
  }
  else {                                // >= 5 m
    // Tier 4: 硬 snap (通常是击退/位移技)
    entry->predicted_state = ack.authoritative_state;
    ReplayFromSeq(ack.acked_input_seq + 1);
    visual_offset_ = {0, 0, 0};  // 立即 snap
  }
  
  history_.TruncateBefore(ack.acked_input_seq);
}

void Predictor::ReplayFromSeq(uint32_t startSeq) {
  auto* anchor = history_.FindBySeq(startSeq - 1);
  MovementState state = anchor->predicted_state;
  
  for (uint32_t seq = startSeq; seq <= current_seq_; ++seq) {
    auto* e = history_.FindBySeq(seq);
    state = apply_input(state, e->input, e->input.client_dt_ms, config_, terrain_);
    e->predicted_state = state;
  }
}
```

### 5.5 视觉偏移衰减

`visual_offset_` 表示"渲染位置 - 仿真位置"的差值。每帧按指数衰减：

```cpp
float3 Predictor::GetRenderPosition() {
  float dt = Time.deltaTime;
  float decay = exp(-dt * 1000.0f / visual_offset_tau_);
  visual_offset_ *= decay;
  
  if (Length(visual_offset_) < 0.01f) {
    visual_offset_ = {0, 0, 0};
  }
  
  return GetCurrentSimPosition() + visual_offset_;
}
```

**优点**：
- 仿真位置始终正确（技能判定用它）
- 视觉平滑过渡（玩家不感知 snap）
- 简单：一个向量 + 衰减常数
- 不改变移动输入反馈（仍然 0 延迟）

**选 `tau` 的原则**：
- 小偏差（Tier 2）：300ms 慢衰减，玩家不察觉
- 中偏差（Tier 3）：100ms 快衰减，避免长时间"鬼影"
- 大偏差（Tier 4）：0（立即对齐）——因为这类偏差常伴随显式事件（被击退），视觉上期望有突变

---

## 6. 远端实体插值

### 6.1 策略：延迟渲染 + Cubic Hermite

**不追求"实时"**——远端实体**渲染于过去 100-120ms**，换取：
- 接收到下一快照时有足够数据插值
- 对网络抖动天然容忍
- 符合人眼视觉舒适区

渲染时间 = `now - interp_delay`，其中：

```cpp
interp_delay_ms = clamp(100.0f, 2.0f * jitter_stddev_ms + 50.0f, 250.0f);
```

稳定网络 80ms，抖动网络自动扩大到 200ms。

### 6.2 快照数据

```cpp
struct EntitySnapshot {
  uint16_t entity_alias_id;   // AoI 内别名（节省 2 字节）
  uint32_t server_tick;
  PackedPos position;          // 量化坐标，见 §6.5
  PackedVel velocity;          // 量化速度
  uint16_t yaw;
  uint8_t state_flags;         // grounded / cast / stunned ...
};
```

见 `ENTITY_SNAPSHOT.md` 具体压缩策略。

### 6.3 Cubic Hermite 插值

已知两个快照 `S0 (t=t0)` 和 `S1 (t=t1)`，渲染 `t ∈ [t0, t1]`：

```cpp
float3 HermiteInterp(float t_normalized,  // [0, 1]
                     float3 p0, float3 v0,
                     float3 p1, float3 v1,
                     float dt_ms) {
  float t = t_normalized;
  float h00 = 2*t*t*t - 3*t*t + 1;
  float h10 = t*t*t - 2*t*t + t;
  float h01 = -2*t*t*t + 3*t*t;
  float h11 = t*t*t - t*t;
  
  float dt_s = dt_ms / 1000.0f;
  return h00 * p0 + h10 * dt_s * v0 
       + h01 * p1 + h11 * dt_s * v1;
}
```

**比线性 lerp 优越处**：
- 拐弯、急停、冲刺起步视觉更自然
- 利用 velocity 信息，不只是位置

**代价**：
- 新快照到达时切线可能不连续（`v0` in [t0,t1] 与下一段 `v0'` in [t1, t2] 不同）
- 解决方法：用 Catmull-Rom 变体——用前一个 `p0` 外推为"虚拟前点"：
  ```
  v0_cat = (p1 - p_prev) / (2 * dt_prev)
  ```

### 6.4 外推（Extrapolation）

下一个快照迟到时：

```cpp
float3 Extrapolate(const EntitySnapshot& latest, 
                   float extrap_ms) {
  if (extrap_ms < 100) {
    // 线性外推
    return latest.position + latest.velocity * (extrap_ms / 1000.0f);
  } else if (extrap_ms < 300) {
    // 减速外推（速度指数衰减）
    float decay = exp(-(extrap_ms - 100) / 150.0f);
    float effective_ms = 100.0f + 150.0f * (1.0f - decay);
    return latest.position + latest.velocity * (effective_ms / 1000.0f);
  } else {
    // 冻结
    return last_extrapolated_position_;
  }
}
```

**300ms 外推仍无新快照**：通常意味着该实体离开 AoI 或严重丢包——停止外推，等新订阅建立。

### 6.5 位置量化

**放弃 BigWorld 的浮点指数-尾数格式**（SIMD 不友好、跨平台不稳），用**固定点整数 delta**：

```cpp
struct PackedPos {
  // 相对 reference point 的 delta，单位 1cm，范围 ±327m
  int16_t dx, dy, dz;
};
```

- 近距离（<100m）实体全部用 delta：6 字节/位置
- Reference point 周期性刷新（每 2 秒或明显偏移时）
- 远距离（>100m）用全量位置：12 字节

### 6.6 Jitter Buffer 自适应

```cpp
class JitterEstimator {
  float recent_rtt_samples_[32];
  
  float Stddev() { /* ... */ }
  
  void Sample(float rtt_ms) {
    recent_rtt_samples_[cursor_++ & 31] = rtt_ms;
  }
};

// 每秒更新一次 interp_delay
void UpdateInterpDelay() {
  float jitter = jitter_estimator_.Stddev();
  float ideal = clamp(100.0f, 2.0f * jitter + 50.0f, 250.0f);
  
  // 平滑变化，避免突变
  interp_delay_ms_ = lerp(interp_delay_ms_, ideal, 0.1f);
}
```

---

## 7. 服务端仲裁与历史缓冲

### 7.1 Movement Simulator 服务端侧

服务端每 tick：
```cpp
for (auto& player : cell.players) {
  // 处理输入帧队列
  input_processor_[player].Tick(ctx);
  
  // 处理 active command（技能位移，§8）
  if (player.movement.active_command) {
    ApplyCommandCurve(player, ctx);
  }
  
  // 记录位置历史（lag comp 用）
  player.position_history.Push(player.movement.position, ctx.CurrentTick);
}
```

### 7.2 位置历史缓冲

```cpp
struct PositionHistory {
  struct Sample {
    uint32_t tick;
    float3 position;
    uint16_t yaw;
  };
  
  Sample ring_[30];  // 30 samples @ 20/30Hz = 1s 历史
  uint32_t head_;
  
  void Push(float3 pos, uint32_t tick);
  
  // 查询：给定时间，返回位置（插值）
  float3 At(uint32_t tick);
};
```

**容量 30 样本覆盖 1s 历史**，足够支撑 200ms 延迟补偿 + 余量。

### 7.3 服务端反作弊校验

参见 §10。简单来说，每次 `ApplyInput` 后：
- 检查速度是否超过 `MovementConfig.max_speed × jitter_allowance`
- 检查位置是否合理（不穿墙、在地形之上）
- 违规 → 触发 `PhysicsCorrection`，服务端权威位置立即广播给作弊客户端

---

## 8. 技能驱动位移（MovementCommand）

### 8.1 什么是 MovementCommand

冲锋、闪现、位移技、击飞、被拉近——这类**不由普通输入产生**的位移。

特点：
- 服务端权威下发，客户端不能自主发起
- 走预定义曲线（非物理模拟）
- 期间输入响应按命令配置处理（多数 suppress）
- **端同曲线**保证两端位置一致

### 8.2 数据模型

```cpp
struct MovementCommand {
  uint32_t command_id;      // 唯一 ID，便于追溯
  uint16_t skill_id;        // 产生此命令的技能
  CommandType type;         // Dash / Launch / Pull / Knockback / Teleport
  
  float3 start_pos;         // 命令发起时的权威位置
  float3 target_pos;        // 目标位置（或速度）
  uint16_t duration_ms;
  uint8_t curve_id;         // 指向曲线库
  
  uint8_t input_policy;     // 0=suppress, 1=allow_turn, 2=allow_full
  uint8_t on_collision;     // 0=stop, 1=continue, 2=end_skill
};

enum class CommandType {
  Dash,           // 受控直线/曲线位移
  Launch,         // 自身抛物线
  LaunchOther,    // 对目标施加抛物线（击飞）
  Pull,           // 拉近自身或目标
  Knockback,      // 击退
  Teleport,       // 瞬移（无过渡）
  FollowEntity,   // 跟随目标（飞行物引导）
};
```

### 8.3 曲线库

曲线在构建期从 Unity AnimationClip 或手写数据提取：

```cpp
struct MovementCurve {
  uint16_t id;
  uint16_t sample_count;
  float samples[64];  // 归一化时间 [0,1] 的位移比例
};
```

**Dash 示例**（冲锋技能）：
```
// 加速 → 匀速 → 减速的曲线
curve[t] = smoothstep(t) × distance
```

**Launch 示例**（击飞）：
```
// 抛物线：水平匀速 + 垂直重力
pos(t) = start + vx*t, start_y + vy*t - 0.5*g*t²
```

### 8.4 执行流程

```
Skill timeline 触发 Dash action
  ↓
服务端: 生成 MovementCommand { command_id, start_pos=当前位置, ... }
  ↓
服务端: 附加到 entity.movement.active_command
  ↓
服务端: 广播 MovementCommandStart 事件给相关客户端（可靠通道）
  ↓
每 tick:
  ├── 服务端: ApplyCommandCurve 推进 entity 位置
  ├── 客户端（主控）: 也本地推进（预测）
  └── 客户端（远端看客）: 收到 MovementCommandStart 后本地推进
  ↓
Duration 到期或触发结束条件:
  ├── 服务端: 清除 active_command
  └── 客户端: 平滑过渡回自由移动
```

### 8.5 `ApplyCommandCurve`

```cpp
MovementState ApplyCommandCurve(
  const MovementState& prev,
  uint16_t dt_ms,
  const MovementCurveLib& lib) {
  
  auto& cmd = prev.active_command;
  uint32_t elapsed = prev.command_elapsed_ms + dt_ms;
  
  if (elapsed >= cmd.duration_ms) {
    MovementState next = prev;
    next.position = cmd.target_pos;  // 确保精确落点
    next.active_command = CommandOverride::None;
    next.command_elapsed_ms = 0;
    return next;
  }
  
  float t_normalized = (float)elapsed / cmd.duration_ms;
  const MovementCurve& curve = lib.Get(cmd.curve_id);
  float progress = SampleCurve(curve, t_normalized);
  
  MovementState next = prev;
  next.position = Lerp(cmd.start_pos, cmd.target_pos, progress);
  next.command_elapsed_ms = elapsed;
  return next;
}
```

### 8.6 端同保障

**关键纪律**：
- 服务端和客户端都用**同一份 `ApplyCommandCurve`** 函数（native plugin 共享）
- 同一份曲线数据（端共享资源）
- 同一 `start_pos`（来自服务端广播，客户端不自行推断）
- 同一 `duration_ms` / `curve_id`（命令数据一致）

因此命令开始后**无需持续同步位置**——两端独立推导即可自然对齐。

### 8.7 冲突处理

command 执行期间若收到新命令（如被击退时正在冲锋）：
- 新命令 `priority > current.priority`：打断 current，执行 new
- 否则：忽略 new
- 优先级：Teleport > Knockback > Launch > Pull > Dash

优先级表写在 `MovementConfig`，策划可调。

---

## 9. 远端预测（Remote Prediction for Commands）

### 9.1 问题

远端玩家释放冲锋：
1. 服务端广播 `MovementCommandStart` 给附近客户端
2. 事件到达有网络延迟（~50ms）
3. 远端客户端看到的"冲锋起点"是 50ms 前的位置
4. 若按当前位置播放，会"先瞬移再冲锋"

### 9.2 解决

`MovementCommand` 带 `server_tick` 字段，客户端按**渲染时间延迟**处理：

```
远端实体渲染时间 = now - interp_delay (约 100ms)
收到的命令 server_tick 对应命令发起时刻
命令应在渲染时间到达对应 tick 时开始
```

```cpp
void OnRemoteCommandStart(const MovementCommandStart& cmd) {
  float start_render_time = cmd.server_tick * tick_ms;
  float current_render_time = now_ms - interp_delay_ms;
  
  if (current_render_time >= start_render_time) {
    // 已到开始时间，立即启动命令（可能丢失前几 ms 的表现）
    StartLocalCommand(cmd, elapsed_ms = current_render_time - start_render_time);
  } else {
    // 还未到，延迟启动
    ScheduleCommand(cmd, delay_ms = start_render_time - current_render_time);
  }
}
```

### 9.3 主控 vs 远端的差异

**主控玩家**：
- 客户端预测命令（立即启动）
- 服务端 ack 纠正 start_pos 微小偏差（一般 < 0.3m，Tier 1 无感）

**远端实体**：
- 等待 `MovementCommandStart` 事件
- 按渲染时间表延迟启动
- 命令期间禁用插值（用曲线计算位置）
- 命令结束恢复插值

---

## 10. 反作弊

### 10.1 基础校验

每次 `ApplyInput` 后检查：

```cpp
bool ValidateMove(const MovementState& prev, const MovementState& next, 
                  uint16_t dt_ms, const MovementConfig& cfg) {
  float dist = Distance(prev.position, next.position);
  float max_dist = cfg.max_speed * (dt_ms / 1000.0f) * 1.1f;  // 10% 容差
  if (dist > max_dist) return false;
  
  float max_v_dist = cfg.max_fall_speed * (dt_ms / 1000.0f) * 1.1f;
  if (std::abs(next.position.y - prev.position.y) > max_v_dist) return false;
  
  if (terrain.collides_with_wall(prev.position, next.position)) return false;
  
  return true;
}
```

### 10.2 Jitter Debt Account

简单的"每步严格校验"会误伤抖动网络。引入**抖动欠账**机制（BigWorld 经典方案）：

```cpp
class JitterDebtAccount {
  float debt_ms_ = 0;                              // 累积的"超速时间"
  const float max_debt_ms_ = 500;                  // 500ms 上限
  const float repayment_rate_ = 0.5f;              // 每秒还 0.5
  
public:
  bool Allow(float overspeed_ratio, uint16_t dt_ms) {
    // overspeed_ratio = 1.0 表示正好贴上限，1.5 表示超 50%
    if (overspeed_ratio <= 1.0f) {
      // 还债
      debt_ms_ = std::max(0.0f, debt_ms_ - dt_ms * repayment_rate_);
      return true;
    }
    
    float debt_incurred = (overspeed_ratio - 1.0f) * dt_ms;
    if (debt_ms_ + debt_incurred > max_debt_ms_) {
      return false;  // 拒绝
    }
    debt_ms_ += debt_incurred;
    return true;
  }
};
```

**效果**：偶尔 1-2 次超速 50%（网络抖动）无感通过；持续超速则累积 debt 到阈值后拒绝。

### 10.3 异常检测（行为分析）

服务端统计每个玩家：
- 位置校验失败次数 / 小时
- 平均每 tick 速度 / 最大速度比
- 短时间内的位置"跳变"次数
- 输入响应时间分布（挂程序通常有异常分布）

阈值超标上报反作弊系统，触发人工审核或自动 ban wave。

### 10.4 Physics Correction

校验失败时：
- 服务端位置回滚到上次合法状态
- 发送 `PhysicsCorrection { authoritative_pos, tick }` 给客户端
- 客户端视为 Tier 4 硬 snap（本章 §5.4）
- 连续触发 5 次以内加入"可疑"观察列表；10 次以内自动临时封号

---

## 11. Unity 客户端集成

### 11.1 `AtlasMovementController : MonoBehaviour`

```csharp
public sealed class AtlasMovementController : MonoBehaviour {
  // Native plugin handle
  IntPtr _nativeHandle;
  
  void Awake() {
    _nativeHandle = NativePlugin.CreateMovementPredictor(entityId_);
  }
  
  void Update() {
    // Input 采集（Unity Input System）
    InputFrame frame = InputCollector.Sample();
    
    // 传给 native plugin 做预测
    NativePlugin.PushInput(_nativeHandle, ref frame);
    
    // 读取渲染位置并应用
    NativePlugin.GetRenderTransform(_nativeHandle, out var pos, out var yaw);
    transform.position = pos;
    transform.rotation = Quaternion.Euler(0, yaw, 0);
  }
  
  void OnStateAck(StateAck ack) {
    NativePlugin.ApplyStateAck(_nativeHandle, ref ack);
  }
  
  void OnCommandStart(MovementCommandStart cmd) {
    NativePlugin.ApplyCommand(_nativeHandle, ref cmd);
  }
  
  void OnDestroy() {
    NativePlugin.DestroyMovementPredictor(_nativeHandle);
  }
}
```

### 11.2 关键纪律

- **禁用** `Rigidbody` / `CharacterController`
- **禁用** Unity `Physics.*`（用自研碰撞）
- **禁用** Root Motion（动画用占位，位移由曲线）
- **禁止**在 `Update` 里调用 `NativePlugin.*` 超过 2 次（每次 P/Invoke ~20ns，高频调用浪费）
- 渲染在 `LateUpdate` 统一写 transform，避免中间帧抖动

### 11.3 远端实体 `AtlasRemoteEntity : MonoBehaviour`

```csharp
public sealed class AtlasRemoteEntity : MonoBehaviour {
  IntPtr _interpHandle;
  
  public void OnSnapshot(EntitySnapshot snap) {
    NativePlugin.PushRemoteSnapshot(_interpHandle, ref snap);
  }
  
  void LateUpdate() {
    NativePlugin.GetInterpolatedTransform(_interpHandle, 
                                           GetRenderTimeMs(),
                                           out var pos, out var yaw);
    transform.position = pos;
    transform.rotation = Quaternion.Euler(0, yaw, 0);
  }
}
```

### 11.4 线程模型

- **主线程**：`Update` / `LateUpdate`，调用 native plugin 读状态 → 写 transform
- **网络线程**：接收 UDP 包，反序列化为事件，push 进 `ConcurrentQueue`
- **主线程 `Update` 开头**：从 queue 取出事件，同步 apply 到 native plugin

**禁止**：网络线程直接改 transform 或 GameObject 状态（Unity 要求主线程）。

### 11.5 GC 优化

Hot path 禁止 `new`：
- InputFrame 用 `struct`，栈分配
- 事件批处理用 `NativeArray<T>` 池化
- 每帧调用数 <10，无 GC 压力

---

## 12. Native Plugin 接口（C API）

### 12.1 导出函数（C 形式便于 P/Invoke）

```cpp
extern "C" {

// Predictor（主控）
void* atlas_predictor_create(uint64_t entity_id, const MovementConfig* cfg);
void  atlas_predictor_destroy(void* handle);
void  atlas_predictor_push_input(void* handle, const InputFrame* input);
void  atlas_predictor_apply_ack(void* handle, const StateAck* ack);
void  atlas_predictor_apply_command(void* handle, const MovementCommandStart* cmd);
void  atlas_predictor_get_render_transform(void* handle, 
                                            float3* pos_out, uint16_t* yaw_out);

// Remote Interpolator
void* atlas_interp_create(uint64_t entity_id);
void  atlas_interp_destroy(void* handle);
void  atlas_interp_push_snapshot(void* handle, const EntitySnapshot* snap);
void  atlas_interp_apply_command(void* handle, const MovementCommandStart* cmd);
void  atlas_interp_get_transform(void* handle, uint32_t render_time_ms,
                                  float3* pos_out, uint16_t* yaw_out);

}
```

### 12.2 C# P/Invoke 封装

```csharp
internal static class NativePlugin {
  const string DLL = "libatlas_movement";
  
  [DllImport(DLL, EntryPoint = "atlas_predictor_create")]
  public static extern IntPtr CreateMovementPredictor(ulong entityId);
  
  [DllImport(DLL, EntryPoint = "atlas_predictor_push_input")]
  public static extern void PushInput(IntPtr handle, ref InputFrame input);
  
  // ... 其余方法
}
```

### 12.3 构建产物

根据 `project_atlas_scope.md`（桌面限定）：
- Windows: `libatlas_movement.dll`
- Linux Server: `libatlas_movement.so`
- macOS (仅 Unity Editor): `libatlas_movement.dylib`

**不需要** iOS / Android 构建。

---

## 13. 性能预算

### 13.1 服务端

| 操作 | 预算（单实体单 tick） |
|---|---|
| `ApplyInput` | ≤ 10 μs |
| `ValidateMove` + jitter debt | ≤ 2 μs |
| 历史缓冲写入 | ≤ 1 μs |
| 广播准备 | ≤ 5 μs |
| **总计** | **≤ 20 μs** |

单 cell 400 实体 × 20 μs = 8 ms（占 33ms 副本 tick 的 24%），可接受。

### 13.2 客户端

| 操作 | 预算（每帧） |
|---|---|
| Input 采集 + push | ≤ 50 μs |
| `apply_input` 本地预测 | ≤ 15 μs |
| `GetRenderTransform` | ≤ 5 μs |
| 远端实体插值（50 个） | ≤ 200 μs |
| **总计** | **≤ 300 μs** = 0.3 ms / 16.67 ms 帧 = 1.8% |

### 13.3 带宽预算

| 方向 | 内容 | 带宽 |
|---|---|---|
| 上行 | InputFrame 冗余 | 3.24 KB/s |
| 下行 | StateAck | ~0.5 KB/s |
| 下行 | 远端实体快照（50 实体 × 20Hz） | ~8-12 KB/s |
| 下行 | MovementCommand 事件 | ~0.5 KB/s |

总下行 ~12 KB/s，5000 并发 × 12KB/s = 60 MB/s 服务端总下行，单服务器可承载。

---

## 14. FAQ 与反模式

### Q1: 为什么不用 Unity `CharacterController`？

- **非确定**：Unity PhysX 内部使用不同浮点路径，端同无法保证
- **不可共享**：服务端没有 Unity 运行时
- **黑盒**：内部逻辑不可控，难以适配 MMO 场景（如跳过某些碰撞）

Atlas 自研胶囊 + 高度图碰撞，代码约 500 行（见 `src/lib/movement/capsule_collide.cc`），完全可控。

### Q2: 为什么把预测逻辑放 C++ 而不是 C#？

C# 路径的问题：
- 跨 Mono / CoreCLR / IL2CPP 浮点不一致风险
- Unity Burst 破坏确定性（禁用 Burst 又丢性能）
- .NET 版本升级可能改变 `Math.*` 行为

C++ 路径：
- 跨编译器可控（通过编译 flag）
- 无 JIT 差异
- 一份代码链接服务端（Linux）和客户端 plugin（Windows）

### Q3: 主控玩家的 200ms 延迟能感知吗？

**不能**，因为：
- 主控走预测路径，本地 0 延迟响应
- 服务端 ack 偏差 < 0.3m 视为 Tier 1 无感
- 玩家感知的"延迟"是"按键 → 看到响应"，主控 < 20ms

200ms 延迟只影响**命中判定**（通过 lag compensation 处理）和**远端实体表现**（通过插值延迟吸收）。

### Q4: 冲锋技能过程中玩家还能转向吗？

取决于 `MovementCommand.input_policy`：
- 0 = suppress：完全不响应输入
- 1 = allow_turn：可转向但不改变冲锋方向
- 2 = allow_full：转向 + 减速/加速

策划按技能设计选择。典型快速冲锋 suppress，长距离位移 allow_turn。

### Q5: 客户端走 C++ predictor，Unity Editor 调试怎么办？

- `libatlas_movement.dylib` 同时构建 macOS 版，Editor 加载
- VS Code / VS 调试器 attach 到 Unity 进程，断点打到 C++ 源码
- 或使用 Unity Editor 的 `ATLAS_CSHARP_FALLBACK` 编译宏：纯 C# 实现（仅编辑器测试用，明确知道数值可能略有差异）

### Q6: 丢失 UDP 包导致位置"卡顿"？

不会——有多层防护：
- 输入帧**3 帧冗余**：连续丢 2 包无损
- 远端实体**外推**：<100ms 迟到无视觉损失
- 快照**delta + 参考点刷新**：丢包后恢复即获最新

### Q7: `MovementCommand` 能在中途被取消吗？

可以，通过：
- 新命令（更高优先级）抢占（§8.7）
- 技能显式调用 `CancelCommand` action
- 玩家死亡时自动清除

取消时端同发 `MovementCommandCancel` 事件，两端立即停止推进。

### Q8: 远端看客看冲锋玩家的位置会和冲锋者自己看到的一致吗？

**会略有差异**（远端有 interp_delay ~100ms）但**曲线一致**：
- 冲锋者 t=0 时在 P0
- 远端在 t=interp_delay 时看到冲锋者在 P0
- 都沿同曲线移动，"形状"一致

这种延迟差属于**物理不可消除**，只能通过 lag compensation 在命中判定时弥补（见 `LAG_COMPENSATION.md`）。

### Q9: 如果 `ApplyInput` 端同漂移 1 ULP，累计 100 帧会变成几厘米偏差吗？

**理论上会**，所以必须：
- 严格遵守 `DETERMINISM_CONTRACT.md`（浮点规约）
- 关键位置用 `Atlas.CombatCore.Math` 软件实现
- 每次 ack 纠正一次，不累计

实测：干净环境下 1000 帧漂移 < 1e-4 m（远小于 0.3m 无感阈值）。

### Q10: Unity 的 Animator 如何与本系统协作？

- MovementController **不知道 Animator 存在**——它只管位置
- Animator 由 Skill 系统的 `PlayAnim` action 驱动
- 若动画需要"看起来在移动"，由 Animator 的 velocity 参数驱动（从 `transform.position` 变化率估算）
- 脚踏地面 IK 走 Unity 标准方案（纯视觉，不影响仿真）

### 反模式清单

- ❌ 直接改 `transform.position`（绕过 predictor）
- ❌ 用 `Time.deltaTime` 作为仿真 dt（用 server tick）
- ❌ 客户端计算伤害/命中（这是服务端权威的）
- ❌ 服务端发"整个位置流"给客户端（应发输入 ack）
- ❌ 远端实体用 `Vector3.Lerp` 而不是 Hermite（视觉差）
- ❌ `apply_input` 里查询 Unity `Physics.Raycast`（端同破坏）
- ❌ 命令完成后再广播 `MovementCommandStart`（来不及预测）
- ❌ 忽略 jitter debt，每 tick 严格校验（误伤合法玩家）

---

## 15. 里程碑

| 阶段 | 交付 |
|---|---|
| P0 末 | Native plugin 骨架；InputFrame 协议定义；端同 diff 测试通过 |
| P1 中 | 主控预测 + 硬 snap 纠正；2 人联机移动同步 |
| P1 末 | 三级软纠正 + visual_offset；4 人联机 150ms 模拟延迟下手感可接受 |
| P2 中 | 远端 Hermite 插值 + 自适应 jitter buffer；MovementCommand 集成 |
| P2 末 | 技能 Dash/Launch 端同；反作弊 jitter debt；压力测试 50 实体 |
| P3 | 配合手感打磨：移动动画、碰撞反馈 |
| P4+ | 400 实体热点压测；Cell 边界移动处理 |

---

## 16. 文档维护

- **Owner**：Tech Lead
- **关联文档**：
  - `OVERVIEW.md`（§7 难点一引用本文）
  - `DETERMINISM_CONTRACT.md`（所有仿真遵循）
  - `SKILL_SYSTEM.md`（Dash/Launch action 对接 §8）
  - `ENTITY_SNAPSHOT.md`（快照压缩细节）
  - `LAG_COMPENSATION.md`（命中判定时间回溯）
  - `UNITY_INTEGRATION.md`（Unity 侧实现细节）
  - `ANTI_CHEAT_STRATEGY.md`（§10 反作弊纳入总体策略）

---

**文档结束。**

**核心纪律重申**：
1. **输入帧而非位置**：客户端永不是位置真源
2. **三类问题分开解**：主控预测 / 远端插值 / 技能位移，策略不同
3. **端同曲线**：技能位移用预定义曲线，自然对齐无需同步流
4. **视觉偏移 ≠ 位置回退**：仿真位置即服务端权威，渲染通过 visual_offset 吸收
5. **Jitter debt 兼顾安全与体验**：严格校验太粗暴，完全信任太危险
