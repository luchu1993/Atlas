# Phase 14 状态：已交付能力与现行 contract

> 配合 [`phase14_movement_authority.md`](phase14_movement_authority.md) 使用。
> 主路线图只承载目标 / 验收 / 里程碑；本文件承载实现日志、已交付能力清单
> 与已收紧的 wire contract，避免主路线图被状态描述持续撑大。
>
> 更新时机：每次有新能力进入主线、新 contract 收紧后立即在此追加。
> 主路线图不要重复这里的内容。

---

## 14.1 已交付能力

### 输入帧链路

- `movement_sim` 提供 InputFrame / MovementState / CharacterQuery 共享模型。
- 客户端 → BaseApp → CellApp 的 `ClientMovementInput` /
  `ClientMovementInputForward` 走 unreliable immediate，含 frame_count、
  seq、`client_dt_ms`。
- BaseApp 与 CellApp 两侧都有服务端校验和 token bucket 限流。
- 重复 / 过大 seq gap / invalid frame 均会被 drop，各类 drop 有独立
  watcher。

### Owner 预测与 ack 链路

- `Atlas.Client` 提供共享 `OwnerMovementPredictor`，Unity MVP、桌面脚本
  客户端、UE MVP 通过 native predictor C API 复用同一仿真。
- owner 输入历史与 ack 过滤已处理 seq 回绕与同 seq server tick latest-wins。
- BaseApp ack relay 已按 seq / cell epoch 过滤旧 ack 并暴露 watcher；
  随实体销毁清理 relay 状态。
- 断线重连后，BaseApp 与 owner predictor 都会用最新 ack 重新播种输入 seq，
  避免旧会话状态把新输入当 stale。

### 纠正与可疑行为

- Correction tier 阈值在 `movement_sim` / `Atlas.Client` / UE native predictor
  共享。
- BaseApp relay 按 ack flags 统计 correction watcher。
- 连续 Tier2 / Snap 大纠正进入 suspicious watcher。
- Unity / UE owner 在 ack replay 后回报 correction report；BaseApp 仅用于
  观测和可疑升级，不参与权威位置计算。

### CellApp 权威 step

- MovementSystem 每 tick 推进玩家输入 / NPC intent / 基础 CharacterMotor，
  耗时进入 watcher。
- 对 movement state 做 finite / 速度 / 垂直速度 / 水平加速度 / 坐标边界校验。
- Position history ring buffer 在 tick 写回后记录，watcher 暴露样本数。
- 脚本可通过 `CellServerEntity.TryGetMovementHistorySample` 读插值后样本。
- Offload 会迁移最近 history 窗口；reject / timeout 回滚同样恢复。

### Physics query 契约（14.2 前置）

- `atlas_physics` query 契约、Null / Flat / Static backend、
  `PhysicsCharacterQuery` movement 适配器已落地。
- Space 持有可替换 `PhysicsQuery` backend，当前默认 Static backend；
  CellApp movement tick 按实体所属 Space 路由查询。
- Atlas collision asset v1 JSON loader 支持 box / plane，
  `atlas_tool validate_collision` / `dump_collision --obj` 可用于内容前置
  检查。
- Space 可装载 collision asset 替换本 Space query；Cell C# 脚本可调
  `CellServerEntity.LoadCollisionAsset(spaceId, path)`。
- `ATLAS_ENABLE_JOLT` 默认 ON；Jolt 5.2.0 通过 FetchContent `URL + SHA256`
  锁版本拉取，Win debug 下 `test_jolt_physics_query` 18 个 case 与三后端
  `test_backend_parity_quick` 通过。Linux 验证留 CI fast-follow。
- `JoltPhysicsQuery` 暴露 `CookMeshShape` / `AddCookedMeshShape` /
  `CookCollisionMeshes` / `RestoreCookedMeshes` / `CurrentJoltStamp`，把
  Jolt `Shape::SaveBinaryState` 二进制写进 `.collisioncache`、加载时
  `RestoreFromBinaryState` 跳过 BVH 重建。Jolt 类型仍不出
  `physics_jolt` 边界。
