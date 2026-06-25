# Physics Backend Parity 测试基线

> **状态**：✅ 当前 quick gate。Phase 14.2+ 的 Flat / Static / Jolt backend
> parity 已落地；新增 backend 或 KCC 行为变更时必须扩展本测试。
>
> **用途**：记录当前 `movement_sim::Step` 在 Flat / Static / Jolt 三类
> `PhysicsQuery` backend 上的行为等价测试。该文档描述已落地的 quick gate，
> 不是 Phase 14.2 接入前的计划。
>
> **前置文档**：[`physics_architecture.md`](physics_architecture.md)、
> [`docs/gameplay/02_sync/MOVEMENT_SYNC.md`](../gameplay/02_sync/MOVEMENT_SYNC.md)、
> [`docs/roadmap/phase14_movement_authority.md`](../roadmap/phase14_movement_authority.md)。

## 1. 当前目标

`tests/parity/test_backend_parity.cpp` 对同一个 `ParityScenario` 分别运行可用
backend，并逐 pair 比较每 tick 的 `MovementState` 和 `MovementStepResult`。

当前保证：

1. Flat / Static / Jolt 在已覆盖 KCC 场景中保持行为等价。
2. `ATLAS_ENABLE_JOLT=OFF` 时仍运行 Flat / Static 可覆盖场景。
3. Jolt 开启时，同一套场景自动加入 Jolt backend。

非目标：

1. 不要求 bit-exact；浮点和 mesh 查询允许按容差漂移。
2. 不验证 Jolt 内部线程、allocator 或 callback 行为。
3. 不替代 `tests/unit/test_movement_sim.cpp`、`tests/unit/test_jolt_physics_query.cpp`
   和 collision pipeline 单测。

## 2. Backend 对照

当前没有 Jolt 专属的角色查询适配层。移动系统统一使用
`movement::PhysicsCharacterQuery` 适配 `physics::PhysicsQuery`；底层 query
决定具体 backend。

| BackendKind | Query 实现 | 覆盖能力 | 编译开关 |
|---|---|---|---|
| `kFlat` | `FlatPhysicsQuery` | 单一水平面 | 必编 |
| `kStatic` | `StaticPhysicsQuery` / chunked static query | box / plane / chunk boundary；mesh 场景用手工 slab box 近似 | 必编 |
| `kJolt` | `JoltPhysicsQuery` | box / mesh / layer mask 等 Jolt 路径 | `ATLAS_ENABLE_JOLT=ON` |

runner 会跳过场景未声明或当前构建不可用的 backend；如果一个场景少于两个可用
backend，gtest 将该场景 skip。

## 3. 当前场景

场景定义集中在 `tests/parity/parity_scenarios.cc`，通过
`AllScenarios()` 返回 quick gate 顺序。

| 场景 | Backend | 目的 |
|---|---|---|
| `flat_walk_forward` | Flat / Static / Jolt | 平地前进、grounded 维持 |
| `flat_stop_start` | Flat / Static / Jolt | 加速 / 停止 / 再启动 |
| `flat_jump_fall` | Flat / Static / Jolt | 起跳、下落、落地 |
| `box_drop_to_top` | Static / Jolt | 从空中落到 box 顶面 |
| `box_walk_steady` | Static / Jolt | box 顶面稳定行走 |
| `mesh_walk_long_path` | Static / Jolt | Static slab 近似与 Jolt mesh 的长路径容差 |
| `layer_mask_excludes_higher_box` | Static / Jolt | query layer mask 一致性 |
| `chunk_boundary_cross` | Static / Jolt | chunked Static 与 Jolt box 的边界穿越 |

## 4. 容差

容差定义在 `tests/parity/parity_scenario.h`：

| Profile | 位置 | 速度 | 方向 dot | 累积漂移 | flag lag |
|---|---:|---:|---:|---:|---:|
| `kStrictTolerance` | `1e-4m` | `1e-3m/s` | `0.99999` | `1e-2m` | `0` |
| `kNormalTolerance` | `1e-3m` | `1e-2m/s` | `0.9999` | `5e-2m` | `0` |
| `kMeshTolerance` | `5e-3m` | `5e-2m/s` | `0.999` | `2e-1m` | `1` |

mesh 场景使用 `kMeshTolerance`，因为 Static backend 用 slab 近似，Jolt backend
使用真实 triangle mesh。

## 5. 测试入口

当前只有一个 parity ctest target：

```text
test_backend_parity_quick
```

CMake 位置：`tests/parity/CMakeLists.txt`。

- 默认链接 `atlas_physics` 和 `atlas_movement_sim`。
- `ATLAS_ENABLE_JOLT=ON` 时追加链接 `atlas_physics_jolt` 并定义
  `ATLAS_PARITY_HAS_JOLT`。
- 关闭 Jolt 时，`kJolt` query factory 返回空，runner 自动跳过不可用 backend。

本地运行示例：

```powershell
ctest --test-dir build\debug -C Debug -R test_backend_parity_quick --output-on-failure
```

## 6. 新增场景规则

新增 backend parity 场景时，同步更新：

1. `tests/parity/parity_scenarios.cc` 的 factory 和 `AllScenarios()`。
2. 本文档的场景表。
3. 与场景匹配的容差 profile；无法解释的漂移不能靠放宽容差掩盖。

场景必须至少覆盖两个 backend，否则只属于单 backend 单测。
