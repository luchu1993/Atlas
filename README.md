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
| [Bazelisk](https://github.com/bazelbuild/bazelisk) | Latest | Bazel version manager; reads `.bazelversion` and downloads the correct Bazel binary |
| [.NET 9 SDK](https://dotnet.microsoft.com/download/dotnet/9.0) | 9.0+ | Required for C# scripting layer; auto-detected at build time |
| [Git](https://git-scm.com/) | Any | Version control |

**Install Bazelisk (Windows):**
```bat
winget install Google.Bazelisk
```

**Visual Studio 2022 workload required:**
- Workload: **Desktop development with C++**

Verify prerequisites after installation:
```bat
cl /?                  :: MSVC compiler — available after opening VS Developer Command Prompt
bazel --version        :: should show 7.x (auto-downloaded by Bazelisk)
dotnet --version       :: should be 9.x.x
git --version
```

#### Linux (Ubuntu 22.04+)

```bash
# Compiler, build tools
sudo apt update
sudo apt install -y build-essential g++-12 git

# Bazelisk — manages Bazel versions automatically
curl -Lo /usr/local/bin/bazel \
  https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64
chmod +x /usr/local/bin/bazel

# .NET 9 SDK — https://learn.microsoft.com/dotnet/core/install/linux
wget https://dot.net/v1/dotnet-install.sh
chmod +x dotnet-install.sh
./dotnet-install.sh --channel 9.0
echo 'export PATH="$HOME/.dotnet:$PATH"' >> ~/.bashrc
source ~/.bashrc

# Verify
g++ --version          # should be 12+
bazel --version        # should show 7.x (auto-downloaded by Bazelisk)
dotnet --version       # should be 9.x.x
```

> **Third-party dependencies** (Google Test, pugixml, RapidJSON, zlib, SQLite) are managed by Bazel via `MODULE.bazel` and `deps.bzl`. They are downloaded and cached automatically on first build — no manual installation needed.

> **.NET SDK auto-detection:** Bazel automatically locates the installed .NET SDK via the `DOTNET_ROOT` environment variable or standard system paths. No version-specific configuration is needed — just install the .NET 9+ SDK.

> **SQLite backend runtime note:** the SQLite backend loads the system SQLite library dynamically at runtime (`sqlite3.dll` / `winsqlite3.dll` on Windows, `libsqlite3.so*` on Linux/macOS). Atlas does not vendor SQLite into the build tree.

### Clone

```bash
git clone <repo-url>
cd Atlas
```

### IDE / Editor Setup (optional)

#### VS Code + clangd

1. Install the recommended extensions (prompted automatically, or manually from `.vscode/extensions.json`):
   - **clangd** — C++ IntelliSense via compile_commands.json
   - **Bazel** — BUILD file syntax highlighting
2. Copy the settings template:
   ```bash
   cp .vscode/settings.json.example .vscode/settings.json
   ```
3. Generate `compile_commands.json`:
   ```bash
   bazel run :refresh_compile_commands
   ```
4. Re-run step 3 whenever you add/remove source files or change BUILD targets.

#### CLion

1. Install the **Bazel for CLion** plugin (Settings -> Plugins -> Marketplace)
2. File -> **Import Bazel Project** -> select the Atlas root directory
3. No additional configuration needed — CLion syncs with Bazel directly

## Building

### Build Commands

```bash
# Build everything (debug by default)
bazel build //...

# Build with a specific configuration
bazel build //... --config=debug
bazel build //... --config=release
bazel build //... --config=hybrid     # optimized with debug symbols

# Build a specific target
bazel build //src/server/machined:machined
bazel build //src/lib/network:atlas_network
```

> The first build downloads third-party dependencies automatically and caches them. Subsequent builds are incremental and only recompile changed inputs.

### Build Configurations (via `.bazelrc`)

| Config | Description |
|--------|-------------|
| *(default)* | Debug mode — full debug symbols, assertions enabled |
| `--config=debug` | Explicit debug mode, `ATLAS_DEBUG=1` |
| `--config=release` | Fully optimized, `NDEBUG` defined |
| `--config=hybrid` | Optimized with debug symbols (RelWithDebInfo equivalent) |

### Sanitizers

| Config | Platform | Description |
|--------|----------|-------------|
| `--config=asan` | Linux | AddressSanitizer (GCC/Clang) |
| `--config=asan-msvc` | Windows | AddressSanitizer (MSVC) |
| `--config=tsan` | Linux | ThreadSanitizer |
| `--config=ubsan` | Linux | UndefinedBehaviorSanitizer |

> TSan and UBSan are not supported by MSVC.

### Optional Build Flags

| Flag | Default | Description |
|------|---------|-------------|
| `--define=ATLAS_DB_MYSQL=1` | `0` | Enable MySQL database backend |
| `--define=ATLAS_USE_IOURING=1` | `0` | Enable io_uring on Linux |

### Build Outputs

Build artifacts are placed in `bazel-bin/` (symlinked by Bazel):

```
bazel-bin/src/server/
├── machined/machined         # Machine daemon (start first)
├── loginapp/atlas_loginapp   # Login gateway
├── baseappmgr/atlas_baseappmgr
├── baseapp/atlas_baseapp
├── cellappmgr/atlas_cellappmgr
├── dbappmgr/atlas_dbappmgr
├── dbapp/atlas_dbapp
└── EchoApp/atlas_echoapp    # Minimal verification app
```

On Windows binaries have `.exe` extension.

## Testing

### Run All Unit Tests

```bash
# All unit tests
bazel test //tests/unit:all

# All unit tests with verbose output
bazel test //tests/unit:all --test_output=all

# Integration tests
bazel test //tests/integration:all
```

### Run a Single Test

```bash
# Run a specific test target
bazel test //tests/unit:test_math

# Run with verbose output
bazel test //tests/unit:test_server_app --test_output=all
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
bazel run //src/server/EchoApp:atlas_echoapp
```

### Full Cluster (development)

Open a separate terminal for each process and start in order:

```bash
# Terminal 1 — service discovery daemon
bazel run //src/server/machined:machined

# Terminal 2 — DBApp manager
bazel run //src/server/dbappmgr:atlas_dbappmgr

# Terminal 3 — BaseApp manager
bazel run //src/server/baseappmgr:atlas_baseappmgr

# Terminal 4 — CellApp manager
bazel run //src/server/cellappmgr:atlas_cellappmgr

# Terminal 5 — database process (XML fallback by default; switch to SQLite via config/CLI)
bazel run //src/server/dbapp:atlas_dbapp

# Terminal 6 — base entity process
bazel run //src/server/baseapp:atlas_baseapp

# Terminal 7 — login gateway (start last; begins accepting client connections)
bazel run //src/server/loginapp:atlas_loginapp
```

Alternatively, run binaries directly from `bazel-bin/` after building.

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
├── BUILD.bazel             Root build file and feature config_settings
├── MODULE.bazel            Bazel module and dependency declarations
├── .bazelrc                Compiler flags and build configurations
├── deps.bzl                Non-registry third-party dependencies
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
│   │   ├── db_sqlite/        SQLite backend (runtime-loaded sqlite3)
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
│   └── tools/              Developer CLI tools
│       └── atlas_tool/
├── tests/
│   ├── unit/               C++ unit tests (Google Test, 60+ test files)
│   ├── integration/        Integration test stubs
│   └── csharp/             C# smoke tests
├── tools/                  Operations tools (cluster_control, db_tools, monitoring)
└── docker/                 Container deployment
```

## License

This project is licensed under the [MIT License](LICENSE).
