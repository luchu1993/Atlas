# Phase 10: CellApp — 空间模拟

> 前置依赖: Phase 8 (BaseApp), Phase 9 (BaseAppMgr), Script Phase 4 (`[Replicated]` + DirtyFlags)
> BigWorld 参考: `server/cellapp/cellapp.hpp`, `server/cellapp/entity.hpp`, `server/cellapp/witness.hpp`

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
  → C# 每 tick 检查 IsDirty → 调用 SerializeReplicatedDelta() → NativeApi
  → C++ Witness 收到 delta blob → 按优先级调度发送
```

**关键差异:**
- BigWorld: 属性变更在 C++ 层追踪（`PropertyOwnerLink` + `eventHistory`）
- Atlas: 属性变更在 C# 层追踪（`DirtyFlags`），C++ 只收到序列化好的 delta blob
- BigWorld: Witness 直接读 Entity 的 C++ 属性生成 delta
- Atlas: Witness 从 C# 获取 delta blob，不解析内容

**这意味着:**
- C++ 不需要 `eventHistory`（C# 管理）
- C++ 不需要 `PropertyOwnerLink`（C# DirtyFlags 替代）
- C++ 的 Witness 简化为：管理 EntityCache + 优先级调度 + 转发 blob
- 位置/方向仍在 C++ 管理（RangeList 需要直接访问）

---

## 2. 消息协议设计

### 2.1 CellApp 内部接口

| 消息 | ID | 方向 | 用途 |
|------|-----|------|------|
| `CreateCellEntity` | 3000 | BaseApp → CellApp | 创建 Cell 实体 |
| `CreateCellEntityAck` | 3001 | CellApp → BaseApp | 创建结果（Cell地址） |
| `DestroyCellEntity` | 3002 | BaseApp → CellApp | 销毁 Cell 实体 |
| `CellEntityRpc` | 3003 | BaseApp → CellApp | 客户端 RPC 转发（[ServerRpc] 经 Base） |
| `BaseEntityRpc` | 3004 | CellApp → BaseApp | Cell → Base RPC |
| `ClientRpcFromCell` | 3005 | CellApp → BaseApp | Cell → Client RPC（经 Proxy 转发） |
| `ReplicatedDelta` | 3006 | CellApp → BaseApp | 属性增量 → 经 Proxy 转发客户端 |
| `CellEntityPosition` | 3007 | CellApp → BaseApp | 位置更新（Volatile） |
| `CreateSpace` | 3010 | 管理/脚本 → CellApp | 创建 Space |
| `DestroySpace` | 3011 | 管理/脚本 → CellApp | 销毁 Space |
| `AvatarUpdate` | 3020 | BaseApp → CellApp | 客户端位置更新 |
| `EnableWitness` | 3021 | BaseApp → CellApp | 开启 AOI（客户端 enableEntities） |
| `DisableWitness` | 3022 | BaseApp → CellApp | 关闭 AOI |

### 2.2 详细消息定义

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
    // 可选: C# cell init data blob

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

struct CreateCellEntityAck {
    uint32_t request_id;
    bool success;
    EntityID cell_entity_id;       // CellApp 上分配的 EntityID
    Address cell_addr;             // CellApp 地址
    std::string error;

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

struct ClientRpcFromCell {
    EntityID base_entity_id;       // Proxy EntityID (BaseApp 侧)
    EntityID source_entity_id;     // 产生 RPC 的实体（可能不是 Proxy 自身）
    uint32_t rpc_id;
    uint8_t target;                // OwnerClient / AllClients / OtherClients
    std::vector<std::byte> payload;

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

struct ReplicatedDelta {
    EntityID base_entity_id;       // Proxy EntityID
    EntityID source_entity_id;     // 产生 delta 的实体
    uint8_t scope;                 // ReplicationScope
    std::vector<std::byte> delta;  // C# SerializeReplicatedDelta() 输出

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
    RangeListNode head_;  // x=z=-FLT_MAX
    RangeListNode tail_;  // x=z=+FLT_MAX
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

    /// 范围变更（AOI 半径调整）
    void set_range(float new_range);

    /// 中心节点移动后更新边界
    void update(float old_x, float old_z);

    /// 子类覆盖
    virtual void on_enter(Entity& entity) = 0;
    virtual void on_leave(Entity& entity) = 0;

private:
    RangeListNode& central_;
    float range_;

    /// 上下界节点
    RangeTriggerBoundNode upper_x_, upper_z_;
    RangeTriggerBoundNode lower_x_, lower_z_;

    /// 旧位置（用于 2D 交叉检测）
    float old_entity_x_ = 0.0f;
    float old_entity_z_ = 0.0f;
};

/// AOI 触发器（Witness 使用）
class AoITrigger : public RangeTrigger {
public:
    explicit AoITrigger(Witness& owner, float radius);

    void on_enter(Entity& entity) override;
    void on_leave(Entity& entity) override;

private:
    Witness& owner_;
};

} // namespace atlas
```

