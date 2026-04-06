# CLAUDE.md

This file provides guidance to Claude Code when working with the Atlas Engine codebase.

## What is Atlas Engine?

Atlas is a modern distributed MMO game server framework written in C++20 with Python 3 scripting. It is inspired by the BigWorld Engine architecture, featuring a distributed multi-process server design with load balancing, scalability, and fault tolerance.

## Build System (CMake)

### Configure and Build

```bash
# Using presets (recommended)
cmake --preset debug-windows       # Windows / MSVC
cmake --preset debug-ninja         # Cross-platform / Ninja

cmake --build --preset debug-windows
cmake --build --preset debug-ninja

# Manual
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
```

### Run Tests

```bash
ctest --preset debug-windows
# or
ctest --test-dir build --output-on-failure
```

### Build Configurations

- **Debug** — full debug symbols, assertions enabled, `ATLAS_DEBUG=1`
- **Release** — fully optimized, no debug, `NDEBUG` defined
- **RelWithDebInfo** (Hybrid) — optimized with debug symbols

### CMake Options

- `ATLAS_BUILD_TESTS` — build unit tests (default ON)
- `ATLAS_BUILD_SERVER` — build server applications (default ON)
- `ATLAS_BUILD_CLIENT_SDK` — build client SDK (default ON)
- `ATLAS_ENABLE_ASAN` — enable AddressSanitizer (GCC/Clang only)

## Architecture

### Server Components (`src/server/`)

Distributed multi-process architecture:

- **machined** — machine daemon, process registration, service discovery
- **LoginApp** — client authentication and connection
- **BaseApp** — entity behavior, state, client proxy
- **CellApp** — spatial partitioning, entity movement, AoI
- **DBApp** — database persistence (MySQL/XML)
- **BaseAppMgr / CellAppMgr / DBAppMgr** — cluster management
- **Reviver** — crash detection and recovery

### Libraries (`src/lib/`)

- **platform** — OS abstraction (I/O, threading, signals, filesystem)
- **foundation** — core utilities (logging, memory, containers, time)
- **network** — sockets, event dispatcher, channels, messages
- **serialization** — binary streams, XML/JSON parsing
- **script / pyscript** — Python 3 scripting integration
- **entitydef** — entity type definitions, data types, mailbox
- **connection** — client-server protocol definitions
- **db / db_mysql / db_xml** — database abstraction and backends
- **server** — server framework base classes
- **math** — vectors, matrices, quaternions

### Client SDK (`src/client_sdk/`)

Lightweight SDK for game clients to connect to Atlas servers.

## Code Conventions

- C++20 standard, no compiler extensions
- Platform-specific code uses suffix: `_windows.cpp`, `_linux.cpp`
- Namespace: `atlas::`
- Use `std::format` for string formatting
- Use `std::expected` or custom `Result<T,E>` for error handling (no exceptions)
- Smart pointers: `std::unique_ptr`, `std::shared_ptr`, and custom `IntrusivePtr<T>`
- All new code should have unit tests (Google Test)
- Follow `.clang-format` style
- In the absence of explicit style guidelines, mimic patterns in existing code

## Testing

Unit tests use Google Test. Each library's tests are in `tests/unit/`.

```bash
# Run all tests
ctest --test-dir build

# Run specific test
./build/bin/tests/test_hello
```
