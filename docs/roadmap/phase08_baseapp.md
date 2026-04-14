# Phase 8: BaseApp — 实体宿主与客户端代理

> 前置依赖: Phase 7 (DBApp), Phase 5 (ScriptApp/EntityApp), Script Phase 4 最小实体/RPC 子集
> BigWorld 参考: `server/baseapp/baseapp.hpp`, `server/baseapp/base.hpp`, `server/baseapp/proxy.hpp`
> 当前代码基线 (2026-04-12): BaseApp 已实现内部消息、登录准备/认证、DB 持久化、`give_client_to()` 本地转移以及 NativeApi 回调；客户端外部接口目前只有 `Authenticate / AuthenticateResult`，`ClientBaseRpc`/`ClientCellRpc`、`EnableEntities` 和 AOI 下行协议仍待 Phase 10 / Phase 12 补齐。另需注意：当前 `SelfRpcFromCell` / `ReplicatedDeltaFromCell` / `BroadcastRpcFromCell` handler 都只会路由到单个 `base_entity_id` 对应的 Proxy，尚未具备 BigWorld 风格的多观察者 fan-out。

---

## 目标

实现 Atlas 的**第一个真正的游戏服务器进程**。BaseApp 持有实体的 Base 部分，
管理客户端连接，处理 RPC 路由和数据库持久化。

## 验收标准

- [x] BaseApp 可启动，注册到 machined，初始化 C# 脚本引擎
- [x] 可创建 Base 实体，C# 脚本逻辑 (`OnInit`, `OnTick`) 可执行
- [x] 可创建 Proxy 实体，接受客户端 RUDP 连接
- [ ] 客户端 exposed RPC 调用经安全校验后分发到 C# 实体
- [ ] C# 实体 `[ClientRpc]` 调用经 C++ 路由发送到客户端
- [x] `WriteToDB()` 将 `[Persistent]` 属性持久化到 DBApp
- [x] `CreateEntityFromDB()` 从 DBApp 加载实体并恢复 C# 状态
- [x] `GiveClientTo()` 可在本地 Proxy 之间转移客户端连接
- [~] `[Replicated]` 属性增量同步到 Proxy 的客户端
- [x] 全部新增代码有单元测试

## 验收状态（2026-04-13）

- 当前 BaseApp 主体已经具备“实体宿主 + 登录准备 + DB 往返 + 本地客户端转移”的可用基线。
- 未完成项主要集中在真正的外部客户端协议闭环: `ClientBaseRpc`/`ClientCellRpc`、稳定的 `[ClientRpc]` 下行协议，以及 AOI/多观察者复制。

---

## 1. BigWorld 架构分析与 Atlas 适配

### 1.1 BigWorld BaseApp 核心机制

| 机制 | BigWorld 实现 | 说明 |
|------|-------------|------|
| **类层次** | `BaseApp : EntityApp : ScriptApp : ServerApp` | 带 Python 脚本和后台任务 |
| **实体集合** | `Bases` (EntityID → Base*) | 简单 map |
| **Base 实体** | `Base : PyObjectPlus` | 持有 Python 对象实例 |
| **Proxy 实体** | `Proxy : Base` + 客户端 Channel | 管理外部 UDP/TCP 连接 |
| **双网络接口** | `intInterface_` (内部) + `extInterface_` (外部) | 内部与外部隔离 |
| **Cell 关联** | `CellEntityMailBox` + `pChannel_` | 指向 CellApp 上的 Real Entity |
| **消息路由** | `setClient(entityID)` 设置当前实体 | 后续消息分发到该实体 |
| **客户端 RPC** | `exposedMethodFromMsgID()` 校验 + `callMethod()` | 只允许 exposed 方法 |
| **giveClientTo** | 本地直接转移 / 远程通过 `acceptClient` 消息 | 支持跨 BaseApp |
| **writeToDB** | 序列化 → 可选请求 CellData → 发送到 DBApp | WriteDBFlags 控制行为 |
| **Entity ID 分配** | `EntityCreator` (BaseAppMgr 分配 ID 区间) | 全局唯一 |
| **Session Key** | uint32 随机值，登录时发给客户端 | 后续消息认证 |
| **ProxyPusher** | 定时器驱动发送 Bundle 到客户端 | 保证数据推送 |
| **Bandwidth 管理** | 下载速率自适应 + 背压检测 | 防止客户端过载 |

### 1.2 Atlas 适配决策

| 方面 | Atlas 决策 | 原因 |
|------|-----------|------|
| **类层次** | `BaseApp : EntityApp : ScriptApp : ServerApp` | 与 Phase 5 一致 |
| **Base 实体** | `BaseEntity` — C++ 薄壳 + C# 实体实例 | 游戏逻辑全在 C# |
| **Proxy 实体** | `Proxy : BaseEntity` + RUDP Channel | 与当前代码一致，复用 Atlas 可靠 UDP 传输 |
| **双网络接口** | 保留: 内部 + 外部两个 `NetworkInterface` | 安全隔离，客户端只能访问外部 |
| **Cell 关联** | 保留 `CellMailbox` 结构 | Phase 10 CellApp 需要 |
| **消息路由** | 客户端消息携带 EntityID，直接查找 | 比 BigWorld 的 `setClient` 更直接 |
| **客户端 RPC** | `EntityDefRegistry::validate_rpc()` 校验 | C++ 校验，C# 分发 |
| **giveClientTo** | 初期仅本地转移 | 远程转移在 Phase 9 (BaseAppMgr) 后实现 |
| **writeToDB** | C# 序列化 blob → NativeApi → C++ → DBApp | C++ 不解析属性值 |
| **Entity ID** | 初期本地递增，Phase 9 后由 BaseAppMgr 分配区间 | 渐进实现 |
| **Session Key** | 保留，32 字节随机令牌 | Phase 9 LoginApp 使用 |
| **ProxyPusher** | tick 中统一推送 | 复用 Updatable 系统 |
| **Bandwidth** | 初期无自适应，固定速率 | 后续优化 |

### 1.3 C# 脚本层的核心影响

**BigWorld (Python):**
```
BaseApp (C++) 持有 Python 对象 (PyObjectPlus)
  → C++ 直接操作属性 (obj.__dict__)
  → C++ 调用 Python 方法 (ob_type->tp_init, callMethod)
  → 序列化: C++ 遍历属性定义 → addToStream()
  → RPC: C++ 查找 MethodDescription → callMethod(obj, data)
```

