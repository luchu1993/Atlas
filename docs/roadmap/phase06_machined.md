# Phase 6: machined — 进程管理与服务发现

> 前置依赖: Phase 5 (服务器框架基类)
> BigWorld 参考: `server/tools/bwmachined/`, `lib/network/machine_guard.hpp`, `lib/network/machined_utils.hpp`

---

## 目标

实现集群基石服务 `machined`，以及配套的客户端库 `MachinedClient`。
每台物理/虚拟机运行一个 machined 守护进程，提供进程注册、服务发现、
存活检测和事件通知。所有服务器进程在启动时自动向 machined 注册，
并通过它发现集群内其他进程。

## 验收标准

- [x] machined 进程可启动并监听 TCP 端口
- [x] 服务器进程启动时自动注册，关闭时自动注销
- [x] 进程可按类型查询已注册的其他进程地址（含负载信息）
- [x] 进程异常退出后 machined 在 ~15 秒内检测到并广播死亡通知（TCP keepalive + 心跳超时）
- [x] Birth/Death Listener 机制可工作：进程可订阅其他类型进程的上下线事件
- [x] 进程唯一性校验：Manager 类型同类只允许一个，普通进程同类型+同名不允许
- [x] Watcher 查询可通过 machined 转发到目标进程
- [x] machined 优雅关闭时通知所有已注册进程
- [x] 单元测试覆盖注册/注销/查询/超时/监听全部场景
- [x] 集成测试: 单进程内嵌多 EventDispatcher，两个模拟进程通过 machined 互相发现
- [~] `atlas_tool` CLI 工具可列出进程、查询 Watcher

## 验收状态（2026-04-13）

- `machined` 当前实现是 `TCP 注册/查询 + UDP heartbeat` 的混合模型，而不是早期草案中的纯 TCP 或 BigWorld 式广播 UDP。
- 服务端的 Watcher 转发链路已经在 `machined`/`WatcherForwarder`/`MachinedClient` 中落地。
- `atlas_tool list` 已可用，但 `atlas_tool watch` 仍是占位提示，尚未把原始 `WatcherRequest` 能力暴露给 CLI。

---

## 1. BigWorld 架构分析与 Atlas 适配

### 1.1 BigWorld machined 核心机制

| 机制 | BigWorld 实现 | 说明 |
|------|-------------|------|
| **通信方式** | 3 个 UDP endpoint（广播、本地、主端口） | 局域网 UDP 广播，20018 端口 |
| **进程注册** | 本地 UDP (`127.0.0.1:20018`) | 仅接受本机进程注册 |
| **服务发现** | UDP 广播查询 + 收集回复 | ProcessStatsMessage 广播 |
| **死亡检测** | 读 `/proc/[pid]/stat` (Linux)，每 1 秒 | 不依赖心跳，直接检查 OS 进程表 |
| **事件通知** | Birth/Death Listener + 地址注入 | 进程注册监听模板，machined 注入地址后发送 |
| **集群拓扑** | Ring + Buddy，Flood keepalive (2s) | 机器级别的存活检测 |
| **消息协议** | MGMPacket: flags(8) + buddy(32) + messages[] | 15+ 消息类型 |
| **进程启动** | CreateMessage → machined fork/exec | machined 可远程启动进程 |
| **Watcher 转发** | WatcherNub 注册为独立类别 | 通过 machined 路由 watcher 查询 |
| **状态持久化** | `/var/run/bwmachined.state` (SIGTERM 时保存) | 重启后恢复进程列表 |

### 1.2 Atlas 适配决策

| 方面 | Atlas 决策 | 原因 |
|------|-----------|------|
| **通信方式** | TCP（配置文件指定地址） | 适合容器化部署（K8s/Docker），UDP 广播在虚拟网络中受限 |
| **进程注册** | TCP 长连接 | 连接断开 = 进程死亡，天然的死亡检测 |
| **死亡检测** | 激进 TCP keepalive + 强制应用层心跳超时 | 双保险: ~8s TCP 层 + 15s 应用层 |
| **集群拓扑** | 单 machined（初期），后续可扩展多 machined | 简化设计，多数项目单机或少量机器 |
| **进程启动** | 不实现（外部编排：systemd / K8s / 手动） | 现代部署不需要 machined 启动进程 |
| **Watcher 转发** | 保留，machined 路由 watcher 请求 | 运维必需能力 |
| **消息协议** | 复用 Atlas `NetworkMessage` concept | 已有成熟的消息框架，不需要新协议 |

### 1.3 TCP 长连接 vs BigWorld UDP

**BigWorld 用 UDP 的原因:**
- 局域网内广播发现机器，无需配置
- 无连接，machined 重启不影响已注册进程
- 简单、高效（machined 只是消息中转）

**Atlas 用 TCP 的原因:**
- 容器/云环境中 UDP 广播不可靠或被禁用
- TCP 连接断开天然检测进程死亡，比轮询 /proc 更可靠、更跨平台
- Atlas 已有 `TcpChannel` 基础设施（帧格式、背压管理）
- 长连接在负载低时开销极小（machined 连接数 < 100）

**TCP 崩溃检测补强 (双保险):**

纯 TCP 的 keepalive 默认超时可达 2 小时，kill -9 场景下无法及时检测。Atlas 采用双保险:

1. **激进 TCP keepalive:** machined 对所有接入连接配置 socket 选项
   ```cpp
   setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, 1);
   setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, 5);   // 5 秒空闲开始探测
   setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, 1);  // 1 秒探测间隔
   setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, 3);    // 3 次失败判定死亡
   // → ~8 秒检测到网络层断开
   ```

2. **强制应用层心跳超时:** machined 在 `on_tick_complete()` 中检查 `last_heartbeat` 时间戳
   ```cpp
   for (auto& [pid, entry] : entries_) {
       if (now - entry.last_heartbeat > heartbeat_timeout_) {  // 默认 15s
           LOG_WARNING("{} heartbeat timeout", entry.name);
           entry.channel->close();  // 触发 on_channel_disconnect
       }
   }
   ```

