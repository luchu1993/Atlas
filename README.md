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

## Building

### Requirements

- CMake 3.20+
- C++20 compiler: MSVC 2022+ / GCC 10+ / Clang 12+
- .NET 9 SDK (scripting layer)

### Compile

```bash
# Windows (Visual Studio)
cmake --preset debug-windows
cmake --build build/debug-windows --config Debug

# Cross-platform (Ninja)
cmake --preset debug-ninja
cmake --build build/debug-ninja

# Manual
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
```

### Test

```bash
ctest --test-dir build --build-config Debug --output-on-failure
```

## Project Structure

```
atlas/
├── cmake/                  CMake modules
├── runtime/                .NET runtime configuration
├── src/
│   ├── lib/                Core libraries
│   │   ├── platform/         OS abstraction layer
│   │   ├── foundation/       Core utilities (logging, memory, containers, time)
│   │   ├── network/          Networking
│   │   ├── serialization/    Serialization
│   │   ├── script/           Script abstraction layer (ScriptEngine / ScriptValue)
│   │   ├── clrscript/        .NET 9 CoreCLR embedding (ClrHost)
│   │   ├── entitydef/        Entity definition system
│   │   ├── connection/       Communication protocols
│   │   ├── db/               Database abstraction (IDatabase + DatabaseFactory)
│   │   ├── db_mysql/         MySQL backend
│   │   ├── db_xml/           XML backend
│   │   ├── server/           Server framework base classes
│   │   └── ...
│   ├── server/             Server applications
│   │   ├── loginapp/
│   │   ├── baseapp/
│   │   ├── baseappmgr/
│   │   ├── dbapp/
│   │   ├── machined/
│   │   ├── EchoApp/          Minimal verification app
│   │   └── ...
│   └── client_sdk/         Client connection SDK
├── src/tools/              Developer tools
│   └── atlas_tool/
├── tests/
│   ├── unit/               C++ unit tests (Google Test)
│   └── csharp/             C# smoke tests
├── tools/                  Operations tools
└── docker/                 Container deployment
```

## License

This project is licensed under the [MIT License](LICENSE).
