# 确定性契约（Determinism Contract）

> **用途**：定义 Atlas 游戏核心仿真层必须遵守的确定性规约。这份契约是所有后续代码的**前置依赖**——违反它意味着端同漂移、和解失败、回放失效、反作弊失能。
>
> **读者**：所有游戏逻辑开发者（C++ 与 C#）、工具链开发者。不涉及渲染、音效、UI 开发者。
>
> **状态**：草案 v0.1 — 待团队评审。
>
> **前置文档**：`OVERVIEW.md`
>
> **更新节奏**：项目早期锁定核心规约后极少变动；新增禁用 API 或新增平台时追加。

---

## 1. 为什么确定性至关重要

Atlas 的多个核心能力**建立在端同仿真一致性之上**：

- **客户端预测 + 和解**：服务端和客户端用同一段代码计算同一输入，结果必须一致
- **战斗回放**：记录输入流，重播得到相同结果（用于 debug、反作弊、比赛观战）
- **跨服务器迁移**：实体从一个 CellApp 迁到另一个，状态必须精确复现
- **自动化回归测试**：每次改动后回放测试用例，结果二进制一致才算通过

**一旦发生漂移**：
- 玩家感知为"我打到了你，服务端说没打到"——最糟糕的用户体验
- 反作弊失去参照基准——作弊者可以钻漏洞
- Bug 无法稳定复现——debug 效率崩溃

**Day 1 不锁定契约的代价**：一年后发现漂移，但代码已无数地方违反规约，重构成本远超当时锁定的成本。这就是本文存在的意义。

---

## 2. 契约适用范围

### 2.1 必须确定性的代码

- **移动仿真**（`src/lib/movement/` C++ native plugin）
- **技能执行**（Timeline + State Machine runtime，`Atlas.CombatCore` C#）
- **Buff 系统**（状态变化、modifier 计算）
- **命中判定**（hitbox 碰撞、伤害计算）
- **AI 决策**（行为树 tick、目标选择）
- **所有"影响游戏世界权威状态"的代码**

### 2.2 允许非确定性的代码（明确豁免）

以下代码**不受本契约约束**，可以使用系统时钟、随机、浮点加速等：

- UI 渲染、UI 动画
- 粒子特效、VFX
- 音效、音效混合
- 相机抖动、后处理
- 客户端本地日志、telemetry
- 网络协议的底层（序列化、握手）
- 编辑器工具（编辑期代码）

**判断标准**：这段代码的输出**会不会写回权威游戏状态**（伤害、位置、buff、物品）？如果不会，属于豁免。

### 2.3 边界用例

- **音效时机**：属于豁免（本地播放）。但"伤害事件"是权威事件，触发音效的**事件时间戳**必须确定性。
- **预测回滚**：客户端视觉预测属于豁免；服务端权威结算必须确定性。
- **调试日志**：可以用 wall clock 写 log 时间戳。但不能用 wall clock 作为游戏逻辑输入。

---

## 3. 浮点规约（Float）

### 3.1 语言与编译器设置

**C++（服务端 + native plugin）**：

```cmake
# 强制 IEEE 754 严格语义，禁用优化引入的不确定性
# MSVC:
add_compile_options(/fp:strict /fp:except-)
# GCC / Clang:
add_compile_options(-frounding-math -fsignaling-nans)
add_compile_options(-ffloat-store)  # 仅 x87，x64 默认 SSE 可忽略

# 禁用以下选项（即使 release 构建也禁用）
# /fp:fast     ← 改变计算顺序
# -ffast-math  ← 违反 IEEE 754
# -funsafe-math-optimizations
# -fassociative-math
# -freciprocal-math
# --use-fast-math (NVCC 等)
```

**CMake 工程层面**：在 `CMakeLists.txt` 设置全局 flag，确定性代码目录不允许单独覆盖。CI 检查是否有子项目启用了禁用选项。

**C#（Unity + CoreCLR）**：

C# 的浮点确定性有**特殊复杂度**：
- JIT 编译差异（Unity Mono vs CoreCLR vs IL2CPP）
- x87 vs SSE 寄存器策略差异
- `Math.Sin` 等内置函数跨平台实现差异

