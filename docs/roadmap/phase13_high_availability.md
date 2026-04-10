# Phase 13: 高可用 — Reviver + DBAppMgr

> 前置依赖: Phase 11 (分布式空间完整可用)
> BigWorld 参考: `server/reviver/`, `server/dbappmgr/`

---

## 目标

为 Atlas 集群添加故障恢复和高可用能力，确保单个进程崩溃不会导致整个集群不可用。

## 验收标准

- [ ] Reviver 可检测 Manager 进程崩溃并自动重启
- [ ] Manager 重启后可从备份恢复状态
- [ ] DBAppMgr 支持多 DBApp 实例和故障转移
- [ ] CellApp 崩溃后，其管理的实体可迁移到其他 CellApp
- [ ] BaseApp 崩溃后，客户端可重连到新 BaseApp

---

## 模块清单

### 13.1 Reviver 进程 (`src/server/Reviver/`)

监控 Manager 进程（BaseAppMgr, CellAppMgr, DBAppMgr），崩溃后自动重启。

```cpp
namespace atlas {

class Reviver : public ServerApp {
protected:
    Result<void> on_init() override;
    void on_tick(float dt) override;

private:
    struct MonitoredProcess {
        ProcessType type;
        Address addr;
        uint64_t last_heartbeat;
        int restart_count = 0;
    };

    /// 检测心跳超时
    void check_heartbeats();

    /// 重启进程
    void restart_process(ProcessType type);

    /// 通知集群 Manager 已恢复
    void notify_manager_restored(ProcessType type, const Address& new_addr);

    std::vector<MonitoredProcess> monitored_;
    Duration heartbeat_timeout_ = std::chrono::seconds(5);
    int max_restart_attempts_ = 3;
};

} // namespace atlas
```

**Reviver 策略:**
- 只监控全局唯一的 Manager 进程（BaseAppMgr, CellAppMgr, DBAppMgr）
- 普通进程（BaseApp, CellApp, DBApp）的故障由对应 Manager 处理
- Reviver 自身的高可用: 可部署多个 Reviver，通过选举确定 active

### 13.2 Manager 状态备份与恢复

Manager 进程需要定期备份关键状态，崩溃重启后可恢复。

```cpp
/// Manager 状态快照接口
class IManagerBackup {
public:
    virtual ~IManagerBackup() = default;

    /// 序列化当前状态
    virtual std::vector<std::byte> snapshot() const = 0;

    /// 从快照恢复
    virtual Result<void> restore(std::span<const std::byte> data) = 0;
};
```

**各 Manager 备份内容:**

| Manager | 备份内容 |
|---------|---------|
| BaseAppMgr | BaseApp 列表、负载数据、全局实体 ID 分配器 |
| CellAppMgr | Space 分区信息（BSP 树）、CellApp 列表 |
| DBAppMgr | DBApp 列表、待处理请求队列 |

**备份策略:**
- 定期快照到本地文件（每 N 秒）
- Manager 重启后先尝试读取最新快照
- 快照过期时从集群中各进程重建状态

### 13.3 DBAppMgr (`src/server/DBAppMgr/`)

管理多个 DBApp 实例，提供负载均衡和故障转移。

```cpp
namespace atlas {

class DBAppMgr : public ServerApp {
protected:
    Result<void> on_init() override;
    void on_tick(float dt) override;

private:
    /// 分配数据库请求到 DBApp
    void on_db_request(const Address& src, BinaryReader& reader);

    /// 处理 DBApp 负载上报
    void on_load_report(const Address& src, BinaryReader& reader);

    /// 处理 DBApp 崩溃
    void on_dbapp_dead(const Address& addr);

    /// 选择 DBApp（按实体类型或 hash 分片）
    Address select_dbapp(uint16_t type_id, DatabaseID dbid) const;

    struct DBAppInfo {
        Address addr;
        float load = 0.0f;
        bool alive = true;
    };
    std::vector<DBAppInfo> dbapps_;
};

} // namespace atlas
```

**分片策略:**
- 简单: 按 `dbid % num_dbapps` 分配
- 高级: 按实体类型分片（不同类型路由到不同 DBApp）
- 故障转移: DBApp 挂掉后，其负责的分片迁移到其他 DBApp

### 13.4 CellApp 故障恢复

CellApp 崩溃时，其管理的 Cell 和实体需要恢复。

**恢复策略:**

```
CellApp #2 崩溃
     │
CellAppMgr 检测到
     │
     ├─ 方案 A: 将 Cell 分配给其他 CellApp
     │  - 其他 CellApp 上的 Ghost → 升级为 Real
     │  - 无 Ghost 的实体 → 从 BaseApp 重建
     │
     └─ 方案 B: 重启 CellApp，恢复状态
        - 从 BaseApp 获取实体列表
        - 重新创建所有实体
        - 重建 Ghost 关系
```

### 13.5 BaseApp 故障恢复

BaseApp 崩溃影响其上的所有客户端连接和 Base 实体。

**恢复策略:**
- 客户端检测到断线 → 重新登录（走 LoginApp 流程）
- Base 实体从 DBApp 重新加载（最近一次 writeToDB 的状态）
- Cell 实体保持不变（在 CellApp 上）
- **数据丢失窗口**: 上次 writeToDB 到崩溃之间的状态变更会丢失

**优化: 定期自动 writeToDB**
- 每 N 分钟自动持久化所有 Proxy 实体
- 缩小数据丢失窗口

---

## 新增文件预估

```
src/server/Reviver/
├── reviver.hpp / .cpp
├── reviver_interface.hpp
└── CMakeLists.txt

src/server/DBAppMgr/
├── dbapp_mgr.hpp / .cpp
├── dbapp_mgr_interface.hpp
└── CMakeLists.txt

src/lib/server/     (追加)
├── manager_backup.hpp / .cpp
└── failover_handler.hpp / .cpp
```

---

## BigWorld 对照

| BigWorld | Atlas | 差异 |
|----------|-------|------|
| Reviver | Reviver | 逻辑类似 |
| DBAppMgr (Alpha) | DBAppMgr | BigWorld 的 DBAppMgr 也较简单 |
| CellApp 故障: Ghost 升级 | 同上 | |
| BaseApp 故障: 重新登录 | 同上 | |
| 自动 writeToDB | 自动 writeToDB | |

## 待细化问题

- [ ] Reviver 多实例选举算法
- [ ] Manager 状态备份的存储位置（本地文件 vs 共享存储）
- [ ] CellApp 恢复期间的消息缓冲
- [ ] 客户端重连时的状态恢复（是否需要发送完整实体状态）
- [ ] 数据丢失窗口的可接受范围（影响自动 writeToDB 频率）
- [ ] 是否引入 WAL（Write-Ahead Log）减少数据丢失