- `.collisioncache` 格式升到 v2：`uint64 jolt_version_stamp`（=
  `JPH_VERSION_ID`，含 features/major/minor/patch）+ 末尾 cooked blob
  section。Reader 兼容 v1 layout（uint32 stamp、无 cooked）作过渡。
- `atlas_tool cook_collision` 链 `atlas_physics_jolt`，cook 时调
  `CookCollisionMeshes` 写入真 cooked blob 和当前 stamp，并 reload 校验。
- `atlas_tool recook --invalid <dir>` 递归扫描 `.collisioncache`，对
  stamp mismatch / 有 mesh 但 cooked 为空的 cache 自动从同名 source
  `.collision.json` 重 cook。
- `physics::CollisionBackendFactory` 抽象接口（physics 层）+
  `JoltCollisionBackendFactory`（physics_jolt 层）把 cooked cache 变成运行时
  `PhysicsQuery`。CellApp 在 Init 注入 Jolt factory（`ATLAS_ENABLE_JOLT` 下），
  每个 Space 创建时继承；Jolt 类型不出 `physics_jolt` 边界。
- `Space::LoadCollisionCacheFromFile` 走 factory：mesh-bearing cache 必须
  stamp 匹配且 cooked 非空（`RestoreCookedMeshes` 跳过 BVH 重建），否则报错
  不降级；无 factory 时 box/plane cache 退回 Static，mesh cache 直接拒绝。
  `CellApp::LoadCollisionAsset` 按扩展名分流：`.collisioncache` 走 cache
  路径，`.collision.json` 仍走 uncooked Static dev 路径。
- collision asset 加 `sphere` / `capsule` shape（v2）：Jolt
  `AddSphere` / `AddCapsule` 全支持，Static 像跳过 mesh 一样跳过它们（parity
  维持 box/plane）。exporter 从 `SphereCollider` / Y 轴 `CapsuleCollider`
  发射（要求 uniform scale）。MVP `Main.unity` 已含 sphere boulder + capsule
  pillar；live cluster 加载 5-shape cache（box×3 / sphere / capsule）无错。
- MVP 碰撞垂直切片已闭合：`Main.unity` → exporter → `main.collision.json` →
  `run_mvp_cluster` cook → `MvpSpace.OnSpaceInit` 经 Jolt backend 加载；
  live cluster 日志确认 `CellApp: loaded collision cache` +
  `MvpSpace: loaded collision` 后 seed 150/150 NPC。`test_collision_pipeline`
  headless 覆盖 cooked cache → Space → 移动撞墙截停 / cooked-mesh 落地。
- `Atlas.Mvp.Editor.AtlasCollisionExporter` 在 Unity batch mode 下扫
  `ServerColliderAuthoring(exportToServer=true)`，把 axis-aligned
  `BoxCollider` 写成 collision asset v2 JSON；旋转 box / sphere /
  capsule / mesh / terrain / 负缩放打 warning 并跳过。
  `tools/bin/export_collision_unity.{bat,sh}` 提供与
  `build_mvp_unity` 一致的 Unity executable 发现策略；Unity 发现
  helper 已提到 `tools/common/unity.py` 复用。

### CharacterMotor

- ground normal 驱动的 slope limit。
- 非跳跃 grounded sweep 命中时支持基础 step-up，显式标记 snap-to-ground，
  起跳 tick 不会被 snap 拉回地面。
- 命中阻挡面后沿裁剪速度消费剩余位移；按配置预算执行初始重叠 depenetration。
- Static backend 覆盖静态 box / 平面 ground probe / raycast、向下 ground
  capsule cast、ground / plane depenetration、layer mask。
- CellApp 把角色胶囊半径传入 ground probe，避免静态盒体边缘退化为点查询。

### MovementCommand（技能位移）

- `movement_sim` 提供 MovementCommand / MovementCurve 模型、曲线注册 store
  和曲线推进纯函数。
- CellApp active command store 在 movement tick 通过默认线性曲线推进。
- 脚本 API：`CellServerEntity.SetMovementCommand` /
  `RegisterMovementCurve` / `ClearMovementCommand`。
- 碰撞策略：`Stop` / `EndSkill` 按 Space Static query 截停 command，
  `Continue` 保持穿越。
