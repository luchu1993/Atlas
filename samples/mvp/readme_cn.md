# Atlas MVP — Unity 端到端演示

> 状态：当前 Unity MVP 运行说明。示例使用现行服务端权威移动路径；UE 对齐与剩余表现层缺口见 `samples/mvp/UEClient/`。
>
多 CellApp Atlas 集群 + Unity 6 客户端，用来演示完整的 BigWorld 风格链路：服务端权威移动 + owner 本地预测、服务端权威技能 / 投射物伤害、AoI 驱动的同屏实体流式同步，以及逐 tick 属性复制。默认 wrapper 启动 4 个 CellApp，世界尺寸为 200 × 200 m，包含 1 个玩家 Avatar 和 150 个游荡 NPC。

## 目录结构

```
samples/mvp/
├── Atlas.Mvp.Base/      # BaseApp 脚本：Account.SelectAvatar、Avatar AoI 配置
├── Atlas.Mvp.Cell/      # CellApp 脚本：Avatar、Npc、NpcAiComponent、ProjectileSimulator
├── Atlas.Mvp.Client/    # netstandard2.1 + net10.0 — Unity 与 atlas_client.exe 共用
└── UnityClient/         # Unity 6 LTS 工程；Runtime/App/Bootstrap.cs 是入口
```

`entity_defs/{Avatar,Npc,Account}.def` 定义实体表面；`Avatar` 是 base + cell 实体，`Npc` 仅存在于 cell。

## 前置条件

- Unity Hub + Unity 6 LTS（工程固定为 `6000.0.43f1-lilith-2`）
- .NET 10 SDK
- CMake + Ninja（构建 helper 会按需准备 Ninja）
- Windows：Visual Studio 2026 或 Visual Studio 2022 17.14+，需安装 MSVC
  （构建 helper 会加载环境）。Linux/macOS：clang 或 gcc。

## 运行

```bash
# 1. 构建原生服务器 + 脚本 DLL（把 Atlas.Mvp.{Base,Cell}.dll 部署到 bin/debug/）。
tools/bin/build.sh debug
tools\bin\build.bat debug

# 2. 把 SDK + 实体 DLL staging 到 Unity Assets 目录。
tools/bin/setup_mvp_unity.sh
tools\bin\setup_mvp_unity.bat

# 3. 启动集群（machined + loginapp + baseappmgr + baseapp + cellappmgr + cellapps + dbapp）。
tools/bin/run_mvp_cluster.sh
tools\bin\run_mvp_cluster.bat

# 4. 在 Unity Hub 中打开 samples/mvp/UnityClient，然后点击 Play。
```

两端默认使用 LoginApp 端口 `20018`。集群中 CellApp 与 BaseApp 以 20 Hz 运行（50 ms tick）。`UnityClient/Assets/Scenes/Main.unity` 已经包含运行时 `Bootstrap` 对象。

**改了 `Atlas.Mvp.{Client,Cell,Base}/*.cs`、`entity_defs/*` 或 `src/csharp/Atlas.Client*/*.cs` 后，必须重跑 `tools\bin\setup_mvp_unity.bat`（或 .sh）再回 Unity Editor**。Unity 项目下有两条 staged 路径，都由 `setup_mvp_unity` 从源头重生成：

- `Assets/Plugins/Atlas.Mvp/Atlas.Mvp.Client.dll` —— 由 `samples/mvp/Atlas.Mvp.Client/` 经 Release `dotnet build` 产出再 copy
- `Assets/Atlas.Client.Unity/*` —— 对 `src/csharp/Atlas.Client.Unity/` 做增量同步，只复制变化文件

`build.bat debug` 只刷新服务端用的 `bin/debug/`，不动这两份 staged 副本。直接编辑 `Assets/Atlas.Client.Unity/` 下的内容会被下次 setup 覆盖；如果 Unity 锁住 native plugin，关闭 Editor 后重跑 setup。永远改 `src/csharp/...` 源。

Unity MVP 自身代码按 `Assets/Scripts/Runtime/` 和
`Assets/Scripts/Editor/` 分层。Runtime 由
`Atlas.Mvp.Unity.Runtime` asmdef 编译，并继续按 `App`、`Camera`、`World`、
`Views`、`UI`、`Input`、`Projectiles`、`Debug` 模块归类；Editor-only
构建入口由 `Atlas.Mvp.Unity.Editor` 编译。AoI box、BSP 几何等调试
overlay 独立放在 `Runtime/Debug/`，通过实例注入接入 HUD，不再作为
静态全局工具混在主运行时代码里。

