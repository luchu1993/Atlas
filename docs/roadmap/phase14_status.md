# Phase 14：现行 wire contract 与最小回归命令集

> 配合 [`phase14_movement_authority.md`](phase14_movement_authority.md) 使用：
> 主路线图承载目标 / 验收 / 里程碑，本文件承载现行 wire contract 与每次 PR 的
> 最小回归命令集。新 contract 收紧后在此追加。

---

## 已收紧的 wire contracts

- **movement input / forward payload**：entity id 必须非 0；
  `client_dt_ms` 必须在 1-250ms wire decode 范围内。
- **movement ack / command start / command end**：entity id 必须非 0；
  真实 command id 不能为 0。
- **MovementCommand**：`duration_ms` 必须非 0；
  `elapsed_ms <= duration_ms`；command 与 end state 坐标必须为有限值；
  enum 必须在协议范围内。
- **correction report**：target 必须非 0；distance 必须为有限非负值；
  flags 必须与 distance tier 一致。
- **movement ack / correction report 的 `correction_flags`**：必须落在
  `{0, Tier1, Tier2, Snap}` 单 tier 枚举集合，多 bit 组合或保留位
  C++ / C# / UE wire decode 直接 drop。
- **C++、C# 与 UE `AtlasNetClient`** 的 movement ack / command decode
  按同一 wire contract 对齐。

---

## 最小回归命令集合

每次 PR 推荐重跑这套作为最小回归：

- `tools\bin\build.bat debug --build-only`
- `ctest --test-dir build\debug -C Debug -R "movement|baseapp_messages|baseapp_movement|cellapp_handlers|cell_movement_system|net_client_abi_layout|jolt_physics_query|collision_pipeline|lag_compensation" --output-on-failure`
- `dotnet test tests\csharp\Atlas.Client.Tests\Atlas.Client.Tests.csproj
  --configuration Debug`
- `tools\bin\build_mvp_ue.bat --config Debug --ue-root E:\UE\UnrealEngine
  --target UEClientEditor --build-config Development --platform Win64 --skip-native
  --skip-defs --skip-codegen --skip-stage`
- `UnrealEditor-Cmd.exe` 跑 `Automation RunTests Atlas.NetClient`。

这套命令只代表 Phase 14 最小回归，不等同于完整验收。完整闭环还需要记录
50 / 100 / 400 `world_stress --move-mode input --movement-verify`、150ms RTT /
2% loss 双客户端，以及 Windows / Linux / Unity native predictor parity 的 PASS
日期和命令输出。
