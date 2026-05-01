# IL2CPP Callback Spike (Phase 0)

**Status:** ✅ Decision recorded. Unity 2022 — 6.5 require **Pattern B**
(`[MonoPInvokeCallback]` + delegate). Pattern A (`[UnmanagedCallersOnly]`)
is unsupported across the entire range. Full matrix and forward-compat
migration path:
[`docs/spike_il2cpp_callback.md`](../../../docs/spike_il2cpp_callback.md).

The probe stays in the tree as a regression check for Unity 6.6+
(planned to embed .NET 10, where Pattern A should light up). Rerun the
matrix against any new Unity LTS before flipping
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
cmake --build build/debug --target atlas_il2cpp_probe --config Debug
# → bin/debug/atlas_il2cpp_probe.dll

# Android arm64 (cross-compile via NDK; needs $ANDROID_NDK_HOME)
cmake -S . -B build/android_arm64 \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
    -DATLAS_BUILD_IL2CPP_PROBE=ON \
    -DATLAS_BUILD_TESTS=OFF -DATLAS_BUILD_CSHARP=OFF \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build/android_arm64 --target atlas_il2cpp_probe
# → libatlas_il2cpp_probe.so

# iOS arm64 (must run on macOS host)
cmake -S . -B build/ios_arm64 -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
    -DATLAS_BUILD_IL2CPP_PROBE=ON \
    -DATLAS_BUILD_TESTS=OFF -DATLAS_BUILD_CSHARP=OFF
cmake --build build/ios_arm64 --target atlas_il2cpp_probe --config Release
# → libatlas_il2cpp_probe.a (iOS uses static linking; no dylibs allowed)

# macOS arm64 (Unity Editor on Apple Silicon)
cmake -S . -B build/macos_arm64 -DATLAS_BUILD_IL2CPP_PROBE=ON \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=Release
cmake --build build/macos_arm64 --target atlas_il2cpp_probe
# → atlas_il2cpp_probe.bundle
```

## Set up the Unity test project

1. Unity Hub → New project → 3D (URP/Built-in either OK) → Unity 2022.3 LTS
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
   - **Editor**: Mono baseline; just press Play.
   - **Standalone Windows**: Switch backend to IL2CPP, build & run.
   - **Android**: Set Scripting Backend = IL2CPP, ARM64 only, build & run on
     a physical arm64 device.
   - **iOS**: Build to Xcode project on a Mac, run on a physical device.

## Expected output

Per platform, you should see Console / logcat / Xcode console messages:

```
[ProbeComponent] Platform=… Backend=…
[ProbeComponent] Pattern A fired with 42
[ProbeComponent] Pattern B fired with 42
```

## Decision

| Result | Action |
|---|---|
| **Both A and B fire on all 4 targets** | Pick A (matches design doc, no GC pinning needed) |
| **A fires only on Mono; B fires everywhere** | Drop the design to B; rewrite §6.3 of the design doc to use `[MonoPInvokeCallback]` + delegates + `GCHandle` pinning |
| **Neither fires on some target** | Bump Unity minimum to 2023 LTS, re-run; if still failing, escalate (reverse P/Invoke trampoline) |

Record the matrix + final pick in `docs/spike_il2cpp_callback.md`. Once
recorded, the spike branch can be merged or discarded — the decision
output is what matters.