**Atlas (C#):**
```
BaseApp (C++) 持有 C# 实体句柄 (GCHandle via ScriptObject)
  → C++ 不直接操作属性（属性在 C# 管理空间）
  → C++ 通过 ScriptEngine 调用 C# 方法
  → 序列化: C# Source Generator 生成 Serialize() → SpanWriter blob
  → RPC: C++ 校验 rpc_id → 转发 blob 到 C# → RpcDispatcher.Dispatch()
```

**核心简化:**
- BigWorld 的 Base/Proxy 有 ~7000 行 C++，Atlas 可能只需 ~1500 行
- 属性管理、序列化、RPC 分发的复杂逻辑全在 C# Source Generator 中
- C++ 只负责: 网络 I/O、消息路由、安全校验、生命周期管理

**双向实体创建路径:**

| 路径 | 发起者 | 流程 |
|------|--------|------|
| **运行时创建** | C# | C# `EntityFactory.Create()` → GCHandle → NativeApi `atlas_create_entity(type_id, handle)` → C++ `entity_mgr_.create_entity()` 绑定句柄 |
| **从 DB 恢复** | C++ | C++ 收到 CheckoutEntityAck(blob) → C++ 创建壳 → NativeApi `atlas_restore_entity(type_id, blob)` → C# `EntityFactory.Restore()` → 返回 GCHandle → C++ 绑定句柄 |

从 DB 恢复需要 **C++ → C# 方向**的调用 `atlas_restore_entity`。此函数在 Step 8.7 的
NativeApi 导出中需要增加（见下方补充）。

**[Replicated] 脏属性同步机制:**

C# Source Generator 在每个实体的 `OnTick()` 中检查 dirty flags。如果有变更，
主动通过 NativeApi 推送 delta blob:

```
每 tick:
  C# ScriptEngine.on_tick()
    → 遍历所有实体
    → entity.CheckDirtyReplicated()
    → 如果有脏属性:
        delta = DeltaWriter.Serialize(dirty_properties)
        NativeApi.atlas_send_replicated_delta(entity_id, delta, delta_len)
    → C++ Proxy::send_replicated_delta(delta) → 发送到客户端
```

这避免了 C++ 轮询 C# 的 dirty 状态，由 C# 侧主动推送，效率更高。

---

## 2. 消息协议设计

### 2.1 BaseApp 内部接口 (从其他服务器进程)

| 消息 | ID | 方向 | 用途 |
|------|-----|------|------|
| `CreateBase` | 2000 | BaseAppMgr → BaseApp | 创建 Base 实体 |
| `CreateBaseFromDB` | 2001 | BaseAppMgr → BaseApp | 从 DB 加载实体 |
| `AcceptClient` | 2002 | BaseApp → BaseApp | 远程客户端转移 |
| `CellEntityCreated` | 2010 | CellApp → BaseApp | Cell 实体创建成功 |
| `CellEntityDestroyed` | 2011 | CellApp → BaseApp | Cell 实体已销毁 |
| `CurrentCell` | 2012 | CellApp → BaseApp | Cell 地址更新 (Offload) |
| `CellRpcForward` | 2013 | CellApp → BaseApp | Cell → Base RPC 转发 |
| `SelfRpcFromCell` | 2014 | CellApp → BaseApp | Cell → 拥有者客户端 RPC，可靠路径 |
| `ReplicatedDeltaFromCell` | 2015 | CellApp → BaseApp | Cell 属性更新 → 转发客户端 |
| `BroadcastRpcFromCell` | 2016 | CellApp → BaseApp | Cell → `otherClients/allClients` RPC，广播路径 |
| `WriteEntityAck` | 4001 | DBApp → BaseApp | `write_to_db()` 结果 |

### 2.2 当前已实现的 BaseApp 外部接口 (从客户端)

| 消息 | ID | 方向 | 用途 |
|------|-----|------|------|
| `Authenticate` | 2020 | Client → BaseApp | SessionKey 认证 |

### 2.3 当前已实现的 BaseApp → Client 消息

| 消息 | ID | 方向 | 用途 |
|------|-----|------|------|
| `AuthenticateResult` | 2021 | BaseApp → Client | 认证结果 |

### 2.4 规划中、但尚未在当前代码落地的外部协议

这些消息仍然是 BaseApp / CellApp / Client SDK 的目标接口，但当前仓库尚未定义对应
`struct` 或 handler：

| 消息 | 规划方向 | 说明 |
|------|----------|------|
| `ClientBaseRpc` / `ClientCellRpc` | Client → BaseApp | Phase 10 / 12 |
| `EnableEntities` | Client → BaseApp | Phase 10 / 12 |
| `HeartbeatPing` | Client → BaseApp | 后续外部连接保活 |
| `ClientRpcCall` | BaseApp → Client | 目标 SDK 协议 |
| `EntityEnter` / `EntityLeave` | BaseApp → Client | Phase 10 AOI |
| `EntityPropertyUpdate` | BaseApp → Client | Phase 10 复制 |
| `CreateBasePlayer` / `ResetEntities` | BaseApp → Client | Phase 12 SDK |

---

## 3. 核心模块设计

> 说明: 本节保留了原始 Phase 8 设计草案，但需要按当前代码理解：
> - `Proxy` 已合并进 `src/server/baseapp/base_entity.hpp`
> - `send_replicated_delta()` / `enable_entities()` / 外部 `ClientBaseRpc`/`ClientCellRpc` 仍未在当前代码中落地
> - 相关客户端同步职责已转移到 Phase 10 / Phase 12 继续完成

### 3.1 BaseEntity — Base 实体

```cpp
// src/server/baseapp/base_entity.hpp
namespace atlas {

class BaseEntity {
public:
    BaseEntity(EntityID id, uint16_t type_id, DatabaseID dbid = 0);
    virtual ~BaseEntity();

    // Non-copyable
    BaseEntity(const BaseEntity&) = delete;
    BaseEntity& operator=(const BaseEntity&) = delete;

    // ========== 标识 ==========
    [[nodiscard]] auto id() const -> EntityID { return id_; }
    [[nodiscard]] auto type_id() const -> uint16_t { return type_id_; }
    [[nodiscard]] auto dbid() const -> DatabaseID { return dbid_; }
    void set_dbid(DatabaseID dbid) { dbid_ = dbid; }

    [[nodiscard]] auto type_name() const -> std::string_view;

    [[nodiscard]] virtual auto is_proxy() const -> bool { return false; }

    // ========== C# 脚本实例 ==========
    /// C# 实体的不透明句柄 (GCHandle)
    void set_script_handle(uint64_t handle) { script_handle_ = handle; }
    [[nodiscard]] auto script_handle() const -> uint64_t { return script_handle_; }

    // ========== Cell 关联 ==========
    [[nodiscard]] auto has_cell() const -> bool { return cell_addr_.has_value(); }
    [[nodiscard]] auto cell_addr() const -> const Address& { return *cell_addr_; }
    [[nodiscard]] auto cell_entity_id() const -> EntityID { return cell_entity_id_; }

    void set_cell(const Address& addr, EntityID cell_id);
    void clear_cell();

    // ========== 数据库 ==========
    [[nodiscard]] auto has_written_to_db() const -> bool { return dbid_ != 0; }

    /// 请求持久化 (C# 调用 NativeApi → 到这里)
    void write_to_db(uint8_t flags,
                     const std::string& identifier,
                     std::span<const std::byte> blob,
                     std::function<void(bool, DatabaseID)> callback = {});

    // ========== 销毁 ==========
    /// 标记为销毁中（防止重入）
    [[nodiscard]] auto is_destroying() const -> bool { return destroying_; }

    /// 请求销毁实体 (异步: 如果 write_to_db=true，等 DB 回调后才真正释放)
    ///
    /// 流程:
    ///   1. destroying_ = true (防止重入，拒绝新 RPC/writeToDB)
    ///   2. 如果 has_cell(): 发送 destroyCellEntity 到 CellApp, 等回调
    ///   3. 如果 write_to_db: 调用 write_to_db(flags=LogOff|可选Delete)
    ///      → 等待 WriteEntityAck 回调
    ///   4. 回调中: 通知 C# OnDestroy() → GCHandle.Free()
    ///   5. 从 EntityManager 中移除
    ///
    /// 如果 write_to_db=false 且无 cell: 同步立即释放
    void destroy(bool delete_from_db = false, bool write_to_db = true);

    // ========== Tick ==========
    /// 发送脏属性到客户端（Proxy 覆盖）
    virtual void send_dirty_to_client() {}

protected:
    EntityID id_;
    uint16_t type_id_;
    DatabaseID dbid_ = 0;
    uint64_t script_handle_ = 0;  // C# GCHandle

    // Cell
    std::optional<Address> cell_addr_;
    EntityID cell_entity_id_ = 0;
    bool cell_create_pending_ = false;

    bool destroying_ = false;
};

} // namespace atlas
```

**与 BigWorld Base 的对比:**

| BigWorld Base | Atlas BaseEntity | 差异 |
|---|---|---|
| `PyObjectPlus` 基类 (Python 对象) | `uint64_t script_handle_` (GCHandle) | C# 通过句柄引用 |
| `pType_` (EntityTypePtr) | `type_id_` + `EntityDefRegistry` 查找 | 更轻量 |
| `pChannel_` (UDP Channel to Cell) | `cell_addr_` + BaseApp 的 Channel 共享 | 共享连接 |
| `pCellEntityMailBox_` | `cell_addr_` + `cell_entity_id_` | 扁平化 |
| `pCellData_` (Python dict) | C# 管理 | C++ 不持有 Cell 数据 |
| ~4000 LOC | ~300 LOC 预估 | 大幅简化 |

### 3.2 Proxy — 客户端代理实体

```cpp
// src/server/baseapp/base_entity.hpp   (`Proxy` 在当前代码中合并于此)
namespace atlas {

class Proxy : public BaseEntity {
public:
    Proxy(EntityID id, uint16_t type_id, DatabaseID dbid = 0);
    ~Proxy() override;

    auto is_proxy() const -> bool override { return true; }

    // ========== 客户端连接 ==========
    [[nodiscard]] auto has_client() const -> bool { return client_channel_ != nullptr; }
    [[nodiscard]] auto client_channel() -> Channel* { return client_channel_; }

    /// 绑定客户端连接 (登录成功后调用)
    auto attach_client(Channel* channel, const SessionKey& key) -> Result<void>;

    /// 解除客户端连接
    void detach_client(bool condemn_channel = true);

    /// 客户端连接断开回调
    void on_client_disconnect(uint8_t reason);

    // ========== Session ==========
    [[nodiscard]] auto session_key() const -> const SessionKey& { return session_key_; }
    void set_session_key(const SessionKey& key) { session_key_ = key; }

    // ========== 客户端消息 ==========

    /// 处理客户端发来的 exposed RPC (cell/base)
    void on_client_rpc(uint32_t rpc_id, BinaryReader& reader);

    /// 发送 ClientRpc 到客户端 (C# 调用 NativeApi → 到这里)
    void send_client_rpc(uint32_t rpc_id, uint8_t target,
                         std::span<const std::byte> payload);

    /// 发送属性增量更新到客户端
    void send_replicated_delta(std::span<const std::byte> delta);

    /// 发送 CreateBasePlayer (客户端创建本地玩家实体)
    void send_create_base_player();

    // ========== GiveClientTo ==========

    /// 本地转移客户端到另一个 Proxy
    auto give_client_to(Proxy& dest) -> Result<void>;

    // ========== Tick ==========
    void send_dirty_to_client() override;

    // ========== 实体同步 ==========
    [[nodiscard]] auto entities_enabled() const -> bool { return entities_enabled_; }
    void enable_entities();

private:
    Channel* client_channel_ = nullptr;    // 非拥有，NetworkInterface 管理
    SessionKey session_key_;

    bool entities_enabled_ = false;
    bool base_player_created_ = false;
    bool giving_client_away_ = false;
};

} // namespace atlas
```

**与 BigWorld Proxy 的对比:**

| BigWorld Proxy | Atlas Proxy | 差异 |
|---|---|---|
| `pClientChannel_` (ChannelPtr) | `client_channel_` (Channel*) | 非拥有指针 |
| `pClientEntityMailBox_` | 不需要 | C# Mailbox 代理处理 |
| `pProxyPusher_` (定时器推送) | tick 中统一推送 | 使用 Updatable |
| `DataDownloads` (流式下载) | 不实现（初期） | 后续优化 |
| `RateLimitMessageFilter` | 速率限制 | 复用已有 NetworkInterface rate limit |
| `Wards` (AOI 实体列表) | Phase 10 CellApp 实现 | 本阶段不需要 |
| `encryptionKey_` + BlockCipher | 初期不加密 | Phase 后续加入 TLS |
| ~3400 LOC | ~500 LOC 预估 | 大幅简化 |

### 3.3 EntityManager — 实体集合管理

```cpp
// src/server/baseapp/entity_manager.hpp
namespace atlas {

class EntityManager {
public:
    explicit EntityManager(ScriptEngine& script);

    // ========== 创建 ==========

    /// 创建 Base 实体 (C# 实例通过 ScriptEngine 创建)
    auto create_entity(uint16_t type_id) -> Result<BaseEntity*>;

    /// 创建 Proxy 实体
    auto create_proxy(uint16_t type_id) -> Result<Proxy*>;

    /// 从数据库恢复实体
    /// 异步: DBApp 返回数据后在回调中创建
    void create_entity_from_db(uint16_t type_id, DatabaseID dbid,
                                std::function<void(Result<BaseEntity*>)> callback);

    void create_entity_from_db_by_name(uint16_t type_id,
                                        const std::string& identifier,
                                        std::function<void(Result<BaseEntity*>)> callback);

    // ========== 查找 ==========

    auto find(EntityID id) -> BaseEntity*;
    auto find_proxy(EntityID id) -> Proxy*;

    auto entity_count() const -> size_t { return entities_.size(); }
    auto proxy_count() const -> size_t;

    // ========== 销毁 ==========

    void destroy(EntityID id, bool delete_from_db = false);
    void destroy_all();

    // ========== Tick ==========

    /// 遍历所有 Proxy，推送脏属性
    void tick();

    // ========== ID 分配 ==========

    /// 设置 ID 分配范围 (由 BaseAppMgr 分配)
    void set_id_range(EntityID start, EntityID end);

private:
    auto allocate_id() -> EntityID;

    /// 创建 C# 实体实例，返回 GCHandle
    auto create_script_entity(uint16_t type_id) -> Result<uint64_t>;

    /// 从 blob 恢复 C# 实体实例
    auto restore_script_entity(uint16_t type_id,
                                std::span<const std::byte> blob) -> Result<uint64_t>;

    ScriptEngine& script_;
    std::unordered_map<EntityID, std::unique_ptr<BaseEntity>> entities_;

    // ID 分配 (由 BaseAppMgr 分配区间)
    EntityID next_id_ = 1;
    EntityID max_id_ = 0xFFFFFFFF;  // Phase 9 后由 BaseAppMgr 设定
    bool range_request_pending_ = false;
};
```

**EntityID 区间管理 (与 Phase 9 BaseAppMgr 协同):**

```cpp
auto EntityManager::allocate_id() -> EntityID {
    if (next_id_ > max_id_) {
        ATLAS_LOG_ERROR("EntityID range exhausted [{}, {}]", next_id_, max_id_);
        return kInvalidEntityID;  // 调用方应处理此错误
    }
    return next_id_++;
}

/// 当区间剩余 < 20% 时，预请求新区间
void EntityManager::check_id_range(BaseApp& app) {
    auto remaining = max_id_ - next_id_;
    auto total = max_id_ - range_start_;
    if (remaining < total / 5 && !range_request_pending_) {
        range_request_pending_ = true;
        app.request_entity_id_range();  // → BaseAppMgr RequestEntityIdRange
    }
}

/// 收到新区间后
void EntityManager::extend_id_range(EntityID new_end) {
    max_id_ = new_end;
    range_request_pending_ = false;
}
```

} // namespace atlas
```

### 3.4 BaseAppNativeProvider — BaseApp 专用 NativeApi

```cpp
// src/server/baseapp/baseapp_native_provider.hpp
namespace atlas {

class BaseApp;  // forward

class BaseAppNativeProvider : public BaseNativeProvider {
public:
    explicit BaseAppNativeProvider(BaseApp& app);

