# Atlas Client SDK (Unity)

Drop-in Unity package for the Atlas Engine client. Wraps the native
`atlas_net_client` DLL plus the managed `Atlas.Client` /
`Atlas.Shared` assemblies, exposes `AtlasNetworkManager` as the single
MonoBehaviour entry point.

## Quick start

From the Atlas repo root:

```bash
# Windows
tools\bin\setup_unity_client.bat --unity-project C:\path\to\YourUnityProject

# Linux / macOS
tools/bin/setup_unity_client.sh --unity-project ~/path/to/YourUnityProject
```

The tool builds the host-platform binaries, copies them into
`Plugins/`, and registers the package in your Unity project's
`Packages/manifest.json`. Open the project in Unity Hub and play.

## Documentation

Full integration guide:
[`docs/client/UNITY_INTEGRATION.md`](../../docs/client/UNITY_INTEGRATION.md)
— Plugin Import Settings, API surface, Android / iOS targets,
troubleshooting.

Architecture rationale:
[`docs/client/UNITY_NATIVE_DLL_DESIGN.md`](../../docs/client/UNITY_NATIVE_DLL_DESIGN.md)
— ABI contract, Pattern B vs Pattern A migration plan.

## Layout

```
com.atlas.client/
├── package.json
├── README.md (this file)
├── Runtime/
│   ├── Atlas.Client.Unity.asmdef
│   └── AtlasNetworkManager.cs
└── Plugins/                     # populated by setup_unity_client tool
    ├── Atlas.Client.dll
    ├── Atlas.Shared.dll
    ├── Windows/x86_64/atlas_net_client.dll
    ├── Linux/x86_64/libatlas_net_client.so
    ├── macOS/atlas_net_client.bundle
    ├── Android/arm64-v8a/libatlas_net_client.so
    └── iOS/libatlas_net_client.a
```

Mobile binaries come from the `net_client cross-platform` GitHub
Actions workflow artefact — the local setup tool only builds for the
host platform.
