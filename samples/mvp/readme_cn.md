# Atlas MVP — Unity 端到端演示

单 Cell Atlas 集群 + Unity 6 客户端，用来演示完整的 BigWorld 风格链路：客户端权威移动、服务端权威技能 / 投射物伤害、AoI 驱动的同屏实体流式同步，以及逐 tick 属性复制。世界尺寸为 200 × 200 m，包含 1 个玩家 Avatar 和 50 个游荡 NPC。

## 目录结构

```
samples/mvp/
├── Atlas.Mvp.Base/      # BaseApp 脚本：Account.SelectAvatar、Avatar AoI 配置
├── Atlas.Mvp.Cell/      # CellApp 脚本：Avatar、Npc、NpcAiComponent、ProjectileSimulator
├── Atlas.Mvp.Client/    # netstandard2.1 + net10.0 — Unity 与 atlas_client.exe 共用
└── UnityClient/         # Unity 6 LTS 工程；Bootstrap.cs 运行时构建场景
```

`entity_defs/{Avatar,Npc,Account}.def` 定义实体表面；`Avatar` 是 base + cell 实体，`Npc` 仅存在于 cell。

## 前置条件

- Unity Hub + Unity 6 LTS（工程固定为 `6000.0.28f1c1`）
- .NET 10 SDK
- CMake + Ninja（构建 helper 会按需准备 Ninja）
- Windows：MSVC 2022（构建 helper 会加载环境）。Linux/macOS：clang 或 gcc。

## 运行

```bash
# 1. 构建原生服务器 + 脚本 DLL（把 Atlas.Mvp.{Base,Cell}.dll 部署到 bin/debug/）。
tools/bin/build.sh debug
tools\bin\build.bat debug

# 2. 把 SDK + 实体 DLL staging 到 samples/mvp/UnityClient/Assets/。
tools/bin/setup_mvp_unity.sh
tools\bin\setup_mvp_unity.bat

# 3. 启动集群（machined + loginapp + baseappmgr + baseapp + cellappmgr + cellapp + dbapp）。
tools/bin/run_mvp_cluster.sh
tools\bin\run_mvp_cluster.bat

# 4. 在 Unity Hub 中打开 samples/mvp/UnityClient，然后点击 Play。
```

两端默认使用 LoginApp 端口 `20018`。集群中 cellapp / baseapp 以 20 Hz 运行（50 ms tick）。`UnityClient/Assets/Scenes/Main.unity` 已经包含运行时 `Bootstrap` 对象。

## 打包 Unity 客户端

```bash
# 一步完成 staging + Unity batchmode player build。
tools/bin/build_mvp_unity.sh
tools\bin\build_mvp_unity.bat

# 如果 Assets 已经 staging 好，可以跳过 setup 阶段。
tools/bin/build_mvp_unity.sh --skip-setup --clean-output
tools\bin\build_mvp_unity.bat --skip-setup --clean-output
```

脚本会按顺序从 `--unity`、`UNITY_EXE`、`UNITY_PATH`、固定工程版本对应的 Unity Hub 安装路径查找 Unity 可执行文件。如果缺少 `ProjectSettings/ProjectVersion.txt`，脚本会扫描本机 Unity Hub 已安装的 Editor 并打印最终选中的可执行文件。Windows 默认输出为 `out/mvp-unity/windows/AtlasMvp.exe`，日志写到 `out/mvp-unity/unity-build.log`。非 Windows player 可传 `--target StandaloneLinux64` 或 `--target StandaloneOSX`。Standalone 构建默认以可调整大小的窗口启动。

## UE 客户端（M1 — codegen 驱动的属性同步）

Unreal Engine 5.7 客户端，通过 Atlas wire 协议连接同一个 MVP 集群。Plugin 直接链接 `atlas_net_client.dll` + `atlas_entitydef_client.dll`，**不**使用 UE 的 replication / RPC。M1 之后：registry-driven 通用 `ApplyDelta` 解码 scalar / struct / list / dict delta；`Atlas.Tools.CppEmitter` 输出 typed entity 类（`atlas::mvp::Account`、`Avatar`、`Npc`），带属性 getter + 上行 RPC stub。下行 RPC dispatch + BP 暴露在 M2。

