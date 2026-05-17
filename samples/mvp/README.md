# Atlas MVP — Unity end-to-end demo

Single-cell Atlas cluster + Unity 6 client demonstrating the full BigWorld-
style stack: client-authoritative movement, server-authoritative skill /
projectile damage, AoI-driven peer streaming, and per-tick property
replication. World is 200 × 200 m with one player avatar and 50 wandering
NPCs.

## Layout

```
samples/mvp/
├── Atlas.Mvp.Base/      # BaseApp scripts: Account.SelectAvatar, Avatar AoI config
├── Atlas.Mvp.Cell/      # CellApp scripts: Avatar, Npc, NpcAiComponent, ProjectileSimulator
├── Atlas.Mvp.Client/    # netstandard2.1 + net10.0 — Unity + atlas_client.exe share
└── UnityClient/         # Unity 6 LTS project; Bootstrap.cs builds the scene at runtime
```

`entity_defs/{Avatar,Npc,Account}.def` define the entity surface; `Avatar` is
base + cell, `Npc` is cell-only.

## Prerequisites

- Unity Hub + Unity 6 LTS (project pinned to `6000.0.28f1c1`)
- .NET 10 SDK
- CMake + Ninja (build helper provisions Ninja on demand)
- Windows: MSVC 2022 (build helper loads env). Linux/macOS: clang or gcc.

## Run it

```bash
# 1. Build native servers + script DLLs (deploys Atlas.Mvp.{Base,Cell}.dll to bin/debug/).
tools/bin/build.sh debug
tools\bin\build.bat debug

# 2. Stage SDK + entity DLL into samples/mvp/UnityClient/Assets/.
tools/bin/setup_mvp_unity.sh
tools\bin\setup_mvp_unity.bat

# 3. Launch the cluster (machined + loginapp + baseappmgr + baseapp + cellappmgr + cellapp + dbapp).
tools/bin/run_mvp_cluster.sh
tools\bin\run_mvp_cluster.bat

# 4. Open samples/mvp/UnityClient in Unity Hub and hit Play.
```

LoginApp port defaults to `20018` on both sides. Cluster runs cellapp /
baseapp at 20 Hz (50 ms tick). `UnityClient/Assets/Scenes/Main.unity`
already contains the runtime `Bootstrap` object.

## Package the Unity client

```bash
# One-shot staging + Unity batchmode player build.
tools/bin/build_mvp_unity.sh
tools\bin\build_mvp_unity.bat

# If assets are already staged, skip the setup phase.
tools/bin/build_mvp_unity.sh --skip-setup --clean-output
tools\bin\build_mvp_unity.bat --skip-setup --clean-output
```

The script discovers the Unity executable from `--unity`, `UNITY_EXE`,
`UNITY_PATH`, then the Unity Hub install path for the pinned project version.
If `ProjectSettings/ProjectVersion.txt` is absent, it scans installed Unity Hub
editors and prints the selected executable. Default Windows output is
`out/mvp-unity/windows/AtlasMvp.exe`; logs land at
`out/mvp-unity/unity-build.log`. Pass `--target StandaloneLinux64` or
`--target StandaloneOSX` for non-Windows players. Standalone builds launch in a
resizable window.

## UE client (M1 — codegen-driven attribute sync)

Unreal Engine 5.7 client connecting to the same MVP cluster over the
Atlas wire protocol. The plugin links `atlas_net_client.dll` +
`atlas_entitydef_client.dll` directly and does **not** use UE
replication / RPC. After M1: registry-driven generic `ApplyDelta`
decodes scalar / struct / list / dict deltas; `Atlas.Tools.CppEmitter`
emits typed entity classes (`atlas::mvp::Account`, `Avatar`, `Npc`)
with property getters and upstream RPC stubs. Downstream RPC dispatch
+ BP exposure arrive in M2.

### Prerequisites

- Unreal Engine 5.7 source build at `UE_ROOT` (e.g.
  `set UE_ROOT=E:\UE\UnrealEngine`)
