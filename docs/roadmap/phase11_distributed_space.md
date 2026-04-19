# Phase 11: 分布式空间 — Real/Ghost + CellAppMgr

> 前置依赖: Phase 10 (CellApp 单机可工作), Phase 9 (BaseAppMgr)
> BigWorld 参考: `server/cellapp/real_entity.hpp`, `server/cellapp/entity_ghost_maintainer.cpp`, `server/cellappmgr/`
> 当前代码基线 (2026-04-12): 仓库里还没有 `CellApp` / `CellAppMgr` 实现；当前内部进程通信基线是
> `EntityApp` / `ManagerApp` 共享的 `RUDP`。因此本阶段文档中的 inter-CellApp / CellAppMgr 通道
> 应默认建立在现有内部 `RUDP` 基线之上，而不是另起一套 TCP 专用控制面。

---

## 目标

将单 CellApp 扩展为多 CellApp 分布式空间。一个 Space 可被 BSP 树分区到多个 CellApp 上，
实体跨 Cell 边界时通过 Real/Ghost 机制保持 AOI 可见性，通过 Offload 实现无缝迁移。

这是 Atlas 区别于普通游戏服务器的**核心分布式能力**。

## 验收标准

- [ ] CellAppMgr 可启动，管理 CellApp 注册和 Space 分区
- [ ] BSP 树可将一个 Space 分配到多个 CellApp
- [ ] 实体在 Cell 边界附近时，相邻 Cell 自动创建 Ghost 副本
- [ ] Ghost 自动接收 Real 的位置更新和属性 delta
- [ ] 实体跨 Cell 边界移动时自动 Offload（convertRealToGhost + convertGhostToReal）
- [ ] Offload 对客户端完全透明（BaseApp 路由自动更新）
- [ ] Ghost 维护器正确管理 Ghost 生命周期（兴趣区域 + 滞后距离）
- [ ] CellAppMgr 可动态调整 BSP 分割线位置（负载均衡）
- [ ] CellApp 死亡 → CellAppMgr 检测到 → 实体从备份恢复（基本容灾）
- [ ] 全部新增代码有单元测试

**Phase 10 §10.1 遗留的 4 条硬约束（必须在本阶段解决，见 §9.6）：**

- [ ] 跨 CellApp `ClientCellRpcForward` 的信任边界收紧（peer 白名单或签名信封，不再无条件信任 `msg.source_entity_id`）
- [ ] EntityID 跨 CellApp 唯一（由 CellAppMgr 分配 cluster-wide 或按 app_id 分高位段）
- [ ] 客户端 RPC 速率 / CPU 预算（按 machined 层 rate limiter 接入）
- [ ] Offload / Real↔Ghost 转换如果需要 "延迟销毁" 语义，**必须**通过 CellApp 的显式通知钩子，**不得**恢复 Phase 10 被刻意移除的 `Space::Tick` silent compaction 路径

**从 `Real→Ghost→Real` 迁移过程对 Phase 10 复制模型的一致性要求：**

- [ ] `SerializeOwnerDelta` / `SerializeOtherDelta` 经 Real→Ghost 路径端到端一致（受众位图不跨界泄露）
- [ ] Witness 的 `EntityCache.last_event_seq / last_volatile_seq` 在 Offload 前后对同一个 peer 保持连续（对客户端透明）
- [ ] Ghost 落后 `CellEntity::kReplicationHistoryWindow`（= 8）帧以外时能触发 Real 端的 snapshot resend，而不是静默丢状态

## 验收状态（2026-04-13）

- [ ] 当前仓库仍未进入 `CellApp` / `CellAppMgr` / Real-Ghost 实现阶段，本阶段保持为设计文档。

## 文档使用约定（与 phase10_cellapp.md 一致）

- **代码示例命名**：本文 C++ 代码块内的 **新增类与方法** 沿用 BigWorld 风格
  `snake_case`（`convert_real_to_ghost`、`add_haunt`、`broadcast_position` 等），
  便于与 BigWorld `RealEntity` / `EntityGhostMaintainer` / BSP 源码对照。
  **实际落地到仓库时全部改为 `PascalCase`**，遵循 `CLAUDE.md` 的 Google Style + Phase 10
  已落地代码（`CellEntity::EnableWitness`、`Witness::HandleAoIEnter`、
  `RangeList::ShuffleXThenZ` 等）的约定。
- **对 Phase 10 已落地代码的引用**：文档中提到 Phase 10 仓库里已经存在的类 / 方法 / 字段时
  直接用仓库里的真实名字（PascalCase 方法、`snake_case_` 尾下划线私有字段）。例如
  `CellEntity::Position()` / `CellEntity::GetReplicationState()` / `range_node_.SetOwnerData(this)`。
- **文件扩展名**：本文示例用 `.hpp/.cpp` 保持 BigWorld 风格参考；**Atlas 仓库统一使用 `.h/.cc`**。
  实现步骤里的 "新增文件" 清单属于 forward-looking 设计描述，用 `.hpp/.cpp` 与其余代码块一致；
  真实落 PR 时按 `.h/.cc` 命名（可见 Phase 10 的 `src/server/cellapp/cell_entity.{h,cc}` 已
  按此规则落地）。
- **枚举值 / 常量**：`kPascalCase`（如 `kMaxSingleTickMove`、`kReplicationHistoryWindow`）。
- **消息 ID 常量**：定义在 `src/lib/network/message_ids.h` 的 `enum class CellApp / CellAppMgr`
  里，Phase 11 追加新枚举值而不是硬编码数字。

---

## 1. BigWorld 架构分析与 Atlas 适配

### 1.1 BigWorld Real/Ghost 核心机制

| 机制 | BigWorld 实现 | 说明 |
|------|-------------|------|
| **Real/Ghost 标识** | `pReal_` 非 null = Real; `pRealChannel_` 非 null = Ghost | 互斥标志 |
| **Haunt** | `RealEntity::haunts_` (vector\<Haunt\>) | Real 追踪所有 Ghost 位置 |
| **Ghost 创建** | `EntityGhostMaintainer` 拉模型 | Real 主动在相邻 Cell 创建 Ghost |
| **兴趣区域** | `ghostDistance + appealRadius + GHOST_FUDGE(20m)` | 三重缓冲防抖动 |
| **Ghost 同步** | `ghostPositionUpdate` + `ghostHistoryEvent` | 位置 (volatile) + 属性 (event history) |
| **Offload** | `convertRealToGhost()` → `convertGhostToReal()` | 序列化 Real → 目标反序列化 |
| **Offload 通知** | `ghostSetNextReal()` 通知所有 Haunt 新 Real 地址 | Ghost 重定向 |
| **消息转发** | `forwardedBaseEntityPacket()` | Ghost 转发到 Real |
| **CellAppChannel** | 专用 inter-CellApp 通道，批量发送 | Timer 驱动 flush (ghostUpdateHertz) |

### 1.2 BigWorld CellAppMgr 核心机制

| 机制 | BigWorld 实现 | 说明 |
|------|-------------|------|
| **BSP 树** | `InternalNode` (分割线) + `CellData` (叶节点) | 交替水平/垂直分割 |
| **负载均衡** | 比较左右子树负载 → 移动分割线 | 带攻击性衰减防振荡 |
| **Entity Bounds** | 多级实体分布追踪 | 精确知道移动分割线影响哪些实体 |
| **Safety Bound** | `max(固定阈值, 平均负载×比例)` | 防止让已过载的 Cell 更差 |
| **Aggression** | 方向切换时 ×0.9，持续时 ×1.1 | 阻尼收敛 |
| **Ghost Distance** | 配置参数，分割线与实际边界的缓冲区 | 决定 Ghost 存在范围 |

### 1.3 Atlas 适配决策

