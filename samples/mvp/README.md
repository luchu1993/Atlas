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
creates the player `Avatar` (base + cell counterpart), binds the client
via `GiveClientTo`, then runs `WorldBootstrap.EnsureSpawned` once to
scatter 50 NPCs. `Avatar.OnInit` on the base side sets a 50 m AoI radius,
so the client streams in everything within reach. Movement is client-
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
- NPCs don't respawn — they stay dead until cluster restart.
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
- **No NPCs visible** — check the server log for
  `WorldBootstrap: queued 50/50 NPC spawns`; if it shows 0, BaseApp is
  missing the Mvp.Base DLL (re-run `build.bat debug` to redeploy via
  CMake).
- **Labels stuck on screen edges / wrong positions** — Unity scene
  template's stray `Main Camera` GameObject collides with Bootstrap's
  runtime camera. Delete the default Main Camera from the hierarchy.
