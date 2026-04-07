# ScriptPhase 5: 热重载机制

> 预估周期: 2–3 周 | 前置依赖: ScriptPhase 3 完成 (ScriptPhase 4 可并行)

---

## 目标

开发期修改 C# 游戏脚本后，无需重启服务端进程即可生效。仅用户脚本程序集可热重载，引擎运行时和共享库不可热重载。

## 验收标准 (M5)

- [ ] 修改 C# 脚本 → 自动编译 → 旧程序集卸载 → 新程序集加载 → 实体状态恢复
- [ ] 热重载过程中无内存泄漏（GCHandle 全部释放，旧 Context 被 GC 回收）
- [ ] 重载失败（编译错误）不影响服务端运行，继续使用旧版本
- [ ] 仅在开发/调试模式启用，生产环境可关闭

---

## 架构设计

### AssemblyLoadContext 隔离模型

```
┌─────────────────────────────────────────────────────────┐
│ Default AssemblyLoadContext (不可卸载)                    │
│                                                          │
│   Atlas.Runtime.dll        ← 引擎运行时，进程生命周期    │
│   Atlas.Shared.dll         ← 共享定义，进程生命周期      │
│   Atlas.Generators.*.dll   ← (仅编译期使用，运行时不加载) │
│   System.*.dll             ← BCL                        │
│                                                          │
├──────────────────────────────────────────────────────────┤
│ ScriptLoadContext v1 (isCollectible: true)  ← 可卸载     │
│                                                          │
│   Atlas.GameScripts.dll    ← 用户游戏脚本               │
│                                                          │
│   ┌─── 卸载 ──────────────────────────────────┐         │
│   │  GCHandle 释放 → Unload() → GC → 回收     │         │
│   └─────────────────────────────────────────────┘        │
│                                                          │
├──────────────────────────────────────────────────────────┤
│ ScriptLoadContext v2 (isCollectible: true)  ← 新加载     │
│                                                          │
│   Atlas.GameScripts.dll    ← 重新编译后的新版本          │
│                                                          │
└──────────────────────────────────────────────────────────┘
```

### 重载时序图

```
[文件变更] ──→ [debounce 500ms] ──→ [dotnet build]
                                         │
                                    编译成功？
                                    ├── 否 → 日志告警，继续使用旧版本
                                    │
                                    ▼ 是
                              [暂停游戏 Tick]
                                    │
                              [序列化实体状态]
                              (使用 ISerializable / Source Generator 生成的代码)
                                    │
                              [释放所有 GCHandle]
                              (C++ ClrObject 析构 → GCHandle.Free)
                                    │
                              [清空函数指针缓存]
                              (ClrStaticMethod.reset())
                                    │
                              [清除 EntityDefRegistry]
                              (unregister_all — C# 重新注册后恢复)
                                    │
                              [ScriptLoadContext.Unload()]
                                    │
                              [GC.Collect() + 等待回收]
                              (验证 WeakReference 已失效)
                                    │
                              [创建新 ScriptLoadContext]
                                    │
                              [加载新程序集]
                                    │
                              [重新绑定函数指针]
                                    │
                              [反序列化实体状态]
                                    │
                              [恢复游戏 Tick]
```

---

## 任务 5.1: `ScriptLoadContext` 实现

### C# 侧: `src/csharp/Atlas.Runtime/Hosting/ScriptLoadContext.cs`

```csharp
namespace Atlas.Hosting;

internal sealed class ScriptLoadContext : AssemblyLoadContext
{
    private readonly string _scriptDirectory;

    public ScriptLoadContext(string scriptDirectory)
        : base(isCollectible: true)
    {
        _scriptDirectory = scriptDirectory;
    }

    protected override Assembly? Load(AssemblyName assemblyName)
    {
        // 用户脚本从脚本目录加载
        var path = Path.Combine(_scriptDirectory,
            $"{assemblyName.Name}.dll");
        if (File.Exists(path))
            return LoadFromAssemblyPath(path);

        // Atlas.Runtime, Atlas.Shared 等从 Default Context 加载
        return null;
    }
}
```

### C# 侧: `src/csharp/Atlas.Runtime/Hosting/ScriptHost.cs`

