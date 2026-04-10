# Phase 7: DBApp + 数据库层

> 前置依赖: Phase 5 (ServerApp/ManagerApp), Phase 6 (machined), Script Phase 4 (`[Persistent]` + Source Generator)
> BigWorld 参考: `server/dbapp/`, `lib/db_storage/idatabase.hpp`, `lib/db_storage_mysql/`, `lib/db_storage_xml/`

---

## 目标

实现实体持久化子系统。DBApp 作为异步数据库代理进程，负责实体的存取/删除/查找，
避免数据库 I/O 阻塞 BaseApp 游戏逻辑。

## 验收标准

- [ ] `IDatabase` 接口完成，支持 put/get/del/lookup + checkout 追踪
- [ ] XML 后端可用（开发/测试用，无需外部数据库服务器）
- [ ] MySQL 后端可用（生产用，基于 `EntityDefRegistry` 的属性描述自动建表）
- [ ] DBApp 进程可启动，注册到 machined，接收 BaseApp 消息
- [ ] 后台线程池异步执行数据库操作，主线程不阻塞
- [ ] Checkout 机制防止同一实体被两个 BaseApp 同时加载
- [ ] 全部新增代码有单元测试

---

## 1. BigWorld 架构分析与 Atlas 适配

### 1.1 BigWorld DBApp 核心机制

| 机制 | BigWorld 实现 | 说明 |
|------|-------------|------|
| **异步 I/O** | `BgTaskManager` 线程池 + 主线程回调 | MySQL 操作在后台线程，结果在主线程处理 |
| **Checkout 追踪** | `BigWorldLogOns` 表 + 内存缓存 | 实体被 BaseApp 加载时记录位置，防止重复加载 |
| **Handler 模式** | 每个操作一个 Handler 对象（自删除） | `WriteEntityHandler`, `LoadEntityHandler` 等 |
| **实体序列化** | 按属性定义顺序的二进制流 | `addToStream(ONLY_PERSISTENT_DATA)` |
| **属性→列映射** | `PropertyMapping` 继承体系 (20+ 子类) | 每种类型一个 Mapping，生成 SQL |
| **MD5 校验** | 持久属性 MD5 摘要 | 确保 BaseApp/DBApp 的 entitydef 一致 |
| **DBID 生成** | MySQL AUTO_INCREMENT / XML 计数器 | 首次 writeToDB 返回分配的 DBID |
| **Auto-Load** | `UpdateAutoLoad` 标志 | 服务器启动时自动恢复实体 |
| **Secondary DB** | BaseApp 本地 SQLite + 合并 | 减少远程 MySQL 写入（Atlas 不需要） |
| **多 DBApp** | Rendezvous Hash 分片 | `DBID % numDBApps`（Phase 13 再实现） |

### 1.2 Atlas 适配决策

| 方面 | Atlas 决策 | 原因 |
|------|-----------|------|
| **异步模型** | 复用已有 `BgTaskManager` | 线程池 + 主线程回调，已验证 |
| **实体序列化** | C# Source Generator 二进制流 | `[Persistent]` 属性由 `Serialize()` 生成，C++ 侧不解析内容 |
| **属性→列映射** | 混合策略：基本类型→列，复杂类型→BLOB | BigWorld 的 20+ Mapping 子类过于复杂 |
| **Checkout** | 保留，内存 Map + DB 标记 | 防止重复加载是核心安全需求 |
| **MD5 校验** | 保留，从 `EntityDefRegistry` 计算 | 确保 DBApp 和 BaseApp 的实体定义一致 |
| **Secondary DB** | 不实现 | 现代 SSD + 连接池足够，无需本地 SQLite |
| **多 DBApp** | 初期单 DBApp，接口预留分片 | Phase 13 实现 DBAppMgr |
| **Auto-Load** | 保留 | 服务器重启时恢复在线玩家 |
| **Handler 模式** | 简化为回调函数 | C++ lambda 比 BigWorld 的 Handler 自删除对象更安全 |

### 1.3 C# 脚本层对数据库层的核心影响

**BigWorld (Python):**
```
Python entity.writeToDB()
  → Python 序列化持久属性 (pickle + 自定义流)
  → C++ Base::addToStream(ONLY_PERSISTENT_DATA)
  → C++ 遍历 entitydef，按属性类型调用 PropertyMapping
  → MySQL 每个属性对应一列
```

