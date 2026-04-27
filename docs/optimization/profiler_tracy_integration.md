# Profiler 基础设施（Tracy + OpenTelemetry）

**优先级：** P0（前置 —— 本目录其它优化任务的度量基础都依赖它）
**子系统：** `src/lib/foundation/`、`src/lib/server/`、`src/lib/network/`、`src/lib/clrscript/`、`src/csharp/Atlas.Runtime/`
**影响：** 建立度量基底。它落地之前，本目录里的其它文档都是空想 —— 没有
统一的 C++/C# 时间轴、frame 标记、以及 100v100 热路径的 zone 级拆解，就
无法量化"改前 vs 改后"。

## 状态（2026 年 4 月）

| Phase | Patch | 状态 | 备注 |
|---|---|---|---|
| 1 — 依赖接入 + 宏壳 | 0001 | ✅ 完成 | Tracy fetch（patch 0004 中升级到 0.13.1）；`foundation/profiler.h` 宏面 |
| 2 — Frame 标记 + tick 驱动器 | 0002 | ✅ 完成 | `ServerApp::AdvanceTime` zone + `TickWorkMs` plot + 每进程 frame 名 |
| 3 — 100v100 热路径 zone | 0003 | ✅ 完成 | cellapp / witness / space / cell_entity / real_entity_data 中 24 个 zone |
| 4 — Tracy-CSharp 集成 | 0004, 0005 | ✅ 完成（有偏离） | Tracy 升至 0.13.1，构建为 SHARED。自写 P/Invoke（LibraryImport）替代 Tracy-NET——后者要求 net10 而 Atlas.Runtime 是 net9。一份 TracyClient.dll 同时支撑 C++ 与托管两端。 |
| 5a — 网络 zone + 字节 plot | 0006 | ✅ 完成 | `Channel::Send` / `Dispatch` zone + `BytesIn`/`BytesOut` plot + 每消息 id 文本 |
| 5b — OpenTelemetry 跨进程 | — | ❌ **延后** | wire-format envelope 改动 + OTel SDK 重量级，跟当前 100v100 attribution 需求关联弱。Tracy 进程内视图已够用。 |
| 6 — 内存 hook + 每池追踪 | 0007 | ✅ 完成 | `atlas::Heap` 抽象 + 全局 `operator new`/`delete` override（20 变体） + 每池 named Tracy hook（`PoolAllocator(name, …)`） |
| 6+ — mimalloc 后端可选 | 0008 | ✅ 完成 | `ATLAS_HEAP_ALLOCATOR=std\|mimalloc` CMake 字符串；将来加入其它 allocator（jemalloc 等）走 3 步扩展 |
| (默认切换) | 0015 | ✅ 完成 | mimalloc 改为默认堆分配器；server exe 部署链路扩展（atlas_deploy_clr_runtime 现在也透 TARGET_RUNTIME_DLLS） |
| (输出布局) | 0009 | ✅ 完成 | `bin/<build_dir>/...`，并存的 CMake build 目录（如 `build/debug` + `build/profile-mimalloc`）互不覆盖 |
| 7 — 客户端 zone + Unity 后端 | 0010 | ✅ 完成（有偏离） | `ClientCallbacks` + `ClientEntity.ApplyPositionUpdate` zone；`Atlas.Client.Unity`（asmdef + `UnityProfilerBackend`）在 Atlas dotnet 流水线之外（Unity-only）。`Atlas.Client.Desktop` 保持 `NullProfilerBackend`，不重复 Tracy 绑定。 |
| 8 — 构建模式 + runbook | 0011 | ✅ 完成 | `release` preset 关 profiler；新增 `profile`（RelWithDebInfo + profiler ON）；运维 runbook 在 [`docs/operations/profiling.md`](../operations/profiling.md)。machined 注 TRACY_PORT 环境变量未实施 —— Tracy 自动 fallback 端口、viewer 的 Discover 解决了多进程 attach。 |
| (Review 修复) | 0012 | ✅ 完成 | profiler-OFF 模式下旁路 AllocDepthGuard 与 channel.cc snprintf 死代码；新增 [`docs/operations/tracy_usage.md`](../operations/tracy_usage.md) Tracy 使用指南。 |

