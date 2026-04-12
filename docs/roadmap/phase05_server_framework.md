# Phase 5: 服务器框架基类 (`src/lib/server/`)

> 前置依赖: 脚本层运行时基线就绪（Script Phase 0-3 + Script Phase 4 最小子集）
> BigWorld 参考: `lib/server/server_app.hpp`, `lib/server/script_app.hpp`, `lib/server/entity_app.hpp`
> 说明: 当前代码已经具备 `Atlas.Shared`、基础属性标记、`SpanWriter/SpanReader`、
> `EntityDefRegistry` 注册等最小共享能力；Script Phase 5/6 属于后续增强项，不是本阶段阻塞条件。

---

## 目标

为所有 Atlas 服务器进程提供统一的运行时骨架。每个进程继承适当层级的基类，注册消息处理器，即可获得：事件循环、网络通信、C# 脚本集成、定时器、后台任务、信号处理、运行时监控。

## 验收标准

- [ ] `ServerApp` 基类可启动主循环，处理信号，优雅关闭
- [ ] `ServerApp` 的 tick 系统可工作：`on_start_of_tick` → `Updatable::update()` → `on_tick_complete`
- [ ] `ServerApp` tick 性能监控可工作：慢帧检测 + Watcher 统计
- [ ] `ScriptApp` 可初始化 `ClrScriptEngine` 并在 tick 中驱动 C# 脚本
- [ ] `EntityApp` 提供 `BgTaskManager` 和脚本定时器
- [ ] `ManagerApp` 为无脚本的管理进程提供轻量基类
- [ ] `ServerConfig` 可从命令行和 JSON 文件加载配置
- [ ] `ServerAppOption<T>` 可声明子类配置项，自动从 JSON 加载并注册 Watcher
- [ ] Watcher 系统可注册和查询运行时指标
- [ ] 至少一个最小示例进程可基于框架启动并运行
- [ ] 全部新增代码有单元测试

---

## 1. BigWorld 架构分析与 Atlas 适配

### 1.1 BigWorld 的类层次

```
ServerApp                     — 事件循环, 时间, 信号, Updatable, Watcher
  ├── ManagerApp              — 管理进程 (无脚本)
  └── ScriptApp               — + Python 解释器, ScriptEvents, personality 模块
        └── EntityApp          — + BgTaskManager, ScriptTimeQueue
              ├── BaseApp      — + Proxy/Base 实体, 客户端连接
              └── CellApp      — + Space, Real/Ghost, AOI
```

### 1.2 Atlas 的适配设计

**关键差异: C# 替代 Python**

| BigWorld (ScriptApp) | Atlas (ScriptApp) | 原因 |
|-----|-----|------|
| 初始化 Python 解释器 | 初始化 ClrHost + 加载 C# 程序集 | 脚本层已替换 |
| `ScriptEvents` 持有 `PyObjectPtr` | `ScriptEvents` 通过 `ScriptEngine` 抽象调用 | 已在 Script Phase 0 解耦 |
| `personality` 模块加载 | `Atlas.Runtime.dll` 加载 | C# 对等物 |
| `PythonServer` (调试端口) | 不需要（C# 有 dotnet-monitor/诊断端口） | .NET 原生工具更好 |
| `ScriptTimers` (Python 可调的定时器) | 通过 `INativeApiProvider` 导出定时器 API 给 C# | C# 侧用 Source Generator 生成调用 |
| `triggerOnInit(isReload)` | `ScriptEngine::on_init(is_reload)` | 接口已存在 |

**Atlas 类层次:**

```
ServerApp                     — 事件循环, GameClock, 信号, Updatable, Watcher, 配置
  ├── ManagerApp              — 管理进程 (BaseAppMgr, CellAppMgr, DBAppMgr, Reviver)
  └── ScriptApp               — + ClrScriptEngine, INativeApiProvider, 脚本生命周期
        └── EntityApp          — + BgTaskManager, EntityDefRegistry, 脚本定时器
              ├── BaseApp
              └── CellApp
```

---

## 2. 实现步骤（按顺序）

### Step 5.1: ServerConfig — 配置加载

**新增文件:**

```
src/lib/server/server_config.hpp
src/lib/server/server_config.cpp
tests/unit/test_server_config.cpp
```

**设计:**

```cpp
// src/lib/server/server_config.hpp
namespace atlas {

/// 进程类型标识
enum class ProcessType : uint8_t {
    Machined    = 0,
    LoginApp    = 1,
    BaseApp     = 2,
    BaseAppMgr  = 3,
    CellApp     = 4,
    CellAppMgr  = 5,
    DBApp       = 6,
    DBAppMgr    = 7,
    Reviver     = 8,
};

auto process_type_name(ProcessType type) -> std::string_view;
auto process_type_from_name(std::string_view name) -> Result<ProcessType>;

struct ServerConfig {
    // ---- 身份 ----
    ProcessType process_type = ProcessType::BaseApp;
    std::string process_name;             // 实例名 (如 "baseapp01")

    // ---- 网络 ----
    Address machined_address;             // machined 地址 (默认 127.0.0.1:20018)
    uint16_t internal_port = 0;           // 内部通信端口 (0=自动)
    uint16_t external_port = 0;           // 外部通信端口 (仅 LoginApp/BaseApp)

    // ---- Tick ----
    int update_hertz = 10;                // tick 频率 (BigWorld 默认 10)

    // ---- 脚本 ----
    std::filesystem::path script_assembly; // C# 程序集路径
    std::filesystem::path runtime_config;  // .runtimeconfig.json 路径

    // ---- 日志 ----
    LogLevel log_level = LogLevel::Info;

    // ---- 杂项 ----
    bool is_production = false;

    // 加载方式
    static auto from_args(int argc, char* argv[]) -> Result<ServerConfig>;
    static auto from_json_file(const std::filesystem::path& path) -> Result<ServerConfig>;

    // 合并: 命令行参数覆盖配置文件
    static auto load(int argc, char* argv[]) -> Result<ServerConfig>;
};

} // namespace atlas
```

**命令行参数格式:**

```bash
atlas_baseapp \
    --type baseapp \
    --name baseapp01 \
    --machined 127.0.0.1:20018 \
    --internal-port 0 \
    --external-port 20100 \
    --config server.json \
    --log-level info
```