**Atlas 对策**：
- 所有端同浮点计算必须通过 **`Atlas.CombatCore.Math`** 库（自研）
- 库内用"软件实现"替代平台数学函数（`Sin`、`Cos`、`Sqrt`、`Exp`、`Pow`）
- 禁止直接调用 `System.Math`、`UnityEngine.Mathf` 做权威计算
- 核心 `+ - × ÷` 依赖 IEEE 754 硬件保证（这是跨平台可信的）

### 3.2 允许的浮点操作

| 操作 | 可信度 | 备注 |
|---|---|---|
| `+`, `-`, `×`, `÷` | ✅ 跨平台一致 | IEEE 754 硬性保证 |
| `==`, `<`, `>`, `≤`, `≥` | ✅ 一致 | 注意 NaN 比较返回 false |
| `sqrt()`（IEEE 754 版） | ✅ 一致 | 正确舍入的 sqrt 是标准的 |
| `Atlas.CombatCore.Math.Sin` 等 | ✅ 一致 | 自研软件实现 |
| `std::sin`, `Math.Sin` | ❌ **禁止** | 跨 libc / JIT 差异 |
| `FMA` 指令 | ⚠️ 审慎 | 需确认所有目标 CPU 支持且编译器生成一致 |
| subnormal 运算 | ⚠️ 审慎 | 确保 FTZ/DAZ 关闭 |
| 浮点异常 | ❌ 不触发 | 禁用 FE_INVALID、FE_DIVBYZERO 等的异常处理 |

### 3.3 跨平台一致性验证

**验证方法**：
- CI 运行 10000 次"随机输入 → 仿真 → 输出"测试
- 分别在 Windows x64 (MSVC)、Linux x64 (Clang)、（未来）WSL 上跑
- 结果按字节比较
- 任一不一致 → 阻塞合并

**首次触发条件**：Phase 0 地基搭建完毕时必须建立，不得延后。

---

## 4. 随机数规约（RNG）

### 4.1 禁用的 RNG 源

**绝对禁用于权威逻辑**：

C++：
- `rand()`, `srand()` ← libc 实现差异巨大
- `std::mt19937` ← 虽然算法固定但 seed_seq 行为有差异
- `std::random_device` ← 硬件源，非确定
- `/dev/urandom`, `CryptGenRandom` ← 非确定
- `std::shuffle` 默认 RNG ← 非确定

C#：
- `System.Random`（不带 seed 参数调用时） ← 基于 DateTime
- `UnityEngine.Random` ← 全局状态，任何调用点修改都影响
- `RandomNumberGenerator.Create()` ← 加密级，非确定
- `Guid.NewGuid()` 用于仿真 ← 基于 MAC 地址和时间

### 4.2 Atlas 指定 RNG

**算法**：`xoshiro256**`（质量好、快、state 小、周期长）

**接口**（端共享，C++ 与 C# 各一份，位比特一致）：

```cpp
// src/lib/foundation/deterministic_rng.h
namespace atlas {
class DeterministicRng {
 public:
  explicit DeterministicRng(uint64_t seed_hi, uint64_t seed_lo);
  
  uint64_t NextUint64();
  uint32_t NextUint32();
  float    NextFloat();           // [0, 1)
  float    NextFloat(float min, float max);
  int32_t  NextInt(int32_t min_inclusive, int32_t max_exclusive);
  bool     NextBool(float true_probability);
  
 private:
  uint64_t state_[4];
};
}
```

### 4.3 种子派生规则

**禁止**使用固定种子 or 墙钟派生。**所有随机源必须由以下输入可复现推导**：

```
seed_hi, seed_lo = hash_128(
  session_id:    uint64,   // 本局/本图的唯一标识
  entity_id:     uint64,   // 发生随机的实体 ID
  tick:          uint64,   // 当前 server tick
  purpose_tag:   uint32,   // 随机用途（crit/loot/ai_choice/etc）
  counter:       uint32,   // 同 tick 内第 N 次随机
)
```

这样**给定输入可精确复现任何随机序列**，支持：
- 战斗回放
- 跨端结果一致
- 反作弊审计
- 平衡性测试（同种子反复跑）

### 4.4 实用约束

- **RNG 实例短生命周期**：通常每个事件新建 RNG，用一次就销毁；不长期持有 state
- **counter 由调用方维护**：同一 tick 内多次随机要自增 counter，不能重用
- **purpose_tag 枚举化**：所有随机用途在 `RandomPurpose` 枚举中注册，便于审计
- **单线程内使用**：RNG 实例不跨线程共享（cell 内串行原则）