针对 100v100 attribution 目标，集成**功能上完成**。两处有意识的延后
（5b OTel、7 Atlas.Client.Desktop Tracy）在下面对应 phase 描述里有交代，
运维 runbook 中也有"未来工作"条目。

## 背景

Atlas 跑 C++ tick 循环（开放世界 20 Hz / 副本 30 Hz），通过 CoreCLR +
`[UnmanagedCallersOnly]` 调用 C# 战斗逻辑。优化工作需要在 native tick
阶段、托管脚本回调、网络序列化、跨进程 RPC 扇出之间做开销归因。现有
代码完全没有相应插桩。

## 目标

1. 统一时间轴交错呈现 native zone 与托管 zone，共用同一时钟源，使一帧
   trace 可以连续显示 `CellApp::Tick → ScriptHost::Update → C#
   CombatLogic.OnTick`。
2. 每进程的 frame 标记跟 `ServerApp::AdvanceTime()` 对齐 —— `OpenWorldTick`
   （20 Hz）和 `DungeonTick`（30 Hz）用不同标签。
3. 热路径上 zone 开销低于约 5 ns；profiler client 未连时无可测开销。
4. 任何生产服务器进程可即时按需 attach；启动 profiler 不需要停机重启。
5. 跨进程因果性：一条网络消息可在单一 trace UI 中从发送进程跟到接收进程。
6. **强制抽象层**：Atlas 中每一处插桩只用 Atlas 自定义的 `ATLAS_PROFILE_*`
   宏，从不直接出现 Tracy/OTel 符号。这条不可妥协。把 Tracy 换成其它
   后端（Optick、Perfetto、内部工具）必须只是 `foundation/profiler.h`
   一文件的修改。
7. **客户端 SDK 目标是 Unity**：同一逻辑面（`Profiler.Zone`、
   `Profiler.FrameMark`、`Profiler.Plot`）在 `Atlas.Client`（netstandard2.1）
   下编译，在 Unity 构建中路由到 `UnityEngine.Profiling.ProfilerMarker` /
   `Profiler.BeginSample`，使 trace 原生落入 Unity Profiler 窗口 / Unity
   Profile Analyzer / Frame Debugger。Unity 端插桩绝不为 Tracy 付出代价，
   且必须能内联到 `ProfilerMarker.Auto()`，让 IL2CPP 把它折进宿主引擎
   现有 sampler 基础设施里。

## 非目标

- GPU profiling（服务器没有）。
- 用于容量规划的统计采样 —— 单独走 `perf` / ETW，不在本框架内。
- 取代 `MemoryTracker` —— Tracy allocator hook 是补充，不是替代。
- C# 堆每分配打 tag —— `dotnet-gcdump` 按需跑，不通过本层。

## 解决方案概览

| 关注点 | 工具 | 位置 |
|--------|------|------|
| Native zone、frame、plot、锁 | Tracy 0.13.x | C++ 全部 |
| 托管 zone（服务端），共用 Tracy 时钟 | 自写 P/Invoke（LibraryImport） | `Atlas.Runtime` 及下游用户脚本 |
| 托管 zone（客户端） | Unity `ProfilerMarker` / `Profiler.BeginSample` | Unity 内消费的 `Atlas.Client` |
| 跨进程 span 关联 | OpenTelemetry C++ + .NET SDK（**延后**） | `network/channel.cc`、`Atlas.Runtime/Hosting` |
| C# 堆深挖（按需） | `dotnet-gcdump`、`dotnet-counters`（服务端）；Unity Memory Profiler（客户端） | 外部，按 PID attach |
| Native 堆深挖 | `atlas::HeapAlloc` 内的 `TracyAlloc` / `TracyFree` | `foundation/heap.cc`、`pool_allocator.cc` |

