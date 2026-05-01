# Atlas Client SDK (Unity)

Drop-in Unity package for the Atlas Engine client. Wraps the native
`atlas_net_client` DLL plus the managed `Atlas.Client` / `Atlas.Shared`
assemblies, exposes a single `AtlasNetworkManager` MonoBehaviour as the
game-code entry point.

## Layout

```
com.atlas.client/
├── package.json
├── README.md
├── Runtime/
│   ├── Atlas.Client.Unity.asmdef     # references managed plugins below
│   └── AtlasNetworkManager.cs        # MonoBehaviour glue
└── Plugins/
    ├── Atlas.Client.dll              # netstandard2.1 — compiled from src/csharp/Atlas.Client
    ├── Atlas.Shared.dll              # netstandard2.1 — compiled from src/csharp/Atlas.Shared
    ├── Windows/x86_64/atlas_net_client.dll
    ├── Linux/x86_64/libatlas_net_client.so
    ├── Android/arm64-v8a/libatlas_net_client.so
    ├── iOS/libatlas_net_client.a
    └── macOS/atlas_net_client.bundle
```

`Plugins/` is empty until the binaries are populated. See *Building the
binaries* below.

## Building the binaries

Native (`atlas_net_client`) — built from the Atlas source tree:

```bash
# Windows host, builds Windows x86_64
cmake --preset release -DATLAS_BUILD_NET_CLIENT=ON
cmake --build build/release --target atlas_net_client --config Release
# → bin/release/atlas_net_client.dll

# Cross-compile to Android arm64 (NDK r25+)
cmake -S . -B build/android_arm64 \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
    -DATLAS_BUILD_NET_CLIENT=ON \
    -DATLAS_BUILD_TESTS=OFF -DATLAS_BUILD_CSHARP=OFF \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build/android_arm64 --target atlas_net_client

# iOS arm64 — must run on macOS host
cmake -S . -B build/ios_arm64 -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
    -DATLAS_BUILD_NET_CLIENT=ON \
    -DATLAS_BUILD_TESTS=OFF -DATLAS_BUILD_CSHARP=OFF
cmake --build build/ios_arm64 --target atlas_net_client_static --config Release
```

Managed (`Atlas.Client.dll` / `Atlas.Shared.dll`):

```bash
dotnet build src/csharp/Atlas.Client/Atlas.Client.csproj -c Release
dotnet build src/csharp/Atlas.Shared/Atlas.Shared.csproj -c Release
```

Copy the `.dll` outputs into `Packages/com.atlas.client/Plugins/` (root
of `Plugins/` for the managed assemblies; platform subfolders for the
native artefacts as shown in the layout above).

## Usage

```csharp
using Atlas.Client.Native;
using Atlas.Client.Unity;
using UnityEngine;

public class Bootstrap : MonoBehaviour
{
    [SerializeField] AtlasNetworkManager net;

    void Start()
    {
        net.LoginFinished += (status, err) =>
        {
            if (status == AtlasLoginStatus.Success) net.Authenticate();
            else Debug.LogError($"login failed: {status} {err}");
        };
        net.AuthFinished += (ok, eid, tid, err) =>
        {
            if (ok) Debug.Log($"connected as entity {eid}");
        };
        net.Login("alice", PasswordHash.Of("alice", "hunter2"));
    }
}
```

## Phase status

- Phase 5 (this package skeleton) — ✅
- Phase 6 (cross-platform native builds in CI) — ⬜

See [`docs/client/UNITY_NATIVE_DLL_DESIGN.md`](../../docs/client/UNITY_NATIVE_DLL_DESIGN.md)
§7 for the architectural rationale.
