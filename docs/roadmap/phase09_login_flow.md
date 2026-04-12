# Phase 9: LoginApp + BaseAppMgr — 登录流程

> 前置依赖: Phase 8 (BaseApp), Phase 7 (DBApp), Phase 6 (machined)
> BigWorld 参考: `server/loginapp/`, `server/baseappmgr/`, `lib/connection/login_interface.hpp`

---

## 目标

实现完整的客户端登录流程和 BaseApp 集群管理。LoginApp 作为客户端登录入口，
BaseAppMgr 作为 BaseApp 集群的全局管理者，二者协同完成从「客户端发起连接」
到「进入游戏世界」的端到端流程。

## 验收标准

- [ ] LoginApp 可接受客户端 RUDP 连接，处理登录请求
- [ ] 登录流程完整: 认证 → 分配 BaseApp → Checkout 实体 → 创建 Proxy → 返回 SessionKey
- [ ] BaseAppMgr 管理所有 BaseApp 的注册、负载上报和 Entity ID 分配
- [ ] BaseAppMgr 选择最低负载 BaseApp 分配新登录
- [ ] SessionKey 机制防止未授权直连 BaseApp
- [ ] 登录速率限制（per-IP + 全局）防暴力攻击
- [ ] 重复登录处理（已在线账号踢下线再登录）
- [ ] 登录过载保护（BaseApp 超载时拒绝新登录）
- [ ] 全局实体 (Global Bases) 注册/查找可工作
- [ ] 压力测试: 100 并发登录请求无异常
- [ ] 全部新增代码有单元测试

## 当前实现状态（2026-04-12）

- 压测执行方式、参数说明、产物结构和结果判读，见 [../LOGIN_STRESS_TESTING.md](../LOGIN_STRESS_TESTING.md)
- 登录链路在极端短线重登压测下的已落地优化、当前残余问题和后续排查方向，见 [../LOGIN_STRESS_REMAINING_ISSUES_20260412.md](../LOGIN_STRESS_REMAINING_ISSUES_20260412.md)
- 客户端登录中途断开后的 `prepare/checkout` 回滚协议设计，见 [../LOGIN_ROLLBACK_PROTOCOL_20260412.md](../LOGIN_ROLLBACK_PROTOCOL_20260412.md)
- 该记录重点覆盖 `force-logoff -> relogin -> checkout -> authenticate` 链路，而不是本设计文档中的功能分解本身
- 传输层现状修正: 当前代码里的客户端登录入口已是**外部 RUDP**，不是本文早期草案中的 TCP
- 多 `LoginApp` 的详细工业级设计见 [../MULTI_LOGINAPP_DESIGN.md](../MULTI_LOGINAPP_DESIGN.md)；
  本文中“单 LoginApp + 外部 LB 足够”的表述仅代表 Phase 9 的历史基线，不再是最终详细方案

---

## 1. BigWorld 架构分析与 Atlas 适配

### 1.1 BigWorld LoginApp 核心机制

| 机制 | BigWorld 实现 | 说明 |
|------|-------------|------|
| **登录协议** | UDP probe + UDP login + RSA 加密凭证 | 客户端先 probe 发现，再发加密登录 |
| **认证** | LoginApp → DBApp `logOn` → BillingSystem | 可插拔认证后端 |
| **BaseApp 分配** | DBApp → BaseAppMgr `createEntity` | DBApp 中转，非 LoginApp 直接分配 |
| **Session Key** | uint32 (时间戳) | Proxy 创建时生成 |
| **回复路径** | DBApp → LoginApp → Client (UDP) | 经 DBApp 中转回复 |
| **速率限制** | per-IP + 全局计数 + 失败回复限速 | 多层防护 |
| **IP 封禁** | `IPAddressBanMap` + 自动过期 | BillingSystem 可触发 |
| **Challenge** | 可选，客户端先做 PoW | 防自动化攻击 |
| **缓存** | 相同凭证短时间内重发缓存结果 | 防重复处理 |
| **多 LoginApp** | 客户端 probe 发现，选最少用户的 | DNS 或 probe 负载均衡 |

### 1.2 BigWorld BaseAppMgr 核心机制

| 机制 | BigWorld 实现 | 说明 |
|------|-------------|------|
| **BaseApp 注册** | `add()` 分配 ID + 发送 InitData | 28-bit AppID |
| **负载均衡** | `findLeastLoadedApp()` 线性搜索 | 排除 retiring 的 |
| **Entity ID** | 每个 BaseApp 自行分配 | AppID 编码在高位 |
| **过载保护** | `calculateOverloaded()` 时间+计数 | 容忍期后拒绝 |
| **Global Bases** | `globalBases_` map + 广播同步 | 跨 BaseApp 全局实体注册 |
| **死亡处理** | 清理 + 通知 CellAppMgr + 重定向 mailbox | 通过 BackupHash 恢复 |
| **启动同步** | 等 CellAppMgr + DBApp 就绪才允许 add | 三重条件检查 |
| **Shared Data** | 两类共享数据 + 广播同步 | BaseApp 间 + 全局 |
| **关闭协调** | 5 阶段关闭流程 | REQUEST → INFORM → PERFORM → DISCONNECT → TRIGGER |

### 1.3 Atlas 适配决策