> 两层机制覆盖不同场景: TCP keepalive 处理网络层问题，心跳超时处理半开连接和跨平台差异。

**machined 重启场景:**
- TCP 断开后，各进程的 `MachinedClient` 自动重连并重新注册
- 短暂的 machined 不可用窗口内，进程间已有的直连 Channel 不受影响

---

## 2. 消息协议设计

### 2.1 已有消息（`machined_types.hpp`，需更新）

Atlas 已有 4 个 machined 消息类型，需要扩展。

| 现有消息 | ID | 用途 | 变更 |
|---------|-----|------|------|
| `RegisterMessage` | 1000 | 注册进程 | **扩展字段** |
| `DeregisterMessage` | 1001 | 注销进程 | 保持 |
| `QueryMessage` | 1002 | 按类型查询进程 | 保持 |
| `QueryResponse` | 1003 | 查询结果 | **扩展字段** |

### 2.2 需要新增的消息

| 消息 | ID | 方向 | 用途 |
|------|-----|------|------|
| `RegisterAck` | 1004 | machined → 进程 | 注册确认（成功/失败+原因） |
| `HeartbeatMessage` | 1005 | 进程 → machined | 强制心跳（5s 间隔，携带负载数据） |
| `HeartbeatAck` | 1006 | machined → 进程 | 心跳确认 |
| `BirthNotification` | 1010 | machined → 进程 | 新进程上线通知 |
| `DeathNotification` | 1011 | machined → 进程 | 进程下线通知 |
| `ListenerRegister` | 1012 | 进程 → machined | 注册 birth/death 监听 |
| `ListenerAck` | 1013 | machined → 进程 | 监听注册确认 |
| `WatcherRequest` | 1020 | 进程/工具 → machined | 查询指定进程的 watcher 值 |
| `WatcherResponse` | 1021 | machined → 请求方 | watcher 查询结果 |
| `WatcherForward` | 1022 | machined → 目标进程 | 转发 watcher 查询到目标 |
| `WatcherReply` | 1023 | 目标进程 → machined | 目标进程回复 watcher 值 |

### 2.3 消息详细定义

#### RegisterMessage（扩展已有）

```cpp
namespace atlas::machined {

struct RegisterMessage {
    uint8_t protocol_version = kCurrentProtocolVersion; // 协议版本 (兼容演进)
    ProcessType process_type;      // 进程类型枚举
    std::string name;              // 进程实例名 ("baseapp01")
    uint16_t internal_port;        // 内部通信端口
    uint16_t external_port;        // 外部通信端口 (0 = 无)
    uint32_t pid;                  // OS 进程 ID

    static constexpr uint8_t kCurrentProtocolVersion = 1;

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // serialize / deserialize ...
};

// machined 返回
struct RegisterAck {
    bool success;
    std::string error_message;     // 失败原因 (空 = 成功)
    uint64_t server_time;          // machined 当前时间 (用于时间同步)

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // serialize / deserialize ...
};

} // namespace atlas::machined
```

#### BirthNotification / DeathNotification

```cpp
namespace atlas::machined {

struct BirthNotification {
    ProcessType process_type;
    std::string name;
    Address internal_addr;
    Address external_addr;         // {0,0} if N/A
    uint32_t pid;

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

struct DeathNotification {
    ProcessType process_type;
    std::string name;
    Address internal_addr;
    uint8_t reason;                // 0=normal, 1=connection_lost, 2=timeout

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

} // namespace atlas::machined
```

#### ListenerRegister

```cpp
namespace atlas::machined {

enum class ListenerType : uint8_t {
    Birth = 0,      // 订阅上线事件
    Death = 1,      // 订阅下线事件
    Both  = 2,      // 两者都订阅
};

struct ListenerRegister {
    ListenerType listener_type;
    ProcessType target_type;       // 要监听的进程类型 (如 "我要监听 BaseApp 上线")

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

struct ListenerAck {
    bool success;

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

} // namespace atlas::machined
```

> **与 BigWorld 的简化:**
> BigWorld 的 ListenerMessage 使用 preAddr/postAddr 地址注入模板，把回调地址嵌入消息格式。
> Atlas 简化为：machined 直接在已建立的 TCP 连接上发送 `BirthNotification`/`DeathNotification`，
> 不需要地址注入。因为是 TCP 长连接，machined 知道每个进程的连接。

#### WatcherRequest / WatcherResponse

```cpp
namespace atlas::machined {

struct WatcherRequest {
    ProcessType target_type;       // 目标进程类型
    std::string target_name;       // 目标实例名 (空 = 所有该类型)
    std::string watcher_path;      // watcher 路径 (如 "app/uptime_seconds")
    uint32_t request_id;           // 请求 ID (用于匹配回复)

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

struct WatcherResponse {
    uint32_t request_id;
    bool found;
    std::string source_name;       // 回复来源进程名
    std::string value;             // watcher 值 (字符串形式)

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

} // namespace atlas::machined
```

---

## 3. 核心模块设计

### 3.1 ProcessRegistry — 进程注册表