## 打包 Unity 客户端

```bash
# 一步完成 staging + Unity batchmode player build。
tools/bin/build_mvp_unity.sh
tools\bin\build_mvp_unity.bat

# 如果 Assets 已经 staging 好，可以跳过 setup 阶段。
tools/bin/build_mvp_unity.sh --skip-setup --clean-output
tools\bin\build_mvp_unity.bat --skip-setup --clean-output
```

脚本会按顺序从 `--unity`、`UNITY_EXE`、`UNITY_PATH`、固定工程版本对应的 Unity Hub 安装路径查找 Unity 可执行文件。`build_mvp_unity` 要求存在 `ProjectSettings/ProjectVersion.txt`；如果固定版本的 Editor 未安装，请传 `--unity` 或设置 `UNITY_EXE`。Windows 默认输出为 `out/mvp-unity/windows/AtlasMvp.exe`，日志写到 `out/mvp-unity/unity-build.log`。非 Windows player 可传 `--target StandaloneLinux64` 或 `--target StandaloneOSX`。Standalone 构建默认以可调整大小的窗口启动。

不启动集群也可以运行打包 Player 的 native predictor 自检：

```bash
./out/mvp-unity/windows/AtlasMvp.exe -batchmode -nographics -atlas-predictor-self-test -logFile out/mvp-unity/predictor-self-test.log
```

Unity runtime 能加载 `atlas_net_client` 并跑完共享 native movement 序列时，
进程以 0 退出，日志包含 `[NativePredictorSelfTest] PASS ticks=10000`。

## 服务端碰撞管线

`Main.unity` 带一个 `AtlasServerColliders` 根，标记
`ServerColliderAuthoring(exportToServer = true)`：地面 slab、每种可导出形状各一个
障碍，以及一块 OW 式 PVP 白盒（带四个门洞的外圈围墙、带 0.4 m 台阶的中央高地、
对称掩体和错位 flank 走廊）。菜单 **Atlas → MVP → Seed Server Test Colliders**
可重建它；`MvpSpace` 加载 cook 产物，让 demo 拥有真实的服务端权威障碍。

把场景的服务端 collider 导出成 Atlas collision asset v2 JSON：

```bash
tools/bin/export_collision_unity.sh  --output samples/mvp/maps/main.collision.json --scene Assets/Scenes/Main.unity
tools\bin\export_collision_unity.bat --output samples\mvp\maps\main.collision.json --scene Assets/Scenes/Main.unity
```

`--scene` 必填 —— batch mode 启动的是无标题空场景，不传它 exporter 会直接报错而
非静默导出 0 个对象。exporter 扫描 `exportToServer = true` 的
`ServerColliderAuthoring`：

- `BoxCollider`（轴对齐）→ `{shape: box, min, max, layer}`。
- `SphereCollider`（uniform scale）→ `{shape: sphere, center, radius, layer}`。
- `CapsuleCollider`（Y 轴 + uniform scale）→ `{shape: capsule, center,
  radius, half_height, layer}`。
- `MeshCollider`：非 convex → `{shape: mesh, ...}`；convex → `{shape: convex,
  ...}`（点云凸包）。两者都把 world-space 顶点写进同名 `.bin` 侧车
  （`main.collision.json` ↔ `main.collision.bin`），顶点已 bake，任意旋转/缩放都可。
  服务端分别用 Jolt 的三角 `MeshShape` / `ConvexHullShape` —— convex collider 的
  服务端形状就是凸包，与 Unity 自身行为一致。
- `TerrainCollider` → `{shape: heightfield, ...}`：完整 heightmap 导成 N×N 样本
  网格写进 `.bin`（N = heightmap 分辨率）；Jolt 把样本数向上取整到自身 block size
  并以无碰撞 padding 补齐。Terrain holes 导成 `FLT_MAX` 样本。
- 旋转的 box、非 uniform / 非 Y 轴 capsule 与负缩放都会打 warning 并跳过。

