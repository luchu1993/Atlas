# Phase 14: 服务端权威移动与本地预测

**Status:** ⬜ 规划中。当前 MVP 仍是客户端权威移动：Unity / UE
owner 每帧本地推进位置，并通过 `Avatar.ReportPos` 把绝对位置上报到
CellApp。生产移动链路需要改成客户端只发输入帧，CellApp 运行权威
CharacterMotor，owner 客户端本地预测并按服务端 ack 和解。

**前置依赖:** Phase 10 (CellApp / Witness / volatile 位置流)、Phase 11
(Real/Ghost / Offload)、Phase 12 (atlas_net_client / Atlas.Client)、
[`docs/physics/physics_architecture.md`](../physics/physics_architecture.md)。

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

必须替换的 MVP 行为：

- `PlayerInputController` 直接写 Unity transform 并调用 `Avatar.Cell.ReportPos`。
- `Avatar.ReportPos` 在 CellApp C# 中直接 `Position = pos`。
- README 中的“client-authoritative movement”只适用于 MVP，不适用于生产路线。

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
};

auto Step(const MovementState& previous, const InputFrame& input,
          const MovementConfig& config, const CharacterQuery& query,
          uint32_t server_tick) -> MovementStepResult;

}
```

第一版提供 `FlatGroundQuery` / `TestCharacterQuery`，只验证输入、预测、
权威 tick、grounded、速度和基础阻挡。Jolt backend 和地图碰撞导出在
Phase 14.2 接入，不阻塞协议和预测链路落地。

### 服务端状态归属

移动状态在 CellApp C++ 侧管理，建议放在 `MovementStateStore`，按
`EntityID` 索引。`CellEntity` 继续保存复制和 AoI 所需的 pose；每个
server tick 由 MovementSystem 写回 `CellEntity.SetPositionAndDirection`
和 `SetOnGround`。

C# 脚本不直接设置玩家位置。脚本只产生高层 intent：

- 玩家：客户端输入帧经 BaseApp stamp 后进入 CellApp 输入队列。
- NPC：AI 组件写入 desired direction / target / stop intent。
- 技能：后续通过 MovementCommand 写入 dash / launch / knockback。

### 客户端预测

`movement_sim` 同时链接到服务端和客户端。Unity 第一阶段优先从
`atlas_net_client` 导出 predictor C API，避免新增第二个 native DLL；UE
可直接链接同一静态库。

owner 每帧流程：

1. 采集输入并量化成 `InputFrame`。
2. 本地 `Predictor::PushInput` 立即推进当前预测状态。
3. 写渲染 transform：`predicted_state.position + visual_offset`。
4. 通过 `AtlasNetSendMovementInput` 发送当前帧和最近两帧冗余。
5. 收到 `MovementStateAck` 后按 `acked_input_seq` replay 未确认输入。

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
- target 必须等于 source，除非后续显式支持受控宠物 / vehicle。
- frame_count、payload size、seq gap、输入频率必须在上限内。
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
| 14.2 | Atlas PhysicsQuery + Jolt static scene | capsule sweep / overlap / ground probe、slope、step、snap |
| 14.3 | Unity collision export / cook | primitive / static mesh / layer / material，CI validate |
| 14.4 | chunk / border query | 大地图 streaming、Cell 边界 ghost region 查询 |

Jolt 只提供查询事实，不使用 Jolt CharacterController。

## Offload 与 Ghost

Offload 消息需要携带：

- `MovementState`
- `last_processed_input_seq`
- active MovementCommand（Phase 14.3+）
- position history 的最近窗口，供 lag compensation 使用

迁移期间 BaseApp 可能把输入转发到旧 CellApp。旧 Real 已转 Ghost 时直接
drop；客户端靠下一次 ack 和 volatile 位置纠正。后续可用 cell epoch 拒绝
旧 ack，避免 owner predictor 接收迁移前状态。

Ghost 仍是只读副本。Ghost 位置由 Real 的 volatile seq 更新，不运行
MovementSystem。

## 安全与预算

必须落地的第一批校验：

- 输入帧 seq 单调递增，重复和过旧帧 drop。
- 每包最多 3 帧，每秒输入包数有 token bucket。
- 每 tick 最多消费固定数量输入；缺输入时按 no-input 推进。
- 速度、加速度、垂直速度、坐标 finite 校验。
- 服务端纠正计数进入 watcher，连续大纠正升级为可疑行为。

Watcher 建议：

```text
movement/input_packets_total
movement/input_dropped_total
movement/input_queue_depth
movement/ack_sent_total
movement/correction_tier1_total
movement/correction_tier2_total
movement/correction_tier3_total
movement/correction_snap_total
movement/step_time_us_p95
```

## 验收

Phase 14.1 完成条件：

- `ReportPos` 不再是玩家移动路径；MVP owner 使用输入帧 + predictor。
- 2 个客户端 150ms RTT / 2% 丢包下，owner 移动无明显回拉。
- 服务端权威位置不接受客户端绝对坐标。
- Windows / Linux 服务端和 Unity native predictor 对同一输入序列的结果在
  阈值内一致。
- 远端玩家仍通过现有 AoI volatile 位置流可见。

Phase 14.2 完成条件：

- CharacterMotor 使用 PhysicsQuery 做 capsule sweep、ground probe、
  depenetration、slope limit、step up、snap to ground。
- Jolt 类型不泄露到 gameplay / server / script 边界。
- Test backend 覆盖 KCC 状态机，不依赖 Jolt runtime。

## 测试矩阵

| 层级 | 覆盖 |
|---|---|
| unit | InputFrame codec、Predictor replay、CharacterMotor flat ground、correction tier |
| integration | BaseApp auth stamp、CellApp Real-only input、ack relay、offload stale input drop |
| client | Unity owner predictor、peer AvatarFilter 不回归、disconnect 清状态 |
| stress | 50/100/400 moving entities，输入丢包、乱序、burst |
| parity | Windows / Linux / Unity native 10k tick diff |

## 文档同步

实现落地时必须同步：

- `samples/mvp/README.md`
- `samples/mvp/readme_cn.md`
- `docs/gameplay/02_sync/MOVEMENT_SYNC.md`
- `docs/ue_client/open_questions.md`
- `docs/physics/physics_architecture.md`

在实现前，MVP 文档继续保留“client-authoritative movement”的描述，但必须
明确它是当前样例限制，不是生产目标。