### 前置条件

- 源码构建的 Unreal Engine 5.7，设置 `UE_ROOT` 指向引擎根目录（例：`set UE_ROOT=E:\UE\UnrealEngine`）
- 与服务端构建同样的 MSVC 2022 + .NET 10 SDK + CMake 工具链

### 构建 + staging

```bash
tools/bin/build_mvp_ue.sh
tools\bin\build_mvp_ue.bat
```

端到端流水线：CMake 构建 `atlas_net_client.dll` + `atlas_entitydef_client.dll`，运行 `Atlas.Tools.DefDump` 抽出 `entity_defs.bin`（生产 + 测试两份 ATDF），`Atlas.Tools.CppEmitter` 生成 `samples/mvp/UEClient/Source/UEClient/gen/<Entity>.gen.h`，全部 stage 到 plugin 的 `ThirdParty/`，最后 UBT 编 `UEClientEditor`。`--config Debug` 对应 Debug SDK；分段 skip flag（`--skip-native` / `--skip-defs` / `--skip-codegen` / `--skip-stage` / `--skip-ue`）支持局部重跑。

### 运行

```bash
# 1. 先启动 MVP 集群（见上面的"运行"小节）。

# 2. 用 UE 5.7 打开工程：
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
LogAtlasUE: AtlasUE module started; atlas_net_client ABI=0x02000000
LogUEClient: Atlas login host=127.0.0.1 port=20018 user=mvp_<guid>
LogUEClient: Atlas login succeeded, authenticating
LogUEClient: Sent Account.SelectAvatar(1) entity_id=<n>
```

同时 Unity 和 UE 两个客户端连到同一集群时，UE 视图会流式接入 Unity 玩家的 `AAvatarCapsule`（目前是 `/Engine/BasicShapes/Cylinder.Cylinder` 占位 mesh），并每帧应用 AvatarFilter 插值后的 transform。属性 delta（HP、level…）通过 registry-driven 路径解码到 `atlas::mvp::Avatar` 槽位上，view 层接入后即可通过 `avatar->Hp()` 等 getter 读取。

### M1 之后的已知缺口

- **没有移动输入** —— UE 端 Avatar 待在服务器分配的位置；codegen 已生成 `Avatar.ReportPos` 上行 stub，但还没接到 controller（M3）
- **断线不重连** —— `on_disconnect` 只打日志
- **`AAvatarCapsule` 是 Cylinder 占位** mesh，作为美术 polish 项替换
- **下行 RPC（`Avatar.ShowDamage` / `OnDied` 等）尚未派发** —— `0xF004` envelope 路由 + 每 entity 的 `DispatchRpc` 留给 M2
- **没有 BP 暴露** —— codegen 输出当前是纯 C++；M2 加 `UAtlasAvatarView` UCLASS bridge 提供 `GetHp` / `OnHpChanged`

## 操作

| 输入 | 动作 |
|---|---|
| **W A S D** | 移动（相机相对方向；W = 相机前方） |
| **鼠标 — 按住右键** | 环绕相机（yaw + pitch ±25°/60°），锁定光标 |
| **鼠标 — 滚轮** | 缩放（0.5×–12×） |
| **Space** | 朝面向方向发射投射物（命中 10 点伤害） |

玩家行走时，相机 yaw 会平滑追上 Avatar 朝向（约 250 ms 阻尼），类似 PUBG。横移（A/D）保持 Avatar 朝前，避免相机跟随环路抖动。

## 玩法表面

