# Atlas Client SDK — Unity Integration Guide

User guide for game developers integrating Atlas's client SDK into a
Unity 2022.3 LTS+ project. Sibling to the architecture-focused
[`UNITY_NATIVE_DLL_DESIGN.md`](UNITY_NATIVE_DLL_DESIGN.md); this doc
is the practical "what do I do" view.

## Contents

- [Prerequisites](#prerequisites)
- [Quick start](#quick-start)
- [Manual setup](#manual-setup)
- [Plugin Import Settings](#plugin-import-settings)
- [API surface](#api-surface)
- [Sample bootstrap script](#sample-bootstrap-script)
- [Mobile targets](#mobile-targets)
- [Updating binaries](#updating-binaries)
- [Troubleshooting](#troubleshooting)

## Prerequisites

| | Requirement |
|---|---|
| **Unity** | 2022.3 LTS or 6.x (≤ 6.5 — see below). API Compatibility = .NET Standard 2.1 (Project Settings → Player → Other Settings). |
| **Atlas repo** | This tree, with `ATLAS_BUILD_NET_CLIENT=ON` enabled when configuring CMake. |
| **.NET SDK** | 9.0+ (`dotnet --version`). |
| **Native toolchain** | Host build only: MSVC 2022 (Windows) / Clang or GCC (Linux) / Xcode CLT (macOS). Mobile builds (Android arm64 / iOS arm64) come from CI artefacts — see [Mobile targets](#mobile-targets). |

Unity 2022 → 6.5 are the **Pattern B** range — `[MonoPInvokeCallback]`
+ delegate is required because `[UnmanagedCallersOnly]` isn't recognised
by the embedded Mono / old .NET 4.x runtime. Unity 6.6+ (planned to
embed .NET 10) will support Pattern A; the SDK source has the
`#if UNITY_5_3_OR_NEWER` attribute guard ready and the migration is
mechanical (see [`docs/spike_il2cpp_callback.md`](../spike_il2cpp_callback.md)
§Forward compatibility).

## Quick start

From the Atlas repo root:

```bash
# Windows
tools\bin\setup_unity_client.bat --unity-project C:\path\to\YourUnityProject

# Linux / macOS
tools/bin/setup_unity_client.sh --unity-project ~/path/to/YourUnityProject
```

The tool builds the host-platform native (`atlas_net_client.{dll,so,bundle}`)
+ managed (`Atlas.Shared.dll`, `Atlas.Client.dll`) binaries, stages them
into `src/csharp/Atlas.Client.Unity/Plugins/<platform>/` (in this repo),
and copies the entire `src/csharp/Atlas.Client.Unity/` folder into your
project's `Assets/Atlas.Client.Unity/`. Open the project in Unity Hub
and the SDK appears under **Assets/Atlas.Client.Unity/** in the Project
window; the asmdef compiles automatically.

Useful flags:

| Flag | Effect |
|---|---|
| `--config Release` (default) | Optimised build. |
| `--config Debug` | Debug symbols + Tracy hooks; bigger DLLs. |
| `--skip-build` | Reuse existing `bin/<config>/` + dotnet outputs. |

The script is idempotent — re-running rebuilds, refreshes `Plugins/`,
and clean-replaces `Assets/Atlas.Client.Unity/` in your project.

## Manual setup

If you'd rather drive each step yourself, or you're scripting a
non-default flow:

### 1. Build the binaries

```bash
# Native (host platform)
cmake --preset release -DATLAS_BUILD_NET_CLIENT=ON
cmake --build build/release --target atlas_net_client --config Release

# Managed
dotnet build src/csharp/Atlas.Shared/Atlas.Shared.csproj -c Release
dotnet build src/csharp/Atlas.Client/Atlas.Client.csproj  -c Release
```

Outputs:

| Binary | Path |
|---|---|
| `atlas_net_client.dll` (Win) | `bin/release/atlas_net_client.dll` |
| `libatlas_net_client.so` (Linux) | `bin/release/libatlas_net_client.so` |
| `atlas_net_client.bundle` (macOS) | `bin/release/atlas_net_client.bundle` |
| `Atlas.Shared.dll` | `src/csharp/Atlas.Shared/bin/Release/netstandard2.1/Atlas.Shared.dll` |
| `Atlas.Client.dll` | `src/csharp/Atlas.Client/bin/Release/netstandard2.1/Atlas.Client.dll` |

### 2. Stage binaries into the SDK folder

```
src/csharp/Atlas.Client.Unity/
└── Plugins/
    ├── Atlas.Shared.dll                     # managed, any platform
    ├── Atlas.Client.dll                     # managed, any platform
    └── Windows/x86_64/atlas_net_client.dll  # native, per platform
        Linux/x86_64/libatlas_net_client.so
        macOS/atlas_net_client.bundle
        Android/arm64-v8a/libatlas_net_client.so
        iOS/libatlas_net_client.a
```

Create the platform subdirectories as needed; Unity infers platform
filters from the path.

### 3. Copy the SDK folder into your Unity project

```bash
cp -r src/csharp/Atlas.Client.Unity <UnityProjectRoot>/Assets/Atlas.Client.Unity
```

Skip `Atlas.Client.Unity.csproj` (IDE-only — would confuse Unity Editor)
and any `bin/` / `obj/` artefacts. Unity picks up the asmdef on next
focus and compiles the assembly.

## Plugin Import Settings

Unity's per-asset Inspector **must** be configured for native plugins
or you'll see `DllNotFoundException` at runtime:

| Asset | Inspector settings |
|---|---|
| `Atlas.Shared.dll` | Tick **Any Platform**. Untick **Auto Reference** (asmdef references it explicitly). |
| `Atlas.Client.dll` | Same as above. |
| `Plugins/Windows/x86_64/atlas_net_client.dll` | Platform: **Standalone Windows** only. CPU = **x86_64**. |
| `Plugins/Linux/x86_64/libatlas_net_client.so` | Platform: **Standalone Linux** only. CPU = **x86_64**. |
| `Plugins/macOS/atlas_net_client.bundle` | Platform: **Standalone macOS** only. CPU = **AnyCPU** (or **ARM64** if Apple Silicon-only). |
| `Plugins/Android/arm64-v8a/libatlas_net_client.so` | Platform: **Android** only. CPU = **ARM64**. |
| `Plugins/iOS/libatlas_net_client.a` | Platform: **iOS** only. Add to Embedded Binaries (Unity does this automatically when the file is under `Plugins/iOS/`). |

Apply each change. Unity will recompile the asmdef.

## API surface

`AtlasNetworkManager` (in `Atlas.Client.Unity`) is the single
MonoBehaviour entry point. Drop it onto a GameObject (typically a
`DontDestroyOnLoad` singleton).

### Lifecycle (driven by Unity)

```
Awake()   → AtlasNetCreate() + register callbacks
Update()  → AtlasNetPoll() + AvatarFilter.UpdateLatency() per entity
OnDestroy → AtlasNetDestroy() + clear filters
```

### Public methods

| Method | Returns | When to call |
|---|---|---|
| `Login(username, passwordHash)` | `int` (rc) | Any time after Awake; legal only in `Disconnected`. |
| `Authenticate()` | `int` | Inside `LoginFinished` handler when status == `Success`. |
| `Logout()` | `int` | Any time; idempotent. |
| `SendBaseRpc(entityId, rpcId, payload)` | `int` | After `AuthFinished(success=true)`. |
| `SendCellRpc(entityId, rpcId, payload)` | `int` | Same. |
| `TryGetInterpolatedTransform(entityId, out pos, out dir, out onGround)` | `bool` | Per-frame, after `EntityEntered` for that entity. Returns `false` if no samples yet. |
| `State` | `AtlasNetState` | Anytime. |

### Events

All events fire on the Unity main thread (during `Update` poll).

| Event | Signature | Fires when |
|---|---|---|
| `LoginFinished` | `(AtlasLoginStatus, string?)` | LoginApp responds or 10s timeout. |
| `AuthFinished` | `(bool, uint, ushort, string?)` | BaseApp responds or 5s timeout. |
| `Disconnected` | `(int reason)` | Server-side close / timeout / network error / `Logout()`. |
| `PlayerBaseCreated` | `(uint eid, ushort tid, byte[] props)` | Server creates the player's Base entity. |
| `PlayerCellCreated` | `(uint space, Vector3 pos, Vector3 dir, byte[] props)` | Player's Cell entity spawns (entered the world). |
| `EntitiesReset` | `()` | `giveClientTo` — clear all AOI state. |
| `EntityEntered` | `(uint eid, ushort tid, Vector3 pos, Vector3 dir, byte[] props)` | Other entity enters AOI. |
| `EntityLeft` | `(uint eid)` | Other entity leaves AOI. |
| `EntityPositionUpdated` | `(uint eid, Vector3 pos, Vector3 dir, bool onGround)` | Server position update. Already fed into AvatarFilter — usually you query `TryGetInterpolatedTransform` instead of subscribing. |
| `EntityPropertyUpdated` | `(uint eid, byte scope, byte[] delta)` | Server property delta. Pair with the codegen-generated `ApplyReplicatedDelta`. |
| `EntityForcedPosition` | `(uint eid, Vector3 pos, Vector3 dir)` | Server snap. AvatarFilter is reset internally to avoid lerping from stale samples; subscribe if you need to bypass interpolation in the renderer too. |
| `Rpc` | `(uint eid, uint rpcId, byte[] payload)` | Server-to-client RPC. Pair with the codegen-generated `ClientRpcDispatcher`. |

### AvatarFilter — automatic

Each entity gets its own filter on first position update. The filter
parameters are exposed on the `AtlasNetworkManager` Inspector:

| Field | Default | Effect |
|---|---|---|
| `Avatar Filter Latency Frames` | 3.0 | Target render latency in server-tick units. Higher = smoother but laggier. |
| `Avatar Filter Server Interval` | 0.1 | Expected server tick rate (seconds). 10 Hz default. |
| `Avatar Filter Curve Power` | 2.0 | Convergence curve exponent. Higher = more aggressive on big lag, smoother on small. |
| `Avatar Filter Max Extrapolation` | 0.05 | Cap on extrapolation when no fresh sample (seconds). |

Game code reads the smoothed transform per frame:

```csharp
if (net.TryGetInterpolatedTransform(remoteEntityId, out var pos, out var dir, out var onGround))
{
    remoteAvatar.transform.position = pos;
    remoteAvatar.transform.rotation = Quaternion.LookRotation(dir);
}
```

`Reset()` is automatic on `EntityForcedPosition` (server-authoritative
snap) and on `EntityLeft` / `EntitiesReset` (entity dropped from AOI).

## Sample bootstrap script

Drop this onto a GameObject alongside `AtlasNetworkManager`:

```csharp
using Atlas.Client.Native;
using Atlas.Client.Unity;
using UnityEngine;

public class TestBootstrap : MonoBehaviour
{
    [SerializeField] private AtlasNetworkManager net;
    [SerializeField] private string username = "alice";
    [SerializeField] private string passwordHash = "abcdef0123";

    private void Start()
    {
        Debug.Log($"Atlas net_client ABI = 0x{AtlasNetNative.AtlasNetGetAbiVersion():X8}");

        net.LoginFinished += (status, err) =>
        {
            Debug.Log($"login: status={status} err={err ?? "<none>"}");
            if (status == AtlasLoginStatus.Success) net.Authenticate();
        };
        net.AuthFinished += (ok, eid, tid, err) =>
        {
            if (ok) Debug.Log($"connected as entity {eid} (type {tid})");
            else   Debug.LogError($"auth failed: {err}");
        };
        net.Disconnected += reason => Debug.LogWarning($"disconnected reason={reason}");
        net.EntityEntered += (eid, tid, pos, dir, _) =>
            Debug.Log($"entity {eid} (type {tid}) entered at {pos}");

        net.Login(username, passwordHash);
    }
}
```

Press **Play** with an Atlas cluster running locally. Start one in a
separate terminal:

```bash
# Windows
tools\bin\run_cluster.bat
# Linux / macOS
tools/bin/run_cluster.sh
```

That brings up machined / loginapp / dbapp / baseappmgr / baseapp /
cellappmgr / cellapp from `build/debug` and parks LoginApp on
`127.0.0.1:20013` (the AtlasNetworkManager Inspector default). Stop
the cluster with Ctrl+C in that terminal — see the script header
for orphan-cleanup notes.

Console should then show the login → auth → entity-creation chain.

## Mobile targets

### Android

- **Min API**: 24 (Android 7.0). Older devices aren't tested; the
  `ANDROID_PLATFORM=android-24` preset comes from this floor.
- **ABI**: arm64-v8a only. ARMv7 is intentionally not built — Atlas
  targets devices ≤ 5 years old.
- **Player Settings**:
  - Scripting Backend: **IL2CPP**
  - Target Architectures: tick **ARM64**, untick others
  - API Compatibility Level: **.NET Standard 2.1**
- **Binary**: download `atlas_net_client-android-arm64` artefact from
  the GitHub Actions `net_client cross-platform` workflow run, drop into
  `Plugins/Android/arm64-v8a/`. Or build locally with NDK r25+:
  ```bash
  export ANDROID_NDK_HOME=$HOME/Android/Sdk/ndk/25.2.9519653
  cmake --preset net-client-android-arm64
  cmake --build --preset net-client-android-arm64
  ```

### iOS

- **arm64** only; static link via `[DllImport("__Internal")]`.
  `AtlasNetNative.cs` already conditional-compiles the right `LibName`.
- **macOS host required** for the build.
- **Player Settings**:
  - Scripting Backend: **IL2CPP** (forced on iOS)
  - Target SDK: Device or Simulator (build separate artefacts)
  - Architecture: **ARM64**
- **Binary**: from CI artefact or local Xcode build:
  ```bash
  cmake --preset net-client-ios-arm64
  cmake --build --preset net-client-ios-arm64
  ```
  Drop `libatlas_net_client_static.a` into `Plugins/iOS/`.

### Pattern A migration (Unity 6.6+, future)

When Unity 6.6 ships with embedded .NET 10, re-run the spike to
verify `[UnmanagedCallersOnly]` works:

```bash
tools\bin\build.bat release -DATLAS_BUILD_IL2CPP_PROBE=ON
# Drop atlas_il2cpp_probe.dll/.so/.bundle into a Unity test project
# under Assets/Plugins/IL2CPPProbe/, attach ProbeComponent.
```

If both Pattern A and B fire on all four targets, follow the
mechanical migration in [`spike_il2cpp_callback.md`](../spike_il2cpp_callback.md)
§Forward compatibility (3 lines per handler, no DLL change, no ABI
bump).

## Updating binaries

When the Atlas server protocol changes (e.g., a new `[EntityProperty]`
field, a new exposed RPC), regenerate the C# bindings via
`Atlas.Generators.Def` and re-run the setup tool:

```bash
tools\bin\setup_unity_client.bat --unity-project C:\path\to\YourUnityProject
```

The tool clean-replaces `Assets/Atlas.Client.Unity/` in your project
with the freshly-built SDK + binaries. Unity will reimport on next
focus; close and reopen the editor if you see stale references.

For a faster iteration loop during gameplay tuning (no protocol
changes), use `--config Debug --skip-build` to just refresh from your
existing build outputs without recompiling.

## Troubleshooting

### `DllNotFoundException: atlas_net_client`

- Native DLL missing or in the wrong location. Verify
  `Plugins/Windows/x86_64/atlas_net_client.dll` exists (or the
  matching path for your platform).
- Plugin Import Settings have wrong CPU/OS filter — Unity won't load
  the DLL on a platform it thinks is unsupported. See
  [Plugin Import Settings](#plugin-import-settings).
- On macOS, Gatekeeper may block unsigned `.bundle` — `xattr -cr
  <UnityProjectRoot>/Assets/Atlas.Client.Unity/Plugins/macOS/atlas_net_client.bundle`.

### `Atlas.Client.Unity` not appearing in the Project window

- The setup tool didn't run, or its target Unity project path was
  wrong. Re-run with `--unity-project <path>` and verify
  `<UnityProjectRoot>/Assets/Atlas.Client.Unity/Atlas.Client.Unity.asmdef`
  exists afterwards.
- The asmdef requires Unity 2022.3+ (via `defineConstraints:
  ["UNITY_2022_3_OR_NEWER"]`). On older Unity the assembly is silently
  skipped — check **Project Settings → Player → Other Settings**.

### `atlas_net_client.AtlasNetCreate failed (abi=0x01000000): ABI version mismatch`

- The native DLL and the C# bindings disagree. Re-run
  `setup_unity_client` with the same `--config` to rebuild both
  sides from the same commit.
- If you copied the native DLL from CI but built the managed side
  locally on a different commit, version drift is the cause.

### Pattern A throws `EntryPointNotFoundException` at runtime

- You're on Unity ≤ 6.5; Pattern B is the only supported path. The
  source uses `[MonoPInvokeCallback]` automatically when
  `UNITY_5_3_OR_NEWER` is defined — verify the asmdef is being
  compiled by Unity (not a side-loaded netstandard project).

### IL2CPP build fails with stripping errors on `AtlasNetCallbackBridge`

- IL2CPP's bytecode stripper can drop the static delegate keep-alive
  fields. Add `[Preserve]` from `UnityEngine.Scripting` if you see
  this — patch upstream if needed. The bridge already uses
  process-lifetime statics specifically to dodge this on the common
  path; report any regression.

### Player log location

| Platform | Path |
|---|---|
| Editor (Win) | `%LOCALAPPDATA%\Unity\Editor\Editor.log` |
| Standalone Win | `%USERPROFILE%\AppData\LocalLow\<Company>\<Product>\Player.log` |
| macOS Player | `~/Library/Logs/<Company>/<Product>/Player.log` |
| Android | `adb logcat -s Unity` |
| iOS | Xcode Console / `~/Library/Logs/CoreSimulator/<dev-id>/system.log` |

The `AtlasNetSetLogHandler` callback (Phase 3 §4.8) routes
`atlas_net_client` internal logs into Unity's `Debug.Log` if you wire
it; not done by default. Hook it in `AtlasNetworkManager.Awake()`
before `AtlasNetCreate` if you want the native side's diagnostics.
