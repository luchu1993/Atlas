# 热重载

> **状态**:✅ 已落地。基础设施(`AssemblyLoadContext` 隔离、
> `ScriptHost`、`HotReloadManager`、`ClrHotReload`、`FileWatcher`、
> `ClrObjectRegistry`)、`ScriptApp` 接入(由 `ServerConfig.enable_hot_reload`
> 等字段驱动,BaseApp / CellApp 自动获得)、C++ 集成测试
> `tests/integration/test_hot_reload.cpp` 已就位。`Config.assembly_name`
> 默认 `Atlas.GameScripts.dll`,可在 JSON 配置中通过
> `hot_reload.assembly_name` 覆盖。

开发期修改 C# 玩法脚本后,无需重启服务端进程即可生效。仅用户脚本程序集
可热重载;引擎运行时(`Atlas.Runtime`)、CoreCLR 桥(`Atlas.ClrHost`)与
共享库(`Atlas.Shared`)留在 Default ALC,不可卸载。

## 1. 程序集隔离

```
Default ALC (不可卸载):
  Atlas.Runtime.dll, Atlas.ClrHost.dll, Atlas.Shared.dll, System.*

ScriptLoadContext (isCollectible: true):
  Atlas.GameScripts.dll  ← 用户脚本,卸载后由 GC 回收
```

`ScriptLoadContext.Load(AssemblyName)` 仅 resolve 用户脚本,把所有引擎
依赖都委托给 Default ALC,避免双 Assembly 实例(参见
[native_api_architecture.md](native_api_architecture.md) §5)。

## 2. 重载时序

```
[FileWatcher 检测变更]
     → [debounce 500ms]
     → [CompileScripts: dotnet build → .reload_staging/ → 备份至 .reload_backup/]
     → [HotReloadManager.SerializeAndUnload]
           · 遍历 EntityManager.Instance.GetAllEntities()
           · 每个实体写入 (typeName, entityId, byteLen, payload)
           · EntityManager.Clear() + ScriptHost.Unload()
     → [ClrScriptEngine.ReleaseAllScriptObjects() + ResetScriptMethodCache()]
     → [EntityDefRegistry::Instance().clear()]
     → [HotReloadManager.LoadAndRestore(path)]
           · ScriptHost.Load(path)(同时触发 DefEntityTypeRegistry.RegisterAll)
           · SpanReader 读回快照 → EntityFactory.Create(typeName) → Deserialize → Register
           · 失败时从 .reload_backup/ 回滚
     → [ClrScriptEngine.RebindScriptMethods()]
```

## 3. 已落地组件

C++ 侧 — `src/lib/clrscript/`:

| 文件 | 说明 |
|---|---|
| `clr_hot_reload.{h,cc}` | `ClrHotReload::Configure / Reload / Poll / ProcessPending / IsEnabled`;`Config` 含 `script_project_path`、`output_directory`、`assembly_name`、`debounce_delay`、`unload_timeout`、`auto_compile`、`enabled` |
| `file_watcher.{h,cc}` | 目录内 `.cs` 文件 `last_write_time` 轮询;排除 `bin/` / `obj/` / `.git/` / `.reload_staging/` / `.reload_backup/` |
| `clr_object_registry.{h,cc}` | 跟踪所有 `ClrObject`;`ReleaseAll()` 在重载前统一释放 `GCHandle` |
| `clr_script_engine.{h,cc}` | `ClrScriptEngine::CallHotReload` 调用 C# `HotReloadManager`;`Host()` 暴露 `ClrHost&` 供方法重绑 |

C# 侧 — `src/csharp/Atlas.Runtime/Hosting/`:

| 文件 | 关键 API |
|---|---|
| `ScriptLoadContext.cs` | `internal sealed class ScriptLoadContext : AssemblyLoadContext`,`isCollectible: true` |
| `ScriptHost.cs` | `Load / Unload(TimeSpan) → bool / Dispose`;`Unload` 内 `GC.Collect` + `WaitForPendingFinalizers` 循环直到 `WeakReference` 失效,超时 `Atlas.Diagnostics.Log.Warning` |
| `HotReloadManager.cs` | `[UnmanagedCallersOnly] SerializeAndUnload()`、`LoadAndRestore(byte* pathUtf8, int pathLen)` |

## 4. 状态快照格式

`HotReloadManager.SerializeAndUnload` 逐实体写入:

```
int32 entityCount
foreach entity:
    string typeName        // SpanWriter.WriteString(packed_int + UTF-8)
    uint32 EntityId
    int32  payloadByteLen  // 便于 LoadAndRestore 跳过已删除类型
    bytes  payload         // entity.Serialize(ref SpanWriter)
```

`LoadAndRestore` 中未知 `typeName` 会跳过对应 `payloadByteLen` 个字节,
避免类型删除导致整段数据损坏。

## 5. 状态迁移兼容

| 变更 | 行为 |
|---|---|
| 新增属性 | `Deserialize` 按 Source Generator 的版本号分支,新字段取默认值 |
| 删除属性 | 按 `payloadByteLen` 直接跳过 |
| 类型变更 | 反序列化失败 → 实体以默认状态重建,记录 Warning |
| 新增实体类型 | 工厂自动包含 |
| 删除实体类型 | 旧快照中该类型记录被跳过 |

## 6. 进程接入

`ScriptApp::Init`(BaseApp / CellApp 共用基类)在 CLR 与脚本程序集就绪
后读取 `ServerConfig`:

| JSON key (`hot_reload`) | `ServerConfig` 字段 | 默认 |
|---|---|---|
| `enabled` | `enable_hot_reload` | `false` |
| `script_project_path` | `hot_reload_script_project_path` | 空 |
| `output_directory` | `hot_reload_output_directory` | `script_assembly` 同目录 |
| `assembly_name` | `hot_reload_assembly_name` | `Atlas.GameScripts.dll` |
| `debounce_ms` | `hot_reload_debounce_ms` | `500` |
| `unload_timeout_ms` | `hot_reload_unload_timeout_ms` | `5000` |
| `auto_compile` | `hot_reload_auto_compile` | `true` |

`enabled=true` 时 `ScriptApp` 会构造 `ClrHotReload` 并在
`OnTickComplete` 末尾调用 `Poll()` + `ProcessPending()`,使主线程独占
重载边界。`enabled=false`(生产默认)时 `ClrHotReload` 不构造,Tick
路径完全无开销。

## 7. 测试

| 层 | 文件 | 覆盖 |
|---|---|---|
| C++ 集成 | `tests/integration/test_hot_reload.cpp` | `IsEnabled` 状态机、disabled 路径无副作用、空目录 Poll 不触发、缺失 assembly / 编译失败的 graceful failure、`ClrObjectRegistry::ActiveCount() == 0` 不变量 |
| C# 单测 | `tests/csharp/Atlas.Runtime.Tests/ScriptHostTests.cs` | `ScriptHost.Load / Unload`、`WeakReference` GC 收敛 |

## 8. 关联

- [native_api_architecture.md](native_api_architecture.md) — `ClrObjectVTable` 与重载边界的 vtable 重绑定。
- [serialization_alignment.md](serialization_alignment.md) — `WriteRawBytes` 在快照中的使用约定。
- [entity_type_registration.md](entity_type_registration.md) — 重载边界的 `EntityDefRegistry::clear()` 与重新注册。
