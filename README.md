# Atlas Engine

Atlas is a distributed MMO server framework built with **C++20** and **C# (.NET)**. It uses a BigWorld-style split between login, base, cell, database, and manager processes, while gameplay logic runs in managed code through embedded CoreCLR.

**[中文文档](README_CN.md)**

## Why Atlas Exists

Atlas is aimed at large, long-running online worlds where the server must scale across processes, keep simulation authority on the server, and still let gameplay teams work in C#. The C++ core owns networking, process coordination, persistence, spatial simulation, and profiling; the C# layer owns entity behavior, shared gameplay contracts, generated client/server types, and sample game logic.

## Current Capabilities

- Multi-process cluster layout with service discovery through `machined`.
- Base/Cell entity model with mailbox RPC and generated C# entity code.
- CellApp spatial simulation with witness-based AoI, ghost entities, BSP partitioning, and offload coordination.
- Embedded .NET runtime for server-side gameplay scripting.
- C# client runtime with desktop and Unity integration surfaces.
- Runtime-loaded XML and SQLite database plugins.
- Tracy instrumentation, baseline stress drivers, and trace comparison tools.

MySQL, DBAppMgr, and Reviver are represented in the tree, but they are not complete production features yet.

## Architecture

```text
Client
  │
  ▼
LoginApp ──► BaseAppMgr ──► BaseApp ◄──► CellApp
                         │               │
                         ▼               ▼
                       DBApp         CellAppMgr
                         │
                    XML / SQLite
```

| Process | Responsibility |
|---|---|
| `machined` | Local service registry, heartbeat target, and Birth/Death notification hub. |
| `LoginApp` | Client-facing login gateway. |
| `BaseAppMgr` | BaseApp registration, selection, and cluster coordination. |
| `BaseApp` | Base entity ownership, client proxy state, mailbox routing, persistence handoff, and scripting. |
| `CellAppMgr` | CellApp registration, space geometry, BSP partitioning, and offload coordination. |
| `CellApp` | Spatial entities, movement, witness updates, AoI, ghosting, and scripting. |
| `DBApp` | Asynchronous entity persistence through configured database backends. |

Server processes share a small framework hierarchy:

```text
ServerApp
├── ManagerApp     daemon and manager processes without CoreCLR
└── ScriptApp      ServerApp plus embedded CoreCLR
    └── EntityApp  ScriptApp plus entity definitions and background tasks
```

`ServerApp` provides configuration, machined registration, watcher metrics, tick callbacks, and signal dispatch. `BaseApp` and `CellApp` extend it through `EntityApp` so C# gameplay can run on top of the C++ server core.

## Requirements

Windows:

- Visual Studio 2022 with the Desktop development with C++ workload
- CMake 3.28+
- .NET 10 SDK
- Python 3.9+
- Git

Linux:

- GCC 13+ or Clang with C++20 support
- CMake 3.28+
- Ninja for the debug preset
- .NET 10 SDK
- Python 3.9+
- Git

The Windows build wrapper loads the MSVC environment automatically. Ninja is provisioned automatically for the Windows debug preset when it is not already available.

## Build

Use the wrappers for normal development:

```bash
# Windows
tools\bin\build.bat debug
tools\bin\build.bat release
tools\bin\build.bat profile

# Linux / macOS / Git Bash
tools/bin/build.sh debug
tools/bin/build.sh release
tools/bin/build.sh profile
```

Common options:

```bash
tools\bin\build.bat debug --clean
tools\bin\build.bat debug --config-only
tools\bin\build.bat debug --build-only
```

Direct CMake remains supported:

```bash
cmake --preset debug
cmake --build build/debug --config Debug
```

| Preset | Purpose |
|---|---|
| `debug` | Fast iteration with Ninja Multi-Config, Debug, MSVC `/Z7`, PCH, and tests. |
| `release` | Optimized build with tests disabled. |
| `hybrid` | RelWithDebInfo for optimized debugging. |
| `profile` | RelWithDebInfo with Tracy and profiling helpers; tests disabled. |
| `vs` | Visual Studio solution under `build/vs/`, Debug default, with tests (Windows). |

Sanitizer presets are available where supported: `asan`, `asan-msvc`, `tsan`, and `ubsan`.

### Linux build and test from Windows (WSL)

Verify the Linux build against the same Windows clone — no second checkout, no manual sync. Source is read in place from `/mnt/<drive>/...`; build artefacts land in `~/atlas-builds/<preset>` inside the WSL filesystem so cross-FS writes don't cap compile speed. `sccache` is picked up automatically when present, mirroring CI.

One-time prerequisite (elevated PowerShell): `wsl --install -d Ubuntu-26.04`.

```bash
# One-time toolchain install (g++-13, ninja, .NET 10, sccache, clang-format, ...)
tools\bin\setup_linux.bat

# Build (mirrors build.bat semantics)
tools\bin\build_linux.bat
tools\bin\build_linux.bat release
tools\bin\build_linux.bat --clean

# Build + ctest
tools\bin\build_linux.bat --with-tests
tools\bin\build_linux.bat --with-tests --label "unit|integration"
```