```csharp
namespace Atlas.Hosting;

internal sealed class ScriptHost : IDisposable
{
    private ScriptLoadContext? _context;
    private WeakReference? _contextRef;
    private Assembly? _scriptAssembly;

    public bool IsLoaded => _context != null;

    public void Load(string assemblyPath)
    {
        var dir = Path.GetDirectoryName(assemblyPath)!;
        _context = new ScriptLoadContext(dir);
        _scriptAssembly = _context.LoadFromAssemblyPath(
            Path.GetFullPath(assemblyPath));
        _contextRef = new WeakReference(_context);
    }

    public void Unload(TimeSpan timeout)
    {
        if (_context == null) return;

        _scriptAssembly = null;
        _context.Unload();
        _context = null;

        // 等待 GC 回收
        var deadline = DateTime.UtcNow + timeout;
        while (_contextRef!.IsAlive && DateTime.UtcNow < deadline)
        {
            GC.Collect();
            GC.WaitForPendingFinalizers();
            Thread.Sleep(100);
        }

        if (_contextRef.IsAlive)
        {
            Atlas.Log.Warning(
                "ScriptLoadContext was not collected within timeout. " +
                "Possible GCHandle leak.");
        }
    }

    public void Dispose()
    {
        Unload(TimeSpan.FromSeconds(5));
    }
}
```

### 工作点

- [ ] `isCollectible: true` 使 Context 可被 GC 回收
- [ ] `Load()` 重写: 仅用户脚本在此 Context 加载，系统/引擎库回落到 Default
- [ ] `Unload()`: 调用 `AssemblyLoadContext.Unload()` → 触发 GC → 验证回收
- [ ] 超时保护: 5 秒内未回收则告警（不阻塞）

---

## 任务 5.2: C++ 侧热重载管理器

### 新建文件: `src/lib/clrscript/clr_hot_reload.hpp`, `clr_hot_reload.cpp`

```cpp
namespace atlas
{

class ClrHotReload
{
public:
    struct Config
    {
        std::filesystem::path script_directory;    // C# 脚本源码目录
        std::filesystem::path output_directory;    // 编译输出目录
        Duration debounce_delay{Milliseconds(500)};
        Duration unload_timeout{Seconds(5)};
        bool auto_compile{true};                   // 文件变更自动编译
    };

    explicit ClrHotReload(ClrScriptEngine& engine);

    [[nodiscard]] auto configure(const Config& config) -> Result<void>;

    // 手动触发重载
    [[nodiscard]] auto reload() -> Result<void>;

    // 检查是否有待处理的重载（由文件监控触发）
    [[nodiscard]] auto has_pending_reload() const -> bool;

    // 在主循环安全点调用（两帧之间）
    [[nodiscard]] auto process_pending_reload() -> Result<void>;

private:
    ClrScriptEngine& engine_;
    Config config_;
    std::atomic<bool> pending_reload_{false};

    [[nodiscard]] auto compile_scripts() -> Result<void>;
    [[nodiscard]] auto do_reload() -> Result<void>;
};

} // namespace atlas
```

### `reload()` 流程实现

