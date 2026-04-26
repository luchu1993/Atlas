# Atlas Profiling

这是抓取运行中 Atlas 服务器集群（以及适用时 Unity 客户端）性能 trace
的运维 runbook。下面布局的设计依据见
[`docs/optimization/profiler_tracy_integration.md`](../optimization/profiler_tracy_integration.md)。
关于"具体怎么用 Tracy"的实操向走读——viewer 安装、frame 视图、plot、
memory tab、锁竞争——见 [`tracy_usage.md`](tracy_usage.md)。

## 构建 preset

| Preset | profiler 插桩 | 何时用 |
|---|---|---|
| `debug` | 开（默认） | 日常开发 |
| `release` | 关 | 上线给玩家用的二进制；零隐藏开销 |
| `profile-release` | 开，RelWithDebInfo | 生产形态的性能测试——优化后代码 + Tracy zone + 符号信息 |
| `hybrid` | 开（RelWithDebInfo） | 不需要 release 级代码生成的快速性能验证 |

```bash
# 生产形态的性能 trace
cmake --preset profile-release
cmake --build build/profile-release --config RelWithDebInfo
```

`release` preset 用 `ATLAS_ENABLE_PROFILER=OFF` 构建，这把每一个
`ATLAS_PROFILE_*` 宏在预处理阶段编译成 no-op，且完全去掉 Tracy DLL
链接。**没有**任何运行时开关能在 `release` 二进制上把 profiler 重新
打开——这是有意为之，跟集成文档里的"上线二进制无 profiler"承诺一致。

## 抓服务器 trace（Tracy）

Atlas 把 Tracy 链接为 SHARED 库，名字是 `TracyClient.dll`（Windows）或
`libTracyClient.so`（Linux）。client 端进程内运行，监听一个 TCP 端口
等 viewer 接入，viewer 没接之前完全 inert（默认 `TRACY_ON_DEMAND`
打开——见 `cmake/Dependencies.cmake`）。

### 单进程

1. 正常启动服务器（如 `bin/profile-release/server/atlas_cellapp.exe`）。
2. 启动 Tracy viewer。
3. 点 **Connect**，地址保留 `127.0.0.1`，端口 `8086`。

第一帧在 viewer 接上的瞬间就会出现；更早的内容不会被缓冲，这是设计
——`ON_DEMAND` 模式意味着没人看的时候零开销。

### 同主机多进程

Tracy 自动让出 listen 端口：第一个进程拿 `8086`，第二个 `8087`，依次
类推。viewer 的 **Discover** 扫描这个端口范围，按程序名列出每一个
活动 session。machined 不需要为此注 `TRACY_PORT` 环境变量。

如果两个进程恰好抢到了相邻端口而你想让它们落在固定号上（例如
CellApp 0 始终在 9000），通过 Tracy 的编译期 `TRACY_DATA_PORT` 定义
重新构建——运行时端口覆盖路径目前 Atlas 没接入。见下文"未来工作"。

### 读 trace

默认视图按线程分行。frame 标记（`OpenWorldTick`、
`<process_name>.Tick`，可通过 `ServerConfig::frame_name` 配置）把时间
轴切分成逻辑 tick。有用的 plot：

| Plot | 来源 | 含义 |
|---|---|---|
| `TickWorkMs` | `ServerApp::AdvanceTime` | 每 tick 工作时间。尖峰跟慢 tick 日志告警相关。 |
| `BytesOut` | `Channel::Send` | 每包出方向大小。值大常跟 `Witness::Update::Pump` 关联。 |
| `BytesIn` | `Channel::OnDataReceived` | 每包入方向大小。 |

Per-pool 内存流出现在 **Memory** tab——`TimerNode` 是其中之一；后续在
`PoolAllocator(name, …)` 下面新加的池会自动出现。

### 集群级 trace

machined 起来的 4 进程集群（`atlas_cellapp` × 2、`atlas_baseapp`、
`atlas_loginapp`）会有 4 个 Tracy listener 落在相邻端口。viewer 一次
attach 一个；切换之间时间轴状态独立干净。今天**没有**单窗口集群视图
——那是集成计划 Phase 5b 有意延后的 OTel 分布式 trace 工作。

## 抓客户端 trace（Unity Profiler）

Atlas Unity 客户端把同一组 zone 名通过 `ProfilerMarker` 路由出去，
在 Unity Profiler 窗口可见：

1. 构建 Unity 客户端，确保 `Atlas.Client.Unity.dll` 在场。
2. 应用启动期 bootstrap backend：
   ```csharp
   Atlas.Diagnostics.Profiler.SetBackend(new Atlas.Client.Unity.UnityProfilerBackend());
   ```
3. 打开 **Window → Analysis → Profiler**。连到运行中的 player
   （Editor 或设备）。

Zone 名通过 `Atlas.Diagnostics.ProfilerNames` 跟服务器对齐——比如
客户端的 `ClientCallbacks.DispatchPropertyUpdate` 跟服务器为同一个
逻辑 property delta 触发的 `Channel::Send` 在两边时间戳上能对齐。
domain reload 注意事项见 `Atlas.Client.Unity/README.md`。

## C# 堆与 GC

Tracy 的内存 hook 只覆盖 native（C++）分配。服务端托管分配走
`dotnet-counters` 和 `dotnet-gcdump`：

```bash
# 实时 counter——GC 压力、分配速率、服务器 tick
dotnet-counters monitor --process-id <PID> System.Runtime Atlas.*

# 一次性堆快照，找泄漏
dotnet-gcdump collect --process-id <PID> --output cellapp.gcdump
```

用 Visual Studio 或 PerfView 打开 `.gcdump` 做对象图分析。

## 切换分配器

默认堆是 `std`（平台 CRT）。做性能对比时切到 mimalloc：

```bash
cmake -B build/profile-release-mimalloc \
      --preset profile-release \
      -DATLAS_HEAP_ALLOCATOR=mimalloc
```

两个配置同时 target `RelWithDebInfo`，互不覆盖各自的 `bin/` 输出
——build 目录名就是 bin 目录名（见 patch 0009）。两个都跑起来，
Tracy trace 并排比对。

## 排查"zone 缺失"

| 现象 | 可能原因 |
|---|---|
| Tracy viewer 一帧都没有 | `release` preset（profiler 关），或进程还没 tick |
| C# zone 缺、C++ zone 在 | `Profiler.SetBackend(new TracyProfilerBackend())` 还没被 `Lifecycle.DoEngineInit` 调到 |
| Plot 一直是 0 | plot 值只在 callsite 被执行时才上报；`TickWorkMs` 这种 tick-driver 的 plot 至少要让 work bracket 跑过一次 |
| `Witness::Update::Pump` 是空的 | 没有 witness peer——通过 stress 框架加载真实实体 |

## 未来工作（不在当前 phase）

- **跨进程 span 关联（OTel）**：集成计划里的 Phase 5b，没启动。所需的
  wire-format envelope 改动会动到每一个 `bundle.cc` 消费者。
- **每进程确定性 Tracy 端口**：今天的 auto-fallback 对开发够用。生产
  部署想要每个 CellApp 实例稳定端口的话，要么走 Tracy 编译期重编，
  要么等我们接 Tracy 0.13+ 的运行时端口 API。
- **生成器输出的 property apply zone**：Atlas C# def 生成器可以在每个
  生成的 `ApplyReplicatedDelta` override 里输出 `Profiler.Zone(...)`。
  不在 profiler 集成 phase 范围内——属于下一轮代码生成器更新。