```cpp
// src/server/machined/process_registry.hpp
namespace atlas {

struct ProcessEntry {
    ProcessType type;
    std::string name;
    Address internal_addr;         // machined 从 TCP 连接中推导 IP + 注册消息中的端口
    Address external_addr;
    uint32_t pid;
    TimePoint registered_at;
    TimePoint last_heartbeat;
    float load = 0.0f;            // 最近心跳上报的负载 (0.0~1.0)

    ChannelId channel_id;          // 到该进程的 TCP 连接 ID (非原始指针, 防悬垂)
};

class ProcessRegistry {
public:
    /// 注册进程 (由 machined 在收到 RegisterMessage 时调用)
    /// 校验规则:
    ///   - 同一 PID 不能重复注册
    ///   - 同一端口不能冲突
    ///   - Manager 类型 (*Mgr, Reviver) 同类只允许一个实例
    ///   - 普通进程同类型 + 同名不允许
    auto register_process(const ProcessEntry& entry) -> Result<void>;

    /// 注销进程
    void unregister_process(ChannelId channel_id);
    void unregister_process(uint32_t pid);

    /// 按类型查询
    auto find_by_type(ProcessType type) const -> std::vector<const ProcessEntry*>;

    /// 按 ChannelId 查询 (TCP 断开时用于定位进程)
    auto find_by_channel(ChannelId channel_id) const -> const ProcessEntry*;

    /// 更新进程负载 (心跳时调用)
    void update_load(ChannelId channel_id, float load);

    /// 所有进程
    auto all() const -> std::vector<const ProcessEntry*>;

    /// 统计
    auto count() const -> size_t;
    auto count_by_type(ProcessType type) const -> size_t;

private:
    /// PID → 进程条目
    std::unordered_map<uint32_t, ProcessEntry> entries_;

    /// ChannelId → PID 反向索引 (TCP 断开时快速查找)
    std::unordered_map<ChannelId, uint32_t> channel_index_;

    /// 类型 → PID 索引 (按类型查询加速)
    std::unordered_multimap<ProcessType, uint32_t> type_index_;
};

} // namespace atlas
```

**设计要点:**
- 使用 `ChannelId` (整数 ID) 而非 `Channel*` 原始指针作为索引键，避免 Channel 销毁后悬垂指针
  - `ChannelId` 由 `NetworkInterface` 在创建 Channel 时分配，在 `on_channel_disconnect` 回调中仍有效
  - 确保 `on_channel_disconnect` 在 Channel 销毁之前被调用（参照 BigWorld 的 `ChannelListener` 接口）
- `type_index_` 按类型索引: 加速 `find_by_type()` 查询（避免全表扫描）
- `internal_addr` 的 IP 部分从 TCP 连接的远端地址推导，端口从 RegisterMessage 获取
- **注册校验规则:**
  - 同一 PID 不能重复注册
  - 同一端口不能冲突
  - Manager 类型进程（BaseAppMgr, CellAppMgr, DBAppMgr, Reviver）：同类型只允许一个实例
  - 普通进程（BaseApp, CellApp, DBApp, LoginApp）：同类型 + 同名不允许（返回错误让调用方改名）

### 3.2 ListenerManager — 事件监听管理

```cpp
// src/server/machined/listener_manager.hpp
namespace atlas {

class ListenerManager {
public:
    /// 注册监听器
    void add_listener(ChannelId subscriber, ListenerType type, ProcessType target);

    /// 移除某连接的所有监听
    void remove_all(ChannelId subscriber);

    /// 触发 birth 通知
    void notify_birth(const ProcessEntry& entry);

    /// 触发 death 通知
    void notify_death(const ProcessEntry& entry, uint8_t reason);

private:
    struct Listener {
        ChannelId subscriber;
        ListenerType type;
    };

    /// ProcessType → 监听者列表 (按目标类型分桶, O(1) 查找)
    std::unordered_multimap<ProcessType, Listener> listeners_;

    /// ChannelId → 该连接订阅的所有 target_type (反向索引, 用于 remove_all)
    std::unordered_multimap<ChannelId, ProcessType> channel_subscriptions_;
};

} // namespace atlas
```

**触发流程:**

```
进程注册成功:
  registry_.register_process(entry)
  listener_mgr_.notify_birth(entry)
    → 在 listeners_ 中查找 key == entry.type 的所有监听者
    → 如果 listener.type 包含 Birth
    → 通过 ChannelId 查找 Channel 发送 BirthNotification

TCP 断开 (进程死亡):
  entry = registry_.find_by_channel(channel_id)
  registry_.unregister_process(channel_id)
  listener_mgr_.notify_death(entry, reason)
  listener_mgr_.remove_all(channel_id)     // 清理死进程自己的监听
```

### 3.3 WatcherForwarder — Watcher 查询转发

```cpp
// src/server/machined/watcher_forwarder.hpp
namespace atlas {

class WatcherForwarder {
public:
    explicit WatcherForwarder(ProcessRegistry& registry);

    /// 处理 watcher 查询请求
    /// 找到目标进程，转发请求，收集回复后返回给请求方
    void handle_request(ChannelId requester, const machined::WatcherRequest& req);

    /// 处理目标进程的 watcher 回复
    void handle_reply(ChannelId target, const machined::WatcherReply& reply);

    /// 清理超时的待处理查询 (在 MachinedApp::on_tick_complete 中调用)
    void check_timeouts();

private:
    struct PendingQuery {
        ChannelId requester;
        uint32_t request_id;
        TimePoint deadline;
    };

    ProcessRegistry& registry_;
    std::unordered_map<uint32_t, PendingQuery> pending_;

    static constexpr Duration kQueryTimeout = Seconds(5);
};

} // namespace atlas
```

### 3.4 MachinedApp — machined 进程主类

