# Phase 14：现行 wire contract 与最小回归命令集

> 状态: 当前 Phase 14 wire contract 与最小回归清单；2026-06-23 记录是
> 留档验收，不替代后续改动的本地复跑。
>
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

每次 PR 推荐重跑这套作为最小回归；如果 CTest 枚举 0 个测试，说明本地
`build/debug` cache 关闭过 `ATLAS_BUILD_TESTS`，先用 `--clean` 重建：

- `tools\bin\build.bat debug`
- `tools\bin\build_mvp_ue.bat --config Debug --skip-defs --skip-codegen
  --skip-stage --skip-ue`
- `ctest --test-dir build\debug -C Debug -R "movement|baseapp_messages|baseapp_movement|cellapp_handlers|cell_movement_system|net_client_abi_layout|jolt_physics_query|collision_asset|collision_pipeline|test_space|lag_compensation|backend_parity" --output-on-failure`
- `dotnet test tests\csharp\Atlas.Client.Tests\Atlas.Client.Tests.csproj
  --configuration Debug`
- `tools\bin\build_mvp_ue.bat --config Debug --ue-root <UE_ROOT>
  --target UEClientEditor --build-config Development --platform Win64 --skip-native
  --skip-defs --skip-codegen --skip-stage`
- `UnrealEditor-Cmd.exe` 跑 `Automation RunTests Atlas.NetClient`。

这套命令只代表 Phase 14 最小回归，不等同于完整验收。完整闭环的 Linux
服务端 parity 记录见下方验收记录。

---

## 2026-06-23 本地验收记录

- `tools\bin\build.bat debug --clean`：PASS。
- `ctest --test-dir build\debug -C Debug -R "movement|baseapp_messages|baseapp_movement|cellapp_handlers|cell_movement_system|net_client_abi_layout|jolt_physics_query|collision_pipeline|lag_compensation" --output-on-failure`：PASS，16 / 16。
- `dotnet test tests\csharp\Atlas.Client.Tests\Atlas.Client.Tests.csproj --configuration Debug`：PASS，99 / 99；包含 C# / Unity 共用 `AtlasNetNative` binding 的 native predictor 10k tick 运行覆盖。
- `tools\bin\build_mvp_ue.bat --config Debug --ue-root <UE_ROOT> --target UEClientEditor --build-config Development --platform Win64 --skip-native --skip-defs --skip-codegen --skip-stage`：PASS。
- `tools\bin\build_mvp_ue.bat --config Debug --ue-root <UE_ROOT> --target UEClient --build-config Development --platform Win64 --skip-native --skip-defs --skip-codegen --skip-stage`：PASS。
- `UnrealEditor-Cmd.exe ... -ExecCmds="Automation RunTests Atlas.NetClient" -TestExit="Automation Test Queue Empty"`：PASS，9 / 9。
- `tools\bin\run_world_stress.bat --clients 50 --account-pool 50 --duration-sec 30 --shortline-pct 0 --move-rate-hz 10 --move-mode input --spread-radius 400 --movement-verify --login-rate-limit-trusted-cidr 127.0.0.0/8 --login-rate-limit-global 10000`：PASS；`move_sent=13550`，`movement_ack_recv=3961`，BaseApp / CellApp `rate`、`invalid`、`seqgap`、`overflow` 均为 0。
- `tools\bin\run_world_stress.bat --clients 100 --account-pool 100 --duration-sec 30 --shortline-pct 0 --move-rate-hz 10 --move-mode input --space-count 2 --cellapp-count 2 --spread-radius 400 --movement-verify --login-rate-limit-trusted-cidr 127.0.0.0/8 --login-rate-limit-global 10000`：PASS；`move_sent=27100`，`movement_ack_recv=4058`，BaseApp / CellApp `rate`、`invalid`、`seqgap`、`overflow` 均为 0。
- `tools\bin\run_world_stress.bat --clients 400 --account-pool 400 --duration-sec 30 --shortline-pct 0 --move-rate-hz 10 --move-mode input --space-count 8 --cellapp-count 4 --spread-radius 400 --movement-verify --login-rate-limit-trusted-cidr 127.0.0.0/8 --login-rate-limit-global 10000`：PASS；`move_sent=106221`，`movement_ack_recv=14242`，BaseApp `rate`、`invalid`、`seqgap`、`ackstale`、`rptdrop` 均为 0；4 个 CellApp 都有 movement input / frame / ack 覆盖（`5744/5743/3501`、`6918/6913/4137`、`88296/88296/52592`、`5263/5261/3084`），且 CellApp `rate`、`invalid`、`seqgap`、`overflow` 均为 0。
- `tools\bin\run_world_stress.bat --clients 0 --script-clients 2 --script-verify --duration-sec 20 --client-transport-impairment-ms 75 200 --login-rate-limit-trusted-cidr 127.0.0.0/8 --login-rate-limit-global 10000`：PASS；两个脚本客户端均产生 `mIn` / `mAck` / `mRpt`，BaseApp `rate`、`invalid`、`seqgap`、`ackstale`、`rptdrop` 均为 0。
- `ctest --test-dir build\debug -C Debug -R "backend_parity" --output-on-failure`：PASS，1 / 1。
- `tools\bin\build.bat debug --build-only`：PASS，14.4 Static chunk / border query。
- `tools\bin\build_mvp_ue.bat --config Debug --skip-defs --skip-codegen --skip-stage --skip-ue`：PASS。
- `ctest --test-dir build\debug -C Debug -R "movement|baseapp_messages|baseapp_movement|cellapp_handlers|cell_movement_system|net_client_abi_layout|jolt_physics_query|collision_asset|collision_pipeline|test_space|lag_compensation|backend_parity" --output-on-failure`：PASS，19 / 19；包含 `chunk_boundary_cross` parity 场景。
- `tools\bin\build_mvp_unity.bat --unity <UnityEditor.exe> --skip-setup --clean-output --development`：PASS；生成 `out\mvp-unity\windows\AtlasMvp.exe`。
  当前 `samples/mvp/UnityClient/ProjectSettings/ProjectVersion.txt` 为
  `6000.0.43f1-lilith-2`，重新验收时使用当前 Unity Editor。
- `out\mvp-unity\windows\AtlasMvp.exe -batchmode -nographics -atlas-predictor-self-test -logFile out\mvp-unity\predictor-self-test-wait.log`：PASS，exit code 0；日志包含 `[NativePredictorSelfTest] PASS ticks=10000`。
- GitHub Actions `Linux Tests`（`.github/workflows/tests-linux.yml`，Ubuntu
  24.04 / GCC 13）：保留的 Phase 14 验收记录为 PASS；workflow 配置
  `ATLAS_BUILD_NET_CLIENT=ON`，运行 CTest `unit` / `integration` labels 与
  `Atlas.Client.IntegrationTests`，覆盖 Linux 服务端单元、集成、backend
  parity 与 C# integration。

Linux 服务端 parity 已由 CI 证明；本机未留存本地 Linux runner 复现记录。