---

## 5. 时间规约

### 5.1 唯一权威时钟：Server Tick

**定义**：
- `server_tick` 类型 `int64`，从 session/图启动时为 0，单调递增
- 开放世界 tick 频率 **20 Hz**（1 tick = 50 ms）
- 副本/竞技场 tick 频率 **30 Hz**（1 tick = 33.33... ms）
- 同一个 session 内 tick 频率固定，不动态调整

**所有游戏逻辑时间计算必须基于 server_tick**：

```cpp
// 好: 基于 tick
if (entity.StunEndTick() <= ctx.CurrentTick()) {
  entity.ClearStun();
}

// 差: 基于墙钟
if (entity.StunEndTime() <= std::chrono::steady_clock::now()) {  // 禁止
  entity.ClearStun();
}
```

### 5.2 亚 tick 精度：事件时间戳

某些场景需要亚 tick 精度（如格挡窗口 100ms 跨越 2 个 tick 边界）。引入**事件时间戳**：

```
event_time = tick × tick_duration_us + intra_tick_us
```

- `tick_duration_us` = 50000 (开放世界) or 33333 (副本)
- `intra_tick_us` = 本 tick 内的微秒偏移 (0 到 tick_duration_us - 1)

这用于命中判定、格挡窗口、输入对齐等精细判定，**不是实时时钟**，仍是 tick 衍生量。

### 5.3 客户端时间

客户端的 `server_tick` 由**时间同步协议**估算（见 `NETWORK_PROTOCOL.md`）：
- 周期性握手样本
- 滑动窗口取最小 RTT 样本
- EMA 平滑
- 永远只是估算，客户端做"我猜服务端现在是 tick X"的预测

### 5.4 允许的 wall clock 用途

仅限以下场景可用 `std::chrono::system_clock` / `DateTime.Now`：
- Telemetry / 日志时间戳
- 协议层的握手、心跳（协议本身）
- 玩家可见的"今日登录时长"等 UI 显示
- 文件 I/O 时间戳

**绝不允许**用于：
- 战斗逻辑任何决策
- Buff 持续时间计算
- 技能冷却计算
- 事件排序

---

## 6. 坐标规约

### 6.1 实体位置表示

```cpp
struct EntityPosition {
  uint32_t cell_id;    // 所在 cell 的 ID
  float3   local;      // cell 内相对坐标（单位：米）
};
```

### 6.2 Cell 尺寸与精度

- Cell 水平尺寸：**256 m × 256 m**
- Cell 内坐标范围：`local.x, local.z ∈ [-128, 128]`
- 高度 `local.y` 不受 cell 约束，但建议范围 `[-512, 512]`

**float 精度分析**：
- 128m 处 float ULP ≈ 7.6e-6 m ≈ 0.008 mm
- 远比角色碰撞（0.4 m）、技能精度（厘米级）需求高
- 整个 cell 内不会出现精度丢失

### 6.3 为什么不用世界坐标

如果用世界坐标，16km 地图 `x ∈ [-8000, 8000]`：
- 8000 处 ULP ≈ 0.00048 m = 0.48 mm
- 虽然对位置够用，但**速度×dt 累加误差会累积**
- 更严重：端同浮点运算在 8000 附近比在 100 附近更容易出现"不同 CPU 结果差 1 ULP"

**cell 相对坐标保证所有运算都在小数值域进行**，是端同一致性的保险。

### 6.4 跨 cell 操作

- 实体跨 cell 时，**local 坐标重新相对于新 cell 原点计算**
- 跨 cell 的距离/方向计算通过 **ghost entity 机制**（见 `01_world/GHOST_ENTITY.md`）
- 永远不做 `entity_A.local - entity_B.local` 除非确认在同 cell
- 跨 cell 距离：`(B.cell_origin - A.cell_origin) + (B.local - A.local)`

### 6.5 角度/朝向规约

- yaw（水平朝向）用 `uint16`：`0..65535` 映射到 `[0, 2π)`，精度 ≈ 0.0055°
- 权威存储用 `uint16`，计算时转 float `radians`
- 加减角度在 `uint16` 域做（自动 wrap）

---

## 7. 集合与迭代顺序

### 7.1 禁用的容器（权威状态）

