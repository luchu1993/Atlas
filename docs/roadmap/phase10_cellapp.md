# Phase 10: CellApp — 空间模拟

> 前置依赖: Phase 8 (BaseApp), Phase 9 (BaseAppMgr), Script Phase 4 (`[Replicated]` + DirtyFlags；Phase 10 需补充按受众过滤的 delta API)
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
`CellApp → BaseApp` 回程协议。消息需要分成两层：

1. `3000-3099`：CellApp 自有消息，只用于 `BaseApp → CellApp` 请求和 CellApp 本地管理
2. `2010-2016`：复用已实现的 `BaseApp` 入站消息，由 CellApp 发回 BaseApp

### 2.2 CellApp 新增消息 (`3000-3099`)

| 消息 | ID | 方向 | 用途 |
|------|-----|------|------|
| `CreateCellEntity` | 3000 | BaseApp → CellApp | 创建 Cell 实体 |
| `DestroyCellEntity` | 3002 | BaseApp → CellApp | 销毁 Cell 实体 |
| `CellEntityRpc` | 3003 | BaseApp → CellApp | 客户端 RPC 转发（[ServerRpc] 经 Base） |
| `CreateSpace` | 3010 | 管理/脚本 → CellApp | 创建 Space |
| `DestroySpace` | 3011 | 管理/脚本 → CellApp | 销毁 Space |
| `AvatarUpdate` | 3020 | BaseApp → CellApp | 客户端位置更新 |
| `EnableWitness` | 3021 | BaseApp → CellApp | 开启 AOI（客户端 enableEntities） |
| `DisableWitness` | 3022 | BaseApp → CellApp | 关闭 AOI |

约束：
- 不再在 `3000` 段定义 `CreateCellEntityAck` / `BaseEntityRpc` / 泛化版 `ClientRpcFromCell` / `ReplicatedDelta` / `CellEntityPosition`
- `CreateCellEntity` 的成功路径直接复用 `baseapp::CellEntityCreated`
- 显式失败回包当前代码未定义；如果实现阶段确实需要失败通知，应新增独立失败消息，而不是覆盖现有 `2010-2016` 语义

### 2.3 复用现有 BaseApp 入站消息 (`CellApp → BaseApp`)

| 消息 | ID | 方向 | 当前代码语义 |
|------|-----|------|-------------|
| `CellEntityCreated` | 2010 | CellApp → BaseApp | Cell 创建成功，Base 记录 `cell_entity_id + cell_addr` |
| `CellEntityDestroyed` | 2011 | CellApp → BaseApp | Cell 已销毁，Base 清理路由 |
| `CurrentCell` | 2012 | CellApp → BaseApp | Phase 11 offload 后更新 Cell 地址 |
| `CellRpcForward` | 2013 | CellApp → BaseApp | Cell → Base RPC，Base 侧分发给脚本 |
| `SelfRpcFromCell` | 2014 | CellApp → BaseApp | 发给拥有者客户端，可靠路径 |
| `ReplicatedDeltaFromCell` | 2015 | CellApp → BaseApp | 当前代码只透传到 `base_entity_id` 对应的单个客户端；Phase 10 可先用于“按观察者逐个回送”的 AOI delta |
| `BroadcastRpcFromCell` | 2016 | CellApp → BaseApp | 消息定义支持 `otherClients/allClients`，但当前 `BaseApp` 实现仍只发给 `base_entity_id` 对应客户端，不能当作已完成的 AOI fan-out |

这部分消息定义已经存在于 `src/server/baseapp/baseapp_messages.hpp`，Phase 10 直接复用，不再在 CellApp 文档里重定义一份并行版本。

当前代码现实约束：
- `BaseApp::on_self_rpc_from_cell()` 可稳定发给单个 Proxy 客户端
- `BaseApp::on_replicated_delta_from_cell()` 也只会发给单个 Proxy，且外层消息 ID 当前固定为 `0xF001`
- `BaseApp::on_broadcast_rpc_from_cell()` 目前并不会根据 `target` 枚举遍历 AOI 观察者集合

因此，Phase 10 文档中的 AOI 下行闭环必须按下面两种路径之一实现，不能把现状写成“已支持广播”：
- 首选路径：`CellApp/Witness` 直接按“每个观察者一个回包”展开，下行时使用观察者自己的 `base_entity_id` 作为路由键，经 `SelfRpcFromCell` / `ReplicatedDeltaFromCell` 发回 BaseApp
- 后续优化：扩展 BaseApp，使 `BroadcastRpcFromCell` 真正具备按观察者集合 fan-out 的能力

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
- `SelfRpcFromCell` 仍用于“拥有者客户端 RPC”这条可靠路径
- `ReplicatedDeltaFromCell` 首阶段只承担 AOI/复制 envelope 的单观察者回送
- Phase 12 若落地真实的 `10102/10103/10104/10111` 客户端协议，则应由 BaseApp 在下行前解包
  `CellAoIEnvelope`，或直接把 `ReplicatedDeltaFromCell` 改为携带真实客户端消息 ID

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