**JSON 配置文件格式:**

```json
{
    "update_hertz": 10,
    "machined_address": "127.0.0.1:20018",
    "is_production": false,
    "script": {
        "assembly": "Atlas.Runtime.dll",
        "runtime_config": "atlas_server.runtimeconfig.json"
    },
    "log_level": "info"
}
```

> 选择 JSON 而非 XML: Atlas 已有 JSON 解析器（`serialization` 库），无需引入新依赖。

**子类配置扩展: `ServerAppOption<T>`**

参照 BigWorld 的 `ServerAppOption` 模式，子类通过声明静态选项自动从 JSON 加载并注册 Watcher：

```cpp
// src/lib/server/server_app_option.hpp
namespace atlas {

/// 配置选项模板: 从 JSON 加载 + 自动注册 Watcher
/// 子类声明静态实例即可使用
template<typename T>
class ServerAppOption {
public:
    ServerAppOption(T default_value,
                    std::string_view json_path,        // JSON 中的路径 (如 "backup_period")
                    std::string_view watcher_path,     // Watcher 路径 (如 "baseapp/backup_period")
                    WatcherMode mode = WatcherMode::ReadOnly);

    [[nodiscard]] auto value() const -> const T& { return value_; }
    void set_value(const T& v) { value_ = v; }   // 仅 ReadWrite 模式

    /// 从 JSON 对象中加载值 (ServerConfig::load 中统一调用)
    void load_from(const nlohmann::json& root);

    /// 注册到 WatcherRegistry (ServerApp::register_watchers 中统一调用)
    void register_watcher(WatcherRegistry& registry);

    /// 全局收集器: 所有 ServerAppOption 实例自注册到此列表
    static auto all_options() -> std::vector<ServerAppOption*>&;

private:
    T value_;
    T default_;
    std::string json_path_;
    std::string watcher_path_;
    WatcherMode mode_;
};

} // namespace atlas
```

**用法示例（BaseApp 中）:**

```cpp
// base_app.cpp
static ServerAppOption<int> s_backup_period{
    10, "backup_period", "baseapp/backup_period", WatcherMode::ReadWrite};
static ServerAppOption<int> s_max_proxies{
    1000, "max_proxies", "baseapp/max_proxies", WatcherMode::ReadOnly};
```

`ServerConfig::load()` 在加载 JSON 后遍历 `ServerAppOption::all_options()` 统一初始化。
`ServerApp::register_watchers()` 中统一注册所有选项到 Watcher。

> **注意:** `ServerConfig` 需要持有原始 JSON 对象（`nlohmann::json extra_`），供 `ServerAppOption` 提取值。
> 已解析的公共字段（`update_hertz` 等）仍保留在 `ServerConfig` 结构体中。

**测试用例:**
- 命令行参数解析（各种参数组合）
- JSON 文件解析（正确/缺失/畸形）
- 命令行覆盖 JSON（优先级测试）
- 默认值验证
- `ServerAppOption<T>` 从 JSON 加载 + Watcher 注册

---

### Step 5.2: Updatable — tick 回调注册系统

**新增文件:**

```
src/lib/server/updatable.hpp
tests/unit/test_updatable.cpp
```

**设计:**

BigWorld 的 Updatable 系统允许对象按层级注册 tick 回调，低层级先执行。Atlas 复用此设计。

```cpp
// src/lib/server/updatable.hpp
namespace atlas {

/// 可在每帧更新的对象接口
class Updatable {
public:
    virtual ~Updatable() = default;
    virtual void update() = 0;

private:
    friend class Updatables;
    int removal_handle_ = -1;  // Updatables 内部使用
};

/// Updatable 集合管理器
/// 支持分层级执行: level 0 先于 level 1
/// 安全: update 过程中可移除对象 (标记删除, 延迟清理)
class Updatables {
public:
    explicit Updatables(int num_levels = 2);

    /// 注册: level 决定执行顺序 (低先高后)
    auto add(Updatable* object, int level = 0) -> bool;

    /// 注销 (可在 update() 内安全调用)
    auto remove(Updatable* object) -> bool;

    /// 按层级顺序调用所有 Updatable::update()
    void call();

    [[nodiscard]] auto size() const -> size_t;

private:
    std::vector<Updatable*> objects_;
    std::vector<int> level_sizes_;     // 每层级的对象数量
    bool in_update_ = false;
    int deleted_count_ = 0;
};

} // namespace atlas
```

**实现要点（参照 BigWorld）:**
- `objects_` 是扁平数组，按 level 分段存储：`[level0_objects... | level1_objects... | ...]`
- `level_sizes_[i]` 记录第 i 层有多少对象
- `add()` 时插入到对应 level 段的末尾
- `call()` 时设置 `in_update_=true`，遍历调用 `update()`
- `remove()` 在 `in_update_` 期间只做标记（设 null），不实际删除
- `call()` 结束后统一清理标记为 null 的槽位
- `removal_handle_` 存储对象在 `objects_` 中的索引，O(1) 定位

**测试用例:**
- 基本注册/注销
- 多层级执行顺序
- update() 内安全移除
- update() 内注册新对象（应在下次 call 时才执行）

---

### Step 5.3: ServerApp — 核心基类

**新增文件:**

```
src/lib/server/server_app.hpp
src/lib/server/server_app.cpp
tests/unit/test_server_app.cpp
```

**设计:**