### 3.3 CellEntity — Cell 侧实体

```cpp
// src/server/CellApp/cell_entity.hpp
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

    // ========== Dirty 属性 ==========
    /// C# 调用 NativeApi 传入 delta blob
    void set_replicated_dirty(uint8_t scope, std::vector<std::byte> delta);

    /// Witness 取走待发送的 delta
    struct PendingDelta {
        uint8_t scope;
        std::vector<std::byte> data;
    };
    auto take_pending_deltas() -> std::vector<PendingDelta>;

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

    // 脏属性（C# 传入的 delta blob）
    std::vector<PendingDelta> pending_deltas_;

    bool destroyed_ = false;
};

} // namespace atlas
```

**与 BigWorld Entity 的对比:**

| BigWorld Entity (~7800 LOC) | Atlas CellEntity (~500 LOC) | 差异 |
|---|---|---|
| `PyObjectPlus` 基类 | `uint64_t script_handle_` | C# GCHandle |
| `properties_` (vector\<ScriptObject\>) | 无（C# 管理） | 属性全在 C# |
| `eventHistory_` / `PropertyOwnerLink` | `pending_deltas_` | C# DirtyFlags 替代 |
| `pVehicle_` / Vehicle 系统 | 初期不实现 | 后续 |
| `pChunk_` / Chunk 空间 | 初期不实现 | 后续 |
| Real/Ghost 双模式 | 仅 Real（Phase 11 扩展） | 渐进 |
| `pControllers_` (20+ 子类) | `Controllers` (3 个初始子类) | C# 可扩展 |

### 3.4 Witness — AOI 管理器

```cpp
// src/server/CellApp/witness.hpp
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

        // 标志
        static constexpr uint8_t ENTER_PENDING  = 0x01;
        static constexpr uint8_t GONE           = 0x08;

        [[nodiscard]] auto is_updatable() const -> bool {
            return (flags & (ENTER_PENDING | GONE)) == 0;
        }

        void update_priority(const Vector3& origin) {
            float dist = (entity->position() - origin).length();
            priority = dist / 5.0 + 1.0;
        }
    };

    /// 处理状态转换 (ENTER_PENDING → 发送 enterAoI, GONE → 移除)
    void handle_state_change(EntityCache& cache);

    /// 发送单个实体的更新
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

**Witness::update() 流程（对齐 BigWorld）:**

```
Witness::update(max_packet_bytes):

  PHASE 1: 堆维护
    rebuild priority_queue_ from aoi_map_ (if needed)
    make_heap (min-heap by priority)

  PHASE 2: 状态转换
    遍历堆，处理 ENTER_PENDING / GONE:
      ENTER_PENDING:
        发送 EntityEnter (type_id, position, [AllClients] 属性全量)
        清除 ENTER_PENDING 标志
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
                2. 属性 delta (从 entity->take_pending_deltas() 获取)
                   按 scope 过滤:
                     AllClients → 发给本 Witness
                     OwnClient → 仅当 entity == owner_ 时发送
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
// src/server/CellApp/space.hpp
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
// src/server/CellApp/cellapp.hpp
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
    C# 检查 DirtyFlags → 调用 NativeApi → CellEntity::set_replicated_dirty()

on_tick_complete():                  // Updatable 之后
    tick_controllers(dt)             // 所有 Controller::update()
    tick_witnesses()                 // 所有 Witness::update()
        for each space:
            for each entity with witness:
                witness->update(max_packet_bytes)
                // → 生成 ClientRpcFromCell / ReplicatedDelta 消息
                // → 发送到 BaseApp Proxy
```

### 3.8 CellAppNativeProvider

```cpp
// src/server/CellApp/cellapp_native_provider.hpp
namespace atlas {

class CellAppNativeProvider : public BaseNativeProvider {
public:
    explicit CellAppNativeProvider(CellApp& app);

    double server_time() override;
    float delta_time() override;
    uint8_t get_process_prefix() override { return 'C'; }

    // C# entity.Client.ShowDamage() → 这里 → 经 BaseApp Proxy 发客户端
    void send_client_rpc(uint32_t entity_id, uint32_t rpc_id,
                         uint8_t target,
                         const std::byte* payload, int32_t len) override;

    // C# entity.Base.SomeMethod() → 这里 → 发给 BaseApp
    void send_base_rpc(uint32_t entity_id, uint32_t rpc_id,
                       const std::byte* payload, int32_t len) override;