| 方面 | Atlas 决策 | 原因 |
|------|-----------|------|
| **登录协议** | RUDP 连接 LoginApp → 发送登录消息 | 与当前实现一致，保留可靠消息语义 |
| **认证路径** | LoginApp → DBApp 验证 | 简化：DBApp 承担认证（初期） |
| **BaseApp 分配** | LoginApp → BaseAppMgr → BaseApp | 比 BigWorld 的 DBApp 中转更直接 |
| **Session Key** | 32 字节随机令牌（与 Phase 8 一致） | 比 BigWorld uint32 更安全 |
| **速率限制** | per-IP + 全局计数 | 保留核心防护 |
| **Challenge** | 初期不实现 | 后续按需 |
| **多 LoginApp** | 当前实现基线为单实例；详细扩展见 `MULTI_LOGINAPP_DESIGN.md` | 先完成单实例链路，再引入内部控制面 |
| **Entity ID 分配** | BaseAppMgr 分配区间 | 类似 BigWorld 但更显式 |
| **Global Bases** | 保留，BaseAppMgr 管理 | 全局单例实体必需 |
| **启动同步** | machined birth 事件驱动 | 比 BigWorld 的轮询更优雅 |
| **关闭协调** | 初期简单关闭 | 后续优化 |

### 1.4 C# 脚本层的影响

**认证回调:**
- BigWorld: DBApp 调用 Python BillingSystem 认证
- Atlas: DBApp 查 `[Identifier]` 列 + 密码哈希列，初期不需要脚本

**实体创建:**
- BigWorld: BaseAppMgr → BaseApp `createBaseWithCellData` → Python `__init__`
- Atlas: BaseAppMgr → BaseApp → C# `EntityFactory.Create()` → `OnInit()`

**登录回调:**
- BigWorld: Proxy `onLoggedOn()` Python 回调
- Atlas: Proxy `OnLoggedIn()` C# 回调

---

## 2. 登录流程（端到端）

```
Client           LoginApp          DBApp         BaseAppMgr        BaseApp
  │                 │                │                │                │
  │─[1] RUDP连接 ─→│                │                │                │
  │─[2] Login ─────→│                │                │                │
  │  (user, pass_h)  │                │                │                │
  │                 │                │                │                │
  │                 │─[3] AuthLogin─→│                │                │
  │                 │ (user, pass_h) │                │                │
  │                 │                │ lookup_by_name │                │
  │                 │                │ 验证密码哈希    │                │
  │                 │←[4] AuthResult│                │                │
  │                 │ (ok, dbid,     │                │                │
  │                 │  type_id)      │                │                │
  │                 │                                 │                │
  │                 │─[5] AllocateBaseApp ───────────→│                │
  │                 │ (type_id, dbid)                  │                │
  │                 │                                 │ findLeastLoaded│
  │                 │←[6] AllocateResult ────────────│                │
  │                 │ (baseapp_addr)                   │                │
  │                 │                                                  │
  │                 │─[7] PrepareLogin ───────────────────────────────→│
  │                 │ (type_id, dbid,                                   │
  │                 │  session_key,                                     │
  │                 │  client_addr)                                     │
  │                 │                                  CheckoutEntity  │
  │                 │                                  → DBApp          │
  │                 │                                  创建 Proxy       │
  │                 │←[8] PrepareLoginAck ────────────────────────────│
  │                 │ (ok, entity_id)                                   │
  │                 │                                                  │
  │←[9] LoginOk ──│                                                  │
  │ (session_key,   │                                                  │
  │  baseapp_addr,  │                                                  │
  │  baseapp_port)  │                                                  │
  │                                                                    │
  │─[10] RUDP连接 ───────────────────────────────────────────────────→│
  │─[11] Authenticate(session_key) ──────────────────────────────────→│
  │                                                     验证session_key│
  │                                                     attach_client  │
  │←[12] AuthResult(ok, entity_id, type, properties) ────────────────│
  │                                                                    │
  │                                            C# Account.OnLoggedIn() │
```

**与 BigWorld 的关键差异:**
- BigWorld: LoginApp → DBApp → BaseAppMgr → BaseApp（DBApp 负责 checkout + 中转）
- Atlas: LoginApp → DBApp (验证) → BaseAppMgr (分配) → BaseApp (checkout + 创建)
  - 更清晰的职责分离：DBApp 只做认证，BaseApp 负责 checkout

---

## 3. 消息协议设计

### 3.1 LoginApp 消息

| 消息 | ID | 方向 | 用途 |
|------|-----|------|------|
| `LoginRequest` | 5000 | Client → LoginApp | 登录请求 |
| `LoginResult` | 5001 | LoginApp → Client | 登录结果 |
| `AuthLogin` | 5002 | LoginApp → DBApp | 验证账号 (含 auto_create 标志) |
| `AuthLoginResult` | 5003 | DBApp → LoginApp | 验证结果 |
| `AllocateBaseApp` | 5004 | LoginApp → BaseAppMgr | 分配 BaseApp |
| `AllocateBaseAppResult` | 5005 | BaseAppMgr → LoginApp | 分配结果 |
| `PrepareLogin` | 5006 | LoginApp → BaseApp | 准备创建 Proxy |
| `PrepareLoginResult` | 5007 | BaseApp → LoginApp | 准备结果 |

### 3.2 BaseAppMgr 消息

| 消息 | ID | 方向 | 用途 |
|------|-----|------|------|
| `RegisterBaseApp` | 6000 | BaseApp → BaseAppMgr | 注册 BaseApp |
| `RegisterBaseAppAck` | 6001 | BaseAppMgr → BaseApp | 注册结果 (AppID + EntityID 范围) |
| `BaseAppReady` | 6002 | BaseApp → BaseAppMgr | BaseApp 初始化完成 |
| `InformLoad` | 6003 | BaseApp → BaseAppMgr | 负载上报 |
| `AllocateBaseApp` | 5004 | LoginApp → BaseAppMgr | (同上) |
| `AllocateBaseAppResult` | 5005 | BaseAppMgr → LoginApp | (同上) |
| `RegisterGlobalBase` | 6010 | BaseApp → BaseAppMgr | 注册全局实体 |
| `DeregisterGlobalBase` | 6011 | BaseApp → BaseAppMgr | 注销全局实体 |
| `GlobalBaseNotification` | 6012 | BaseAppMgr → BaseApp | 广播全局实体变更 |
| `RequestEntityIdRange` | 6020 | BaseApp → BaseAppMgr | 请求新 EntityID 区间 |
| `RequestEntityIdRangeAck` | 6021 | BaseAppMgr → BaseApp | 新区间分配结果 |