```cpp
// src/lib/server/server_app.hpp
namespace atlas {

class ServerApp {
public:
    ServerApp(EventDispatcher& dispatcher, NetworkInterface& network);
    virtual ~ServerApp();

    // Non-copyable
    ServerApp(const ServerApp&) = delete;
    ServerApp& operator=(const ServerApp&) = delete;

    // ========== 生命周期 ==========

    /// 入口: init → run → fini
    /// 返回值作为进程退出码
    auto run_app(int argc, char* argv[]) -> int;

    /// 请求优雅关闭 (可在任意时刻调用)
    void shutdown();

    // ========== 访问器 ==========

    [[nodiscard]] auto dispatcher() -> EventDispatcher& { return dispatcher_; }
    [[nodiscard]] auto network() -> NetworkInterface& { return network_; }
    [[nodiscard]] auto config() const -> const ServerConfig& { return config_; }
    [[nodiscard]] auto game_clock() -> GameClock& { return game_clock_; }
    [[nodiscard]] auto watcher() -> WatcherRegistry& { return watcher_registry_; }

    // ========== 时间 ==========

    /// 当前游戏时间 (tick 计数)
    [[nodiscard]] auto game_time() const -> uint64_t { return game_clock_.frame_count(); }

    /// 游戏时间(秒)
    [[nodiscard]] auto game_time_seconds() const -> double;

    /// 进程运行时长(秒)
    [[nodiscard]] auto uptime_seconds() const -> double;

    // ========== Updatable 注册 ==========

    auto register_for_update(Updatable* object, int level = 0) -> bool;
    auto deregister_for_update(Updatable* object) -> bool;

protected:
    // ========== 子类覆盖点 ==========

    /// 初始化 (在 EventDispatcher/Network 就绪后调用)
    /// 子类应先调用基类 init()
    [[nodiscard]] virtual auto init(int argc, char* argv[]) -> bool;

    /// 清理 (在 run() 返回后调用)
    virtual void fini();

    /// 默认实现: dispatcher_.run()
    [[nodiscard]] virtual auto run() -> bool;

    /// run() 返回后调用
    virtual void on_run_complete() {}

    // ---- Tick 钩子 (每帧调用顺序) ----

    /// 在 time++ 之前
    virtual void on_end_of_tick() {}

    /// 在 time++ 之后, Updatable::update() 之前
    virtual void on_start_of_tick() {}

    /// 所有 Updatable 完成后
    virtual void on_tick_complete() {}

    // ---- 信号 ----

    /// 收到 OS 信号时调用 (在主线程上下文)
    virtual void on_signal(Signal sig);

    // ---- Watcher ----
    virtual void register_watchers();

private:
    /// 每帧推进: 钩子调用 + Updatable 执行
    /// 注册为 EventDispatcher 的定时器
    void advance_time();

    /// 信号回调
    void handle_signal(Signal sig);

    EventDispatcher& dispatcher_;
    NetworkInterface& network_;
    ServerConfig config_;
    GameClock game_clock_;
    Updatables updatables_;
    WatcherRegistry watcher_registry_;

    // tick 定时器与性能统计
    TimerHandle tick_timer_;
    TimePoint last_tick_time_;
    struct TickStats {
        Duration last_duration{};
        Duration max_duration{};
        uint64_t slow_count = 0;
    } tick_stats_;

    // 信号
    bool shutdown_requested_ = false;
    SignalDispatchTask signal_task_;

    // 启动时间
    TimePoint start_time_;
};

} // namespace atlas
```

**`run_app()` 流程 (参照 BigWorld `runApp()`):**

```
run_app(argc, argv):
    1. config_ = ServerConfig::load(argc, argv)
    2. 设置日志级别
    3. raise_fd_limit()                    // 提升文件描述符/句柄上限
    4. init(argc, argv)                    // 子类链式调用
       ├── 安装信号处理 (SIGINT, SIGTERM)
       ├── network_.bind(config_.internal_port) // 端口绑定 (构造时不绑定)
       ├── network_.set_extension_data(this)    // 消息处理器回溯 app 实例
       ├── 注册 tick 定时器: dispatcher_.add_repeating_timer(
       │       1s / config_.update_hertz, [this]{ advance_time(); })
       ├── 注册 Watcher (含 ServerAppOption 统一注册)
       └── 子类特定初始化
    5. LOG_INFO("{} started (pid={})", config_.process_name, pid)
    6. run()                               // 默认: dispatcher_.run()
    7. fini()                              // 子类链式清理
    8. network_.prepare_for_shutdown()
    9. 返回退出码
```

**关键时序说明:**
- `NetworkInterface` 在 `main()` 中构造时**不绑定端口**（仅创建 socket）
- 端口绑定在 `init()` 中执行，此时 `ServerConfig` 已解析完毕
- BigWorld 也是此模式：构造时创建，`init()` 中绑定
- `set_extension_data(this)` 让消息处理回调可通过 `NetworkInterface` 回溯到 `ServerApp` 实例，避免全局单例

**文件描述符/句柄限制提升:**

```cpp
/// 在 run_app() 早期调用，确保高连接数进程有足够 fd
void ServerApp::raise_fd_limit() {
#ifdef __linux__
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    rl.rlim_cur = rl.rlim_max;  // 提升到硬上限
    setrlimit(RLIMIT_NOFILE, &rl);
#elif defined(_WIN32)
    _setmaxstdio(8192);         // Windows 默认 512，提升到 8192
#endif
}
```

> BigWorld 在 `ServerApp::init()` 中显式提升 fd 上限，Atlas 沿用此做法。

**`advance_time()` 流程 (对齐 BigWorld `advanceTime()`):**

```
advance_time():
    1. 测量实际 tick 耗时, 慢帧检测       // 见下方 tick 性能监控
    2. game_clock_.tick(fixed_delta)       // 推进游戏时间 (固定步长 = 1/update_hertz)
    3. on_end_of_tick()                    // 子类钩子: 新 tick 值下, Updatable 之前
    4. on_start_of_tick()                  // 子类钩子: Updatable 执行前
    5. updatables_.call()                  // 按层级调用所有 Updatable
    6. on_tick_complete()                  // 子类钩子: tick 处理完毕
```

> **与 BigWorld 对齐:** `on_end_of_tick()` 在 `game_clock_.tick()` 之后调用（新 tick 值），
> 语义为"新 tick 开始前的清理工作"。子类在此钩子中调用 `game_time()` 获取的是新值。

> **关键决策: 固定步长 tick**
>
> BigWorld 用 `GameTime` (uint32 计数器) + `updateHertz` (默认10) 驱动固定步长。
> Atlas 用 `GameClock::tick(Duration)` 传入固定 delta (`1s / update_hertz`)，
> 同样实现确定性 tick，但保留了 `GameClock` 的时间缩放能力。

**Tick 性能监控:**

参照 BigWorld 的 `advanceTime()` 慢帧检测机制，每次 `advance_time()` 开始时测量实际 tick 耗时：