`run_mvp_cluster` 启动时会把 `samples/mvp/maps/main.collision.json` cook 成
`.collisioncache`，`MvpSpace.OnSpaceInit` 通过 Jolt backend 加载它；cache 缺失
时 space 保留平地。手动 cook：

```bash
bin\debug\atlas_tool.exe cook_collision samples\mvp\maps\main.collision.json -o samples\mvp\maps\main.collisioncache
# → samples\mvp\maps\main.collisioncache
```

`atlas_tool recook --invalid <dir>` 只支持默认命名约定
（`foo.collision.json` -> `foo.collision.collisioncache`）。MVP 使用自定义
`main.collisioncache` 路径，因此刷新它要重新运行 `run_mvp_cluster`，或手动执行
上面的 `cook_collision ... -o samples/mvp/maps/main.collisioncache` 命令。

## 服务端导航网格 + NPC 寻路

`MvpSpace.OnSpaceInit` 还会用原始 `main.collision.json` 加 `main.nav.json`
参数 sidecar 在内存里 bake 一张 navmesh（`CellServerEntity.LoadNavMesh`）。
bake 成功后 NPC 漫游改用 `NavMoveTo`——路径会穿门洞、上台阶、绕掩体，
而不是贴墙滑行。无 navmesh（如 `ATLAS_ENABLE_RECAST=OFF`）时 NPC 退回
原来的直线漫游。

命令行检查 navmesh 或某条具体路线：

```bash
bin\debug\atlas_tool.exe cook_nav samples\mvp\maps\main.collision.json --params samples\mvp\maps\main.nav.json
bin\debug\atlas_tool.exe dump_nav samples\mvp\maps\main.collision.json --params samples\mvp\maps\main.nav.json --obj nav.obj
bin\debug\atlas_tool.exe path_nav samples\mvp\maps\main.collision.json --params samples\mvp\maps\main.nav.json --from -30,0,-30 --to 0,2,16 --obj path.obj
```

## UE 客户端

Unreal Engine 5.x 客户端，通过 Atlas wire 协议连接同一个 MVP 集群。Plugin
直接链接 `atlas_net_client.dll` + `atlas_entitydef_client.dll`，**不**使用 UE
的 replication / RPC。当前路径覆盖登录 / 自动登录、HUD 与 Avatar Blueprint
view 基类、SpaceData / NPC count、generated RPC、属性 delta / component decode，
以及 owner-authoritative movement input + native predictor / ack replay。剩余 UI
和工具缺口以 `samples/mvp/UEClient/README.md` 为准。

### 前置条件

- 源码构建的 Unreal Engine 5.x，设置 `UE_ROOT` 指向引擎根目录（例：`set UE_ROOT=E:\UE\UnrealEngine`）
- 与服务端构建同样的 Visual Studio 2026 / Visual Studio 2022 17.14+ +
  .NET 10 SDK + CMake 工具链

### 构建 + staging

```bash
tools/bin/build_mvp_ue.sh
tools\bin\build_mvp_ue.bat
```

端到端流水线：CMake 构建 `atlas_net_client.dll` + `atlas_entitydef_client.dll`，运行 `Atlas.Tools.DefDump` 抽出 `entity_defs.bin`（生产 + 测试两份 ATDF），`Atlas.Tools.CppEmitter` 生成 `samples/mvp/UEClient/Source/UEClient/gen/<Entity>.gen.h`，全部 stage 到 plugin 的 `ThirdParty/`，最后 UBT 编 `UEClientEditor`。`--config Debug` 对应 Debug SDK；分段 skip flag（`--skip-native` / `--skip-defs` / `--skip-codegen` / `--skip-stage` / `--skip-ue`）支持局部重跑。

### 运行

```bash
# 1. 先启动 MVP 集群（见上面的"运行"小节）。

# 2. 用 Unreal Editor 打开工程：
"%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe" samples\mvp\UEClient\UEClient.uproject

# 3. 编辑器中点 Play（PIE）。UEClientGameMode 会向 127.0.0.1:20018 发起 Login
#    + Authenticate（账号 mvp_<guid>，密码 hash mvp_hash），认证成功后把玩家
#    entity 转为 atlas::mvp::Account，通过 codegen 生成的 typed stub
#    调用 SelectAvatar(1)。
```