    // ---- Time ----
    double server_time() override;
    float delta_time() override;

    // ---- Process ----
    uint8_t get_process_prefix() override { return 'B'; }

    // ---- RPC ----
    /// C# entity.Client.ShowDamage() → 这里 → 找到 Proxy → 发送到客户端
    void send_client_rpc(uint32_t entity_id, uint32_t rpc_id,
                         uint8_t target,
                         const std::byte* payload, int32_t len) override;

    /// C# entity.Cell.MoveTo() → 这里 → 找到 BaseEntity → 转发到 CellApp
    void send_cell_rpc(uint32_t entity_id, uint32_t rpc_id,
                       const std::byte* payload, int32_t len) override;

    /// C# 调用另一个 Base 实体的方法 (同进程或跨进程)
    void send_base_rpc(uint32_t entity_id, uint32_t rpc_id,
                       const std::byte* payload, int32_t len) override;

    // ---- Entity types ----
    void register_entity_type(const std::byte* data, int32_t len) override;
    void unregister_all_entity_types() override;

    // ---- DB (新增) ----
    void write_to_db(uint32_t entity_id, int64_t dbid,
                     uint8_t flags,
                     const char* identifier, int32_t id_len,
                     const std::byte* blob, int32_t blob_len) override;

    void delete_from_db(uint16_t type_id, int64_t dbid) override;

private:
    BaseApp& app_;
};

} // namespace atlas
```

**RPC 路由实现:**

```cpp
void BaseAppNativeProvider::send_client_rpc(
    uint32_t entity_id, uint32_t rpc_id,
    uint8_t target, const std::byte* payload, int32_t len)
{
    auto* proxy = app_.entity_mgr().find_proxy(entity_id);
    if (!proxy || !proxy->has_client()) {
        ATLAS_LOG_WARNING("send_client_rpc: entity {} has no client", entity_id);
        return;
    }
    proxy->send_client_rpc(rpc_id, target, {payload, static_cast<size_t>(len)});
}

