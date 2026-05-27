# Phase 14: 服务端权威移动与本地预测

**Status:** 🟨 14.1 主链路已可用——输入帧协议、CellApp 权威 step、owner
预测和解、MovementCommand fanout、Static physics query、collision asset v1、
position history 都已落地。当前重心是协议边界硬化、验证矩阵和文档对齐。
已交付能力清单、收紧的 wire contract、最小回归命令集见
[`phase14_status.md`](phase14_status.md)。

**前置依赖:** Phase 10 (CellApp / Witness / volatile 位置流)、Phase 11
(Real/Ghost / Offload)、Phase 12 (atlas_net_client / Atlas.Client)、
[`docs/physics/physics_architecture.md`](../physics/physics_architecture.md)。

## 工作上下文

继续工作时不要重置当前脏工作树；大量未提交改动都属于 Phase 14 推进中的
上下文。UE 源码根目录为 `E:\UE\UnrealEngine`。已交付能力详单、wire contract
快照、最小回归命令集统一放在 [`phase14_status.md`](phase14_status.md)；
本文件只承载目标 / 验收 / 里程碑 / 决策日志 / 红线，不重复 status。

## 里程碑驱动的下一步

独立开发节奏，串行推进；每个里程碑独立可合入、可跑、可回滚，
工作量上限 3 天。完成一个再开下一个，避免提前把 6 个月的决策一次拍死。

### 14.1 收尾里程碑

**M0.1**：`world_stress --script-verify` / `--movement-verify` 在最新协议 hardening
下回归，确认 watcher gate 没误伤。
- 完成判据：50 / 100 / 400 entity smoke 全过；watcher 汇总非零。

### 14.2 Jolt query backend（里程碑）

**M1b**：Jolt 实际接入与 hello-world raycast。
- 在 `cmake/Dependencies.cmake` 加 Jolt FetchContent（带 SHA256）。
- `JoltPhysicsQuery` 初始化真正的 PhysicsSystem，注册 box body，跑通 1 个
  raycast 返回正确 hit。
- ATLAS_ENABLE_JOLT 默认翻 ON。
- Win 本地 + CI Linux 都过。

**M2**：单 box collision asset 跑通 CharacterMotor。
- `JoltCharacterQuery` 适配 `PhysicsCharacterQuery` 接口。
- 现有 Static-based CellApp 集成测试在 Jolt backend 下也过。
- `Space::SetPhysicsBackend` 支持 `kStatic` / `kJolt` 切换。

**M3**：Backend Parity quick gate 上 CI。
- 实装 [`backend_parity_testing.md`](../physics/backend_parity_testing.md) PR #1–#3。
- Flat ↔ Static ↔ Jolt 在 5 个必备场景上 quick 档通过。
- 每个 PR 自动跑；红就阻断。

**M4**：Static mesh 最小切片。
- Collision asset schema v2：mesh triangle list，JSON metadata + `.bin` side-car。
- Jolt 加载 mesh shape；手写 `.collision.json` + `.bin` 跑通。
- Parity 场景集追加 `mesh_walk_long_path`。

**M5**：`atlas_tool cook_collision` 最小可用。
- 把 v2 asset 编成 `joltcache`（含 `source_hash` + `jolt_version`）。
- Runtime mismatch → 拒绝启动，不 silent fallback。
- 还不接 Unity；先支持手写 asset cook。

### 14.3 Unity 导出 MVP（里程碑）

**M6**：`ServerColliderAuthoring` MonoBehaviour（primitive only）。
- 默认 `exportToServer = false`，显式标记才导出。
- Scene view gizmo 显示即将导出的 collider。

**M7**：命令行 exporter 端到端。
- Unity batch mode 把 primitive collider 导出成 v2 asset。
- 跑通：Unity → exporter → `cook_collision` → CellApp 加载。
- 还不做 mesh / heightfield / volume。

### 之后

按遇到再做：heightfield、chunking、moving platform、ladder、跨 cell 物理、
完整 data-driven skill timeline、pathfinding。**不提前规划**。

## 决策日志

为何这么选，方便半年后自己 / AI 会话回看：

