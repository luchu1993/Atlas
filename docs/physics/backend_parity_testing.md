# Physics Backend Parity 测试设计

> **用途**：定义 Atlas 三个 `CharacterQuery` backend（FlatGround / Static /
> Jolt）在 KCC 关键路径上的等价性测试，作为 Phase 14.2 接入 Jolt 时的护城河。
>
> **读者**：服务端 / 物理 / 测试基础设施工程。
>
> **状态**：草案 v0.1 — 14.2 Jolt 接入前需评审，落地与 14.2.18 同步。
>
> **前置文档**：[`physics_architecture.md`](physics_architecture.md)、
> [`docs/gameplay/02_sync/MOVEMENT_SYNC.md`](../gameplay/02_sync/MOVEMENT_SYNC.md)、
> [`docs/roadmap/phase14_movement_authority.md`](../roadmap/phase14_movement_authority.md)。

---

## 1. 目标

让 `movement_sim::Step` 在三个 backend 上对同一输入序列产出**行为等价**的轨迹，
作为 14.2 接入 Jolt 时回归 14.1 已有 KCC 行为的硬约束。

具体保证：

1. 14.2 Jolt backend 落地后，14.1 已通过的 KCC 行为不退化。
2. 每个 backend 的优化 / 升级（Static 改 BVH、Jolt 升大版本）必须先过 parity gate。
3. 14.5 Combat / Skill query 接入后，parity 范围按相同框架扩展，不另起一套。

非目标：

1. 不要求三 backend bit-exact——浮点行为天然有 ULP 差异。
2. 不验证 Jolt 内部 callback / 内存 / 线程行为。
3. 不验证跨平台浮点完全一致；platform parity 由独立 CI 矩阵覆盖。
4. 不替代 `test_movement_sim.cpp` 的纯算法单测，那是更细的下层。

---

## 2. 三 backend 对照

| Backend | 用途 | 静态几何能力 | 动态几何能力 | 编译开关 |
|---|---|---|---|---|
| `FlatGroundQuery` | 14.1 KCC 单元测试基线 | 单一水平面 | 无 | 无（必编） |
| `StaticPhysicsQuery` | 14.1 / 14.2 资源管线测试主 backend | box / plane (asset v1)；mesh / heightfield (asset v2) | 无 | 无（必编） |
| `JoltCharacterQuery` | 14.2 起的生产 backend | 全部 asset v2 形状 + 后续扩展 | 后续阶段 | `ATLAS_ENABLE_JOLT=ON` |

Parity 矩阵：

| 组合 | 14.1 覆盖 | 14.2 覆盖 | 备注 |
|---|---|---|---|
| Flat ↔ Static | ✅ 已隐式 | ✅ 显式 gate | 用平地场景验证 Static 退化为单一平面时的等价 |
| Static ↔ Jolt | — | ✅ 主战场 | 14.2.18 的核心断言 |
| Flat ↔ Jolt | — | ✅ 烟雾测试 | 单一平面场景；快速发现 Jolt 退化 |

不在矩阵内的组合（如同一个 backend 自比）不算 parity，归常规回归测试。

---

## 3. 场景库

每个场景是一个 `ParityScenario`，含：初始状态、输入帧序列、tick 数、collision
builder（构造同语义的 Static / Jolt 场景）、容差档位、`skip_backends` 白名单。

### 3.1 必备场景（14.2.18 验收基线）