- Priority：只允许更高 priority 打断当前 command，并先广播旧 command 的
  cancelled end；同一 command_id 可续写。
- `allow_full` 仍为保留协议值，服务端拒绝执行（混合策略未落地）。
- Active command + 最近 history 窗口 + motor state 都随 Offload 迁移，
  失败回滚一并恢复。
- CellApp 集成测试覆盖：step / 边缘探地 / 坠落 / depenetration / 陡坡拒绝 /
  跳跃撞低顶后保持空中 / command 碰撞截停 / command priority 冲突 /
  allow_turn 只更新朝向。

### MovementCommand 客户端 fanout

- `MovementCommandStart` / `MovementCommandEnd` 已有 CellApp fanout、
  BaseApp → Client wire id、native client 转发、C# 解码事件、UE
  `AtlasNetClient` 解码。
- Unity / UE MVP owner predictor 与 script-client predictor 都能应用
  command start / end，并按 `curve_id` 采样注册曲线。
- 非 owner 端的 remote interpolator（`ClientEntity` /
  UE `FBpAvatarEntity`）在命令期间覆盖 `AvatarFilter` 输出。

### 压测与脚本路径

- 真实 `atlas_client` 脚本客户端可通过
  `world_stress --client-transport-impairment-ms` 注入双向 RUDP 延迟 / 丢包。
- 2 client × 20 秒 × 150ms RTT / 2% loss smoke 已跑通；脚本 tap 可见
  `mIn` / `mAck` / `mRpt` 与 Tier1 / Tier2 correction。
- `run_world_stress` script-client smoke 结束后打印 BaseApp / CellApp
  movement watcher summary。
- `--script-verify` 同时要求服务端 watcher 汇总非零，把客户端 tap 和服务端
  权威链路对齐。
- 裸协议 `world_stress --move-mode input --movement-verify` 直接压
  input / ack 链路；50 / 100 / 400 moving entities smoke 已通过 watcher gate。
- `--movement-input-redundant-frames` 覆盖每包 2/3 帧的 burst / stale 去重；
  裸协议 input drop / reorder 注入已进入 world_stress。

### Skill 接入第一条路径

- MVP `Avatar.Dash` 通过 own-client cell RPC 写入 server-stamped
  `MovementCommand`，作为技能位移接入的首条 playable action。
- 完整 data-driven skill timeline 仍在 14.5+ 接入。

---

## 已收紧的 wire contracts

- **movement input / forward payload**：entity id 必须非 0；
  `client_dt_ms` 必须在 1-250ms wire decode 范围内。
- **movement ack / command start / command end**：entity id 必须非 0；
  真实 command id 不能为 0。
- **MovementCommand**：`duration_ms` 必须非 0；
  `elapsed_ms <= duration_ms`；command 与 end state 坐标必须为有限值；
  enum 必须在协议范围内。
- **correction report**：target 必须非 0；distance 必须为有限非负值；
  flags 必须与 distance tier 一致。
- **movement ack / correction report 的 `correction_flags`**：必须落在
  `{0, Tier1, Tier2, Snap}` 单 tier 枚举集合，多 bit 组合或保留位
  C++ / C# / UE wire decode 直接 drop。
- **C++、C# 与 UE `AtlasNetClient`** 的 movement ack / command decode
  按同一 wire contract 对齐。

---

## 已验证的命令集合

每次 PR 推荐重跑这套作为最小回归：

- `tools\bin\build.bat debug --build-only`
- `ctest --test-dir build\debug -C Debug --output-on-failure`，过滤 BaseApp /
  CellApp movement message、handler、ABI 和 `movement_sim` 相关测试。
- `dotnet test tests\csharp\Atlas.Client.Tests\Atlas.Client.Tests.csproj
  --configuration Debug`
- `tools\bin\build_mvp_ue.bat --config Debug --ue-root E:\UE\UnrealEngine
  --target UEClientEditor --build-config Development --platform Win64 --skip-native
  --skip-defs --skip-codegen --skip-stage`
- `UnrealEditor-Cmd.exe` 跑 `Automation RunTests Atlas.NetClient`。
