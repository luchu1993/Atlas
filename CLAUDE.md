# CLAUDE.md

This file provides guidance to Claude Code when working with the Atlas Engine codebase.

## What is Atlas Engine?

Atlas is a modern distributed MMO game server framework written in C++20 with C# (.NET 9) scripting. It is inspired by the BigWorld Engine architecture, featuring a distributed multi-process server design with load balancing, scalability, and fault tolerance. The scripting layer embeds CoreCLR via hostfxr and uses [UnmanagedCallersOnly] for zero-overhead C++ ↔ C# interop.

## Build System (CMake)

### Build

```bash
# Configure (debug)
cmake --preset debug

# Build
cmake --build build/debug --config Debug

# Configure + build (release)
cmake --preset release
cmake --build build/release --config Release
```

### Run Tests

```bash
# All tests
cd build/debug && ctest --build-config Debug

# Unit tests only
ctest --build-config Debug --label-regex unit

# Integration tests only
ctest --build-config Debug --label-regex integration

# Specific test
ctest --build-config Debug -R test_hello

# Verbose output
ctest --build-config Debug --output-on-failure
```

### Build Presets (via CMakePresets.json)

- **`debug`** — full debug symbols, assertions enabled, `ATLAS_DEBUG=1`
- **`release`** — fully optimized, `NDEBUG` defined
- **`hybrid`** — optimized with debug symbols (RelWithDebInfo)

### Sanitizers

- `--preset asan` — AddressSanitizer (Linux / GCC / Clang)
- `--preset asan-msvc` — AddressSanitizer (Windows / MSVC)
- `--preset tsan` — ThreadSanitizer (Linux only)
- `--preset ubsan` — UndefinedBehaviorSanitizer (Linux only)

### CMake Options

- `ATLAS_BUILD_TESTS` — Build tests (default: ON)
- `ATLAS_BUILD_CSHARP` — Build C# projects via dotnet (default: ON)
- `ATLAS_DB_MYSQL` — Enable MySQL database backend (default: OFF)
- `ATLAS_USE_IOURING` — Enable io_uring I/O poller, Linux only (default: OFF)

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
- **script** — language-agnostic scripting abstraction (ScriptEngine / ScriptValue / ScriptObject)
- **entitydef** — entity type definitions, data types, mailbox
- **connection** — client-server protocol definitions
- **db / db_mysql / db_xml** — database abstraction and backends
- **server** — server framework base classes
- **math** — vectors, matrices, quaternions

### Client SDK (`src/client_sdk/`)

Lightweight SDK for game clients to connect to Atlas servers.

## Code Conventions

Follows the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html).
See `docs/CODING_STYLE.md` for the full project style guide.

### Key rules

- C++20 standard, no compiler extensions
- **Formatting**: 2-space indent, attached braces, 100-column limit (enforced by `.clang-format`)
- **Naming**: `PascalCase` functions, `snake_case` variables, `kPascalCase` enum values/constants, `snake_case_` class members
- Accessors matching member names stay `snake_case`: `port()` for `port_`, `set_port()` for setter
- Coroutine protocol functions (`await_ready`, `initial_suspend`, etc.) and STL interface functions (`begin`, `end`, `size`, etc.) stay `snake_case`
- Platform-specific code uses suffix: `_windows.cc`, `_linux.cc`
- Namespace: `atlas::`
- Use `std::format` for string formatting
- Use `std::expected` or custom `Result<T,E>` for error handling (no exceptions)
- Smart pointers: `std::unique_ptr`, `std::shared_ptr`, and custom `IntrusivePtr<T>`
- All new code should have unit tests (Google Test)
- In the absence of explicit style guidelines, follow Google C++ Style Guide

## Testing

Unit tests use Google Test. Each library's tests are in `tests/unit/`.

```bash
# Run all unit tests
cd build/debug && ctest --build-config Debug --label-regex unit

# Run specific test
ctest --build-config Debug -R test_hello

# Run with verbose output
ctest --build-config Debug -R test_hello --output-on-failure
```

## Pre-Commit Requirements

Before every commit, ensure the following two checks pass:

**1. All unit tests pass**

```bash
cd build/debug && ctest --build-config Debug --label-regex unit --output-on-failure
```

**2. clang-format has no violations**

```bash
# Check (dry-run)
clang-format --dry-run --Werror <changed files>

# Fix in place
clang-format -i <changed files>
```

On Windows, clang-format is at:
`C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-format.exe`

Do not commit if either check fails. Fix the issues first, then re-stage and commit.
