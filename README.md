# Atlas Engine

A modern distributed MMO game server framework written in **C++20** with **C# (.NET 9)** scripting, inspired by the **BigWorld Engine** architecture. Features multi-process distributed design with load balancing, spatial partitioning, and fault tolerance, supporting **Windows** and **Linux** cross-platform deployment.

**[中文文档](README_CN.md)**

## Features

- **Distributed Multi-Process Architecture** — LoginApp, BaseApp, CellApp, DBApp and their managers, with load balancing and fault tolerance
- **Spatial Partitioning** — CellApp + CellAppMgr maintain a BSP-tree partition with witness-based AoI, ghost entities, and cross-cell offload
- **Entity System** — Entities distributed across Base / Cell / Client, communicating via Mailbox RPC
- **C# (.NET 9) Scripting** — High-performance C# scripting via embedded CoreCLR; zero-overhead interop with `[UnmanagedCallersOnly]`
- **Cross-Platform** — Full OS API abstraction, unified build on Windows and Linux
- **Pluggable Database** — MySQL (production), SQLite (development), and XML (lightweight fallback) backends
- **Client Runtime** — C# `Atlas.Client` with desktop and Unity surfaces; `Atlas.Generators.Def` source generator emits typed entity classes / delta sync from `.def` files

## Architecture

```
Client ──► LoginApp ──► BaseAppMgr ──► BaseApp ◄──► CellApp
                                          │              │
                                        DBApp        CellAppMgr
                                          │
                                   MySQL / SQLite / XML
```

| Process | Role |
|---------|------|
| **LoginApp** | Client authentication and login |
| **BaseApp** | Entity state management, client proxy, persistence |
| **CellApp** | Spatial simulation, entity movement, AoI (Area of Interest), ghost replication |
| **CellAppMgr** | BSP-tree spatial partitioning, cell offload, geometry distribution |
| **DBApp** | Asynchronous database read/write across XML, SQLite, or MySQL backends |
| **BaseAppMgr / DBAppMgr** | Cluster load balancing and coordination |
| **Reviver** | Crash detection and automatic recovery (placeholder) |
| **machined** | Machine daemon, service registration and discovery |

## Server Framework (`src/lib/server/`)

The `server` library provides the base class hierarchy shared by all Atlas server processes:

```
ServerApp
├── ManagerApp          — manager/daemon processes (no scripting)
│   ├── BaseAppMgr
│   ├── CellAppMgr
│   ├── DBAppMgr
│   ├── machined
│   └── EchoApp         — minimal verification app
└── ScriptApp           — ServerApp + CoreCLR scripting layer
    └── EntityApp       — ScriptApp + entity definitions + background task pool
        ├── BaseApp
        └── CellApp
```

Key components provided by `ServerApp`:

- **`ServerConfig`** — loads process configuration from CLI flags and JSON
- **`MachinedClient`** — TCP connection to machined for registration, heartbeats, service discovery, and Birth/Death notifications
- **`WatcherRegistry`** — hierarchical path-based registry for observable process metrics (read/write via path strings)
- **`Updatable` / `Updatables`** — level-ordered per-tick callback system; safe for add/remove during iteration
- **`SignalDispatchTask`** — dispatches OS signals (SIGINT, SIGTERM, etc.) into the event loop

## Development Setup

### Prerequisites

#### Windows