Tracy 承担内循环、纳秒级视图。OpenTelemetry 承担进程间、毫秒级视图（延后
落地）。两者通过 trace ID 桥接：每条网络消息头携带 W3C `traceparent`，
Atlas 把它写进 Tracy 作为 zone 文本注解，UI 可交叉跳转。

## 宏 API（代码唯一可触碰的面）

`src/lib/foundation/profiler.h` 定义整个抽象。**`foundation/profiler.cc`
之外的任何文件都不许 include `<tracy/Tracy.hpp>`。** code review 必须拒
绝直接出现 Tracy 引用。

```cpp
// Frame 标记 —— 顶层 tick 驱动器调用点每处一个。
ATLAS_PROFILE_FRAME(name)         // FrameMarkNamed; AdvanceTime() 末尾
ATLAS_PROFILE_FRAME_START(name)   // 配对 Start/End 形式
ATLAS_PROFILE_FRAME_END(name)

// 作用域 zone —— RAII，client 已连时约 2 ns，未连时为 0。
ATLAS_PROFILE_ZONE()              // 自动捕获函数名
ATLAS_PROFILE_ZONE_N(name)        // 显式名字，编译期字面量
ATLAS_PROFILE_ZONE_C(name, color) // 带色 zone（RGB 十六进制）
ATLAS_PROFILE_ZONE_TEXT(buf, len) // 给当前 zone 附加动态字符串

// Plot —— 标量时间序列（实体数、队列深度、带宽）。
ATLAS_PROFILE_PLOT(name, value)

// 自由格式 message 与 trace-id 注解。
ATLAS_PROFILE_MESSAGE(buf, len)
ATLAS_PROFILE_MESSAGE_C(buf, len, color)

// Allocator hook —— 仅在 heap.cc / pool_allocator.cc 内使用。
ATLAS_PROFILE_ALLOC(ptr, size)
ATLAS_PROFILE_FREE(ptr)
ATLAS_PROFILE_ALLOC_NAMED(ptr, size, pool_name)

// 锁竞争 —— 包装 mutex 类型。
ATLAS_PROFILE_LOCKABLE(type, var)
ATLAS_PROFILE_LOCK_MARK(var)

// 编译期开关 —— 未定义时所有宏展开为 no-op。
#ifndef ATLAS_PROFILE_ENABLED
#  define ATLAS_PROFILE_ENABLED 1
#endif
```

C# 端镜像同一面。**C# 面在服务端与客户端完全一致；只有后端不同。**

```csharp
using var _ = Profiler.Zone();              // [CallerMemberName] 自动名字
using var _ = Profiler.Zone("CombatTick");
Profiler.Plot("ActiveBuffs", count);
Profiler.Message($"trace={traceId}");
Profiler.FrameMark("DungeonTick");
```

为把 Unity 不兼容的代码隔离在 `Atlas.Client` 之外，面被拆到两个 assembly：

- `Atlas.Shared/Diagnostics/Profiler.cs` —— 公共、纯面 API。netstandard2.1
  友好。调用可替换的 `IProfilerBackend` 接口。默认后端是 `NullProfilerBackend`
  （每个方法都是 JIT 可内联消除的 no-op）。
- `Atlas.Runtime/Diagnostics/TracyProfilerBackend.cs` —— 服务端后端，对接
  自写 Tracy P/Invoke。在启动时由 `Lifecycle.DoEngineInit` 安装。
- `Atlas.Client.Unity/UnityProfilerBackend.cs`（新 assembly，Unity-only，
  在 `#if UNITY_2022_3_OR_NEWER` 下条件编译）—— 安装一个把 `Zone()`
  `[MethodImpl(AggressiveInlining)]` 路由到缓存 `ProfilerMarker.Auto()`
  的后端。

Unity 后端必须：
1. 不在 `Atlas.Client` 里引入任何非 Unity 依赖 —— `Atlas.Client` 保持
   netstandard2.1，零 UnityEngine 引用。
2. 用 `ProfilerMarker`（优于 `Profiler.BeginSample` —— 可用时 IL2CPP
   把它折成单次 `BurstStart/End`）。
