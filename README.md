# Atlas Engine

A modern distributed MMO game server framework written in **C++20** with **Python 3** scripting, inspired by the **BigWorld Engine** architecture. Features multi-process distributed design with load balancing, spatial partitioning, and fault tolerance, supporting **Windows** and **Linux** cross-platform deployment.

**[中文文档](README_CN.md)**

## Features

- **Distributed Multi-Process Architecture** — LoginApp, BaseApp, CellApp, DBApp and more, with load balancing and fault tolerance
- **Entity System** — Entities distributed across Base / Cell / Client, communicating via Mailbox RPC
- **Python 3 Scripting** — Unified Python scripting for both server and client game logic
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

## Building

### Requirements

- CMake 3.20+
- C++20 compiler: MSVC 2022+ / GCC 10+ / Clang 12+
- Python 3.10+ (scripting layer, enabled in later phases)

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
├── src/
│   ├── lib/                Core libraries
│   │   ├── platform/         OS abstraction layer
│   │   ├── foundation/       Core utilities (logging, memory, containers, time)
│   │   ├── network/          Networking
│   │   ├── serialization/    Serialization
│   │   ├── script/           Script abstraction layer
│   │   ├── pyscript/         Python 3 integration
│   │   ├── entitydef/        Entity definition system
│   │   ├── connection/       Communication protocols
│   │   ├── db/               Database abstraction
│   │   ├── server/           Server framework base classes
│   │   └── ...
│   ├── server/             Server applications
│   │   ├── loginapp/
│   │   ├── baseapp/
│   │   ├── cellapp/
│   │   ├── dbapp/
│   │   └── ...
│   └── client_sdk/         Client connection SDK
├── tests/                  Tests
├── scripts/                Python game scripts
├── tools/                  Operations tools (Python)
└── docker/                 Container deployment
```

## License

This project is licensed under the [MIT License](LICENSE).