```cpp
// src/server/machined/machined_app.hpp
namespace atlas {

/// machined 守护进程
/// 继承 ManagerApp (无脚本引擎)
class MachinedApp : public ManagerApp {
public:
    MachinedApp(EventDispatcher& dispatcher, NetworkInterface& network);

protected:
    auto init(int argc, char* argv[]) -> bool override;
    void fini() override;
    void on_tick_complete() override;

    void register_watchers() override;

private:
    // ---- 消息处理器 ----
    void on_register(const Address& src, ChannelId ch,
                     const machined::RegisterMessage& msg);
    void on_deregister(const Address& src, ChannelId ch,
                       const machined::DeregisterMessage& msg);
    void on_query(const Address& src, ChannelId ch,
                  const machined::QueryMessage& msg);
    void on_heartbeat(const Address& src, ChannelId ch,
                      const machined::HeartbeatMessage& msg);
    void on_listener_register(const Address& src, ChannelId ch,
                              const machined::ListenerRegister& msg);
    void on_watcher_request(const Address& src, ChannelId ch,
                            const machined::WatcherRequest& msg);
    void on_watcher_reply(const Address& src, ChannelId ch,
                          const machined::WatcherReply& msg);

    // ---- TCP 连接事件 ----
    void on_channel_disconnect(ChannelId channel_id);

    // ---- 信号处理 ----
    void on_signal(Signal sig) override;

    // ---- 心跳超时检测 (在 on_tick_complete 中调用) ----
    void check_heartbeat_timeouts();

    // ---- 组件 ----
    ProcessRegistry registry_;
    ListenerManager listener_mgr_;
    WatcherForwarder watcher_fwd_{registry_};

    static constexpr Duration kHeartbeatTimeout = Seconds(15);
};

} // namespace atlas
```

**`init()` 流程:**

```
MachinedApp::init():
    ManagerApp::init()
    start TCP server on config().machined_port (default 20018)
    configure aggressive TCP keepalive on accepted connections:
        TCP_KEEPIDLE=5, TCP_KEEPINTVL=1, TCP_KEEPCNT=3
    register message handlers:
        RegisterMessage   → on_register
        DeregisterMessage → on_deregister
        QueryMessage      → on_query
        HeartbeatMessage  → on_heartbeat
        ListenerRegister  → on_listener_register
        WatcherRequest    → on_watcher_request
        WatcherReply      → on_watcher_reply
    set channel disconnect callback → on_channel_disconnect
    LOG_INFO("machined started on port {}", port)
```

**`on_tick_complete()` 流程:**

```
MachinedApp::on_tick_complete():
    check_heartbeat_timeouts()           // 检查心跳超时 (15s 无心跳 → 关闭连接)
    watcher_fwd_.check_timeouts()        // 清理超时的 watcher 查询
```

**`on_signal()` — 优雅关闭:**

```
MachinedApp::on_signal(sig):
    if (sig == SIGTERM || sig == SIGINT):
        // 通知所有已注册进程 machined 即将关闭
        for entry in registry_.all():
            send ShutdownNotification to entry.channel_id
        // 短暂等待 (2s) 让进程处理通知
        dispatcher_.add_timer(2s, [this]{ shutdown(); })
```

**TCP 连接断开 = 进程死亡检测:**

```
on_channel_disconnect(channel):
    entry = registry_.find_by_channel(&channel)
    if (entry == nullptr) return          // 未注册连接 (查询客户端等)

    LOG_WARNING("{} {} (pid={}) disconnected",
                process_type_name(entry->type), entry->name, entry->pid)

    // 保存副本用于通知
    auto dead_entry = *entry;

    registry_.unregister_process(&channel)
    listener_mgr_.notify_death(dead_entry, /*reason=*/1)  // connection_lost
    listener_mgr_.remove_all(&channel)
```

---

## 4. MachinedClient — 客户端库 (`src/lib/server/`)

### 4.1 设计

每个 `ServerApp` 内置一个 `MachinedClient`，自动处理注册/注销/查询/监听。

```cpp
// src/lib/server/machined_client.hpp
namespace atlas {

class MachinedClient {
public:
    explicit MachinedClient(NetworkInterface& network);
    ~MachinedClient();

    // ========== 连接与注册 ==========

    /// 连接到 machined 并注册自身
    /// 在 ServerApp::init() 中调用
    [[nodiscard]] auto connect_and_register(const Address& machined_addr,
                                             const ServerConfig& config) -> Result<void>;

    /// 注销并断开 (在 ServerApp::fini() 中调用)
    void deregister_and_disconnect();

    /// 是否已连接
    [[nodiscard]] auto is_connected() const -> bool;

    // ========== 服务发现 (同步 API, 仅在 init() 阶段使用) ==========

    /// 查询指定类型的所有进程
    /// **仅在 ServerApp::init() 阶段安全调用** (事件循环未启动)
    /// 内部通过 dispatcher.process_once() 驱动网络直到收到回复
    [[nodiscard]] auto query(ProcessType type) -> Result<std::vector<machined::ProcessInfo>>;

    /// 查询指定类型的单个进程 (Manager 进程)
    /// 支持重试 (Manager 可能还没启动)
    /// **仅在 ServerApp::init() 阶段安全调用**
    [[nodiscard]] auto find_one(ProcessType type, int retries = 5,
                                 Duration retry_interval = Seconds(1))
        -> Result<machined::ProcessInfo>;

    // ========== 服务发现 (异步 API, 运行时使用) ==========

    /// 异步查询 (事件循环运行中安全调用)
    using QueryCallback = std::function<void(Result<std::vector<machined::ProcessInfo>>)>;
    void query_async(ProcessType type, QueryCallback callback);

    // ========== 事件监听 ==========

    using BirthCallback = std::function<void(const machined::BirthNotification&)>;
    using DeathCallback = std::function<void(const machined::DeathNotification&)>;

    /// 监听指定类型进程的上线事件
    void listen_for_birth(ProcessType target, BirthCallback callback);

    /// 监听指定类型进程的下线事件
    void listen_for_death(ProcessType target, DeathCallback callback);

    // ========== Watcher ==========

    /// 查询远程进程的 watcher 值
    [[nodiscard]] auto query_watcher(ProcessType target, const std::string& name,
                                      const std::string& path)
        -> Result<std::string>;

private:
    void on_birth_notification(const Address& src, Channel* ch,
                               const machined::BirthNotification& msg);
    void on_death_notification(const Address& src, Channel* ch,
                               const machined::DeathNotification& msg);
    void on_watcher_response(const Address& src, Channel* ch,
                             const machined::WatcherResponse& msg);

    /// 心跳定时器 (5s 间隔发送 HeartbeatMessage)
    void send_heartbeat();

    /// 断线重连
    void on_disconnect(Channel& channel);
    void attempt_reconnect();

    NetworkInterface& network_;
    Channel* channel_ = nullptr;         // 到 machined 的 TCP 连接
    Address machined_addr_;
    ServerConfig config_;                // 缓存, 重连时需要
    bool registered_ = false;

    // 事件回调
    std::vector<std::pair<ProcessType, BirthCallback>> birth_listeners_;
    std::vector<std::pair<ProcessType, DeathCallback>> death_listeners_;

    // 心跳
    TimerHandle heartbeat_timer_;
    static constexpr Duration kHeartbeatInterval = Seconds(5);

    // 重连
    TimerHandle reconnect_timer_;
    int reconnect_attempts_ = 0;
    static constexpr int kMaxReconnectAttempts = 30;
    static constexpr Duration kReconnectInterval = Seconds(2);
};

} // namespace atlas
```