    // C# 属性变更 → 标记 dirty
    void set_replicated_dirty(uint32_t entity_id, uint8_t scope,
                               const std::byte* delta, int32_t len);

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
  │                               │── NativeApi ──────────────→│
  │                               │   EntityFactory.Create()   │
  │                               │   entity.OnCreatedOnCell() │
  │                               │                            │
  │←── CreateCellEntityAck ──────│                            │
  │   (ok, cell_id, cell_addr)   │                            │
  │                               │                            │
  │ base_entity.set_cell(addr,id)│                            │
```

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
  │               │                  │ → EntityEnter/Leave    │
  │               │                  │ → ReplicatedDelta      │
  │               │                  │ → CellEntityPosition   │
  │               │                  │                        │
  │               │←── 批量消息 ────│                        │
  │               │ ClientRpcFromCell│                        │
  │               │ ReplicatedDelta │                        │
  │               │ EntityEnter     │                        │
  │               │                  │                        │
  │               │── 转发到客户端 ──────────────────────────→│
```

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
src/server/CellApp/
├── space.hpp / .cpp
├── cell_entity.hpp / .cpp
tests/unit/test_space.cpp
tests/unit/test_cell_entity.cpp
```

**测试用例:**
- Space 创建/销毁
- 实体添加/移除
- 位置设置 → RangeList 更新
- `set_replicated_dirty()` → `take_pending_deltas()`
- 实体销毁 → 从 Space + RangeList 移除

### Step 10.5: Witness / AOI

**新增文件:**
```
src/server/CellApp/
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
- 实体有 dirty delta → Witness 携带 delta 发送
- Scope 过滤: AllClients vs OwnClient
- AOI 半径变更 → 重新计算
- 1000 实体 AOI 性能

### Step 10.6: CellApp 消息定义

**新增文件:**
```
src/server/CellApp/cellapp_messages.hpp
tests/unit/test_cellapp_messages.cpp
```

### Step 10.7: CellAppNativeProvider + INativeApi 扩展

**新增/更新文件:**
```
src/server/CellApp/cellapp_native_provider.hpp / .cpp
src/lib/clrscript/native_api_provider.hpp       (扩展: set_replicated_dirty 等)
src/lib/clrscript/clr_native_api.hpp / .cpp      (扩展: atlas_set_replicated_dirty 等)
```

新增导出:
```cpp
ATLAS_NATIVE_API void atlas_set_position(uint32_t entity_id,
    float x, float y, float z);
ATLAS_NATIVE_API void atlas_set_replicated_dirty(uint32_t entity_id,
    uint8_t scope, const uint8_t* delta, int32_t len);
ATLAS_NATIVE_API int32_t atlas_add_move_controller(uint32_t entity_id,
    float dest_x, float dest_y, float dest_z, float speed, int user_arg);
ATLAS_NATIVE_API int32_t atlas_add_timer_controller(uint32_t entity_id,
    float interval, bool repeat, int user_arg);
ATLAS_NATIVE_API int32_t atlas_add_proximity_controller(uint32_t entity_id,
    float range, int user_arg);
ATLAS_NATIVE_API void atlas_cancel_controller(uint32_t entity_id,
    int32_t controller_id);
```

### Step 10.8: CellApp 进程

**新增文件:**
```
src/server/CellApp/
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
9. ClientRpcFromCell / ReplicatedDelta → BaseApp
10. EnableWitness / DisableWitness
11. Watcher 注册

### Step 10.9: BaseApp 集成更新

**更新文件:**
```
src/server/BaseApp/baseapp.hpp / .cpp
```

- BaseApp 发送 `CreateCellEntity` → CellApp
- BaseApp 处理 `CreateCellEntityAck` → `base_entity.set_cell()`
- BaseApp 转发 `ClientRpcFromCell` → Proxy → Client
- BaseApp 转发 `ReplicatedDelta` → Proxy → Client
- BaseApp 转发 `AvatarUpdate` (Client → CellApp)
- BaseApp 发送 `EnableWitness` / `DisableWitness`

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
5. 新实体进入 AOI → 客户端收到 EntityEnter
6. 实体离开 AOI → 客户端收到 EntityLeave
7. C# 修改 [Replicated] 属性 → 客户端收到 delta
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

src/server/CellApp/
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

src/server/BaseApp/                     (扩展)
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
| `Witness` 优先级堆 + 带宽控制 | `Witness` 对齐设计 | delta 来自 C# |
| `eventHistory` + `PropertyOwnerLink` | `pending_deltas_` (C# DirtyFlags) | **核心简化** |
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

**Atlas:** C# Source Generator 生成 `DirtyFlags` → C# `SerializeReplicatedDelta()` 输出 blob → NativeApi 传到 C++ → `CellEntity::pending_deltas_` → Witness 取走发送

优势: C++ 不需要理解属性结构，只传递 blob。
代价: C++ 无法合并多个 tick 的 delta（C# 每 tick 只产生一个 delta blob）。
但对于大多数游戏场景，单 tick delta 足够。

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
- **CellApp:** 生成 `ClientRpcFromCell` 消息发送到 BaseApp，由 BaseApp Proxy 转发

同一个 C# 代码 `entity.Client.ShowDamage()` 在不同进程自动路由到正确路径。