void BaseAppNativeProvider::send_cell_rpc(
    uint32_t entity_id, uint32_t rpc_id,
    const std::byte* payload, int32_t len)
{
    auto* entity = app_.entity_mgr().find(entity_id);
    if (!entity || !entity->has_cell()) {
        ATLAS_LOG_WARNING("send_cell_rpc: entity {} has no cell", entity_id);
        return;
    }

    // 转发到 CellApp
    auto* channel = app_.network().find_channel(entity->cell_addr());
    if (!channel) return;

    baseapp::CellRpcForward msg{entity->cell_entity_id(), rpc_id,
        std::vector<std::byte>(payload, payload + len)};
    channel->send_message(msg);
}
```

### 3.5 BaseApp — 主进程类

```cpp
// src/server/baseapp/baseapp.hpp
namespace atlas {

class BaseApp : public EntityApp {
public:
    /// int_network 传给 EntityApp/ServerApp 链作为内部网络 (network_)
    /// ext_network 是 BaseApp 独有的客户端网络
    BaseApp(EventDispatcher& dispatcher, NetworkInterface& int_network,
            NetworkInterface& ext_network);

    // ========== 访问器 ==========
    [[nodiscard]] auto ext_network() -> NetworkInterface& { return ext_network_; }
    [[nodiscard]] auto entity_mgr() -> EntityManager& { return entity_mgr_; }
    [[nodiscard]] auto entity_defs() -> const EntityDefRegistry& { return *entity_defs_; }

protected:
    // ---- ServerApp 覆盖 ----
    auto init(int argc, char* argv[]) -> bool override;
    void fini() override;
    void on_tick_complete() override;
    void register_watchers() override;

