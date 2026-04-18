# ScriptPhase 5: 热重载机制

> 预估周期: 2–3 周 | 前置依赖: ScriptPhase 3 完成（ScriptPhase 4 可并行） | **状态: 🟡 进行中**

---

## 目标

开发期修改 C# 游戏脚本后，无需重启服务端进程即可生效。仅用户脚本程序集可热重载；引擎运行时（`Atlas.Runtime`）与共享库（`Atlas.Shared`）不可热重载。

## 验收标准 (M5)

- [~] 修改 C# 脚本 → 自动编译 → 旧程序集卸载 → 新程序集加载 → 实体状态恢复。
- [~] 热重载过程中无内存泄漏（`GCHandle` 全部释放，旧 `AssemblyLoadContext` 被 GC 回收）。
- [~] 重载失败（编译错误）不影响服务端运行，继续使用旧版本。
- [~] 仅在开发/调试模式启用，生产环境可关闭（`ClrHotReload::Config::enabled`）。

---

## 架构

### `AssemblyLoadContext` 隔离

```
Default ALC (不可卸载):
  Atlas.Runtime.dll, Atlas.Shared.dll, Atlas.Generators.*.dll(仅编译期), System.*

ScriptLoadContext (isCollectible: true):
  Atlas.GameScripts.dll  ← 用户脚本，卸载后由 GC 回收
```

### 重载时序

```
[FileWatcher 检测变更]
     → [debounce 500ms]
     → [CompileScripts: dotnet build → .reload_staging/ → 备份旧版至 .reload_backup/]
     → [HotReloadManager.SerializeAndUnload]
           · 遍历 EntityManager.Instance.GetAllEntities()
           · 每个实体写入 (typeName, entityId, byteLen, serializedPayload)
           · EntityManager.Clear() + ScriptHost.Unload()
     → [ClrScriptEngine.ReleaseAllScriptObjects() + ResetScriptMethodCache()]
     → [EntityDefRegistry::instance().unregister_all()]
     → [HotReloadManager.LoadAndRestore(path)]
           · ScriptHost.Load(path)（同时触发 DefEntityTypeRegistry.RegisterAll() 注册到 C++）
           · SpanReader 读回快照 → EntityFactory.Create(typeName) → Deserialize → Register
           · 失败时从 .reload_backup/ 回滚
     → [ClrScriptEngine.RebindScriptMethods()]
```

---

## 已落地组件

### C++ 侧 — `src/lib/clrscript/`

| 文件 | 说明 |
|------|------|
| `clr_hot_reload.{h,cc}` | `ClrHotReload::Configure / Reload / Poll / ProcessPending / IsEnabled`；`Config` 包含 `script_project_path`、`output_directory`、`debounce_delay`、`unload_timeout`、`auto_compile`、`enabled` |
| `file_watcher.{h,cc}` | 目录内 `.cs` 文件的 `last_write_time` 轮询；排除 `bin/` / `obj/` / `.git/` / `.reload_staging/` / `.reload_backup/` |
| `clr_object_registry.{h,cc}` | 跟踪所有 `ClrObject`；`ReleaseAll()` 在热重载前统一释放 `GCHandle` |
| `clr_script_engine.{h,cc}` | `ClrScriptEngine::CallHotReload(method)` / `CallHotReload(method, path)` 调用 C# `HotReloadManager`；`Host()` 暴露 `ClrHost&` 供重绑定使用 |

### C# 侧 — `src/csharp/Atlas.Runtime/Hosting/`

| 文件 | 关键 API |
|------|---------|
| `ScriptLoadContext.cs` | `internal sealed class ScriptLoadContext : AssemblyLoadContext`；`isCollectible: true`；`Load(AssemblyName)` 覆盖仅加载用户脚本 |
| `ScriptHost.cs` | `Load(string) / Unload(TimeSpan) → bool / Dispose()`；`Unload` 内部 `GC.Collect()` + `WaitForPendingFinalizers()` 循环直到 `WeakReference` 失效，超时则 `Atlas.Log.Warning` 报告可疑泄漏 |
| `HotReloadManager.cs` | `[UnmanagedCallersOnly] static int SerializeAndUnload()` / `[UnmanagedCallersOnly] static unsafe int LoadAndRestore(byte* pathUtf8, int pathLen)` |

### 状态快照格式

`HotReloadManager.SerializeAndUnload` 逐实体写入：

```
int32 entityCount
foreach entity:
    string typeName           // SpanWriter.WriteString（packed_int + UTF-8）
    uint32 EntityId
    int32  payloadByteLen     // 便于 LoadAndRestore 侧跳过已删除类型
    bytes  payload            // entity.Serialize(ref SpanWriter)
```

`LoadAndRestore` 中未知 `typeName` 会跳过对应 `payloadByteLen` 个字节，避免类型删除导致整段数据损坏。

## 状态迁移兼容

| 变更 | 行为 |
|------|------|
| 新增属性 | `Deserialize` 按 Source Generator 的版本号分支，新字段取默认值 |
| 删除属性 | `SkipEntityData` 按 `payloadByteLen` 直接跳过 |
| 类型变更 | 反序列化失败 → 实体以默认状态重建并记录 Warning |
| 新增实体类型 | 工厂自动包含 |
| 删除实体类型 | 旧快照中的该类型记录被跳过 |

## 剩余工作

- `test_hot_reload.*` 集成测试：编译 → 重载 → 状态恢复、回滚、连续重载（debounce）、`GCHandle leak_count == 0`。
- `enable_hot_reload` 生产环境开关的收口与 Release 构建裁剪。
- Debug 模式下 `ClrObject` 创建调用栈记录，便于定位泄漏实例。
