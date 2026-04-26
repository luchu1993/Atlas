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
- **Naming**: `PascalCase` functions (including accessors/mutators), `snake_case` variables, `kPascalCase` enum values/constants, `snake_case_` class members
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

## Performance Profiling

### Baseline workflow

Always rebuild profile-release before running a baseline — stale binaries produce
meaningless Tracy data:

```bash
cmake --build build/profile-release --config RelWithDebInfo
```

Run the baseline stress test (100 clients, 120 s, spread-radius 200 m, Tracy capture):

```bash
# Linux / Git Bash
bash tools/cluster_control/run_baseline_profile.sh
```

```powershell
# Windows PowerShell
.\tools\cluster_control\run_baseline_profile.ps1

# Custom parameters
.\tools\cluster_control\run_baseline_profile.ps1 -Clients 50 -DurationSec 60
```

Tracy captures land in `.tmp/prof/baseline/` named
`<process>_<git-short>_<timestamp>.tracy`.

### Comparing captures

Use `tools/profile/compare_tracy.py` to diff two cellapp captures:

```bash
python tools/profile/compare_tracy.py \
    .tmp/prof/baseline/cellapp_<old>.tracy \
    .tmp/prof/baseline/cellapp_<new>.tracy
```

The script calls `tracy-csvexport` automatically (looked up from
`bin/profile-release/tools/`), exports aggregate zone stats to CSV, and prints
a Markdown table of mean / p95 / p99 / max for the key CellApp zones with
regression flags (default threshold: 10 %).

Options:
- `--threshold N` — change regression threshold (default 10 %)
- `--csvexport <path>` — override csvexport binary location
- `--keep-csv` — save the intermediate CSVs to `tracy_csv_export/`

### Key zones to watch (CellApp)

| Zone | Notes |
|---|---|
| `Tick` | Overall tick wall time; budget = 100 ms at 10 Hz |
| `CellApp::TickWitnesses` | Dominant cost; should be < 80 % of Tick mean |
| `Witness::Update::Pump` | Per-observer serialization loop |
| `Witness::SendEntityUpdate` | Per-peer delta/snapshot send |
| `Script.OnTick` | Total C# script time per tick |
| `Script.EntityTickAll` | Entity `OnTick` dispatch |
| `Script.PublishReplicationAll` | Replication frame pump; watch for GC spikes |

### Validating a capture is healthy

A healthy cellapp capture is **≥ 50 MB** for a 120 s run. A file smaller than
a few MB means tracy-capture disconnected early — usually because the cellapp
was overloaded (check the log for `Slow tick` warnings) or the profile-release
binary was not rebuilt after instrumentation changes.