- **Jolt 引入**：用 `cmake/Dependencies.cmake` 的 FetchContent 模式，与
  gtest / pugixml / tracy 等现有第三方一致；CI 已缓存 `_deps`。本来想 vendor
  到 `third_party/`，发现项目根本没这个目录，所有依赖走 FetchContent。
- **`ATLAS_ENABLE_JOLT` 默认值**：M1a 默认 OFF，骨架先合入不引入实际依赖；
  M1b 验证 fetch 链路后翻 ON。OFF 路径长期保留给 atlas_tool / 部分 parity
  测试。
- **Collision asset v2 mesh 存储**：JSON metadata + `.bin` side-car，不内嵌
  base64。大地图 mesh 几百 MB，JSON 内嵌读写会爆内存。
- **Layer 起步集**：`StaticWorld` / `Character` / `Projectile` 三个，遇到再加。
  独立开发可随时改名 + 重 cook，不需要团队对齐。
- **Chunking 起步策略**：不做。单 Space 单 PhysicsScene 跑通；地图大到一个
  scene 装不下时再加 chunk。
- **Cache mismatch 策略**：拒绝启动，不 silent fallback。否则升级 Jolt 后
  会跑在过期 cache 上而不自知。
- **Unity exporter 走 batch mode**：不解析 YAML。prefab variant / nested
  prefab / 多场景叠加自己实现成本远高于 Unity 启动一次。

## 红线

独立开发也守的硬约束：

- Jolt 类型不出 `physics_jolt` 边界。
- Atlas collision asset 不等于 Jolt cooked binary。
- Entity 不等于 PhysicsBody。
- 角色移动不交给 Jolt CharacterController。
- 所有 query 必须带 layer / mask / result limit。
- Parity test 必须过；任何 backend 改动都要先过 parity gate。

## 目标

建立人形角色移动基础设施，覆盖玩家、NPC 和后续类人怪物：

- 服务端是位置、碰撞、grounded 状态和技能位移的唯一权威。
- 玩家客户端有 0 延迟本地响应，通过输入历史、replay 和 visual offset
  吸收服务端纠正。
- NPC 与玩家共用 CharacterMotor，只是输入来源从客户端输入变成 AI intent。
- 远端实体继续走服务端位置快照；第一阶段复用现有 `AvatarFilter`，Hermite
  插值和高级 jitter buffer 后置。
- 移动输入与 `.def` RPC 分离，避免高频输入占用可靠 RPC 通道。

## 当前基线

已有可复用能力：

- BaseApp 已能把客户端绑定到 source entity，并校验 own-client RPC。
- CellApp 已有 `CellEntity.Position / Direction / OnGround`、RangeList、
  Witness、`0xF001` volatile 位置广播、Real/Ghost haunt 和 offload。
- `atlas_net_client` 已复用服务端 RUDP，实现 reliable / unreliable 发送。
- `Atlas.Client` 已有实体管理和 owner/peer 区分，peer 位置走 `AvatarFilter`。

已替换 / 待替换的 MVP 行为：

- Unity `PlayerInputController` 和 UE `UAtlasPlayerInputController` 已发送
  `AtlasMovementInputFrame`，并按 `MovementStateAck` replay 未确认输入。
- MVP `NpcAiComponent` 已写入 movement intent，不再直接积分位置。
- MVP `Avatar.ReportPos` 已移除；`StressAvatar.ReportPos` 只保留给压测路径。

## 非目标

- 第一阶段不接 Jolt，不做 Unity collision export / cook / chunk streaming。
- 第一阶段不实现完整技能位移、root-motion 曲线、lag compensation。
- 不把移动输入塞进 `ClientCellRpc`，也不通过 `.def` 生成高频移动 RPC。
- 不让 `client_dt_ms` 驱动服务端时间；它只能用于诊断和异常检测。
- 不要求 Unity PhysX / UE Physics 与服务端物理端同。

## 分层设计

### `atlas_movement_sim`

新增 `src/lib/movement_sim/` 静态库：

