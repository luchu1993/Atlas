# Tracy Profiler 使用指南

本文档是"我具体怎么在 Atlas 上用 Tracy"的实操向说明。运维 runbook（构建
preset、部署、多进程编排）见 [`profiling.md`](profiling.md)；设计依据
（为什么选 Tracy、为什么是这个形态）见
[`../optimization/profiler_tracy_integration.md`](../optimization/profiler_tracy_integration.md)。

## Tracy 能给你什么

Tracy 是采样 + 插桩的混合 profiler。Atlas 把它当**插桩** profiler 用——
引擎中每一个 `ATLAS_PROFILE_ZONE_N(...)` 标注的命名区域都成为 Tracy 时间
轴上的一行。viewer 同时也能在此基础上抓系统级采样（CPU 上下文切换、
内核调用），所以你能在并排的两个视角下看到 Atlas 的逻辑结构和操作
系统的底层视图。

具体来说，把 viewer 接到一个运行中的 CellApp 之后你能看到：

- **Frame 时间轴**：每个逻辑 tick 一根 bar（`OpenWorldTick`、
  `<process_name>.Tick` 等）。鼠标悬停显示时长，点击放大。
- **每帧 zone 树**：`Tick → CellApp::OnTickComplete → TickWitnesses →
  Witness::Update → Witness::Update::Pump → SendEntityUpdate → …`。一直
  向下钻到耗时所在层。
- **Plot**：`TickWorkMs`、`BytesIn`、`BytesOut` 跟时间轴在同一时间维度
  上画。`TickWorkMs` 上的尖峰直接对应当时打开的那个 zone。
- **内存追踪**：每个池一条流（`TimerNode`、未来加入的 Atlas 池）外加
  全局堆。Memory tab 显示分配/释放计数、当前用量、未释放清单——找泄漏
  用得上。
- **锁竞争**：用 `ATLAS_PROFILE_LOCKABLE` 包过的 `std::mutex` 实例会在
  时间轴上画出等待/持有区间。
- **C# zone 交错呈现**：`Script.OnTick` 以及任何来自托管代码的
  `Atlas.Diagnostics.Profiler.Zone(...)` 调用跟 C++ zone 出现在同一条
  时间轴上——统一视图。

Tracy 默认**不**给你的：

- **跨进程视图**：一个 viewer 同一时间只能 attach 一个进程。4 进程
  集群要在 attach session 之间切换。viewer 的 Discover 对话框会列出全
  部，重连一键。
- **分布式 trace**：没有"BaseApp 发出的 RPC 到了 CellApp"这种概念，
  那是 profiler 集成计划 Phase 5b 有意延后的 OpenTelemetry 工作。
- **持续抓取**：默认 Tracy 在内存里全缓冲。长时间会话会累计几百 MB。
  关闭前用 **Save trace** 留底。

## 拿 viewer

Atlas 把 Tracy native 钉在 **0.13.1**（见 `cmake/Dependencies.cmake`）。
viewer 的 wire protocol 必须**精确匹配**这个 minor 版本 —— 拿一个
0.12.x 的 viewer 接 0.13.1 的 client，能连上但 zone 会被静默丢弃。

### 推荐方式：让构建顺带拿过来（Windows）

把 `ATLAS_BUILD_TRACY_VIEWER` 选项打开，CMake configure 时会从上游
release 下载 `windows-0.13.1.zip`，把里面的 6 个 exe（`tracy-profiler`
GUI 加上 `tracy-capture` / `tracy-csvexport` / `tracy-import-chrome` /
`tracy-import-fuchsia` / `tracy-update` CLI 工具）部署到
`bin/<build_dir>/tools/`：

```bash
cmake --preset profile-release -DATLAS_BUILD_TRACY_VIEWER=ON
cmake --build build/profile-release --config RelWithDebInfo
# 之后 Tracy 工具集就在：
# bin/profile-release/tools/tracy-profiler.exe
```

下载只在第一次 configure 时发生（约 5 MB），之后 cache 在 build dir 里。
版本严格匹配：升级 Tracy native 时（`cmake/Dependencies.cmake` 里那
个 URL）顺手把这里的 release URL 也改了即可。

