# Atlas MVP — UE Client

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
- **.NET 10 SDK** for the C# server / codegen / DefDump pipeline.
- **Python 3.10+** (stdlib only) for the build & launch wrappers.
- **Visual Studio 2022/18 + MSVC C++ toolchain** for the native client SDK.

## One-button local dev loop

```sh
# Build cluster + plugin + ATDF, then spawn cluster and open UE Editor.
python tools/run_mvp_ue.py            # defaults: --config Release
```

The wrapper:
1. Runs `tools/build_mvp_ue.py` (builds atlas_net_client.dll + ATDF +
   verifies digest vs `bin/<config>/Atlas.Mvp.Cell.dll`, then UEClientEditor).
2. Spawns the MVP cluster in the background (5 server processes on 127.0.0.1
   under `.tmp/world-stress/<timestamp>/logs/`).
3. Opens `UEClient.uproject` in UnrealEditor — hit **PIE** to play.
4. On UnrealEditor exit (or Ctrl+C) it closes the cluster cleanly.

Skip individual steps with `--no-build`, `--no-cluster`, `--no-ue`.

## Build only (no launch)

```sh
# Native + ATDF + UE plugin recompile; default config = Release.
python tools/build_mvp_ue.py
```

Mismatch guards that fire at this stage:

- **ATDF ↔ cluster digest** — `entity_defs.bin` is cross-checked against
  `bin/<config>/Atlas.Mvp.Cell.dll`'s `EntityDefDigest.Sha256Hex` via DefDump's
  reflection-only `--digest-only` mode. A stale build cache fails here
  instead of bleeding through to a login-time `def_mismatch`.
- **Debug CRT** — `--config Debug` is rejected unless `--build-config DebugGame`,
  because UE Editor (Development) can't load Debug-CRT DLLs.

If you see the def_mismatch fail, the fix is in the error message:
`python tools/build.py <preset>` then re-run `build_mvp_ue.py` without
`--skip-defs`.

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
- No bot mode (`tools/run_mvp_unity_bots.py` has no UE equivalent yet).
- Replay / playback unimplemented on both clients.

## Layout

- `Plugins/AtlasUE/Source/AtlasUE/Public/` — plugin headers (subsystem,
  net client, login / HUD / SpaceData / Avatar view).
- `Plugins/AtlasUE/Source/AtlasUE/Private/AtlasCore/` — wire decoders
  (AoI envelope, property decoder, RPC dispatch) shared with Unity.
- `Plugins/AtlasUE/ThirdParty/AtlasNetClient/Win64/` — staged
  `atlas_net_client.dll` + `mimalloc.dll` (Release CRT).
- `Plugins/AtlasUE/ThirdParty/AtlasEntityDef/` — staged
  `atlas_entitydef_client.dll` + `entity_defs.bin` (ATDF v3 with digest).
- `Source/UEClient/` — game module: GameMode, AvatarCapsule actor, input
  controller, generated entity stubs under `gen/`.