```cpp
namespace atlas::movement {

struct InputFrame {
  uint32_t seq{0};
  uint32_t input_tick{0};
  int8_t move_x{0};
  int8_t move_z{0};
  uint16_t view_yaw{0};
  int8_t view_pitch{0};
  uint16_t buttons{0};
  uint16_t client_dt_ms{0};
};

struct MovementState {
  math::Vector3 position;
  math::Vector3 velocity;
  math::Vector3 direction;
  uint32_t flags{0};
  uint32_t last_processed_input_seq{0};
};

class CharacterQuery {
 public:
  virtual auto GroundProbe(const math::Vector3& pos) const -> GroundHit = 0;
  virtual auto SweepCapsule(const CapsuleCast& cast) const -> SweepHit = 0;
  virtual auto OverlapCapsule(const Capsule& capsule) const -> bool = 0;
  virtual auto DepenetrateCapsule(const Capsule& capsule) const
      -> DepenetrationHit = 0;
};

auto Step(const MovementState& previous, const InputFrame& input,
          const MovementConfig& config, const CharacterQuery& query,
          uint32_t server_tick) -> MovementStepResult;

}
```

当前提供 `FlatGroundQuery`、`PhysicsCharacterQuery` 和 Static backend，
验证输入、预测、权威 tick、grounded、速度、基础阻挡、depenetration、
slope、step 和 snap。Jolt backend 和地图碰撞导出继续后置接入，不阻塞
协议和预测链路落地。

### 服务端状态归属

移动状态在 CellApp C++ 侧管理，建议放在 `MovementStateStore`，按
`EntityID` 索引。`CellEntity` 继续保存复制和 AoI 所需的 pose；每个
server tick 由 MovementSystem 写回 `CellEntity.SetPositionAndDirection`
和 `SetOnGround`。

C# 脚本不直接设置玩家位置。脚本只产生高层 intent：

- 玩家：客户端输入帧经 BaseApp stamp 后进入 CellApp 输入队列。
- NPC：AI 组件写入 desired direction / target / stop intent。
- 技能：C# 通过 `CellServerEntity.SetMovementCommand` 写入 dash / launch /
  knockback command。

### 客户端预测

`movement_sim` 同时链接到服务端和客户端。Unity / UE 第一阶段复用
`atlas_net_client` 导出的 predictor C API，避免新增第二个 native DLL。

owner 每帧流程：

1. 采集输入并量化成 `InputFrame`。
2. 本地 `Predictor::PushInput` 立即推进当前预测状态。
3. 写渲染 transform：`predicted_state.position + visual_offset`。
4. 通过 `AtlasNetSendMovementInput` 发送当前帧和最近两帧冗余。
5. 收到 `MovementStateAck` 后按 `acked_input_seq` replay 未确认输入。

Ack 在 owner 侧也是 latest-wins：新 input seq 覆盖旧 seq；同一 input seq
只接受更大的 `server_tick`，重复或更旧 tick 不触发 replay / report。

如果 owner 断线后重连到同一个 entity，客户端下一帧输入 seq 从最新 ack
的 `acked_input_seq + 1` 继续；BaseApp relay 同步播种该客户端的输入 seq
观察值，避免重连前的旧序列状态导致新输入被丢弃。

Unity / UE MVP 使用的 `atlas_net_client` 暴露
`AtlasNetSetTransportImpairment`，可在登录前或运行中给当前及后续 RUDP
channel 注入每向延迟和 datagram loss。

纠正规则：

| 误差 | 行为 |
|---|---|
| `< 0.3m` | 只修内部状态，不改 visual offset |
| `0.3m - 1.5m` | replay 后保留旧渲染位置，visual offset 慢衰减 |
| `1.5m - 5m` | 快速衰减 |
| `>= 5m` | snap，通常来自传送、死亡、强制纠正 |

## 协议

移动协议是引擎原生协议，不进入 `.def` / generator。

### Client -> BaseApp

新增 `ClientMovementInput`，unreliable immediate。payload 包含：

```text
u32 target_entity_id
u8  frame_count       // 1..3
InputFrame[frame_count]
```

BaseApp 只做边界校验：