```cpp
auto ClrHotReload::do_reload() -> Result<void>
{
    ATLAS_LOG_INFO("Hot reload: starting...");

    // 1. 编译
    if (config_.auto_compile)
    {
        auto compile_result = compile_scripts();
        if (!compile_result)
        {
            ATLAS_LOG_ERROR("Hot reload: compile failed: {}",
                            compile_result.error().message());
            return compile_result.error();
        }
    }

    // 2. 通知 C# 侧序列化状态并卸载
    auto serialize_and_unload = engine_.call_managed("SerializeAndUnload");
    if (!serialize_and_unload) return serialize_and_unload.error();

    // 3. C++ 侧释放所有 ClrObject（GCHandle）
    engine_.release_all_script_objects();

    // 4. 清空函数指针缓存
    engine_.reset_script_method_cache();

    // 4.5. 清除 C++ 侧实体类型注册表（热重载后由 C# 重新注册）
    EntityDefRegistry::instance().unregister_all();

    // 5. 通知 C# 侧加载新程序集并恢复状态
    // C# Bootstrap 初始化时会调用 EntityTypeRegistry.RegisterAll()
    // 重新将所有 [Entity] 类型注册到 C++ EntityDefRegistry
    auto assembly_path = config_.output_directory / "Atlas.GameScripts.dll";
    auto load_and_restore = engine_.call_managed(
        "LoadAndRestore", assembly_path);
    if (!load_and_restore)
    {
        // 加载失败: 回滚到备份版本
        ATLAS_LOG_ERROR("Hot reload: load failed, rolling back: {}",
                        load_and_restore.error().message());
        auto backup_dir = config_.output_directory / ".reload_backup";
        if (std::filesystem::exists(backup_dir)
            && !std::filesystem::is_empty(backup_dir))
        {
            for (auto& entry : std::filesystem::directory_iterator(backup_dir))
            {
                auto dest = config_.output_directory / entry.path().filename();
                std::filesystem::copy(entry.path(), dest,
                    std::filesystem::copy_options::overwrite_existing);
            }
            // 尝试用旧版本恢复
            auto rollback = engine_.call_managed(
                "LoadAndRestore", assembly_path);
            if (!rollback)
            {
                ATLAS_LOG_CRITICAL("Hot reload: rollback also failed: {}",
                                   rollback.error().message());
            }
        }
        else
        {
            // 首次热重载（无备份）或备份为空 — 无法回滚
            ATLAS_LOG_CRITICAL(
                "Hot reload: no backup available for rollback. "
                "Server will continue without script assembly loaded.");
        }
        return load_and_restore.error();
    }

    // 6. 重新绑定函数指针
    engine_.rebind_script_methods();

    ATLAS_LOG_INFO("Hot reload: complete");
    return Result<void>{};
}
```

### `compile_scripts()` 实现

```cpp
auto ClrHotReload::compile_scripts() -> Result<void>
{
    // 编译到临时目录，避免编译失败时覆盖正在使用的 DLL
    auto temp_dir = config_.output_directory / ".reload_staging";
    std::filesystem::create_directories(temp_dir);

    auto cmd = std::format("dotnet build \"{}\" -c Debug -o \"{}\" --nologo -v q",
                           config_.script_directory.string(),
                           temp_dir.string());

    int exit_code = std::system(cmd.c_str());
    if (exit_code != 0)
    {
        // 编译失败，清理临时目录，继续使用旧版本
        std::filesystem::remove_all(temp_dir);
        return Error{ErrorCode::ScriptError,
            std::format("dotnet build failed with exit code {}", exit_code)};
    }

    // 编译成功，备份旧 DLL 并替换
    auto backup_dir = config_.output_directory / ".reload_backup";
    std::filesystem::create_directories(backup_dir);

    for (auto& entry : std::filesystem::directory_iterator(config_.output_directory))
    {
        if (entry.path().extension() == ".dll" || entry.path().extension() == ".pdb")
        {
            auto dest = backup_dir / entry.path().filename();
            std::filesystem::copy(entry.path(), dest,
                std::filesystem::copy_options::overwrite_existing);
        }
    }

    // 将新编译结果移入正式目录
    for (auto& entry : std::filesystem::directory_iterator(temp_dir))
    {
        auto dest = config_.output_directory / entry.path().filename();
        std::filesystem::copy(entry.path(), dest,
            std::filesystem::copy_options::overwrite_existing);
    }
    std::filesystem::remove_all(temp_dir);

    return Result<void>{};
}
```

**关键设计**: 编译输出到 `.reload_staging/` 临时目录，编译成功后才替换正式目录。旧版本备份到 `.reload_backup/`，加载失败时可回滚。

### 工作点

