# Atlas MVP — UE Client

**Status:** Current UE MVP walkthrough. Login, SpaceData, Avatar view,
movement input / prediction, movement command playback, and generated RPC are
wired; remaining visual-layer gaps are listed below.

The Unreal counterpart of `samples/mvp/UnityClient`. Same wire (RUDP + ATDF),
same MVP feature set (Account login → SelectAvatar → AoI / NPCs), driven by
the `AtlasUE` plugin (`Plugins/AtlasUE/`) and a thin game module
(`Source/UEClient/`).

Bilingual docs: see `README_cn.md` for 中文.

## Prerequisites

- **Unreal Engine** 5.x source build. The plugin is compiled by UBT against
  whatever engine the project's `UEClient.uproject` points at; set the
  `UE_ROOT` environment variable (or pass `--ue-root`) to the engine root
  (folder containing `Engine/Build/BatchFiles/`).
- **Windows / Win64** for the complete local UE Editor loop. The `.sh`
  wrappers exist for script parity, but the sample plugin currently links and
  loads `ThirdParty/*/Win64` only.
- **.NET 10 SDK** for the C# server / codegen / DefDump pipeline.
- **Python 3.10+** (stdlib only) for the build & launch wrappers.
- **Visual Studio 2026 or Visual Studio 2022 17.14+ with MSVC C++
  toolchain** for the native client SDK.

## One-button local dev loop

```text
# Build cluster + plugin + ATDF, then spawn cluster and open UE Editor.
tools\bin\run_mvp_ue.bat              # Windows / Win64, default --config Release
```

`tools/bin/run_mvp_ue.sh` is kept for wrapper parity. Non-Windows UE Editor
builds require extending `AtlasUE.Build.cs` and `AtlasUE.cpp` beyond the
current Win64 ThirdParty paths.

The wrapper:
1. Runs `tools/bin/build_mvp_ue.{bat,sh}` (builds atlas_net_client.dll + ATDF +
   verifies digest vs `bin/<config>/Atlas.Mvp.Cell.dll`, then UEClientEditor).
2. Spawns the MVP cluster in the background on 127.0.0.1 (the standard
   world-stress stack plus one BaseApp and one CellApp; logs under
   `.tmp/world-stress/<timestamp>/logs/`).
3. Opens `UEClient.uproject` in UnrealEditor — hit **PIE** to play.
4. On UnrealEditor exit (or Ctrl+C) it closes the cluster cleanly.

Skip individual steps with `--no-build`, `--no-cluster`, `--no-ue`.

## Build only (no launch)

```text
# Native + ATDF + UE plugin recompile; default config = Release.
tools\bin\build_mvp_ue.bat             # Windows / Win64
```

`tools/bin/build_mvp_ue.sh` can drive the same Python pipeline, but the sample
plugin does not yet link or load non-Win64 ThirdParty artifacts.

Mismatch guards that fire at this stage:

- **ATDF ↔ cluster digest** — `entity_defs.bin` is cross-checked against
  `bin/<config>/Atlas.Mvp.Cell.dll`'s `EntityDefDigest.Sha256Hex` via DefDump's
  reflection-only `--digest-only` mode. A stale build cache fails here
  instead of bleeding through to a login-time `def_mismatch`.
- **Debug CRT** — `--config Debug` is rejected unless `--build-config DebugGame`,
  because UE Editor (Development) can't load Debug-CRT DLLs.

If you see the def_mismatch fail, the fix is in the error message:
- Windows: `tools\bin\build.bat <preset>`
- Linux/macOS: `tools/bin/build.sh <preset>`

Then re-run `build_mvp_ue` and do not pass `--skip-defs`.

## Wiring the UI in Blueprint

The plugin ships C++ base classes; the BP author owns layout / styling.

**Login screen** (`UAtlasLoginWidget`):
1. New Widget Blueprint, **Reparent to** `AtlasLoginWidget`.
2. Layout: text boxes for Host / Port / Username + a Login button.
3. Login button OnClicked → call `BeginLoginFromFields(Host, Port, Username, PasswordHash)`.
4. Override the events `OnNetStateChanged`, `OnLoginFinished`,
   `OnReconnectScheduled`, `OnReconnectExhausted` to update status text.
   Use `Atlas Subsystem → GetLastNetErrorMessage` to read the failure reason.
5. In `UEClientGameMode`, set `LoginWidgetClass = BP_AtlasLogin`.
   GameMode skips its hardcoded auto-login when this is set and removes
   the widget once the session reaches `Authenticated`.

**HUD** (`UAtlasHudWidget`):
1. New Widget Blueprint, **Reparent to** `AtlasHudWidget`.
2. Layout: text blocks / progress bars for FPS, ping, bandwidth, HP, NPC count.
3. Override `OnHudRefresh(Stats)` — `Stats.Fps`, `Stats.Net.RttMs`,
   `Stats.Net.BytesSent/Received`, `Stats.PlayerEntityId`, `Stats.NpcCount`
   are all `BlueprintReadOnly`.
4. For HP / level / damage popups: call
   `UEClientGameMode → GetPlayerAvatarView` and bind to the
   `UAtlasAvatarView` delegates (`OnHpChanged`, `OnShowDamage`, etc.).
5. In `UEClientGameMode`, set `HudWidgetClass = BP_AtlasHud`. The HUD spawns
   after `Authenticated` (when the login widget is dismissed).

Logout from BP: call `Atlas Subsystem → Logout`. The subsystem flips
`bAutoReconnectEnabled` off, hands `ATLAS_DISCONNECT_LOGOUT` to the SDK
so the server releases the Account proxy, and a fresh `BeginLogin` re-arms
auto-retry.

## Known gaps vs Unity

- No input remapping UI / settings menu.
- No chat input or scrollback (server-side `Avatar.Say()` exists).
- No equipment / weapon swap UI.
- No damage floater 3D actor or projectile trail VFX.
- No follow camera / orbit controls; `APlayerController.ControlRotation` only
  drives the movement frame.
- No bot mode (`tools/bin/run_mvp_unity_bots.{bat,sh}` has no UE equivalent yet).
- Non-Win64 UE plugin linkage/runtime loading is not wired yet.
- Session recording / playback tooling is not implemented for both clients;
  MovementCommand playback is implemented in the Unity and UE movement paths.

## Layout

- `Plugins/AtlasUE/Source/AtlasUE/Public/` — plugin headers (subsystem,
  net client, login / HUD / SpaceData / Avatar view).
- `Plugins/AtlasUE/Source/AtlasUE/Private/AtlasCore/` — wire decoders
  (AoI envelope, property decoder, RPC dispatch) matching the Unity protocol path.
- `Plugins/AtlasUE/ThirdParty/AtlasNetClient/Win64/` — staged
  `atlas_net_client.dll` + `mimalloc.dll` (Release CRT).
- `Plugins/AtlasUE/ThirdParty/AtlasEntityDef/` — staged
  `atlas_entitydef_client.dll` + `entity_defs.bin` (ATDF v3 with digest).
- `Source/UEClient/` — game module: GameMode, AvatarCapsule actor, input
  controller, generated entity stubs under `gen/`.
