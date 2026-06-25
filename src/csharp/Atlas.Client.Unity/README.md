# Atlas.Client.Unity

**Status:** Current Unity SDK package guide. `tools/bin/setup_unity_client.{bat,sh}`
stages managed and native binaries; `Plugins/` does not ship built binaries in git.

Unity client SDK for Atlas Engine. The MVP pins Unity 6
(`6000.0.43f1-lilith-2`); the package keeps a Unity 2022.3 LTS+ /
netstandard2.1 surface. Drop this folder into a Unity project's
`Assets/Atlas.Client.Unity/` (or symlink it) and the `Atlas.Client.Unity`
asmdef compiles against `Atlas.Client.dll` / `Atlas.Shared.dll` shipped
under `Plugins/`.

`tools/bin/setup_unity_client.{bat,sh}` syncs the runtime SDK files into Unity by
copying changed files and pruning stale files. If Unity locks a native plugin,
close the Editor and rerun setup.

## Layout

```
Atlas.Client.Unity/
├── Atlas.Client.Unity.asmdef       # Unity-side compilation unit
├── Atlas.Client.Unity.csproj       # IDE-only mirror (see "IDE wiring" below)
├── README.md
├── AtlasClient.cs                  # high-level connect/auth wrapper
├── AtlasNetworkManager.cs          # MonoBehaviour entry point
├── LoginClient.cs                  # login + auth flow
├── UnityLogBackend.cs              # routes Atlas.Diagnostics.Log -> Debug.Log
├── UnityProfilerBackend.cs         # routes Atlas.Diagnostics.Profiler -> ProfilerMarker
├── UnityConversions.cs             # Atlas <-> Unity Vector3/Quaternion helpers
├── Coro/
│   └── UnityLoop.cs                # PlayerLoop tick for Atlas coroutines
├── Runtime/
│   ├── AtlasUnityFramePump.cs      # app-owned frame dispatcher; no global ticker
│   └── AtlasEntityViewRegistry.cs  # session-owned entity view lifecycle helper
└── Plugins/                        # populated by tools/bin/setup_unity_client.{bat,sh}
    ├── Atlas.Client.dll            # managed, from src/csharp/Atlas.Client/bin/<config>/
    ├── Atlas.Shared.dll            # managed, from src/csharp/Atlas.Shared/bin/<config>/
    ├── Windows/x86_64/atlas_net_client.dll
    ├── Windows/x86_64/mimalloc.dll      # or mimalloc-debug.dll for Debug
    ├── Linux/x86_64/libatlas_net_client.so
    ├── Linux/x86_64/libmimalloc.so      # or libmimalloc-debug.so for Debug
    ├── macOS/atlas_net_client.bundle
    ├── Android/arm64-v8a/libatlas_net_client.so
    └── iOS/libatlas_net_client_static.a
```

`Plugins/` does not ship binaries in git; only Unity placeholder/import
metadata is tracked. Native and managed binaries are produced by the build and
dropped here by `tools/bin/setup_unity_client.{bat,sh}`. Mobile targets come
from a cross-platform CI workflow — the local helper only stages the host
platform.

## Quick start

```bash
# Windows
tools\bin\setup_unity_client.bat --unity-project C:\path\to\YourUnityProject

# Linux / macOS
tools/bin/setup_unity_client.sh --unity-project ~/path/to/YourUnityProject
```

The tool builds the host-platform binaries, populates `Plugins/`, and
syncs the runtime SDK contents into the target Unity project's `Assets/`
while excluding `Atlas.Client.Unity.csproj`, `bin/`, `obj/`, and `.gitkeep`.

## IDE wiring (optional)

`Atlas.Client.Unity.csproj` is registered with `src/csharp/CMakeLists.txt`
so the project shows under the VS solution's `CSharp/` folder and Unity
sources are browsable / refactorable from the same sln as the server-side
C#. The csproj is **not deployed** — Unity Editor remains the sole compiler
for shipping bits via the asmdef.

Two compile modes, switched by `UnityEditorPath` env var:

- **Default (no env var)** — sources are excluded from the compile graph
  and the assembly compiles empty. Files stay visible in Solution Explorer;
  the csproj never breaks on a server-only dev box or CI without Unity.
- **Opt-in IntelliSense** — set `UnityEditorPath` to a Unity install root
  (e.g. `C:\Program Files\Unity\Hub\Editor\6000.0.43f1-lilith-2\Editor`).
  The csproj references every DLL under `Data\Managed\UnityEngine\`, defines
  `UNITY_2022_3_OR_NEWER`, and compiles the same sources Unity does — VS
  flags Unity API drift before Unity Editor reports it.

## Logging backend

`Atlas.Diagnostics.Log` is a shared facade backed by an `ILogBackend`; it
defaults to a no-op sink. Install the Unity backend during boot so
server-emitted errors land in Unity's console:

```csharp
using Atlas.Diagnostics;
using Atlas.Client.Unity;

Log.SetBackend(new UnityLogBackend());
```

Trace/Debug/Info route to `Debug.Log`, Warning to `Debug.LogWarning`,
Error/Critical to `Debug.LogError` — standard Unity log filters apply.

## Profiler backend

```csharp
using Atlas.Diagnostics;
using Atlas.Client.Unity;

Profiler.SetBackend(new UnityProfilerBackend());
```

After that, every `using var _ = Profiler.Zone(...)` call inside
`Atlas.Client` shows up as a sample in the Unity Profiler window, sharing
zone names with the server's Tracy trace via `Atlas.Diagnostics.ProfilerNames`.

## Domain reload caveat (Editor only)

`UnityProfilerBackend` caches `ProfilerMarker` instances for the life of the
domain. On Editor assembly reload, those instances become stale. The Unity
host should reset the backend on `AssemblyReloadEvents.beforeAssemblyReload`:

```csharp
#if UNITY_EDITOR
[InitializeOnLoadMethod]
static void RegisterReloadHandler() {
    UnityEditor.AssemblyReloadEvents.beforeAssemblyReload +=
        () => Atlas.Diagnostics.Profiler.ResetBackend();
}
#endif
```

A player build (no Editor) doesn't need this — there is no reload event.

## Documentation

- [`docs/client/UNITY_INTEGRATION.md`](../../../docs/client/UNITY_INTEGRATION.md) — Plugin Import Settings, API surface, mobile targets, troubleshooting
- [`docs/client/UNITY_NATIVE_DLL_DESIGN.md`](../../../docs/client/UNITY_NATIVE_DLL_DESIGN.md) — ABI contract, design rationale
