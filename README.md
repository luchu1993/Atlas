# Atlas Engine

A modern distributed MMO game server framework written in **C++20** with **C# (.NET 9)** scripting, inspired by the **BigWorld Engine** architecture. Features multi-process distributed design with load balancing, spatial partitioning, and fault tolerance, supporting **Windows** and **Linux** cross-platform deployment.

**[中文文档](README_CN.md)**

## Features

- **Distributed Multi-Process Architecture** — LoginApp, BaseApp, CellApp, DBApp and more, with load balancing and fault tolerance
- **Entity System** — Entities distributed across Base / Cell / Client, communicating via Mailbox RPC
- **C# (.NET 9) Scripting** — High-performance C# scripting via embedded CoreCLR; zero-overhead interop with `[UnmanagedCallersOnly]`
- **Cross-Platform** — Full OS API abstraction, unified build on Windows and Linux
- **Pluggable Database** — MySQL (production) and XML (development) backends
- **Client SDK** — Lightweight connection SDK, not tied to any specific game client engine

## Architecture

```
Client ──► LoginApp ──► BaseAppMgr ──► BaseApp ◄──► CellApp
                                          │              │
                                        DBApp        CellAppMgr
                                          │
                                        MySQL
```

| Process | Role |
|---------|------|
| **LoginApp** | Client authentication and login |
| **BaseApp** | Entity state management, client proxy, persistence |
| **CellApp** | Spatial partitioning, entity movement, AoI (Area of Interest) |
| **DBApp** | Asynchronous database read/write |
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
| [CMake](https://cmake.org/download/) | 3.20+ | Or use the version bundled with Visual Studio |
| [.NET 9 SDK](https://dotnet.microsoft.com/download/dotnet/9.0) | 9.0+ | Required for C# scripting layer |
| [Git](https://git-scm.com/) | Any | Required for FetchContent dependency fetching |

**Visual Studio 2022 workload and components required:**
- Workload: **Desktop development with C++**
- Individual component: **C++ CMake tools for Windows** (optional, VS ships a bundled CMake)

Verify prerequisites after installation:
```bat
cl /?                  :: MSVC compiler — available after opening VS Developer Command Prompt
cmake --version        :: should be 3.20+
dotnet --version       :: should be 9.x.x
git --version
```

#### Linux (Ubuntu 22.04+)

```bash
# Compiler, build tools
sudo apt update
sudo apt install -y build-essential g++-12 cmake ninja-build git

# .NET 9 SDK — https://learn.microsoft.com/dotnet/core/install/linux
wget https://dot.net/v1/dotnet-install.sh
chmod +x dotnet-install.sh
./dotnet-install.sh --channel 9.0
echo 'export PATH="$HOME/.dotnet:$PATH"' >> ~/.bashrc
source ~/.bashrc

# Verify
g++ --version          # should be 12+
cmake --version        # should be 3.20+
dotnet --version       # should be 9.x.x
```

> **Third-party dependencies** (Google Test, pugixml, RapidJSON, zlib) are automatically downloaded by CMake `FetchContent` during the first configure — no manual installation needed. Ensure internet access (or a pre-seeded `_deps` cache) during the first configure.

### Clone

```bash
git clone <repo-url>
cd Atlas
```

## Building

### Configure and Compile

```bash
# Windows — Visual Studio generator (recommended)
cmake --preset debug-windows
cmake --build --preset debug-windows

# Windows — Release
cmake --preset release-windows
cmake --build --preset release-windows

# Linux — GCC / Make
cmake --preset debug-linux
cmake --build --preset debug-linux

# Cross-platform — Ninja (requires ninja installed)
cmake --preset debug-ninja
cmake --build --preset debug-ninja
```

> The first configure will download third-party dependencies (~100 MB). Subsequent configures are instant.

### CMake Options

Pass `-D<option>=ON/OFF` to `cmake --preset ... -D...` to override defaults:

| Option | Default | Description |
|--------|---------|-------------|
| `ATLAS_BUILD_TESTS` | `ON` | Build Google Test unit tests |
| `ATLAS_BUILD_SERVER` | `ON` | Build server processes |
| `ATLAS_BUILD_CLIENT_SDK` | `ON` | Build client SDK |
| `ATLAS_ENABLE_ASAN` | `OFF` | Enable AddressSanitizer (GCC/Clang only) |
| `ATLAS_ENABLE_TSAN` | `OFF` | Enable ThreadSanitizer (GCC/Clang only) |
| `ATLAS_ENABLE_UBSAN` | `OFF` | Enable UndefinedBehaviorSanitizer (GCC/Clang only) |

Example — build without tests:
```bash
cmake --preset debug-linux -DATLAS_BUILD_TESTS=OFF
cmake --build --preset debug-linux
```

### Build Outputs

```
build/debug-windows/bin/Debug/
├── machined.exe          # Machine daemon (start first)
├── atlas_loginapp.exe    # Login gateway
├── atlas_baseappmgr.exe  # BaseApp cluster manager
├── atlas_baseapp.exe     # Base entity process
├── atlas_cellappmgr.exe  # CellApp cluster manager
├── atlas_dbappmgr.exe    # DBApp cluster manager
├── atlas_dbapp.exe       # Database process
├── atlas_echoapp.exe     # Minimal verification app
├── atlas_tool.exe        # Developer CLI tool
├── atlas_engine.dll      # Core shared library
└── zlib.dll              # Compression runtime
```

On Linux: `.exe` → no extension, `.dll` → `.so`.

## Testing

### Run All Unit Tests

```bash
# Windows (Visual Studio preset)
ctest --preset debug-windows

# Windows — direct
ctest --test-dir build/debug-windows --build-config Debug --output-on-failure

# Linux
ctest --preset debug-ninja
# or
ctest --test-dir build/debug-linux --output-on-failure
```

### Run a Single Test Binary

```bash
# Windows
.\build\debug-windows\bin\Debug\test_server_app.exe

# Linux
./build/debug-linux/bin/test_server_app
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

```bat
cd build\debug-windows\bin\Debug
.\atlas_echoapp.exe
```

### Full Cluster (development)

Open a separate terminal for each process. Navigate to the output directory first:

```bat
cd build\debug-windows\bin\Debug
```

Then start in order:

```bat
# Terminal 1 — service discovery daemon
.\machined.exe

# Terminal 2 — DBApp manager
.\atlas_dbappmgr.exe

# Terminal 3 — BaseApp manager
.\atlas_baseappmgr.exe

# Terminal 4 — CellApp manager
.\atlas_cellappmgr.exe

# Terminal 5 — database process (XML dev backend)
.\atlas_dbapp.exe

# Terminal 6 — base entity process
.\atlas_baseapp.exe

# Terminal 7 — login gateway (start last; begins accepting client connections)
.\atlas_loginapp.exe
```

On Linux replace `.\<name>.exe` with `./<name>` and the directory is `build/debug-linux/bin/`.

### Configuration

Each process loads configuration from CLI flags and a JSON config file (via `ServerConfig`). Common options:

| Flag | Description | Example |
|------|-------------|---------|
| `--config <path>` | Path to JSON config file | `--config conf/baseapp.json` |
| `--machined-host <ip>` | Address of the machined daemon | `--machined-host 127.0.0.1` |
| `--port <n>` | Port for this process to listen on | `--port 20010` |

### Database Backend

- **XML backend** (development/testing) — zero configuration, data stored as XML files, enabled by default
- **MySQL backend** (production) — requires a running MySQL instance; configure connection details in the process JSON config

### C# Scripts

Game logic scripts live in the `scripts/` directory and are loaded at runtime via embedded CoreCLR. The .NET runtime configuration is at `runtime/atlas_server.runtimeconfig.json`.

## Project Structure

```
atlas/
├── cmake/                  CMake modules
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