    // ---- ScriptApp 覆盖 ----
    auto create_native_provider() -> std::unique_ptr<INativeApiProvider> override;
    void on_script_ready() override;

private:
    // ---- 内部消息处理器 ----
    void on_cell_entity_created(const Address& src, Channel* ch,
                                 const baseapp::CellEntityCreated& msg);
    void on_cell_rpc_forward(const Address& src, Channel* ch,
                              const baseapp::CellRpcForward& msg);
    void on_self_rpc_from_cell(const Address& src, Channel* ch,
                                const baseapp::SelfRpcFromCell& msg);
    void on_replicated_delta_from_cell(const Address& src, Channel* ch,
                                       const baseapp::ReplicatedDeltaFromCell& msg);
    void on_broadcast_rpc_from_cell(const Address& src, Channel* ch,
                                     const baseapp::BroadcastRpcFromCell& msg);
    void on_write_to_db_response(const Address& src, Channel* ch,
                                  const dbapp::WriteEntityAck& msg);

    // ---- 外部消息处理器 (客户端) ----
    void on_client_authenticate(const Address& src, Channel* ch,
                                 const baseapp::Authenticate& msg);
    // `ClientBaseRpc` / `ClientCellRpc` / `EnableEntities` 尚未在当前代码中实现

    // ---- 客户端连接管理 ----
    void on_ext_channel_disconnect(ChannelId channel_id);

    // ---- Machined 事件 ----
    void on_cellapp_birth(const machined::BirthNotification& notif);
    void on_cellapp_death(const machined::DeathNotification& notif);

    // ---- 组件 ----
    NetworkInterface& ext_network_;       // 外部（客户端）网络
    EntityManager entity_mgr_;
    EntityDefRegistry* entity_defs_ = nullptr;

    // Proxy 地址索引: ChannelId → EntityID (避免 Channel* 悬垂指针)
    std::unordered_map<ChannelId, EntityID> client_to_entity_;

    // 待处理的 writeToDB 回调
    struct PendingWrite {
        EntityID entity_id;
        std::function<void(bool, DatabaseID)> callback;
    };
    std::unordered_map<uint32_t, PendingWrite> pending_writes_;
    uint32_t next_request_id_ = 1;
};

} // namespace atlas
```

**双网络接口架构:**

```
                    ┌──────────────────────────────────┐
                    │           BaseApp 进程            │
                    │                                    │
  Clients ───RUDP──→│ ext_network_ (port 20100)         │
                    │   ├── Authenticate                │
                    │   ├── ClientBaseRpc / ClientCellRpc│
                    │   └── EnableEntities              │
                    │                                    │
  CellApp ──RUDP──→│ network_ (internal, port auto)     │
  DBApp            │   ├── CellEntityCreated            │
  machined         │   ├── CellRpcForward               │
  Managers         │   └── WriteToDBResponse            │
                    └──────────────────────────────────┘
```

> **为什么两个 NetworkInterface？**
> 与 BigWorld 设计一致。外部接口面向不可信的客户端，可做速率限制、消息过滤。
> 内部接口面向可信的服务器进程，不需要额外安全检查。

**`init()` 流程:**

```
BaseApp::init():
    EntityApp::init()                       // → ScriptApp → ServerApp 链式初始化
                                             // ScriptApp 初始化 ClrHost + 加载 C# 程序集
                                             // C# EntityTypeRegistry.RegisterAll() 执行

    ext_network_.start_rudp_server(ext_addr) // 监听外部 RUDP 端口
    ext_network_ disconnect callback → on_ext_channel_disconnect
    configure_ext_security()                 // 外部接口安全策略 (见下)

    注册内部消息处理器 (CellEntityCreated, CellRpcForward, etc.)
    注册外部消息处理器 (Authenticate, ClientBaseRpc, ClientCellRpc, HeartbeatPing, etc.)

    // 向 BaseAppMgr 注册 (同步，init 阶段安全)
    auto mgr = machined_client().find_one(ProcessType::BaseAppMgr)
    send RegisterBaseApp(internal_addr, external_addr) to mgr
    wait for RegisterBaseAppAck → 获取 app_id + EntityID 区间
    entity_mgr_.set_id_range(range_start, range_end)
    app_id_ = ack.app_id

    // 监听 CellApp 上下线
    machined_client().listen_for_birth(CellApp, on_cellapp_birth)
    machined_client().listen_for_death(CellApp, on_cellapp_death)

    // 通知 BaseAppMgr 就绪
    send BaseAppReady to mgr

    register_watchers()
    LOG_INFO("BaseApp started (app_id={})", app_id_)
```

**外部接口安全策略:**

```cpp
void BaseApp::configure_ext_security() {
    // 1. 认证超时: 连接建立后 10 秒内必须发送 Authenticate，否则断开
    ext_network_.set_auth_timeout(Seconds(10));

    // 2. 认证前消息白名单: 当前代码只允许 Authenticate；
    //    HeartbeatPing 仍是后续外部协议
    ext_network_.set_unauthenticated_whitelist({
        baseapp::Authenticate::descriptor().id(),
    });

    // 3. 最大同时连接数 (客户端外部消息: ClientBaseRpc / ClientCellRpc)
    ext_network_.set_max_connections(config().max_client_connections);  // 默认 1000

    // 4. 空闲超时: 60 秒无消息断开
    ext_network_.set_idle_timeout(Seconds(60));

    // 5. 每连接速率限制
    ext_network_.set_rate_limit(config().client_rate_limit);
}
```

> BigWorld 的 `InitialConnectionFilter` 拒绝非预期来源的包。Atlas 当前外部接口同样运行在
> 数据报语义之上（RUDP），因此仍必须显式维护认证前白名单、超时和连接状态，而不是依赖 TCP
> 连接态替代这些校验。

**`on_tick_complete()` 流程:**

```
BaseApp::on_tick_complete():
    EntityApp::on_tick_complete()     // ScriptEngine::on_tick() → C# Tick
    entity_mgr_.tick()                // 遍历 Proxy，推送脏属性到客户端
    check_client_idle_timeouts()      // 检查客户端空闲超时
    check_entity_id_range()           // EntityID 区间剩余检查，不足时预请求
```

**客户端心跳与空闲超时:**

```cpp
void BaseApp::on_client_heartbeat(const Address& src, ChannelId ch_id,
                                   const HeartbeatPing& msg) {
    // 目标设计：当前代码尚未实现外部客户端心跳
}