**C++**：
- `std::unordered_map`, `std::unordered_set` ← 迭代顺序非确定
- `std::hash_map` 各变体
- 存放仿真状态的 `std::map<Key, Value>` 如果 Key 是浮点或指针 ← 浮点比较 / 指针地址非确定

**C#**：
- `Dictionary<K, V>` 用于权威仿真状态 ← 虽然 .NET 实现保持插入顺序，但**不是契约**，未来版本可能改
- `HashSet<T>`
- `ConcurrentDictionary`

### 7.2 推荐容器

**C++**：
- `std::vector<std::pair<Key, Value>>` + 排序 → 二分查找
- `std::map<K, V>`（Key 为整数/字符串时有序）
- 自研 `FlatMap<K, V>`（基于 vector，保证迭代顺序）

**C#**：
- `SortedDictionary<K, V>`
- `List<KeyValuePair<K, V>>` + 手动排序
- 自研 `DeterministicDict<K, V>`

### 7.3 迭代规则

需要遍历实体/事件时：

```csharp
// 好: 按确定性键排序后迭代
foreach (var entity in cell.Entities.OrderBy(e => e.Id)) {
  entity.Tick();
}

// 差: 直接迭代 unordered 容器
foreach (var entity in entities) {  // 若 entities 是 HashSet 禁止
  entity.Tick();
}
```

**事件排序**：一个 tick 内多个事件按 `(event_time_us, source_entity_id, event_id)` 三级排序后消费。

### 7.4 并行执行的约束

- **Cell 内实体仿真强制串行**（单线程），不得并行
- 多 cell 之间可并行，但 cross-cell 消息在 tick 结束时统一收集、排序后处理
- 并行仅允许用于**纯读操作**（空间查询、碰撞查询）且不能在查询期间修改空间结构

---

## 8. 禁用 API 完整清单

### 8.1 C++ 禁止项

| API | 禁止原因 | 替代 |
|---|---|---|
| `rand()`, `srand()` | libc 实现差异 | `DeterministicRng` |
| `std::mt19937` | 行为不完全规定 | `DeterministicRng` |
| `std::random_device` | 非确定 | `DeterministicRng` |
| `std::chrono::system_clock::now()` | 墙钟 | `ctx.CurrentTick()` |
| `std::chrono::steady_clock::now()` | 墙钟 | `ctx.CurrentTick()` |
| `std::sin/cos/tan/exp/log/pow` | libc 差异 | `Atlas::Math::Sin` 等 |
| `std::unordered_map/set` 存权威 | 迭代顺序 | `std::map` 或 `FlatMap` |
| `memcmp` 比较浮点 | -0.0 和 +0.0 bit 不同但相等 | 按语义比较 |
| `#pragma fenv_access off` | 浮点状态优化 | 不使用 |
| `-ffast-math` / `/fp:fast` | 破坏 IEEE 754 | `/fp:strict` |
| `reinterpret_cast<float>(int)` 跨端 | 字节序差异 | 显式序列化 |
| 未指定求值顺序（`f(g(), h())`） | 跨编译器差异 | 拆分语句 |

### 8.2 C# 禁止项

| API | 禁止原因 | 替代 |
|---|---|---|
| `System.Random`（默认构造） | 基于时钟 | `DeterministicRng` |
| `UnityEngine.Random` | 全局状态 | `DeterministicRng` |
| `DateTime.Now / UtcNow` | 墙钟 | `ctx.CurrentTick` |
| `Environment.TickCount` | 墙钟 | `ctx.CurrentTick` |
| `System.Math.*` 权威计算 | JIT 差异 | `Atlas.CombatCore.Math.*` |
| `UnityEngine.Mathf.*` 权威计算 | 同上 | 同上 |
| `Dictionary<K,V>` 权威状态 | 非契约顺序 | `SortedDictionary` 或自研 |
| `Guid.NewGuid()` 仿真 ID | 基于 MAC+时钟 | 自研 ID 分配器 |
| `Task.Run` 修改仿真状态 | 并行修改 | cell 内串行 |
| `object.GetHashCode()` 用于排序 | 默认实现非确定 | 显式 Id 字段 |
| `float.NaN` 作为"未设置"哨兵 | NaN 比较行为复杂 | 独立 `HasValue` 字段 |

### 8.3 违例检测

CI 集成**静态检查**：
- C++：clang-tidy 自定义 check + 正则扫描
- C#：Roslyn analyzer，扫描所有 `using System.Random` 等 import
- 违例阻塞 PR 合并