| ID | 场景 | tick | KCC 路径 | Flat 适用 | 容差档 |
|---|---|---|---|---|---|
| `flat_walk_forward` | 平地匀速前进 5 秒 | 150 | velocity 积分、grounded 维持 | ✅ | strict |
| `flat_stop_start` | 输入 0→1→0→1 交替 | 240 | acceleration / deceleration ramp | ✅ | strict |
| `flat_strafe_diagonal` | 同时按前 + 侧向 | 150 | 速度规范化 | ✅ | strict |
| `flat_jump_fall` | 起跳 → 落地 | 120 | jump 速度、gravity、ground detect | ✅ | strict |
| `slope_walk_up_walkable` | 走可走斜坡（接近 slope limit）| 180 | ground normal + slope clamp | ❌ | normal |
| `slope_reject_steep` | 顶陡坡 | 60 | velocity projection / clamp | ❌ | normal |
| `step_up_small` | 走过 step_height 以内台阶 | 90 | step-up sweep | ❌ | normal |
| `wall_slide_lateral` | 撞墙后沿墙滑 | 120 | sliding velocity continuation | ❌ | normal |
| `ceiling_jump_under_low` | 跳起撞低顶 | 60 | upward sweep 阻断 | ❌ | normal |
| `depen_initial_overlap` | 初始位置陷入 box | 30 | depenetration | ❌ | normal |
| `ground_snap_descend_lip` | 走过下沉台阶边沿 | 120 | snap-to-ground | ❌ | normal |
| `box_edge_capsule_probe` | 走过 box 边缘（半径 capsule cast） | 150 | ground probe 不退化为点查询 | ❌ | mesh |

### 3.2 扩展场景（14.2 主线之后追加）

| ID | 场景 | 追加阶段 |
|---|---|---|
| `mesh_walk_long_path` | 跨多个 mesh triangle 长距离前进 | 14.2 mesh 落地 |
| `heightfield_slope_traverse` | 在 heightfield 上斜向移动 | 14.2 heightfield 落地 |
| `chunk_boundary_cross` | 跨 chunk 边界查询 | 14.4 chunk 落地 |
| `moving_platform_ride` | 站在移动平台上 | 14.2.19 落地 |
| `skill_dash_through_corridor` | MovementCommand dash 沿走廊 | 14.5 skill 接入 |

每追加一个新场景，必须同时提供：

- 三个 backend 各自的 collision builder
- 容差档位（参见 §4）
- `skip_backends`（如 mesh 类场景必须 skip Flat）

### 3.3 场景实现位置

```
tests/parity/
├── CMakeLists.txt
├── parity_runner.{h,cc}        # ParityScenario / Runner
├── parity_tolerance.h          # 容差档位常量
├── scenarios/
│   ├── flat_scenarios.cc
│   ├── slope_scenarios.cc
│   ├── obstacle_scenarios.cc
│   └── mesh_scenarios.cc       # ATLAS_ENABLE_JOLT 才编
└── test_backend_parity.cc       # gtest 入口
```

---

## 4. 容差模型

按场景档位选择容差，每个 tick 都断言 per-field 偏差不超过档位上限。

### 4.1 档位定义

| 档位 | 适用场景 | 位置 (m) | 速度 (m/s) | 方向 dot | flags | snap event |
|---|---|---|---|---|---|---|
| `strict` | 纯水平 / 数值化路径 | 1e-4 | 1e-3 | > 0.99999 | bit-exact | bit-exact |
| `normal` | 静态 primitive、step / slope | 1e-3 | 1e-2 | > 0.9999 | bit-exact | bit-exact |
| `mesh` | mesh / heightfield 接触 | 5e-3 | 5e-2 | > 0.999 | bit-exact | 1-tick 容滞 |

### 4.2 累积漂移

除 per-tick 断言外，还断言 N tick 累积漂移：

```
cumulative_drift_m = ‖ end_position_a - end_position_b ‖
```

10k tick parity（场景：`flat_walk_forward` 循环）下：

| 档位 | cumulative_drift 上限 |
|---|---|
| `strict` | 1e-2 m |
| `normal` | 5e-2 m |
| `mesh` | 2e-1 m |

超过即失败。

### 4.3 bit-exact 字段

下列字段不允许任何 backend 差异：

- `last_processed_input_seq`
- `flags & kMovementFlagGrounded`（normal / mesh 档允许 1-tick 滞后，记录但不 fail）
- `MovementStepResult.jumped`
- `MovementStepResult.blocked`（mesh 档允许 1-tick 滞后）

### 4.4 合理差异（不算失败）

- ground normal 在 mesh 边沿三角形交界处的差异（角度 > 15° 即放弃比较该 tick 的 normal）
- `SweepHit.fraction` 在 backend 间数值差异（只比对最终 position）

---

## 5. Harness 结构

### 5.1 数据类型