### 4.2 ServerApp 集成

在 Phase 5 的 `ServerApp` 中添加 machined 集成:

```cpp
// server_app.hpp 中新增:
class ServerApp {
    // ...
    [[nodiscard]] auto machined_client() -> MachinedClient& { return machined_client_; }

private:
    MachinedClient machined_client_;
};

// server_app.cpp init() 中:
// 注意: 此时事件循环尚未启动, MachinedClient 内部用
// dispatcher.process_once() 驱动网络, 同步等待回复是安全的
bool ServerApp::init(int argc, char* argv[]) {
    // ... 已有初始化 ...

    // 连接 machined 并注册
    auto result = machined_client_.connect_and_register(
        config_.machined_address, config_);
    if (!result) {
        ATLAS_LOG_ERROR("Failed to register with machined: {}", result.error());
        return false;
    }

    return true;
}

// server_app.cpp fini() 中:
void ServerApp::fini() {
    machined_client_.deregister_and_disconnect();
}
```

### 4.3 断线重连

machined 可能临时不可用（重启/故障）。`MachinedClient` 需要自动重连。

```
MachinedClient::on_disconnect():
    registered_ = false
    LOG_WARNING("Lost connection to machined, will retry...")
    reconnect_timer_ = dispatcher.add_repeating_timer(
        kReconnectInterval, [this]{ attempt_reconnect(); })

MachinedClient::attempt_reconnect():
    if (++reconnect_attempts_ > kMaxReconnectAttempts)
        LOG_ERROR("Failed to reconnect to machined after {} attempts", ...)
        return

    auto result = network_.connect_tcp(machined_addr_)
    if (result.has_value())
        channel_ = result.value()
        // 重新注册
        send RegisterMessage
        // 重新注册所有监听
        for each (type, callback) in birth_listeners_:
            send ListenerRegister(Birth, type)
        for each (type, callback) in death_listeners_:
            send ListenerRegister(Death, type)
        // 重启心跳定时器
        heartbeat_timer_ = dispatcher.add_repeating_timer(
            kHeartbeatInterval, [this]{ send_heartbeat(); })
        cancel reconnect_timer_
        reconnect_attempts_ = 0
        LOG_INFO("Reconnected to machined")
```

---

## 5. 进程发现与 Birth/Death 典型流程

### 5.1 BaseApp 启动流程

```
BaseApp                    machined                   BaseAppMgr
  │                           │                           │
  │── TCP connect ───────────→│                           │
  │── Register(BaseApp,       │                           │
  │     "baseapp01",          │                           │
  │     port=20100) ─────────→│                           │
  │                           │ 记录到 registry           │
  │←── RegisterAck(ok) ──────│                           │
  │                           │                           │
  │                           │── BirthNotification ────→│
  │                           │   (type=BaseApp,          │
  │                           │    name="baseapp01",      │
  │                           │    addr=...)              │
  │                           │                           │
  │                           │                           │ BaseAppMgr 现在知道新 BaseApp
  │                           │                           │ 直连 BaseApp 发送 init data
  │←── (BaseAppMgr 直连) ────────────────────────────────│
```

### 5.2 BaseApp 异常退出

```
BaseApp                    machined                   BaseAppMgr
  │                           │                           │
  │ (crash / kill -9)         │                           │
  ╳ TCP 连接断开              │                           │
                              │ on_channel_disconnect     │
                              │ registry_.unregister      │
                              │                           │
                              │── DeathNotification ────→│
                              │   (type=BaseApp,          │
                              │    name="baseapp01",      │
                              │    reason=connection_lost) │
                              │                           │
                              │                           │ BaseAppMgr 处理:
                              │                           │  - 通知 CellAppMgr
                              │                           │  - 恢复实体
```

### 5.3 Manager 查询（带重试）

```
BaseApp                    machined
  │                           │
  │ 需要找到 BaseAppMgr       │
  │                           │
  │── Query(BaseAppMgr) ────→│
  │                           │ registry.find_by_type(BaseAppMgr)
  │←── QueryResponse([]) ───│ (空: BaseAppMgr 还没启动)
  │                           │
  │ (等待 1s, 重试)           │
  │── Query(BaseAppMgr) ────→│
  │←── QueryResponse([mgr]) ─│ (找到了!)
  │                           │
  │ 直连 BaseAppMgr           │
```

---

## 6. Watcher 查询转发流程

```
运维工具              machined              BaseApp (目标)
  │                      │                      │
  │── TCP connect ──────→│                      │
  │── WatcherRequest ───→│                      │
  │   (target=BaseApp,   │                      │
  │    name="baseapp01", │                      │
  │    path="app/uptime")│                      │
  │                      │── WatcherForward ───→│
  │                      │   (request_id,       │
  │                      │    path="app/uptime")│
  │                      │                      │
  │                      │   WatcherRegistry    │
  │                      │   .get("app/uptime") │
  │                      │   → "3600"           │
  │                      │                      │
  │                      │←── WatcherReply ────│
  │                      │   (request_id,       │
  │                      │    value="3600")     │
  │                      │                      │
  │←── WatcherResponse ─│                      │
  │   (value="3600")     │                      │
```