---

## 9. 端同验证工具

### 9.1 Dual-run 测试

同一机器上**用同一输入运行仿真两次**，比较最终状态：

```
输入流 (input_log.bin) ─┬─► 仿真实例 A ─► 状态快照 A
                       └─► 仿真实例 B ─► 状态快照 B

diff(A, B) must be empty
```

用途：抓出**隐藏的非确定性**（未初始化内存、迭代顺序、时序依赖）。

### 9.2 Cross-platform 测试

在 Windows 和 Linux 分别跑同一输入：

```
Windows: input_log → state_snapshot_win
Linux:   input_log → state_snapshot_linux

diff(state_snapshot_win, state_snapshot_linux) must be empty
```

用途：抓出**跨平台差异**（浮点、数学函数、字节序）。

### 9.3 Cross-language 测试（关键）

C++ 仿真与 C# 仿真用同输入：

```
C++ (native plugin): input → state_cpp
C#  (CoreCLR/Unity): input → state_cs

diff(state_cpp, state_cs) must be within tolerance
```

tolerance：
- 位置：≤ 1e-5 m（浮点算术允许的最终误差）
- 计数器（int）：必须完全相等
- 状态 ID：必须完全相等

核心理念：C++ 和 C# 走不同执行路径但应产生**语义一致**结果。硬件浮点允许 1 ULP 差异，但不允许 N 次连锁放大。

### 9.4 Replay 测试

- 记录在线玩家一段战斗（输入流 + 事件流）
- 实验室重放，状态重现
- 常用于 debug 客户反馈的 bug

### 9.5 CI 集成

所有测试**PR 必跑**：
- Dual-run 基础测试（1000 tick 对局）：PR 必过
- Cross-platform 测试：nightly build
- Cross-language 测试：nightly build
- Replay 回归：已收集 bug 样本的 replay 每次都跑

---

## 10. 浮点数学库 `Atlas.CombatCore.Math`

为保障 `sin/cos/sqrt/exp/log` 等的端同一致，自研软件实现：

### 10.1 设计约束

- **端同二进制一致**：C++ 与 C# 版本用相同位运算算法
- **IEEE 754 严格**：结果正确舍入到最接近的 float
- **精度 ≥ 23 bit**（float 全精度）
- **性能可接受**：每次调用 < 100 ns（查表法 + 多项式）

### 10.2 接口

```cpp
// C++ 版本
namespace atlas::math {
  float Sin(float radians);
  float Cos(float radians);
  void  SinCos(float radians, float* sin_out, float* cos_out);
  float Sqrt(float x);
  float InvSqrt(float x);       // 1/sqrt(x)
  float Atan2(float y, float x);
  float Exp(float x);
  float Log(float x);
  float Pow(float base, float exp);
}
```

```csharp
// C# 版本
namespace Atlas.CombatCore.Math;

public static class FMath {
  public static float Sin(float radians);
  public static float Cos(float radians);
  public static (float sin, float cos) SinCos(float radians);
  // ... 其余方法名保持一致
}
```

### 10.3 实现策略

- 查表 + 三次多项式逼近
- 关键常数（π, ln2 等）共用同一个 constants 文件，C++ 和 C# 各自内嵌
- 单元测试对比 Rust 的 `libm`（公认高质量参考）

---

## 11. 常见疑问 FAQ

### Q1: 为什么不用定点数避免所有这些麻烦？

**权衡考量**：
- 定点数：绝对端同，但代码复杂度高，策划数值调参不直观，性能（定点乘法）不一定更快
- IEEE 754 + 严格模式：99% 情况下端同，复杂度低，性能好

团队规模 2 技术 + 策划主导数值，定点数带来的"不直观"代价**远超端同收益**。已有多个商业 MMOARPG（包括 BDO）使用 float。

### Q2: `Atlas.CombatCore.Math.Sin` 比 `Math.Sin` 慢多少？

实测（单次调用）：
- `Math.Sin`: ~20–30 ns
- `Atlas.CombatCore.Math.FMath.Sin`: ~50–80 ns

慢 2–3 倍，但**每帧调用量有限**（每实体 tick 通常 < 10 次三角函数），总开销 < 100 μs / tick，可接受。

### Q3: Unity 的 Burst Compiler 会破坏确定性吗？