### 3.2b BaseApp 间消息 (跨 BaseApp 操作)

| 消息 | ID | 方向 | 用途 |
|------|-----|------|------|
| `ForceLogoff` | 2030 | BaseApp → BaseApp | 强制踢下线 (重复登录) |
| `ForceLogoffAck` | 2031 | BaseApp → BaseApp | 踢下线结果 |

### 3.3 详细消息定义

#### LoginRequest / LoginResult

```cpp
namespace atlas::login {

struct LoginRequest {
    std::string username;
    std::string password_hash;     // SHA-256(password + salt)

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // serialize / deserialize ...
};

enum class LoginStatus : uint8_t {
    Success           = 0,
    InvalidCredentials= 1,
    AlreadyLoggedIn   = 2,
    ServerFull        = 3,
    RateLimited       = 4,
    ServerNotReady    = 5,
    InternalError     = 6,
};

struct LoginResult {
    LoginStatus status;
    SessionKey session_key;        // 仅 Success 时有效
    Address baseapp_addr;          // BaseApp 外部地址
    std::string error_message;     // 失败时的描述

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

} // namespace atlas::login
```

#### RegisterBaseApp / RegisterBaseAppAck

```cpp
namespace atlas::baseappmgr {

struct RegisterBaseApp {
    Address internal_addr;         // 内部通信地址
    Address external_addr;         // 外部通信地址 (客户端连接用)
    bool is_service_app;           // 是否为 ServiceApp

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

struct RegisterBaseAppAck {
    bool success;
    uint32_t app_id;               // 分配的 BaseApp ID
    EntityID entity_id_start;      // EntityID 分配范围起始
    EntityID entity_id_end;        // EntityID 分配范围结束
    uint64_t game_time;            // 当前游戏时间 (时间同步)

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

} // namespace atlas::baseappmgr
```

#### InformLoad

```cpp
namespace atlas::baseappmgr {

struct InformLoad {
    float load;                    // 0.0 ~ 1.0 CPU 负载
    uint32_t entity_count;         // 实体总数
    uint32_t proxy_count;          // Proxy 数量 (在线客户端)

    static auto descriptor() -> const MessageDesc& { /*...*/ }
    // ...
};

} // namespace atlas::baseappmgr
```

---

## 4. 核心模块设计

### 4.1 LoginApp

```cpp
// src/server/LoginApp/loginapp.hpp
namespace atlas {

class LoginApp : public ManagerApp {
public:
    using ManagerApp::ManagerApp;

protected:
    auto init(int argc, char* argv[]) -> bool override;
    void fini() override;
    void on_tick_complete() override;
    void register_watchers() override;

private:
    // ---- 外部消息处理器 (客户端) ----
    void on_login_request(const Address& src, Channel* ch,
                          const login::LoginRequest& msg);

    // ---- 内部消息处理器 ----
    void on_auth_result(const Address& src, Channel* ch,
                        const login::AuthLoginResult& msg);
    void on_allocate_result(const Address& src, Channel* ch,
                            const login::AllocateBaseAppResult& msg);
    void on_prepare_login_result(const Address& src, Channel* ch,
                                  const login::PrepareLoginResult& msg);

    // ---- 速率限制 ----
    auto check_rate_limit(uint32_t client_ip) -> bool;
    void cleanup_stale_state();

    // ---- 组件 ----
    NetworkInterface ext_network_;        // 外部 (客户端)

    // ---- 外部连接断开 (清理 PendingLogin) ----
    void on_ext_channel_disconnect(ChannelId channel_id);

    // 待处理的登录 (request_id → PendingLogin)
    struct PendingLogin {
        ChannelId client_channel_id;   // 使用 ChannelId 替代 Channel* 避免悬垂指针
        std::string username;
        Address client_addr;
        TimePoint request_time;
        // 状态机
        enum class State { Authenticating, Allocating, Preparing };
        State state;
        // 中间结果
        DatabaseID dbid = 0;
        uint16_t type_id = 0;
        Address baseapp_addr;
        SessionKey session_key;
    };
    std::unordered_map<uint32_t, PendingLogin> pending_;
    uint32_t next_request_id_ = 1;

    static constexpr Duration kLoginTimeout = Seconds(30);  // PendingLogin 超时

    // 速率限制
    struct RateState {
        int count = 0;
        TimePoint window_start;
    };
    std::unordered_map<uint32_t, RateState> per_ip_rates_;  // IP → rate
    int global_login_count_ = 0;
    TimePoint global_window_start_;

    // 依赖进程地址 + 可用状态 (通过 machined birth/death 事件更新)
    Address dbapp_addr_;
    bool dbapp_available_ = false;
    Address baseappmgr_addr_;
    bool baseappmgr_available_ = false;
};

} // namespace atlas
```

**登录处理状态机:**