| Tool | Version | Notes |
|------|---------|-------|
| [Visual Studio 2022](https://visualstudio.microsoft.com/) | 17.x | Select **"Desktop development with C++"** workload |
| [CMake](https://cmake.org/download/) | 3.28+ | Build system; the version bundled with Visual Studio also works |
| [.NET 9 SDK](https://dotnet.microsoft.com/download/dotnet/9.0) | 9.0+ | Required for C# scripting layer; auto-detected at build time |
| [Python](https://www.python.org/) | 3.9+ | Required only for `tools/build.{bat,sh}` and profiling helpers |
| [Git](https://git-scm.com/) | Any | Version control |
| [Ninja](https://ninja-build.org/) | 1.11+ | Optional — auto-downloaded to `.tmp/ninja/` on first run if missing |

> The `tools/build.bat` helper auto-loads the MSVC environment via `vswhere` + `vcvars64.bat`, so you don't need to launch the **x64 Native Tools Command Prompt** manually.

Verify after installation:
```bat
cmake --version        :: should be 3.28+
dotnet --version       :: should be 9.x.x
python --version       :: should be 3.9+
```

#### Linux (Ubuntu 22.04+)

```bash
# Compiler, build tools, CMake, Ninja
sudo apt update
sudo apt install -y build-essential g++-13 cmake ninja-build git python3

# .NET 9 SDK — https://learn.microsoft.com/dotnet/core/install/linux
wget https://dot.net/v1/dotnet-install.sh
chmod +x dotnet-install.sh
./dotnet-install.sh --channel 9.0
echo 'export PATH="$HOME/.dotnet:$PATH"' >> ~/.bashrc
source ~/.bashrc

# Verify
g++ --version          # should be 13+
cmake --version        # should be 3.28+
dotnet --version       # should be 9.x.x
```

> **Third-party dependencies** (Google Test, pugixml, RapidJSON, zlib, SQLite, Tracy, mimalloc) are managed by CMake via `FetchContent` and downloaded on first configure.

### Clone

```bash
git clone <repo-url>
cd Atlas
```

### IDE / Editor Setup (optional)

#### VS Code + clangd

1. Install the recommended extensions (prompted automatically, or manually from `.vscode/extensions.json`):
   - **clangd** — C++ IntelliSense via compile_commands.json
   - **CMake Tools** — CMake integration (auto-loads MSVC env on Windows)
2. Copy the settings template:
   ```bash
   cp .vscode/settings.json.example .vscode/settings.json
   ```
3. CMake generates `compile_commands.json` automatically (`CMAKE_EXPORT_COMPILE_COMMANDS` is on by default).

#### CLion / Visual Studio

Open the Atlas root directory directly. The IDE detects `CMakePresets.json` and loads the toolchain (including MSVC env) automatically.

## Building

### One-shot helper (recommended)

`tools/build.py` wraps cmake configure + build with two daily papercuts handled:

- **Windows**: auto-loads MSVC env via vswhere + vcvars64.bat (no x64 Native Tools shell needed)
- **Any OS**: auto-downloads the matching Ninja binary to `.tmp/ninja/` if missing from PATH

```bash
# Windows (cmd or PowerShell)
tools\build.bat debug
tools\build.bat profile
tools\build.bat release

# Linux / macOS
tools/build.sh debug
tools/build.sh profile
tools/build.sh release

# Options (apply to any preset)
tools\build.bat debug --clean         # wipe build/<preset> before configuring
tools\build.bat debug --config-only   # cmake configure, skip build
tools\build.bat debug --build-only    # skip configure, just build
```

### Direct cmake (advanced)

```bash
# debug preset uses Ninja Multi-Config; requires Ninja on PATH and MSVC env loaded
cmake --preset debug
cmake --build build/debug --config Debug

# profile / release stay on the default generator (VS solution on Windows, Make on Linux)
cmake --preset profile
cmake --build build/profile --config RelWithDebInfo

cmake --preset release
cmake --build build/release --config Release
```

### Build Presets

| Preset | Generator | Config | Notes |
|--------|-----------|--------|-------|
| `debug` | Ninja Multi-Config | Debug | `/Z7` debug info + `atlas_common.h` PCH; fastest iteration |
| `release` | Default | Release | Fully optimized, `NDEBUG`, **no tests** |
| `hybrid` | Default | RelWithDebInfo | Optimized + debug symbols |
| `profile` | Default | RelWithDebInfo | Tracy + viewer + CLI helpers; **no tests**; for performance work |

> The `debug` preset switched to Ninja Multi-Config because MSBuild's per-project overhead dominated full-debug iteration time. `/Z7` (embedded CodeView) avoids the mspdbsrv.exe lock contention `/Zi` introduces under parallel cl.exe.

### Sanitizers

| Preset | Platform | Description |
|--------|----------|-------------|
| `asan` | Linux | AddressSanitizer (GCC/Clang) |
| `asan-msvc` | Windows | AddressSanitizer (MSVC) |
| `tsan` | Linux | ThreadSanitizer |
| `ubsan` | Linux | UndefinedBehaviorSanitizer |

> TSan and UBSan are not supported by MSVC. CI runs `asan` + `ubsan` weekly via `.github/workflows/sanitizers.yml`.

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `ATLAS_BUILD_TESTS` | `ON` | Build unit and integration tests |
| `ATLAS_BUILD_CSHARP` | `ON` | Build C# projects via dotnet |
| `ATLAS_DB_MYSQL` | `OFF` | Enable MySQL database backend |
| `ATLAS_USE_IOURING` | `OFF` | Enable io_uring poller (Linux only) |
| `ATLAS_HEAP_ALLOCATOR` | `mimalloc` | `std` or `mimalloc` heap backend |

### Build Outputs

All EXE / DLL / .lib artifacts land in a flat `bin/<preset>/` tree, regardless of generator:

```
bin/debug/
├── machined.exe
├── atlas_loginapp.exe
├── atlas_baseappmgr.exe
├── atlas_baseapp.exe
├── atlas_cellappmgr.exe
├── atlas_cellapp.exe
├── atlas_dbapp.exe
├── atlas_echoapp.exe
├── atlas_client.exe
├── atlas_tool.exe
├── Atlas.Runtime.dll
├── Atlas.Shared.dll
├── ...                  (other managed assemblies, runtime DLLs)
└── test_*.exe           (test executables, debug preset only)
```

On Linux, omit the `.exe` suffix.

## Testing

```bash
cd build/debug

# All tests
ctest --build-config Debug --output-on-failure

# Unit tests only
ctest --build-config Debug --label-regex unit

# Integration tests only
ctest --build-config Debug --label-regex integration

# Single test
ctest --build-config Debug -R test_math --output-on-failure
```

### C# Tests

```bash
dotnet test tests/csharp
```

### Code Style Check (pre-commit)

```bash
# Windows (clang-format shipped with VS)
"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-format.exe" --dry-run --Werror <changed files>

# Linux / cross-platform
clang-format --dry-run --Werror <changed files>

# Fix in place
clang-format -i <changed files>
```

CI enforces clang-format 19.1.5 via `.github/workflows/clang-format.yml`.

## Running

Atlas uses a multi-process architecture. Each process must be started **in order** in separate terminals.

### Startup Order

```
machined → BaseAppMgr → CellAppMgr → DBApp → BaseApp → CellApp → LoginApp
```

> **Important**: `machined` is the service discovery hub for all processes and must be started first.

### Quick Start — EchoApp (build verification)

`EchoApp` is a minimal standalone process with no external dependencies, useful for validating a build:

```bash
./bin/debug/atlas_echoapp        # add .exe on Windows
```

### Full Cluster (development)

Open a separate terminal per process and start in order:

```bash
./bin/debug/machined
./bin/debug/atlas_baseappmgr
./bin/debug/atlas_cellappmgr
./bin/debug/atlas_dbapp
./bin/debug/atlas_baseapp
./bin/debug/atlas_cellapp
./bin/debug/atlas_loginapp        # last — opens external port for clients
```

For multi-client load testing see `tools/cluster_control/run_baseline_profile.{sh,ps1}` and `src/tools/world_stress/`.

### Configuration

Each process loads configuration from CLI flags and a JSON config file (via `ServerConfig`). Common options:

| Flag | Description | Example |
|------|-------------|---------|
| `--config <path>` | Path to JSON config file | `--config conf/baseapp.json` |
| `--machined <host:port>` | Address of the machined daemon | `--machined 127.0.0.1:20018` |
| `--internal-port <n>` | Internal listen port for this process | `--internal-port 20010` |
| `--external-port <n>` | External/client-facing port when applicable | `--external-port 20013` |

### Database Backend

- **XML backend** (development/testing) — zero configuration, file-based storage; default fallback
- **SQLite backend** (development) — single-file relational backend with checkout / lookup semantics close to MySQL; supports `sqlite_path`, `sqlite_wal`, `sqlite_busy_timeout_ms`
- **MySQL backend** (production candidate) — requires a running MySQL instance; build with `-DATLAS_DB_MYSQL=ON`

Example database config:

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

Common DBApp CLI overrides:

```bash
--db-type sqlite
--db-sqlite-path data/atlas_dev.sqlite3
--db-sqlite-wal true
--db-sqlite-busy-timeout-ms 5000
```

### C# Scripts

Game logic lives in `src/csharp/` (`Atlas.Shared`, `Atlas.Runtime`) and is loaded at runtime via embedded CoreCLR. Sample script projects live in `samples/base/`, `samples/client/`, and `samples/stress/`. Generated entity classes / delta sync come from `Atlas.Generators.Def` (driven by `.def` files).

The .NET runtime configuration is at `runtime/atlas_server.runtimeconfig.json`.

## Profiling

The `profile` preset enables Tracy instrumentation and downloads the viewer + CLI helpers (`tracy-capture`, `tracy-csvexport`) to `bin/profile/`.

```bash
# Build profile
tools\build.bat profile

# Run a baseline (200 clients, 120 s) — Linux / Git Bash
bash tools/cluster_control/run_baseline_profile.sh

# Windows PowerShell
.\tools\cluster_control\run_baseline_profile.ps1 -Clients 50 -DurationSec 60

# Compare two cellapp captures
python tools/profile/compare_tracy.py \
    .tmp/prof/baseline/cellapp_<old>.tracy \
    .tmp/prof/baseline/cellapp_<new>.tracy
```

Captures land in `.tmp/prof/baseline/` named `<process>_<git-short>_<timestamp>.tracy`.

## Project Structure

```
atlas/
├── CMakeLists.txt              Root build file
├── CMakePresets.json           Build presets (debug, release, profile, hybrid, sanitizers)
├── cmake/                      CMake modules
│   ├── AtlasCompilerOptions.cmake
│   ├── AtlasOutputDirectory.cmake  Flat bin/<preset>/ layout
│   ├── Dependencies.cmake          Third-party deps via FetchContent
│   ├── FindDotNet.cmake            .NET SDK auto-detection
│   └── AtlasDotNetBuild.cmake      C# project build helper
├── docs/                       Design docs (roadmap, scripting, gameplay, rpc, optimization, …)
├── runtime/                    .NET runtime configuration
├── samples/                    Sample game scripts (base / client / stress)
├── src/
│   ├── lib/                    Core libraries
│   │   ├── platform/             OS abstraction (I/O, threading, signals, filesystem)
│   │   ├── foundation/           Core utilities (logging, memory, containers, time, atlas_common.h PCH)
│   │   ├── network/              Sockets, RUDP, event dispatcher, channels, messages
│   │   ├── serialization/        Binary streams, XML/JSON parsing
│   │   ├── math/                 Vectors, matrices, quaternions
│   │   ├── physics/              Physics / collision (placeholder)
│   │   ├── resmgr/               Resource manager (placeholder)
│   │   ├── coro/                 C++20 coroutine helpers (RPC await, cancellation)
│   │   ├── script/               Script abstraction (ScriptEngine / ScriptValue)
│   │   ├── clrscript/            .NET 9 CoreCLR embedding (ClrHost, native-API provider)
│   │   ├── entitydef/            Entity type definitions, data types, mailbox
│   │   ├── connection/           Client-server protocol definitions
│   │   ├── space/                Space + cell shared types (used by cellapp / cellappmgr)
│   │   ├── db/                   Database abstraction (IDatabase + DatabaseFactory)
│   │   ├── db_mysql/             MySQL backend
│   │   ├── db_sqlite/            SQLite backend
│   │   ├── db_xml/               XML backend
│   │   └── server/               Server framework base classes (ServerApp / EntityApp / ManagerApp)
│   ├── server/                 Server processes
│   │   ├── machined/             Machine daemon
│   │   ├── loginapp/             Login gateway
│   │   ├── baseappmgr/           BaseApp cluster manager
│   │   ├── baseapp/              Base entity host
│   │   ├── cellappmgr/           CellApp cluster manager (BSP-tree partition, offload)
│   │   ├── cellapp/              Spatial simulation (witness, AoI, ghost, controller)
│   │   ├── dbapp/                Database process
│   │   ├── dbappmgr/             DBApp cluster manager (placeholder)
│   │   ├── reviver/              Crash detection and recovery (placeholder)
│   │   └── EchoApp/              Minimal verification process
│   ├── csharp/                 C# managed libraries
│   │   ├── Atlas.Shared/             Protocol types, entity definitions, RPC contracts
│   │   ├── Atlas.Runtime/            Server-side CoreCLR hosting and engine bindings
│   │   ├── Atlas.Client/             Client entity runtime (callbacks / factory / manager)
│   │   ├── Atlas.Client.Desktop/     Desktop client adapter
│   │   ├── Atlas.Client.Unity/       Unity asmdef + adapters
│   │   ├── Atlas.ClrHost/            CoreCLR hostfxr wrapper
│   │   ├── Atlas.Generators.Def/     Source generator: .def → entity classes / delta sync
│   │   ├── Atlas.Generators.Events/  Source generator: event wiring
│   │   └── Atlas.Tools.DefDump/      .def inspector / dumper
│   ├── client/                 Console client application (connection + native provider)
│   └── tools/                  Operator tooling
│       ├── atlas_tool/            Multi-purpose CLI (config validation, watcher inspect, …)
│       ├── login_stress/          Login-only stress driver
│       ├── world_stress/          Full-cluster stress driver (used by run_baseline_profile)
│       └── crash_demo/            Crash-handler verification
├── tests/
│   ├── unit/                   C++ unit tests (Google Test)
│   ├── integration/            End-to-end integration tests (Google Test)
│   └── csharp/                 C# tests (Atlas.Runtime.Tests, Atlas.Generators.Tests, Atlas.SmokeTest)
└── tools/                      Build / cluster / profile helpers
    ├── build.py / .bat / .sh     One-shot configure + build with auto vcvars + Ninja provisioning
    ├── cluster_control/          Multi-process orchestration + baseline runners
    └── profile/                  Tracy capture comparison + analysis scripts
```

## License

This project is licensed under the [MIT License](LICENSE).