- [ ] `compile_scripts()`: 编译到临时目录 `.reload_staging/`，成功后替换正式目录，旧版备份到 `.reload_backup/`
- [ ] `do_reload()`: 序列化→卸载→加载→恢复完整流程
- [ ] `release_all_script_objects()`: 遍历并析构 C++ 侧所有 `ClrObject`
- [ ] `reset_script_method_cache()`: 清空所有 `ClrStaticMethod` 缓存
- [ ] `EntityDefRegistry::unregister_all()`: 清除 C++ 侧实体类型注册表。C# 新程序集加载后 `Bootstrap.Initialize()` 会调用 `EntityTypeRegistry.RegisterAll()` 重新注册
- [ ] `rebind_script_methods()`: 重新绑定到新程序集的入口方法
- [ ] `process_pending_reload()`: 在主循环安全点（两帧之间）检查并执行
- [ ] 加载失败回滚: 从 `.reload_backup/` 恢复旧 DLL 并重新加载

---

## 任务 5.3: 文件监控

### 方案选择

| 方案 | 优点 | 缺点 |
|------|------|------|
| C# `FileSystemWatcher` | 简单，C# 原生 | 跨边界调用通知 |
| C++ `ReadDirectoryChangesW` (Win) / `inotify` (Linux) | 已有 platform 层 | 需要平台适配 |
| 轮询 (stat 检查时间戳) | 最简单，跨平台 | CPU 开销，延迟高 |

**推荐**: C++ 侧轮询方案（初期实现简单可靠，后续可升级为 OS 原生通知）。

```cpp
class FileWatcher
{
public:
    explicit FileWatcher(const std::filesystem::path& directory);

    // 检查是否有文件变更（比较 last_write_time）
    [[nodiscard]] auto check_changes() -> bool;

private:
    std::filesystem::path directory_;
    std::unordered_map<std::string, std::filesystem::file_time_type> timestamps_;
};
```

### 工作点

- [ ] 初始扫描: 记录所有 `.cs` 文件的 `last_write_time`
- [ ] `check_changes()`: 重新扫描，比较时间戳
- [ ] Debounce: 检测到变更后等待 500ms 再触发（避免连续保存多个文件时多次触发）
- [ ] 排除: `bin/`, `obj/`, `.git/`, `.reload_staging/`, `.reload_backup/` 目录

---

## 任务 5.4: 状态迁移

### 序列化/反序列化策略

实体状态通过 Source Generator 生成的 `ISerializable` 实现进行序列化：

```
旧实体 ─── Serialize() ──→ byte[] (内存缓冲区)
                                    │
                              卸载旧 Context
                              加载新 Context
                                    │
新实体 ←── Deserialize() ─── byte[] (内存缓冲区)
```

### C# 侧: `src/csharp/Atlas.Runtime/Hosting/HotReloadManager.cs`

```csharp
namespace Atlas.Hosting;

internal static class HotReloadManager
{
    private static byte[]? _stateSnapshot;
    private static ScriptHost? _host;

    [UnmanagedCallersOnly]
    public static int SerializeAndUnload()
    {
        try
        {
            // 序列化所有实体状态
            var writer = new SpanWriter(64 * 1024);
            try
            {
                var entities = EntityManager.Instance.GetAllEntities();
                writer.WriteInt32(entities.Count);
                foreach (var entity in entities)
                {
                    // 使用 Source Generator 生成的 TypeName 常量，避免反射
                    // entity.GetType().Name 在 IL2CPP 下虽可用但与零反射原则矛盾
                    writer.WriteString(entity.TypeName);  // Source Generator 生成的常量属性
                    writer.WriteUInt32(entity.EntityId);
                    entity.Serialize(ref writer);          // Source Generator 生成
                }
                _stateSnapshot = writer.WrittenSpan.ToArray();
            }
            finally { writer.Dispose(); }

            // 卸载
            EntityManager.Instance.Clear();
            _host?.Unload(TimeSpan.FromSeconds(5));

            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    [UnmanagedCallersOnly]
    public static unsafe int LoadAndRestore(byte* pathUtf8, int pathLen)
    {
        try
        {
            var path = Encoding.UTF8.GetString(
                new ReadOnlySpan<byte>(pathUtf8, pathLen));
            _host = new ScriptHost();
            _host.Load(path);

            // 恢复实体状态
            if (_stateSnapshot != null)
            {
                var reader = new SpanReader(_stateSnapshot);
                var count = reader.ReadInt32();
                for (int i = 0; i < count; i++)
                {
                    var typeName = reader.ReadString();
                    var entityId = reader.ReadUInt32();
                    var entity = EntityFactory.Create(typeName);
                    if (entity != null)
                    {
                        entity.EntityId = entityId;
                        entity.Deserialize(ref reader);
                        EntityManager.Instance.Register(entity);
                    }
                }
                _stateSnapshot = null;
            }

            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }
}
```