```
on_login_request():
    1. 检查速率限制 (per-IP + 全局)
    2. 检查 BaseAppMgr/DBApp 是否可用 (dbapp_available_ && baseappmgr_available_)
       → 不可用: 回复 LoginResult(ServerNotReady)
    3. 创建 PendingLogin (state = Authenticating, 记录 ChannelId)
    4. 发送 AuthLogin → DBApp

on_auth_result():
    5. 匹配 PendingLogin
    6. 检查客户端连接是否仍然有效 (ChannelId 查找)
       → 已断开: 清理 PendingLogin, return
    7. 如果认证失败 → 回复客户端 LoginResult(fail)
    8. 如果成功: state = Allocating
    9. 生成 SessionKey
    10. 发送 AllocateBaseApp → BaseAppMgr

on_allocate_result():
    11. 匹配 PendingLogin + 检查连接有效性
    12. 如果分配失败 → 回复客户端 LoginResult(ServerFull)
    13. 如果成功: state = Preparing
    14. 记录 baseapp_addr
    15. 发送 PrepareLogin → BaseApp (含 session_key)

on_prepare_login_result():
    16. 匹配 PendingLogin + 检查连接有效性
    17. 如果创建失败 → 回复客户端 LoginResult(InternalError)
    18. 如果成功:
    19. 回复客户端 LoginResult(Success, session_key, baseapp_addr)
    20. 清理 PendingLogin

on_ext_channel_disconnect(channel_id):
    // 客户端在登录过程中断开: 清理关联的 PendingLogin
    for (auto it = pending_.begin(); it != pending_.end(); ++it)
        if (it->second.client_channel_id == channel_id)
            pending_.erase(it); break;

cleanup_stale_state():  // 在 on_tick_complete 中调用
    // 超时清理: 清除超过 kLoginTimeout (30s) 的 PendingLogin
    auto now = Clock::now();
    for (auto it = pending_.begin(); it != pending_.end(); )
        if (now - it->second.request_time > kLoginTimeout)
            send_login_failure(it->second.client_channel_id, InternalError, "Timeout");
            it = pending_.erase(it);
        else ++it;
```

**依赖进程监控:**

```cpp
// init() 中:
machined_client().listen_for_death(ProcessType::DBApp,
    [this](auto&) { dbapp_available_ = false;
        ATLAS_LOG_WARNING("DBApp unavailable, rejecting new logins"); });
machined_client().listen_for_birth(ProcessType::DBApp,
    [this](auto& n) { dbapp_addr_ = n.internal_addr; dbapp_available_ = true;
        ATLAS_LOG_INFO("DBApp available at {}", n.internal_addr); });
// 同样处理 BaseAppMgr
```

### 4.2 BaseAppMgr

```cpp
// src/server/BaseAppMgr/baseappmgr.hpp
namespace atlas {

class BaseAppMgr : public ManagerApp {
public:
    using ManagerApp::ManagerApp;

protected:
    auto init(int argc, char* argv[]) -> bool override;
    void fini() override;
    void on_tick_complete() override;
    void register_watchers() override;

private:
    // ---- BaseApp 管理 ----
    void on_register_baseapp(const Address& src, Channel* ch,
                              const baseappmgr::RegisterBaseApp& msg);
    void on_baseapp_ready(const Address& src, Channel* ch,
                           const baseappmgr::BaseAppReady& msg);
    void on_inform_load(const Address& src, Channel* ch,
                         const baseappmgr::InformLoad& msg);
    void on_baseapp_death(const machined::DeathNotification& notif);

    // ---- 登录分配 ----
    void on_allocate_baseapp(const Address& src, Channel* ch,
                              const login::AllocateBaseApp& msg);

    // ---- Global Bases ----
    void on_register_global_base(const Address& src, Channel* ch,
                                  const baseappmgr::RegisterGlobalBase& msg);
    void on_deregister_global_base(const Address& src, Channel* ch,
                                    const baseappmgr::DeregisterGlobalBase& msg);

    // ---- 负载均衡 ----
    auto find_least_loaded() const -> const BaseAppInfo*;
    auto is_overloaded() const -> bool;

    // ---- BaseApp 集合 ----
    struct BaseAppInfo {
        Address internal_addr;
        Address external_addr;
        uint32_t app_id = 0;
        float load = 0.0f;
        uint32_t entity_count = 0;
        uint32_t proxy_count = 0;
        TimePoint last_load_report;
        bool is_ready = false;        // finishedInit 后为 true
        bool is_retiring = false;
    };
    std::unordered_map<Address, BaseAppInfo> baseapps_;

    // ---- Entity ID 分配 ----
    EntityID next_entity_range_start_ = 1;
    static constexpr uint32_t kEntityIdRangeSize = 10000;

    // ---- Global Bases ----
    struct GlobalBaseEntry {
        std::string key;               // 全局名称
        Address base_addr;             // 持有者 BaseApp
        EntityID entity_id;            // 实体 ID
        uint16_t type_id;
    };
    std::unordered_map<std::string, GlobalBaseEntry> global_bases_;

    // ---- 过载保护 ----
    TimePoint overload_start_;
    int logins_since_overload_ = 0;
    static constexpr float kOverloadThreshold = 0.9f;
    static constexpr int kOverloadLoginLimit = 5;
    static constexpr Duration kOverloadTolerancePeriod = Seconds(5);

    // ---- 状态 ----
    bool has_started_ = false;
    uint32_t next_app_id_ = 0;
};

} // namespace atlas
```

**负载均衡算法 (参照 BigWorld):**