| 方面 | Atlas 决策 | 原因 |
|------|-----------|------|
| **Real/Ghost 标识** | 对齐 BigWorld: `real_data_` + `real_channel_` 互斥 | 清晰且高效 |
| **Haunt** | 对齐 BigWorld: Real 持有 Haunt 列表 | 拉模型已验证 |
| **Ghost 同步** | 位置: `GhostPositionUpdate`; 属性: `GhostDelta` (C# blob + 序号) | 属性 delta 来自 C# |
| **Offload** | 对齐 BigWorld: 序列化 → 传输 → 反序列化 | 标准做法 |
| **CellAppChannel** | 复用 Atlas 内部 `RUDP` channel + 批量/多消息发送 | 与当前内部通信基线一致 |
| **BSP 树** | 对齐 BigWorld: 交替分割 + 负载均衡 | 简化：初期不实现 EntityBoundLevels |
| **负载均衡** | 简化版: 比较左右负载 + 移动分割线 + aggression | 去掉多级 entity bounds |
| **Ghost 创建** | 对齐 BigWorld 拉模型: GhostMaintainer | 核心机制不变 |
| **C# 脚本** | Ghost 不创建 C# 实例（C++ only） | Ghost 是只读数据副本 |

### 1.4 C# 脚本层的核心影响

**BigWorld:**
```
Real Entity: Python 实例 + 完整属性
Ghost Entity: Python 实例 + 部分属性 (ghosted data)
  → Ghost 也持有 Python 对象用于属性存取
  → Ghost 的 eventHistory 接收属性变更
```

**Atlas:**
```
Real Entity: C# 实例 (GCHandle) + C++ CellEntity
Ghost Entity: C++ GhostEntity ONLY (无 C# 实例)
  → Ghost 只存储位置 + 受众过滤后的 other_snapshot + 有界 other_delta history + 最新复制序号
  → 跨 Cell 观察者必然是 non-owner（owning client 的 Proxy 固定指向 Real 所在 BaseApp）
    所以 Ghost 只需缓存 "other" 受众那一路数据，owner_snapshot / owner_delta 永远落在 Real CellApp
  → Witness 仍按每观察者 EntityCache.last_event_seq / last_volatile_seq 决定发送什么
  → Ghost 的 blob 由 Real 广播过来，原样转发给 Witness
```

**关键简化:**
- BigWorld 的 Ghost 有完整的 Python 对象，可以访问属性
- Atlas 的 Ghost **没有 C# 实例**，只是一个 C++ 数据容器
- Ghost 存储（与 Phase 10 受众模型对齐）:
  - 位置 / 方向 / on_ground（来自 `GhostPositionUpdate`，latest-wins）
  - `other_snapshot`（来自 `CreateGhost` 或后续 `GhostDelta` 里的 full-snapshot 刷新）
  - 有界 `other_delta` history 窗口 + `latest_event_seq` / `latest_volatile_seq`（让观察者的 `last_*_seq` catch-up 逻辑可以原样复用 Phase 10 的 Witness catch-up + snapshot fallback）
- Witness 看到 Ghost 时，依然按照"每观察者自己的发送进度"决定发送 Ghost 缓存的 blob 给客户端
- 这避免了为每个 Ghost 创建 C# 对象的 GC 压力

> **受众模型（Phase 10 PR-C 落地）**：Phase 10 的 `DeltaSyncEmitter` 已经按 8-scope × 2-audience 生成 `OwnerVisibleMask` / `OtherVisibleMask`，`CellEntity::ReplicationState` 有独立的 `owner_snapshot` / `other_snapshot` + `history deque<ReplicationFrame>`（详见 phase10_cellapp.md §3.3）。Phase 11 的 Ghost 就是这张表的"跨进程旁路投影"——只镜像 other 受众的那一列。凡是文档遗留说"`[AllClients]` blob"的地方都已改写为上述语义。

---

## 2. 消息协议设计

### 2.1 Inter-CellApp 消息 (Real ↔ Ghost)

> **CellApp 3000-3099 段已分配（Phase 10）：**
> 3000 `CreateCellEntity` / 3002 `DestroyCellEntity` / 3003 `ClientCellRpcForward` /
> 3004 `InternalCellRpc` / 3010 `CreateSpace` / 3011 `DestroySpace` /
> 3020 `AvatarUpdate` / 3021 `EnableWitness` / 3022 `DisableWitness`。
> 3023-3099 **保留给 Phase 10 扩展**（例如延迟 Offload 通知钩子、
> Watcher 补强等）。Phase 11 从 **3100** 起，参见 `src/lib/network/message_ids.h`。

| 消息 | ID | 方向 | 用途 |
|------|-----|------|------|
| `CreateGhost` | 3100 | Real CellApp → Ghost CellApp | 创建 Ghost 副本，携带 `other_snapshot` 初态 |
| `DeleteGhost` | 3101 | Real CellApp → Ghost CellApp | 删除 Ghost |
| `GhostPositionUpdate` | 3102 | Real → Ghost | 位置/方向 volatile 更新（latest-wins，带 `volatile_seq`） |
| `GhostDelta` | 3103 | Real → Ghost | 按 `other` 受众过滤的 `other_delta` + `event_seq`（复用 Phase 10 `DeltaSyncEmitter` 的 audience mask） |
| `GhostSnapshotRefresh` | 3106 | Real → Ghost | 当 Ghost 的 `last_event_seq` 超出 history 窗口时，Real 主动重灌 `other_snapshot` |
| `GhostSetReal` | 3104 | 新 Real → Ghost | Offload 后通知新 Real 地址 |
| `GhostSetNextReal` | 3105 | 旧 Real → Ghost | Offload 前通知即将迁移 |

### 2.2 Offload 消息

| 消息 | ID | 方向 | 用途 |
|------|-----|------|------|
| `OffloadEntity` | 3110 | 旧 CellApp → 新 CellApp | 传输完整 Real 数据 |
| `OffloadEntityAck` | 3111 | 新 CellApp → 旧 CellApp | 确认接收 |
| `baseapp::CurrentCell` | 2012 | 新 CellApp → BaseApp | 复用现有 BaseApp 入站消息，通知 Base 新的 Cell 地址 |

### 2.3 CellAppMgr 消息

| 消息 | ID | 方向 | 用途 |
|------|-----|------|------|
| `RegisterCellApp` | 7000 | CellApp → CellAppMgr | 注册 |
| `RegisterCellAppAck` | 7001 | CellAppMgr → CellApp | 注册结果 (ID + 配置) |
| `InformCellLoad` | 7002 | CellApp → CellAppMgr | 负载上报 |
| `CreateSpaceRequest` | 7003 | BaseApp/脚本 → CellAppMgr | 创建 Space |
| `AddCellToSpace` | 7004 | CellAppMgr → CellApp | 分配 Cell 给 CellApp |
| `UpdateGeometry` | 7005 | CellAppMgr → CellApp | BSP 树/Cell 边界更新 |
| `ShouldOffload` | 7006 | CellAppMgr → CellApp | 启用/禁用实体迁移 |

### 2.4 详细消息定义

```cpp
namespace atlas::cellapp {

struct CreateGhost {
    EntityID real_entity_id;       // Real 侧 EntityID
    uint16_t type_id;
    Vector3 position;
    Vector3 direction;
    bool on_ground;
    Address real_cellapp_addr;     // Real 所在 CellApp
    Address base_addr;             // BaseApp 地址
    EntityID base_entity_id;       // BaseApp EntityID
    uint64_t event_seq;            // Ghost 的 latest_event_seq 起点
    uint64_t volatile_seq;         // 同上
    std::vector<std::byte> other_snapshot;  // other 受众初态（非 owner 视角）

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

struct GhostPositionUpdate {
    EntityID ghost_entity_id;
    Vector3 position;
    Vector3 direction;
    bool on_ground;
    uint64_t volatile_seq;         // 独立于 event_seq；latest-wins
                                    // （与 Phase 10 `CellEntity::ReplicationFrame::volatile_seq` 对齐）

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

struct GhostDelta {
    EntityID ghost_entity_id;
    uint64_t event_seq;            // 累积有序；对应 Phase 10 event_seq 流
    std::vector<std::byte> other_delta;  // `DeltaSyncEmitter::SerializeOtherDelta` 的 output
                                          // 即 `_dirtyFlags & OtherVisibleMask` 位过滤的增量

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

struct GhostSnapshotRefresh {
    EntityID ghost_entity_id;
    uint64_t event_seq;                      // 新基线的序号
    std::vector<std::byte> other_snapshot;   // 替换 Ghost 缓存的基线
    // Ghost 落后超过 `CellEntity::kReplicationHistoryWindow`（Phase 10 = 8）帧时，
    // 连续 delta 窗口不足以追上 — Real 主动重灌一次快照，Ghost 把
    // `latest_event_seq` 跳到此处，之后继续吃 delta。
};

struct OffloadEntity {
    EntityID real_entity_id;
    uint16_t type_id;
    SpaceID space_id;
    Vector3 position;
    Vector3 direction;
    Address base_addr;
    EntityID base_entity_id;
    // C# GCHandle — 不跨进程传递，接收端用 persistent_blob 重建 handle
    // （原 script_handle 字段此处其实冗余；保留仅为旧设计稿参考，实际落地时去掉）
    std::vector<std::byte> persistent_blob;          // 完整实体状态 — 由 C# ServerEntity.Serialize 产出
                                                      // （Phase 10 已落地的 abstract；不要另造 SerializeFull）
    // Phase 10 复制模型的一致性延续：
    //   把 owner_snapshot / other_snapshot 一并随 Offload 送过去，
    //   接收端可以立刻填 replication_state_ 而无需等第一次 publish_replication_frame
    std::vector<std::byte> owner_snapshot;
    std::vector<std::byte> other_snapshot;
    uint64_t latest_event_seq;                        // 让 Witness 的 last_event_seq 延续
    uint64_t latest_volatile_seq;
    // Controller 状态
    std::vector<std::byte> controller_data;
    // Haunt 列表 (哪些 CellApp 有 Ghost)
    std::vector<Address> existing_haunts;

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

} // namespace atlas::cellapp
```

---

## 3. 核心模块设计

### 3.1 CellEntity 扩展 — Real/Ghost 双模式

Phase 10 的 `CellEntity` 扩展为支持 Real 和 Ghost。**注意 Phase 10 已有的字段不动**：`witness_`、`controllers_`、`range_node_`、`replication_state_`、`script_handle_`、`base_addr_` / `base_entity_id_` 都保留在 `CellEntity` 原位，Phase 11 只**追加** Real/Ghost 区分字段。Witness 在 Phase 10 的析构顺序（`witness_.reset()` → `controllers_.StopAll()` → 合成 FLT_MAX shuffle → `RangeList::Remove`）是 UAF 防护的关键，不能通过把 witness 搬到子对象里而打破。

```cpp
// src/server/cellapp/cell_entity.hpp (扩展)
namespace atlas {

class CellEntity {
public:
    // ========== Real/Ghost 状态 ==========
    [[nodiscard]] auto is_real() const -> bool { return real_data_ != nullptr; }
    [[nodiscard]] auto is_ghost() const -> bool { return real_channel_ != nullptr; }

    [[nodiscard]] auto real_data() -> RealEntityData* { return real_data_.get(); }
    [[nodiscard]] auto real_channel() -> Channel* { return real_channel_; }

    /// Real → Ghost 转换 (Offload 发起方)
    void convert_real_to_ghost(Channel* new_real_channel);

    /// Ghost → Real 转换 (Offload 接收方)
    void convert_ghost_to_real(/* offload data */);

    // ========== Ghost 特有 ==========
    /// 更新 Ghost 的缓存数据 (由 Real 广播)
    /// 注意：GhostPositionUpdate 的 volatile_seq 直接写到
    ///   replication_state_->latest_volatile_seq（复用 Phase 10 字段）；
    ///   不额外维护一个 `ghost_update_number_`。
    void ghost_update_position(const Vector3& pos, const Vector3& dir,
                                bool on_ground, uint64_t volatile_seq);

    /// GhostDelta → 追加到 replication_state_->history
    /// GhostSnapshotRefresh → 重置 replication_state_->other_snapshot
    /// 全部复用 Phase 10 的 `publish_replication_frame` 语义（event_seq > latest_event_seq 时推进）
    void ghost_apply_delta(uint64_t event_seq, std::span<const std::byte> other_delta);
    void ghost_apply_snapshot(uint64_t event_seq, std::span<const std::byte> other_snapshot);

    // `cached_allclients_blob()` 不需要——改读
    // `replication_state_->other_snapshot`（Phase 10 已存在）。

    // ========== 生命周期断言（Phase 10 §10.1 遗留） ==========
    /// EnableWitness / Add*Controller / atlas_set_position 等路径入口都要
    /// `assert(is_real())`——Ghost 是纯数据镜像，脚本钩子只能活在 Real 上。

private:
    // Phase 10 已有字段保持不变：
    //   script_handle_, position_, direction_, on_ground_,
    //   base_addr_, base_entity_id_,
    //   witness_  (unique_ptr; Phase 10 析构顺序关键，不得搬移),
    //   controllers_, range_node_, replication_state_

    // Phase 11 新增：
    std::unique_ptr<RealEntityData> real_data_;  // 非 null = Real
    Channel* real_channel_ = nullptr;             // 非 null = Ghost
    Address next_real_addr_;                       // Offload 过渡期
};

} // namespace atlas
```

**Ghost 与 RangeList：** Ghost 与 Real 一样拥有 `range_node_`，以 `owner_data_ = this`（Phase 10 `EntityRangeListNode::SetOwnerData`，`cell_entity.cc:27`）暴露回指针。这让跨 Cell 的观察者 Witness 能把 Ghost 当作普通 peer 纳入 AoI — AoITrigger 不需要区分 Real/Ghost，只在交付 client-facing envelope 时（Witness 侧）从 Ghost 的 `replication_state_.other_snapshot` 取数据。

### 3.2 RealEntityData — Real 实体扩展数据

**明确职责范围**：`RealEntityData` 只装 Real-only 的分布式状态（haunts、velocity、Offload 过渡）。`witness_`、`controllers_`、`replication_state_` 都留在 Phase 10 既有的 `CellEntity` 里；Witness 的 enable/disable 仍然通过 `CellEntity::EnableWitness` / `DisableWitness` 调用（入口加一行 `assert(is_real())`）。如果把 witness 搬到 RealEntityData 里，Phase 10 的 `~CellEntity` 析构顺序、UAF 防护（见 Phase 10 Round-2 review F1-F3）会失效。

```cpp
// src/server/cellapp/real_entity_data.hpp
namespace atlas {

class RealEntityData {
public:
    explicit RealEntityData(CellEntity& owner);
    ~RealEntityData();

    // ========== Haunt 管理 ==========
    struct Haunt {
        Channel* channel;              // 到 Ghost 所在 CellApp 的通道
        TimePoint creation_time;
    };

    void add_haunt(Channel* channel);
    void del_haunt(Channel* channel);
    void delete_all_ghosts();          // 发送 DeleteGhost 到所有 Haunt

    [[nodiscard]] auto haunts() -> std::vector<Haunt>& { return haunts_; }
    [[nodiscard]] auto haunt_count() const -> size_t { return haunts_.size(); }

    // ========== Ghost 广播 ==========
    //
    // 数据源都读自 owner_ 的 Phase 10 字段：
    //   - 位置/方向/on_ground → owner_.Position() / Direction() / OnGround()
    //   - other_delta → owner_.GetReplicationState()->history 最新帧的 other_delta
    //   - other_snapshot（refresh 时） → owner_.GetReplicationState()->other_snapshot
    //   - event_seq / volatile_seq → owner_.GetReplicationState()->latest_*_seq
    // 这样 Real→Ghost 广播与 Witness 下行走的是同一份 ReplicationState，不会出现
    // "Ghost 看到的序号" 与 "观察者客户端看到的序号" 跨进程错位。

    /// 广播 GhostPositionUpdate 到所有 Haunt（当 volatile_seq 推进时）
    void broadcast_position();

    /// 广播 GhostDelta 到所有 Haunt（当 event_seq 推进时）
    void broadcast_delta();

    /// 主动重灌 other_snapshot — 当某个 Haunt 报告 event_seq 跨窗口
    /// （或 Haunt 刚注册）时发 GhostSnapshotRefresh
    void broadcast_snapshot_refresh(Channel* target);

    // ========== Velocity 追踪 ==========
    [[nodiscard]] auto velocity() const -> const Vector3& { return velocity_; }
    void update_velocity(const Vector3& new_pos, float dt);

    // ========== Offload 序列化 ==========
    void write_offload_data(BinaryWriter& w) const;
    void read_offload_data(BinaryReader& r);

private:
    CellEntity& owner_;
    std::vector<Haunt> haunts_;
    Vector3 velocity_;
    Vector3 position_sample_;
    uint64_t sample_tick_ = 0;

    // 上一次广播已确认推进到的序号，用于检测 Haunt 是否落后超窗口
    uint64_t last_broadcast_event_seq_ = 0;
    uint64_t last_broadcast_volatile_seq_ = 0;
};

} // namespace atlas
```

### 3.3 GhostMaintainer — Ghost 生命周期管理

```cpp
// src/server/cellapp/ghost_maintainer.hpp
namespace atlas {

class GhostMaintainer {
public:
    explicit GhostMaintainer(CellApp& app);

    /// 每隔 N tick 对所有 Real 实体执行 Ghost 维护
    void run();

private:
    /// 检查单个实体的 Ghost 需求
    void check_entity(CellEntity& entity);

    /// 计算兴趣区域
    struct InterestRect {
        float min_x, min_z, max_x, max_z;
    };
    auto calculate_interest_area(const CellEntity& entity) const -> InterestRect;

    /// 查找与兴趣区域重叠的所有 Cell
    auto find_overlapping_cells(const InterestRect& rect) const
        -> std::vector<CellInfo>;

    CellApp& app_;
    float ghost_distance_ = 500.0f;          // 配置: Ghost 创建距离
    float ghost_hysteresis_ = 20.0f;          // 配置: 防抖缓冲 (BigWorld GHOST_FUDGE)
    Duration min_ghost_lifespan_ = Seconds(2); // Ghost 最短存活时间
};

} // namespace atlas
```

**GhostMaintainer::check_entity() 算法（对齐 BigWorld EntityGhostMaintainer）:**

```
check_entity(entity):
    // 只处理 Real 实体
    if (!entity.is_real()) return

    // 1. 标记所有现有 Haunt 为"待删除"
    mark_all_haunts(entity.real_data()->haunts())

    // 2. 计算兴趣区域 = 实体位置 ± (ghostDistance + appealRadius)
    interest = calculate_interest_area(entity)
    // 膨胀 hysteresis 防抖动
    interest.inflate(ghost_hysteresis_)

    // 3. 遍历兴趣区域内的所有 Cell
    for cell in find_overlapping_cells(interest):
        if cell == entity 所在的 Cell:
            continue    // 不在自己的 Cell 创建 Ghost

        haunt = find_haunt_for(cell.cellapp_addr)
        if haunt exists:
            unmark(haunt)    // 取消"待删除"标记
        else:
            // 创建新 Ghost
            entity.real_data()->add_haunt(cell.channel)
            send_create_ghost(cell.channel, entity)

    // 4. 删除仍标记为"待删除"的 Haunt
    //    (条件: 非 Offload 目标, Ghost 存活时间 > min_lifespan)
    for haunt in marked_haunts:
        if haunt.age() > min_ghost_lifespan_:
            send_delete_ghost(haunt.channel, entity)
            entity.real_data()->del_haunt(haunt.channel)
```

### 3.4 OffloadChecker — 实体迁移检测

```cpp
// src/server/cellapp/offload_checker.hpp
namespace atlas {

class OffloadChecker {
public:
    explicit OffloadChecker(CellApp& app);

    /// 每隔 N tick 检查是否有实体需要 Offload
    void run();

private:
    /// 检查单个实体是否越过 Cell 边界
    auto should_offload(const CellEntity& entity) const
        -> std::optional<Address>;  // 返回目标 CellApp 地址

    CellApp& app_;
};

} // namespace atlas
```

**Offload 判断逻辑:**

```
should_offload(entity):
    cell_bounds = entity.cell().bounds()
    pos = entity.position()

    // 实体是否在当前 Cell 的边界外?
    if cell_bounds.contains(pos.x, pos.z):
        return nullopt    // 在边界内，不需要 Offload

    // 查找位置所属的 Cell
    target_cell = space.bsp_tree().find_cell(pos.x, pos.z)
    if target_cell == null || target_cell.cellapp_addr == self:
        return nullopt

    return target_cell.cellapp_addr
```

### 3.5 Offload 流程实现

```
Entity 从 CellApp A (旧) 迁移到 CellApp B (新):

Step 1: CellApp A 检测到实体越界
    OffloadChecker::should_offload(entity) → CellApp B 地址

Step 2: CellApp A 序列化 Real 数据
    entity.real_data()->write_offload_data(writer)
    // 包含: 完整状态 + Controller 数据 + Haunt 列表
    // C# 实体: 调用 NativeApi → C# Serialize() → blob

Step 3: CellApp A 通知所有 Ghost 即将迁移
    for haunt in entity.real_data()->haunts():
        send GhostSetNextReal(next_addr = CellApp B) to haunt

Step 4: CellApp A → CellApp B: 发送 OffloadEntity 消息

Step 5: CellApp B 接收
    if 已有该实体的 Ghost:
        ghost.convert_ghost_to_real(offload_data)
        // Ghost 升级为 Real (零创建延迟!)
    else:
        create new Real entity from offload_data

Step 6: CellApp B 创建 C# 实体
    NativeApi → C# EntityFactory.Create() + Deserialize(blob)
    // 或: C# 实体跨进程迁移 (序列化 → 反序列化)

Step 7: CellApp B 通知所有 Ghost 新 Real 地址
    for haunt_addr in offload_data.existing_haunts:
        send GhostSetReal(new_real_addr = CellApp B)

Step 8: CellApp B → BaseApp: 发送 CurrentCell
    BaseApp 更新 base_entity.cell_addr_ → CellApp B

Step 9: CellApp A 清理
    entity.convert_real_to_ghost(channel_to_B)
    // Real → Ghost (或直接删除)

Step 10: CellApp B 运行 GhostMaintainer
    // 为新 Real 创建/更新 Ghost

客户端完全无感知: 所有通信经 BaseApp Proxy 中转
```

### 3.6 Cell — Space 的子区域

Phase 10 中 Space = Cell (单 CellApp)。Phase 11 分离：

```cpp
// src/server/cellapp/cell.hpp
namespace atlas {

struct CellBounds {
    float min_x, min_z, max_x, max_z;

    [[nodiscard]] auto contains(float x, float z) const -> bool;
    [[nodiscard]] auto area() const -> float;
};

class Cell {
public:
    Cell(Space& space, const CellBounds& bounds);

    [[nodiscard]] auto bounds() const -> const CellBounds& { return bounds_; }
    void set_bounds(const CellBounds& b) { bounds_ = b; }

    // ---- Real 实体管理 ----
    void add_real_entity(CellEntity* entity);
    void remove_real_entity(CellEntity* entity);

    [[nodiscard]] auto real_entity_count() const -> size_t;

    template<typename Fn>
    void for_each_real_entity(Fn&& fn);

    // ---- 负载 ----
    [[nodiscard]] auto should_offload() const -> bool { return should_offload_; }
    void set_should_offload(bool v) { should_offload_ = v; }

private:
    Space& space_;
    CellBounds bounds_;
    std::vector<CellEntity*> real_entities_;  // swap-back O(1) 删除
    bool should_offload_ = false;
};

} // namespace atlas
```

### 3.7 BSP 树

```cpp
// src/server/cellappmgr/bsp_tree.hpp
namespace atlas {

using CellID = uint32_t;

struct CellInfo {
    CellID cell_id;
    Address cellapp_addr;
    CellBounds bounds;
    float load = 0.0f;
    uint32_t entity_count = 0;
};

class BSPNode {
public:
    virtual ~BSPNode() = default;

    /// 查询点所在的 Cell
    [[nodiscard]] virtual auto find_cell(float x, float z) const
        -> const CellInfo* = 0;

    /// 查询与矩形重叠的所有 Cell
    virtual void visit_rect(float min_x, float min_z,
                             float max_x, float max_z,
                             std::function<void(const CellInfo&)> visitor) const = 0;

    /// 更新负载
    virtual void update_load() = 0;

    /// 负载均衡 (移动分割线)
    virtual void balance(float safety_bound) = 0;

    /// 序列化 (发给 CellApp)
    virtual void serialize(BinaryWriter& w) const = 0;
    static auto deserialize(BinaryReader& r) -> std::unique_ptr<BSPNode>;
};

/// 叶节点 — 一个 Cell
class BSPLeaf : public BSPNode {
public:
    explicit BSPLeaf(CellInfo info);

    auto find_cell(float x, float z) const -> const CellInfo* override;
    void visit_rect(float min_x, float min_z, float max_x, float max_z,
                     std::function<void(const CellInfo&)> visitor) const override;
    void update_load() override {}
    void balance(float safety_bound) override {}

    [[nodiscard]] auto info() -> CellInfo& { return info_; }

private:
    CellInfo info_;
};

/// 内部节点 — 分割线
class BSPInternal : public BSPNode {
public:
    enum class Axis { X, Z };

    BSPInternal(Axis axis, float position,
                 std::unique_ptr<BSPNode> left,
                 std::unique_ptr<BSPNode> right);

    auto find_cell(float x, float z) const -> const CellInfo* override;
    void visit_rect(float min_x, float min_z, float max_x, float max_z,
                     std::function<void(const CellInfo&)> visitor) const override;
    void update_load() override;
    void balance(float safety_bound) override;

private:
    Axis axis_;
    float position_;                 // 分割线位置
    std::unique_ptr<BSPNode> left_;  // axis 负方向
    std::unique_ptr<BSPNode> right_; // axis 正方向

    // 负载均衡状态
    float left_load_ = 0.0f;
    float right_load_ = 0.0f;
    float aggression_ = 1.0f;
    enum class Direction { None, Left, Right } prev_direction_ = Direction::None;
};

class BSPTree {
public:
    BSPTree() = default;

    void set_root(std::unique_ptr<BSPNode> root) { root_ = std::move(root); }

    [[nodiscard]] auto find_cell(float x, float z) const -> const CellInfo*;

    void visit_rect(float min_x, float min_z, float max_x, float max_z,
                     std::function<void(const CellInfo&)> visitor) const;

    void update_load();
    void balance(float safety_bound);

    void serialize(BinaryWriter& w) const;
    static auto deserialize(BinaryReader& r) -> BSPTree;

    /// 分裂叶节点 (添加新 Cell)
    void split(CellID cell_id, BSPInternal::Axis axis,
               float position, const CellInfo& new_cell);

private:
    std::unique_ptr<BSPNode> root_;
};

} // namespace atlas
```

**负载均衡算法（简化版，对齐 BigWorld 核心逻辑）:**

```cpp
void BSPInternal::balance(float safety_bound) {
    // 递归更新子树负载
    left_->update_load();
    right_->update_load();

    float diff = left_load_ - right_load_;
    Direction direction = (diff > 0.01f) ? Direction::Left :
                          (diff < -0.01f) ? Direction::Right :
                          Direction::None;

    if (direction == Direction::None) return;

    // Safety: 不让已过载的一侧更差
    float growing_load = (direction == Direction::Left) ? right_load_ : left_load_;
    if (growing_load >= safety_bound) return;

    // Aggression 阻尼
    if (direction != prev_direction_ && prev_direction_ != Direction::None) {
        aggression_ *= 0.9f;  // 方向切换 → 减速
    } else {
        aggression_ = std::min(aggression_ * 1.1f, 2.0f);
    }
    prev_direction_ = direction;

    // 移动分割线
    float move = diff * 0.1f * aggression_;  // 每次移动 10% × aggression
    position_ += move;

    // 更新子节点边界
    // → 触发 CellApp 的 Cell bounds 更新
    // → CellApp 的 OffloadChecker 根据新边界决定是否迁移实体
}
```

### 3.8 CellAppMgr 进程

```cpp
// src/server/cellappmgr/cellappmgr.hpp
namespace atlas {

class CellAppMgr : public ManagerApp {
public:
    using ManagerApp::ManagerApp;

protected:
    auto init(int argc, char* argv[]) -> bool override;
    void fini() override;
    void on_tick_complete() override;
    void register_watchers() override;

private:
    // ---- CellApp 管理 ----
    void on_register_cellapp(const Address& src, Channel* ch,
                              const cellappmgr::RegisterCellApp& msg);
    void on_inform_load(const Address& src, Channel* ch,
                         const cellappmgr::InformCellLoad& msg);
    void on_cellapp_death(const machined::DeathNotification& notif);

    // ---- Space 管理 ----
    void on_create_space_request(const Address& src, Channel* ch,
                                  const cellappmgr::CreateSpaceRequest& msg);

    // ---- 定期任务 ----
    void load_balance();             // 每 ~1s
    void send_geometry_updates();    // BSP 变更后

    // ---- CellApp 集合 ----
    struct CellAppInfo {
        Address addr;
        uint32_t app_id = 0;
        float load = 0.0f;
        float smoothed_load = 0.0f;
        uint32_t entity_count = 0;
        bool is_ready = false;
    };
    std::unordered_map<Address, CellAppInfo> cellapps_;

    // ---- Space 分区 ----
    struct SpacePartition {
        SpaceID space_id;
        BSPTree bsp;
    };
    std::unordered_map<SpaceID, SpacePartition> spaces_;

    // ---- 负载均衡 ----
    TimerHandle balance_timer_;
    float load_smoothing_bias_ = 0.3f;
    float safety_bound_ = 0.9f;

    uint32_t next_app_id_ = 0;
    uint32_t next_cell_id_ = 0;
};

} // namespace atlas
```

---

## 4. C# 实体跨进程 Offload

**核心问题:** C# 实体实例通过 GCHandle 绑定到当前进程的 CLR。Offload 到另一个 CellApp 进程时，GCHandle 不可跨进程传递。

**方案: 序列化 → 销毁 → 反序列化（直接复用 Phase 10 的 `Serialize` / `Deserialize` 抽象）**

```
CellApp A (旧 Real):
  1. C++ CellApp 通过 NativeCallbacks 的 SerializeEntity 回调触发 C# 序列化
     （方向是 C++ → C#；不是 atlas_* export。见下方 §11.10 修正）
  2. C# entity.Serialize(ref SpanWriter) → 完整状态 blob
     （`ServerEntity.Serialize` 已经是 Phase 10 的 abstract；generator 已覆盖所有 .def 属性）
  3. GCHandle.Free() → C# 实例可被 GC
  4. 发送 blob 到 CellApp B（OffloadEntity 消息）

CellApp B (新 Real):
  1. 收到 blob
  2. C++ CellApp 通过 NativeCallbacks 的 RestoreEntity 回调（Phase 10 已存在）
     触发 C# 侧创建
  3. C# EntityFactory.Create(typeName) → 新实例 → entity.Deserialize(ref SpanReader) → 恢复状态
  4. GCHandle.Alloc() → 新 handle 经回调回传
  5. CellEntity::SetScriptHandle(handle)
```

> **复用 Phase 10，不引入新方法：**
> `ServerEntity.Serialize(ref SpanWriter)` 和 `Deserialize(ref SpanReader)` 已经是
> abstract 的 "完整状态" 序列化（src/csharp/Atlas.Runtime/Entity/ServerEntity.cs:25-28），
> DefGenerator 已经按所有 `.def` 属性生成了实现。
> **不要**为 Offload 新造 `SerializeFull()` / `SerializePersistent()` 对；
> 如果将来需要"只含 persistent" 的变种，另行单独讨论并保持命名与现有 abstract 对齐。
>
> 受众过滤的 `SerializeForOwnerClient` / `SerializeForOtherClients` 是 **AoI 复制**用，
> **与 Offload 无关**；二者不要混用。

---

## 5. 实现步骤

### Step 11.1: CellEntity Real/Ghost 扩展

**更新文件:**
```
src/server/cellapp/cell_entity.hpp / .cpp      (扩展 Real/Ghost 字段)
src/server/cellapp/real_entity_data.hpp / .cpp (新增)
tests/unit/test_real_ghost.cpp
```

**测试用例:**
- Real → Ghost 转换 (`convert_real_to_ghost`)
- Ghost → Real 转换 (`convert_ghost_to_real`)
- Ghost 位置更新缓存
- Ghost delta 缓存
- Haunt 添加/删除
- 广播位置/delta 到所有 Haunt
- Offload 序列化/反序列化往返

### Step 11.2: Inter-CellApp 消息定义

**新增文件:**
```
src/server/cellapp/intercell_messages.hpp
tests/unit/test_intercell_messages.cpp
```

### Step 11.3: Cell 分区

**新增文件:**
```
src/server/cellapp/cell.hpp / .cpp
tests/unit/test_cell.cpp
```

### Step 11.4: GhostMaintainer

**新增文件:**
```
src/server/cellapp/ghost_maintainer.hpp / .cpp
tests/unit/test_ghost_maintainer.cpp
```

**测试用例（关键）:**
- 实体进入 Ghost 区域 → 创建 Ghost
- 实体离开 Ghost 区域 → 删除 Ghost（滞后 + min lifespan）
- 兴趣区域计算（ghostDistance + hysteresis）
- 多 Cell 重叠区域正确处理
- Ghost 创建/删除的批量效率

### Step 11.5: OffloadChecker + Offload 流程

**新增文件:**
```
src/server/cellapp/offload_checker.hpp / .cpp
tests/unit/test_offload.cpp
```

**测试用例:**
- 实体越界检测
- Real→Ghost→Real 完整 Offload 往返
- Offload 期间消息缓冲（`GhostSetNextReal` → 暂停接收旧 Real 消息）
- BaseApp 收到 `CurrentCell` 更新路由
- C# 实体序列化 → 反序列化状态一致

### Step 11.6: BSP 树

**新增文件:**
```
src/server/cellappmgr/bsp_tree.hpp / .cpp
tests/unit/test_bsp_tree.cpp
```

**测试用例:**
- find_cell (点查询)
- visit_rect (范围查询)
- 分裂 (split)
- 序列化/反序列化
- 负载均衡移动分割线
- Aggression 阻尼收敛

### Step 11.7: CellAppMgr 消息定义

**新增文件:**
```
src/server/cellappmgr/cellappmgr_messages.hpp
tests/unit/test_cellappmgr_messages.cpp
```

### Step 11.8: CellAppMgr 进程

**新增文件:**
```
src/server/cellappmgr/
├── CMakeLists.txt
├── main.cpp
├── cellappmgr.hpp / .cpp
```

**实现顺序:**
1. 基本启动 (ManagerApp + machined)
2. CellApp 注册管理
3. 负载上报 + 平滑
4. Space 创建 + BSP 初始化
5. Cell 分配给 CellApp
6. 定期负载均衡 (移动分割线)
7. 发送 UpdateGeometry 到 CellApp
8. 发送 ShouldOffload 控制信号
9. CellApp 死亡处理
10. Watcher 注册

### Step 11.9: CellApp 集成更新

**更新文件:**
```
src/server/cellapp/cellapp.hpp / .cpp
src/server/cellapp/cellapp_native_provider.hpp / .cpp   (Add*Controller / EnableWitness 入口加 assert)
src/server/cellapp/space.hpp / .cpp
src/server/baseapp/baseapp.hpp / .cpp                   (cellapp_channel_ → 多路由表)
```

- Space 持有 Cell + BSP 树 (从 CellAppMgr 接收)
- 处理 CreateGhost / DeleteGhost / GhostPositionUpdate / GhostDelta / GhostSnapshotRefresh
- 处理 OffloadEntity (接收端)
- 发送 OffloadEntity (发送端)
- 处理 UpdateGeometry (更新 Cell 边界)
- 定期运行 GhostMaintainer + OffloadChecker
- 处理 ShouldOffload 控制
- **所有写入口加 `assert(entity->is_real())`**（Ghost 是只读镜像）：
  - `CellAppNativeProvider::AddMoveController` / `AddTimerController` / `AddProximityController`
  - `CellAppNativeProvider::SetEntityPosition`（Ghost 不允许从 C# 脚本改位置；
    Ghost 位置只能由 `GhostPositionUpdate` 写入）
  - `CellEntity::EnableWitness`

**BaseApp 侧改造（Phase 10 遗留单 channel 模型升级）：**

Phase 10 的 `BaseApp` 只维护**一个** `cellapp_channel_{nullptr}`（`src/server/baseapp/baseapp.h:155`），
通过 machined 订阅**单个** CellApp 地址。Phase 11 需要改成按 Space/Cell 分路由的表：

```cpp
// baseapp.h (Phase 11)
std::unordered_map<Address, Channel*> cellapp_channels_;  // all known CellApps
std::unordered_map<EntityID, Address> base_id_to_cell_addr_;  // 当前实体的 Cell 路由
```

- `ClientCellRpcForward` 的 `cellapp_channel_->SendMessage(...)` 改为：
  1. 按 `msg.target_entity_id` 查 `base_id_to_cell_addr_` → 目标 CellApp 地址
  2. 按地址查 `cellapp_channels_` → `Channel*`
  3. 发送
- `baseapp::CurrentCell` (2012) 已经存在（Phase 10 预留）：CellApp Offload 成功后发给 BaseApp，
  BaseApp 更新 `base_id_to_cell_addr_`
- machined 订阅改为监听 **所有** CellApp 的 Birth/Death，分别注册/清理 channel

### Step 11.10: NativeApi / NativeCallbacks 扩展（Offload + Ghost 生命周期）

**方向说明**：`ATLAS_NATIVE_API` 是 **C# → C++** 方向（C# 脚本主动调 C++，如 Phase 10 的
`atlas_set_position` / `atlas_publish_replication_frame`）。Offload 的序列化触发是 **C++ → C#**
方向：CellApp 进程决定某个实体要 Offload，需要**让 C# 把该实体序列化出来**。
这走的是 `NativeCallbacks`（C# 在 `SetNativeCallbacks` 时注册的函数指针表），**不是 atlas_* 导出**。

**更新文件:**
```
src/lib/clrscript/native_api_provider.hpp           (C++ → C# callback 接口)
src/server/baseapp/baseapp_native_provider.hpp      (NativeCallbacks 表 + 类型别名)
src/server/cellapp/cellapp_native_provider.hpp      (新增 SerializeEntity 回调字段)
src/csharp/Atlas.Runtime/Core/NativeCallbacks.cs    (表里追加 Serialize 入口)
```

**C++ 侧 NativeCallbacks 表新增条目（Phase 10 已有 `RestoreEntityFn` / `DispatchRpcFn` /
`EntityDestroyedFn`，见 `src/server/baseapp/baseapp_native_provider.h`；Phase 11 追加）：**

```cpp
// 触发 C# 把 entity 完整序列化到给定 writer；out_len 回传实际字节数。
// entity 必须处于 Real 状态（脚本实例只存在于 Real CellApp）。
// 零拷贝约定：C# 侧写入 caller 提供的 buffer；若容量不足，C# 返回
// 所需大小而不写入，C++ 分配更大 buffer 后重试。
using SerializeEntityFn = int32_t (*)(uint32_t entity_id,
                                      uint8_t* out_buf,
                                      int32_t out_buf_cap,
                                      int32_t* out_len);
```

**C# 侧**：`Atlas.Runtime.Core.NativeCallbacks` 表里追加 `SerializeEntity`，内部直接调
Phase 10 已经存在的 abstract `ServerEntity.Serialize(ref SpanWriter)` —
**不新增 `SerializeFull`**，复用现成抽象（见 §4 的说明）。

**atlas_* 导出（C# → C++ 方向）新增（Phase 11）：**

```cpp
// Ghost 管理（由 Real CellApp 的 C# 脚本或 C++ 自身触发 — 当前计划主要是 C++ 自驱动
// 的 GhostMaintainer，这组 atlas_* 可能最终只用在诊断/watcher 手动触发场景，
// 如无需要可不实现）
ATLAS_NATIVE_API void atlas_request_offload(uint32_t entity_id);
```

**Phase 10 follow-up 要求在这个 Step 一并落实的断言** — 所有已有的 `CellAppNativeProvider`
写入口（Phase 10 已经落地）都要加 `is_real()` 检查：

```cpp
// 以 SetEntityPosition 为例（src/server/cellapp/cellapp_native_provider.cc）
void CellAppNativeProvider::SetEntityPosition(uint32_t entity_id, float x, float y, float z) {
  auto* entity = lookup_ ? lookup_(entity_id) : nullptr;
  if (!entity) { /* log, return */ }
  // Phase 11 新增：Ghost 是只读镜像，脚本试图改 Ghost 位置是 bug
  if (!entity->IsReal()) {
    ATLAS_LOG_WARNING("atlas_set_position on Ghost (entity_id={}) — rejected", entity_id);
    return;
  }
  entity->SetPosition(math::Vector3{x, y, z});
}
```

同样的守卫加到 `AddMoveController` / `AddTimerController` / `AddProximityController` /
`PublishReplicationFrame`。`CellEntity::EnableWitness` 入口加 `assert(IsReal())`（或运行期
日志+返回，视脚本错误处理策略）。

### Step 11.11: 集成测试

**新增文件:**
```
tests/integration/test_distributed_space.cpp
```

端到端场景（machined + DBApp + BaseAppMgr + BaseApp + CellAppMgr + 2×CellApp + LoginApp）：
1. 全部启动，CellAppMgr 创建 Space 并分配 2 个 Cell
2. 实体在 Cell A 创建 → AOI 工作
3. 实体移向 Cell 边界 → Ghost 在 Cell B 创建
4. 另一实体在 Cell B 的 Witness 看到 Ghost
5. 实体越过边界 → Offload 到 Cell B
6. BaseApp 收到 CurrentCell 更新
7. 客户端无断线，AOI 持续工作
8. 返回 Cell A → 反向 Offload
9. 负载均衡: 人为增加 Cell A 负载 → 分割线移动

---

## 6. 文件清单汇总

```
src/server/cellapp/                    (扩展 + 新增)
├── cell_entity.hpp / .cpp              (扩展: Real/Ghost)
├── real_entity_data.hpp / .cpp         (新增)
├── cell.hpp / .cpp                     (新增)
├── ghost_maintainer.hpp / .cpp         (新增)
├── offload_checker.hpp / .cpp          (新增)
├── intercell_messages.hpp              (新增)
├── cellapp.hpp / .cpp                  (扩展)
├── space.hpp / .cpp                    (扩展)

src/server/cellappmgr/                 (新增)
├── CMakeLists.txt
├── main.cpp
├── cellappmgr.hpp / .cpp
├── cellappmgr_messages.hpp
├── bsp_tree.hpp / .cpp

src/lib/clrscript/                     (扩展)
├── native_api_provider.hpp
├── clr_native_api.hpp / .cpp

tests/unit/
├── test_real_ghost.cpp
├── test_intercell_messages.cpp
├── test_cell.cpp
├── test_ghost_maintainer.cpp
├── test_offload.cpp
├── test_bsp_tree.cpp
├── test_cellappmgr_messages.cpp

tests/integration/
└── test_distributed_space.cpp
```

---

## 7. 依赖关系与执行顺序

```
Step 11.2: 消息定义                   ← 无依赖
Step 11.3: Cell                        ← 无依赖
Step 11.6: BSP 树                      ← 无依赖
Step 11.7: CellAppMgr 消息            ← 无依赖

Step 11.1: CellEntity Real/Ghost      ← 依赖 11.2
Step 11.4: GhostMaintainer            ← 依赖 11.1 + 11.3
Step 11.5: OffloadChecker             ← 依赖 11.1 + 11.3

Step 11.8: CellAppMgr 进程            ← 依赖 11.6 + 11.7
Step 11.9: CellApp 集成               ← 依赖 11.1 + 11.4 + 11.5
Step 11.10: NativeApi 扩展            ← 依赖 11.5

Step 11.11: 集成测试                  ← 依赖全部
```

**推荐执行顺序:**

```
第 1 轮 (并行): 11.2 消息 + 11.3 Cell + 11.6 BSP + 11.7 CellAppMgr 消息
第 2 轮:        11.1 CellEntity Real/Ghost 扩展
第 3 轮 (并行): 11.4 GhostMaintainer + 11.5 OffloadChecker + 11.8 CellAppMgr
第 4 轮 (并行): 11.9 CellApp 集成 + 11.10 NativeApi
第 5 轮:        11.11 集成测试
```

---

## 8. BigWorld 完整对照

| BigWorld | Atlas | 差异说明 |
|----------|-------|---------|
| `RealEntity` 类 (~1000 LOC) | `RealEntityData` (~400 LOC) | Atlas 无 Python 对象管理 |
| `Entity::initGhost()` Python 实例 | Ghost 无 C# 实例 | **核心简化:** C++ only |
| `Haunt` (Channel + time) | `Haunt` (Channel + time) | 一致 |
| `EntityGhostMaintainer` 拉模型 | `GhostMaintainer` 拉模型 | 算法一致 |
| `ghostDistance + appealRadius + GHOST_FUDGE` | `ghost_distance + hysteresis` | 简化配置 |
| `ghostPositionUpdate` + `ghostHistoryEvent` | `GhostPositionUpdate` + `GhostDelta` | Atlas delta 来自 C# blob + 复制序号 |
| `convertRealToGhost()` 保留 Python 对象 | `convert_real_to_ghost()` 释放 C# | C# 实例不跨进程 |
| `convertGhostToReal()` 已有 Python 对象 | `convert_ghost_to_real()` 反序列化新 C# | 需完整反序列化 |
| BSP InternalNode + CellData | BSPInternal + BSPLeaf | 结构一致 |
| EntityBoundLevels (多级追踪) | 简化: 直接按负载差移动 | 去掉多级复杂度 |
| Aggression 阻尼 (0.9/1.1) | Aggression 阻尼 (0.9/1.1) | 一致 |
| `CellAppChannel` (UDP + 批量 flush) | RUDP channel + tick flush | Atlas 沿用现有内部可靠 UDP 基线 |
| `forwardedBaseEntityPacket()` | Ghost 转发到 Real | 一致模式 |
| Ghost Controller (GHOST_ONLY domain) | Ghost 无 Controller | Atlas 简化 |
| `ghostSetNextReal` + `ghostSetReal` | 一致 | 防消息乱序 |

---

## 9. 关键设计决策记录

### 9.1 Ghost 无 C# 实例

**决策: Ghost 是纯 C++ 数据容器，不创建 C# 对象。**

BigWorld 的 Ghost 持有完整 Python 对象，因为 Python 属性需要原生对象来访问。
Atlas 中属性以 blob 形式在 C++ 层流转，Witness 发送给客户端时不需要解析。

优势:
- 避免为每个 Ghost 创建 C# GCHandle（跨 Cell 边界可能有大量 Ghost）
- GC 压力小
- C++ 层完全可控

代价:
- Ghost 上不能执行脚本逻辑（BigWorld 的 Ghost 可以运行 GHOST_ONLY Controller）
- Atlas 的 Controller 全部只在 Real 上执行

### 9.2 Offload 时 C# 实体序列化

**决策: 复用 Phase 10 已落地的 `Serialize` / `Deserialize` abstract，不新增 `SerializeFull`。**

不尝试"迁移" GCHandle（CLR 不支持跨进程），而是：
1. 旧进程 C++ 通过 NativeCallbacks 的 `SerializeEntity`（Phase 11 新增）触发 C# 序列化
2. C# `ServerEntity.Serialize(ref SpanWriter)`（Phase 10 已有 abstract，generator 已覆盖全部
   `.def` 属性）→ 完整状态 blob
3. 旧进程 `GCHandle.Free()`
4. 新进程通过 NativeCallbacks 的 `RestoreEntity`（Phase 10 已有）触发
   `EntityFactory.Create()` + `ServerEntity.Deserialize(ref SpanReader)`

**Phase 10 的 `Serialize` 已是完整状态序列化**（不是受众过滤版），Source Generator
对所有 `.def` 属性都已覆盖（见 `src/csharp/Atlas.Generators.Def/Emitters/` 与
`src/csharp/Atlas.Runtime/Entity/ServerEntity.cs:25-28`）。**不要**为 Offload 再造
`SerializeFull` / `DeserializePersistent` 之类的并行对 —— 会分裂 "实体状态的唯一写入口"
这个 Phase 10 契约。

受众过滤的 `SerializeForOwnerClient` / `SerializeForOtherClients` 是 AoI 复制路径，
**与 Offload 无关**，二者不要混用（Phase 10 §10.3 follow-up 已记录）。

### 9.3 BSP 负载均衡简化

**决策: 去掉 BigWorld 的 EntityBoundLevels，用简单负载差驱动。**

BigWorld 追踪多级实体分布（知道哪些负载的实体在哪个位置），用于精确计算分割线移动量。
Atlas 初期用简单公式: `move = load_diff * 0.1 * aggression`。

足够实现基本负载均衡。如果需要更精细控制，可后续加入 EntityBoundLevels。

### 9.4 Inter-CellApp 通信用 RUDP

**决策: 复用 Atlas 现有内部 RUDP channel。**

BigWorld 用专用 UDP CellAppChannel + 定时器 flush。
Atlas 当前服务进程之间已经统一跑在 `NetworkInterface::start_rudp_server()` /
`connect_rudp_nocwnd()` 这一套内部可靠 UDP 基线上。Phase 11 不应再单独引入一套 TCP-only
控制面，否则会让 BaseApp / CellApp / Manager 的内部通信模型再次分裂。

工程约束:

- inter-CellApp 与 CellAppMgr 通信优先复用现有内部 `RUDP`
- 仍可按 tick 进行 bundle/flush，模拟 BigWorld `CellAppChannel` 的批量发送语义
- 若后续压测证明大包重传或 head-of-line 行为不可接受，再单独评估新 lane / 新承载，而不是先文档分叉

### 9.5 初期不实现的功能

| 功能 | 原因 | 何时实现 |
|------|------|---------|
| Ghost Controller (GHOST_ONLY) | Ghost 无 C# 实例 | 如需要可加 C++ only Controller |
| EntityBoundLevels | 简化优先 | 按需 |
| Meta Load Balance (CellApp 组) | 需要更多 CellApp | 按需 |
| Space 动态分裂/合并 Cell | 初期固定分区 | 按需 |
| Vehicle 跨 Cell | 复杂 | 按需 |

### 9.6 Phase 10 §10.1 硬约束的落实去处（对照表）

Phase 10 doc §10.1 列出了 4 条"必须在 Phase 11 解决"的遗留项。本文具体落实位置如下：

| Phase 10 §10.1 约束 | 本文落实位置 |
|---|---|
| 多 CellApp `ClientCellRpcForward` 的信任边界 | §2.1 `ClientCellRpcForward` 信封要加 `source_forwarding_cellapp`；§2.3 CellAppMgr 维护可信 peer 白名单（基于 machined 注册）；Step 11.8 CellAppMgr 实现时校验 |
| EntityID 跨 CellApp 唯一 | Step 11.8 CellAppMgr 实现顺序第 2 项："EntityID 按 app_id 高位分段 / 或 cluster-wide 递增 ID 池" — 替换 Phase 10 的 `CellApp::next_entity_id_` 本地单调 |
| 客户端 RPC 速率 / CPU 预算 | §9（新）"分布式下的 rate limit / CPU 预算" — 走 machined 层统一 rate limiter（与 Offload 消息共用预算） |
| Offload / Real↔Ghost 延迟销毁不得恢复 `Space::Tick` silent compaction | §3.5 Offload 流程 Step 9 明说："CellApp A 清理经 CellApp 显式的通知钩子完成，不走 Space 内部 compaction"；Step 11.9 在 `~CellEntity` 的现有 FLT_MAX shuffle 前增加通知 hook |

另外从 Phase 10 §10.2-§10.3 follow-up 带下来的**代码硬化项**（Phase 11 顺手收尾）：

| Phase 10 follow-up | Phase 11 去处 |
|---|---|
| §10.2 #9 ProximityController C# 回调 wiring | Step 11.10 NativeCallbacks 表补 `ProximityEvent` 条目 |
| §10.3 #12 SerializeOwnerDelta/OtherDelta 空 flags 短路 | Step 11.1 Real→Ghost 广播路径里，`RealEntityData::broadcast_delta` 检测 `other_delta.size() <= 1` 时 skip（避免每 tick 1-byte envelope 灌满 inter-cell 带宽）|
| §10.3 #13 `SerializeReplicatedDelta*` legacy API 标 deprecated | Step 11.9 把 Phase 10 legacy 的 reliable/unreliable delta 路径从 Witness 下行中彻底移除，统一走 `BuildAndConsumeReplicationFrame` |