`UEClientGameMode` 的 host / port / 凭据是 `UPROPERTY` 默认值——如果集群跑在非默认端口，在 World Settings 中调整。

### 验收信号

UEClient Output Log 按顺序应该出现：

```
LogAtlasUE: Loaded ATDF from ...entity_defs.bin; digest_size=32
LogAtlasUE: AtlasUE module started; atlas_net_client ABI=0x02050000
LogUEClient: Atlas login host=127.0.0.1 port=20018 user=mvp_<guid>
LogUEClient: Atlas login succeeded, authenticating
LogUEClient: Sent Account.SelectAvatar(1) entity_id=<n>
```

同时 Unity 和 UE 两个客户端连到同一集群时，UE 视图会流式接入 Unity 玩家的 `AAvatarCapsule`（目前是 `/Engine/BasicShapes/Cylinder.Cylinder` 占位 mesh）。属性 delta（HP、level…）经 codegen 的 `OnHpChanged` 虚函数桥到 `UAtlasAvatarView`，BP 中直接 `Bind Event to On Hp Changed` 即可；同一条路径覆盖 `client_methods`（伤害浮字、投射物 spawn / end、复活等）。任何想暴露 Atlas BP 表面的 Actor 添加一个 `UAtlasAvatarView` component，gamemode 工厂会在 entity 入场时把 view 和 entity 绑定起来。

Unity / UE owner movement 发送量化输入帧，并用 native `movement_sim` 本地预测；
CellApp 持有最终位置权威，并通过 movement ack 驱动 replay。owner 在 replay
后回报 correction tier，只用于服务端 watcher，不参与权威位置。重连后
movement ack 也会播种下一帧 owner input seq，避免旧 CellApp 输入序列状态把
新会话输入当成 stale。Peer avatar 仍走 `AvatarFilter` 插值轨迹，技能期间由
`MovementCommandStart` / `MovementCommandEnd` 覆盖 filter 输出。NPC AI 写入
movement intent，并由同一套服务端 `CellMovementSystem` / `movement_sim::Step`
路径推进。Unity 可调用
`AtlasNetworkManager.SetTransportImpairment(75, 200, seed)`，UE 可调用
`UAtlasSubsystem::SetTransportImpairment(75, 200, seed)`，在约 150 ms RTT /
2% datagram loss 下验证同一路径。

`UAtlasSubsystem` 断线时按指数退避自动重连（1 s → 2 → 4 → … 上限 30 s）。
首次成功 `BeginLogin` 时缓存凭据供 subsystem 回放；重试前清掉 EntityManager，
stale actor 不会残留在死掉的 net ctx 上。需要把重试交给游戏层（例如未来有
LoginScreen UMG），关掉 `bAutoReconnectEnabled` 即可。

### UE 客户端缺口

UE 专属 UI 与相机缺口以 `samples/mvp/UEClient/README.md` 为准；下方操作表
描述的是 Unity MVP 客户端。

## 操作

| 输入 | 动作 |
|---|---|
| **W A S D** | 移动（相机相对方向；W = 相机前方） |
| **鼠标 — 按住右键** | 环绕相机（yaw + pitch ±25°/60°），锁定光标 |
| **鼠标 — 滚轮** | 缩放（0.5×–12×） |
| **Space** | 朝面向方向发射投射物（命中 10 点伤害） |
| **Left Shift** | 通过服务端权威 MovementCommand 向前冲刺 |
| **点击聊天输入框** | 输入消息（Enter 发送，Esc 取消） |
| **1 / 2 / 3** | 装备 Sword / Bow / Staff（服务端经 EquipmentComponent 处理） |

玩家行走时，相机 yaw 会平滑追上 Avatar 朝向（约 250 ms 阻尼），类似 PUBG。横移（A/D）保持 Avatar 朝前，避免相机跟随环路抖动。

## 玩法表面

- **Avatar**（青色胶囊）：100 HP，死亡 3 s 后在原点复活。死亡会清除当前
  active MovementCommand，确保冲刺表现先结束再进入复活流程。每击杀一个 NPC
  获得 +10 gold，每累计 50 gold 提升 1 级。Gold/Level 在 cell 侧
  （scope=`own_client`，`persistent="true"`），按 delta 复制到 owner 客户端，
  HUD 显示 `LV` / `GOLD` 行。