```cpp
auto BaseAppMgr::find_least_loaded() const -> const BaseAppInfo* {
    const BaseAppInfo* best = nullptr;
    float lowest_load = 2.0f;

    for (const auto& [addr, info] : baseapps_) {
        if (!info.is_ready || info.is_retiring) continue;
        if (info.load < lowest_load) {
            lowest_load = info.load;
            best = &info;
        }
    }
    return best;
}

auto BaseAppMgr::is_overloaded() const -> bool {
    auto* best = find_least_loaded();
    if (!best || best->load <= kOverloadThreshold) return false;

    auto now = Clock::now();
    if (overload_start_ == TimePoint{}) {
        overload_start_ = now;
        logins_since_overload_ = 0;
    }

    auto duration = now - overload_start_;
    if (duration > kOverloadTolerancePeriod ||
        logins_since_overload_ >= kOverloadLoginLimit) {
        return true;
    }

    ++logins_since_overload_;
    return false;
}
```

**Entity ID 区间分配:**

```cpp
// BaseApp 注册时分配
void BaseAppMgr::on_register_baseapp(...) {
    // ...
    EntityID range_start = next_entity_range_start_;
    EntityID range_end = range_start + kEntityIdRangeSize - 1;
    next_entity_range_start_ = range_end + 1;

    RegisterBaseAppAck ack{true, app_id, range_start, range_end, game_time};
    channel->send_message(ack);

    // BaseApp 收到后设置: entity_mgr_.set_id_range(start, end)
}
```

### 4.3 AuthLogin — DBApp 侧认证

在 DBApp (Phase 7) 中新增认证消息处理：

```cpp
// src/server/DBApp/dbapp.cpp (扩展)
void DBApp::on_auth_login(const Address& src, Channel* ch,
                           const login::AuthLogin& msg)
{
    // 1. 按用户名查找实体
    database_->lookup_by_name(account_type_id_, msg.username,
        [this, src, ch, username = msg.username,
         password_hash = msg.password_hash, request_id = msg.request_id]
        (LookupResult result)
    {
        login::AuthLoginResult reply;
        reply.request_id = request_id;

        if (!result.found) {
            if (auto_create_accounts_) {
                // 账号不存在 + 自动创建开启: 创建新账号
                create_account(username, password_hash, request_id, ch);
                return;
            }
            reply.success = false;
            reply.status = login::LoginStatus::InvalidCredentials;
        } else {
            // 验证密码哈希
            if (!result.password_hash.empty() &&
                !verify_password_hash(password_hash, result.password_hash)) {
                reply.success = false;
                reply.status = login::LoginStatus::InvalidCredentials;
            } else {
                reply.success = true;
                reply.dbid = result.dbid;
                reply.type_id = account_type_id_;
            }
        }

        ch->send_message(reply);
    });
}
```

> **密码验证:** Phase 7 的 DB 表已含 `sm_passwordHash` 列，`LookupResult` 返回密码哈希。
> DBApp 使用 bcrypt 或 argon2 验证。初期可选简化为 SHA-256 对比。
>
> **账号自动创建:** `auto_create_accounts_` 配置项。开启后首次登录自动创建账号实体，
> 便于开发和测试。生产环境关闭，由独立的注册流程创建账号。

### 4.4 PrepareLogin — BaseApp 侧处理

在 BaseApp (Phase 8) 中新增登录准备：

```cpp
// src/server/BaseApp/baseapp.cpp (扩展)
void BaseApp::on_prepare_login(const Address& src, Channel* ch,
                                const login::PrepareLogin& msg)
{
    // 1. 从 DBApp checkout 实体
    entity_mgr_.create_entity_from_db(msg.type_id, msg.dbid,
        [this, src, ch, session_key = msg.session_key,
         client_addr = msg.client_addr, request_id = msg.request_id]
        (Result<BaseEntity*> result)
    {
        login::PrepareLoginResult reply;
        reply.request_id = request_id;

        if (!result.has_value()) {
            reply.success = false;
            reply.error = result.error().message();
            ch->send_message(reply);
            return;
        }

        auto* entity = result.value();

        // 2. 如果不是 Proxy，转为 Proxy（或验证类型支持客户端）
        auto* proxy = dynamic_cast<Proxy*>(entity);
        if (!proxy) {
            reply.success = false;
            reply.error = "Entity type does not support client";
            ch->send_message(reply);
            return;
        }

        // 3. 存储 session key (等待客户端连接, 30 秒超时)
        proxy->set_session_key(session_key);
        pending_logins_[session_key] = {proxy->id(), Clock::now()};
        // pending_logins_ 在 on_tick_complete 中检查超时:
        // 超过 kSessionKeyTimeout (30s) 未被认领 → 销毁 Proxy + checkin

        reply.success = true;
        reply.entity_id = proxy->id();
        ch->send_message(reply);
    });
}
```

**客户端连接 BaseApp 时:**

```cpp
void BaseApp::on_client_authenticate(const Address& src, Channel* ch,
                                      const baseapp_ext::Authenticate& msg)
{
    // 查找 pending login
    auto it = pending_logins_.find(msg.session_key);
    if (it == pending_logins_.end()) {
        // 无效 session key → 拒绝
        send_auth_failure(ch, "Invalid session key");
        return;
    }

    EntityID entity_id = it->second;
    pending_logins_.erase(it);

    auto* proxy = entity_mgr_.find_proxy(entity_id);
    if (!proxy) {
        send_auth_failure(ch, "Entity not found");
        return;
    }

    // 绑定客户端
    auto result = proxy->attach_client(ch, msg.session_key);
    if (!result) {
        send_auth_failure(ch, result.error().message());
        return;
    }

    // 记录映射
    client_to_entity_[ch] = entity_id;

    // 发送认证成功 + 初始实体数据
    proxy->send_create_base_player();

    // 触发 C# 回调
    // → NativeApi → C# Account.OnLoggedIn()
}
```

### 4.5 SessionKey