```cpp
void ServerApp::advance_time() {
    auto now = steady_clock::now();
    auto actual_duration = now - last_tick_time_;
    last_tick_time_ = now;

    auto expected = Duration(1s) / config_.update_hertz;

    // 慢帧检测: 实际耗时 > 2 倍期望 tick 间隔
    if (actual_duration > expected * 2) {
        ATLAS_LOG_WARNING("Tick took {:.1f}ms (expected {:.1f}ms)",
            duration_ms(actual_duration), duration_ms(expected));
        tick_stats_.slow_count++;
    }

    tick_stats_.last_duration = actual_duration;
    tick_stats_.max_duration = std::max(tick_stats_.max_duration, actual_duration);

    // ... 后续 tick 逻辑 ...
}
```

**ServerApp 注册的 tick 性能 Watcher:**

```cpp
w.add("tick/duration_ms", [this]{ return duration_ms(tick_stats_.last_duration); });
w.add("tick/max_duration_ms", [this]{ return duration_ms(tick_stats_.max_duration); });
w.add("tick/slow_count", tick_stats_.slow_count);
w.add("tick/total_count", [this]{ return game_time(); });
```

**信号处理:**

```cpp
// ServerApp::init() 中
install_signal_handler(Signal::Interrupt, [this](Signal s) { handle_signal(s); });
install_signal_handler(Signal::Terminate, [this](Signal s) { handle_signal(s); });

// ServerApp::handle_signal() — 在主线程通过 dispatch_pending_signals() 调用
void ServerApp::handle_signal(Signal sig) {
    on_signal(sig);  // 子类可覆盖
}

// 默认: SIGINT/SIGTERM → shutdown
void ServerApp::on_signal(Signal sig) {
    ATLAS_LOG_INFO("Received signal {}, shutting down...", static_cast<int>(sig));
    shutdown();
}

void ServerApp::shutdown() {
    shutdown_requested_ = true;
    dispatcher_.stop();
}
```

> **注意:** Atlas 已有 `dispatch_pending_signals()` 机制，但当前没有集成到 `EventDispatcher` 中。
> 需要将信号分发注册为 `FrequentTask`（参照 BigWorld 的 `SignalProcessor` 做法）。

**新增: SignalDispatchTask**

```cpp
// src/lib/server/signal_dispatch_task.hpp
namespace atlas {

/// FrequentTask 适配器: 每次 EventDispatcher tick 分发待处理信号
class SignalDispatchTask : public FrequentTask {
public:
    void do_task() override {
        dispatch_pending_signals();
    }
};

} // namespace atlas
```

在 `ServerApp::init()` 中将其注册到 dispatcher:
```cpp
dispatcher_.add_frequent_task(&signal_task_);
```

**测试用例:**
- `run_app()` 生命周期（init → run → fini 顺序）
- `advance_time()` 钩子调用顺序
- `shutdown()` 导致 `run()` 返回
- `Updatable` 在 tick 中被调用
- 配置加载集成

---

### Step 5.4: ScriptApp — C# 脚本集成层

**新增文件:**

```
src/lib/server/script_app.hpp
src/lib/server/script_app.cpp
tests/unit/test_script_app.cpp
```

**设计:**

```cpp
// src/lib/server/script_app.hpp
namespace atlas {

/// ServerApp + C# 脚本引擎
/// 替代 BigWorld 的 ScriptApp (Python) 层
class ScriptApp : public ServerApp {
public:
    ScriptApp(EventDispatcher& dispatcher, NetworkInterface& network);
    ~ScriptApp() override;

    /// 脚本引擎访问
    [[nodiscard]] auto script_engine() -> ScriptEngine& {
        ATLAS_ASSERT(script_engine_ != nullptr);
        return *script_engine_;
    }

protected:
    // ---- ServerApp 覆盖 ----

    [[nodiscard]] auto init(int argc, char* argv[]) -> bool override;
    void fini() override;

    /// tick 钩子: 驱动 C# on_tick
    void on_tick_complete() override;

    // ---- ScriptApp 子类覆盖点 ----

    /// 子类创建进程特有的 INativeApiProvider
    /// 在 ClrHost 初始化之前调用
    /// **约束: 实现中不可依赖任何 CLR 相关的东西 (CLR 尚未启动)**
    [[nodiscard]] virtual auto create_native_provider()
        -> std::unique_ptr<INativeApiProvider>;

    /// 脚本初始化完成后的回调 (子类可注册实体类型等)
    virtual void on_script_ready() {}

    // ---- 热重载 (与 Script Phase 5 集成) ----

    /// 重新加载脚本程序集 (触发 on_init(true))
    /// 在 Script Phase 5 热重载机制实现后完善
    void reload_scripts();

private:
    std::unique_ptr<ScriptEngine> script_engine_;
    std::unique_ptr<INativeApiProvider> native_provider_;
};

} // namespace atlas
```

**`ScriptApp::init()` 流程:**

```
ScriptApp::init(argc, argv):
    1. ServerApp::init(argc, argv)              // 基类初始化
    2. native_provider_ = create_native_provider()
    3. set_native_api_provider(native_provider_) // 注册到全局
    4. script_engine_ = std::make_unique<ClrScriptEngine>()
    5. script_engine_->initialize()             // 启动 CoreCLR
    6. script_engine_->load_module(config().script_assembly)
    7. script_engine_->on_init(false)           // 触发 C# OnInit
    8. on_script_ready()                        // 子类钩子
```

**`ScriptApp::on_tick_complete()` 流程:**

```
ScriptApp::on_tick_complete():
    script_engine_->on_tick(delta_time)
    // C# 侧的 Atlas.Runtime 在此 tick 中:
    //   - 处理待发送的 RPC
    //   - 更新 Entity 逻辑
    //   - 触发 C# 定时器
```

**`ScriptApp::fini()` 流程:**

```
ScriptApp::fini():
    script_engine_->on_shutdown()       // 触发 C# OnShutdown
    script_engine_->finalize()          // 关闭 CoreCLR
    script_engine_.reset()
    ServerApp::fini()
```

**`ScriptApp::reload_scripts()` 流程 (与 Script Phase 5 热重载集成):**