- **NPC**（灰色胶囊）：世界启动时生成 150 个，散布在 ±100 m 内，以 3 m/s 游荡，每 4 s 重新选点。每 3–7 s 朝面向方向发射投射物。100 HP，不复活，死亡后保持死亡直到集群重启。
- **Projectile**：不是复制实体。服务端运行确定性的弹道模拟器（重力 12，生命周期 2 s，命中半径 1.5 m）；客户端从发射 RPC 开始本地积分同一条弧线。结束事件（命中 / 落地 / 过期）会携带服务端权威结束位置广播，让命中特效落在实际结算伤害的位置。
- **HUD**：`LabelOverlay` 把每个实体的名字 + HP 标签投影到屏幕；伤害数字从打击点上浮并淡出。左上角显示 fps / ping / NPC 计数。**F1** 切换右下角 debug 面板：双向带宽（KB/s）、RUDP 发送队列深度、客户端→服务端 RPC 速率、AoI 进入/离开速率，以及 `show AoI` 开关。启用 AoI 调试后，绿色框是进入边界（50 m），黄色框是离开边界（55 m，滞回）。

## 架构概览

`Bootstrap` 构建运行时场景（地面、光照、相机、根 overlay），调用
`AtlasNetworkManager.Login` → `Authenticate` → 执行
`Account.Base.SelectAvatar(1)` RPC。服务端 `Account.SelectAvatar` 创建玩家
`Avatar`（base + cell 对应体），通过 `GiveClientTo` 绑定客户端。
`MvpBootstrap : IAtlasAppInitializer` 在脚本 app init 时把 `MvpSpace`
注册为 space master；space 1 创建时，primary CellApp 会自动生成这个
`CellSpaceEntity`。`MvpSpace.OnSpaceInit` 散布 150 个 NPC，并用 AtlasLoop
timer 维护补充循环：存活数 ≤ 150 时启动补充，每 2 秒生成 1 个，到达 250
后停止。每次计数变化时通过
`SpaceData.SetInt32(SpaceId, SpaceDataKeys.NpcCount, n)` 发布到 cellapp 的
SpaceData，cell→client envelope 广播给该 space 内所有客户端的
`SpaceDataManager`。base 侧 `Avatar.OnInit` 设置 50 m AoI 半径，因此客户端会
流式接收范围内实体。移动是服务端权威：`PlayerInputController` 把 WASD /
joystick 采样成 native 输入帧，本地预测，发送当前帧和最近两帧冗余，并在
`MovementStateAck` 后 replay 未确认输入、上报 correction tier telemetry。
Left Shift 触发 `Avatar.Dash`，由 cell 脚本写入 server-stamped
`MovementCommand`，并通过 `MovementCommandStart` / `MovementCommandEnd`
广播给 owner 和 peer。Dash command 使用 cell 脚本注册的显式 movement curve；
死亡会调用 `ClearMovementCommand`，让进行中的冲刺通过同一条
`MovementCommandEnd` 路径带 `cancelled` 结束原因后再复活。Unity / UE 也在
客户端本地注册同一 curve id，owner prediction 和 remote overlay 都会按它采样；
当前样本仍是线性。
投射物伤害是服务端权威：`LaunchProjectile` 把一次射击注册到每个 space 的
`ProjectileSimulator`，由它在每个 cellapp tick 积分，并广播
`OnProjectileFired` / `OnProjectileEnded` RPC，让 AoI 内所有客户端渲染同步弧线。
属性复制（HP、NpcType 等）走常规 delta pump。

### 组件系统

`EquipmentComponent`（component_id=2）以 `equipment` 名挂在 Avatar 上（`scope="all_clients"`）。它含 1 个复制属性 `weaponId`（int32 / all_clients / reliable）和 1 个 cell 方法 `EquipWeapon(weaponId)`（exposed=`own_client`）。按 **1 / 2 / 3** 走 `avatar.Equipment.EquipWeapon(N)`，cell 侧 partial 校验范围后写入 `WeaponId`，下一帧组件 delta 把变更回流到 `ReplicatedComponent.ApplyDelta` → `OnWeaponIdChanged` → `EquipmentComponent.WeaponChanged` 静态事件 → HUD 的 `WEAPON` 行。AoI 内的其它客户端也会收到同一 slot 的 delta，实时看到对方换武器。