运维工具不需要知道 BaseApp 的地址，只需连接 machined。

---

## 7. 实现步骤

### Step 6.1: 扩展消息定义 (`machined_types.hpp`)

**更新文件:** `src/lib/network/machined_types.hpp`

在已有 RegisterMessage / DeregisterMessage / QueryMessage / QueryResponse 基础上:

1. 扩展 `RegisterMessage`: 增加 `ProcessType process_type`, `external_port` 字段
2. 新增 `RegisterAck`
3. 新增 `HeartbeatMessage` / `HeartbeatAck`
4. 新增 `BirthNotification` / `DeathNotification`
5. 新增 `ListenerRegister` / `ListenerAck`
6. 新增 `WatcherRequest` / `WatcherResponse` / `WatcherForward` / `WatcherReply`
7. 全部 `static_assert(NetworkMessage<T>)` 验证

**新增文件:** `tests/unit/test_machined_messages.cpp`
- 所有消息的 serialize → deserialize 往返测试
- 边界条件（空字符串、最大长度等）

### Step 6.2: ProcessRegistry

**新增文件:**
```
src/server/machined/process_registry.hpp / .cpp
tests/unit/test_process_registry.cpp
```

**测试用例:**
- 注册/注销基本流程
- 重复注册（同 PID）→ 拒绝
- 端口冲突检测
- Manager 类型唯一性：注册第二个 BaseAppMgr → 拒绝
- 普通进程同名拒绝：两个 name="baseapp01" 的 BaseApp → 拒绝
- 按类型查询（含负载信息）
- ChannelId 反向索引
- `update_load()` 正确更新
- 并发注册/注销安全性（单线程，但注销可能在遍历中发生）

### Step 6.3: ListenerManager

**新增文件:**
```
src/server/machined/listener_manager.hpp / .cpp
tests/unit/test_listener_manager.cpp
```

**测试用例:**
- 注册监听 → 触发 birth → 回调收到
- 注册监听 → 触发 death → 回调收到
- 多个监听者同时收到通知
- 进程断开时清理其监听器
- 只通知匹配 target_type 的监听者

### Step 6.4: WatcherForwarder

**新增文件:**
```
src/server/machined/watcher_forwarder.hpp / .cpp
tests/unit/test_watcher_forwarder.cpp
```

**测试用例:**
- 查询转发到正确的目标进程
- 目标进程回复后正确路由给请求者
- 超时处理（目标进程未回复）
- 目标进程不存在时直接返回 not_found

### Step 6.5: MachinedApp — machined 进程

**新增文件:**
```
src/server/machined/machined_app.hpp / .cpp
src/server/machined/main.cpp
src/server/machined/CMakeLists.txt
```

**实现顺序:**
1. 基本 TCP 服务器启动
2. RegisterMessage / DeregisterMessage 处理
3. QueryMessage 处理
4. TCP 断开 → 死亡检测
5. ListenerRegister 处理 + Birth/Death 通知
6. Watcher 转发
7. Watcher 注册（machined 自身的运行指标）

**machined 自身的 Watcher:**
```
machined/process_count         — 注册进程总数
machined/process_count/baseapp — BaseApp 数量
machined/process_count/cellapp — CellApp 数量
machined/uptime_seconds        — machined 运行时长
machined/connections           — 当前 TCP 连接数
```

### Step 6.6: MachinedClient — 客户端库

**新增文件:**
```
src/lib/server/machined_client.hpp / .cpp
tests/unit/test_machined_client.cpp
```

**实现顺序:**
1. `connect_and_register()` — TCP 连接 + 发送 RegisterMessage + 等待 RegisterAck
2. `deregister_and_disconnect()` — 发送 DeregisterMessage + 断开
3. `query()` — 发送 QueryMessage + 等待 QueryResponse
4. `find_one()` — query() + 重试逻辑
5. `listen_for_birth()` / `listen_for_death()` — 发送 ListenerRegister + 注册本地回调
6. Birth/Death 通知接收与回调分发
7. 断线重连逻辑
8. `query_watcher()` — 发送 WatcherRequest + 等待 WatcherResponse

**测试用例（需要 machined 实例）:**
- 使用测试用的内嵌 MachinedApp（同进程），或 mock
- 注册 → 查询 → 找到自己
- 注册 → 另一进程查询 → 找到
- 断开 → 重连 → 重新注册
- 监听器收到 birth/death 通知

### Step 6.7: ServerApp 集成

**更新文件:**
```
src/lib/server/server_app.hpp  (添加 MachinedClient 成员)
src/lib/server/server_app.cpp  (init/fini 中调用)
```

在 `ServerApp::init()` 中:
```cpp
auto result = machined_client_.connect_and_register(
    config_.machined_address, config_);
if (!result) {
    ATLAS_LOG_WARNING("Could not connect to machined: {}", result.error());
    // 非致命: 进程可独立运行 (单机开发模式)
}
```

> **设计决策: machined 连接失败不阻止进程启动。**
> 开发/测试时可能不运行 machined，进程应能独立工作。
> 仅在集群模式下 machined 是必需的。

### Step 6.8: 集成测试

**新增文件:**
```
tests/integration/test_machined_integration.cpp
```

**测试方案: 单进程内嵌多 EventDispatcher**

避免多进程协调和端口冲突，在单个测试进程中创建 3 组 `EventDispatcher` + `NetworkInterface`:

```cpp
// 创建 machined
EventDispatcher md_disp("machined");
NetworkInterface md_net(md_disp);
MachinedApp machined(md_disp, md_net);
machined.init(/* ... */);

// 创建模拟进程 A
EventDispatcher a_disp("app_a");
NetworkInterface a_net(a_disp);
// ... MachinedClient 连接 machined ...

// 手动驱动事件循环推进:
md_disp.process_once();
a_disp.process_once();
```

**端到端场景:**
1. 启动 machined
2. 启动两个模拟进程 (使用 MachinedClient)
3. 进程 A 注册 → machined 记录
4. 进程 B 注册 → machined 记录 → 进程 A 收到 birth 通知
5. 进程 B 查询 → 找到进程 A（含负载信息）
6. 进程 A 断开 → 进程 B 收到 death 通知
7. 心跳超时测试: 进程 A 停止心跳 → 15s 后 machined 判定死亡
8. 全部通过

### Step 6.9: atlas_tool CLI 运维工具

**新增文件:**
```
src/tools/atlas_tool/main.cpp
src/tools/atlas_tool/CMakeLists.txt
```

简单的 TCP 客户端，连接 machined 执行运维操作:

```bash
# 列出所有注册进程
atlas_tool list
# 输出: TYPE         NAME          ADDR              LOAD   PID
#        BaseApp     baseapp01     192.168.1.10:20100 0.35  12345
#        CellApp     cellapp01     192.168.1.10:20200 0.12  12346

# 查询指定进程的 Watcher 值
atlas_tool watch baseapp01 app/uptime_seconds
# 输出: 3600

# 请求进程关闭
atlas_tool shutdown baseapp01
```

实现成本低（复用 `MachinedClient` 的 query/watcher API），但对开发和运维非常有价值。

---

## 8. 文件清单汇总

```
src/lib/network/
├── machined_types.hpp           (更新: 扩展消息 + 协议版本)

src/lib/server/
├── machined_client.hpp / .cpp   (Step 6.6)
├── server_app.hpp / .cpp        (Step 6.7: 添加 MachinedClient)

src/server/machined/
├── CMakeLists.txt
├── main.cpp
├── machined_app.hpp / .cpp      (Step 6.5)
├── process_registry.hpp / .cpp  (Step 6.2)
├── listener_manager.hpp / .cpp  (Step 6.3)
└── watcher_forwarder.hpp / .cpp (Step 6.4)

src/tools/atlas_tool/
├── CMakeLists.txt
└── main.cpp                     (Step 6.9)

tests/unit/
├── test_machined_messages.cpp   (Step 6.1)
├── test_process_registry.cpp    (Step 6.2)
├── test_listener_manager.cpp    (Step 6.3)
├── test_watcher_forwarder.cpp   (Step 6.4)
└── test_machined_client.cpp     (Step 6.6)

tests/integration/
└── test_machined_integration.cpp (Step 6.8)
```

---

## 9. 依赖关系与执行顺序

```
Step 6.1: 消息定义扩展          ← 无依赖, 最先开始
    │
    ├── Step 6.2: ProcessRegistry     ← 仅依赖消息定义
    │       │
    │       ├── Step 6.3: ListenerManager ← 依赖 ProcessRegistry
    │       │
    │       └── Step 6.4: WatcherForwarder ← 依赖 ProcessRegistry
    │               │
    │               ▼
    │       Step 6.5: MachinedApp       ← 依赖 6.2 + 6.3 + 6.4
    │
    └── Step 6.6: MachinedClient        ← 依赖消息定义, 可与 6.2-6.4 并行
            │
            ▼
        Step 6.7: ServerApp 集成       ← 依赖 6.6
            │
            ├── Step 6.8: 集成测试     ← 依赖 6.5 + 6.7
            │
            └── Step 6.9: atlas_tool   ← 依赖 6.6, 可与 6.8 并行
```

**推荐执行顺序:**

```
第 1 轮:           6.1 消息定义
第 2 轮 (并行):    6.2 ProcessRegistry + 6.6 MachinedClient
第 3 轮 (并行):    6.3 ListenerManager + 6.4 WatcherForwarder
第 4 轮:           6.5 MachinedApp
第 5 轮:           6.7 ServerApp 集成
第 6 轮 (并行):    6.8 集成测试 + 6.9 atlas_tool
```

---

## 10. BigWorld 完整对照

| BigWorld | Atlas | 差异说明 |
|----------|-------|---------|
| 3 个 UDP endpoint | 1 个 TCP server | Atlas 用 TCP 长连接简化设计 |
| UDP 广播发现 machined | 配置文件指定地址 | 适合容器化部署 |
| 轮询 `/proc/[pid]/stat` 检测死亡 (1s) | 激进 TCP keepalive (~8s) + 心跳超时 (15s) | 双保险, 跨平台 |
| Ring + Buddy 集群拓扑 | 单 machined（初期） | 后续可选多 machined |
| ListenerMessage 地址注入 | TCP 直接发送 Notification | 简化, 无需注入模板 |
| ProcessMessage (Register/Deregister/Birth/Death) | 独立消息类型 + 协议版本号 | 更清晰, 可演进 |
| ProcessStatsMessage (CPU/内存) | HeartbeatMessage (负载 + 统计) | 查询时返回缓存负载 |
| WatcherNub 独立类别注册 | Watcher 查询经 machined 转发 | 相同能力, 不同实现 |
| MGMPacket 二进制协议 | 复用 `NetworkMessage` concept | 统一消息框架 |
| CreateMessage (远程启动进程) | 不实现 | 进程由外部编排 |
| 状态持久化 `/var/run/bwmachined.state` | 不实现（初期） | 重启后进程自动重新注册 |
| Staggered replies 防广播风暴 | 不需要 | TCP 无广播风暴 |
| `MachineDaemon::findInterface()` 广播查询 | `MachinedClient::find_one()` 直连查询 | TCP 直连, 确定性 |
| `MachineDaemon::registerBirthListener()` | `MachinedClient::listen_for_birth()` | 接口类似, 传输不同 |
| `cluster_tool` 运维工具 | `atlas_tool` CLI | 当前已支持列出进程，Watcher/关闭能力待补 |
| PID+category+name 唯一性校验 | Manager 单实例 + 同类型同名拒绝 | 更严格的校验规则 |
| Channel* 原始指针 | ChannelId (整数 ID) | 避免悬垂指针 |