- **Avatar**（青色胶囊）：100 HP，死亡 3 s 后在原点复活。
- **NPC**（灰色胶囊）：世界启动时生成 50 个，散布在 ±100 m 内，以 3 m/s 游荡，每 4 s 重新选点。每 3–7 s 朝面向方向发射投射物。100 HP，不复活，死亡后保持死亡直到集群重启。
- **Projectile**：不是复制实体。服务端运行确定性的弹道模拟器（重力 12，生命周期 2 s，命中半径 1.5 m）；客户端从发射 RPC 开始本地积分同一条弧线。结束事件（命中 / 落地 / 过期）会携带服务端权威结束位置广播，让命中特效落在实际结算伤害的位置。
- **HUD**：`LabelOverlay` 把每个实体的名字 + HP 标签投影到屏幕；伤害数字从打击点上浮并淡出。右上角显示 fps / ping 和 `show AoI` 开关。AoI 调试默认隐藏；启用后，绿色框是进入边界（50 m），黄色框是离开边界（55 m，滞回）。

## 架构概览

`Bootstrap` 构建运行时场景（地面、光照、相机、根 overlay），调用 `AtlasNetworkManager.Login` → `Authenticate` → 执行 `Account.Base.SelectAvatar(1)` RPC。服务端 `Account.SelectAvatar` 创建玩家 `Avatar`（base + cell 对应体），通过 `GiveClientTo` 绑定客户端。cell 侧 `Avatar.OnInit` 通过 `SpaceOwnerRegistry` 检查后按需 `EntityFactory.CreateLocalCell("MvpSpace", ...)` 创建 space-owner entity；`MvpSpace : CellSpaceEntity` 在 `OnSpaceInit` 散布 50 个 NPC，并用 AtlasLoop timer 维护补充循环：存活数 ≤ 50 时启动补充，每 3 秒生成 1 个，到达 80 后停止。每次计数变化时通过 `SpaceData.SetInt32(SpaceId, SpaceDataKeys.NpcCount, n)` 发布到 cellapp 的 SpaceData，cell→client envelope 广播给该 space 内所有客户端的 `SpaceDataManager`。base 侧 `Avatar.OnInit` 设置 50 m AoI 半径，因此客户端会流式接收范围内实体。移动是客户端权威：`PlayerInputController` 每帧写入 `transform.position`，并以 20 Hz 通过 `Avatar.Cell.ReportPos` 上报到 cell。投射物伤害是服务端权威：`LaunchProjectile` 把一次射击注册到每个 space 的 `ProjectileSimulator`，由它在每个 cellapp tick 积分，并广播 `OnProjectileFired` / `OnProjectileEnded` RPC，让 AoI 内所有客户端渲染同步弧线。属性复制（HP、NpcType 等）走常规 delta pump。

## 已知 MVP 简化点

- 移动是客户端权威（没有反作弊）。生产环境需要服务端校验器 + 最大速度检查。
- NPC AI 是随机游走 + 周期性开火；没有仇恨 / 目标选择。
- NPC 死亡后按数量补充（≤ 50 触发，每 3 秒 1 个，封顶 80），但死亡实例本身不复活。
- 登录使用硬编码 username + password hash；LoginApp 的 dev 模式会接受任何符合配置的输入。
- `ProjectileSimulator` 状态是 per-space 但仍在进程内；没有实现跨 cell 投射物 handoff。

## 排障

- **Unity 编译报缺少 `Atlas.Mvp.Client`** — 你忘了运行 `setup_mvp_unity`，或者打包时用了带 `--skip-setup` 的 `build_mvp_unity`。
- **`build_mvp_unity` 找不到 Unity** — 传 `--unity <Unity.exe>` 或设置 `UNITY_EXE`；没有 `ProjectVersion.txt` 时脚本会先扫描 Unity Hub 安装目录。
- **`Login failed: BadCredentials`** — loginapp 配置里的 `accept_any_user` 关闭了。开发环境可以打开它，或者预先创建用户。
- **看不到 NPC** — 检查 cellapp 日志是否有 `MvpSpace: seeded 50/50 NPCs`；如果显示 0，说明 CellApp 缺少 Mvp.Cell DLL（重新运行 `build.bat debug`，让 CMake 重新部署）。
- **标签卡在屏幕边缘 / 位置不对** — Unity 场景模板自带的 `Main Camera` GameObject 与 Bootstrap 运行时相机冲突。删除层级面板中的默认 Main Camera。
