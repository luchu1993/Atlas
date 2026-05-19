# Atlas MVP — UE 客户端

`samples/mvp/UnityClient` 的 Unreal 对应实现。相同的 wire 协议（RUDP + ATDF）
和相同的 MVP 功能集（Account 登录 → SelectAvatar → AoI / NPCs），由
`AtlasUE` plugin（`Plugins/AtlasUE/`）和精简的 game module
（`Source/UEClient/`）驱动。

英文版见 `README.md`。

## 前置

- **Unreal Engine** 5.x source build。Plugin 由 UBT 针对 `UEClient.uproject`
  指向的引擎编译；设环境变量 `UE_ROOT`（或传 `--ue-root`）到引擎根目录
  （含 `Engine/Build/BatchFiles/` 的那个）。
- **.NET 10 SDK** —— C# 服务端 / codegen / DefDump 流水线用。
- **Python 3.10+**（仅 stdlib）—— build 与启动 wrapper 用。
- **Visual Studio 2022/18 + MSVC C++ 工具链** —— native client SDK 编译。

## 一键本地开发循环

```sh
# 构建 cluster + plugin + ATDF，启动 cluster 并打开 UE Editor。
python tools/run_mvp_ue.py            # 默认 --config Release
```

Wrapper 流程：
1. 跑 `tools/build_mvp_ue.py`（build atlas_net_client.dll + ATDF +
   校验 digest 对比 `bin/<config>/Atlas.Mvp.Cell.dll`，再 build UEClientEditor）。
2. 后台启 MVP cluster（5 个服务端进程在 127.0.0.1，log 落
   `.tmp/world-stress/<时间戳>/logs/`）。
3. 用 UnrealEditor 打开 `UEClient.uproject` —— 点 **PIE** 即可游玩。
4. 关闭 UnrealEditor（或 Ctrl+C）后自动收 cluster。

`--no-build`、`--no-cluster`、`--no-ue` 可按需跳过单个步骤。

## 仅构建（不启动）

```sh
# Native + ATDF + UE plugin 重编；默认 config = Release。
python tools/build_mvp_ue.py
```

此阶段触发的不匹配守护：

- **ATDF ↔ cluster digest** —— DefDump 的 `--digest-only` 反射模式读
  `bin/<config>/Atlas.Mvp.Cell.dll` 的 `EntityDefDigest.Sha256Hex`，
  与 `entity_defs.bin` 内嵌 digest 比对。stale build cache 在此 fail，
  不会泄漏到 login-time `def_mismatch`。
- **Debug CRT** —— `--config Debug` 除非搭 `--build-config DebugGame` 否则
  被拒；UE Editor (Development) 不能加载 Debug-CRT DLL。

如果出 def_mismatch fail，错误信息已经给出修复命令：
`python tools/build.py <preset>` 然后不带 `--skip-defs` 重跑 `build_mvp_ue.py`。

## 在 Blueprint 里搭 UI

Plugin 提供 C++ 基类；BP 作者负责 layout / 美术。

**登录界面**（`UAtlasLoginWidget`）：
1. 新建 Widget Blueprint，**Reparent 到** `AtlasLoginWidget`。
2. 布局：Host / Port / Username 输入框 + Login 按钮。
3. Login 按钮 OnClicked → 调
   `BeginLoginFromFields(Host, Port, Username, PasswordHash)`。
4. Override 事件 `OnNetStateChanged`、`OnLoginFinished`、
   `OnReconnectScheduled`、`OnReconnectExhausted` 更新状态文本。
   失败原因用 `Atlas Subsystem → GetLastNetErrorMessage` 拿。
5. 在 `UEClientGameMode` 设 `LoginWidgetClass = BP_AtlasLogin`。
   GameMode 在 set 后跳过硬编码 auto-login，session 进 `Authenticated`
   时自动 dismiss widget。

**HUD**（`UAtlasHudWidget`）：
1. 新建 Widget Blueprint，**Reparent 到** `AtlasHudWidget`。
2. 布局:FPS / Ping / 带宽 / HP / NPC count 的 text block 或 progress bar。
3. Override `OnHudRefresh(Stats)` —— `Stats.Fps`、`Stats.Net.RttMs`、
   `Stats.Net.BytesSent/Received`、`Stats.PlayerEntityId`、`Stats.NpcCount`
   都是 `BlueprintReadOnly`。
4. HP / 等级 / 伤害飘字：调 `UEClientGameMode → GetPlayerAvatarView`
   再绑定 `UAtlasAvatarView` 的 delegates（`OnHpChanged`、`OnShowDamage` 等）。
5. 在 `UEClientGameMode` 设 `HudWidgetClass = BP_AtlasHud`。HUD 在
   `Authenticated` 后（登录 widget 被 dismiss 时）自动生成。

BP 端登出：调 `Atlas Subsystem → Logout`。Subsystem 关掉
`bAutoReconnectEnabled`、向 SDK 发 `ATLAS_DISCONNECT_LOGOUT`
让 server 释放 Account proxy；下次 `BeginLogin` 会重新 arm auto-retry。

## 与 Unity 客户端的功能差距

- 没有输入键位重映射 / 设置菜单。
- 没有聊天输入或回滚（服务端 `Avatar.Say()` RPC 在）。
- 没有装备 / 武器切换 UI。
- 没有伤害飘字 3D actor 或抛射物轨迹 VFX。
- 没有 bot 模式（`tools/run_mvp_unity_bots.py` 没有 UE 对应物）。
- Replay / playback 两端都没实现。

## 目录布局

- `Plugins/AtlasUE/Source/AtlasUE/Public/` —— plugin 头文件（subsystem、
  net client、login / HUD / SpaceData / Avatar view）。
- `Plugins/AtlasUE/Source/AtlasUE/Private/AtlasCore/` —— wire 解码器
  （AoI envelope、property decoder、RPC dispatch），与 Unity 共享。
- `Plugins/AtlasUE/ThirdParty/AtlasNetClient/Win64/` —— stage 的
  `atlas_net_client.dll` + `mimalloc.dll`（Release CRT）。
- `Plugins/AtlasUE/ThirdParty/AtlasEntityDef/` —— stage 的
  `atlas_entitydef_client.dll` + `entity_defs.bin`（ATDF v3 带 digest）。
- `Source/UEClient/` —— game module：GameMode、AvatarCapsule actor、
  输入控制器、`gen/` 下的 entity stubs。