void BaseApp::check_client_idle_timeouts() {
    auto now = Clock::now();
    for (auto it = client_to_entity_.begin(); it != client_to_entity_.end(); ) {
        auto idle = ext_network_.channel_idle_time(it->first);
        if (idle > kClientIdleTimeout) {  // 默认 60 秒
            auto* proxy = entity_mgr_.find_proxy(it->second);
            if (proxy) proxy->on_client_disconnect(REASON_TIMEOUT);
            it = client_to_entity_.erase(it);
        } else {
            ++it;
        }
    }
}
```

---

## 4. 端到端流程

### 4.1 创建实体

```
C# (Atlas.Runtime)              BaseApp (C++)
    │                               │
    │ EntityFactory.Create("Avatar")│
    │ → new AvatarEntity()          │
    │ → GCHandle.Alloc()            │
    │ → NativeApi.CreateEntity(     │
    │     type_id, handle)          │
    │───────────────────────────────→│
    │                               │ entity_mgr_.create_entity(type_id)
    │                               │ → allocate_id()
    │                               │ → new BaseEntity(id, type_id)
    │                               │ → entity.set_script_handle(handle)
    │                               │ → entities_[id] = entity
    │                               │
    │←── return EntityID ───────────│
    │                               │
    │ entity.OnInit() callback      │
```

### 4.2 客户端 Exposed RPC 调用

```
Client                 BaseApp ext_network_          C++ BaseApp           C# Entity
  │                         │                           │                     │
  │── ClientBaseRpc ───────→│                           │                     │
  │   (entity_id, rpc_id,   │                           │                     │
  │    args_blob)            │                           │                     │
  │                         │── on_client_rpc ─────────→│                     │
  │                         │                           │                     │
  │                         │   1. client_to_entity_    │                     │
  │                         │      查找 EntityID        │                     │
  │                         │   2. entity_defs_         │                     │
  │                         │      .is_exposed(         │                     │
  │                         │        type_id, rpc_id)   │                     │
  │                         │   3. 检查 rpc 方法        │                     │
  │                         │      有 exposed 标记      │                     │
  │                         │                           │                     │
  │                         │   if valid:               │                     │
  │                         │   ── NativeApi ───────────────────────────────→│
  │                         │      dispatch_rpc(        │                     │
  │                         │        entity_handle,     │  RpcDispatcher     │
  │                         │        rpc_id, blob)      │  .Dispatch()       │
  │                         │                           │  → entity          │
  │                         │                           │    .OnRequestMove()│
```

### 4.3 WriteToDB

```
C# Entity                  BaseApp C++                DBApp
    │                          │                         │
    │ entity.WriteToDB()       │                         │
    │ → Serialize([Persistent])│                         │
    │ → SpanWriter blob        │                         │
    │ → NativeApi.write_to_db( │                         │
    │     entity_id, dbid=0,   │                         │
    │     flags, identifier,   │                         │
    │     blob)                │                         │
    │──────────────────────────→                         │
    │                          │ find entity             │
    │                          │ → entity.write_to_db()  │
    │                          │                         │
    │                          │── WriteEntity ─────────→│
    │                          │   (request_id, flags,   │
    │                          │    type_id, dbid=0,     │
    │                          │    identifier, blob)    │
    │                          │                         │ → IDatabase.put_entity()
    │                          │                         │   → DBID=42 分配
    │                          │←── WriteEntityAck ─────│
    │                          │   (request_id, ok,      │
    │                          │    dbid=42)             │
    │                          │                         │
    │                          │ entity.set_dbid(42)     │
    │←── callback(ok, 42) ────│                         │
```

### 4.4 GiveClientTo (本地)

```
C# Account entity        BaseApp C++              Client
    │                        │                        │
    │ GiveClientTo(avatar)   │                        │
    │ → NativeApi ──────────→│                        │
    │                        │                        │
    │                        │ 1. account_proxy       │
    │                        │    .detach_client(     │
    │                        │      condemn=false)    │
    │                        │                        │
    │                        │ 2. ResetEntities ─────→│
    │                        │                        │
    │                        │ 3. avatar_proxy        │
    │                        │    .attach_client(     │
    │                        │      channel, key)     │
    │                        │                        │
    │                        │ 4. CreateBasePlayer ──→│
    │                        │    (avatar type_id,    │
    │                        │     avatar properties) │
    │                        │                        │
    │ Account.OnLoseClient() │                        │
    │ Avatar.OnGetClient()   │                        │
```

### 4.5 CreateEntityFromDB

```
C# (请求加载)          BaseApp C++                DBApp
    │                      │                         │
    │ CreateFromDB(        │                         │
    │   "Avatar",          │                         │
    │   identifier=        │                         │
    │   "hero123")         │                         │
    │──────────────────────→                         │
    │                      │── CheckoutEntity ──────→│
    │                      │   (mode=ByName,         │
    │                      │    identifier="hero123")│
    │                      │                         │ checkout_mgr
    │                      │                         │ SELECT + UPDATE
    │                      │←── CheckoutEntityAck ──│
    │                      │   (status=ok,           │
    │                      │    dbid=42, blob=...)   │
    │                      │                         │
    │                      │ entity_mgr_             │
    │                      │   .restore_script_entity│
    │                      │   (type_id, blob)       │
    │                      │ → C# Deserialize(blob)  │
    │                      │ → 创建 BaseEntity       │
    │                      │                         │
    │←── callback(entity) ─│                         │