3. 把 marker 按名字缓存在 thread-static 字典里，使每次调用代价为一次
   字典查找加一次 `marker.Begin()`。能写成 `Zone("LiteralName")` 的
   call site 优先用 static-readonly marker。
4. 把 `Plot(name, value)` 转给 `ProfilerCounterValue<T>`（Unity 2022.2+）；
   旧版 Unity 退化到 `Profiler.EmitFrameMetaData`。
5. 永不加载 Tracy native 库；Unity 构建甚至不能引用 Tracy 绑定。

C++ 与 C# 宏层在 `ATLAS_PROFILE_ENABLED=0`（服务端）以及没有 backend
被 install 的情况下（客户端）必须编译为 no-op。具体到 Unity 客户端，
release 构建排除 `UnityProfilerBackend` 后，每一个 `Profiler.Zone` call
site 必须降为 IL2CPP linker 能 strip 的死代码。

## 分阶段实施

### Phase 1 —— 依赖接入与宏壳

**文件：**
- `cmake/Dependencies.cmake` —— 加 Tracy `FetchContent`（钉 tag `v0.11.1`）
  以及 OpenTelemetry C++ SDK（仅 header-only metrics + tracing 部分）。
- `cmake/AtlasCompilerOptions.cmake` —— 由新 CMake option `ATLAS_ENABLE_PROFILER`
  推 `ATLAS_PROFILE_ENABLED`（`debug` / `hybrid` 默认 ON，`release` 默认 OFF
  直至 Phase 7 翻转）。
- `src/lib/foundation/profiler.h` —— 完整宏面，启用与禁用两条分支。Tracy
  头**仅在此文件**（与 `profiler.cc`）include。
- `src/lib/foundation/profiler.cc` —— 处理无法 header-only 的小实现（如
  plot 配置、broadcast 关）。
- `src/lib/foundation/CMakeLists.txt` —— 加源文件，链接 `TracyClient`。
- `tests/unit/foundation/test_profiler.cc` —— 验证所有宏在
  `ATLAS_PROFILE_ENABLED=0` 与 `=1` 两态下都能编译；零模式下不产出 Tracy
  符号。

**验收：**
- `cmake --preset debug && cmake --build build/debug` profiler ON 构建通过。
- `cmake --preset release` profiler OFF 构建通过（二进制中无 Tracy 符号 ——
  通过 `nm` / `dumpbin` 验证）。
- 新单测在两种模式下通过。

### Phase 2 —— Frame 标记与 tick 驱动器

**文件：**
- `src/lib/server/server_app.cc:206` `ServerApp::AdvanceTime()` —— 把工作
  括起的部分包上 `ATLAS_PROFILE_ZONE_N("Tick")`，末尾发
  `ATLAS_PROFILE_FRAME(config_.frame_name)`。`frame_name` 来自
  `ServerAppConfig`（新字段，默认 `"Tick"`；CellApp/BaseApp 按 Space 类型
  设为 `"OpenWorldTick"` / `"DungeonTick"`）。
- `src/lib/server/entity_app.cc` —— 围绕实体事件 drain 与 updatable 组
  加 zone，每组分别命名（`Updatables_L0`、`Updatables_L1`）。
- `src/lib/server/server_app.h` —— `TickStats` 把 `last_work_duration` 通过
  `ATLAS_PROFILE_PLOT("TickWorkMs", ms)` 上报，让帧时间历史可与 zone 并列
  查看。

**验收：**
- Tracy GUI 接到运行中的 CellApp 显示连续 frame bar，按 Space 类型标注。
- 日志中的 slow-tick 告警跟 `TickWorkMs` plot 上的尖峰对应。

### Phase 3 —— 100v100 关键路径 zone

**文件：**
- `src/server/cellapp/cellapp.cc` —— witness drain 循环、AoI 重建、controller
  resolve 周围加 zone。
- `src/server/cellapp/witness.cc` —— `Update()`、优先队列重建、delta 序列化、
  send 循环加 zone。