### 聊天

`Avatar` 暴露 cell 方法 `Say(text)`（`exposed="own_client"`，服务端 200 字符上限），通过 `AllClients.OnChat(senderId, text)` 扇出给 AoI 内所有观察者。HUD 左下角是聊天面板（5 行滚动 + 输入框）；点击输入框聚焦后会阻塞 WASD/Space，避免移动和射击混进消息。两个客户端同时连接时彼此可见，端到端验证 own_client → cell → AllClients 广播链路。

### 持久化覆盖

`Account.loginCount` 与 `Account.lastLogin` 标记为 `persistent="true"`，会自动经 DBApp 往返：`Account.SelectAvatar` 绑定角色时自增计数、写入 UTC 时间，并在交出客户端前主动刷新 Account。logoff 时 DBApp 仍会持久化最终 Account 快照，下次会话通过 `CheckoutEntity` 重新加载。HUD 的 `SESSION` 行显示 `#N · MM-DD HH:MM`（本次会话编号 + 上次会话的 UTC 时间），是 DBApp 保存/加载循环正常工作的最直观确认。

`Avatar.gold` 与 `Avatar.level` 同样声明了 `persistent="true"`，logoff 时确实会写入 DBApp（自动分配 DBID），但 MVP 的 `Account.SelectAvatar` 总是调用 `EntityFactory.CreateBase("Avatar")`，每次铸造一个全新的 Avatar，并不会按上一次的 DBID 走 checkout 加载。因此会话内 Gold/Level 的变更 + 复制按预期工作，但跨会话会重置为初值。补齐 `Account → avatarDbid` + `CreateBaseFromDbid` checkout 通路即可闭环，留作后续。

`LoginFlow` 会缓存最后一次提交的凭证；遇到非用户主动的断线，会以 1 / 2 / 4 / … / 30 s 指数退避自动重连（与 `UAtlasSubsystem` 行为一致）。登录界面显示剩余秒数；用新凭证点 LOGIN 会取消挂起的重连，HUD 的 `RETURN` 按钮则触发清晰登出并停用自动重连。

## Headless bot 压测

`build_mvp_unity` 产出 `out/mvp-unity/windows/AtlasMvp.exe` 后，可对运行中的集群扇出 N 份独立 Unity 玩家进程：

```bash
tools/bin/run_mvp_unity_bots.sh -n 4 --duration 60
tools\bin\run_mvp_unity_bots.bat -n 4 --duration 60
```

每个 bot 以 `mvp_bot_<idx>` 登录（LoginApp 开发模式接受任意用户名），等进入世界后由 `BotPilot` 接管 `PlayerInputController` 的 joystick/fire 源，跑随机朝向的游走 + 周期开火——cell 侧负载与真实键盘玩家一致。bot 到达指定 duration 后调用 `Application.Quit()` 退出。400 ms 启动间隔默认满足 LoginApp 5/60s 每 IP 上限；要扇出更多，重启集群时加 `--login-rate-limit-per-ip 0`（`run_world_stress` 参数）。每个 bot 的 Unity 日志写入 `out/mvp-unity-bots/bot_<idx>.log`。

与 headless `run_world_stress` / `run_login_stress` 驱动不同，这个驱动每个 bot 都跑完整的 Unity runtime（managed GC、`Atlas.Client.Unity` 桥、Mvp.Client codegen + ApplyDelta），能暴露协议压测工具看不到的 Unity 侧开销。

## 多 cellapp 部署

wrapper 默认 `--cellapp-count 4`（2×2 BSP 网格）；要单 CellApp 测试传 `1`。

```bash
tools/bin/run_mvp_cluster.sh
tools\bin\run_mvp_cluster.bat
```

第一次脚本驱动创建 cell 实体时（Avatar 经 `Account.SelectAvatar`），BaseApp 会发 `CreateSpaceRequest` 给 CellAppMgr，`initial_cell_count = 当时已注册的 cellapp 数`（上限 16）。CellAppMgr 调 `BSPTree::Split` N-1 次——交替 X/Z 轴、位置 0——产出 N 个叶子。4 cellapp 时落成 2×2 网格：(x<0,z<0)、(x≥0,z<0)、(x<0,z≥0)、(x≥0,z≥0)。每个 cellapp 收到一条 `AddCellToSpace` 加广播的 `UpdateGeometry`。BaseApp 把待发的 `CreateCellEntity` 排队，等 `SpaceCreatedResult` 回来后投到 primary host；落点不在该 leaf 的实体由接收方 CellApp 的 OffloadChecker 按 position 迁移。