- Same MSVC 2022 + .NET 10 SDK + CMake toolchain as the server build

### Build + stage

```bash
tools/bin/build_mvp_ue.sh
tools\bin\build_mvp_ue.bat
```

End-to-end pipeline: CMake builds `atlas_net_client.dll` +
`atlas_entitydef_client.dll`, runs `Atlas.Tools.DefDump` to extract
`entity_defs.bin` (production + test ATDFs) and `Atlas.Tools.CppEmitter`
to generate `samples/mvp/UEClient/Source/UEClient/gen/<Entity>.gen.h`,
stages everything into the plugin's `ThirdParty/`, then UBT builds
`UEClientEditor`. `--config Debug` mirrors a Debug SDK; per-stage skip
flags (`--skip-native` / `--skip-defs` / `--skip-codegen` /
`--skip-stage` / `--skip-ue`) allow partial re-runs.

### Run

```bash
# 1. Start the MVP cluster (see "Run it" above).

# 2. Open the project in UE 5.7:
"%UE_ROOT%\Engine\Binaries\Win64\UnrealEditor.exe" samples\mvp\UEClient\UEClient.uproject

# 3. In the editor, hit Play (PIE). UEClientGameMode kicks off Login +
#    Authenticate against 127.0.0.1:20018 with a mvp_<guid> username and
#    the shared mvp_hash password. After auth, it casts the player
#    entity to atlas::mvp::Account and calls SelectAvatar(1) via the
#    codegen-emitted typed stub.
```

`UEClientGameMode` exposes host / port / credentials as `UPROPERTY`
defaults — tweak in World Settings if the cluster is on a non-default
port.

### Acceptance signals

The UEClient Output Log should show, in order:

```
LogAtlasUE: Loaded ATDF from ...entity_defs.bin; digest_size=32
LogAtlasUE: AtlasUE module started; atlas_net_client ABI=0x02000000
LogUEClient: Atlas login host=127.0.0.1 port=20018 user=mvp_<guid>
LogUEClient: Atlas login succeeded, authenticating
LogUEClient: Sent Account.SelectAvatar(1) entity_id=<n>
```

With both Unity and UE clients connected, the UE view streams in the
Unity player's `AAvatarCapsule` (currently
`/Engine/BasicShapes/Cylinder.Cylinder` placeholder mesh) and applies
`AvatarFilter`-interpolated transforms each frame. Property deltas
(HP, level, …) decode through the generic registry path and land on
`atlas::mvp::Avatar` slots, ready for game-side `avatar->Hp()` reads
once the view layer subscribes.

### Known gaps after M1

- **No movement input** — the UE-side Avatar sits where the server
  places it; outbound `Avatar.ReportPos` stub is generated but not
  yet wired to a controller (M3)
- **No reconnect on disconnect** — `on_disconnect` only logs
- **`AAvatarCapsule` is a Cylinder placeholder** — swap for a project
  mesh as a polish pass
- **Downstream RPC (`Avatar.ShowDamage`, `OnDied`, …) not yet dispatched**
  — `0xF004` envelope routing + per-entity `DispatchRpc` land in M2
- **No BP exposure** — codegen output is pure C++ today; M2 adds a
  `UAtlasAvatarView` UCLASS bridge for `GetHp` / `OnHpChanged`

## Controls

| Input | Action |
|---|---|
| **W A S D** | Move (camera-relative; W = camera forward) |
| **Mouse — right-button hold** | Orbit camera (yaw + pitch ±25°/60°), cursor locked |
| **Mouse — scroll wheel** | Zoom (0.5×–12×) |
| **Space** | Fire projectile in facing direction (10 dmg on hit) |

When the player walks, the camera yaw smoothly catches up to the avatar's
facing (~250 ms damping), PUBG-style. Strafing (A/D) keeps the avatar
oriented forward so the camera-follow loop stays stable.

## Gameplay surface