- `src/server/cellapp/space.cc` —— spatial-query 回调加 zone。
- `src/server/cellapp/cell_entity.cc` —— `OnRealEntityUpdate` 与 ghost 扇出
  加 zone。
- `src/server/cellapp/real_entity_data.cc` —— delta envelope 构建加 zone。

**命名约定：** `Subsystem::Method` —— 例如 `"Witness::Update"`、
`"RealEntityData::BuildDelta"`。匹配 Tracy 的源位置捕获，让 GUI 易读。

**验收：**
- 一次 100v100 stress trace 显示每 tick ≥30 个命名 zone，覆盖 ≥90% 的
  measured work duration。任何超过 5% 的"无名时间"在 phase 完成前必须
  补 zone 填上。

### Phase 4 —— Tracy-CSharp 集成

**文件：**
- `src/csharp/Atlas.Runtime/Atlas.Runtime.csproj` —— 加 `Tracy.CSharp` 包
  引用（钉版到 wire protocol 与所选 native Tracy tag 对应的 release）。
- `src/csharp/Atlas.Runtime/Diagnostics/Profiler.cs` —— 新文件，镜像 C++
  侧的宏面（`Zone`、`FrameMark`、`Plot`、`Message`）。
- `src/csharp/Atlas.Runtime/Hosting/ScriptHost.cs` —— 启动时拉起托管 Tracy
  client，**在** native `ClrHost::Initialize` 返回**之后**。两侧必须发到
  同一 Tracy listener 端口。
- `src/lib/clrscript/clr_host.cc` —— 通过现有 config 通路把 Tracy server
  端口暴露出来，让托管端通过环境变量（`TRACY_PORT`，默认 9000+pid）取到。
- `src/csharp/Atlas.Runtime/Entity/*` —— 把 Phase 3 发现的实体 tick 回调与
  property setter 热路径插桩起来。

**验收：**
- 单 Tracy GUI session 显示 native 与托管 zone 交错，时间戳连续。
- 托管 `Profiler.Zone("CombatLogic.OnTick")` 嵌套在 native
  `ScriptHost::InvokeTick` zone 之下 —— 时间无空隙。

### Phase 5 —— 网络消息 trace 与 OpenTelemetry 桥（**5b 延后**）

**文件：**
- `src/lib/network/bundle.h` / `bundle.cc` —— 给 message envelope 扩展
  可选 `traceparent`（16 字节 trace_id + 8 字节 span_id + flag）。wire
  bump 在本 phase 文档化，绑定到现有 `kAtlasAbiVersion` bump。
- `src/lib/network/channel.cc:41` `Channel::Send()` —— 发出
  `ATLAS_PROFILE_ZONE_N("Channel::Send")` 与
  `ATLAS_PROFILE_PLOT("BytesOut", n)`；若有活跃 OTel context，序列化进
  新 envelope 字段；发出 `ATLAS_PROFILE_MESSAGE("trace=<id>")`。
- `src/lib/network/channel.cc:189` dispatch —— 打开按 message ID 符号形式
  命名的 Tracy zone，从 envelope 恢复 OTel context。
- `src/lib/network/CMakeLists.txt` —— 链接 OTel SDK。
- `src/csharp/Atlas.Runtime/Hosting/ScriptHost.cs` —— 用 machined 注册的
  同 service name 初始化 .NET `OpenTelemetry` provider。
- 新建 `cmake/AtlasOtelDeploy.cmake` —— 可选的 Jaeger/Tempo 收集器端点，
  通过 `server_config` 中 `atlas.otel.endpoint` 配置。

**验收：**
- 一条从 BaseApp 发到 CellApp 的消息产出两个 Tracy zone（每进程一个），
  各自文本注解中带匹配的 trace ID。
- 同一 trace ID 在 Jaeger 中表现为一棵 span 树。
- 当 `atlas.otel.endpoint` 未设时，没有 OTel 网络流量（OTel exporter 计数
  为零验证）。

### Phase 6 —— 内存与 pool allocator hook