---

## 11. 关键设计决策记录

### 11.1 TCP vs UDP

**决策: TCP。**

BigWorld 的 UDP 广播适合 2000 年代的物理机部署。现代环境中:
- K8s pod 之间 UDP 广播不可用
- Docker bridge 网络 UDP 广播受限
- 云 VPC 需要特殊配置才能广播
- TCP 连接状态天然提供心跳（TCP keepalive + 断开检测）

成本: machined 维护 O(N) 条 TCP 连接，但游戏集群进程数通常 < 100，完全可接受。

### 11.2 machined 连接失败策略

**决策: 非致命，降级运行。**

单机开发模式下不强制运行 machined。进程可独立启动，只是无法使用服务发现。
集群部署时，编排系统保证 machined 先于其他进程启动。

### 11.3 ProcessType 与 Phase 5 统一

Phase 5 中已定义 `ProcessType` 枚举（在 `server_config.hpp` 中）。
machined 消息中复用同一枚举。`machined_types.hpp` 中的 `ProcessInfo::type` 字段
从 `std::string` 改为 `ProcessType`。

### 11.4 崩溃检测: 双保险策略

**问题:** TCP keepalive 默认超时可达 2 小时。kill -9 场景下 machined 无法及时发现进程死亡。

**决策: 激进 TCP keepalive + 强制应用层心跳超时。**

| 机制 | 配置 | 检测延迟 | 覆盖场景 |
|------|------|----------|----------|
| TCP keepalive | IDLE=5s, INTVL=1s, CNT=3 | ~8s | 网络断开、进程崩溃 |
| 应用层心跳 | 5s 间隔, 15s 超时 | ~15s | 半开连接、跨平台差异 |

- HeartbeatMessage 每 5 秒发送一次，携带负载数据（CPU、实体数等）
- machined 在 `on_tick_complete()` 中检查 `last_heartbeat` 时间戳，超过 15 秒判定死亡
- machined 缓存最新心跳中的负载数据，供 `QueryResponse` 返回

### 11.5 与 C# 脚本层的关系

**machined 与 C# 无直接关系。** machined 是纯 C++ 进程（`ManagerApp` 基类，无脚本引擎）。

但间接影响:
- `ScriptApp`（BaseApp/CellApp 等）在 `init()` 中先注册 machined，再初始化 ClrHost
- C# 侧可通过 `INativeApiProvider` 查询进程信息（`get_process_prefix()` 已有）
- 后续可按需在 `INativeApiProvider` 中新增 `query_processes()` 暴露给 C#

### 11.6 多 machined 扩展（保留设计空间）

初期只需单 machined。但 API 设计应兼容未来多 machined:
- `ServerConfig::machined_address` 可扩展为地址列表
- `MachinedClient` 内部可实现 failover（连接第一个失败则尝试下一个）
- machined 之间同步通过专用消息（类似 BigWorld 的 `MachinedAnnounceMessage`，不在本阶段实现）

**单点故障降级策略:**
machined 崩溃时，已建立的进程间直连 Channel 不受影响。受影响的是:
- 新进程无法注册/发现
- Birth/Death 通知不可用
`MachinedClient` 自动重连（最多 30 次，间隔 2s），machined 恢复后自动恢复功能。

### 11.7 协议版本号

`RegisterMessage` 包含 `protocol_version` 字段（当前版本 1）。machined 收到旧版本消息时做兼容处理（忽略新增字段的缺失，使用默认值）。后续扩展消息字段时只需递增版本号，保证前后兼容。

BigWorld 经历了 50 个协议版本的演进，版本号从第一天引入可以避免后续痛苦的不兼容升级。

### 11.8 ChannelId vs Channel* 指针

使用 `ChannelId`（整数 ID）代替 `Channel*` 原始指针作为进程索引键。原因:
- `Channel*` 在 `NetworkInterface` 销毁连接后变成悬垂指针
- `ChannelId` 由 `NetworkInterface` 分配，生命周期独立于 Channel 对象
- `on_channel_disconnect(ChannelId)` 回调在 Channel 销毁前触发，保证 registry 能安全清理
- 需要在 Atlas 的 `NetworkInterface` 中添加 `ChannelId` 分配机制（如果尚未存在）

### 11.9 同步查询 API 的安全性

`MachinedClient::query()` 和 `find_one()` 是阻塞式 API，内部通过 `dispatcher.process_once()` 循环驱动网络。

**仅在 `ServerApp::init()` 阶段安全调用**——此时事件循环尚未启动，不会与 tick 处理冲突。

运行时（`run()` 之后）应使用 `query_async()` 异步 API。BigWorld 的 `machined_utils` 也采用同样的模式（init 阶段同步查询）。

### 11.10 进程唯一性校验

注册时校验规则:
- **Manager 类型**（BaseAppMgr, CellAppMgr, DBAppMgr, Reviver）：同类型只允许一个实例。重复注册返回错误。
- **普通进程**（BaseApp, CellApp, DBApp, LoginApp）：同类型 + 同名不允许。不同名字可注册多个实例。
- **machined 本身**不注册到自己的 registry。

### 11.11 machined 优雅关闭

machined 收到 SIGTERM/SIGINT 时:
1. 向所有已注册进程发送 `ShutdownNotification`（复用 Phase 5 的 `ShutdownRequest`）
2. 等待 2 秒让进程处理通知
3. 关闭

进程收到通知后可自行保存状态并断开。machined 不等待所有进程完成——它只是"尽力通知"。
