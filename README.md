# Atlas Engine

A modern distributed MMO game server framework written in **C++20** with **C# (.NET 9)** scripting, inspired by the **BigWorld Engine** architecture. Features multi-process distributed design with load balancing, spatial partitioning, and fault tolerance, supporting **Windows** and **Linux** cross-platform deployment.

**[中文文档](README_CN.md)**

## Features

- **Distributed Multi-Process Architecture** — LoginApp, BaseApp, CellApp, DBApp and more, with load balancing and fault tolerance
- **Entity System** — Entities distributed across Base / Cell / Client, communicating via Mailbox RPC
- **C# (.NET 9) Scripting** — High-performance C# scripting via embedded CoreCLR; zero-overhead interop with `[UnmanagedCallersOnly]`
- **Cross-Platform** — Full OS API abstraction, unified build on Windows and Linux
- **Pluggable Database** — MySQL (production), SQLite (development), and XML (lightweight fallback) backends
- **Client SDK** — Lightweight connection SDK, not tied to any specific game client engine

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
| **CellApp** | Spatial partitioning, entity movement, AoI (Area of Interest) |
| **DBApp** | Asynchronous database read/write across XML, SQLite, or MySQL backends |
| **BaseAppMgr / CellAppMgr / DBAppMgr** | Cluster load balancing and coordination |
| **Reviver** | Crash detection and automatic recovery |
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
| [CMake](https://cmake.org/download/) | 3.28+ | Build system; can also use the one bundled with Visual Studio |
| [.NET 9 SDK](https://dotnet.microsoft.com/download/dotnet/9.0) | 9.0+ | Required for C# scripting layer; auto-detected at build time |
| [Git](https://git-scm.com/) | Any | Version control |

**Visual Studio 2022 workload required:**
- Workload: **Desktop development with C++**

Verify prerequisites after installation:
```bat
cl /?                  :: MSVC compiler — available after opening VS Developer Command Prompt
cmake --version        :: should be 3.28+
dotnet --version       :: should be 9.x.x
git --version
```

#### Linux (Ubuntu 22.04+)

```bash
# Compiler, build tools, CMake
sudo apt update
sudo apt install -y build-essential g++-13 cmake git

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

> **Third-party dependencies** (Google Test, pugixml, RapidJSON, zlib, SQLite) are managed by CMake via `FetchContent`. They are downloaded and cached automatically on first configure — no manual installation needed.

> **.NET SDK auto-detection:** CMake automatically locates the installed .NET SDK via the `DOTNET_ROOT` environment variable or standard system paths. No version-specific configuration is needed — just install the .NET 9+ SDK.

### Clone

```bash
git clone <repo-url>
cd Atlas
```

### IDE / Editor Setup (optional)

#### VS Code + clangd

1. Install the recommended extensions (prompted automatically, or manually from `.vscode/extensions.json`):
   - **clangd** — C++ IntelliSense via compile_commands.json
   - **CMake Tools** — CMake integration
2. Copy the settings template:
   ```bash
   cp .vscode/settings.json.example .vscode/settings.json
   ```
3. CMake generates `compile_commands.json` automatically in the build directory when `CMAKE_EXPORT_COMPILE_COMMANDS` is enabled (already configured).

#### CLion

1. File -> **Open** -> select the Atlas root directory (CLion detects the `CMakeLists.txt` automatically)
2. Select the desired CMake preset (debug, release, etc.) from the toolbar
3. No additional configuration needed

## Building

### Build Commands

```bash
# Configure (debug by default)
cmake --preset debug

# Build
cmake --build build/debug --config Debug

# Configure + build with a specific configuration
cmake --preset release
cmake --build build/release --config Release

cmake --preset hybrid
cmake --build build/hybrid --config RelWithDebInfo
```

> The first configure downloads third-party dependencies automatically via `FetchContent` and caches them. Subsequent builds are incremental and only recompile changed inputs.

### Build Presets (via `CMakePresets.json`)

| Preset | Description |
|--------|-------------|
| `debug` | Debug mode — full debug symbols, assertions enabled, `ATLAS_DEBUG=1` |
| `release` | Fully optimized, `NDEBUG` defined |
| `hybrid` | Optimized with debug symbols (RelWithDebInfo equivalent) |

### Sanitizers

| Preset | Platform | Description |
|--------|----------|-------------|
| `asan` | Linux | AddressSanitizer (GCC/Clang) |
| `asan-msvc` | Windows | AddressSanitizer (MSVC) |
| `tsan` | Linux | ThreadSanitizer |
| `ubsan` | Linux | UndefinedBehaviorSanitizer |

> TSan and UBSan are not supported by MSVC.

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `ATLAS_BUILD_TESTS` | `ON` | Build unit and integration tests |
| `ATLAS_BUILD_CSHARP` | `ON` | Build C# projects via dotnet |
| `ATLAS_DB_MYSQL` | `OFF` | Enable MySQL database backend |
| `ATLAS_USE_IOURING` | `OFF` | Enable io_uring on Linux |

### Build Outputs

Build artifacts are placed in `build/<preset>/`:

```
build/debug/src/server/
├── machined/Debug/machined         # Machine daemon (start first)
├── loginapp/Debug/atlas_loginapp   # Login gateway
├── baseappmgr/Debug/atlas_baseappmgr
├── baseapp/Debug/atlas_baseapp
├── dbapp/Debug/atlas_dbapp
└── EchoApp/Debug/atlas_echoapp    # Minimal verification app
```

On Windows binaries have `.exe` extension. On Linux, the `Debug/` subdirectory is absent (single-config generator).

## Testing

### Run All Unit Tests

```bash
cd build/debug

# All tests
ctest --build-config Debug

# Unit tests only
ctest --build-config Debug --label-regex unit

# Integration tests only
ctest --build-config Debug --label-regex integration

# All tests with verbose output on failure
ctest --build-config Debug --output-on-failure
```

### Run a Single Test

```bash
# Run a specific test by name
ctest --build-config Debug -R test_math

# Run with verbose output
ctest --build-config Debug -R test_server_app --output-on-failure
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

## Running

Atlas uses a multi-process architecture. Each process must be started **in order** in separate terminals.

### Startup Order

```
machined → DBAppMgr → BaseAppMgr → CellAppMgr → DBApp → BaseApp → CellApp → LoginApp
```

> **Important**: `machined` is the service discovery hub for all processes and must be started first.

### Quick Start — EchoApp (build verification)

`EchoApp` is a minimal standalone process with no dependencies on other services, useful for verifying the build:

```bash
./build/debug/src/server/EchoApp/Debug/atlas_echoapp
```

### Full Cluster (development)

Open a separate terminal for each process and start in order:

```bash
# Terminal 1 — service discovery daemon
./build/debug/src/server/machined/Debug/machined

# Terminal 2 — DBApp manager
# ./build/debug/src/server/dbappmgr/Debug/atlas_dbappmgr  # placeholder

# Terminal 3 — BaseApp manager
./build/debug/src/server/baseappmgr/Debug/atlas_baseappmgr

# Terminal 4 — database process (XML fallback by default; switch to SQLite via config/CLI)
./build/debug/src/server/dbapp/Debug/atlas_dbapp

# Terminal 5 — base entity process
./build/debug/src/server/baseapp/Debug/atlas_baseapp

# Terminal 6 — login gateway (start last; begins accepting client connections)
./build/debug/src/server/loginapp/Debug/atlas_loginapp
```

On Linux (single-config generator), omit the `Debug/` subdirectory.

### Configuration

Each process loads configuration from CLI flags and a JSON config file (via `ServerConfig`). Common options:

| Flag | Description | Example |
|------|-------------|---------|
| `--config <path>` | Path to JSON config file | `--config conf/baseapp.json` |
| `--machined <host:port>` | Address of the machined daemon | `--machined 127.0.0.1:20018` |
| `--internal-port <n>` | Internal listen port for this process | `--internal-port 20010` |
| `--external-port <n>` | External/client-facing port when applicable | `--external-port 20013` |

### Database Backend

- **XML backend** (development/testing) — zero configuration, file-based storage, still the default fallback
- **SQLite backend** (development) — single-file relational backend with checkout / lookup semantics closer to MySQL; requires a runtime SQLite library and supports `sqlite_path`, `sqlite_wal`, and `sqlite_busy_timeout_ms`
- **MySQL backend** (production candidate) — requires a running MySQL instance; configure connection details in the process JSON config

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

Game logic scripts live in the `scripts/` directory and are loaded at runtime via embedded CoreCLR. The .NET runtime configuration is at `runtime/atlas_server.runtimeconfig.json`.

## Project Structure

```
atlas/
├── CMakeLists.txt          Root build file
├── CMakePresets.json        Build presets (debug, release, sanitizers)
├── cmake/                  CMake modules
│   ├── AtlasCompilerOptions.cmake
│   ├── Dependencies.cmake       Third-party deps via FetchContent
│   ├── FindDotNet.cmake          .NET SDK auto-detection
│   └── AtlasDotNetBuild.cmake    C# project build helper
├── docs/
│   ├── roadmap/            Phase-by-phase development plan
│   └── scripting/          C# scripting layer design docs
├── runtime/                .NET runtime configuration
├── scripts/                C# game logic scripts (loaded at runtime)
│   ├── base/               Base entity scripts
│   ├── cell/               Cell entity scripts
│   └── common/             Shared definitions
├── src/
│   ├── lib/                Core libraries
│   │   ├── platform/         OS abstraction layer (I/O, threading, signals, filesystem)
│   │   ├── foundation/       Core utilities (logging, memory, containers, time)
│   │   ├── network/          Sockets, event dispatcher, channels, messages
│   │   ├── serialization/    Binary streams, XML/JSON parsing
│   │   ├── math/             Vectors, matrices, quaternions
│   │   ├── physics/          Physics / collision stubs
│   │   ├── chunk/            World chunk / streaming stubs
│   │   ├── resmgr/           Resource manager stubs
│   │   ├── script/           Script abstraction layer (ScriptEngine / ScriptValue)
│   │   ├── clrscript/        .NET 9 CoreCLR embedding (ClrHost)
│   │   ├── entitydef/        Entity type definitions, data types, mailbox
│   │   ├── connection/       Client-server protocol definitions
│   │   ├── db/               Database abstraction (IDatabase + DatabaseFactory)
│   │   ├── db_mysql/         MySQL backend
│   │   ├── db_sqlite/        SQLite backend
│   │   ├── db_xml/           XML backend
│   │   └── server/           Server framework base classes
│   ├── server/             Server processes
│   │   ├── machined/         Machine daemon
│   │   ├── loginapp/         Login gateway
│   │   ├── baseapp/          Base entity host
│   │   ├── baseappmgr/       BaseApp cluster manager
│   │   ├── cellapp/          Spatial simulation
│   │   ├── cellappmgr/       CellApp cluster manager
│   │   ├── dbapp/            Database process
│   │   ├── dbappmgr/         DBApp cluster manager
│   │   ├── reviver/          Crash detection and recovery
│   │   └── EchoApp/          Minimal verification process
│   ├── csharp/             C# managed libraries
│   │   ├── Atlas.Shared/       Protocol types, entity definitions, RPC contracts
│   │   ├── Atlas.Runtime/      CoreCLR hosting and engine bindings
│   │   ├── Atlas.Generators.Entity/   Source generator for entity classes
│   │   ├── Atlas.Generators.Events/   Source generator for event wiring
│   │   └── Atlas.Generators.Rpc/      Source generator for RPC stubs
│   ├── client_sdk/         Client connection SDK
│   └── client/             Console client application
├── tests/
│   ├── unit/               C++ unit tests (Google Test, 70 tests)
│   ├── integration/        Integration tests (7 tests)
│   └── csharp/             C# smoke tests
└── docker/                 Container deployment
```

## License

This project is licensed under the [MIT License](LICENSE).