Override the build dir with `ATLAS_LINUX_BUILD_DIR` if you want it elsewhere.

## Test

```bash
cd build/debug
ctest --build-config Debug --label-regex unit --output-on-failure
ctest --build-config Debug --label-regex integration --output-on-failure
ctest --build-config Debug -R test_math --output-on-failure
```

C# tests:

```bash
dotnet test tests/csharp
```

Before committing C++ changes, run unit tests and clang-format on changed C++ files:

```bash
clang-format --dry-run --Werror <changed files>
```

## Run a Local Cluster

A development cluster starts in this order:

```text
machined → BaseAppMgr → CellAppMgr → DBApp → BaseApp → CellApp → LoginApp
```

On Linux or Git Bash, the cluster wrapper starts the same stack through the world-stress driver with zero clients and keeps it alive until interrupted:

```bash
tools/bin/run_cluster.sh
```

Scripted cluster validation:

```bash
tools/bin/test_cluster.sh
# Windows
tools\bin\test_cluster.bat
```

## Database

Atlas loads database backends as plugins through the database factory. XML is the lightweight local backend; SQLite provides a single-file relational backend with WAL and busy-timeout support. The MySQL option exists, but the implementation is not complete in the current tree.

Example DBApp configuration:

```json
{
  "database": {
    "type": "sqlite",
    "sqlite_path": "data/atlas_dev.sqlite3",
    "sqlite_wal": true,
    "sqlite_busy_timeout_ms": 5000
  }
}
```

## C# Gameplay and Client Runtime

Managed code lives under the C# project tree and is built with the native server when `ATLAS_BUILD_CSHARP=ON`. `Atlas.Shared` carries shared contracts, `Atlas.Runtime` binds server gameplay to the engine, `Atlas.Client` powers client-side entity state, and `Atlas.Generators.Def` turns entity definitions into typed code.

Sample gameplay projects cover base, client, and stress scenarios.

Unity integration stages the Atlas managed client assemblies and the native `atlas_net_client` plugin for a Unity project.

```bash
tools/bin/setup_unity_client.sh --unity-project <path>
# Windows
tools\bin\setup_unity_client.bat --unity-project <path>
```

## Properties and RPC

Entities are declared in `entity_defs/<Name>.def`. The generator turns each definition into a typed `partial` class on both server and client; gameplay code only fills in implementations.

```xml
<!-- entity_defs/Avatar.def -->
<entity name="Avatar">
  <properties>
    <property name="hp"    type="int32" scope="all_clients" reliable="true" />
    <property name="gold"  type="int32" scope="base"        persistent="true" />
    <property name="level" type="int32" scope="own_client" />
  </properties>

  <client_methods>
    <method name="ShowDamage">
      <arg name="amount"     type="int32" />
      <arg name="attackerId" type="uint32" />
    </method>
  </client_methods>

  <base_methods>
    <method name="UseItem" exposed="own_client">
      <arg name="itemId" type="int32" />
    </method>
  </base_methods>
</entity>
```

Property `scope` controls who sees a write: `all_clients`, `own_client`, `other_clients`, `cell_public`, `cell_public_and_own`, `base`, `base_and_client`. `persistent="true"` opts a field into DBApp persistence; `reliable="true"` routes the delta on the reliable channel.

Server side — assigning to a generated property emits a delta to every observer in scope; `Client.<Method>(...)` is a typed stub that fans out to the matching client RPC.

```csharp
[Entity("Avatar")]
public partial class Avatar : ServerEntity
{
    public partial void UseItem(int itemId)        // exposed="own_client"
    {
        Hp -= 5;                                    // synced to all_clients
        Client.ShowDamage(50, EntityId);            // server → owning client
    }
}
```

Client side — `OnXxxChanged(old, new)` is the generated change hook; client-method partials are invoked when the server pushes an RPC.

```csharp
[Entity("Avatar")]
public partial class Avatar : ClientEntity
{
    partial void OnHpChanged(int oldValue, int newValue) { /* react to sync */ }

    public partial void ShowDamage(int amount, uint attackerId) { /* handle push */ }
}
```

Containers (`list[T]`, `dict[K,V]`, `list[list[int32]]`, …) and user `<struct>` types replicate field- or op-level deltas through the same setter path; see `entity_defs/StressAvatar.def` and `samples/stress/` for the full coverage matrix.

## Profiling and Stress

The `profile` preset enables Tracy instrumentation and profiling helpers. The default baseline runs 200 clients for 120 seconds and writes captures under `.tmp/prof/baseline/`.

```bash
tools\bin\build.bat profile
tools/bin/run_baseline_profile.sh
```

Compare CellApp captures:

```bash
python tools/profile/compare_tracy.py \
  .tmp/prof/baseline/cellapp_<old>.tracy \
  .tmp/prof/baseline/cellapp_<new>.tracy
```

For deeper context, start with the profiling, scripting, gameplay, stress-test, Unity, and coding-style docs under `docs/`.

## License

Atlas is released under the [MIT License](LICENSE).
