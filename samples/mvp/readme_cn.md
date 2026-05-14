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

脚本会按顺序从 `--unity`、`UNITY_EXE`、`UNITY_PATH`、固定工程版本对应的 Unity Hub 安装路径查找 Unity 可执行文件。Windows 默认输出为 `out/mvp-unity/windows/AtlasMvp.exe`，日志写到 `out/mvp-unity/unity-build.log`。非 Windows player 可传 `--target StandaloneLinux64` 或 `--target StandaloneOSX`。Standalone 构建默认以可调整大小的窗口启动。

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

`Bootstrap` 构建运行时场景（地面、光照、相机、根 overlay），调用 `AtlasNetworkManager.Login` → `Authenticate` → 执行 `Account.Base.SelectAvatar(1)` RPC。服务端 `Account.SelectAvatar` 创建玩家 `Avatar`（base + cell 对应体），通过 `GiveClientTo` 绑定客户端，然后执行一次 `WorldBootstrap.EnsureSpawned` 散布 50 个 NPC。base 侧 `Avatar.OnInit` 设置 50 m AoI 半径，因此客户端会流式接收范围内实体。移动是客户端权威：`PlayerInputController` 每帧写入 `transform.position`，并以 20 Hz 通过 `Avatar.Cell.ReportPos` 上报到 cell。投射物伤害是服务端权威：`LaunchProjectile` 把一次射击注册到每个 space 的 `ProjectileSimulator`，由它在每个 cellapp tick 积分，并广播 `OnProjectileFired` / `OnProjectileEnded` RPC，让 AoI 内所有客户端渲染同步弧线。属性复制（HP、NpcType 等）走常规 delta pump。

## 已知 MVP 简化点

- 移动是客户端权威（没有反作弊）。生产环境需要服务端校验器 + 最大速度检查。
- NPC AI 是随机游走 + 周期性开火；没有仇恨 / 目标选择。
- NPC 不复活，死亡后保持死亡直到集群重启。
- 登录使用硬编码 username + password hash；LoginApp 的 dev 模式会接受任何符合配置的输入。
- `ProjectileSimulator` 状态是 per-space 但仍在进程内；没有实现跨 cell 投射物 handoff。

## 排障

- **Unity 编译报缺少 `Atlas.Mvp.Client`** — 你忘了运行 `setup_mvp_unity`，或者打包时用了带 `--skip-setup` 的 `build_mvp_unity`。
- **`build_mvp_unity` 找不到 Unity** — 传 `--unity <Unity.exe>`，或者把 `UNITY_EXE` 设置为 `ProjectSettings/ProjectVersion.txt` 中固定版本的 Unity Editor。
- **`Login failed: BadCredentials`** — loginapp 配置里的 `accept_any_user` 关闭了。开发环境可以打开它，或者预先创建用户。
- **看不到 NPC** — 检查服务端日志是否有 `WorldBootstrap: queued 50/50 NPC spawns`；如果显示 0，说明 BaseApp 缺少 Mvp.Base DLL（重新运行 `build.bat debug`，让 CMake 重新部署）。
- **标签卡在屏幕边缘 / 位置不对** — Unity 场景模板自带的 `Main Camera` GameObject 与 Bootstrap 运行时相机冲突。删除层级面板中的默认 Main Camera。