struct CellEntityRpc {
    EntityID cell_entity_id;
    uint32_t rpc_id;
    EntityID caller_entity_id;     // 调用者（用于安全校验）
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
    void on_cell_entity_rpc(const Address& src, Channel* ch,
                             const cellapp::CellEntityRpc& msg);
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

    // 全局 EntityID → CellEntity* 索引
    std::unordered_map<EntityID, CellEntity*> entity_population_;

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
                // → 首阶段生成逐观察者的 SelfRpcFromCell / ReplicatedDeltaFromCell
                // → 如后续补齐 BaseApp observer fan-out，再引入 BroadcastRpcFromCell 优化
                // → 发送到 BaseApp Proxy
```

### 3.8 CellAppNativeProvider

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

### 4.2 客户端移动 → AOI 更新

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
6. CellEntityRpc 处理 (客户端 RPC 经 BaseApp 转发)
7. Controller tick
8. Witness tick + 消息生成
9. 首阶段: SelfRpcFromCell / ReplicatedDeltaFromCell → BaseApp
   优化阶段: BroadcastRpcFromCell → BaseApp
10. EnableWitness / DisableWitness
11. Watcher 注册

### Step 10.9: BaseApp 集成更新

**更新文件:**
```
src/server/baseapp/baseapp.hpp / .cpp
```

- BaseApp 发送 `CreateCellEntity` → CellApp
- BaseApp 处理 `baseapp::CellEntityCreated` / `CellEntityDestroyed` / `CurrentCell`
- BaseApp 转发 `baseapp::SelfRpcFromCell` / `BroadcastRpcFromCell`
- BaseApp 转发 `baseapp::ReplicatedDeltaFromCell`（当前代码是 byte-for-byte 透传到客户端）
- 若保持当前 handler，不修改外层消息 ID，则客户端侧需先消费 `0xF001 + CellAoIEnvelope`
- 若要直接对接 Phase 12 的 `EntityEnter/Leave/PropertyUpdate/PositionUpdate`，则 BaseApp 必须在这里解包并改发真实客户端消息
- BaseApp 新增 `AvatarUpdate` (Client → CellApp) / `EnableWitness` / `DisableWitness`
- 若本阶段保持当前 BaseApp handler 语义不变，则 AOI 下行必须走“CellApp 按观察者逐个回送”的路径；
  若要真正使用 `BroadcastRpcFromCell(target=other/all)`，则需在本步骤补齐 BaseApp 的多观察者 fan-out

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

```
Step 10.1: RangeList               ← 无依赖, 纯算法
Step 10.2: RangeTrigger            ← 依赖 10.1
    │
Step 10.3: Controller              ← 无依赖, 可与 10.1 并行
Step 10.6: 消息定义                ← 无依赖, 可并行
    │
Step 10.4: Space + CellEntity      ← 依赖 10.1 + 10.2 + 10.3
    │
Step 10.5: Witness / AOI           ← 依赖 10.2 + 10.4
    │
Step 10.7: NativeApi 扩展          ← 依赖 10.3 + 10.4
    │
Step 10.8: CellApp 进程            ← 依赖 10.4 + 10.5 + 10.6 + 10.7
Step 10.9: BaseApp 集成            ← 依赖 10.6 + 10.8
Step 10.10: 集成测试               ← 依赖全部
```

**推荐执行顺序:**

```
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
| `EntityPopulation` (全局 map) | `entity_population_` (CellApp 成员) | 相同模式 |
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

补包规则：
- 属性事件流必须保持顺序；若缺失中间 delta，则退回 snapshot/refresh
- volatile 位置流采用 latest-wins；观察者落后时直接发送最新 absolute 位置即可

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
  使用观察者 `base_entity_id` 回送 `SelfRpcFromCell` / `ReplicatedDeltaFromCell`；
  后续如扩展 BaseApp observer fan-out，再把 `BroadcastRpcFromCell` 用作聚合优化

同一个 C# 代码 `entity.Client.ShowDamage()` 在不同进程自动路由到正确路径。

### 9.6 EntityID 分层

当前代码里 `BaseEntity` 已经持有 `cell_entity_id_ + cell_addr_`，说明需要区分：
- `base_entity_id`: BaseApp / 客户端可见的稳定实体标识
- `cell_entity_id`: CellApp 内部路由标识，可在未来 offload/重建时变化

因此：
- CellApp 运行时中的 C# `EntityId` 应与 `cell_entity_id` 对齐，由 C++ 分配后通过
  `RestoreEntity(cell_entity_id, ...)` 注入脚本层；其高 8 位应复用 `ProcessType::CellApp`
- CellApp → BaseApp 的创建/销毁/迁移消息可以带 `cell_entity_id`
- 面向客户端的 AOI/RPC/属性更新 payload 必须携带稳定的可见实体 ID，当前优先使用
  `base_entity_id`，不能直接泄露 CellApp 局部 ID
- 若后续引入“纯 Cell、无 Base 对应物”的实体，则需要额外定义稳定的 `public_entity_id`
  层；在本阶段先把范围限定为“客户端可见实体均有 Base 对应物”