- channel 必须已认证并绑定 source entity。
- target 必须非 0 且等于 source，除非后续显式支持受控宠物 / vehicle。
- frame_count、payload size、seq gap、输入频率必须在上限内。
- 输入帧 `client_dt_ms` 必须在 1-250ms。
- 不解析成位置，不运行移动仿真。

通过后 BaseApp stamp `source_entity_id`，按 target 当前 CellAddr 转发。

### BaseApp -> CellApp

新增 `ClientMovementInputForward`，unreliable immediate。payload 包含：

```text
u32 source_entity_id
u32 target_entity_id
u8  frame_count
InputFrame[frame_count]
```

CellApp 校验：

- source 必须等于 target。
- source / target 必须非 0，输入帧 `client_dt_ms` 必须在 1-250ms。
- target 必须是本进程 Real，Ghost 或 unknown 直接 drop。
- 乱序 / 重复 input seq drop。
- 单 tick 消费预算固定，客户端不能靠堆积输入追赶超速。

### CellApp -> owner client

新增 `MovementStateAck`，latest-wins，建议每 3 个 server tick 发送一次：

```text
u32 entity_id
u32 acked_input_seq
u32 server_tick
MovementState authoritative_state
u16 correction_flags
```

Ack 丢失无害，下一次 ack 覆盖旧状态。不要复用 `0xF001` AoI envelope；
新增 client-facing wire id，避免污染 `ClientSession.DispatchAoIEnvelope`。
CellApp -> BaseApp 的内部 ack 额外携带 `cell_epoch`；BaseApp 丢弃低于
当前 `CellEpoch` 的旧 ack，不把迁移前状态转发给 owner。

新增 `MovementCorrectionReport`，client -> BaseApp，unreliable immediate：

```text
u32 target_entity_id
u32 acked_input_seq
u32 server_tick
f32 distance_m
u16 correction_flags
```

BaseApp 只接受 owner 对已 relay ack 的 latest-wins report，并校验
target 非 0、`distance_m` 为有限非负值，且 `distance_m` 与 `correction_flags`
一致。Report 只进入 watcher 和可疑升级，不参与权威位置计算。

## Tick 顺序

CellApp 推荐顺序：

```text
1. 接收网络输入，MovementInputBuffer 去重入队
2. C# OnTick：AI / 技能脚本产生 movement intent
3. MovementSystem：玩家输入、NPC intent、基础 CharacterMotor
4. 写回 CellEntity pose，记录 position history
5. Projectile / Skill / Trigger 查询
6. PublishReplicationFrame，Witness / Ghost pump 发送位置和属性
7. 发送 MovementStateAck 给 owner
```

服务端只按 `ServerApp` fixed tick 推进移动。`client_dt_ms` 仅用于检测异常
客户端帧率、输入堆积和重放攻击。

## 物理接入阶段

| 阶段 | 查询后端 | 交付 |
|---|---|---|
| 14.1 | Flat/Test query | 输入帧、权威 Step、预测和解、MVP 替换 `ReportPos` |
| 14.2 | PhysicsQuery + Jolt scene | Static query 与 collision asset validate 已建；Jolt scene |
| 14.3 | Unity collision export / cook | primitive / static mesh / layer / material，CI validate |
| 14.4 | chunk / border query | 大地图 streaming、Cell 边界 ghost region 查询 |

Jolt 只提供查询事实，不使用 Jolt CharacterController。

## Offload 与 Ghost

Offload 消息需要携带：

- `MovementState` 和 `last_processed_input_seq`，14.1 已随 `OffloadEntity`
  迁移并在 reject / timeout 回滚时恢复。
- active MovementCommand，C# 写入 API、基础 store、默认线性曲线执行和
  Stop / Continue / EndSkill 碰撞策略、priority 抢占、Offload / revert
  迁移已落地；MovementCommandStart / End fanout / wire / decode、owner
  predictor 和 remote interpolator 应用已落地；suppress 和 allow_turn 输入策略
  已落地，allow_full 仍为保留值；C# `ClearMovementCommand`
  可由技能取消、死亡等脚本事件清除 active command 并广播 cancelled end；
  command 正常结束、碰撞截停和非法终止也会带 reason；MVP `Avatar.Dash` 已作为
  脚本写入 command 的首个 playable action。完整技能 timeline 仍在 14.3+ 接入