```

---

## 5. 实现步骤

### Step 8.1: BaseApp 消息定义

**新增文件:**
```
src/server/baseapp/baseapp_messages.hpp
tests/unit/test_baseapp_messages.cpp
```

当前代码把内部接口和已实现的外部认证消息都放在 `baseapp_messages.hpp`；
不存在单独的 `baseapp_ext_messages.hpp`。

### Step 8.2: BaseEntity

**新增文件:**
```
src/server/baseapp/base_entity.hpp / .cpp
tests/unit/test_base_entity.cpp
```

**测试用例:**
- 创建/销毁生命周期
- Cell 关联设置/清除
- DBID 管理
- `write_to_db()` 序列化正确性

### Step 8.3: Proxy

**新增文件:**
```
src/server/baseapp/base_entity.hpp / .cpp   # 当前代码将 `Proxy` 合并在此文件中
tests/unit/test_base_entity.cpp
```

**测试用例:**
- `attach_client()` / `detach_client()` 生命周期
- `give_client_to()` 本地转移
- `send_client_rpc()` 消息格式
- `send_replicated_delta()` 消息格式
- `on_client_rpc()` 安全校验（合法/非法 rpc_id）
- 客户端断开触发 `on_client_disconnect()`
- `enable_entities()` 状态管理

### Step 8.4: EntityManager

**新增文件:**
```
src/server/baseapp/entity_manager.hpp / .cpp
tests/unit/test_entity_manager.cpp
```

**测试用例:**
- 创建/查找/销毁 Base 实体
- 创建/查找 Proxy 实体
- Entity ID 分配不重复
- ID 范围限制
- `tick()` 遍历 Proxy
- `destroy_all()` 清理

### Step 8.5: BaseAppNativeProvider

**新增文件:**
```
src/server/baseapp/baseapp_native_provider.hpp / .cpp
tests/unit/test_baseapp_native_provider.cpp
```

**实现:** 所有 `INativeApiProvider` 方法的 BaseApp 特化实现。

**需同步更新:** `INativeApiProvider` / `BaseNativeProvider` 补齐当前代码实际使用的方法：
`send_client_rpc()` / `send_cell_rpc()` / `send_base_rpc()` / `write_to_db()` /
`give_client_to()` / `set_native_callbacks()`。

### Step 8.6: BaseApp 进程

**新增文件:**
```
src/server/baseapp/
├── CMakeLists.txt
├── main.cpp
├── baseapp.hpp / .cpp
```

**实现顺序:**
1. 基本启动（EntityApp + machined 注册 + 双网络接口）
2. C# 脚本引擎初始化（load Atlas.Runtime.dll）
3. EntityDefRegistry 从 C# 注册
4. EntityManager 集成
5. 外部接口: Authenticate → 绑定 Proxy
6. NativeApi: send_client_rpc → Proxy → 客户端 (下行 [ClientRpc])
7. NativeApi: write_to_db → 发送到 DBApp
8. DBApp 回调处理 (`WriteEntityAck`)
9. NativeApi: create_entity_from_db → CheckoutEntity 流程
10. NativeApi: give_client_to → 本地 Proxy 转移
11. Watcher 注册

注：
- `ClientBaseRpc` / `ClientCellRpc` / `EnableEntities` / AOI 脏属性推送在当前代码里仍未落地
- 这些外部协议由 Phase 10 / Phase 12 接着完成

### Step 8.7: INativeApiProvider 扩展

**更新文件:**
```
src/lib/clrscript/native_api_provider.hpp  (新增方法)
src/lib/clrscript/base_native_provider.hpp (stub 实现)
src/lib/clrscript/clr_native_api.hpp      (新增导出函数)
src/lib/clrscript/clr_native_api.cpp      (新增导出函数)
```

**C# → C++ 导出函数 (C# 通过 NativeApi 调用):**
```cpp
ATLAS_NATIVE_API int32_t atlas_create_entity(uint16_t type_id, uint64_t handle);
ATLAS_NATIVE_API void atlas_destroy_entity(uint32_t entity_id, uint8_t flags);
ATLAS_NATIVE_API void atlas_write_to_db(uint32_t entity_id, int64_t dbid,
    uint8_t flags, const char* identifier, int32_t id_len,
    const uint8_t* blob, int32_t blob_len);
ATLAS_NATIVE_API void atlas_give_client_to(uint32_t src_entity_id,
    uint32_t dest_entity_id);
ATLAS_NATIVE_API void atlas_create_cell_entity(uint32_t entity_id,
    uint32_t space_id);
ATLAS_NATIVE_API void atlas_send_replicated_delta(uint32_t entity_id,
    const uint8_t* delta, int32_t delta_len);
```

**C++ → C# 导出函数 (C++ 通过 ScriptEngine/函数指针调用):**

从 DB 恢复、生命周期回调等场景需要 C++ 主动调用 C#。通过 C# 在启动时注册的
`[UnmanagedCallersOnly]` 函数指针实现:

```cpp
// C++ 调用 C# (函数指针在 C# 初始化时注册)
using RestoreEntityFn = uint64_t(*)(uint16_t type_id,
    const uint8_t* blob, int32_t blob_len);          // 返回 GCHandle
using NotifyEntityDestroyFn = void(*)(uint64_t handle);
using NotifyClientDisconnectFn = void(*)(uint64_t handle, uint8_t reason);
using NotifyLoggedInFn = void(*)(uint64_t handle);
using DispatchRpcFn = void(*)(uint64_t handle,
    uint32_t rpc_id, const uint8_t* payload, int32_t len);

// INativeApiProvider 中新增:
struct ScriptCallbacks {
    RestoreEntityFn restore_entity = nullptr;
    NotifyEntityDestroyFn notify_destroy = nullptr;
    NotifyClientDisconnectFn notify_client_disconnect = nullptr;
    NotifyLoggedInFn notify_logged_in = nullptr;
    DispatchRpcFn dispatch_rpc = nullptr;
};
```

> C# `Atlas.Runtime` 在 `OnInit()` 中通过 `NativeApi.RegisterCallbacks(...)` 注册这些函数指针。
> C++ 缓存指针后直接调用，零开销。

### Step 8.8: 集成测试

**新增文件:**
```
tests/integration/test_baseapp_integration.cpp
```

端到端场景 (需要 machined + DBApp):
1. 启动 machined + DBApp (XML) + BaseApp
2. BaseApp 初始化 C# 脚本引擎
3. 创建 Base 实体 → C# OnInit 回调
4. WriteToDB → DBApp 存储 → 返回 DBID
5. 模拟客户端 RUDP 连接 → Authenticate
6. 创建 Proxy → 绑定客户端
7. 客户端发送 exposed RPC → C# 处理
8. C# 发送 ClientRpc → 客户端接收
9. GiveClientTo → 客户端切换实体
10. 客户端断开 → OnClientDeath 回调

---

## 6. 文件清单汇总

```
src/server/baseapp/
├── CMakeLists.txt
├── main.cpp
├── baseapp.hpp / .cpp              (Step 8.6)
├── baseapp_messages.hpp            (Step 8.1)
├── base_entity.hpp / .cpp          (Step 8.2 / 8.3, `Proxy` 内嵌于此)
├── entity_manager.hpp / .cpp       (Step 8.4)
└── baseapp_native_provider.hpp/.cpp (Step 8.5)

src/lib/clrscript/                  (Step 8.7, 更新)
├── native_api_provider.hpp
├── base_native_provider.hpp
├── clr_native_api.hpp / .cpp

tests/unit/
├── test_baseapp_messages.cpp
├── test_base_entity.cpp
├── test_proxy.cpp
├── test_entity_manager.cpp
└── test_baseapp_native_provider.cpp