**文件：**
- `src/lib/foundation/heap.cc` —— 全局 `operator new`/`delete` 路由到
  `atlas::HeapAlloc`/`HeapFree`，后者在 hook 点调
  `ATLAS_PROFILE_ALLOC` / `ATLAS_PROFILE_FREE`。
- `src/lib/foundation/pool_allocator.cc` —— 在每池 acquire/release 处加
  `ATLAS_PROFILE_ALLOC_NAMED(ptr, size, pool_name_)`。pool 名来自构造
  时传入的稳定指针。
- `src/lib/foundation/intrusive_ptr.h` —— 不动；Tracy 已经透过底层
  allocator 归因。

**验收：**
- Tracy "Memory" tab 按池名分独立流。
- 受控泄漏（分配未释放）在发生后 1 秒内出现在未释放清单。

### Phase 7 —— 客户端 SDK profiler 抽象（Unity 兼容）

**文件：**
- `src/csharp/Atlas.Shared/Diagnostics/Profiler.cs` —— 公共宏镜像 API
  （`Zone`、`Plot`、`Message`、`FrameMark`）。所有方法路由到
  `IProfilerBackend Profiler.Backend`；默认后端 `NullProfilerBackend`。
  方法标 `[MethodImpl(MethodImplOptions.AggressiveInlining)]`。
- `src/csharp/Atlas.Shared/Diagnostics/IProfilerBackend.cs` —— 接口：
  `BeginZone(string)`、`EndZone(IntPtr)`、`Plot(string, double)`、
  `Message(string)`、`FrameMark(string)`。`BeginZone` 返回的 token 是
  不透明 `IntPtr`，后端可缓存 marker 而不让 API 表面感知。
- `src/csharp/Atlas.Shared/Diagnostics/NullProfilerBackend.cs` —— no-op
  fallback。
- `src/csharp/Atlas.Client.Unity/`（新 assembly，Unity-only，asmdef +
  managed plugin）—— 含 `UnityProfilerBackend.cs`：
  - `ProfilerMarker` 缓存按字面量名 key，首次 `BeginZone` 时填入。
  - `Plot` 转到 `ProfilerCounterValue<T>`（Unity 2022.2+）；旧 Unity 退化
    到 `Profiler.EmitFrameMetaData`。
  - `FrameMark` 不做事 —— Unity 自己驱动 render frame marker；服务端的
    frame 名作为 `Profiler.BeginSample` 转发，把逻辑 tick frame 与 render
    frame 区分开。
- `src/csharp/Atlas.Client.Unity/Atlas.Client.Unity.asmdef` —— Unity
  assembly 定义，带 `defineConstraints: ["UNITY_2022_3_OR_NEWER"]` 和
  Unity Profiler API 的 `versionDefines`。
- `src/csharp/Atlas.Client.Desktop/DesktopBootstrap.cs` —— **不**安装 Tracy
  后端（保持 `NullProfilerBackend`），避免把 Tracy P/Invoke 重复到客户端
  或反向耦合 Atlas.Runtime；桌面客户端默认无 trace。
- `src/csharp/Atlas.Client/ClientHost.cs` —— 客户端 tick 边界
  `Profiler.FrameMark("ClientTick")`；message dispatch 插桩。
- `src/csharp/Atlas.Client/ClientEntity.cs` —— property apply、predicted
  movement、prediction reconciliation 插桩（base virtual no-op 的
  `Apply{Owner,Other}Snapshot` 不插，等代码生成器改造时一起出 zone）。

**后端选择（编译/装载顺序）：**

```
Atlas.Shared 启动用 NullProfilerBackend。
  └─ Atlas.Runtime（服务端） → 装 TracyProfilerBackend
  └─ Atlas.Client.Unity（Unity Awake） → 装 UnityProfilerBackend
  └─ Atlas.Client.Desktop（桌面客户端） → 保留 Null（默认）
```

应用选其一。试图装第二个会被拒绝并打 warning。设计上不合并 —— 两个后端
针对不同 UI。

**验收：**
- Unity 示例场景跑 Atlas 客户端，Unity Profiler 窗口显示对应
  `ClientHost.Tick`、`ClientEntity.ApplyDelta`、predicted movement 的
  命名 sample —— 名字与桌面示例下 Tracy 中显示的一致。