- position history 的最近窗口，14.1 已随 `OffloadEntity` 迁移并在 reject /
  timeout 回滚时恢复，供 lag compensation 使用

迁移期间 BaseApp 可能把输入转发到旧 CellApp。旧 Real 已转 Ghost 时直接
drop；客户端靠下一次 ack 和 volatile 位置纠正。BaseApp 已按 cell epoch
拒绝旧 ack，避免 owner predictor 接收迁移前状态。

Ghost 仍是只读副本。Ghost 位置由 Real 的 volatile seq 更新，不运行
MovementSystem。

## 安全与预算

14.1 已落地的第一批校验：

- 输入帧 seq 单调递增，重复和过旧帧 drop。
- 每包最多 3 帧，每秒输入包数有 token bucket。
- 每 tick 最多消费固定数量输入；缺输入时按 no-input 推进。
- 速度、加速度、垂直速度、坐标 finite 校验。
- 服务端纠正计数进入 watcher，连续大纠正升级为可疑行为。

Watcher 建议：

```text
movement/input_packets_total
movement/input_dropped_total
movement/input_invalid_dropped_total
movement/input_queue_depth
movement/position_history_samples
movement/position_history_samples_recorded_total
movement/ack_sent_total
movement/active_commands
movement/command_started_total
movement/command_ended_total
movement/command_completed_total
movement/command_cancelled_total
movement/command_collision_total
movement/command_invalid_total
movement/ack_stale_dropped_total
movement/correction_tier1_total
movement/correction_tier2_total
movement/correction_snap_total
movement/correction_suspicious_total
movement/correction_report_total
movement/correction_report_dropped_total
movement/step_time_us_p95
```

## 验收

Phase 14.1 完成条件：

- Unity / UE MVP owner 不再走 `ReportPos`，均使用输入帧 + predictor。
- 2 个客户端 150ms RTT / 2% 丢包下，owner 移动无明显回拉。
  `atlas_net_client` 侧用 `AtlasNetSetTransportImpairment(75, 200, seed)`；
  `world_stress` 脚本客户端用
  `--script-clients 2 --client-transport-impairment-ms 75 200`。
- 50/100/400 裸协议 moving entities 用
  `run_world_stress --move-mode input --movement-verify` 直接压
  BaseApp / CellApp movement input 和 ack watcher；400 档配合多 Space 与
  `--spread-radius` 做初始落点播种，避免把单 leaf AoI 饱和误判为 input 问题。
- 服务端权威位置不接受客户端绝对坐标。
- Windows / Linux 服务端和 Unity native predictor 对同一输入序列的结果在
  阈值内一致。
- 远端玩家仍通过现有 AoI volatile 位置流可见。

Phase 14.2 完成条件：

- CharacterMotor 使用 PhysicsQuery 做 capsule sweep、ground probe、
  depenetration、slope limit、step up、snap to ground。
- Jolt 类型不泄露到 gameplay / server / script 边界。
- Test backend 覆盖 box / plane KCC 状态机和 raycast，不依赖 Jolt runtime。

## 测试矩阵

| 层级 | 覆盖 |
|---|---|
| unit | InputFrame codec、Predictor replay、CharacterMotor flat / static query、correction tier |
| integration | BaseApp auth stamp、CellApp Real-only input、ack stale drop、offload stale input drop |
| client | Unity / UE owner predictor、peer AvatarFilter 不回归、disconnect 清状态 |
| stress | 50/100/400 moving entities，输入丢包、乱序、burst、RUDP 延迟 / loss |
| parity | Windows / Linux / Unity native 10k tick diff |

## 文档同步

实现落地时必须同步：

- `samples/mvp/README.md`
- `samples/mvp/readme_cn.md`
- `docs/gameplay/02_sync/MOVEMENT_SYNC.md`
- `docs/ue_client/open_questions.md`
- `docs/physics/physics_architecture.md`

Unity / UE MVP 文档需描述输入帧 + ack replay，并明确 MVP `Avatar` 不再暴露
`ReportPos`，压测使用 `StressAvatar.ReportPos`。