### Linux / macOS

Tracy 上游目前不发 Linux/macOS 预编译。CMake 在这些平台开
`ATLAS_BUILD_TRACY_VIEWER=ON` 只会打 warning。要 viewer 的话单独从
fetch 下来的源码手动构建：

```bash
# Atlas 已经把 Tracy 源码 fetch 到了 build dir
cd build/<preset>/_deps/tracy-src/profiler
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
# 出来的 tracy-profiler 在 build/ 下面
```

依赖：装系统包 `libglfw3-dev`、`libfreetype-dev`、`libcapstone-dev`、
`libcurl4-openssl-dev`，以及 Wayland 或 X11 显示库。具体清单见 Tracy
仓库里的 `profiler/README.md`。

### 后路：自己去 GitHub 下

任何时候不想走 CMake 选项，可以直接去
<https://github.com/wolfpld/tracy/releases/tag/v0.13.1> 下
`windows-0.13.1.zip` 自己解压。

## Attach 到服务器进程

### 第 1 步 —— 用启用 profiler 的方式构建

下面任一 preset 都可以：

```bash
cmake --preset debug              # 日常开发
cmake --preset profile-release    # 生产形态
cmake --build build/<preset> --config Debug
```

`release` preset 故意**关掉** profiler——上线二进制不带 Tracy。
`profile-release` 才是任何"这样改更快吗"实验的目标。

### 第 2 步 —— 跑服务器

```bash
bin/profile-release/server/atlas_cellapp.exe --config server.json
```

默认情况下 Atlas 的 Tracy client 跑在 **on-demand** 模式
（`TRACY_ON_DEMAND=ON`）。在 viewer 接入之前，profiler 基础设施基本上
0 CPU 占用——不 flush、不 broadcast、不缓冲。这是有意设计的：我们要
保留把插桩留在生产二进制里的可能（用 `profile-release` 而不是
`release`），同时不让 profiler 24/7 跑开销。

### 第 3 步 —— 接 viewer

1. 启动 `Tracy.exe`（Linux 上是 `tracy-profiler`）。
2. 工具栏点 **Connect**。
3. 地址默认 `127.0.0.1`，保留。端口 `8086` 是 Tracy 默认 listen 端口。
4. 点 **Connect**。

第一帧 100 ms 以内就出来。attach 之前的所有数据都丢了——Tracy 的
on-demand 模式不预先缓冲。围绕这点规划：要追启动期问题就改用
`-DATLAS_PROFILER_ON_DEMAND=OFF` 构建，从进程起点就缓冲。

### 同主机多个服务器

Tracy 会自动把 listen 端口往后让：进程 1 拿 8086，进程 2 拿 8087，
依次类推。viewer 的 **Discover** 按钮（在 Connect 旁边）扫描本地端口
区间，按程序名列出每一个活动 session。machined 给每个 Atlas 进程都
带各自的 client；你不需要注 `TRACY_PORT` 环境变量。

两个 CellApp 都在跑时你会看到两条：

```
Discover ─┬─ atlas_cellapp [PID 12345] @ 127.0.0.1:8086
          └─ atlas_cellapp [PID 12346] @ 127.0.0.1:8087
```

点其中一条 attach。要换到另一个就 **Disconnect** 再选另一条。session
之间状态独立。

## 读时间轴

### Frame 行（屏幕顶部）

顶部那根粗 bar 是**帧**。Atlas 每个 tick 触发一个命名 frame（默认
`<process_name>.Tick`，`ServerConfig::frame_name` 覆盖时则是
`DungeonTick` 之类）。每根 bar 的宽度是这个 tick 的墙钟时长；颜色
编码"是否超出配置帧预算"。

慢 tick 显示成红色。点开一个就能放大到它的 zone 树。

### Zone 树（主时间轴）

每一行是一个线程；每块带颜色的方块是一个有时长的 zone。Atlas 顶层
zone 是 `Tick`。下面（CellApp 上）通常会看到：