- `Atlas.Client.csproj`（netstandard2.1）构建无 UnityEngine 引用。
- `Profiler.Backend = NullProfilerBackend` 的 IL2CPP 构建会 strip zone
  body（取一处 site，检查生成的 C++ 验证）。
- 双向 feature 的逻辑 zone 名在服务端（Tracy）与客户端（Unity Profiler）
  一致（如 `Combat.OnDamage` 两侧都出现，将来通过消息中的 `traceparent`
  关联 —— 见 Phase 5）。

### Phase 8 —— 构建模式、部署、machined 编排

**文件：**
- `CMakePresets.json` —— 加 `profile` preset（RelWithDebInfo +
  `ATLAS_ENABLE_PROFILER=ON`）；普通 `release` 改为 profiler OFF。
- `src/server/machined/` —— 进程拉起时不必显式注 `TRACY_PORT` 环境变量
  （Tracy 会自动 fallback 端口，viewer 的 Discover 处理多进程 attach）。
  跨进程时可设 `OTEL_SERVICE_NAME=<process_type>-<instance_id>`，等 OTel
  落地时启用。
- `docs/operations/profiling.md`（新）—— 简版运维 runbook：怎么 attach
  Tracy GUI、怎么 attach `dotnet-counters`、怎么读 `TickWorkMs` plot。
- `docs/operations/tracy_usage.md`（新，Patch 0012 加入）—— Tracy 详细
  使用指南。

**验收：**
- machined 起 4 进程集群，分到 4 个独立 Tracy 端口。
- 单 Tracy GUI 不重启就能在 4 进程间切换查看。

## CMake Option 面

```cmake
option(ATLAS_ENABLE_PROFILER  "Enable Tracy profiler instrumentation" ON)
option(ATLAS_PROFILER_ON_DEMAND "Tracy ON_DEMAND mode (zero cost when no client)" ON)
set(ATLAS_HEAP_ALLOCATOR "std" CACHE STRING "Heap backend (std | mimalloc)")
```

## 性能预算

| 操作 | 预算 | 来源 |
|---|---|---|
| `ATLAS_PROFILE_ZONE` 启用，client 已连 | ≤ 5 ns | Tracy 手册（约 2.25 ns）+ 宏开销 |
| `ATLAS_PROFILE_ZONE` 启用，client 未连（`ON_DEMAND`） | ≤ 1 ns | 分支 + atomic load |
| `ATLAS_PROFILE_ZONE` 编译期禁用 | 0 ns | 宏空展开 |
| OTel span 每条网络消息开/关（延后） | ≤ 200 ns | OTel C++ SDK 微基准 |
| 100v100、约 5k zone/tick 的总 profiler 开销 | ≤ 0.5 ms | 由上推算 |

如果某 phase 的 stress test 显示 profiler 开销超过 0.5 ms/tick，那个
phase 不通过 —— 降低 zone 密度，或在该子系统中改成更低频率采样。

## 风险

- **Wire protocol 漂移**：Tracy native 与托管 P/Invoke 必须用同一 protocol
  版本。两端钉到一对已知良好的版本（Phase 1 + Phase 4），不一起升不能
  单升一边。CI 必须在 C# 包版本与 C++ submodule tag 不一致时 fail。
- **采样下托管调用栈断层**：`[UnmanagedCallersOnly]` thunk 没 PDB；采样
  profiler 可能在 native 与托管 zone 之间显示 "[unknown]" 帧。缓解：本
  抽象的主模式是**显式 zone，不是采样**。采样是次级诊断手段，永远不是
  ground truth。
- **100v100 队列饱和**：见 Phase 1 默认 256 MB 队列。仍饱和时把 witness
  内循环细粒度 zone 降级为 plot counter 而不是逐事件 zone。
- **trace context 跨逻辑会话泄漏**：网络 envelope 中的 `traceparent` 必须
  在 session 边界（login handoff、channel 重建）清掉，避免把无关请求合并
  到同一棵树。Phase 5 中审。