**会**。Burst 默认启用 `-ffast-math`。**确定性代码路径禁止用 Burst**，即使性能有损失。Burst 只用于非权威的批处理（如远端实体位置插值批量计算）。

### Q4: `System.Math.Sin` 跨 .NET 版本一致吗？

**.NET 6+ 统一使用 `libsys` 中的实现**（基于 Intel 数学内核），**在相同 .NET 版本下跨 OS 是一致的**。但**跨 .NET 主版本不保证**——升级运行时主版本时 `Math.Sin` 的低位 bit 可能变化。Unity Mono 实现又是另一套。

Atlas 选择自研是**根治不确定性**，避免未来升级 .NET 版本或切换到 IL2CPP 时的惊喜。

### Q5: 服务端可以用不同 tick 频率吗？

**不可以在同一 session 内切换**。session 启动时确定 tick 频率，直到 session 结束。

跨 session 可以不同（开放世界 vs 副本），但两个 session 间不互相影响。

### Q6: 确定性代码可以用 `<algorithm>` 吗（sort、find 等）？

- `std::sort` / `std::stable_sort`：**允许**，前提是比较器确定性（避免浮点作 key）
- `std::find_if`：**允许**
- `std::shuffle`：**禁止**（需要 RNG，且标准库实现差异）
- `std::partition`：**允许**（`stable_partition` 更安全）

### Q7: `struct` 布局跨 C++/C# 一致吗？

**不能依赖**。跨语言数据交换必须通过 FlatBuffers / 显式序列化，**不能直接内存 memcpy**。即使同为 C++，跨编译器 struct 填充也可能不同。

### Q8: 发现确定性漏洞（某代码用了 `DateTime.Now`）怎么办？

**按严重程度处理**：
1. 发现在开发阶段：立即修复，加 CI 检查防止复发
2. 发现在上线后、未被利用：悄悄修复，下个版本上线
3. 发现在上线后、已被利用（作弊）：紧急 hotfix + 封号

**永远不要**觉得"影响小可以留着"——确定性漏洞会随时间累加，到后期牵一发动全身。

### Q9: 客户端 Unity 编辑器里调试时，可以暂时关掉确定性检查吗？

**不可以**。即使编辑期也必须保持，否则：
- "在编辑器没问题，上线却漂移"
- 本地测试用例失效
- 和解 bug 定位困难

可以提供 **`ATLAS_ASSUME_DETERMINISTIC` 宏**：启用时禁用某些运行时检查（减少开销），但代码路径完全相同。

### Q10: 确定性和 AI 的兼容性？

AI 决策常用随机（巡逻方向、攻击选择），**这些随机必须走 `DeterministicRng`**。
- 不能说 "AI 反正不精确，用 `UnityEngine.Random` 图方便"
- AI 回放同样是调试工具，没有确定性无法重现 boss bug

---

## 12. 实施里程碑

| 阶段 | 交付内容 |
|---|---|
| P0 周 1–2 | 建立 CMake/csproj 编译器规约；编写 CI 禁用 API 静态检查 |
| P0 周 3–4 | 实现 `DeterministicRng` C++ + C# 版本；跨语言等价性测试 |
| P0 周 5–6 | 实现 `Atlas.CombatCore.Math` 基础函数；测试对比参考实现 |
| P0 周 7–8 | Dual-run 框架 + Cross-platform CI；首次端同 10k tick 0 漂移 |
| P1+ | 每个新模块（移动/技能/buff）引入时跑确定性测试，不达标不合并 |

---

## 13. 文档维护

- **Owner**：Tech Lead（整体负责）+ 每小节指定负责人
- **评审频率**：新增禁用 API、新增平台、发现漂移事件后评审
- **相关文档**：
  - `OVERVIEW.md`（§4 Day-1 硬决策引用本文）
  - `02_sync/MOVEMENT_SYNC.md`（位置仿真依赖本文 §3 §6）
  - `03_combat/SKILL_SYSTEM.md`（技能执行依赖本文 §4 §5）
  - `09_tools/LOAD_TESTING.md`（端同验证工具链）

---

**文档结束。**

**核心纪律重申**：
1. **不可妥协**：确定性契约高于性能优化、开发便利、短期交付压力
2. **不可例外**：任何"暂时这样"的违规最终都会引发严重事故
3. **CI 为盾**：人工审查会漏，必须用自动化检查兜底
4. **禁用优于允许**：列不进"允许"就视为禁止，避免灰色地带
