# Phase 10: CellApp — 空间模拟

> 前置依赖: Phase 8 (BaseApp), Phase 9 (BaseAppMgr), Script Phase 4 (`[Replicated]` + DirtyFlags；Phase 10 需补充按受众过滤的 delta API)
> 协同依赖: [DEF_GENERATOR_DESIGN](../DEF_GENERATOR_DESIGN.md) 的 C++ 部分（Step 5-9: .def 解析、EntityDefRegistry 扩展、BaseApp 外部接口、Exposed 校验、sourceEntityID 验证）
> BigWorld 参考: `server/cellapp/cellapp.hpp`, `server/cellapp/entity.hpp`, `server/cellapp/witness.hpp`
> 当前代码基线 (2026-04-12): 仓库里还没有 `CellApp` 实现，`src/server/cellapp/` 只有 `CMakeLists.txt`。因此本阶段必须复用已经落地的 `BaseApp` 回程协议（`src/server/baseapp/baseapp_messages.hpp`），并以当前脚本层实际提供的复制接口为前提调整设计。

---

## 目标

实现 MMO 的核心——空间模拟引擎。CellApp 负责游戏世界的实时模拟：
实体在空间中的位置管理、RangeList 空间索引、AOI（兴趣范围）计算、
带宽感知的增量属性同步、Controller 行为系统。

**本阶段仅实现单 CellApp**（无 Real/Ghost），Phase 11 扩展为分布式多 CellApp。

## 验收标准

- [ ] CellApp 可启动（EntityApp 基类），注册到 machined，初始化 C# 脚本引擎
- [ ] Space 可创建/销毁，实体可在 Space 中创建/移动/销毁
- [ ] RangeList 双轴排序链表正确维护，移动操作 O(δ/密度) 接近 O(1)
- [ ] RangeTrigger 正确检测实体的 2D 进入/离开事件（双轴交叉校验）
- [ ] Witness/AOI 系统可工作：优先级堆驱动、带宽感知的增量同步
- [ ] `[Replicated]` DirtyFlags 驱动 AOI 属性更新，按 Scope 过滤
- [ ] Controller 系统支持 MoveToPoint / Timer / Proximity
- [ ] BaseApp 可请求创建/销毁 Cell 实体，Cell 回报地址给 Base
- [ ] 客户端 RPC（经 BaseApp 转发）正确分发到 Cell 实体 C# 脚本
- [ ] Cell 实体 `[ClientRpc]` 经 BaseApp Proxy 发送到客户端
- [ ] 1000 实体在单 Space 中 tick 性能达标（< 50ms @ 10Hz）
- [ ] 全部新增代码有单元测试

## 验收状态（2026-04-13）

- [ ] 当前仓库仍无 `CellApp` 实现；`src/server/cellapp/` 只有构建骨架与文档前置约束。

---

## 1. BigWorld 架构分析与 Atlas 适配

### 1.1 BigWorld CellApp 核心机制

| 机制 | BigWorld 实现 | 说明 |
|------|-------------|------|
| **类层次** | `CellApp : EntityApp : ScriptApp : ServerApp` | 带 Python + BgTaskManager |
| **Space/Cell** | Space 持有 RangeList + 所有实体; Cell 持有本 CellApp 的 Real 实体 | 一个 Space 可跨多 CellApp |
| **实体存储** | EntityPopulation (全局 map) + Cell::Entities (vector swap-back) + Space::entities | 三级索引 |
| **RangeList** | 双轴排序链表 (X + Z)，哨兵头尾 | `volatile float` 防浮点精度问题 |
| **RangeTrigger** | upper/lower 边界节点 + 2D 交叉检测 + 滞后老位置 | 先扩后缩避免假事件 |
| **Witness** | AoITrigger (RangeTrigger 子类) + EntityCache 优先级堆 | 距离/5+1 公式，带宽感知 |
| **Controller** | 20+ 子类, Domain (REAL/GHOST/BOTH) | 移动/定时/范围/导航/加速 |
| **Real/Ghost** | RealEntity 扩展 + Haunt 列表 + Ghost 只读副本 | Phase 11 实现 |
| **实体 Entity** | ~7800 LOC, PyObjectPlus | 位置/方向/Vehicle/Volatile/属性/事件历史 |

### 1.2 Atlas 适配决策

| 方面 | Atlas 决策 | 原因 |
|------|-----------|------|
| **类层次** | `CellApp : EntityApp` | 与 BaseApp 同级 |
| **Space/Cell** | 初期: Space = Cell（单 CellApp 管理整个 Space） | Phase 11 分离 |
| **实体存储** | 全局 map + Space 内 vector | 两级足够（无 Ghost） |
| **RangeList** | **完全对齐 BigWorld 算法** | 核心算法已验证，无需改动 |
| **RangeTrigger** | **完全对齐 BigWorld 算法** | 2D 交叉检测逻辑精妙，直接复用 |
| **Witness** | 对齐 BigWorld 优先级堆 + 带宽控制 | AOI 是性能关键路径 |
| **Controller** | 初期 3 个: MoveToPoint / Timer / Proximity | C# 可扩展更多 |
| **Real/Ghost** | 本阶段全部为 Real | Phase 11 加入 |
| **Entity** | C++ 薄壳 (~500 LOC) + C# 实例 | BigWorld 7800→Atlas 500 |

### 1.3 C# 脚本层的核心影响

**BigWorld (Python):**
```
Entity : PyObjectPlus
  → C++ 持有 Python 属性 dict
  → C++ 直接调用 onCreated() / onDestroy() / onWriteToDB()
  → 属性变更: C++ PropertyOwnerLink 追踪 → eventHistory
  → 客户端同步: Witness 遍历 eventHistory → 发送 delta
```