```
ScriptApp::reload_scripts():
    script_engine_->on_shutdown()       // 通知 C# 准备重载
    script_engine_->reload_module(config().script_assembly)
    script_engine_->on_init(true)       // is_reload = true
    on_script_ready()                   // 子类重新注册
```

> 此方法在 Script Phase 5 热重载机制完成后实现。当前阶段仅保留存根。

**与 BigWorld ScriptApp 的映射:**

| BigWorld ScriptApp | Atlas ScriptApp | 说明 |
|---|---|---|
| `initScript("base", path)` | `script_engine_->load_module(path)` | C# 程序集加载 |
| `initPersonality()` | 由 `Atlas.Runtime` 在 `on_init()` 中自处理 | personality 概念内化到 C# |
| `triggerOnInit(isReload)` | `script_engine_->on_init(is_reload)` | 已有接口 |
| `scriptEvents_` | 通过 `INativeApiProvider` 桥接 | C# EventBus 代替 |
| `PythonServer` (调试端口) | 不需要 | 用 `dotnet-monitor` |

---

### Step 5.5: EntityApp — 实体进程基类

**新增文件:**

```
src/lib/server/entity_app.hpp
src/lib/server/entity_app.cpp
tests/unit/test_entity_app.cpp
```

**设计:**

```cpp
// src/lib/server/entity_app.hpp
namespace atlas {

/// ScriptApp + 实体管理基础设施
/// BaseApp 和 CellApp 的直接基类
class EntityApp : public ScriptApp {
public:
    EntityApp(EventDispatcher& dispatcher, NetworkInterface& network);
    ~EntityApp() override;

    /// 后台任务管理器 (线程池 + 主线程回调)
    [[nodiscard]] auto bg_task_manager() -> BgTaskManager& { return bg_task_manager_; }

    /// 实体定义注册表 (具体实现在 Phase 8 完善, 此处声明接口)
    [[nodiscard]] auto entity_defs() -> EntityDefRegistry& { return entity_defs_; }

protected:
    [[nodiscard]] auto init(int argc, char* argv[]) -> bool override;
    void fini() override;

    /// 在 tick 中调用脚本定时器
    void on_start_of_tick() override;

    /// SIGQUIT 处理: 打印调用栈 (Manager 杀死无响应进程时触发)
    void on_signal(Signal sig) override;

    void register_watchers() override;

private:
    BgTaskManager bg_task_manager_;
    EntityDefRegistry entity_defs_;      // 实体类型定义 (Phase 8 填充)
};

} // namespace atlas
```

**EntityApp::on_signal() — SIGQUIT 调用栈 dump:**

参照 BigWorld，当 Manager 检测到进程无响应并发送 kill 信号时，EntityApp 捕获 SIGQUIT
打印调用栈信息，便于事后诊断：

```cpp
void EntityApp::on_signal(Signal sig) {
    if (sig == Signal::Quit) {
        ATLAS_LOG_CRITICAL("Received SIGQUIT — dumping call stack");
        print_stack_trace();  // platform 库提供
        // 不调用 shutdown()，让 Manager 处理后续
        return;
    }
    ScriptApp::on_signal(sig);  // 默认处理
}
```

**与 BigWorld EntityApp 的映射:**

| BigWorld EntityApp | Atlas EntityApp | 说明 |
|---|---|---|
| `BgTaskManager` | `BgTaskManager` (已有) | 直接复用 |
| `EntityAppTimeQueue` (ScriptTimers) | C# 侧通过 NativeApi 管理 | 定时器逻辑在 C# Source Generator 中 |
| `tickStats()` | Watcher 统计 | 通过 Watcher 上报 |
| `callTimers()` | `ScriptEngine::on_tick()` 中处理 | C# 侧 EventBus/Timer |
| `EntityType` 注册表 | `EntityDefRegistry` | 声明接口, Phase 8 实现 |
| SIGQUIT 调用栈 dump | `on_signal(Signal::Quit)` | 诊断无响应进程 |

> **简化:** BigWorld 的 `ScriptTimers` / `EntityAppTimeQueue` 是为了让 Python 脚本能注册定时器。
> Atlas 中 C# 侧的定时器由 `Atlas.Runtime` 管理，通过 `ScriptEngine::on_tick()` 统一驱动，
> 不需要 C++ 侧的 ScriptTimers 机制。

---

### Step 5.6: ManagerApp — 管理进程基类

**新增文件:**

```
src/lib/server/manager_app.hpp
src/lib/server/manager_app.cpp
```

**设计:**

```cpp
// src/lib/server/manager_app.hpp
namespace atlas {

/// 管理进程基类 (无脚本引擎)
/// BaseAppMgr, CellAppMgr, DBAppMgr, Reviver 的基类
class ManagerApp : public ServerApp {
public:
    ManagerApp(EventDispatcher& dispatcher, NetworkInterface& network);

protected:
    void register_watchers() override;
};

} // namespace atlas
```

管理进程不加载 C# 脚本，不需要 `ScriptEngine`。它们只处理集群管理消息。

---

### Step 5.7: Watcher — 运行时监控

**新增文件:**

```
src/lib/server/watcher.hpp
src/lib/server/watcher.cpp
tests/unit/test_watcher.cpp
```

**设计:**

BigWorld 的 Watcher 系统用宏 `MF_WATCH` 注册变量，支持层级路径、类型推导和读写模式。Atlas 用 C++20 模板替代宏。