- **生产环境 profiler 开**：即使 `ON_DEMAND`，能访问 Tracy 端口的攻击者
  能观察热路径代码结构。Tracy listener 必须只绑定到集群内部接口；
  machined 负责不把端口范围对外暴露。
- **Unity backend marker 泄漏**：Unity 的 `ProfilerMarker` 缓存必须在
  domain reload（Unity Editor）时清掉 —— 残留的托管对象绑到陈旧 marker
  ID 会让下次 play session 崩。`UnityProfilerBackend` 必须订阅
  `AssemblyReloadEvents.beforeAssemblyReload` 并清缓存。
- **服务端↔客户端 zone 名漂移**：双侧都插桩的 feature（如服务端 Tracy
  与客户端 Unity Profiler 都看到的伤害结算）必须用共享名常量。定义在
  `Atlas.Shared/Diagnostics/ProfilerNames.cs`，避免任一侧的笔误打破
  trace 关联。
- **抽象侵蚀**：早晚会有人为了用 Tracy 独有特性直接 `#include
  <tracy/Tracy.hpp>`。CI grep 守住：`git grep -n 'tracy/Tracy.hpp' src/ |
  grep -v 'foundation/profiler'` 必须返回空。

## 回滚预案

如果 Tracy 中途证明不可行（wire protocol 阻塞、license 重新评估等）：
把 `foundation/profiler.{h,cc}` 替换为指向其它后端（microprofile、Optick、
内部 ring-buffer dumper）的实现。宏面是契约 —— 其它文件无需改动。这正
是 Phase 1 对抽象层不可妥协的根本原因。

## 关键文件（汇总）

- `src/lib/foundation/profiler.h` —— **唯一允许 include Tracy 头的文件**
- `src/lib/foundation/profiler.cc`
- `src/lib/foundation/heap.h` / `heap.cc` —— `atlas::HeapAlloc` + 全局
  `operator new`/`delete` override（20 变体）+ Tracy hook
- `src/lib/foundation/pool_allocator.cc` —— 每池 named hook
- `src/lib/server/server_app.cc` —— frame 标记 + plot
- `src/lib/network/channel.cc` —— send/recv zone + BytesIn/BytesOut plot
- `src/csharp/Atlas.Shared/Diagnostics/Profiler.cs` —— 公共宏镜像（服务端
  + 客户端）
- `src/csharp/Atlas.Shared/Diagnostics/IProfilerBackend.cs`
- `src/csharp/Atlas.Shared/Diagnostics/NullProfilerBackend.cs`
- `src/csharp/Atlas.Shared/Diagnostics/ProfilerNames.cs` —— 共享 zone 名
  常量
- `src/csharp/Atlas.Runtime/Diagnostics/TracyNative.cs` —— Tracy 0.13 P/Invoke
- `src/csharp/Atlas.Runtime/Diagnostics/TracyProfilerBackend.cs` —— 服务端
  后端
- `src/csharp/Atlas.Runtime/Core/Lifecycle.cs` —— 启动时安装 backend
- `src/csharp/Atlas.Client.Unity/UnityProfilerBackend.cs` —— Unity
  `ProfilerMarker` 后端
- `src/csharp/Atlas.Client.Unity/Atlas.Client.Unity.asmdef`
- `src/csharp/Atlas.Client/ClientCallbacks.cs`、`ClientEntity.cs` ——
  客户端 zone 插桩
- `src/server/cellapp/{cellapp,witness,space,cell_entity,real_entity_data}.cc`
- `cmake/Dependencies.cmake` —— Tracy + mimalloc fetch
- `cmake/AtlasCompilerOptions.cmake` —— `ATLAS_PROFILE_ENABLED` /
  `ATLAS_HEAP_<NAME>` 注入
- `CMakePresets.json` —— `profile` preset
- `tests/unit/test_profiler.cpp`、`tests/unit/test_heap.cpp`
- `docs/operations/profiling.md`、`docs/operations/tracy_usage.md`