```cpp
namespace atlas::physics::parity {

enum class BackendKind : uint8_t { kFlat, kStatic, kJolt };

struct ToleranceProfile {
  float position_eps_m;
  float velocity_eps_mps;
  float direction_dot_min;
  float cumulative_drift_m;
  uint8_t flag_lag_ticks;
  uint8_t blocked_lag_ticks;
};

struct ParityScenario {
  std::string_view id;
  movement::MovementConfig config;
  movement::MovementState initial_state;
  std::vector<movement::InputFrame> inputs;
  uint32_t tick_count{0};
  ToleranceProfile tolerance;
  std::vector<BackendKind> skip_backends;

  std::function<std::unique_ptr<movement::CharacterQuery>(BackendKind)> build_query;
};

struct PerTickRecord {
  uint32_t tick;
  movement::MovementState state;
  movement::MovementStepResult step;
};

struct ParityResult {
  bool passed{true};
  std::optional<uint32_t> first_divergence_tick;
  std::string diff_summary;
};

}
```

### 5.2 Runner 接口

```cpp
// Runs one backend through scenario, returns per-tick record vector.
auto RunScenario(const ParityScenario& scenario, BackendKind backend)
    -> std::vector<PerTickRecord>;

// Compares two backend runs against the scenario's tolerance.
auto ComparePair(const ParityScenario& scenario,
                 const std::vector<PerTickRecord>& a,
                 const std::vector<PerTickRecord>& b)
    -> ParityResult;
```

### 5.3 gtest 集成

```cpp
TEST_P(BackendParityTest, ScenarioPassesAllPairs) {
  const auto& scenario = GetParam();
  std::map<BackendKind, std::vector<PerTickRecord>> runs;
  for (auto backend : AllBackends()) {
    if (Contains(scenario.skip_backends, backend)) continue;
    runs[backend] = RunScenario(scenario, backend);
  }
  for (const auto& [a_kind, a_run] : runs) {
    for (const auto& [b_kind, b_run] : runs) {
      if (a_kind >= b_kind) continue;
      auto result = ComparePair(scenario, a_run, b_run);
      EXPECT_TRUE(result.passed)
          << "Pair " << Name(a_kind) << " vs " << Name(b_kind)
          << " first diverge tick=" << result.first_divergence_tick.value_or(0)
          << "\n" << result.diff_summary;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(All, BackendParityTest,
                         ::testing::ValuesIn(AllScenarios()));
```

`AllScenarios()` 在 `ATLAS_ENABLE_JOLT` 关闭时只返回 Flat / Static 适用场景，
Jolt-only 场景被跳过；`build_query` 中 `kJolt` 路径在关闭编译开关时返回
`nullptr`，runner 视为 skip。

### 5.4 不依赖 Jolt 的编译

`ATLAS_ENABLE_JOLT=OFF` 时：

- `test_backend_parity` 仍编译，运行 Flat ↔ Static parity
- `scenarios/mesh_scenarios.cc` 不编译，从 CMake target_sources 中排除
- CI 矩阵跑两组：`ATLAS_ENABLE_JOLT=OFF` 的最小 parity；`ATLAS_ENABLE_JOLT=ON` 的完整 parity

---

## 6. 调度与 CI 集成

### 6.1 ctest target

| Target | 触发 | 时间预算 |
|---|---|---|
| `test_backend_parity_quick` | 每 PR，必跑 | < 30s |
| `test_backend_parity_full` | 涉及 `movement_sim` / `physics*` 改动的 PR；nightly | < 5 min |
| `test_backend_parity_10k` | nightly only | < 30 min |

切分依据：

- `quick` = 所有 14.1 已覆盖的场景，每个 ≤ 240 tick
- `full` = 全场景，含 mesh / heightfield，每个 ≤ 600 tick
- `10k` = 选 3 个场景循环跑 10k tick，验累积漂移

### 6.2 CI gate

- PR 必跑 `quick`；红 → 阻断合入
- PR 标签 `physics-impact` 或路径匹配 `src/lib/{movement_sim,physics,physics_jolt}/` 时自动加跑 `full`
- nightly `10k` 失败 → Slack 通知 physics owner，不阻断 PR（避免间歇性问题阻塞合入流，但要求 24h 内复现 / 退回）