**Atlas (C#):**
```
C# entity.WriteToDB()
  → C# Source Generator 生成的 Serialize([Persistent]) 方法
  → SpanWriter 生成二进制 blob
  → NativeApi → C++ 发送到 DBApp
  → C++ DBApp 收到 blob，不解析内容
  → 存储策略:
      a) 简单属性 (string/int/float) → 单独列 (可查询)
      b) 整体 blob → 一列 (快速读写)
```

**关键差异:**
- BigWorld 的 C++ 层知道每个属性的类型和值（因为序列化在 C++ 中发生）
- Atlas 的 C++ 层只知道属性元信息（从 `EntityDefRegistry`），**不解析**实际值
- 属性值由 C# `SpanWriter` 序列化，C++ 只是传递和存储二进制数据
- 这大幅简化了 C++ 侧的 PropertyMapping 体系

**存储策略选择:**

| 策略 | 优势 | 劣势 | 适用场景 |
|------|------|------|---------|
| **纯 BLOB** | 最简单，C++ 不需要解析 | 无法 SQL 查询单个属性 | 初期实现，适合大多数场景 |
| **标识符列 + BLOB** | 可按名字/ID 查找实体 | 需要知道标识符属性 | **推荐方案** |
| **全列映射** | 完全可查询 | C++ 需解析属性值，复杂 | 后续可选优化 |

> **推荐: 标识符列 + BLOB。** 只有被 `[Identifier]` 标记的属性（如账号名）提取为独立列，
> 其余持久属性作为整体 BLOB 存储。这在查询能力和实现复杂度之间取得平衡。

---

## 2. 消息协议设计

### 2.1 DBApp 接口消息

| 消息 | ID | 方向 | 用途 |
|------|-----|------|------|
| `WriteEntity` | 4000 | BaseApp → DBApp | 保存实体 (新建或更新) |
| `WriteEntityAck` | 4001 | DBApp → BaseApp | 保存结果 (成功 + DBID) |
| `LoadEntity` | 4002 | BaseApp → DBApp | 加载实体 (按 DBID 或名字) |
| `LoadEntityAck` | 4003 | DBApp → BaseApp | 加载结果 (实体数据) |
| `DeleteEntity` | 4004 | BaseApp → DBApp | 删除实体 |
| `DeleteEntityAck` | 4005 | DBApp → BaseApp | 删除结果 |
| `LookupEntity` | 4006 | BaseApp → DBApp | 按名字查找 DBID |
| `LookupEntityAck` | 4007 | DBApp → BaseApp | 查找结果 |
| `CheckoutEntity` | 4008 | BaseApp → DBApp | 检出实体 (加载 + 锁定) |
| `CheckoutEntityAck` | 4009 | DBApp → BaseApp | 检出结果 |
| `CheckinEntity` | 4010 | BaseApp → DBApp | 归还实体 (解除锁定) |

### 2.2 消息详细定义

#### WriteEntity

```cpp
namespace atlas::dbapp {

/// 写入标志 (参照 BigWorld WriteDBFlags)
enum class WriteFlags : uint8_t {
    None          = 0,
    CreateNew     = 1 << 0,  // 新实体，DBID=0，由 DB 分配
    ExplicitDBID  = 1 << 1,  // 使用指定的 DBID
    LogOff        = 1 << 2,  // 实体下线，清除 checkout 记录
    Delete        = 1 << 3,  // 从数据库删除
    AutoLoadOn    = 1 << 4,  // 标记为自动加载
    AutoLoadOff   = 1 << 5,  // 取消自动加载
    CellData      = 1 << 6,  // 预留: blob 中包含 Cell 数据 (Phase 10 实现)
    Reserved      = 1 << 7,  // 预留
};

struct WriteEntity {
    WriteFlags flags;
    uint16_t type_id;
    int64_t dbid;              // 0 = 新实体
    uint32_t entity_id;        // BaseApp 上的 EntityID
    uint32_t request_id;       // 用于匹配 Ack
    std::string identifier;    // [Identifier] 属性值 (用于 sm_identifier 列)
    // payload: 持久属性的二进制 blob (C# SpanWriter 序列化)
    // 在 Variable 消息中跟在固定字段后面
    //
    // 变长部分格式:
    //   [identifier_len: uint16] [identifier: bytes]
    //   [blob: 剩余全部字节]

    static auto descriptor() -> const MessageDesc& {
        static const MessageDesc desc{4000, "dbapp::WriteEntity",
            MessageLengthStyle::Variable, -1};
        return desc;
    }
    // serialize / deserialize ...
};

struct WriteEntityAck {
    uint32_t request_id;
    bool success;
    int64_t dbid;              // 分配的或已有的 DBID
    std::string error;         // 失败原因 (空=成功)

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

} // namespace atlas::dbapp
```

#### LoadEntity / CheckoutEntity

```cpp
namespace atlas::dbapp {

enum class LoadMode : uint8_t {
    ByDBID = 0,           // 按 DBID 加载
    ByName = 1,           // 按标识符属性名加载
};

struct CheckoutEntity {
    LoadMode mode;
    uint16_t type_id;
    int64_t dbid;              // mode=ByDBID 时有效
    std::string identifier;    // mode=ByName 时有效
    uint32_t entity_id;        // BaseApp 上要绑定的 EntityID
    uint32_t request_id;

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

struct CheckoutEntityAck {
    uint32_t request_id;
    uint8_t status;            // 0=成功, 1=未找到, 2=已被检出, 3=DB错误
    int64_t dbid;              // 实体 DBID
    // payload: 持久属性的二进制 blob (用于反序列化到 C# 实体)
    // 已被检出时: 附加当前持有者的 BaseApp 地址

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

} // namespace atlas::dbapp
```

#### CheckinEntity

```cpp
namespace atlas::dbapp {

struct CheckinEntity {
    uint16_t type_id;
    int64_t dbid;

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

} // namespace atlas::dbapp
```

---

## 3. 核心模块设计

### 3.1 IDatabase — 数据库抽象接口

```cpp
// src/lib/db/idatabase.hpp
namespace atlas {

using DatabaseID = int64_t;
constexpr DatabaseID kInvalidDBID = 0;

struct DatabaseConfig {
    std::string type;              // "xml" or "mysql"
    // XML 后端
    std::filesystem::path xml_dir; // 数据目录

    // MySQL 后端
    std::string mysql_host = "127.0.0.1";
    uint16_t mysql_port = 3306;
    std::string mysql_user;
    std::string mysql_password;
    std::string mysql_database = "atlas";
    int mysql_pool_size = 4;
};

/// 实体数据包
struct EntityData {
    DatabaseID dbid;
    uint16_t type_id;
    std::vector<std::byte> blob;       // 持久属性二进制
    std::string identifier;            // 标识符属性值（如账号名）
};

/// Checkout 信息
struct CheckoutInfo {
    Address base_addr;                 // 持有者 BaseApp 内部地址
    uint32_t app_id;                   // 持有者 BaseApp 的 AppID (比地址更稳定)
    uint32_t entity_id;                // BaseApp 上的 EntityID
};

/// 异步操作回调
struct PutResult {
    bool success;
    DatabaseID dbid;                   // 新分配或已有的 DBID
    std::string error;
};

struct GetResult {
    bool success;
    EntityData data;
    std::optional<CheckoutInfo> checked_out_by; // 如果已被检出
    std::string error;
};

struct DelResult {
    bool success;
    std::string error;
};

struct LookupResult {
    bool found;
    DatabaseID dbid;
    std::string password_hash;         // sm_passwordHash 值 (认证时使用，可为空)
};

class IDatabase {
public:
    virtual ~IDatabase() = default;

    /// 初始化 (连接数据库, 创建/迁移表结构)
    [[nodiscard]] virtual auto startup(const DatabaseConfig& config,
                                        const EntityDefRegistry& entity_defs)
        -> Result<void> = 0;

    /// 关闭
    virtual void shutdown() = 0;

    // ========== 实体 CRUD (异步) ==========

    /// 保存实体 (新建或更新)
    /// dbid=0 表示新实体，DB 分配 ID
    virtual void put_entity(DatabaseID dbid, uint16_t type_id,
                            std::span<const std::byte> blob,
                            const std::string& identifier,
                            std::function<void(PutResult)> callback) = 0;

    /// 加载实体 (按 DBID)
    virtual void get_entity(DatabaseID dbid, uint16_t type_id,
                            std::function<void(GetResult)> callback) = 0;

    /// 删除实体
    virtual void del_entity(DatabaseID dbid, uint16_t type_id,
                            std::function<void(DelResult)> callback) = 0;

    /// 按标识符查找 DBID
    virtual void lookup_by_name(uint16_t type_id,
                                 const std::string& identifier,
                                 std::function<void(LookupResult)> callback) = 0;

    // ========== Checkout 追踪 ==========
    // 设计决策: checkout 操作全部异步化，与 CRUD 保持一致。
    // BigWorld 将 checkout 信息合并在 putEntity/getEntity 中更新，Atlas 也采用此模式:
    //   - put_entity + WriteFlags::LogOff → 自动清除 checkout
    //   - get_entity 返回值 GetResult 中包含 checked_out_by 信息
    //   - checkout_entity 是 get + set_checkout 的原子组合 (见下)
    // 独立的 set/clear 仅用于特殊场景 (如 BaseApp 死亡批量清理)。

    /// 原子检出: 查询实体 + 设置 checkout (单次 DB 事务)
    /// 如果实体已被检出，返回 GetResult 中 checked_out_by 非空
    virtual void checkout_entity(DatabaseID dbid, uint16_t type_id,
                                  const CheckoutInfo& new_owner,
                                  std::function<void(GetResult)> callback) = 0;

    /// 按标识符原子检出
    virtual void checkout_entity_by_name(uint16_t type_id,
                                          const std::string& identifier,
                                          const CheckoutInfo& new_owner,
                                          std::function<void(GetResult)> callback) = 0;

    /// 清除 checkout (实体下线时, 通常与 put_entity + LogOff 配合)
    virtual void clear_checkout(DatabaseID dbid, uint16_t type_id,
                                 std::function<void(bool)> callback) = 0;

    /// 批量清除: 某个 BaseApp 死亡时清除其所有 checkout
    virtual void clear_checkouts_for_address(const Address& base_addr,
                                              std::function<void(int cleared_count)> callback) = 0;

    // ========== Auto-Load ==========

    /// 获取所有标记为自动加载的实体
    virtual void get_auto_load_entities(
        std::function<void(std::vector<EntityData>)> callback) = 0;

    /// 设置/清除自动加载标记
    virtual void set_auto_load(DatabaseID dbid, uint16_t type_id,
                                bool auto_load) = 0;

    // ========== 维护 ==========

    /// 在主线程调用，收取后台线程完成的回调
    virtual void process_results() = 0;

    /// 是否支持多 DBApp
    [[nodiscard]] virtual auto supports_multi_dbapp() const -> bool { return false; }
};

} // namespace atlas
```

**与 BigWorld `IDatabase` 的对比:**

| BigWorld | Atlas | 差异 |
|----------|-------|------|
| `getEntity(key, stream, handler)` | `get_entity(dbid, type_id, callback)` | Atlas 用 lambda 回调替代 Handler 对象 |
| `putEntity(key, entityID, stream, mailbox, ...)` | `put_entity(dbid, type_id, blob, identifier, callback)` | Atlas 简化参数，blob 不解析 |
| Handler 对象自删除 | `std::function` 回调 | 更安全，无手动内存管理 |
| `EntityDBKey` (name + dbid) | DBID 和 name 分开的 API | 更清晰 |
| `startup(EntityDefs, dispatcher, retries)` | `startup(config, entity_defs)` | Atlas 传 `EntityDefRegistry` |
| `set_checkout` / `get_checkout` 同步 | `checkout_entity` 异步 (含原子 get+lock) | 全部异步，MySQL 安全 |
| `BigWorldLogOns` 无密码 | `sm_passwordHash` 列 | Atlas 含认证数据 |
| `remapEntityMailboxes` (死亡重映射) | `clear_checkouts_for_address` (异步) | Atlas 初期仅清理 |

### 3.2 CheckoutManager — 实体检出管理

防止同一实体被两个 BaseApp 同时加载（BigWorld 的核心安全机制）。

```cpp
// src/server/DBApp/checkout_manager.hpp
namespace atlas {

class CheckoutManager {
public:
    /// 检出结果
    struct CheckoutResult {
        enum Status {
            Success,               // 可以检出
            AlreadyCheckedOut,     // 被别人检出
            PendingCheckout,       // 正在被检出中 (等待)
        };
        Status status;
        CheckoutInfo current_owner;  // AlreadyCheckedOut 时返回持有者信息
    };

    auto try_checkout(DatabaseID dbid, uint16_t type_id,
                      const CheckoutInfo& new_owner) -> CheckoutResult;

    /// 完成检出 (DB 操作成功后调用)
    void confirm_checkout(DatabaseID dbid, uint16_t type_id);

    /// 归还实体 (实体下线)
    void checkin(DatabaseID dbid, uint16_t type_id);

    /// 查询持有者
    auto get_owner(DatabaseID dbid, uint16_t type_id) const
        -> std::optional<CheckoutInfo>;

    /// BaseApp 死亡时清除其所有 checkout
    void clear_all_for(const Address& base_addr);

private:
    struct Key {
        DatabaseID dbid;
        uint16_t type_id;
        auto operator==(const Key&) const -> bool = default;
    };
    struct KeyHash {
        auto operator()(const Key& k) const -> size_t;
    };

    enum class State { Checking, Confirmed };

    struct Entry {
        CheckoutInfo owner;
        State state;
    };

    std::unordered_map<Key, Entry, KeyHash> checkouts_;
};

} // namespace atlas
```

**BigWorld 对照:**
- BigWorld 在 `BigWorldLogOns` MySQL 表中持久化 checkout 记录，内存用 `LogOnRecordsCache`
- Atlas 初期 checkout 仅在内存中，DBApp 重启时从 DB 的 `checkout` 列恢复
- `PendingCheckout` 状态处理并发检出请求（BigWorld 的 `registerCheckoutCompletionListener`）

### 3.3 XML 后端

```cpp
// src/lib/db_xml/xml_database.hpp
namespace atlas {

class XmlDatabase : public IDatabase {
public:
    auto startup(const DatabaseConfig& config,
                  const EntityDefRegistry& entity_defs) -> Result<void> override;
    void shutdown() override;

    void put_entity(DatabaseID dbid, uint16_t type_id,
                    std::span<const std::byte> blob,
                    const std::string& identifier,
                    std::function<void(PutResult)> callback) override;

    void get_entity(DatabaseID dbid, uint16_t type_id,
                    std::function<void(GetResult)> callback) override;

    void del_entity(DatabaseID dbid, uint16_t type_id,
                    std::function<void(DelResult)> callback) override;

    void lookup_by_name(uint16_t type_id, const std::string& identifier,
                         std::function<void(LookupResult)> callback) override;

    // Checkout (内存实现)
    void checkout_entity(DatabaseID dbid, uint16_t type_id,
                          const CheckoutInfo& new_owner,
                          std::function<void(GetResult)> callback) override;
    void checkout_entity_by_name(uint16_t type_id, const std::string& identifier,
                                  const CheckoutInfo& new_owner,
                                  std::function<void(GetResult)> callback) override;
    void clear_checkout(DatabaseID dbid, uint16_t type_id,
                         std::function<void(bool)> callback) override;
    void clear_checkouts_for_address(const Address& base_addr,
                                      std::function<void(int)> callback) override;

    // Auto-load
    void get_auto_load_entities(
        std::function<void(std::vector<EntityData>)> callback) override;
    void set_auto_load(DatabaseID dbid, uint16_t type_id, bool auto_load) override;

    void process_results() override;  // 同步实现，直接调用回调

private:
    std::filesystem::path base_dir_;
    DatabaseID next_dbid_ = 1;
    const EntityDefRegistry* entity_defs_ = nullptr;

    // 名字 → DBID 索引 (per type)
    using NameIndex = std::unordered_map<std::string, DatabaseID>;
    std::unordered_map<uint16_t, NameIndex> name_indices_;

    // Checkout 内存表
    std::unordered_map<uint64_t, CheckoutInfo> checkouts_; // key = (type_id << 48) | dbid

    // Auto-load
    std::set<uint64_t> auto_load_set_;
};

} // namespace atlas
```

**文件存储格式:**

```
data/db/
├── meta.json                  # { "next_dbid": 42 }
├── Avatar/
│   ├── 1.bin                  # DBID=1 的持久属性 blob
│   ├── 2.bin
│   └── index.json             # { "hero_name": 1, "another_name": 2 }
├── Account/
│   ├── 1.bin
│   └── index.json
└── auto_load.json             # [{"type_id": 1, "dbid": 1}, ...]
```

> **选择 .bin 而非 .xml:** Atlas 的持久数据是 C# `SpanWriter` 的二进制 blob，
> 存为 XML 没有意义。直接存二进制文件更快，index.json 提供名字索引可读性。

**同步执行:** XML 后端不使用后台线程。文件 I/O 在主线程直接执行，
`process_results()` 为空操作。开发/测试场景下延迟可接受。

> **注意: 同步 vs 异步行为差异。**
> XML 后端的回调在调用方法时同步执行，MySQL 后端在下一次 `process_results()` 中异步执行。
> 依赖回调同步完成的代码在 XML 下正常、MySQL 下异常。
> 建议: XmlDatabase 可提供一个 `deferred_callbacks` 模式，将回调推迟到 `process_results()`
> 中统一执行，用于排查异步时序 Bug:
> ```cpp
> // xml_database.hpp
> bool deferred_mode_ = false;  // 测试时可设为 true
> std::vector<std::function<void()>> deferred_;
>
> void process_results() override {
>     for (auto& cb : deferred_) cb();
>     deferred_.clear();
> }
> ```

### 3.4 MySQL 后端

```cpp
// src/lib/db_mysql/mysql_database.hpp
namespace atlas {

class MysqlDatabase : public IDatabase {
public:
    auto startup(const DatabaseConfig& config,
                  const EntityDefRegistry& entity_defs) -> Result<void> override;
    void shutdown() override;

    void put_entity(DatabaseID dbid, uint16_t type_id,
                    std::span<const std::byte> blob,
                    const std::string& identifier,
                    std::function<void(PutResult)> callback) override;

    void get_entity(DatabaseID dbid, uint16_t type_id,
                    std::function<void(GetResult)> callback) override;

    void del_entity(DatabaseID dbid, uint16_t type_id,
                    std::function<void(DelResult)> callback) override;

    void lookup_by_name(uint16_t type_id, const std::string& identifier,
                         std::function<void(LookupResult)> callback) override;

    // ... checkout, auto_load ...

    void process_results() override;

    auto supports_multi_dbapp() const -> bool override { return true; }

private:
    /// 后台任务管理 (复用 BgTaskManager)
    std::unique_ptr<BgTaskManager> bg_tasks_;

    /// MySQL 连接池
    std::unique_ptr<MysqlConnectionPool> pool_;

    /// 实体定义引用 (用于建表)
    const EntityDefRegistry* entity_defs_ = nullptr;

    /// 待执行回调 (后台线程放入，主线程取出)
    struct PendingCallback {
        std::function<void()> callback;
    };
    std::mutex pending_mutex_;
    std::vector<PendingCallback> pending_callbacks_;
};

} // namespace atlas
```

**表结构 (每个实体类型一张表):**

```sql
CREATE TABLE IF NOT EXISTS `tbl_Avatar` (
    `sm_dbid`       BIGINT PRIMARY KEY AUTO_INCREMENT,
    `sm_typeId`     SMALLINT UNSIGNED NOT NULL,
    `sm_identifier` VARCHAR(255) DEFAULT NULL,     -- [Identifier] 属性值
    `sm_passwordHash` VARCHAR(128) DEFAULT NULL,   -- 密码哈希 (bcrypt/argon2, 仅 Account 类型)
    `sm_data`       MEDIUMBLOB NOT NULL,            -- 持久属性二进制 blob
    `sm_autoLoad`   TINYINT(1) NOT NULL DEFAULT 0,
    `sm_checkedOut` TINYINT(1) NOT NULL DEFAULT 0,
    `sm_baseAddr`   BINARY(6) DEFAULT NULL,         -- 检出者地址 (4 IP + 2 port)
    `sm_baseAppId`  INT UNSIGNED DEFAULT NULL,      -- 检出者的 AppID (比地址更稳定)
    `sm_entityId`   INT UNSIGNED DEFAULT NULL,      -- 检出者的 EntityID
    `sm_createTime` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `sm_updateTime` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,

    UNIQUE INDEX `idx_identifier` (`sm_identifier`),
    INDEX `idx_autoLoad` (`sm_autoLoad`),
    INDEX `idx_checkedOut` (`sm_checkedOut`, `sm_baseAddr`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

**与 BigWorld 表结构的对比:**

| BigWorld | Atlas | 差异 |
|----------|-------|------|
| 每个属性一列 | `sm_data` BLOB + `sm_identifier` 列 | Atlas 大幅简化，只提取标识符 |
| `BigWorldLogOns` 独立表 | `sm_checkedOut` + `sm_baseAddr` 内嵌 | 减少 JOIN |
| 20+ PropertyMapping 子类 | 无需 PropertyMapping | 序列化由 C# 完成 |
| 每种集合类型创建子表 | 集合在 blob 中 | 简化，代价是无法 SQL 查询集合元素 |

**自动建表/迁移:**

```cpp
// startup() 中:
for (const auto& type_desc : entity_defs.all_types()) {
    auto table_name = "tbl_" + type_desc.name;

    // 检查表是否存在
    if (!table_exists(table_name)) {
        create_table(table_name, type_desc);
    } else {
        // 检查 identifier 属性是否变更
        migrate_table_if_needed(table_name, type_desc);
    }
}
```

> **Schema 版本:** 在 `atlas_meta` 表中记录 schema 版本号。
> 迁移策略保守：只允许**新增列**和**修改 identifier 列类型**，
> 删除属性不删列（数据保留），结构性变更需要人工介入。

**MysqlConnectionPool:**

```cpp
// src/lib/db_mysql/mysql_connection_pool.hpp
namespace atlas {

class MysqlConnectionPool {
public:
    explicit MysqlConnectionPool(const DatabaseConfig& config);
    ~MysqlConnectionPool();

    /// 获取连接 (阻塞等待，用于后台线程)
    [[nodiscard]] auto acquire() -> MysqlConnection;

    /// 归还连接
    void release(MysqlConnection conn);

    /// 连接数
    [[nodiscard]] auto size() const -> int;

private:
    // 实现使用信号量 + 队列
    std::queue<MysqlConnection> available_;
    std::mutex mutex_;
    std::condition_variable cv_;
    int max_size_;
};

} // namespace atlas
```

**后台任务模式:**

```cpp
void MysqlDatabase::put_entity(DatabaseID dbid, uint16_t type_id,
                                std::span<const std::byte> blob,
                                const std::string& identifier,
                                std::function<void(PutResult)> callback)
{
    // 复制 blob 到 vector (跨线程传递)
    auto data = std::vector<std::byte>(blob.begin(), blob.end());
    auto id_copy = identifier;

    bg_tasks_->add_task(
        // 后台线程
        [this, dbid, type_id, data = std::move(data),
         id_copy = std::move(id_copy)]() mutable
        {
            auto conn = pool_->acquire();
            // 执行 INSERT or UPDATE ...
            // result = ...
            pool_->release(std::move(conn));
            return result;
        },
        // 主线程回调
        [callback = std::move(callback), result]() {
            callback(result);
        }
    );
}
```

### 3.5 DBApp 进程

```cpp
// src/server/DBApp/dbapp.hpp
namespace atlas {

class DBApp : public ManagerApp {
public:
    using ManagerApp::ManagerApp;

protected:
    auto init(int argc, char* argv[]) -> bool override;
    void fini() override;
    void on_tick_complete() override;

    void register_watchers() override;

private:
    // ---- 消息处理器 ----
    void on_write_entity(const Address& src, Channel* ch,
                         const dbapp::WriteEntity& msg);
    void on_checkout_entity(const Address& src, Channel* ch,
                            const dbapp::CheckoutEntity& msg);
    void on_checkin_entity(const Address& src, Channel* ch,
                           const dbapp::CheckinEntity& msg);
    void on_delete_entity(const Address& src, Channel* ch,
                          const dbapp::DeleteEntity& msg);
    void on_lookup_entity(const Address& src, Channel* ch,
                          const dbapp::LookupEntity& msg);

    // ---- Birth/Death 处理 ----
    void on_baseapp_death(const machined::DeathNotification& notif);

    std::unique_ptr<IDatabase> database_;
    CheckoutManager checkout_mgr_;
    const EntityDefRegistry* entity_defs_ = nullptr;
};

} // namespace atlas
```

**`init()` 流程:**

```
DBApp::init():
    ManagerApp::init()

    // 从 EntityDefRegistry 获取实体定义 (ScriptApp 未来注册, 或从配置加载)
    // 注: DBApp 是 ManagerApp，没有脚本引擎
    // 实体定义通过 machined 或直接从配置文件加载
    entity_defs_ = load_entity_defs_from_config()

    // 创建数据库后端
    if (config().db_type == "xml")
        database_ = make_unique<XmlDatabase>()
    else
        database_ = make_unique<MysqlDatabase>()

    database_->startup(config().db_config, *entity_defs_)

    // 注册消息处理器
    register_handlers()

    // 监听 BaseApp 死亡 (清理 checkout)
    machined_client().listen_for_death(ProcessType::BaseApp,
        [this](auto& notif) { on_baseapp_death(notif); })
```

**`on_tick_complete()` 流程:**

```
DBApp::on_tick_complete():
    database_->process_results()    // 收取后台线程完成的回调
```

**BaseApp 死亡处理:**

```cpp
void DBApp::on_baseapp_death(const machined::DeathNotification& notif) {
    // 清除死亡 BaseApp 的所有 checkout 记录 (内存 + DB)
    checkout_mgr_.clear_all_for(notif.internal_addr);
    database_->clear_checkouts_for_address(notif.internal_addr,
        [name = notif.name](int count) {
            ATLAS_LOG_WARNING("Cleared {} checkouts for dead BaseApp: {}", count, name);
        });
}
```

### 3.6 EntityDefRegistry 在 DBApp 中的加载

**问题:** DBApp 是 `ManagerApp`，没有 C# 脚本引擎。但它需要 `EntityDefRegistry` 来建表和校验。

**方案:** DBApp 从**配置文件**加载实体定义。

```
启动时:
1. C# 编译期: Source Generator 生成 entity_defs.json (实体类型描述)
2. 部署时: entity_defs.json 随服务器程序分发
3. DBApp 启动: 从 entity_defs.json 解析到 EntityDefRegistry
4. BaseApp 启动: C# 运行时通过 NativeApi 注册到 EntityDefRegistry
5. 校验: BaseApp 注册的 MD5 与 DBApp 的 MD5 一致才允许工作
```

```cpp
// src/lib/entitydef/entity_def_registry.hpp (扩展)
class EntityDefRegistry {
public:
    // 现有: 从 C# 二进制注册
    Result<void> register_type(const std::byte* data, int32_t len);

    // 新增: 从 JSON 文件加载 (DBApp 用)
    static auto from_json_file(const std::filesystem::path& path)
        -> Result<EntityDefRegistry>;

    // MD5 摘要 (持久属性)
    auto persistent_properties_digest() const -> std::array<uint8_t, 16>;
};
```

---

## 4. 端到端流程

### 4.1 writeToDB (新实体)

```
C# Avatar entity           BaseApp (C++)           DBApp             MySQL
     │                         │                      │                 │
     │ WriteToDB()             │                      │                 │
     │ → Serialize([Persistent])                      │                 │
     │ → SpanWriter blob       │                      │                 │
     │ → NativeApi ──────────→│                      │                 │
     │                         │                      │                 │
     │                         │── WriteEntity ──────→│                 │
     │                         │   (flags=CreateNew,  │                 │
     │                         │    type_id=1,        │                 │
     │                         │    dbid=0,           │                 │
     │                         │    blob=...)         │                 │
     │                         │                      │                 │
     │                         │                      │── INSERT ──────→│
     │                         │                      │   tbl_Avatar    │
     │                         │                      │   LAST_INSERT_ID│
     │                         │                      │←── dbid=42 ────│
     │                         │                      │                 │
     │                         │←── WriteEntityAck ──│                 │
     │                         │   (success, dbid=42) │                 │
     │                         │                      │                 │
     │←── callback(dbid=42) ──│                      │                 │
```

### 4.2 CheckoutEntity (登录加载)

```
BaseApp (C++)                  DBApp                 MySQL
     │                           │                      │
     │── CheckoutEntity ────────→│                      │
     │   (mode=ByName,           │                      │
     │    identifier="hero123")  │                      │
     │                           │                      │
     │                           │ checkout_mgr_        │
     │                           │   .try_checkout()    │
     │                           │                      │
     │                           │── SELECT ───────────→│
     │                           │   WHERE identifier=  │
     │                           │   AND checkedOut=0   │
     │                           │←── row (dbid, blob) ─│
     │                           │                      │
     │                           │── UPDATE ───────────→│
     │                           │   SET checkedOut=1,  │
     │                           │   baseAddr=...       │
     │                           │←── ok ──────────────│
     │                           │                      │
     │←── CheckoutEntityAck ────│                      │
     │   (status=Success,        │                      │
     │    dbid=42,               │                      │
     │    blob=...)              │                      │
     │                           │                      │
     │ → C# Deserialize(blob)   │                      │
     │ → 恢复实体状态            │                      │
```

### 4.3 Checkin (实体下线)

```
BaseApp                        DBApp                 MySQL
     │                           │                      │
     │── WriteEntity ───────────→│                      │
     │   (flags=LogOff,          │                      │
     │    dbid=42, blob=...)     │                      │
     │                           │── UPDATE blob ──────→│
     │                           │── UPDATE checkedOut=0→│
     │                           │                      │
     │                           │ checkout_mgr_        │
     │                           │   .checkin(42)       │
     │                           │                      │
     │←── WriteEntityAck ───────│                      │
```

---

## 5. 实现步骤

### Step 7.1: 数据库抽象层 (`src/lib/db/`)

**新增文件:**
```
src/lib/db/
├── CMakeLists.txt
├── idatabase.hpp              # IDatabase 接口 + 数据结构
├── database_config.hpp        # DatabaseConfig
└── database_factory.hpp / .cpp # create_database(config) 工厂
tests/unit/test_database_types.cpp
```

**交付:** IDatabase 接口定义、数据结构、工厂函数。无需实现。

### Step 7.2: XML 后端 (`src/lib/db_xml/`)

**新增文件:**
```
src/lib/db_xml/
├── CMakeLists.txt
├── xml_database.hpp / .cpp
tests/unit/test_xml_database.cpp
```

**测试用例:**
- put (新建) → get → 数据一致
- put (更新) → get → 数据已更新
- del → get → 未找到
- lookup_by_name → 正确 DBID
- lookup 不存在的名字 → found=false
- DBID 自增不重复
- checkout / checkin 流程
- auto_load 标记和查询
- 重启后数据仍在（文件持久化）

### Step 7.3: DBApp 消息定义

**新增文件:**
```
src/server/DBApp/dbapp_messages.hpp
tests/unit/test_dbapp_messages.cpp
```

所有 DBApp 消息的 serialize/deserialize 往返测试。

### Step 7.4: CheckoutManager

**新增文件:**
```
src/server/DBApp/checkout_manager.hpp / .cpp
tests/unit/test_checkout_manager.cpp
```

**测试用例:**
- 正常检出/归还
- 重复检出 → `AlreadyCheckedOut`
- 并发检出同一实体 → 只有一个成功
- BaseApp 死亡 → `clear_all_for` 清理所有 checkout
- 确认检出 → 状态从 Checking 变为 Confirmed

### Step 7.5: DBApp 进程

**新增文件:**
```
src/server/DBApp/
├── CMakeLists.txt
├── main.cpp
├── dbapp.hpp / .cpp
├── dbapp_messages.hpp         (from 7.3)
├── checkout_manager.hpp / .cpp (from 7.4)
```

**实现顺序:**
1. 基本启动（ManagerApp + machined 注册）
2. 加载 EntityDefRegistry（从 JSON）
3. 创建 IDatabase 后端
4. WriteEntity 处理
5. CheckoutEntity / CheckinEntity 处理
6. DeleteEntity / LookupEntity 处理
7. BaseApp 死亡监听 → 清理 checkout
8. Watcher 注册

### Step 7.6: EntityDefRegistry 扩展

**更新文件:**
```
src/lib/entitydef/entity_def_registry.hpp / .cpp (扩展)
tests/unit/test_entity_def_registry_json.cpp
```

新增 `from_json_file()` 方法和 `persistent_properties_digest()` 方法。

**entity_defs.json 生成与部署路径:**

```
编译期:
  C# Atlas.Shared 项目构建时，Source Generator 生成 entity_defs.json
  → $(OutputDir)/config/entity_defs.json

部署期:
  entity_defs.json 随服务器程序分发到部署目录
  目录结构: deploy/
             ├── bin/atlas_dbapp
             ├── bin/atlas_baseapp
             └── config/entity_defs.json

运行期:
  DBApp 通过配置加载: --entitydef-path config/entity_defs.json
  或 ServerConfig JSON: "entitydef_path": "config/entity_defs.json"
```

**entity_defs.json 格式:**

```json
{
    "version": 1,
    "md5_digest": "a1b2c3...",
    "types": [
        {
            "type_id": 1,
            "name": "Account",
            "properties": [
                {"name": "accountName", "type": "string", "persistent": true, "identifier": true},
                {"name": "level", "type": "int32", "persistent": true, "identifier": false}
            ],
            "rpcs": [...]
        }
    ]
}
```

### Step 7.7: MySQL 后端 (`src/lib/db_mysql/`)

**新增文件:**
```
src/lib/db_mysql/
├── CMakeLists.txt
├── mysql_database.hpp / .cpp
├── mysql_connection_pool.hpp / .cpp
├── mysql_connection.hpp / .cpp    # RAII 连接包装
tests/unit/test_mysql_database.cpp  (需要 MySQL 实例)
```

**依赖:** MySQL C Connector (libmysqlclient) 或 MariaDB Connector/C

**实现顺序:**
1. MysqlConnection — RAII 连接
2. MysqlConnectionPool — 线程安全连接池
3. 自动建表（从 EntityDefRegistry）
4. put_entity (INSERT / UPDATE)
5. get_entity (SELECT)
6. del_entity (DELETE)
7. lookup_by_name (SELECT by identifier)
8. checkout 列管理
9. auto_load 查询
10. Schema 迁移

### Step 7.8: 集成测试

**新增文件:**
```
tests/integration/test_dbapp_integration.cpp
```

端到端场景:
1. 启动 machined + DBApp (XML 后端)
2. 模拟 BaseApp 发送 WriteEntity (新建)
3. 发送 CheckoutEntity → 获取数据
4. 发送 WriteEntity (更新)
5. 发送 CheckinEntity (归还)
6. 再次 CheckoutEntity → 获取更新后的数据
7. 模拟 BaseApp 死亡 → checkout 自动清除

---

## 6. 文件清单汇总

```
src/lib/db/
├── CMakeLists.txt
├── idatabase.hpp
├── database_config.hpp
└── database_factory.hpp / .cpp

src/lib/db_xml/
├── CMakeLists.txt
└── xml_database.hpp / .cpp

src/lib/db_mysql/
├── CMakeLists.txt
├── mysql_database.hpp / .cpp
├── mysql_connection_pool.hpp / .cpp
└── mysql_connection.hpp / .cpp

src/lib/entitydef/                    (扩展)
├── entity_def_registry.hpp / .cpp    (+ from_json_file, digest)

src/server/DBApp/
├── CMakeLists.txt
├── main.cpp
├── dbapp.hpp / .cpp
├── dbapp_messages.hpp
└── checkout_manager.hpp / .cpp

tests/unit/
├── test_database_types.cpp
├── test_xml_database.cpp
├── test_dbapp_messages.cpp
├── test_checkout_manager.cpp
└── test_entity_def_registry_json.cpp

tests/integration/
└── test_dbapp_integration.cpp
```

---

## 7. 依赖关系与执行顺序

```
Step 7.1: IDatabase 接口         ← 无依赖
    │
    ├── Step 7.2: XML 后端        ← 依赖 7.1
    │
    └── Step 7.7: MySQL 后端      ← 依赖 7.1, 可延后

Step 7.3: DBApp 消息定义          ← 无依赖
Step 7.4: CheckoutManager         ← 无依赖
Step 7.6: EntityDefRegistry 扩展  ← 无依赖

    全部就绪后:
Step 7.5: DBApp 进程              ← 依赖 7.1-7.4, 7.6
Step 7.8: 集成测试                ← 依赖 7.5
```

**推荐执行顺序:**

```
第 1 轮 (并行): 7.1 IDatabase + 7.3 消息定义 + 7.4 CheckoutManager + 7.6 EntityDefRegistry
第 2 轮:        7.2 XML 后端
第 3 轮:        7.5 DBApp 进程 (使用 XML 后端验证)
第 4 轮:        7.7 MySQL 后端 (可独立开发, 替换 XML 后端测试)
第 5 轮:        7.8 集成测试
```

---

## 8. BigWorld 完整对照

| BigWorld | Atlas | 差异说明 |
|----------|-------|---------|
| `IDatabase` (20+ 方法) | `IDatabase` (精简到核心 CRUD + checkout) | Atlas 不需要 Secondary DB、consolidate 等 |
| Handler 对象模式 (自删除) | `std::function` 回调 | 更安全，无手动内存管理 |
| `EntityDefs` (XML 解析 + MD5) | `EntityDefRegistry` (C# 注册/JSON 加载 + MD5) | Source Generator 替代 XML 解析 |
| 每属性一列 (PropertyMapping 体系) | `sm_identifier` 列 + `sm_data` BLOB | **核心简化：** C# 序列化 blob，C++ 不解析 |
| `BigWorldLogOns` 独立表 | `sm_checkedOut` 列内嵌 | 减少 JOIN |
| `BgTaskManager` 线程池异步 | `BgTaskManager` (已有) | 直接复用 |
| MySQL + XML + SQLite 三后端 | XML + MySQL 两后端 | 不需要 SQLite Secondary DB |
| DBAppMgr + Rendezvous Hash | 单 DBApp (Phase 13 扩展) | 初期简化 |
| `LogOnRecordsCache` | `CheckoutManager` 内存 Map | 功能相同 |
| Auto-Load (`UpdateAutoLoad`) | `auto_load` 标记 + 查询 | 功能相同 |
| `WriteDBFlags` (10 个标志位) | `WriteFlags` (6 个) | 移除 Secondary DB 相关 |
| Python `base.writeToDB()` | C# `entity.WriteToDB()` | C# Source Generator 序列化 |
| `base.addToStream(ONLY_PERSISTENT_DATA)` | C# `entity.Serialize([Persistent])` | 编译期生成，不需要运行时反射 |

---

## 9. 关键设计决策记录

### 9.1 BLOB 存储 vs 列存储

**决策: `sm_identifier` + `sm_data` BLOB。**

BigWorld 将每个持久属性映射为数据库列，需要 20+ PropertyMapping 子类来处理类型转换。
Atlas 中序列化由 C# Source Generator 完成，C++ 侧不接触属性值。

如果按列存储：
- C++ 需要解析 C# blob → 逆向 SpanWriter 的输出 → 按属性分列 → 性能和维护成本高
- 每次 C# 新增属性类型都需要 C++ 同步更新 PropertyMapping

BLOB 方案：
- C++ 只存/取二进制，不解析
- 唯一例外: `[Identifier]` 属性提取为独立列（用于名字查找）
- Identifier 值由 C# 在调用 `WriteToDB` 时通过 NativeApi 单独传递

后续优化: 如果需要 SQL 查询更多属性，可在 `INativeApiProvider` 中新增 API，
让 C# 传递指定属性的键值对供 C++ 存入独立列。但初期不需要。

### 9.2 DBApp 无脚本引擎

**决策: DBApp 继承 `ManagerApp`，不加载 C# 运行时。**

DBApp 只做数据存取，不执行游戏逻辑，不需要脚本引擎。
实体定义从 JSON 文件加载（Source Generator 在编译期生成）。

### 9.3 Checkout 持久化

**决策: 内嵌到实体表（`sm_checkedOut` 列）。**

BigWorld 用独立的 `BigWorldLogOns` 表。Atlas 内嵌到实体表，
减少一次 JOIN，查询和更新在同一行完成。

DBApp 重启时从 `sm_checkedOut=1` 的行恢复 checkout 信息到内存。

### 9.4 Identifier 属性

**需要在 `[Persistent]` 基础上新增 `[Identifier]` Attribute。**

```csharp
[Entity("Account")]
public partial class Account : ServerEntity
{
    [Persistent, Identifier]     // 提取为数据库独立列，可按名查找
    private string _accountName;

    [Persistent]
    private int _level;
}
```

`[Identifier]` 是 `[Persistent]` 的子集，标记该属性为实体的名字标识。
每个实体类型最多一个 `[Identifier]` 属性（BigWorld 相同限制）。

Source Generator 生成的 `EntityTypeRegistry.RegisterAll()` 会在属性描述中标记 identifier。
`EntityDefRegistry` 的 `PropertyDescriptor` 新增 `bool identifier` 字段。

### 9.5 Checkout 操作全部异步

**决策: IDatabase 的 checkout 方法全部异步化。**

BigWorld 的 set_checkout/get_checkout 在某些路径下是同步的（内存缓存命中时），
但 Atlas 的 MySQL 后端在后台线程执行所有 DB 操作，同步接口会阻塞主线程。

解决方案: checkout 合并到 CRUD 流程中：
- `checkout_entity()` = SELECT + UPDATE 在单次 DB 事务中完成（原子操作）
- `put_entity()` + `WriteFlags::LogOff` 自动清除 checkout（同一事务）
- 独立的 `clear_checkouts_for_address()` 用于 BaseApp 死亡批量清理

这与 BigWorld 的做法一致——`putEntity` 中的 `addLogOnRecord` / `removeLogOnRecord`
都在同一个 DB 事务中执行。

### 9.6 密码存储

**决策: 在实体表中增加 `sm_passwordHash` 列。**

初期只有 Account 类型需要密码存储。密码使用 bcrypt 或 argon2 哈希存储，
不存储明文。`LookupResult` 返回密码哈希供 DBApp 在 AuthLogin 中验证。

后续可替换为外部认证服务（OAuth2、LDAP 等），此时 `sm_passwordHash` 不再使用。

### 9.7 CheckoutInfo 包含 AppID

**决策: CheckoutInfo 中增加 `app_id` 字段（BaseAppMgr 分配的 AppID）。**

比地址更稳定——BaseApp 重启后地址变化，但同一逻辑 AppID 通常不复用。
Phase 9 的跨 BaseApp 踢下线可用 AppID 路由 ForceLogoff 消息。

### 9.8 与 C# NativeApi 的交互

C# 侧调用 `WriteToDB` 时需要传递:
1. 持久属性 blob（`SpanWriter` 输出）
2. Identifier 值（字符串，如果有）
3. DBID（0 = 新实体）
4. WriteFlags

需要在 `INativeApiProvider` 中新增:

```cpp
// native_api_provider.hpp 新增
virtual void write_to_db(uint32_t entity_id, int64_t dbid,
                          uint8_t flags,
                          const char* identifier, int32_t id_len,
                          const std::byte* blob, int32_t blob_len) = 0;

virtual void delete_from_db(uint16_t type_id, int64_t dbid) = 0;
```

BaseApp 的 `NativeApiProvider` 实现中将其转为网络消息发送到 DBApp。