cellappmgr 日志关键字确认：

```
CellAppMgr: created Space 1 with 4 cell(s); primary host app_id=...
```

各 cellapp 端 (`cellapp.stdout` / `cellapp_NN.stdout`)：

```
CellApp: added Cell N to Space 1
CellApp: ... received offload entity / converted ghost↔real ...
```

`BSPTree::Balance` 循环（`CellAppMgr::OnTickComplete` 每 30 ticks 一次）会持续调整 split 位置以均衡叶子间负载。配合 `run_mvp_unity_bots -n 16` 以上把负载推高，能在 `UpdateGeometry` 的重广播里观察到 split 线移动。

**客户端没有 cell-id 属性——这是有意设计。** Phase 11 保证 Ghost 迁移对客户端透明：C# Ghost mirror 是服务端被动副本，Witness 看到的是一个稳定的 Avatar，不论 Real 当下在哪个 CellApp。所以没有也不该有客户端可见的"当前 cell"，BigWorld 模型也不鼓励加这种属性。

**底层基础验证测试**（与 MVP 解耦）：

```bash
bin/debug/test_bsp_tree.exe                  # BSP 路由、balance、serialize、unsplit
bin/debug/test_distributed_space.exe         # over-RUDP：CreateGhost / GhostPositionUpdate /
                                             #   GhostDelta / OffloadEntity + SpaceData mirror
bin/debug/test_offload_traversal.exe         # BSP-driven offload / teleport 用例
bin/debug/test_cellappmgr_integration.exe    # late split 与 multi-cell bootstrap
```

## 已知 MVP 简化点

- 移动已走服务端权威输入帧、owner replay 和共享 `movement_sim::Step` 路径；Static / Jolt
  PhysicsQuery 已覆盖坡面、台阶、depenetration 和大纠正审计。`run_mvp_cluster`
  会 cook 并加载 Unity 导出的 collision cache。Static chunk / border query 已进入
  回归基线；volume export 仍是后续项。
- MVP `Avatar.ReportPos` 已移除；仅压测用 `StressAvatar.ReportPos`
  保留给 legacy load generation。
- NPC AI 通过 movement intent 随机游走 + 周期性开火；没有仇恨 / 目标选择。
- NPC 死亡后按数量补充（≤ 150 触发，每 2 秒 1 个，封顶 250），但死亡实例本身不复活。
- 登录使用硬编码 username + password hash；LoginApp 的 dev 模式会接受任何符合配置的输入。
- `ProjectileSimulator` 状态是 per-space 但仍在进程内；没有实现跨 cell 投射物 handoff。
- CellAppMgr 已可在 late CellApp 注册时拆分已有 Space；MVP 简化点是
  投射物仍未实现跨 cell handoff。

## 排障

- **Unity 编译报缺少 `Atlas.Mvp.Client`** — 你忘了运行 `setup_mvp_unity`，或者打包时用了带 `--skip-setup` 的 `build_mvp_unity`。
- **`build_mvp_unity` 找不到 Unity** — 传 `--unity <Unity.exe>` 或设置 `UNITY_EXE`；同时确认 `ProjectSettings/ProjectVersion.txt` 存在，并且对应版本的 Editor 已安装。
- **`Login failed: BadCredentials`** — loginapp 配置里的 `accept_any_user` 关闭了。开发环境可以打开它，或者预先创建用户。
- **看不到 NPC** — 检查 cellapp 日志是否有 `MvpSpace: seeded 150/150 NPCs`；如果显示 0，说明 CellApp 缺少 Mvp.Cell DLL（重新运行 `build.bat debug`，让 CMake 重新部署）。
- **标签卡在屏幕边缘 / 位置不对** — Unity 场景模板自带的 `Main Camera` GameObject 与 Bootstrap 运行时相机冲突。删除层级面板中的默认 Main Camera。