```cpp
// src/lib/server/watcher.hpp
namespace atlas {

enum class WatcherMode : uint8_t {
    ReadOnly,
    ReadWrite,
};

/// Watcher 值类型
enum class WatcherType : uint8_t {
    Int64, UInt64, Double, Bool, String,
};

/// 单个可观察值
class WatcherEntry {
public:
    virtual ~WatcherEntry() = default;

    [[nodiscard]] virtual auto get_as_string() const -> std::string = 0;
    virtual auto set_from_string(std::string_view value) -> bool { return false; }

    [[nodiscard]] auto mode() const -> WatcherMode { return mode_; }
    [[nodiscard]] auto type() const -> WatcherType { return type_; }
    [[nodiscard]] auto description() const -> std::string_view { return desc_; }

protected:
    WatcherEntry(WatcherMode mode, WatcherType type, std::string_view desc)
        : mode_(mode), type_(type), desc_(desc) {}

private:
    WatcherMode mode_;
    WatcherType type_;
    std::string desc_;
};

/// 引用已有变量的 Watcher
template<typename T>
class DataWatcher : public WatcherEntry { /* ... */ };

/// 通过 getter/setter 函数的 Watcher
template<typename T>
class FunctionWatcher : public WatcherEntry { /* ... */ };

/// 通过成员方法的 Watcher
template<typename T, typename Obj>
class MemberWatcher : public WatcherEntry { /* ... */ };

/// Watcher 注册中心 (层级路径)
/// 归 ServerApp 所有 (成员变量), 非全局单例, 便于单元测试
class WatcherRegistry {
public:
    WatcherRegistry() = default;

    // Non-copyable, movable
    WatcherRegistry(const WatcherRegistry&) = delete;
    WatcherRegistry& operator=(const WatcherRegistry&) = delete;

    /// 注册变量引用
    template<typename T>
    void add(std::string_view path, const T& ref,
             std::string_view desc = "",
             WatcherMode mode = WatcherMode::ReadOnly);

    /// 注册 getter 函数
    template<typename T>
    void add(std::string_view path, std::function<T()> getter,
             std::string_view desc = "");

    /// 注册 getter/setter
    template<typename T>
    void add_rw(std::string_view path, T& ref,
                std::string_view desc = "");

    /// 查询
    [[nodiscard]] auto get(std::string_view path) const
        -> std::optional<std::string>;

    /// 设置 (仅 ReadWrite 模式)
    auto set(std::string_view path, std::string_view value) -> bool;

    /// 列出指定路径下的子项
    [[nodiscard]] auto list(std::string_view prefix = "") const
        -> std::vector<std::string>;

    /// 导出所有 watcher 的当前值 (用于远程查询)
    [[nodiscard]] auto snapshot() const
        -> std::vector<std::pair<std::string, std::string>>;

private:
    struct Node {
        std::unique_ptr<WatcherEntry> entry;  // null for directory nodes
        std::map<std::string, Node, std::less<>> children;
    };
    Node root_;
};

} // namespace atlas
```

> **设计决策: WatcherRegistry 归 ServerApp 所有，非全局单例。**
>
> BigWorld 的 Watcher 是全局的，导致单元测试困难（需在测试间清理状态）。
> Atlas 改为 `ServerApp` 持有 `WatcherRegistry` 成员，通过 `app.watcher()` 访问。
> 如果消息处理器等上下文需要访问，可通过 `NetworkInterface::extension_data()` 回溯到 app 实例。

**ServerApp 默认注册的 Watcher:**

```cpp
void ServerApp::register_watchers() {
    auto& w = watcher_registry_;
    w.add("app/type", [this]{ return process_type_name(config_.process_type); });
    w.add("app/name", config_.process_name);
    w.add("app/pid", []{ return get_current_pid(); });
    w.add("app/uptime_seconds", [this]{ return uptime_seconds(); });
    w.add("app/game_time", [this]{ return game_time(); });
    w.add("app/update_hertz", config_.update_hertz);

    // tick 性能指标
    w.add("tick/duration_ms", [this]{ return duration_ms(tick_stats_.last_duration); });
    w.add("tick/max_duration_ms", [this]{ return duration_ms(tick_stats_.max_duration); });
    w.add("tick/slow_count", tick_stats_.slow_count);

    // ServerAppOption 统一注册
    for (auto* opt : ServerAppOption<void>::all_options()) {
        opt->register_watcher(w);
    }
}
```

**测试用例:**
- 注册和查询各种类型
- 层级路径 (如 `network/bytes_sent`, `network/bytes_received`)
- ReadWrite 模式的 set
- `list()` 子树枚举
- `snapshot()` 全量导出

---

### Step 5.8: 进程间消息接口约定

Atlas 已有完善的消息框架（`NetworkMessage` concept + `InterfaceTable` + `TypedMessageHandler`），**不需要** BigWorld 的宏系统。只需建立约定。

**新增文件:**

```
src/lib/server/common_messages.hpp
```

**设计:**

定义所有进程共用的消息（心跳、广播等）。进程特有消息在各 `src/server/XxxApp/` 中定义。

```cpp
// src/lib/server/common_messages.hpp
namespace atlas::msg {

/// 心跳消息 (所有进程 → machined)
struct Heartbeat {
    uint64_t game_time;
    float load;       // 0.0 ~ 1.0 负载

    static auto descriptor() -> const MessageDesc& {
        static const MessageDesc desc{
            100, "Heartbeat", MessageLengthStyle::Fixed,
            sizeof(uint64_t) + sizeof(float)};
        return desc;
    }

    void serialize(BinaryWriter& w) const {
        w.write(game_time);
        w.write(load);
    }

    static auto deserialize(BinaryReader& r) -> Result<Heartbeat> {
        auto gt = r.read<uint64_t>();
        if (!gt) return gt.error();
        auto ld = r.read<float>();
        if (!ld) return ld.error();
        return Heartbeat{*gt, *ld};
    }
};
static_assert(NetworkMessage<Heartbeat>);

/// 关闭通知 (Manager → 各进程)
struct ShutdownRequest {
    uint8_t reason;   // 0=normal, 1=maintenance, 2=emergency

    static auto descriptor() -> const MessageDesc& {
        static const MessageDesc desc{
            101, "ShutdownRequest", MessageLengthStyle::Fixed,
            sizeof(uint8_t)};
        return desc;
    }

    void serialize(BinaryWriter& w) const { w.write(reason); }

    static auto deserialize(BinaryReader& r) -> Result<ShutdownRequest> {
        auto reason = r.read<uint8_t>();
        if (!reason) return reason.error();
        return ShutdownRequest{*reason};
    }
};
static_assert(NetworkMessage<ShutdownRequest>);

} // namespace atlas::msg
```

**消息 ID 分配规范:**

