# Atlas.Client.Unity

Unity-only assembly that plugs Atlas's profiler facade into Unity's Profiler /
Profile Analyzer / Frame Debugger via `ProfilerMarker`.

## Why this lives outside the dotnet build pipeline

These sources reference `UnityEngine` types and only compile under Unity. They
are **not** registered with Atlas's `src/csharp/CMakeLists.txt` deliberately —
the dotnet CLI used by Atlas's CMake driver has no path to `UnityEngine.dll`,
and adding one would couple the server build to a Unity install.

The Unity client team consumes this folder in one of two ways:

1. **As source**: drop the folder into the Unity project's
   `Assets/Atlas.Client.Unity/` (or a UPM package's `Runtime/`). Unity's
   compiler picks up the `.asmdef` and builds it against the Unity-side
   `Atlas.Shared.dll` / `Atlas.Client.dll` managed plugins.
2. **As precompiled plugin**: build via a Unity-aware csproj (set up on the
   client team's side, not in this repo) and ship the resulting
   `Atlas.Client.Unity.dll`.

Either way, the consumer is responsible for placing the Atlas.Shared.dll and
Atlas.Client.dll managed plugins where the asmdef's `precompiledReferences`
can resolve them — typically in a `Plugins/` sibling folder.

## Bootstrap (Unity-side)

The backend is opt-in. Somewhere during Unity boot (typically a
`[RuntimeInitializeOnLoadMethod]` in the application's own assembly):

```csharp
using Atlas.Diagnostics;
using Atlas.Client.Unity;

Profiler.SetBackend(new UnityProfilerBackend());
```

After that, every `using var _ = Profiler.Zone(...)` call inside Atlas.Client
(`ClientCallbacks.DispatchRpc`, `ClientEntity.ApplyPositionUpdate`, etc.)
shows up as a sample in the Unity Profiler window, sharing zone names with
the server's Tracy trace via `Atlas.Diagnostics.ProfilerNames`.

## Logging (Unity-side)

`Atlas.Client` logs through the `IClientLogger` interface and defaults to a
no-op sink. Install the Unity backend during the same boot step so
`ClientCallbacks` / `ClientEntityManager` errors land in Unity's console:

```csharp
using Atlas.Client;
using Atlas.Client.Unity;

ClientLog.SetLogger(new UnityClientLogger());
```

`Info` routes to `Debug.Log`, `Warn` to `Debug.LogWarning`, `Error` to
`Debug.LogError` — so the standard Unity log filters and on-device log
handlers apply unchanged.

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