```cpp
// src/lib/connection/session_key.hpp
namespace atlas {

struct SessionKey {
    std::array<uint8_t, 32> bytes = {};

    static auto generate() -> SessionKey;

    auto operator==(const SessionKey& other) const -> bool;
    auto operator!=(const SessionKey& other) const -> bool { return !(*this == other); }

    [[nodiscard]] auto is_zero() const -> bool;

    // 序列化
    void write_to(BinaryWriter& w) const;
    static auto read_from(BinaryReader& r) -> Result<SessionKey>;
};

} // namespace atlas
```

### 4.6 Global Bases — 全局实体注册

BigWorld 的 Global Bases 允许跨 BaseApp 查找特定命名的实体。
Atlas 通过 BaseAppMgr 管理，初期只在 C++ 层实现。

```cpp
// BaseApp 侧: 注册全局实体
void BaseApp::register_global_base(const std::string& key,
                                     EntityID entity_id, uint16_t type_id)
{
    baseappmgr::RegisterGlobalBase msg{key, entity_id, type_id};
    // 发送到 BaseAppMgr
}

// BaseAppMgr 侧: 广播到所有 BaseApp
void BaseAppMgr::on_register_global_base(...) {
    global_bases_[msg.key] = {msg.key, src_addr, msg.entity_id, msg.type_id};

    // 广播给所有 BaseApp
    GlobalBaseNotification notif{msg.key, src_addr, msg.entity_id, msg.type_id, /*added=*/true};
    for (auto& [addr, info] : baseapps_) {
        if (info.is_ready) {
            auto* ch = network().find_channel(addr);
            if (ch) ch->send_message(notif);
        }
    }
}
```

---

## 5. 重复登录处理

当已在线的账号再次登录时，分**本地**和**跨 BaseApp** 两种情况。

### 5.1 本地重复登录 (旧 Proxy 在同一 BaseApp)

```
Client B (新)        LoginApp          BaseApp-X (旧+新)      Client A (旧)
  │                     │                    │                     │
  │── Login(user) ─────→│                    │                     │
  │                     │ ... auth + alloc   │                     │
  │                     │── PrepareLogin ──→│                     │
  │                     │                   │ CheckoutEntity(42)   │
  │                     │                   │ → DBApp: AlreadyCheckedOut │
  │                     │                   │   (owner = 本 BaseApp)│
  │                     │                   │                      │
  │                     │                   │ 本地踢掉旧 Proxy:    │
  │                     │                   │  old_proxy.detach()   │
  │                     │                   │  old_proxy.checkin()  │
  │                     │                   │──── LoggedOff ──────→│
  │                     │                   │                      ╳
  │                     │                   │ 重试 Checkout → 成功 │
  │                     │                   │ 创建新 Proxy          │
  │                     │←── ok ───────────│                      │
  │←── LoginOk ────────│                   │                      │
```

### 5.2 跨 BaseApp 重复登录 (旧 Proxy 在不同 BaseApp)

```
Client B(新)   LoginApp   BaseApp-Y(新)   BaseApp-X(旧Proxy)  Client A(旧)
  │               │            │                │                  │
  │─ Login ──────→│            │                │                  │
  │               │ ... auth + alloc (分配到 Y)  │                  │
  │               │─ PrepareLogin ─→│           │                  │
  │               │                 │ Checkout   │                  │
  │               │                 │ → DBApp:   │                  │
  │               │                 │ AlreadyCheckedOut             │
  │               │                 │ (owner=BaseApp-X, app_id=2)  │
  │               │                 │                               │
  │               │                 │─ ForceLogoff(dbid=42) ──────→│
  │               │                 │                    踢旧Proxy  │
  │               │                 │                    detach+checkin
  │               │                 │                    ──LogOff──→│
  │               │                 │←─ ForceLogoffAck(ok) ───────│ ╳
  │               │                 │                               │
  │               │                 │ 重试 Checkout → 成功          │
  │               │                 │ 创建新 Proxy                  │
  │               │←─ ok ──────────│                               │
  │←─ LoginOk ───│                 │                               │
```

> **ForceLogoff 路由:** BaseApp-Y 从 CheckoutInfo 中获取 `base_addr`（BaseApp-X 的内部地址），
> 直接发送 `ForceLogoff` 消息。无需经过 BaseAppMgr 中转。

---

## 6. 实现步骤

### Step 9.1: 消息定义

**新增文件:**
```
src/server/LoginApp/login_messages.hpp
src/server/BaseAppMgr/baseappmgr_messages.hpp
src/lib/connection/session_key.hpp / .cpp
tests/unit/test_login_messages.cpp
tests/unit/test_session_key.cpp
```

### Step 9.2: BaseAppMgr 进程

**新增文件:**
```
src/server/BaseAppMgr/
├── CMakeLists.txt
├── main.cpp
├── baseappmgr.hpp / .cpp
├── baseappmgr_messages.hpp     (from 9.1)
```

**实现顺序:**
1. 基本启动（ManagerApp + machined 注册）
2. BaseApp 注册（`RegisterBaseApp` / `RegisterBaseAppAck`）
3. Entity ID 区间分配
4. 负载上报（`InformLoad`）
5. 负载均衡（`find_least_loaded`）
6. 登录分配（`AllocateBaseApp`）
7. 过载保护（`is_overloaded`）
8. BaseApp 死亡处理
9. Global Bases 管理
10. Watcher 注册

**测试用例:**
- BaseApp 注册/注销
- Entity ID 区间不重叠
- 负载均衡选择最低负载
- 过载保护触发和恢复
- Global Bases 注册/注销/广播
- BaseApp 死亡 → 清理

### Step 9.3: LoginApp 进程

**新增文件:**
```
src/server/LoginApp/
├── CMakeLists.txt
├── main.cpp
├── loginapp.hpp / .cpp
├── login_messages.hpp          (from 9.1)
```