| 范围 | 用途 |
|------|------|
| 0 – 99 | 保留 (未来) |
| 100 – 199 | 公共消息 (Heartbeat, Shutdown, etc.) |
| 1000 – 1099 | machined 消息 (已有 Register/Deregister/Query) |
| 2000 – 2999 | BaseApp 内部接口 |
| 3000 – 3999 | CellApp 内部接口 |
| 4000 – 4999 | DBApp 内部接口 |
| 5000 – 5999 | LoginApp 接口 |
| 6000 – 6999 | BaseAppMgr 接口 |
| 7000 – 7999 | CellAppMgr 接口 |
| 8000 – 8999 | DBAppMgr 接口 |
| 10000 – 19999 | 外部接口 (客户端 ↔ 服务器，保留给完整客户端协议；当前代码的 BaseApp 认证消息暂时仍放在 `2000–2999`) |
| 50000 – 59999 | C# RPC 转发 (脚本层 RPC 封装在统一消息中) |

> **与 BigWorld 的差异:**
> BigWorld 每个进程类型有独立的 InterfaceMinder，消息 ID 从 0 开始递增（进程内唯一）。
> Atlas 使用全局唯一 MessageID (uint16_t)，简化路由（不需要知道消息来自哪个接口）。

> **重要: C# RPC 不应每个方法占一个 MessageID。**
> 应使用少量"RPC 转发"消息（如 ID=50000 用于 Base→Cell RPC, 50001 用于 Cell→Client 等），
> 消息体内携带 EntityDef 方法索引 + 序列化参数。C# Source Generator 负责编/解码。
> 这样 RPC 数量不受 MessageID 空间限制。

---

### Step 5.9: CMakeLists.txt 构建配置

**更新文件:**

```
src/lib/server/CMakeLists.txt
tests/unit/CMakeLists.txt (追加测试)
```

```cmake
# src/lib/server/CMakeLists.txt
atlas_add_library(atlas_server
    server_config.cpp
    server_app.cpp
    script_app.cpp
    entity_app.cpp
    manager_app.cpp
    updatable.cpp
    watcher.cpp
    signal_dispatch_task.cpp
)

target_link_libraries(atlas_server
    PUBLIC
        atlas_foundation
        atlas_network
        atlas_platform
        atlas_serialization
        atlas_script
        atlas_clrscript
)
```

---

### Step 5.10: 最小示例进程 (验证框架)

**新增文件:**

```
src/server/EchoApp/echo_app.hpp
src/server/EchoApp/echo_app.cpp
src/server/EchoApp/main.cpp
src/server/EchoApp/CMakeLists.txt
```

一个最小的服务器进程，验证整个框架可用。

```cpp
// src/server/EchoApp/echo_app.hpp
namespace atlas {

class EchoApp : public ManagerApp {
public:
    using ManagerApp::ManagerApp;

protected:
    auto init(int argc, char* argv[]) -> bool override;
    void on_tick_complete() override;
    void fini() override;

private:
    uint64_t tick_count_ = 0;
};

} // namespace atlas
```

```cpp
// src/server/EchoApp/main.cpp
int main(int argc, char* argv[]) {
    atlas::EventDispatcher dispatcher("echo");
    atlas::NetworkInterface network(dispatcher);
    atlas::EchoApp app(dispatcher, network);
    return app.run_app(argc, argv);
}
```

验证清单:
- [ ] 进程启动，打印启动日志
- [ ] tick 定时器按 `update_hertz` 频率触发
- [ ] Ctrl+C 信号 → 优雅关闭
- [ ] Watcher 可查询 `app/uptime_seconds`
- [ ] 配置从命令行和 JSON 加载

---

## 3. 文件清单汇总

```
src/lib/server/
├── CMakeLists.txt              (更新)
├── server_config.hpp / .cpp     (Step 5.1)
├── server_app_option.hpp        (Step 5.1, 模板 header-only)
├── updatable.hpp / .cpp         (Step 5.2)
├── server_app.hpp / .cpp        (Step 5.3)
├── signal_dispatch_task.hpp     (Step 5.3)
├── script_app.hpp / .cpp        (Step 5.4)
├── entity_app.hpp / .cpp        (Step 5.5)
├── manager_app.hpp / .cpp       (Step 5.6)
├── watcher.hpp / .cpp           (Step 5.7)
└── common_messages.hpp          (Step 5.8)

src/server/EchoApp/              (Step 5.10)
├── CMakeLists.txt
├── echo_app.hpp / .cpp
└── main.cpp

tests/unit/
├── test_server_config.cpp       (Step 5.1)
├── test_updatable.cpp           (Step 5.2)
├── test_server_app.cpp          (Step 5.3)
├── test_script_app.cpp          (Step 5.4)
├── test_entity_app.cpp          (Step 5.5)
└── test_watcher.cpp             (Step 5.7)
```

---

## 4. 依赖关系与实现顺序

```
Step 5.1: ServerConfig         ← 无依赖, 可最先开始
Step 5.2: Updatable            ← 无依赖, 可并行
     │
     ▼
Step 5.3: ServerApp            ← 依赖 5.1 + 5.2
     │
     ├── Step 5.6: ManagerApp  ← 直接继承 ServerApp, 简单
     │
     ▼
Step 5.4: ScriptApp            ← 依赖 5.3 + clrscript
     │
     ▼
Step 5.5: EntityApp            ← 依赖 5.4
     │
Step 5.7: Watcher              ← 无依赖, 可与 5.1/5.2 并行, 但在 5.3 中集成
Step 5.8: common_messages      ← 无依赖, 消息定义
Step 5.9: CMakeLists           ← 最后统一
Step 5.10: EchoApp             ← 依赖全部, 端到端验证
```

**推荐执行顺序:**

```
第 1 轮 (并行): 5.1 ServerConfig + 5.2 Updatable + 5.7 Watcher
第 2 轮:        5.3 ServerApp + 5.8 common_messages
第 3 轮:        5.4 ScriptApp + 5.6 ManagerApp
第 4 轮:        5.5 EntityApp
第 5 轮:        5.9 CMake + 5.10 EchoApp (端到端验证)
```

---

## 5. BigWorld 完整对照