- **Avatar** (cyan capsule): 100 HP, respawns at origin 3 s after death.
- **NPC** (grey capsule): 50 spawned at world bootstrap, scattered in
  ±100 m, wander at 3 m/s with 4 s retarget interval. Fire a projectile at
  their facing direction every 3–7 s. 100 HP, no respawn — die for good.
- **Projectile**: not a replicated entity — server runs a deterministic
  ballistic simulator (gravity 12, 2 s lifetime, 1.5 m hit radius);
  clients integrate the same arc locally from the launch RPC. End event
  (hit / ground / expire) is broadcast with the server's authoritative end
  position so hit FX lands where damage was applied.
- **HUD**: per-entity name + HP label projected to screen via
  `LabelOverlay`; damage popups float up from the strike point and fade.
  Top-right shows fps / ping and a `show AoI` toggle. AoI debug is hidden by
  default; when enabled, green box = enter boundary (50 m) and yellow box =
  leave boundary (55 m, hysteresis).

## Architecture in one paragraph

`Bootstrap` builds the runtime scene (ground, light, camera, root
overlays), calls `AtlasNetworkManager.Login` → `Authenticate` → invokes
`Account.Base.SelectAvatar(1)` RPC. Server-side `Account.SelectAvatar`
creates the player `Avatar` (base + cell counterpart) and binds the
client via `GiveClientTo`. The cell-side `Avatar.OnInit` consults
`SpaceOwnerRegistry` and, if absent, spawns the per-space owner entity
`MvpSpace : CellSpaceEntity` once. `MvpSpace.OnSpaceInit` scatters 50
NPCs and arms an AtlasLoop refill timer: when live count drops to ≤ 50
the refill engages, spawning 1 NPC every 3 s until the cap of 80, then
disengages. Each count change publishes via `SpaceData.SetInt32(SpaceId,
SpaceDataKeys.NpcCount, n)` — the cellapp fans that out as a
`kSpaceDataUpdate` envelope to every client's `SpaceDataManager`.
`Avatar.OnInit` on the base side sets a 50 m AoI radius, so the client
streams in everything within reach. Movement is client-
authoritative: `PlayerInputController` writes `transform.position` from
WASD each frame and reports to the cell at 20 Hz via
`Avatar.Cell.ReportPos`. Projectile damage is server-authoritative: `LaunchProjectile`
registers a shot in the per-space `ProjectileSimulator` which integrates
each cellapp tick and broadcasts `OnProjectileFired` / `OnProjectileEnded`
RPCs so all clients in AoI render a synchronised arc. Property
replication (HP, NpcType, etc.) rides the normal delta pump.

## Known MVP shortcuts

- Movement is client-authoritative (no anti-cheat). Production needs a
  server-side validator + max-speed check.
- NPC AI is random walk + periodic fire; no aggro / target.
- NPCs are refilled by count (engage at ≤ 50, 1 per 3 s, cap 80); the
  dead instance itself doesn't respawn.
- Login uses a hardcoded username + password hash; LoginApp's dev mode
  accepts anything matching its config.
- ProjectileSimulator state is per-space but in-process; cross-cell
  projectile handoff is not implemented.

## Troubleshooting

- **Unity compile errors about missing `Atlas.Mvp.Client`** — you forgot
  to run `setup_mvp_unity`, or run `build_mvp_unity` without `--skip-setup`.
- **`build_mvp_unity` cannot find Unity** — pass `--unity <Unity.exe>` or set
  `UNITY_EXE`; without `ProjectVersion.txt`, the script scans Unity Hub installs first.
- **`Login failed: BadCredentials`** — LoginApp's `accept_any_user` is off
  in your loginapp config. Either enable it for dev or pre-create the user.
- **No NPCs visible** — check the cellapp log for
  `MvpSpace: seeded 50/50 NPCs`; if it shows 0, CellApp is missing
  the Mvp.Cell DLL (re-run `build.bat debug` to redeploy via CMake).
- **Labels stuck on screen edges / wrong positions** — Unity scene
  template's stray `Main Camera` GameObject collides with Bootstrap's
  runtime camera. Delete the default Main Camera from the hierarchy.