```
Tick
├─ CellApp::OnEndOfTick
│   ├─ CellApp::TickGhostPump
│   ├─ CellApp::TickOffloadChecker
│   └─ CellApp::TickOffloadAckTimeouts
├─ Updatables
├─ CellApp::OnTickComplete
│   ├─ Script.OnTick                ← C# 战斗 / 移动逻辑
│   ├─ CellApp::TickControllers
│   │   └─ Space::Tick (× N 个 space)
│   ├─ CellApp::TickWitnesses
│   │   └─ Witness::Update (× N 个有 witness 的实体)
│   │       ├─ Witness::Update::Transitions
│   │       ├─ Witness::Update::PriorityHeap
│   │       └─ Witness::Update::Pump
│   │           └─ Witness::SendEntityUpdate (× N 个 peer)
│   ├─ CellApp::TickBackupPump
│   └─ CellApp::TickClientBaselinePump
```

zone 下面挂着 `Channel::Send` 就是网络出方向的开销；时间轴下面的
`BytesOut` plot 显示当时的字节数。

### Plot（时间轴下方）

plot 跟 frame 同步对齐。Atlas 输出：

| Plot | 来源 | 典型读法 |
|---|---|---|
| `TickWorkMs` | 每 tick 工作时间 | 应当远低于 `1000/update_hertz`（30 Hz 是 33 ms） |
| `BytesOut` | 每包出方向 | 尖峰对应 `Witness::Update::Pump` zone |
| `BytesIn` | 每包入方向 | 尖峰对应 `Channel::Dispatch` zone |

悬停任一点读当时的值。**Edit plots** 菜单可以改颜色、改格式（比如显示
为内存而不是裸数字）、改刻度。

### Find / Find Zone

`Ctrl-F` 打开 zone 搜索。输入任意 zone 名过滤列表。**Find Zone**（独立
对话框）显示一个 zone 在整个 trace 里的时长直方图——找异常值很方便
（`Witness::Update::Pump` 大多 0.5 ms，但有 8 ms 的尾巴？点尾巴上的
那点，时间轴跳到那一刻）。

## Memory tab

Atlas 报两类分配：

1. **全局堆** —— 每一个 `new` / `delete`（含 C++17 / C++14 sized /
   aligned 各变体），经过 `atlas::HeapAlloc` 路由。报告为匿名事件。
2. **每池单独流** —— 每个 `PoolAllocator(name, …)`。带池名上报。
   今天的池包含 `TimerNode`；新池只要构造时传稳定指针，名字就会自动
   出现在这里。

Memory tab 显示：

- **总分配/释放字节** 随时间变化
- **每分配器图表** —— 每个池名一条流，加上"匿名"代表全局堆
- **当前活跃分配清单** —— 在光标时间戳处什么还没释放。找泄漏用：
  暂停集群，跳到已知的稳定状态，找出本该释放却还在的项。

### 池命名约定

`PoolAllocator(pool_name, …)` 要求 `pool_name` 是字符串字面量或其它
静态分配指针。Tracy 用指针 identity（不是值）作为 key 索引每池内存
流——一个栈上 `std::string::c_str()` 会污染 trace。Atlas 现有的池都
传字面量；新加的也要。

## 同一 trace 中的 C# zone

服务端 C# 层（Atlas.Runtime）在 `Lifecycle.DoEngineInit` 中安装
`TracyProfilerBackend`。装上之后，托管代码里每一个
`Profiler.Zone(...)` / `Profiler.ZoneN(name)` 都发到 C++ 端用的同一
个 Tracy client。具体地：

- `Script.OnTick` zone 包住整个托管 tick 体
- （未来）一旦 def 代码生成器更新，property apply 路径上生成器自动
  输出的 zone 会出现在 `Script.OnTick` 之下

zone 跟 C++ zone 连续交错——没有独立的"托管"面板，trace 看上去就像
Atlas 是一个调用了更多 C++ 的单 C++ 程序。