### 6.3 Jolt 升级流程的 parity 角色

按 `physics_architecture.md` §16 的 Jolt 升级流程，第 2 / 第 5 步分别对应：

- 第 2 步"跑 backend 集成测试" = `test_backend_parity_full` + `test_backend_parity_10k`
- 第 5 步"对比 query golden results" = 跑 parity 并把当前 Static run 作为 golden

升级 PR 必须附带 parity 全套通过截图 / artifact。

---

## 7. 失败诊断

### 7.1 diff_summary 格式

```
Pair Static vs Jolt diverged at tick 47
  position:  Static=(1.234, 0.000, 5.678) Jolt=(1.234, 0.012, 5.678) delta_y=0.012m
  velocity:  Static=(2.000, 0.000, 0.000) Jolt=(2.000, -0.500, 0.000)
  grounded:  Static=1 Jolt=0
  ground_normal_y: Static=1.000 Jolt=0.987
  inputs[45..47]: seq=45 (1,0,0,btn=0,dt=33ms) ...
  initial_state: pos=(0,0,0) vel=(0,0,0)
  config: max_speed=5 step=0.35 slope=50
```

足以让人脱离 IDE 就能定位差异点。

### 7.2 dump artifact

失败时 runner 把两个 backend 的完整 `PerTickRecord` vector 序列化到
`build/parity_artifacts/<scenario_id>.json`，CI 上传。包含：

- 输入帧
- 两 backend per-tick 状态
- 容差档位与超阈字段
- collision builder 描述（box list / mesh hash / plane spec）

### 7.3 复现 CLI

```
atlas_tool replay_parity \
  --scenario slope_walk_up_walkable \
  --backend-a static --backend-b jolt \
  --dump-per-tick
```

读 `scenarios/*.cc` 的相同定义，本地复现失败，不需要 gtest 环境。

---

## 8. 范围外

明确**不在 parity 范围**的内容，避免后续讨论摇摆：

1. 跨平台浮点：Windows MSVC vs Linux clang 的位差异由独立的 platform parity job 验
2. 跨语言 predictor：C++ / C# / UE owner predictor 之间的 parity 由 `test_movement_sim` + 10k tick predictor parity 覆盖，不混进 backend parity
3. 性能 / 内存：parity 只断言行为，性能由 Tracy / watcher 单独追
4. Jolt simulation step：query-only 阶段不验
5. Cross-Cell 与 Offload 一致性：由 phase11 / phase14.4 自己的集成测试覆盖

---

## 9. 演化

### 9.1 14.5 Skill / Combat 接入

新增 `SkillQueryParityScenario`：构造若干 entity + skill hitbox，断言三 backend 返回
同一 EntityId 候选集（顺序无关），命中字段不要求 bit-exact。

### 9.2 14.6 NavMesh

Path 查询 parity 不要求路径完全一致（A* 实现差异天然不同），只断言：

- 起点 / 终点可达性一致
- 路径总长度差异 < 容差
- 中间点全部在 walkable 区域

### 9.3 collision asset 演化

asset 版本 bump（v1 → v2 → ...）必须在同一 PR 内提供：

- 新场景或现有场景的 v2 builder
- 通过 `test_backend_parity_full`

否则 schema 变更不允许合入。

---

## 10. 落地清单（14.2.18 PR 切片）

| PR | 内容 | 行数估算 |
|---|---|---|
| #1 | `tests/parity/` 骨架 + Flat/Static runner + `flat_walk_forward` 场景 | ~600 |
| #2 | slope / step / wall / ceiling / depen 场景 + tolerance profile | ~800 |
| #3 | `test_backend_parity_quick` ctest target + CI gate | ~200 |
| #4 | dump_artifact + `atlas_tool replay_parity` | ~400 |
| #5 | Jolt scenario builder + `mesh_scenarios.cc`（与 14.2.6 同步）| ~500 |
| #6 | nightly `_10k` target + 累积漂移断言 | ~300 |
| #7 | physics_architecture.md / phase14 roadmap 状态更新 | 文档 |
