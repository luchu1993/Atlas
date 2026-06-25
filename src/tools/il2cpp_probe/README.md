# IL2CPP Callback Probe

**Status:** ✅ Current decision recorded. Unity 2022.3 LTS and the current MVP
Unity 6 editor (`6000.0.43f1-lilith-2`) require **Pattern B**
(`[MonoPInvokeCallback]` + delegate). Pattern A (`[UnmanagedCallersOnly]`) is
unsupported across that retained validation range.
Full matrix and forward-compat migration path:
[`docs/spike_il2cpp_callback.md`](../../../docs/spike_il2cpp_callback.md).

The probe stays in the tree as a regression check for future target Unity
runtimes whose embedded runtime / BCL may support Pattern A. Rerun the matrix
against the exact Unity version before flipping
`UNITY_NATIVE_DLL_DESIGN.md` §6.3 to Pattern A.

## What this is

- `probe.cc` — minimal native library, two exports (`probe_set_callback`,
  `probe_fire`). Zero Atlas dependencies — measures the FFI layer alone.
- `Unity/ProbeComponent.cs` — Unity MonoBehaviour that fires both patterns
  in `Start()`:
  - **Pattern A**: `[UnmanagedCallersOnly]` + function pointer (.NET 5+ native style)
  - **Pattern B**: `[MonoPInvokeCallback]` + delegate (IL2CPP-AOT style)

## Build the native library

`ATLAS_BUILD_IL2CPP_PROBE` is OFF by default; enable per-target.

```bash
# Windows x64 (the dev driver)
cmake --preset debug -DATLAS_BUILD_IL2CPP_PROBE=ON
cmake --build --preset debug --target atlas_il2cpp_probe --config Debug
# → bin/debug/atlas_il2cpp_probe.dll

# Android arm64 (cross-compile via NDK; needs $ANDROID_NDK_HOME)
cmake --preset net-client-android-arm64 -DATLAS_BUILD_IL2CPP_PROBE=ON
cmake --build --preset net-client-android-arm64 --target atlas_il2cpp_probe
# → bin/net-client-android-arm64/libatlas_il2cpp_probe.so

# iOS arm64 (must run on macOS host)
cmake --preset net-client-ios-arm64 -DATLAS_BUILD_IL2CPP_PROBE=ON
cmake --build --preset net-client-ios-arm64 --target atlas_il2cpp_probe --config Release
# → bin/net-client-ios-arm64/libatlas_il2cpp_probe.a

# macOS arm64 (Unity Editor on Apple Silicon)
cmake --preset net-client-macos-arm64 -DATLAS_BUILD_IL2CPP_PROBE=ON
cmake --build --preset net-client-macos-arm64 --target atlas_il2cpp_probe
# → bin/net-client-macos-arm64/atlas_il2cpp_probe.bundle
```

## Set up the Unity test project

1. Unity Hub → New project → 3D (URP/Built-in either OK) → Unity 2022.3 LTS
   or the current MVP Unity 6 editor version.
2. Drop the build artifacts into `Assets/Plugins/IL2CPPProbe/`:

   ```
   Assets/Plugins/IL2CPPProbe/
   ├── x86_64/
   │   └── atlas_il2cpp_probe.dll                (Windows Standalone + Editor on Win)
   ├── Android/arm64-v8a/
   │   └── libatlas_il2cpp_probe.so
   ├── iOS/
   │   └── libatlas_il2cpp_probe.a
   └── macOS/
       └── atlas_il2cpp_probe.bundle             (Editor on macOS)
   ```

   In Unity Inspector for each binary, check only the platforms it targets
   (so e.g. the iOS `.a` doesn't try to load on Android builds).

3. Copy `Unity/ProbeComponent.cs` into `Assets/Scripts/`.

4. Create an empty scene, add an empty GameObject, attach `ProbeComponent`.

5. Player Settings:
   - Enable **Allow 'unsafe' Code** for the test assembly, because
     `ProbeComponent` uses a function pointer for Pattern A.
   - **Editor**: Mono baseline; just press Play.
   - **Standalone Windows**: Switch backend to IL2CPP, build & run.
   - **Android**: Set Scripting Backend = IL2CPP, ARM64 only, build & run on
     a physical arm64 device.
   - **iOS**: Build to Xcode project on a Mac, run on a physical device.

## Expected output

On Unity 2022.3 LTS and the current MVP Unity 6 editor version, Pattern B is
the expected passing path. Pattern A may throw or fail to fire depending on
runtime/backend. The key success signal is:

```
[ProbeComponent] Platform=… Backend=…
[ProbeComponent] Pattern B fired with 42
```

On any future Unity runtime rerun, record whether Pattern A also fires on every
target before considering a bridge migration.

## Decision

| Result | Action |
|---|---|
| **Both A and B fire on all target platforms** | Pattern A becomes a candidate; update `UNITY_NATIVE_DLL_DESIGN.md` and migrate behind a build-time toggle |
| **A fails anywhere; B fires everywhere** | Keep Pattern B |
| **Neither fires on some target** | Bump Unity minimum to 2023 LTS, re-run; if still failing, escalate (reverse P/Invoke trampoline) |

Update `docs/spike_il2cpp_callback.md` when a rerun changes the matrix or
the final callback pattern.