| BigWorld | Atlas | 差异说明 |
|----------|-------|---------|
| `ServerApp` + 构造函数接收 `dispatcher` & `interface` | `ServerApp` 相同模式 | 一致 |
| `runApp(argc, argv)` → `init() → run() → fini()` | `run_app(argc, argv)` 相同流程 | 一致 |
| `advanceTime()` — ++time → end → start → updatables → complete | `advance_time()` 对齐 BigWorld 6 步 | 一致 (含慢帧检测) |
| `Updatable` / `Updatables` 分层级 tick | `Updatable` / `Updatables` | 一致 |
| `GameTime` (uint32) + `updateHertz` | `GameClock` (已有, 支持时间缩放) | Atlas 更灵活 |
| `SignalProcessor` 作为 `FrequentTask` | `SignalDispatchTask` | 一致模式 |
| `ServerAppConfig` + `ServerAppOption` | `ServerConfig` + `ServerAppOption<T>` | Atlas 用 JSON + 模板 |
| `ScriptApp` — Python 解释器生命周期 | `ScriptApp` — ClrHost 生命周期 | C# 替代 Python |
| `ScriptApp::initScript(componentName, path)` | `ScriptEngine::load_module(path)` | 已有接口 |
| `ScriptApp::triggerOnInit(isReload)` | `ScriptEngine::on_init(is_reload)` | 已有接口, 含 reload 存根 |
| `ScriptApp::PythonServer` | 不需要 | .NET 有原生诊断工具 |
| `EntityApp::BgTaskManager` | `BgTaskManager` (已有) | 直接复用 |
| `EntityApp::ScriptTimers` / `ScriptTimeQueue` | C# 侧管理 | 不需要 C++ 侧等价物 |
| `EntityApp` SIGQUIT dump stack | `on_signal(Signal::Quit)` | 诊断无响应进程 |
| `ManagerApp` (无脚本) | `ManagerApp` (无脚本) | 一致 |
| Mercury Interface 宏 (`BEGIN_MERCURY_INTERFACE`) | `NetworkMessage` concept + `InterfaceTable` | Atlas 用 C++20 concept，已有，无需宏 |
| `MF_WATCH` 宏 (全局) | `WatcherRegistry` (ServerApp 成员) | Atlas 非全局, 利于测试 |
| `Watcher` 远程查询协议 | 后续 Phase 6 (machined) 集成 | 分阶段实现 |
| `interface.pExtensionData()` 回溯 app | `network_.set_extension_data(this)` | 一致模式 |
| 提升 fd 上限 (`setrlimit`) | `raise_fd_limit()` | 一致 |

---

## 6. 关键设计决策记录

### 6.1 EventDispatcher 和 NetworkInterface 的所有权

**BigWorld 模式:** `ServerApp` 构造函数接收外部创建的 `dispatcher` 和 `interface` 引用。`main()` 中创建它们，传给 ServerApp。

**Atlas 沿用此模式:** 这样做的好处是测试中可以注入 mock 的 dispatcher/network。

```cpp
// main.cpp 模式:
int main(int argc, char* argv[]) {
    EventDispatcher dispatcher("baseapp");
    NetworkInterface network(dispatcher);
    BaseApp app(dispatcher, network);
    return app.run_app(argc, argv);
}
```

### 6.2 tick 驱动方式

**BigWorld:** `run()` 调用 `dispatcher.processUntilBreak()`，`advanceTime()` 是一个注册在 dispatcher 上的定时器回调。

**Atlas 相同:** `advance_time()` 注册为 `dispatcher.add_repeating_timer(tick_interval, ...)`。EventDispatcher 的 `run()` 驱动一切。

### 6.3 脚本定时器

**BigWorld:** `EntityApp` 有 `ScriptTimers` / `ScriptTimeQueue`，允许 Python 注册定时器回调。定时器触发时调用 Python 函数。

**Atlas 简化:** C# 侧在 `Atlas.Runtime` 中自行管理定时器列表，每次 `ScriptEngine::on_tick(dt)` 时遍历并触发。不需要 C++ 侧暴露定时器注册 API。如果后续需要精确定时器，可通过 `INativeApiProvider` 新增 `add_timer` / `cancel_timer` 方法。

### 6.4 消息 ID 全局唯一 vs 进程内唯一

**BigWorld:** 每个进程类型有独立的 `InterfaceMinder`，消息 ID 从 0 开始。收到消息时，根据来源进程类型决定使用哪张接口表。

**Atlas:** 使用全局唯一 `MessageID` (uint16_t, 0-65535)。按范围分配给不同进程类型。优势是路由简单，不需要额外的接口标识。代价是 ID 空间有限，但 65535 对游戏服务器绰绰有余。

### 6.5 INativeApiProvider 的生命周期

`ScriptApp` 创建 provider，在 ClrHost 初始化前注册，在 fini() 中随 ScriptEngine 一起清理。每个进程类型（BaseApp, CellApp 等）提供自己的 Provider 子类，覆盖自己支持的方法。

```
ScriptApp::init()
  → create_native_provider()     // 子类覆盖
  → set_native_api_provider(...)
  → ClrHost::initialize()

ScriptApp::fini()
  → ClrHost::finalize()
  → set_native_api_provider(nullptr)
```

**约束:** `create_native_provider()` 实现中不可依赖任何 CLR 相关内容（CLR 尚未启动）。

### 6.6 WatcherRegistry 所有权

**决策: WatcherRegistry 归 ServerApp 所有（成员变量），非全局单例。**

BigWorld 的 Watcher 是全局的，导致单元测试困难（需在测试间清理状态），且无法在同一进程内运行多个 ServerApp 实例。Atlas 将 `WatcherRegistry` 作为 `ServerApp` 的成员，通过 `app.watcher()` 访问。消息处理器中可通过 `NetworkInterface::extension_data()` 回溯到 app 实例。

### 6.7 NetworkInterface 端口绑定时序

**决策: 构造时不绑定端口，`init()` 中绑定。**

`main()` 中创建 `NetworkInterface` 时仅创建 socket，不执行 `bind()`。端口绑定推迟到 `ServerApp::init()` 中，此时 `ServerConfig` 已解析，可从配置中获取端口号。BigWorld 也是此模式。

### 6.8 子类配置扩展

**决策: `ServerAppOption<T>` 模板。**

类似 BigWorld 的 `ServerAppOption`，子类通过声明静态模板实例自动从 JSON 加载并注册 Watcher。见 Step 5.1 中的详细设计。`ServerConfig` 需要持有原始 JSON 对象供选项提取。