C++ zone 在但托管 zone 缺失时，最常见原因是 `Lifecycle.DoEngineInit`
还没跑（runtime 没启动），或者 install 被拒（已经有非 null backend）。
看 `ATLAS_LOG_INFO` 输出找 `"Tracy profiler backend already installed"`。

## 锁竞争

把竞争激烈的 mutex 用 `ATLAS_PROFILE_LOCKABLE(std::mutex, my_lock)`
包起来代替 `std::mutex my_lock`，就把它加入了 Tracy 的竞争时间轴。
包过的锁在源码层行为完全一样（一样的 `lock()` / `unlock()`、跟
`lock_guard` 一样配合），但会向 viewer 报告等待 / 持有区间。

Lock tab 然后会显示：

- 哪些线程在这个锁上抢
- 每个线程等了多久
- 持有时长分布

用来确认热路径上的 mutex 没有把你以为是并行的工作悄悄串行化。

`ATLAS_PROFILE_LOCK_MARK(var)` 不实际加锁，只对一个锁发一个同步事件
——当你用原子手动串行化了一段东西、想让 viewer 记录这个同步时刻时
有用。

## 保存与回放

viewer 的 **File → Save** 把 trace 写到 `.tracy` 文件。之后用
**File → Open** 打开，不用源进程跑也能再分析。trace 文件几分钟录制
通常 50–500 MB——离线分析无压力，磁盘空间留出来就行。

线上小贴士：复现一个客户报告的慢 tick 时，抓一份 trace、Save、把
`.tracy` 发给排查的工程师。文件里有他需要的一切——不用搭一个一样的
集群。

## Tracy 开着的成本

| 场景 | 单 zone 开销 | 备注 |
|---|---|---|
| `release` preset（profiler 关） | 0 ns | 宏展开 no-op；Tracy DLL 不链接。 |
| profiler 开，无 viewer 连（`TRACY_ON_DEMAND`） | ~1 ns | atomic load + 分支。inert。 |
| profiler 开，viewer 已连 | ~2.25 ns | RDTSC + 队列 push。Tracy 文档值。 |
| 堆分配，profiler 开 | 额外 ~5–10 ns | re-entry guard（3× TLS）+ Tracy hook。 |

30 Hz 的 CellApp 在 100v100、每 tick ~5k zone 的最坏情况下，~1.5 ms / s
的开销——CPU 占比不到 0.5%。inert 模式更是无感。

## 常见踩坑

- **viewer 看不到任何 frame**：进程在跑但从来没触发 `FrameMark`。检查
  `ServerConfig::frame_name` 不为空（空时会从 `process_name` 自动派生）。
- **zone 出现了但时间轴"卡住"**：进程还没 tick（init 还在跑）。等几
  秒钟。
- **Memory tab churn 远高于你分配的量**：STL 容器的内部分配也算进来——
  `std::vector::push_back` 触发 grow 就是一个真实的 `operator new`。
  按分配器名过滤把刻意池子的流量跟 STL 顺带流量分开。
- **包过的 mutex 一直没出现 lock zone**：锁实际没竞争——Tracy 只报告
  等待，不报告零竞争获取。从另一个线程上故意制造竞争来验证 wrap
  生效。
- **C++ 跟 C# zone 没交错（托管缺失）**：backend install 失败。看启动
  时 `"already installed"` 警告。

## 想再深入

- Tracy 自家手册：<https://github.com/wolfpld/tracy/releases> →
  每个 release 旁边的 `tracy.pdf`。详尽——每个宏、每个 viewer 功能
  都讲。
- Atlas profiler 设计：[`docs/optimization/profiler_tracy_integration.md`](../optimization/profiler_tracy_integration.md)
- Atlas 宏面：[`src/lib/foundation/profiler.h`](../../src/lib/foundation/profiler.h)
- Atlas 托管 facade：[`src/csharp/Atlas.Shared/Diagnostics/Profiler.cs`](../../src/csharp/Atlas.Shared/Diagnostics/Profiler.cs)
- Tracy 0.13 wire protocol：只有源码——读
  `build/<preset>/_deps/tracy-src/server/TracyWorker.cpp` 是权威版本。