**实现顺序:**
1. 基本启动（ManagerApp + 双网络接口 + machined）
2. 发现 DBApp 和 BaseAppMgr（machined query）
3. 外部 RUDP 监听（客户端连接）
4. `LoginRequest` 处理 → 状态机
5. 速率限制（per-IP + 全局）
6. 发送 `AuthLogin` → DBApp → 处理 `AuthLoginResult`
7. 发送 `AllocateBaseApp` → BaseAppMgr → 处理结果
8. 发送 `PrepareLogin` → BaseApp → 处理结果
9. 回复客户端 `LoginResult`
10. 超时清理（过期的 PendingLogin）
11. Watcher 注册

### Step 9.4: DBApp 认证扩展

**更新文件:**
```
src/server/DBApp/dbapp.hpp / .cpp (新增 on_auth_login)
src/server/DBApp/dbapp_messages.hpp (新增 AuthLogin / AuthLoginResult)
```

### Step 9.5: BaseApp 登录集成

**更新文件:**
```
src/server/BaseApp/baseapp.hpp / .cpp (新增 on_prepare_login, pending_logins_)
src/server/BaseApp/baseapp_messages.hpp (新增 PrepareLogin / PrepareLoginResult)
```

同时集成:
- BaseApp 启动时向 BaseAppMgr 注册
- 定期上报负载（`InformLoad`）
- Entity ID 区间管理
- 重复登录踢下线逻辑

### Step 9.6: 集成测试

**新增文件:**
```
tests/integration/test_login_flow.cpp
```

端到端场景（需要 machined + DBApp + BaseAppMgr + BaseApp + LoginApp）：
1. 全部启动，等待就绪
2. 模拟客户端连接 LoginApp
3. 发送 `LoginRequest` → 完整登录流程
4. 客户端连接 BaseApp → `Authenticate` → 成功
5. C# `Account.OnLoggedIn()` 回调
6. 第二个客户端用相同账号登录 → 第一个被踢
7. 大量并发登录 → 速率限制生效
8. 关闭一个 BaseApp → BaseAppMgr 检测到死亡

---

## 7. 文件清单汇总

```
src/lib/connection/
├── session_key.hpp / .cpp

src/server/LoginApp/
├── CMakeLists.txt
├── main.cpp
├── loginapp.hpp / .cpp
└── login_messages.hpp

src/server/BaseAppMgr/
├── CMakeLists.txt
├── main.cpp
├── baseappmgr.hpp / .cpp
└── baseappmgr_messages.hpp

src/server/DBApp/                       (扩展)
├── dbapp.hpp / .cpp                    (+ on_auth_login)
├── dbapp_messages.hpp                  (+ AuthLogin messages)

src/server/BaseApp/                     (扩展)
├── baseapp.hpp / .cpp                  (+ on_prepare_login, BaseAppMgr 注册, ForceLogoff)
├── baseapp_messages.hpp                (+ PrepareLogin, ForceLogoff, RequestEntityIdRange)

tests/unit/
├── test_login_messages.cpp
├── test_session_key.cpp
├── test_baseappmgr.cpp

tests/integration/
└── test_login_flow.cpp
```

---

## 8. 依赖关系与执行顺序

```
Step 9.1: 消息定义 + SessionKey     ← 无依赖
    │
    ├── Step 9.2: BaseAppMgr         ← 依赖 9.1
    │
    ├── Step 9.4: DBApp 认证扩展     ← 依赖 9.1
    │
    └── Step 9.5: BaseApp 登录集成   ← 依赖 9.1 + 9.2
            │
            ▼
        Step 9.3: LoginApp           ← 依赖 9.2 + 9.4 + 9.5
            │
            ▼
        Step 9.6: 集成测试           ← 依赖全部
```

**推荐执行顺序:**

```
第 1 轮:            9.1 消息定义 + SessionKey
第 2 轮 (并行):     9.2 BaseAppMgr + 9.4 DBApp 认证 + 9.5 BaseApp 登录
第 3 轮:            9.3 LoginApp
第 4 轮:            9.6 集成测试
```

---

## 9. BigWorld 完整对照

| BigWorld | Atlas | 差异说明 |
|----------|-------|---------|
| UDP probe + UDP login | RUDP 连接 + 消息 | Atlas 仍更简单，且与当前实现一致 |
| RSA 加密凭证 | 明文 (初期) + 后续 TLS | 简化，TLS 更现代 |
| LoginApp → DBApp → BaseAppMgr → BaseApp | LoginApp → DBApp (认证) → BaseAppMgr → BaseApp | Atlas 路径更清晰 |
| SessionKey = uint32 (timestamp) | SessionKey = 32 bytes (random) | Atlas 更安全 |
| `BillingSystem` 可插拔认证 | DBApp 查 sm_passwordHash | 初期简化，后续可外接 |
| `findLeastLoadedApp()` | `find_least_loaded()` | 算法一致 |
| `calculateOverloaded()` 时间+计数 | `is_overloaded()` 相同逻辑 | 一致 |
| 28-bit AppID | 32-bit app_id | 范围更大 |
| EntityID 由 BaseApp 自行分配 | BaseAppMgr 分配区间 | Atlas 更可控 |
| `globalBases_` + 广播 | `global_bases_` + 广播 | 相同模式 |
| `BackupHash` 备份 | 初期不实现 | Phase 13 |
| 5 阶段关闭 | 初期简单关闭 | 后续优化 |
| 多 LoginApp + probe 发现 | 当前基线单 LoginApp；多实例扩展见 `MULTI_LOGINAPP_DESIGN.md` | 详细方案已拆分到独立文档 |
| `SharedData` (BaseApp间+全局) | 初期不实现 | 按需 |
| 登录结果缓存 (防重发) | 当前不实现 | Atlas 现阶段无 probe/cache 需求 |
| Channel\* 原始指针 | ChannelId + 断开回调 | 避免悬垂指针 |
| 无 session 超时 | pending_logins_ 30s 超时 | 防泄漏 |
| 重复登录仅本地踢 | ForceLogoff 跨 BaseApp | 多 BaseApp 安全 |
| 无自动创建账号 | auto_create 可配置 | 便于开发 |

