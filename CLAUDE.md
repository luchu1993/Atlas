# CLAUDE.md

This file provides guidance to Claude Code when working with the Atlas Engine codebase.

## What is Atlas Engine?

Atlas is a modern distributed MMO game server framework written in C++20 with C# (.NET 9) scripting. It is inspired by the BigWorld Engine architecture, featuring a distributed multi-process server design with load balancing, scalability, and fault tolerance. The scripting layer embeds CoreCLR via hostfxr and uses [UnmanagedCallersOnly] for zero-overhead C++ ↔ C# interop.

## Build System (Bazel)

### Build

```bash
# Build everything
bazel build //...

# Build with a specific config
bazel build //... --config=release
bazel build //... --config=debug
```

### Run Tests

```bash
# All unit tests
bazel test //tests/unit:all

# Specific test
bazel test //tests/unit:test_hello

# Integration tests
bazel test //tests/integration:all

# All tests with output
bazel test //tests/unit:all --test_output=all
```

### Build Configurations (via .bazelrc)

- **Default (Debug)** — full debug symbols, assertions enabled
- **`--config=release`** — fully optimized, `NDEBUG` defined
- **`--config=debug`** — explicit debug mode, `ATLAS_DEBUG=1`
- **`--config=hybrid`** — optimized with debug symbols

### Sanitizers

- `--config=asan` — AddressSanitizer (Linux / GCC / Clang)
- `--config=asan-msvc` — AddressSanitizer (Windows / MSVC)
- `--config=tsan` — ThreadSanitizer (Linux only)
- `--config=ubsan` — UndefinedBehaviorSanitizer (Linux only)

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
bazel test //tests/unit:all

# Run specific test
bazel test //tests/unit:test_hello

# Run with verbose output
bazel test //tests/unit:test_hello --test_output=all
```

## Pre-Commit Requirements

Before every commit, ensure the following two checks pass:

**1. All unit tests pass**

```bash
bazel test //tests/unit:all --test_output=errors
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