### 类型结构变更处理

| 变更类型 | 处理方式 |
|---------|---------|
| 新增属性 | 反序列化时使用默认值（版本号机制） |
| 删除属性 | 反序列化时跳过多余字节（版本号机制） |
| 属性类型变更 | 反序列化失败 → 实体以默认状态重建 → 日志告警 |
| 新增实体类型 | 无影响（工厂自动包含新类型） |
| 删除实体类型 | 对应实体丢失 → 日志告警 |

### 工作点

- [ ] Source Generator 生成的 `Serialize`/`Deserialize` 包含版本号
- [ ] 版本号不匹配时优雅降级而非崩溃
- [ ] `_stateSnapshot` 临时存储在托管堆（不跨 Context）
- [ ] 失败回滚: 恢复失败时重新加载旧程序集（如果可用）

---

## 任务 5.5: 安全卸载保证

### GCHandle 注册表

```cpp
// C++ 侧维护所有活跃的 ClrObject
class ClrObjectRegistry
{
public:
    void register_object(ClrObject* obj);
    void unregister_object(ClrObject* obj);

    // 热重载前调用: 释放所有注册的 ClrObject
    void release_all();

    // Debug: 当前存活数量
    [[nodiscard]] auto active_count() const -> size_t;

private:
    std::unordered_set<ClrObject*> objects_;
    std::mutex mutex_;
};
```

### 卸载验证

```csharp
// C# 侧验证 Context 确实被回收
internal static class UnloadVerifier
{
    public static bool WaitForUnload(WeakReference contextRef,
                                     TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (contextRef.IsAlive && DateTime.UtcNow < deadline)
        {
            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();
            Thread.Sleep(100);
        }
        return !contextRef.IsAlive;
    }
}
```

### 工作点

- [ ] `ClrObjectRegistry`: 追踪所有 C++ 侧的 `ClrObject` 实例
- [ ] `release_all()`: 热重载前遍历并析构所有 ClrObject → GCHandle.Free
- [ ] `WeakReference` 验证: 卸载后 Context 必须被 GC 回收
- [ ] 超时告警: 5 秒未回收说明存在 GCHandle 泄漏，输出详细诊断信息
- [ ] Debug 模式: 在 `ClrObject` 构造函数中记录创建调用栈，泄漏时可定位

---

## 任务 5.6: 集成测试

| 测试文件 | 核心用例 |
|---------|---------|
| `test_hot_reload.cpp` | 加载脚本 → 调用 → 修改源码 → 重载 → 调用新逻辑 → 验证 |
| `test_hot_reload.cpp` | 重载期间创建的实体正确迁移 |
| `test_hot_reload.cpp` | 编译失败 → 继续使用旧版本 |
| `test_hot_reload.cpp` | 连续快速重载（debounce 验证） |
| `test_hot_reload.cpp` | 重载后 GCHandle 泄漏计数为 0 |
| `test_hot_reload.cpp` | 实体类型新增属性 → 旧实体用默认值恢复 |

---

## 风险与缓解

| 风险 | 概率 | 影响 | 缓解措施 |
|------|------|------|---------|
| GCHandle 泄漏导致 Context 无法回收 | 高 | 高 | `ClrObjectRegistry` 强制释放 + Debug 追踪 |
| 状态迁移失败（类型结构剧变） | 中 | 中 | 版本号降级 + 失败实体用默认状态重建 |
| `dotnet build` 编译慢影响体验 | 中 | 低 | 增量编译; 后续可探索 Roslyn API 内存编译 |
| Finalizer 线程持有资源阻止回收 | 低 | 高 | `GC.WaitForPendingFinalizers()` + 超时保护 |
| 生产环境误启用热重载 | 低 | 高 | 配置项 `enable_hot_reload` 默认 false; Release 构建移除 |