---

## 10. 关键设计决策记录

### 10.1 登录路径简化

**BigWorld:** LoginApp → DBApp (`logOn`) → BillingSystem → CheckoutEntity → BaseAppMgr (`createEntity`) → BaseApp → 回复链
- DBApp 承担了认证 + checkout + BaseApp 创建请求的中转

**Atlas:** LoginApp 作为编排者，分别调用各组件
- LoginApp → DBApp (仅认证)
- LoginApp → BaseAppMgr (仅分配)
- LoginApp → BaseApp (checkout + 创建 Proxy)

优势: 职责清晰，每个组件只做一件事。代价: LoginApp 的状态机稍复杂。

### 10.2 Entity ID 区间分配

**BigWorld:** 每个 BaseApp 自行分配 EntityID，AppID 编码在高位保证全局唯一。

**Atlas:** BaseAppMgr 显式分配 EntityID 区间（如 1-10000, 10001-20000, ...）。
- 更直观，不依赖位编码
- BaseApp 用完区间后向 BaseAppMgr 申请新区间
- 缺点：需要额外的区间请求消息（但频率极低）

### 10.3 LoginApp 无脚本引擎

**决策: LoginApp 继承 `ManagerApp`（无脚本）。**

LoginApp 只做消息路由和速率限制，不执行游戏逻辑。
认证逻辑在 DBApp 中，实体创建在 BaseApp 中。

### 10.4 PendingLogin 安全性

**决策: ChannelId + 断开清理 + 超时。**

PendingLogin 使用 ChannelId（非 Channel\*）避免悬垂指针。三层保护:
1. **断开清理:** `on_ext_channel_disconnect()` 立即清除对应的 PendingLogin
2. **有效性检查:** 每个异步回调步骤先检查 ChannelId 是否仍然有效
3. **超时清理:** 30 秒未完成的 PendingLogin 在 `cleanup_stale_state()` 中清除

LoginApp 的 PendingLogin 是非持久状态。LoginApp 崩溃后客户端需重连重试。
这是可接受的，因为登录流程通常在秒级完成。

### 10.5 跨 BaseApp 重复登录踢下线

**决策: BaseApp 直接发送 ForceLogoff，无需 BaseAppMgr 中转。**

CheckoutEntityAck 返回 `AlreadyCheckedOut` 时包含 `CheckoutInfo.base_addr`。
新 BaseApp 直接向旧 BaseApp 发送 `ForceLogoff(dbid)` 消息。旧 BaseApp 踢掉旧 Proxy，
checkin 实体，回复 `ForceLogoffAck`。新 BaseApp 收到 Ack 后重试 Checkout。

重试上限: 3 次。如果 3 次仍然失败（旧 BaseApp 无响应），返回 PrepareLoginResult 失败。

### 10.6 依赖进程监控

**决策: LoginApp 通过 machined birth/death 事件动态追踪 DBApp 和 BaseAppMgr 可用性。**

当依赖不可用时，新登录请求直接返回 `ServerNotReady`，避免发送到不可达的地址。
依赖恢复后自动恢复服务，无需重启 LoginApp。

### 10.7 Entity ID 区间续期

**决策: BaseApp 在区间剩余 < 20% 时主动请求新区间。**

`RequestEntityIdRange` 消息由 BaseApp 在 `check_entity_id_range()` 中触发。
BaseAppMgr 分配新区间后回复 `RequestEntityIdRangeAck`。
BaseApp 的 EntityManager 调用 `extend_id_range(new_end)` 扩展上限。

ID 区间的典型使用: 10000 个 ID 可支持很长时间（实体创建+销毁后 ID 不回收）。
极端场景下（频繁创建+销毁），续期消息频率约每几分钟一次，开销可忽略。

### 10.8 账号自动创建

**决策: `AuthLogin` 消息包含 `auto_create` 标志，DBApp 支持自动创建账号。**

开启后，首次登录时 DBApp 自动创建 Account 实体（使用提供的密码哈希），
返回新分配的 DBID。方便开发和测试。生产环境关闭此配置。

### 10.9 Session Key 超时

**决策: BaseApp 端 pending_logins_ 设 30 秒超时。**

LoginApp 返回 LoginResult(Success) 后，客户端应在 30 秒内连接 BaseApp 并 Authenticate。
超时未认领的 Proxy 被销毁，实体 checkin 回 DBApp。防止 session key 泄漏后被长期滥用。

### 10.10 初期不实现的功能

| 功能 | 原因 | 何时实现 |
|------|------|---------|
| 加密通信 (RSA/TLS) | 初期内网部署不需要 | 按需 |
| 多 LoginApp | 详细设计见 `MULTI_LOGINAPP_DESIGN.md` | 按详细文档推进 |
| Login Challenge (PoW) | 内网无需防 DDoS | 按需 |
| 登录结果缓存 | 当前登录协议无 probe/cache 需求 | 不实现 |
| SharedData 机制 | 复杂，初期无场景 | 按需 |
| 备份 Hash | 需要多 BaseApp 容灾 | Phase 13 |
| 协调关闭 | 初期直接停 | 后续 |