**Atlas (C#):**
```
CellEntity (C++ 薄壳)
  → C++ 只持有 GCHandle + position/direction
  → 属性管理完全在 C# (Source Generator DirtyFlags)
  → C# 每 tick 检查 IsDirty → 生成 owner/other 快照与 delta（需扩展当前生成器）→ NativeApi
  → C++ CellEntity 发布最新 replication frame；每个 Witness 仍各自保存“已发送到哪一版”的状态
```

**关键差异:**
- BigWorld: 属性变更在 C++ 层追踪（`PropertyOwnerLink` + `eventHistory`）
- Atlas: 属性变更在 C# 层追踪（`DirtyFlags`），C++ 只收到序列化好的 delta blob
- BigWorld: Witness 直接读 Entity 的 C++ 属性生成 delta，并在 `EntityCache` 中维护 `lastEventNumber_ / lastVolatileUpdateNumber_`
- Atlas: Witness 不解析属性内容，但仍要像 BigWorld 一样在每个 `EntityCache` 里维护“该观察者已经消费到哪一个 replication/event/volatile 序号”

**这意味着:**
- C++ 不需要 `eventHistory`（C# 管理）
- C++ 不需要 `PropertyOwnerLink`（C# DirtyFlags 替代）
- C++ 的 Witness 简化为：管理 `EntityCache` 的发送进度/状态、优先级调度、转发 blob
- 位置/方向仍在 C++ 管理（RangeList 需要直接访问）

---

## 2. 消息协议设计

### 2.1 协议分层

当前代码下，Phase 10 不能再定义一套与 `BaseApp` 现有处理器并行的
`CellApp → BaseApp` 回程协议。消息需要分成三层：

1. `3000-3099`：CellApp 自有消息，`BaseApp → CellApp` 请求和 CellApp 本地管理
2. `2010-2016`：复用已实现的 `BaseApp` 入站消息，由 CellApp 发回 BaseApp
3. `2022-2023`：BaseApp 外部接口扩展，接收客户端 RPC（对齐 DEF_GENERATOR_DESIGN Step 6）

### 2.2 CellApp 新增消息 (`3000-3099`)

| 消息 | ID | 方向 | 用途 |
|------|-----|------|------|
| `CreateCellEntity` | 3000 | BaseApp → CellApp | 创建 Cell 实体 |
| `DestroyCellEntity` | 3002 | BaseApp → CellApp | 销毁 Cell 实体 |
| `ClientCellRpcForward` | 3003 | BaseApp → CellApp | 客户端 Cell RPC 转发，携带 sourceEntityID（对应 BigWorld `runExposedMethod`） |
| `InternalCellRpc` | 3004 | BaseApp → CellApp | 服务器内部 Base→Cell RPC，无需 Exposed（对应 BigWorld `runScriptMethod`） |
| `CreateSpace` | 3010 | 管理/脚本 → CellApp | 创建 Space |
| `DestroySpace` | 3011 | 管理/脚本 → CellApp | 销毁 Space |
| `AvatarUpdate` | 3020 | BaseApp → CellApp | 客户端位置更新 |
| `EnableWitness` | 3021 | BaseApp → CellApp | 开启 AOI（客户端 enableEntities） |
| `DisableWitness` | 3022 | BaseApp → CellApp | 关闭 AOI |

> **为什么拆分两条 CellRpc 消息？**
> BigWorld 区分 `runExposedMethod`（客户端发起，REAL_ONLY，需要 Exposed 校验 + sourceEntityID 验证）
> 和 `runScriptMethod`（服务器内部 Base→Cell，REAL_ONLY，可调用所有 Cell 方法，无需 Exposed）。
> 参见 [BigWorld RPC 参考 Section 3.1 / 3.3](../BIGWORLD_RPC_REFERENCE.md)。
> 合并为一条消息会导致 CellApp 无法区分来源、安全校验逻辑混乱。

约束：
- 不再在 `3000` 段定义 `CreateCellEntityAck` / `BaseEntityRpc` / 泛化版 `ClientRpcFromCell` / `ReplicatedDelta` / `CellEntityPosition`
- `CreateCellEntity` 的成功路径直接复用 `baseapp::CellEntityCreated`
- 显式失败回包当前代码未定义；如果实现阶段确实需要失败通知，应新增独立失败消息，而不是覆盖现有 `2010-2016` 语义

### 2.2.1 BaseApp 外部接口扩展（客户端 RPC 接收）

DEF_GENERATOR_DESIGN Step 6 定义了两条新的 BaseApp 外部接口消息，用于接收客户端发起的 RPC：

| 消息 | ID | 方向 | 用途 | 状态 |
|------|-----|------|------|------|
| `ClientBaseRpc` | 2022 | Client → BaseApp | 客户端调用 Base 上的 exposed 方法 | **已实现** |
| `ClientCellRpc` | 2023 | Client → BaseApp | 客户端调用 Cell 上的 exposed 方法（经 BaseApp 转发到 CellApp） | **仅保留消息 ID** |

> **当前代码状态：**
> - `ClientBaseRpc` (2022)：struct 定义和 handler (`on_client_base_rpc`) 已实现于 `baseapp_messages.hpp` / `baseapp.cpp`
> - `ClientCellRpc` (2023)：仅在 `message_ids.hpp` 中保留了枚举值，**尚无 struct 定义和 handler**
> - 客户端侧 `client_native_provider.cpp::send_cell_rpc()` 目前是 stub（仅打日志）
>
> Phase 10 Step 10.9 需要新增 `ClientCellRpc` 的 struct 定义和 handler。

`ClientBaseRpc` 格式：`[rpc_id | payload]`（已实现）
- BaseApp 通过 `find_proxy_by_channel()` 确定调用者，天然只能调用自己的 Base
- 校验 `is_exposed()` 后直接分发给 C# 脚本

`ClientCellRpc` 格式：`[target_entity_id | rpc_id | payload]`（Phase 10 新增）
- `target_entity_id == proxy.entity_id()`：调用自己的 Cell（OWN_CLIENT / ALL_CLIENTS）
- `target_entity_id != proxy.entity_id()`：调用其他实体的 Cell（仅 ALL_CLIENTS）
- BaseApp 嵌入 `source_entity_id = proxy.entity_id()`（客户端不可伪造）后转发为 `ClientCellRpcForward`

> `ClientBaseRpc` 已定义在 `src/server/baseapp/baseapp_messages.hpp` 中，与 `Authenticate` (2020) / `AuthenticateResult` (2021) 属于同一外部接口段。
> `ClientCellRpc` 的 struct 定义需在 Phase 10 Step 10.9 中新增到同一文件。
> Phase 10 的 BaseApp 集成步骤（Step 10.9）负责实现这两条消息的处理器和 Exposed 校验逻辑。

### 2.3 复用现有 BaseApp 入站消息 (`CellApp → BaseApp`)

| 消息 | ID | 方向 | 当前代码语义 |
|------|-----|------|-------------|
| `CellEntityCreated` | 2010 | CellApp → BaseApp | Cell 创建成功，Base 记录 `cell_entity_id + cell_addr` |
| `CellEntityDestroyed` | 2011 | CellApp → BaseApp | Cell 已销毁，Base 清理路由 |
| `CurrentCell` | 2012 | CellApp → BaseApp | Phase 11 offload 后更新 Cell 地址 |
| `CellRpcForward` | 2013 | CellApp → BaseApp | Cell → Base RPC，Base 侧分发给脚本 |
| `SelfRpcFromCell` | 2014 | CellApp → BaseApp | 发给拥有者客户端，可靠路径 |
| `ReplicatedDeltaFromCell` | 2015 | CellApp → BaseApp | **Unreliable**。当前代码只透传到 `base_entity_id` 对应的单个客户端；Phase 10 用于 Volatile 位置更新（latest-wins，允许丢包） |
| `BroadcastRpcFromCell` | 2016 | CellApp → BaseApp | 消息定义支持 `otherClients/allClients`，但当前 `BaseApp` 实现仍只发给 `base_entity_id` 对应客户端，不能当作已完成的 AOI fan-out |

这部分消息定义已经存在于 `src/server/baseapp/baseapp_messages.hpp`，Phase 10 直接复用，不再在 CellApp 文档里重定义一份并行版本。

当前代码现实约束：
- `BaseApp::on_self_rpc_from_cell()` 可稳定发给单个 Proxy 客户端
- `BaseApp::on_replicated_delta_from_cell()` 也只会发给单个 Proxy，且外层消息 ID 当前固定为 `0xF001`
- `BaseApp::on_broadcast_rpc_from_cell()` 目前并不会根据 `target` 枚举遍历 AOI 观察者集合

因此，Phase 10 文档中的 AOI 下行闭环必须按下面两种路径之一实现，不能把现状写成”已支持广播”：
- 首选路径：`CellApp/Witness` 直接按”每个观察者一个回包”展开，下行时使用观察者自己的 `base_entity_id` 作为路由键发回 BaseApp
- 后续优化：扩展 BaseApp，使 `BroadcastRpcFromCell` 真正具备按观察者集合 fan-out 的能力

> **传输可靠性约束（关键）：**
> - `SelfRpcFromCell` (2014) — **Reliable**，用于：拥有者客户端 RPC、有序属性 delta (`event_seq`)
> - `ReplicatedDeltaFromCell` (2015) — **Unreliable**，仅用于：Volatile 位置/朝向更新（latest-wins，允许丢包）
> - `BroadcastRpcFromCell` (2016) — **Unreliable**，用于：广播 ClientRpc（后续优化阶段）
>
> 属性 delta 要求按 `event_seq` 有序到达（断档时需补发或退回 snapshot），因此**必须走 Reliable 路径**（`SelfRpcFromCell`），不能走 Unreliable 的 `ReplicatedDeltaFromCell`。Volatile 位置更新是 latest-wins 语义，允许丢包，适合走 Unreliable 路径。

### 2.3.1 Phase 10 首阶段的 `0xF001` 过渡协议

如果 Phase 10 不同步改造 BaseApp 的外部下行消息路由，那么
`ReplicatedDeltaFromCell` 的 payload 不能直接被写成“已经是
`EntityEnter` / `EntityPropertyUpdate` 等真实客户端消息”。

当前代码可闭环的首阶段方案是：
- `BaseApp::on_replicated_delta_from_cell()` 继续把外层消息 ID 固定为 `0xF001`
- `msg.delta` 内部承载一个 CellApp 定义的二级 envelope
- 客户端 / 测试桩先只识别 `0xF001`，再在包内按 `kind` 分发

建议的 envelope:

```cpp
enum class CellAoIEnvelopeKind : uint8_t {
    EntityEnter = 1,
    EntityLeave = 2,
    EntityPositionUpdate = 3,
    EntityPropertyUpdate = 4,
};

struct CellAoIEnvelope {
    CellAoIEnvelopeKind kind;
    EntityID public_entity_id;     // 当前阶段使用 base_entity_id
    std::vector<std::byte> payload;
};
```

约束：
- `SelfRpcFromCell` (Reliable) 用于：(1) 拥有者客户端 RPC；(2) 有序属性 delta（`event_seq`，不可丢包）
- `ReplicatedDeltaFromCell` (Unreliable) 仅用于：Volatile 位置/朝向更新（latest-wins，允许丢包）
- 属性 delta 的 `CellAoIEnvelope` (kind=EntityPropertyUpdate) **必须走 `SelfRpcFromCell`**，否则 `event_seq` 有序性无法保证
- Phase 12 若落地真实的 `10102/10103/10104/10111` 客户端协议，则应由 BaseApp 在下行前解包
  `CellAoIEnvelope`，或改用真实客户端消息 ID

### 2.4 新增消息的详细定义

```cpp
namespace atlas::cellapp {

struct CreateCellEntity {
    EntityID base_entity_id;       // BaseApp 上的 EntityID
    uint16_t type_id;
    SpaceID space_id;
    Vector3 position;
    Vector3 direction;
    Address base_addr;             // BaseApp 地址（回报用）
    uint32_t request_id;
    std::vector<std::byte> script_init_data;
    // BaseApp/脚本层提供的 Cell 初始化数据。CellApp 会把它与
    // space/base/position/direction/on_ground 等运行时字段合成为
    // 完整 restore blob，再传给 NativeCallbacks::RestoreEntity()。

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

// 客户端 Cell RPC 转发（对应 BigWorld runExposedMethod, REAL_ONLY）
// BaseApp 在 on_client_cell_rpc() 中校验 Exposed + 跨实体权限后构造此消息
struct ClientCellRpcForward {
    EntityID target_entity_id;     // 目标实体（base_entity_id 空间）
    EntityID source_entity_id;     // BaseApp 嵌入的 proxy.entity_id()，客户端不可伪造
    uint32_t rpc_id;
    std::vector<std::byte> payload;

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

// 服务器内部 Base→Cell RPC（对应 BigWorld runScriptMethod, REAL_ONLY）
// 不需要 Exposed 校验，Base 可调用所有 Cell 方法
struct InternalCellRpc {
    EntityID target_entity_id;     // 目标实体（base_entity_id 空间）
    uint32_t rpc_id;
    std::vector<std::byte> payload;

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

struct DestroyCellEntity {
    EntityID base_entity_id;

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

struct AvatarUpdate {
    EntityID cell_entity_id;
    Vector3 position;
    Vector3 direction;
    bool on_ground;

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

struct EnableWitness {
    EntityID cell_entity_id;
    float aoi_radius;

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

struct DisableWitness {
    EntityID cell_entity_id;

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

struct CreateSpace {
    SpaceID space_id;
    // 后续可扩展: 空间配置 (地图名, 尺寸等)

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

struct DestroySpace {
    SpaceID space_id;

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

} // namespace atlas::cellapp
```

回程消息 `CellEntityCreated` / `CellEntityDestroyed` / `CurrentCell` /
`CellRpcForward` / `SelfRpcFromCell` / `ReplicatedDeltaFromCell` /
`BroadcastRpcFromCell` 直接复用 `src/server/baseapp/baseapp_messages.hpp`。

---

## 3. 核心模块设计

### 3.1 RangeList — 双轴排序链表

**完全对齐 BigWorld 算法。** 这是经过 15 年 MMO 生产验证的核心数据结构。

```cpp
// src/lib/space/range_list_node.hpp
namespace atlas {

/// 节点排序优先级（同位置时的次序）
enum class RangeListOrder : uint16_t {
    Head        = 0,        // 哨兵头 (-FLT_MAX)
    Entity      = 100,      // 实体节点
    LowerBound  = 190,      // 触发器下界
    UpperBound  = 200,      // 触发器上界
    Tail        = 0xFFFF,   // 哨兵尾 (+FLT_MAX)
};

/// 节点标志（用于选择性交叉通知）
enum class RangeListFlags : uint8_t {
    None             = 0,
    EntityTrigger    = 0x01,   // 实体经过触发器
    LowerAoITrigger  = 0x02,   // 下界触发
    UpperAoITrigger  = 0x04,   // 上界触发
    IsEntity         = 0x10,   // 节点是实体
    IsLowerBound     = 0x20,   // 节点是下界
};

class RangeListNode {
public:
    virtual ~RangeListNode() = default;

    [[nodiscard]] virtual auto x() const -> float = 0;
    [[nodiscard]] virtual auto z() const -> float = 0;
    [[nodiscard]] virtual auto order() const -> RangeListOrder = 0;

    [[nodiscard]] auto wants_flags() const -> RangeListFlags { return wants_flags_; }
    [[nodiscard]] auto makes_flags() const -> RangeListFlags { return makes_flags_; }

    // 双轴链表指针
    RangeListNode* prev_x = nullptr;
    RangeListNode* next_x = nullptr;
    RangeListNode* prev_z = nullptr;
    RangeListNode* next_z = nullptr;

protected:
    RangeListFlags wants_flags_ = RangeListFlags::None;
    RangeListFlags makes_flags_ = RangeListFlags::None;

    /// 交叉回调（子类覆盖）
    virtual void on_crossed_x(RangeListNode& other, bool positive) {}
    virtual void on_crossed_z(RangeListNode& other, bool positive) {}

    friend class RangeList;
};

class FixedRangeListNode final : public RangeListNode {
public:
    FixedRangeListNode(float x, float z, RangeListOrder order) : x_(x), z_(z), order_(order) {}

    [[nodiscard]] auto x() const -> float override { return x_; }
    [[nodiscard]] auto z() const -> float override { return z_; }
    [[nodiscard]] auto order() const -> RangeListOrder override { return order_; }

private:
    float x_;
    float z_;
    RangeListOrder order_;
};

} // namespace atlas
```

```cpp
// src/lib/space/range_list.hpp
namespace atlas {

class RangeList {
public:
    RangeList();
    ~RangeList();

    /// 插入节点（初始位置 -FLT_MAX，然后 shuffle 到正确位置）
    void insert(RangeListNode* node);

    /// 移除节点
    void remove(RangeListNode* node);

    /// 节点位置变更后重新排序
    /// 参数 old_x / old_z 用于触发器的 2D 交叉检测
    void shuffle_x_then_z(RangeListNode* node, float old_x, float old_z);

private:
    /// X 轴冒泡排序
    void shuffle_x(RangeListNode* node, float old_x);
    /// Z 轴冒泡排序
    void shuffle_z(RangeListNode* node, float old_z);

    /// 哨兵节点
    FixedRangeListNode head_{-FLT_MAX, -FLT_MAX, RangeListOrder::Head};
    FixedRangeListNode tail_{+FLT_MAX, +FLT_MAX, RangeListOrder::Tail};
};

} // namespace atlas
```

**shuffle 算法（对齐 BigWorld）:**

```
shuffle_x(node, old_x):
    our_x = node->x()

    // 向左冒泡
    while (our_x < prev->x()  ||
           (our_x == prev->x() && node->order() < prev->order())):
        // 交叉通知（如果双方 flags 匹配）
        if (should_notify(node, prev)):
            node->on_crossed_x(prev, /*positive=*/false)
            prev->on_crossed_x(node, /*positive=*/true)
        // 交换链表位置
        unlink(node); link_before(node, prev)
        prev = node->prev_x

    // 向右冒泡（对称逻辑）
    while (our_x > next->x()  ||
           (our_x == next->x() && node->order() > next->order())):
        ...
```

**关键: 2D 交叉检测（对齐 BigWorld `RangeTriggerNode::crossedXEntity`）:**

X 轴 shuffle 时检测 Z 轴旧位置，Z 轴 shuffle 时检测 X 轴新位置：
- X 正向交叉 → 检查 entity 的 **oldZ** 是否在触发器 Z 范围内
- Z 正向交叉 → 检查 entity 的 **newX**（已 shuffle 后）是否在触发器 X 范围内
- 这保证了 2D 区域检测的正确性

### 3.2 RangeTrigger — AOI 触发器

```cpp
// src/lib/space/range_trigger.hpp
namespace atlas {

class RangeTrigger {
public:
    RangeTrigger(RangeListNode& central, float range);

    void insert(RangeList& list);
    void remove(RangeList& list);
    void remove_without_contracting();

    /// 范围变更（AOI 半径调整）
    void set_range(float new_range);

    /// 中心节点移动后更新边界
    void update(float old_x, float old_z);
    void shuffle_x_then_z_expand(bool x_inc, bool z_inc, float old_x, float old_z);
    void shuffle_x_then_z_contract(bool x_inc, bool z_inc, float old_x, float old_z);

    /// 子类覆盖
    virtual void on_enter(CellEntity& entity) = 0;
    virtual void on_leave(CellEntity& entity) = 0;

private:
    RangeListNode& central_;
    RangeTriggerNode upper_bound_;
    RangeTriggerNode lower_bound_;

    /// 旧位置（用于 2D 交叉检测）
    float old_entity_x_ = 0.0f;
    float old_entity_z_ = 0.0f;
};

/// AOI 触发器（Witness 使用）
class AoITrigger : public RangeTrigger {
public:
    explicit AoITrigger(Witness& owner, float radius);

    void on_enter(CellEntity& entity) override;
    void on_leave(CellEntity& entity) override;

private:
    Witness& owner_;
};

} // namespace atlas
```

### 3.3 CellEntity — Cell 侧实体

```cpp
// src/server/cellapp/cell_entity.hpp
namespace atlas {

class CellEntity {
public:
    CellEntity(EntityID id, uint16_t type_id, Space& space);
    ~CellEntity();

    // ========== 标识 ==========
    [[nodiscard]] auto id() const -> EntityID { return id_; }
    [[nodiscard]] auto type_id() const -> uint16_t { return type_id_; }

    // ========== 位置/方向 ==========
    [[nodiscard]] auto position() const -> const Vector3& { return position_; }
    [[nodiscard]] auto direction() const -> const Vector3& { return direction_; }

    /// 设置位置（触发 RangeList shuffle + Witness 通知）
    void set_position(const Vector3& pos);
    void set_direction(const Vector3& dir);
    void set_position_and_direction(const Vector3& pos, const Vector3& dir);

    [[nodiscard]] auto is_on_ground() const -> bool { return on_ground_; }
    void set_on_ground(bool v) { on_ground_ = v; }

    // ========== 空间 ==========
    [[nodiscard]] auto space() -> Space& { return space_; }

    // ========== 脚本 ==========
    void set_script_handle(uint64_t h) { script_handle_ = h; }
    [[nodiscard]] auto script_handle() const -> uint64_t { return script_handle_; }

    // ========== Base 关联 ==========
    [[nodiscard]] auto base_addr() const -> const Address& { return base_addr_; }
    [[nodiscard]] auto base_entity_id() const -> EntityID { return base_entity_id_; }
    void set_base(const Address& addr, EntityID base_id);

    // ========== Witness ==========
    [[nodiscard]] auto has_witness() const -> bool { return witness_ != nullptr; }
    [[nodiscard]] auto witness() -> Witness* { return witness_.get(); }
    void enable_witness(float aoi_radius);
    void disable_witness();

    // ========== Controller ==========
    auto add_controller(std::unique_ptr<Controller> ctrl, int user_arg = 0)
        -> ControllerID;
    auto cancel_controller(ControllerID id) -> bool;

    // ========== 复制帧 ==========
    /// 不能使用单消费者的 pending queue，也不能只保留“最新一帧”。
    /// CellEntity 需要同时保存“最新快照 + 有界 delta 历史”，供慢观察者补包。
    struct ReplicationFrame {
        uint64_t event_seq = 0;              // 本条属性 delta 的序号
        uint64_t volatile_seq = 0;           // 本条位置/朝向更新的序号
        std::vector<std::byte> owner_delta;
        std::vector<std::byte> other_delta;
        Vector3 position;
        Vector3 direction;
        bool on_ground = false;
    };

    struct ReplicationState {
        uint64_t latest_event_seq = 0;
        uint64_t latest_volatile_seq = 0;
        std::vector<std::byte> owner_snapshot;
        std::vector<std::byte> other_snapshot;
        std::deque<ReplicationFrame> history;  // 固定窗口，例如最近 8~16 帧
    };
    void publish_replication_frame(ReplicationFrame frame);
    [[nodiscard]] auto replication_state() const -> const ReplicationState*;

    // ========== RangeList 节点 ==========
    [[nodiscard]] auto range_node() -> EntityRangeListNode& { return range_node_; }

    // ========== 销毁 ==========
    [[nodiscard]] auto is_destroyed() const -> bool { return destroyed_; }
    void destroy();

private:
    EntityID id_;
    uint16_t type_id_;
    Vector3 position_;
    Vector3 direction_;
    bool on_ground_ = false;

    Space& space_;
    uint64_t script_handle_ = 0;

    // Base 关联
    Address base_addr_;
    EntityID base_entity_id_ = 0;

    // 组件
    std::unique_ptr<Witness> witness_;
    Controllers controllers_;
    EntityRangeListNode range_node_;

    // 最新快照 + 有界 delta 历史（供所有 Witness 只读共享）
    std::optional<ReplicationState> replication_state_;

    bool destroyed_ = false;
};

} // namespace atlas
```

**与 BigWorld Entity 的对比:**

| BigWorld Entity (~7800 LOC) | Atlas CellEntity (~500 LOC) | 差异 |
|---|---|---|
| `PyObjectPlus` 基类 | `uint64_t script_handle_` | C# GCHandle |
| `properties_` (vector\<ScriptObject\>) | 无（C# 管理） | 属性全在 C# |
| `eventHistory_` / `PropertyOwnerLink` | `replication_state_` + `EntityCache` 发送序号 | C# DirtyFlags 产出 blob；每个 Witness 仍维护自己的消费进度 |
| `pVehicle_` / Vehicle 系统 | 初期不实现 | 后续 |
| `pChunk_` / Chunk 空间 | 初期不实现 | 后续 |
| Real/Ghost 双模式 | 仅 Real（Phase 11 扩展） | 渐进 |
| `pControllers_` (20+ 子类) | `Controllers` (3 个初始子类) | C# 可扩展 |

### 3.4 Witness — AOI 管理器

```cpp
// src/server/cellapp/witness.hpp
namespace atlas {

class Witness {
public:
    Witness(CellEntity& owner, float aoi_radius);
    ~Witness();

    /// 每 tick 末尾调用: 生成并发送客户端更新
    void update(uint32_t max_packet_bytes);

    /// AOI 半径
    [[nodiscard]] auto aoi_radius() const -> float;
    void set_aoi_radius(float radius);

    // ---- AoITrigger 回调 ----
    void add_to_aoi(CellEntity* entity);
    void remove_from_aoi(CellEntity* entity);

private:
    // ---- EntityCache ----
    struct EntityCache {
        CellEntity* entity;
        double priority = 0.0;        // 距离/5 + 1（越小越优先）
        uint8_t flags = 0;
        uint64_t last_event_seq = 0;
        uint64_t last_volatile_seq = 0;

        // 标志
        static constexpr uint8_t ENTER_PENDING  = 0x01;
        static constexpr uint8_t GONE           = 0x08;
        static constexpr uint8_t REFRESH        = 0x10;

        [[nodiscard]] auto is_updatable() const -> bool {
            return (flags & (ENTER_PENDING | GONE | REFRESH)) == 0;
        }

        void update_priority(const Vector3& origin) {
            float dist = (entity->position() - origin).length();
            priority = dist / 5.0 + 1.0;
        }
    };

    /// 处理状态转换 (ENTER_PENDING / REFRESH / GONE)
    void handle_state_change(EntityCache& cache);

    /// 发送单个实体的更新（根据该观察者的 last_event_seq / last_volatile_seq 决定是否发送）
    void send_entity_update(EntityCache& cache);

    CellEntity& owner_;
    std::unique_ptr<AoITrigger> trigger_;

    /// EntityID → EntityCache
    std::unordered_map<EntityID, EntityCache> aoi_map_;

    /// 优先级堆（min-heap，priority 最小的先出）
    std::vector<EntityCache*> priority_queue_;

    /// 带宽赤字追踪
    int bandwidth_deficit_ = 0;
};

} // namespace atlas
```

**Witness::update() 流程（按 BigWorld 的 `EntityCache` 思路适配）:**

```
Witness::update(max_packet_bytes):

  PHASE 1: 堆维护
    rebuild priority_queue_ from aoi_map_ (if needed)
    make_heap (min-heap by priority)

  PHASE 2: 状态转换
    遍历堆，处理 ENTER_PENDING / REFRESH / GONE:
      ENTER_PENDING:
        发送 EntityEnter (type_id, position, [AllClients] 全量快照)
        记录 cache.last_event_seq = state.latest_event_seq
        记录 cache.last_volatile_seq = state.latest_volatile_seq
        清除 ENTER_PENDING 标志
      REFRESH:
        重新发送快照（类似 BigWorld 的 refresh/create path）
        更新 last_event_seq = state.latest_event_seq
        更新 last_volatile_seq = state.latest_volatile_seq
        清除 REFRESH 标志
      GONE:
        发送 EntityLeave (entity_id)
        从 aoi_map_ 移除

  PHASE 3: 属性/位置更新
    bytes_sent = 0
    max_priority = front.priority + MAX_PRIORITY_DELTA

    while (堆非空 && front.priority < max_priority
           && bytes_sent < max_packet_bytes - deficit):

        pop_heap → cache
        if cache.is_updatable():
            send_entity_update(cache):
                1. 位置更新 (Volatile: position + direction)
                   - 读取 entity->replication_state()
                   - 若 state.latest_volatile_seq > cache.last_volatile_seq：
                     直接发送 history 中最新那条 absolute volatile update
                   - volatile 是 latest-wins，不要求补发中间每一帧
                2. 属性 delta (读取 entity->replication_state())
                   - 若 cache.last_event_seq == state.latest_event_seq：本 tick 无属性包
                   - 若 history 中存在从 cache.last_event_seq + 1 到 latest_event_seq 的连续 delta：
                     依序补发这些 delta，并推进 cache.last_event_seq
                   - 若出现序号断档，或观察者落后到 history 窗口之外：
                     发送 owner_snapshot / other_snapshot，并把 cache 标记为 REFRESH 完成
                   - 当前仓库的脚本生成器还没有 owner/other 的“快照 + 增量”双接口，
                     Phase 10 需要先扩展 Script Phase 4
                3. 累计 bytes_sent

            cache.update_priority(owner_.position())

        push_heap(cache)  // 更新优先级后放回

  PHASE 4: 带宽赤字
    deficit = max(0, bytes_sent - max_packet_bytes)
    下次 update 时扣减 deficit
```

### 3.5 Controller 系统

```cpp
// src/lib/space/controller.hpp
namespace atlas {

using ControllerID = uint32_t;

class Controller {
public:
    virtual ~Controller() = default;

    auto id() const -> ControllerID { return id_; }
    auto entity() -> CellEntity& { return *entity_; }
    auto user_arg() const -> int { return user_arg_; }

    virtual void start() {}
    virtual void update(float dt) = 0;
    virtual void stop() {}

    [[nodiscard]] auto is_finished() const -> bool { return finished_; }

protected:
    void finish();        // 标记完成, 通知 C# 回调

private:
    friend class Controllers;
    ControllerID id_ = 0;
    CellEntity* entity_ = nullptr;
    int user_arg_ = 0;
    bool finished_ = false;
};

} // namespace atlas
```

```cpp
// src/lib/space/controllers.hpp
namespace atlas {

class Controllers {
public:
    auto add(std::unique_ptr<Controller> ctrl, CellEntity* entity, int user_arg)
        -> ControllerID;
    auto cancel(ControllerID id, CellEntity* entity) -> bool;
    void update(float dt);
    void stop_all();

    [[nodiscard]] auto count() const -> size_t;

private:
    std::unordered_map<ControllerID, std::unique_ptr<Controller>> controllers_;
    ControllerID next_id_ = 1;
    bool in_update_ = false;
    std::vector<ControllerID> pending_cancel_;
};

} // namespace atlas
```

**初始 Controller 子类:**

```cpp
// src/lib/space/move_controller.hpp
class MoveToPointController : public Controller {
public:
    MoveToPointController(const Vector3& dest, float speed, bool face_movement);
    void update(float dt) override;
private:
    Vector3 destination_;
    float speed_;
    bool face_movement_;
};

// src/lib/space/timer_controller.hpp
class TimerController : public Controller {
public:
    TimerController(float interval, bool repeat);
    void update(float dt) override;
private:
    float interval_;
    float elapsed_ = 0.0f;
    bool repeat_;
};

// src/lib/space/proximity_controller.hpp
class ProximityController : public Controller {
public:
    ProximityController(float range);
    void start() override;  // 插入 RangeTrigger
    void update(float dt) override {}  // 无 tick（事件驱动）
    void stop() override;   // 移除 RangeTrigger
private:
    float range_;
    std::unique_ptr<ProximityTrigger> trigger_;
};
```

### 3.6 Space — 游戏空间

```cpp
// src/server/cellapp/space.hpp
namespace atlas {

using SpaceID = uint32_t;

class Space {
public:
    explicit Space(SpaceID id);
    ~Space();

    [[nodiscard]] auto id() const -> SpaceID { return id_; }

    // ---- 实体管理 ----
    auto add_entity(std::unique_ptr<CellEntity> entity) -> CellEntity*;
    void remove_entity(EntityID id);
    auto find_entity(EntityID id) -> CellEntity*;
    [[nodiscard]] auto entity_count() const -> size_t;

    // ---- 空间索引 ----
    [[nodiscard]] auto range_list() -> RangeList& { return range_list_; }

    // ---- Tick ----
    void tick(float dt);

    // ---- 遍历 ----
    template<typename Fn>
    void for_each_entity(Fn&& fn);

private:
    SpaceID id_;
    std::unordered_map<EntityID, std::unique_ptr<CellEntity>> entities_;
    RangeList range_list_;
};

} // namespace atlas
```

### 3.7 CellApp 进程

```cpp
// src/server/cellapp/cellapp.hpp
namespace atlas {

class CellApp : public EntityApp {
public:
    CellApp(EventDispatcher& dispatcher, NetworkInterface& network);

    [[nodiscard]] auto spaces() -> std::unordered_map<SpaceID, std::unique_ptr<Space>>&
        { return spaces_; }
    auto find_entity(EntityID id) -> CellEntity*;

    /// 通过 base_entity_id 查找实体（RPC 路由使用）
    auto find_entity_by_base_id(EntityID base_entity_id) -> CellEntity*;

protected:
    auto init(int argc, char* argv[]) -> bool override;
    void fini() override;
    void on_end_of_tick() override;
    void on_tick_complete() override;
    void register_watchers() override;

    auto create_native_provider() -> std::unique_ptr<INativeApiProvider> override;
    void on_script_ready() override;

private:
    // ---- 消息处理器 ----
    void on_create_cell_entity(const Address& src, Channel* ch,
                                const cellapp::CreateCellEntity& msg);
    void on_destroy_cell_entity(const Address& src, Channel* ch,
                                 const cellapp::DestroyCellEntity& msg);

    // 客户端 Cell RPC（经 BaseApp 转发），需要 Exposed + sourceEntityID 校验
    // 对应 BigWorld Entity::runExposedMethod()
    void on_client_cell_rpc_forward(const Address& src, Channel* ch,
                                     const cellapp::ClientCellRpcForward& msg);

    // 服务器内部 Base→Cell RPC，无需 Exposed 校验
    // 对应 BigWorld Entity::runScriptMethod()
    void on_internal_cell_rpc(const Address& src, Channel* ch,
                               const cellapp::InternalCellRpc& msg);

    void on_avatar_update(const Address& src, Channel* ch,
                           const cellapp::AvatarUpdate& msg);
    void on_enable_witness(const Address& src, Channel* ch,
                            const cellapp::EnableWitness& msg);
    void on_disable_witness(const Address& src, Channel* ch,
                             const cellapp::DisableWitness& msg);
    void on_create_space(const Address& src, Channel* ch,
                          const cellapp::CreateSpace& msg);

    // ---- Tick ----
    void tick_controllers(float dt);
    void tick_witnesses();

    // ---- 组件 ----
    std::unordered_map<SpaceID, std::unique_ptr<Space>> spaces_;

    // cell_entity_id → CellEntity* 索引（CellApp 内部路由）
    std::unordered_map<EntityID, CellEntity*> entity_population_;

    // base_entity_id → CellEntity* 索引（RPC 路由，因为客户端和 BaseApp 使用 base_entity_id）
    std::unordered_map<EntityID, CellEntity*> base_entity_population_;

    EntityID next_entity_id_ = 1;
};

} // namespace atlas
```

**Tick 流程（对齐 BigWorld）:**

```
CellApp tick 序列:

on_end_of_tick():                    // game_clock tick 后, Updatable 前
    (预留: Phase 11 inter-cell channel flush)

ScriptEngine::on_tick(dt):           // C# tick (在 ScriptApp::on_tick_complete 中)
    C# 实体逻辑
    C# 检查 DirtyFlags → 调用 NativeApi → CellEntity::publish_replication_frame()
    CellEntity 维护 latest snapshot + bounded history

on_tick_complete():                  // Updatable 之后
    tick_controllers(dt)             // 所有 Controller::update()
    tick_witnesses()                 // 所有 Witness::update()
        for each space:
            for each entity with witness:
                witness->update(max_packet_bytes)
                // → 属性 delta (event_seq): 逐观察者走 SelfRpcFromCell (Reliable)
                // → Volatile 位置更新: 逐观察者走 ReplicatedDeltaFromCell (Unreliable)
                // → 如后续补齐 BaseApp observer fan-out，再引入 BroadcastRpcFromCell 优化
                // → 发送到 BaseApp Proxy
```

### 3.8 CellApp RPC 安全校验（对齐 DEF_GENERATOR Step 7-9）

CellApp 的两条 RPC 入站路径对应 BigWorld 的双消息设计：

#### 3.8.1 on_client_cell_rpc_forward — 客户端发起的 Cell RPC

对应 BigWorld `Entity::runExposedMethod()`，必须执行完整的安全校验链：

```cpp
void CellApp::on_client_cell_rpc_forward(const Address& src, Channel* ch,
                                          const cellapp::ClientCellRpcForward& msg)
{
    auto* entity = find_entity_by_base_id(msg.target_entity_id);
    if (!entity) return;

    // Ghost 透明转发（Phase 11）：
    // if (!entity->is_real()) { entity->forward_to_real(...); return; }

    auto* rpc_desc = entity_def_registry_.find_rpc(msg.rpc_id);
    if (!rpc_desc || rpc_desc->exposed == ExposedScope::None)
    {
        // 不应到达——BaseApp 应已拦截非 exposed RPC
        ATLAS_LOG_WARNING("CellApp: non-exposed RPC {} reached cell, dropping", msg.rpc_id);
        return;
    }

    // 方向校验：确保是 Cell 方向 (0x02) 的 RPC，防止 Base RPC 被错误路由到 CellApp
    if (rpc_desc->direction() != 0x02)
    {
        ATLAS_LOG_WARNING("CellApp: RPC 0x{:06X} direction {} != CellRpc(0x02), dropping",
                          msg.rpc_id, rpc_desc->direction());
        return;
    }

    // OWN_CLIENT 方法：验证调用者身份（对齐 BigWorld Section 4.5）
    if (rpc_desc->exposed == ExposedScope::OwnClient)
    {
        if (msg.target_entity_id != msg.source_entity_id)
        {
            ATLAS_LOG_WARNING("Blocked cell RPC: source {} != target {}, "
                              "method is exposed as OWN_CLIENT",
                              msg.source_entity_id, msg.target_entity_id);
            return;
        }
    }
    // ALL_CLIENTS: 不验证 sourceEntityID（设计如此——任何 AoI 内客户端可调用）

    script_engine().dispatch_cell_rpc(entity->id(), msg.rpc_id, msg.payload);
}
```

> **安全保障（四层纵深，对齐 BigWorld Section 4）：**
> 1. **BaseApp 第一层**：`on_client_cell_rpc()` 检查 `is_exposed(rpc_id)`，非 exposed 直接丢弃
> 2. **BaseApp 第二层**：跨实体调用检查 `target != proxy && exposed != AllClients` → 丢弃
> 3. **CellApp 第三层**：方向校验 `rpc_desc->direction() == 0x02`，防止 Base RPC 被错误路由
> 4. **CellApp 第四层**：OwnClient 方法验证 `source == target`，source 由 BaseApp 嵌入不可伪造

#### 3.8.2 on_internal_cell_rpc — 服务器内部 Base→Cell RPC

对应 BigWorld `Entity::runScriptMethod()`，来自可信的 BaseApp，无需校验：

```cpp
void CellApp::on_internal_cell_rpc(const Address& src, Channel* ch,
                                    const cellapp::InternalCellRpc& msg)
{
    auto* entity = find_entity_by_base_id(msg.target_entity_id);
    if (!entity) return;

    // Ghost 透明转发（Phase 11）：
    // if (!entity->is_real()) { entity->forward_to_real(...); return; }

    // 内部 RPC 不需要 Exposed 校验——Base 可调用所有 Cell 方法
    script_engine().dispatch_cell_rpc(entity->id(), msg.rpc_id, msg.payload);
}
```

### 3.9 CellAppNativeProvider

```cpp
// src/server/cellapp/cellapp_native_provider.hpp
namespace atlas {

class CellAppNativeProvider : public BaseNativeProvider {
public:
    explicit CellAppNativeProvider(CellApp& app);

    double server_time() override;
    float delta_time() override;
    uint8_t get_process_prefix() override {
        return static_cast<uint8_t>(ProcessType::CellApp);
    }

    // C# entity.Client.ShowDamage() → 这里 → 经 BaseApp Proxy 发客户端
    void send_client_rpc(uint32_t entity_id, uint32_t rpc_id,
                         uint8_t target,
                         const std::byte* payload, int32_t len) override;

    // C# entity.Base.SomeMethod() → 这里 → 发给 BaseApp
    void send_base_rpc(uint32_t entity_id, uint32_t rpc_id,
                       const std::byte* payload, int32_t len) override;

    // C# 属性变更 → 发布本 tick 的复制帧
    void publish_replication_frame(uint32_t entity_id,
                                   uint64_t event_seq,
                                   uint64_t volatile_seq,
                                   const std::byte* owner_snapshot, int32_t owner_snapshot_len,
                                   const std::byte* other_snapshot, int32_t other_snapshot_len,
                                   const std::byte* owner_delta, int32_t owner_len,
                                   const std::byte* other_delta, int32_t other_len);

    // Entity types
    void register_entity_type(const std::byte* data, int32_t len) override;
    void unregister_all_entity_types() override;

private:
    CellApp& app_;
};

} // namespace atlas
```

---

## 4. 端到端流程

### 4.1 BaseApp 请求创建 Cell 实体

```
BaseApp                        CellApp                     C# (Cell Script)
  │                               │                            │
  │── CreateCellEntity ──────────→│                            │
  │   (base_id, type, space,      │                            │
  │    position, base_addr)       │                            │
  │                               │ allocate CellEntity ID     │
  │                               │ space.add_entity()         │
  │                               │ range_list.insert()        │
  │                               │                            │
  │                               │ compose full restore blob  │
  │                               │ (space_id/base_entity_id/  │
  │                               │  position/direction/       │
  │                               │  on_ground/script_init_data)│
  │                               │── NativeCallbacks ────────→│
  │                               │   RestoreEntity(cell_id,   │
  │                               │    type_id, 0, blob, len)  │
  │                               │   Deserialize(blob)        │
  │                               │   OnInit(false)            │
  │                               │                            │
  │←── CellEntityCreated ────────│                            │
  │   (base_id, cell_id, addr)   │                            │
  │                               │                            │
  │ base_entity.set_cell(addr,id)│                            │
```

约束：
- `RestoreEntity()` 调用前，CellApp 必须先合成完整初始化 blob；不能传 `null, 0`
- 这样 C# 实体的 `OnInit(false)` 才能在第一次执行前看到正确的 `space/base/position` 初始态

### 4.2 客户端 → Cell RPC（安全校验全链路）

对齐 DEF_GENERATOR_DESIGN Section 6.2，展示 Client→Cell RPC 的完整安全校验链：

```
Client                    BaseApp                         CellApp
  │                         │                               │
  │── ClientCellRpc ──────→│                               │
  │   (target_entity_id,    │                               │
  │    rpc_id, payload)     │                               │
  │                         │                               │
  │                         ├─ 1. find_proxy_by_channel()   │
  │                         ├─ 2. entity_def_registry_      │
  │                         │     .find_rpc(rpc_id)         │
  │                         ├─ 3. is_exposed? ──No──→ DROP  │
  │                         ├─ 4. target != proxy &&        │
  │                         │     exposed != AllClients?    │
  │                         │     ──Yes──→ DROP             │
  │                         ├─ 5. embed source_entity_id    │
  │                         │     = proxy.entity_id()       │
  │                         │     (客户端不可伪造)            │
  │                         │                               │
  │                         ├── ClientCellRpcForward ──────→│
  │                         │   (target, source, rpc_id,    │
  │                         │    payload)                   │
  │                         │                               │
  │                         │                               ├─ 6. find_entity_by_base_id(target)
  │                         │                               ├─ 7. Ghost? → forward (Phase 11)
  │                         │                               ├─ 8. OWN_CLIENT?
  │                         │                               │     source == target?
  │                         │                               │     ──No──→ DROP
  │                         │                               ├─ 9. dispatch_cell_rpc(...)
  │                         │                               │     → C# partial 方法执行
```

> **与 BigWorld 的对照（参见 BIGWORLD_RPC_REFERENCE Section 3.1）：**
> - 步骤 1-5 对应 BigWorld `Proxy::cellEntityMethod()` (`baseapp/proxy.cpp:2491-2530`)
> - 步骤 6-9 对应 BigWorld `Entity::runMethodHelper(isExposed=true)` (`cellapp/entity.cpp:5418-5508`)
> - `source_entity_id` 由 BaseApp 写入，等同于 BigWorld `b << id_` (`proxy.cpp:2527`)

### 4.3 客户端移动 → AOI 更新

```
Client          BaseApp          CellApp (tick)           Other Clients
  │                │                  │                        │
  │── position ──→│                  │                        │
  │               │── AvatarUpdate ─→│                        │
  │               │                  │ entity.set_position()  │
  │               │                  │ → range_list shuffle   │
  │               │                  │ → AoITrigger 检测      │
  │               │                  │                        │
  │               │                  │ [tick_witnesses()]     │
  │               │                  │ Witness::update()      │
  │               │                  │ → 优先级堆调度          │
  │               │                  │ → 对每个观察者逐个展开   │
  │               │                  │   客户端消息 payload     │
  │               │                  │   (EntityEnter/Leave/    │
  │               │                  │    EntityPropertyUpdate/ │
  │               │                  │    EntityPositionUpdate) │
  │               │                  │ → 以“观察者 base_entity_id”│
  │               │                  │   为路由键回送 BaseApp   │
  │               │                  │   SelfRpcFromCell /      │
  │               │                  │   ReplicatedDeltaFromCell│
  │               │                  │   (outer msg id = 0xF001 │
  │               │                  │    on BaseApp path, inner │
  │               │                  │    payload = CellAoIEnvelope)
  │               │                  │                        │
  │               │←── 逐观察者消息 ─│                        │
  │               │ SelfRpc / ReplicatedDelta│                 │
  │               │                  │                        │
  │               │── 转发到客户端 ──────────────────────────→│
```

说明：
- 这是与当前 `BaseApp` 实现真正闭环的 Phase 10 路径
- `BroadcastRpcFromCell` 保留为后续优化接口，但当前不应承担“AOI 广播已实现”的设计语义

---

## 5. 实现步骤

### Step 10.1: RangeList 核心算法

**新增文件:**
```
src/lib/space/
├── CMakeLists.txt
├── range_list_node.hpp
├── range_list.hpp / .cpp
tests/unit/test_range_list.cpp
```

**测试用例（关键）:**
- 插入 → 节点正确排序 (X + Z)
- 移动 → shuffle 后顺序仍正确
- 大量随机插入/移动 → 排序不变量
- 哨兵边界 (FLT_MAX)
- 同位置不同 order 的排序
- 性能: 1000 节点 shuffle < 1ms

### Step 10.2: RangeTrigger 2D 检测

**新增文件:**
```
src/lib/space/
├── range_trigger.hpp / .cpp
├── range_trigger_node.hpp / .cpp
├── entity_range_list_node.hpp / .cpp
tests/unit/test_range_trigger.cpp
```

**测试用例（关键）:**
- 实体进入触发器 2D 范围 → `on_enter` 调用
- 实体离开触发器 2D 范围 → `on_leave` 调用
- 对角线穿过 → 正确检测（不是只有 X 或 Z 方向）
- 触发器移动 → 进入/离开新范围内的实体
- 先扩后缩 → 无假事件
- 范围变更 → 正确重新检测
- 高频小移动 → 无漏检

### Step 10.3: Controller 系统

**新增文件:**
```
src/lib/space/
├── controller.hpp / .cpp
├── controllers.hpp / .cpp
├── move_controller.hpp / .cpp
├── timer_controller.hpp / .cpp
├── proximity_controller.hpp / .cpp
tests/unit/test_controllers.cpp
```

**测试用例:**
- MoveToPoint: 移动到目标 → finish 回调
- MoveToPoint: 面向移动方向
- Timer: 单次/重复触发
- Proximity: 实体进入/离开范围回调
- update 中 cancel → 安全
- 多 Controller 同时 tick

### Step 10.4: Space + CellEntity

**新增文件:**
```
src/server/cellapp/
├── space.hpp / .cpp
├── cell_entity.hpp / .cpp
tests/unit/test_space.cpp
tests/unit/test_cell_entity.cpp
```

**测试用例:**
- Space 创建/销毁
- 实体添加/移除
- 位置设置 → RangeList 更新
- `publish_replication_frame()` → `replication_state()` 历史窗口轮转
- 实体销毁 → 从 Space + RangeList 移除

### Step 10.5: Witness / AOI

**新增文件:**
```
src/server/cellapp/
├── witness.hpp / .cpp
├── aoi_trigger.hpp / .cpp
tests/unit/test_witness.cpp
```

**测试用例（关键）:**
- 实体进入 AOI 半径 → `add_to_aoi` → ENTER_PENDING
- Witness::update() → 发送 EntityEnter
- 实体离开 → GONE → 发送 EntityLeave
- 优先级: 近的实体优先发送
- 带宽限制: 超出后停止发送，下 tick 继续
- 慢观察者若还在 history 窗口内 → 依序补发 delta
- 慢观察者若落后超出 history 窗口 → 自动走 snapshot/refresh，不会永久漏包
- 实体有 dirty delta → Witness 按观察者自己的发送序号决定是否携带 delta
- `ReplicatedDeltaFromCell` 的 payload 正确编码/解码 `CellAoIEnvelope`
- Scope 过滤: AllClients vs OwnClient
- AOI 半径变更 → 重新计算
- 1000 实体 AOI 性能

### Step 10.6: CellApp 消息定义

**新增文件:**
```
src/server/cellapp/cellapp_messages.hpp
tests/unit/test_cellapp_messages.cpp
```

### Step 10.7: CellAppNativeProvider + INativeApi 扩展

**新增/更新文件:**
```
src/server/cellapp/cellapp_native_provider.hpp / .cpp
src/lib/clrscript/clr_native_api_defs.hpp       (单一来源，新增 atlas_* 导出)
src/lib/clrscript/native_api_provider.hpp       (由 defs 自动扩展)
src/lib/clrscript/base_native_provider.hpp / .cpp
src/lib/clrscript/clr_native_api.hpp / .cpp     (由 defs 自动扩展)
```

新增导出:
```cpp
ATLAS_NATIVE_API void atlas_set_position(uint32_t entity_id,
    float x, float y, float z);
ATLAS_NATIVE_API void atlas_publish_replication_frame(uint32_t entity_id,
    uint64_t event_seq,
    uint64_t volatile_seq,
    const uint8_t* owner_snapshot, int32_t owner_snapshot_len,
    const uint8_t* other_snapshot, int32_t other_snapshot_len,
    const uint8_t* owner_delta, int32_t owner_len,
    const uint8_t* other_delta, int32_t other_len);
ATLAS_NATIVE_API int32_t atlas_add_move_controller(uint32_t entity_id,
    float dest_x, float dest_y, float dest_z, float speed, int user_arg);
ATLAS_NATIVE_API int32_t atlas_add_timer_controller(uint32_t entity_id,
    float interval, bool repeat, int user_arg);
ATLAS_NATIVE_API int32_t atlas_add_proximity_controller(uint32_t entity_id,
    float range, int user_arg);
ATLAS_NATIVE_API void atlas_cancel_controller(uint32_t entity_id,
    int32_t controller_id);
```

设计约束：
- `clr_native_api_defs.hpp` 是导出表的唯一来源；不要只修改单个 provider/header
- `BaseNativeProvider` 也要同步提供默认 no-op / 报错实现，保证非 CellApp 进程可继续链接

### Step 10.8: CellApp 进程

**新增文件:**
```
src/server/cellapp/
├── CMakeLists.txt
├── main.cpp
├── cellapp.hpp / .cpp
├── cellapp_native_provider.hpp / .cpp
```

**实现顺序:**
1. 基本启动 (EntityApp + machined)
2. Space 管理 (创建/销毁)
3. CreateCellEntity 处理 (从 BaseApp 请求)
4. CellEntity 生命周期 (C# 脚本集成)
5. AvatarUpdate 处理 (位置更新)
6. ClientCellRpcForward 处理 (客户端 Cell RPC 经 BaseApp 转发，含 Exposed + sourceEntityID 校验)
7. InternalCellRpc 处理 (服务器内部 Base→Cell RPC，无校验)
8. Controller tick
9. Witness tick + 消息生成
10. 首阶段: SelfRpcFromCell (Reliable: RPC + 属性 delta) / ReplicatedDeltaFromCell (Unreliable: Volatile 位置) → BaseApp
    优化阶段: BroadcastRpcFromCell → BaseApp
11. EnableWitness / DisableWitness
12. Watcher 注册

### Step 10.9: BaseApp 集成更新

**更新文件:**
```
src/server/baseapp/baseapp.hpp / .cpp
src/server/baseapp/baseapp_messages.hpp         (新增 ClientCellRpc struct；ClientBaseRpc 已存在)
```

**外部接口（客户端 RPC 接收）— 对齐 DEF_GENERATOR Step 6-8：**

- `ClientBaseRpc` (2022) struct 和 handler 已存在，无需重建
- **新增** `ClientCellRpc` (2023) struct 定义（当前仅有 message_ids.hpp 中的枚举值）
- `on_client_base_rpc()` 处理器：
  1. `find_proxy_by_channel()` 确定调用者
  2. `entity_def_registry_.find_rpc(rpc_id)` 查找方法
  3. `is_exposed()` 校验 → 非 exposed 丢弃
  4. 方向校验：`rpc_desc->direction() == 0x03` → 非 Base 方向丢弃（防止客户端用 Base 通道调用 Cell RPC）
  5. 通过 → 分发给 C# 脚本
- `on_client_cell_rpc()` 处理器：
  1. `find_proxy_by_channel()` 确定调用者
  2. `entity_def_registry_.find_rpc(rpc_id)` 查找方法
  3. `is_exposed()` 校验 → 非 exposed 丢弃
  4. 方向校验：`rpc_desc->direction() == 0x02` → 非 Cell 方向丢弃（防止客户端用 Cell 通道调用 Base RPC）
  5. 跨实体校验：`target != proxy && exposed != AllClients` → 丢弃
  6. 嵌入 `source_entity_id = proxy.entity_id()` 构造 `ClientCellRpcForward`
  7. 发送到目标实体的 CellApp（单 CellApp 阶段所有 Cell 地址相同，多 CellApp 阶段需全局路由表）

**内部接口（Cell→Base 回程）：**

- BaseApp 处理 `baseapp::CellEntityCreated` / `CellEntityDestroyed` / `CurrentCell`
- BaseApp 转发 `baseapp::SelfRpcFromCell` / `BroadcastRpcFromCell`
- BaseApp 转发 `baseapp::ReplicatedDeltaFromCell`（Unreliable，仅用于 Volatile 位置更新；当前代码是 byte-for-byte 透传到客户端）
- 若保持当前 handler，不修改外层消息 ID，则客户端侧需先消费 `0xF001 + CellAoIEnvelope`
- 若要直接对接 Phase 12 的 `EntityEnter/Leave/PropertyUpdate/PositionUpdate`，则 BaseApp 必须在这里解包并改发真实客户端消息

**生命周期与位置：**

- BaseApp 发送 `CreateCellEntity` → CellApp
- BaseApp 新增 `AvatarUpdate` (Client → CellApp) / `EnableWitness` / `DisableWitness`

**AOI 下行路径约束：**

- 若本阶段保持当前 BaseApp handler 语义不变，则 AOI 下行必须走”CellApp 按观察者逐个回送”的路径
- 若要真正使用 `BroadcastRpcFromCell(target=other/all)`，则需在本步骤补齐 BaseApp 的多观察者 fan-out

### Step 10.10: 集成测试

**新增文件:**
```
tests/integration/test_cellapp_integration.cpp
```

端到端场景（machined + DBApp + BaseAppMgr + BaseApp + CellApp + LoginApp）：
1. 全部启动
2. 客户端登录 → 创建 Proxy → 创建 Cell 实体
3. EnableWitness → AOI 开始工作
4. 移动实体 → 其他客户端收到位置更新
5. 新实体进入 AOI → 客户端收到 `0xF001(CellAoIEnvelope: EntityEnter)`
6. 实体离开 AOI → 客户端收到 `0xF001(CellAoIEnvelope: EntityLeave)`
7. C# 修改 [Replicated] 属性 → 客户端收到 `0xF001(CellAoIEnvelope: EntityPropertyUpdate)`
8. Controller: MoveToPoint → 实体平滑移动
9. 1000 实体同空间 → tick 性能测量
10. **RPC 安全校验（对齐 DEF_GENERATOR Step 11 C++ 测试）:**
    - 非 exposed Cell RPC → BaseApp 拒绝
    - Exposed OWN_CLIENT Cell RPC → 接受 owner 调用
    - Exposed OWN_CLIENT Cell RPC → 拒绝非 owner 调用（CellApp sourceEntityID 校验）
    - Exposed ALL_CLIENTS Cell RPC → 接受任意调用
    - 跨实体 Cell RPC + OWN_CLIENT → BaseApp 拒绝（target != proxy）
    - 跨实体 Cell RPC + ALL_CLIENTS → BaseApp 接受并转发
    - 非 exposed Base RPC → BaseApp 拒绝
    - 内部 Base→Cell RPC (InternalCellRpc) → CellApp 直接执行（无校验）

---

## 6. 文件清单汇总

```
src/lib/space/
├── CMakeLists.txt
├── range_list_node.hpp
├── range_list.hpp / .cpp
├── range_trigger.hpp / .cpp
├── range_trigger_node.hpp / .cpp
├── entity_range_list_node.hpp / .cpp
├── controller.hpp / .cpp
├── controllers.hpp / .cpp
├── move_controller.hpp / .cpp
├── timer_controller.hpp / .cpp
└── proximity_controller.hpp / .cpp

src/server/cellapp/
├── CMakeLists.txt
├── main.cpp
├── cellapp.hpp / .cpp
├── cellapp_messages.hpp
├── cellapp_native_provider.hpp / .cpp
├── space.hpp / .cpp
├── cell_entity.hpp / .cpp
├── witness.hpp / .cpp
└── aoi_trigger.hpp / .cpp

src/lib/clrscript/                      (扩展)
├── native_api_provider.hpp
├── clr_native_api.hpp / .cpp

src/server/baseapp/                     (扩展)
├── baseapp.hpp / .cpp
├── baseapp_messages.hpp                (新增 ClientBaseRpc / ClientCellRpc)

tests/unit/
├── test_range_list.cpp
├── test_range_trigger.cpp
├── test_controllers.cpp
├── test_space.cpp
├── test_cell_entity.cpp
├── test_witness.cpp
├── test_cellapp_messages.cpp

tests/integration/
└── test_cellapp_integration.cpp
```

---

## 7. 依赖关系与执行顺序

### 7.0 已知代码差异 / 前置修复项

Phase 10 开始前需确认或修复以下已知问题：

| # | 严重度 | 文件 | 问题 | 修复方式 |
|---|--------|------|------|---------|
| 1 | 高 | `baseapp_messages.hpp` | `ClientCellRpc` (2023) 仅保留消息 ID，**无 struct 定义和 handler** | Step 10.9 新增 struct + handler |
| 2 | 高 | `baseapp_messages.hpp` | `ReplicatedDeltaFromCell` (2015) 标记为 **Unreliable**，不能用于有序属性 delta (`event_seq`) | 属性 delta 走 `SelfRpcFromCell` (Reliable)；`ReplicatedDeltaFromCell` 仅用于 Volatile 位置更新 |
| 3 | 中 | `DeltaSyncEmitter.cs` / `entity_def_registry.hpp` | C# `ReplicationScope` 只有 4 值 (0-3)，C++ 有 8 值 (0-7)，枚举值含义不同（C# AllClients=3 vs C++ AllClients=4）。当前 `>=` 过滤因数值巧合而正确，但不可扩展 | Phase 10 采用 .def 定义后须改为显式 scope 匹配 |
| 4 | 中 | `client_native_provider.cpp` | `send_cell_rpc()` 是 stub（仅打日志），客户端无法调用 Cell RPC | Step 10.10 集成测试前须实现（依赖 Step 10.9 的 `ClientCellRpc` struct） |

```
前置: DEF_GENERATOR Step 5  ← EntityDefRegistry 扩展 (.def 解析、find_rpc、is_exposed)
      (CellApp RPC 校验和 BaseApp 外部接口都依赖 EntityDefRegistry)

Step 10.1: RangeList               ← 无依赖, 纯算法
Step 10.2: RangeTrigger            ← 依赖 10.1
    │
Step 10.3: Controller              ← 无依赖, 可与 10.1 并行
Step 10.6: 消息定义                ← 无依赖, 可并行（含 ClientCellRpcForward / InternalCellRpc）
    │
Step 10.4: Space + CellEntity      ← 依赖 10.1 + 10.2 + 10.3
    │
Step 10.5: Witness / AOI           ← 依赖 10.2 + 10.4
    │
Step 10.7: NativeApi 扩展          ← 依赖 10.3 + 10.4
    │
Step 10.8: CellApp 进程            ← 依赖 10.4 + 10.5 + 10.6 + 10.7 + DEF_GENERATOR Step 5
Step 10.9: BaseApp 集成            ← 依赖 10.6 + 10.8 + DEF_GENERATOR Step 5-6
Step 10.10: 集成测试               ← 依赖全部
```

**推荐执行顺序:**

```
第 0 轮 (前置): DEF_GENERATOR Step 5 — EntityDefRegistry .def 解析扩展
第 1 轮 (并行): 10.1 RangeList + 10.3 Controller + 10.6 消息定义
第 2 轮:        10.2 RangeTrigger
第 3 轮 (并行): 10.4 Space/CellEntity + 10.7 NativeApi
第 4 轮:        10.5 Witness/AOI
第 5 轮 (并行): 10.8 CellApp 进程 + 10.9 BaseApp 集成
第 6 轮:        10.10 集成测试
```

---

## 8. BigWorld 完整对照

| BigWorld | Atlas | 差异说明 |
|----------|-------|---------|
| `CellApp : EntityApp` Singleton | `CellApp : EntityApp` | 相同层次 |
| `Entity : PyObjectPlus` (~7800 LOC) | `CellEntity` (~500 LOC) | C# 管理属性/逻辑 |
| `RangeList` 双轴排序链表 | `RangeList` **完全对齐** | 核心算法不变 |
| `RangeTrigger` + 2D 交叉检测 | `RangeTrigger` **完全对齐** | 滞后老位置 + 先扩后缩 |
| `Witness` 优先级堆 + 带宽控制 | `Witness` 对齐设计 | delta 来自 C#，但 `EntityCache` 仍保留发送进度 |
| `eventHistory` + `PropertyOwnerLink` | `replication_state_` + `EntityCache.last_*_seq` | **核心简化**，而不是去掉观察者侧状态 |
| `EntityPopulation` (全局 map) | `entity_population_` + `base_entity_population_` | 双索引：cell_id 内部路由 + base_id RPC 路由 |
| `runExposedMethod` (REAL_ONLY) | `ClientCellRpcForward` (3003) | 客户端→Cell，含 sourceEntityID 校验 |
| `runScriptMethod` (REAL_ONLY) | `InternalCellRpc` (3004) | Base→Cell 内部，无校验 |
| `cellEntityMethod` (外部接口) | `ClientCellRpc` (2023) | BaseApp 外部接口，客户端入口（Phase 10 新增 struct） |
| `baseEntityMethod` (外部接口) | `ClientBaseRpc` (2022) | BaseApp 外部接口，客户端入口 |
| `Space` + `Cell` 分离 | `Space` = `Cell` (初期) | Phase 11 分离 |
| `RealEntity` + `Ghost` | 仅 Real (Phase 11 扩展) | 渐进 |
| 20+ Controller 子类 | 3 个 (MoveToPoint/Timer/Proximity) | C# 可扩展 |
| Vehicle 系统 | 初期不实现 | 后续 |
| Chunk/Terrain 空间 | 初期不实现 | 后续 |
| `volatile float` 防精度 | **保留** | 关键细节 |

---

## 9. 关键设计决策记录

### 9.1 RangeList 完全复用 BigWorld 算法

**决策: 不简化，不替换。**

BigWorld 的 RangeList 是经过 15 年 MMO 生产验证的核心算法。其巧妙之处：
- 双轴独立排序，配合交叉检测实现 2D 区域检测
- `volatile float` 防止编译器优化导致的精度不一致
- 先扩后缩 trigger shuffle 避免假事件
- 哨兵节点避免边界检查

尝试"简化"反而容易引入 bug。直接移植，保留所有细节。

### 9.2 属性同步: C# DirtyFlags 替代 C++ eventHistory

**BigWorld:** C++ 的 `PropertyOwnerLink` 追踪每个属性变更 → `eventHistory` 记录 → Witness 读取 history 生成客户端 delta

**Atlas（按当前代码约束修正后）:** C# Source Generator 生成 `DirtyFlags`，并需要扩展出
“owner/other 快照 + owner/other delta” 两套序列化结果，以及对应的 event/volatile 序号
→ NativeApi 传到 C++ → `CellEntity::replication_state_` 维护“最新快照 + 有界历史” →
各 `Witness::EntityCache` 自己记录已经发送到哪个序号

优势: C++ 不需要理解属性结构，只传递面向不同受众的 blob。
代价: Script Phase 4 需要补齐按受众过滤的“快照 + 增量 + 序号”API；当前仓库只有
`SerializeReplicatedDelta()` 与全量快照 `SerializeForOwnerClient()` /
`SerializeForOtherClients()`，不足以直接完成 BigWorld 风格的 AOI 增量同步。

**补包规则（两种流的语义截然不同，不可混淆）：**

| 流 | 语义 | 丢包/落后处理 | 原因 |
|---|---|---|---|
| **属性事件流** (`event_seq`) | 有序、累积 | 必须按序补发 delta；若断档超出 history 窗口则退回 snapshot/refresh | 属性变更不可跳过，否则客户端状态永久偏离 |
| **Volatile 位置流** (`volatile_seq`) | Latest-wins | 直接发送最新 absolute 位置，丢弃中间帧 | 位置是瞬时状态，旧值无意义，补发反而浪费带宽 |

这两种流在 `EntityCache` 中各自独立追踪（`last_event_seq` / `last_volatile_seq`），
Witness 在 `send_entity_update()` 中分别处理。BigWorld 的 `lastEventNumber_` /
`lastVolatileUpdateNumber_` 采用相同分治策略。

### 9.3 位置仍在 C++ 管理

**决策: `position_` / `direction_` 在 C++ 的 CellEntity 中。**

RangeList 需要在 shuffle 时直接读取 `float x()` / `float z()`。
如果位置在 C# 中，每次 shuffle 需要 P/Invoke 回 C#，性能不可接受。

C# 通过 `atlas_set_position()` NativeApi 设置位置。
C++ 直接读取 position 驱动 RangeList。

### 9.4 本阶段无 Real/Ghost

**决策: 所有实体都是 Real，Phase 11 引入 Ghost。**

接口预留:
- `CellEntity` 没有 `is_real()` / `is_ghost()` 区分（Phase 11 加入）
- Witness 只附加到 Real Entity（Phase 11 约束）
- Controller 只在 Real Entity 执行（Phase 11 约束）

### 9.5 INativeApiProvider 区分 BaseApp 和 CellApp

`send_client_rpc()` 在两个进程中行为不同:
- **BaseApp:** 直接发送到 Proxy 的客户端 Channel
- **CellApp:** Phase 10 首阶段按观察者逐个展开，
  属性 delta 经 `SelfRpcFromCell` (Reliable) 回送，Volatile 位置经 `ReplicatedDeltaFromCell` (Unreliable) 回送；
  后续如扩展 BaseApp observer fan-out，再把 `BroadcastRpcFromCell` 用作聚合优化

同一个 C# 代码 `entity.Client.ShowDamage()` 在不同进程自动路由到正确路径。

### 9.6 EntityID 分层与 RPC 路由

当前代码里 `BaseEntity` 已经持有 `cell_entity_id_ + cell_addr_`，说明需要区分：
- `base_entity_id`: BaseApp / 客户端可见的稳定实体标识
- `cell_entity_id`: CellApp 内部路由标识，可在未来 offload/重建时变化

**RPC 路由的 ID 空间选择（对齐 DEF_GENERATOR_DESIGN）：**

DEF_GENERATOR_DESIGN Step 8-9 中的 `target_entity_id` 和 `source_entity_id` 均在
`base_entity_id` 空间中：
- 客户端发送 `ClientCellRpc` 时填写的 `target_entity_id` 是 `base_entity_id`（客户端只知道这个）
- BaseApp 嵌入的 `source_entity_id = proxy.entity_id()` 也是 `base_entity_id`
- CellApp 的 `on_client_cell_rpc_forward()` 使用 `base_entity_population_` 索引定位实体
- OWN_CLIENT 校验 `target_entity_id != source_entity_id` 在 `base_entity_id` 空间比较

因此 CellApp 维护双索引：
- `entity_population_`: `cell_entity_id` → `CellEntity*`（内部管理、生命周期）
- `base_entity_population_`: `base_entity_id` → `CellEntity*`（RPC 路由、AOI 下行）

其余规则不变：
- CellApp 运行时中的 C# `EntityId` 应与 `cell_entity_id` 对齐，由 C++ 分配后通过
  `RestoreEntity(cell_entity_id, ...)` 注入脚本层；其高 8 位应复用 `ProcessType::CellApp`
- CellApp → BaseApp 的创建/销毁/迁移消息可以带 `cell_entity_id`
- 面向客户端的 AOI/RPC/属性更新 payload 必须携带稳定的 `base_entity_id`，不能直接泄露 CellApp 局部 ID
- 若后续引入”纯 Cell、无 Base 对应物”的实体，则需要额外定义稳定的 `public_entity_id`
  层；在本阶段先把范围限定为”客户端可见实体均有 Base 对应物”

### 9.7 RPC 安全校验：双消息 + 四层纵深

**决策: CellApp 接收 RPC 拆分为 `ClientCellRpcForward` 和 `InternalCellRpc` 两条消息。**

这是 BigWorld 的核心安全设计（`runExposedMethod` vs `runScriptMethod`），拆分的原因：

1. **安全边界清晰**：客户端路径需要 Exposed 校验 + direction 验证 + sourceEntityID 验证；内部路径来自可信 BaseApp，无需任何校验
2. **不可混淆来源**：合并为一条消息需要额外的 `is_from_client` 标志，而标志本身可能被错误设置
3. **对齐 BigWorld REAL_ONLY 语义**：Phase 11 引入 Ghost 时，两条消息都标记为 REAL_ONLY，Ghost 透明转发

安全校验全链路（四层纵深，对齐 BigWorld Section 4.1-4.5）：

| 层 | 位置 | 检查 | 拒绝时 |
|---|---|---|---|
| 第 1 层 | BaseApp `on_client_cell_rpc()` | `is_exposed(rpc_id)` | 丢弃 + 日志 |
| 第 1.5 层 | BaseApp `on_client_cell_rpc()` | `direction() == 0x02`（防止 Base RPC 走 Cell 通道） | 丢弃 + 日志 |
| 第 2 层 | BaseApp `on_client_cell_rpc()` | 跨实体 + 非 ALL_CLIENTS | 丢弃 + 日志 |
| 第 3 层 | CellApp `on_client_cell_rpc_forward()` | `direction() == 0x02`（纵深校验） | 丢弃 + 日志 |
| 第 4 层 | CellApp `on_client_cell_rpc_forward()` | OWN_CLIENT + source != target | 丢弃 + 日志 |