tests/integration/
└── test_baseapp_integration.cpp
```

---

## 7. 依赖关系与执行顺序

```
Step 8.1: 消息定义                  ← 无依赖
Step 8.2: BaseEntity                ← 无依赖
Step 8.3: Proxy                     ← 依赖 8.2
Step 8.4: EntityManager             ← 依赖 8.2, 8.3
Step 8.7: INativeApiProvider 扩展   ← 无依赖, 可并行

    全部就绪后:
Step 8.5: BaseAppNativeProvider     ← 依赖 8.4 + 8.7
Step 8.6: BaseApp 进程              ← 依赖 8.1-8.5
Step 8.8: 集成测试                  ← 依赖 8.6
```

**推荐执行顺序:**

```
第 1 轮 (并行): 8.1 消息定义 + 8.2 BaseEntity + 8.7 NativeApi 扩展
第 2 轮:        8.3 Proxy
第 3 轮 (并行): 8.4 EntityManager + 8.5 BaseAppNativeProvider
第 4 轮:        8.6 BaseApp 进程
第 5 轮:        8.8 集成测试
```

---

## 8. BigWorld 完整对照

| BigWorld | Atlas | 差异说明 |
|----------|-------|---------|
| `BaseApp : EntityApp` Singleton | `BaseApp : EntityApp` | 相同层次 |
| `intInterface_` + `extInterface_` 双网络 | `network_` + `ext_network_` | 相同设计 |
| `Bases` (EntityID→Base*) | `EntityManager` | Atlas 封装了创建/销毁逻辑 |
| `Base : PyObjectPlus` (~4000 LOC) | `BaseEntity` (~300 LOC) | 逻辑在 C# 侧 |
| `Proxy : Base` (~3400 LOC) | `Proxy : BaseEntity` (~500 LOC) | 大幅简化 |
| `EntityCreator` (Mgr 分配 ID 区间) | `EntityManager::allocate_id()` | 初期本地递增 |
| `setClient(entityID)` 消息路由 | `client_to_entity_` (ChannelId) 映射 | 直接查找，避免悬垂指针 |
| `exposedMethodFromMsgID()` 校验 | `EntityDefRegistry::is_exposed()` | 更集中 |
| `callMethod(obj, data)` Python 调用 | NativeApi → C# `RpcDispatcher` | 编译期生成 |
| `entity.client.method()` Python 动态 | `entity.Client.Method()` struct | Source Generator |
| `ProxyPusher` 定时器推送 | tick 中统一推送 | 使用 Updatable |
| `DataDownloads` 流式下载 | 初期不实现 | 后续优化 |
| `RateLimitMessageFilter` | 复用 NetworkInterface rate limit | 更简单 |
| `giveClientTo()` 本地+远程 | 初期仅本地 | 远程在 Phase 9 |
| Python `onClientDeath()` 回调 | C# `OnClientDisconnect()` | 命名更清晰 |

---

## 9. 关键设计决策记录

### 9.1 双网络接口

**决策: 保留 BigWorld 的双接口设计。**

`main.cpp`:
```cpp
int main(int argc, char* argv[]) {
    EventDispatcher dispatcher("baseapp");
    NetworkInterface int_network(dispatcher);  // 内部
    NetworkInterface ext_network(dispatcher);  // 外部 (客户端)
    BaseApp app(dispatcher, int_network, ext_network);
    return app.run_app(argc, argv);
}
```

外部接口单独监听一个端口，只注册客户端允许的消息处理器。
内部接口处理所有服务器间通信。

### 9.2 C# 实体生命周期

C# 实体实例通过 `GCHandle` 防止被 GC 回收。C++ 持有 `uint64_t script_handle_`。

```
创建:
  C# EntityFactory.Create("Avatar") → new AvatarEntity()
  C# GCHandle.Alloc(entity) → IntPtr handle
  C# NativeApi.atlas_create_entity(type_id, handle.ToInt64())
  C++ entity_mgr_.create_entity() → entity.set_script_handle(handle)

销毁:
  C++ entity_mgr_.destroy(id)
  C++ → NativeApi 通知 C# → GCHandle.Free(handle)
  C# 实体可被 GC 回收
```

### 9.3 客户端到实体的映射

**BigWorld:** `setClient(entityID)` 消息设置"当前处理实体"，后续消息分发到该实体。
这是因为 BigWorld 的外部 UDP 接口不在每条消息中携带 EntityID。

**Atlas:** 每条外部消息携带 `entity_id` 字段。原因:
- 当前外部协议走 RUDP，但仍选择显式 EntityID，避免 `select client` 这类隐式会话状态
- 更直观，无隐式状态
- `client_to_entity_` 映射仅用于连接断开时查找对应实体

### 9.4 异步实体销毁

**决策: destroy() 是异步操作，不立即释放。**

如果实体需要 writeToDB，destroy() 先发送写请求到 DBApp，在 WriteEntityAck 回调中
才真正释放。destroying_ 标志阻止销毁期间的新 RPC/writeToDB 调用。

如果不需要 writeToDB 且无 Cell 实体，destroy() 可同步完成。

### 9.5 外部接口安全

**决策: 多层安全策略。**

| 层 | 机制 | 说明 |
|---|---|---|
| 连接层 | 认证超时 (10s) | 连接后必须尽快 Authenticate |
| 连接层 | 最大连接数 | 防止连接耗尽 |
| 消息层 | 白名单过滤 | 认证前只允许 Authenticate + Heartbeat |
| 消息层 | 速率限制 | 复用 NetworkInterface 现有机制 |
| 会话层 | 空闲超时 (60s) | 清理僵尸连接 |
| RPC 层 | EntityDefRegistry 校验 | 只允许 exposed cell/base 方法 |

### 9.6 脏属性同步

**决策: C# 侧主动推送 delta blob。**

C# Source Generator 为每个 [Replicated] 属性生成 dirty tracking 代码。
每 tick C# 检查 dirty flags，如果有变更则通过 NativeApi 推送 delta 到 C++，
C++ 直接转发到客户端。C++ 不解析 delta 内容。

### 9.7 初期不实现的 BigWorld 功能

| 功能 | 原因 | 何时实现 |
|------|------|---------|
| 远程 giveClientTo | 需要 BaseAppMgr 路由 | Phase 9 |
| 实体备份 (BackupHash) | 需要多 BaseApp | Phase 13 |
| DataDownloads 流式下载 | 初期不需要大文件传输 | 按需 |
| Global Bases | 需要 BaseAppMgr 广播 | Phase 9 |
| Secondary DB (SQLite) | Atlas 不采用此方案 | 不实现 |
| Cell 数据请求 (writeToDB + cell) | 需要 CellApp | Phase 10 |
| 客户端加密 | 初期明文 | 按需加入 TLS |
