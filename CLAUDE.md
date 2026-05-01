# CLAUDE.md

Guidance for Claude Code when working in the Atlas Engine repository.

## What is Atlas

Distributed MMO server framework: C++20 core + C# (.NET 9) gameplay scripting via embedded CoreCLR. Multi-process layout (machined / login / base / baseappmgr / cell / cellappmgr / db / dbappmgr / reviver / EchoApp). Inspired by BigWorld; Mailbox-based RPC; spatial partition via BSP tree; Tracy-instrumented for performance work.

User-facing docs: `README.md` (English), `README_CN.md` (中文). Design docs under `docs/`.

## Build

Use the helper instead of raw cmake whenever possible — it loads MSVC env on Windows and provisions Ninja on demand:

```bash
tools\build.bat <preset> [--clean] [--config-only] [--build-only]   # Windows
tools/build.sh   <preset> [--clean] [--config-only] [--build-only]  # Linux / macOS
```

Presets: `debug` (Ninja Multi-Config + `/Z7` + PCH, fast iteration), `release` (no tests), `profile` (Tracy + viewer, no tests), `hybrid` (RelWithDebInfo). Sanitizer presets: `asan`, `asan-msvc`, `tsan` (Linux), `ubsan` (Linux).

Direct cmake also works:

```bash
cmake --preset debug
cmake --build build/debug --config Debug
```

The `debug` preset uses Ninja Multi-Config — `tools/build.bat` handles Ninja + MSVC env automatically; raw cmake requires both pre-arranged.

## Testing

```bash
cd build/debug
ctest --build-config Debug --label-regex unit       --output-on-failure
ctest --build-config Debug --label-regex integration --output-on-failure
ctest --build-config Debug -R test_<name>           --output-on-failure   # specific test
```

C# tests: `dotnet test tests/csharp`.

## Pre-commit checks

Both must pass before committing:

```bash
# Unit tests
cd build/debug && ctest --build-config Debug --label-regex unit --output-on-failure

# clang-format (CI uses 19.1.5; VS-bundled binary is fine locally)
clang-format --dry-run --Werror <changed files>
clang-format -i <changed files>          # fix in place
```

Windows clang-format path: `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-format.exe`.

## Repository layout

| Path | Contents |
|---|---|
| `src/lib/` | Core libs: `platform`, `foundation` (PCH source `atlas_common.h`), `network`, `serialization`, `math`, `physics`, `resmgr`, `coro`, `script`, `clrscript`, `entitydef`, `connection`, `space`, `db`, `db_mysql`, `db_sqlite`, `db_xml`, `server` |
| `src/server/` | Processes: `machined`, `loginapp`, `baseappmgr`, `baseapp`, `cellappmgr`, `cellapp`, `dbapp`, `dbappmgr` (placeholder), `reviver` (placeholder), `EchoApp` |
| `src/csharp/` | C# libs: `Atlas.Shared`, `Atlas.Runtime`, `Atlas.Client`, `Atlas.Client.Desktop`, `Atlas.Client.Unity`, `Atlas.ClrHost`, `Atlas.Generators.{Def,Events}`, `Atlas.Tools.DefDump` |
| `src/client/` | Console / desktop client app |
| `src/tools/` | `atlas_tool`, `login_stress`, `world_stress`, `crash_demo` |
| `samples/` | Sample game scripts: `base/`, `client/`, `stress/` |
| `tests/` | `unit/` (Google Test), `integration/` (Google Test), `csharp/` |
| `tools/` | `build.{py,bat,sh}`, `cluster_control/`, `profile/` |
| `docs/` | Design docs: `roadmap/`, `scripting/`, `gameplay/`, `rpc/`, `optimization/`, `property_sync/`, `coro/`, `generator/`, `client/`, `operations/`, `bigworld_ref/`, `stress_test/`, `CODING_STYLE.md` |
| `bin/<preset>/` | Flat output dir for all EXE / DLL / .lib (configured by `cmake/AtlasOutputDirectory.cmake`) |

## Server framework hierarchy (`src/lib/server/`)

```
ServerApp
├── ManagerApp     — manager / daemon (no scripting): BaseAppMgr, CellAppMgr, DBAppMgr, machined, EchoApp
└── ScriptApp      — ServerApp + CoreCLR
    └── EntityApp  — ScriptApp + entity defs + bg-task pool: BaseApp, CellApp
```

Provided by `ServerApp`: `ServerConfig` (CLI + JSON), `MachinedClient` (registration / heartbeat / Birth-Death), `WatcherRegistry` (path-based metrics), `Updatable`/`Updatables` (level-ordered tick callbacks), `SignalDispatchTask`.

## Code conventions

Follows the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html); see `docs/CODING_STYLE.md` for project-specific overrides.

- C++20, no compiler extensions
- 2-space indent, attached braces, 100-col limit (enforced by `.clang-format`)
- `PascalCase` functions (incl. accessors/mutators); `snake_case` variables; `kPascalCase` enum values / constants; `snake_case_` class members
- `snake_case` stays for STL interface (`begin`, `end`, `size`, …) and coroutine protocol (`await_ready`, `initial_suspend`, …)
- Platform-specific files: `_windows.cc` / `_linux.cc`
- Namespace: `atlas::`
- String formatting: `std::format`
- Error handling: `std::expected` or custom `Result<T,E>` — no exceptions
- Smart pointers: `std::unique_ptr`, `std::shared_ptr`, custom `IntrusivePtr<T>`
- All new code needs unit tests (Google Test)

### Comments

Comments are a debt — pay them only when the code itself can't carry the meaning. Write in English; explain *why*, not *what*; never embed task / phase / ticket numbers (`P0.1`, `Phase 12`, `#1234`) — they rot the moment scope shifts.

1. **Default to no comment.** A comment must justify its existence: hidden invariant, surprising trade-off, workaround for a specific bug, contract a reader can't infer from the signature. If removing the comment wouldn't confuse a future reader, delete it.
2. **Every comment block ≤ 2 lines, in every source file** — `.h`, `.cc`, `.cs`, `.cmake`, `CMakeLists.txt`, build scripts, test files. No paragraph blocks, no section banners (`// ====`, `// ----`, `// ## Section`), no file-level narrative comments, no class-header bios, no historical narratives, no XML-doc paragraphs. Implementation files should read top-to-bottom as plain code; headers and structs should explain *why* a single line at a time. If a thought needs more than two lines to explain, the design doc is the right place — link it instead.
3. **Self-documenting code beats explanatory comments.** A long comment masking a tangled function is a refactor request, not documentation. Rename the variable, extract the helper, split the branch — and the comment goes away on its own.
4. **Clean up violations on contact.** When you read or edit a source file whose comments break these rules — restating the obvious, narrating history, padding with banners, repeating the function name — refactor them out as part of the same change. Don't leave stale prose behind for someone else.

### Refactoring

Refactoring is a first-class part of every change, not a separate "cleanup PR" that never lands.

1. **Refactor proactively.** When a change touches code that has decayed — overly long function, tangled branching, unclear ownership, name that no longer fits — fix it as part of the same commit. Do not file the cleanup as a TODO. The further refactors get from the original change, the less likely they are to happen.
2. **Delete dead code and config promptly.** When a feature is removed, replaced, or made obsolete, remove its code paths, dead branches, unused fields, stale CMake options, abandoned config keys, and orphaned tests in the *same* change. Do not leave `_unused` shims, `// removed` comment markers, deprecated wrappers, or feature flags pointing at empty implementations. If the old API still has callers, migrate them first.
3. **No band-aid / patch-style fixes.** When the old design has a problem, think through the proper fix and apply it. Symptom-suppressing workarounds are forbidden: extra layers wrapping the broken layer, special cases sprinkled at call sites, "just in case" defensive checks that hide the underlying contract violation, comments saying "FIXME: works around X". If the proper fix is too large for the current change, stop, describe the situation, and discuss scope before writing code.
4. **New work surfaces old problems — fix them in place.** While implementing a new feature, if you discover the existing feature it depends on or conflicts with is broken / misdesigned, do not route around it. Stop and think: should the new feature change shape, or should the old one? Then refactor whichever is wrong. Building a new feature on top of broken foundations multiplies the debt instead of paying it down.

The bias is: when in doubt, refactor. Half-measures rot fast. Either commit to the proper fix or push back on scope before code lands.

### Documentation

Design docs under `docs/` carry the same debt as comments — keep them lean and current, or delete them. A stale doc is worse than no doc: it actively misleads.

1. **Prune obsolete docs promptly.** When a design is superseded, rejected, or replaced, delete the document in the same change that retires the feature. Do not leave outdated design notes around — readers will mistake them for current intent. Half a dozen `_old`, `_v1`, `_deprecated` files is a smell; commit to one source of truth.
2. **Update doc status when the task lands.** When a piece of work completes, update the corresponding doc's status in the same commit (e.g., `Proposed` → `Implemented`, `Planned` → `Done`, or delete the planning doc outright if it has no lasting value). Never leave docs claiming "in progress" or "planned" for work that already shipped.
3. **Strip implementation detail after landing.** Once a feature ships, the doc's job is to explain *what exists and why*, not to relive the implementation journey. Remove step-by-step plans, phase breakdowns, abandoned alternatives, intermediate TODO checklists, and migration notes that no longer apply. The code is the source of truth for *how*; the doc is the source of truth for *what / why*.
4. **Review the whole doc on contact.** When editing any part of a doc, scan the rest for violations of these rules — stale status, lingering implementation steps, references to deleted code or removed APIs, broken cross-links — and fix them in the same change. Don't leave rot for someone else.

### Tool scripts

Developer-facing tools (build helpers, cluster launchers, dev-loop setup) follow a fixed shape so the same name works on every platform.

1. **Logic in Python.** The actual work lives in `<tool>/name.py` — stdlib only (`argparse`, `pathlib`, `subprocess`). Third-party deps creep into CI install lists, so reach for them only when the stdlib path is genuinely worse. The `.py` is grouped by topic under `tools/` (`tools/build.py`, `tools/cluster_control/run_world_stress.py`, etc.).
2. **All wrappers in `tools/bin/`.** Every platform wrapper — `name.bat`, `name.sh`, the preset-variant `.ps1` — lives in `tools/bin/`, never next to the `.py`. One directory to add to PATH, one place a user looks for "what tools does this repo have". Each wrapper is a one-liner that locates Python and invokes the matching `.py` via a relative path (`"%~dp0..\cluster_control\run_world_stress.py"` / `"$(dirname "$0")/../cluster_control/run_world_stress.py"`).
3. **Wrapper name = `.py` name.** `tools/bin/build.{bat,sh}` invokes `tools/build.py`; `tools/bin/run_world_stress.{bat,sh}` invokes `tools/cluster_control/run_world_stress.py`. Drift between the two breaks user expectations; rename both at the same time.
4. **Share via `tools/common/`.** Repeated helpers (repo-root resolution, MSVC env loading, port reservation, subprocess runners, structured log printers) belong in `tools/common/` as a proper Python package, not copy-pasted across scripts. Each script's `.py` is the *glue* that wires arg parsing to `common/` calls; the *work* lives in `common/`. New shared logic enters `common/` on its second use, not its first.
5. **Treat `tools/` as one Python project.** Cohesion inside each `common/` module (one job per file), reuse across scripts, and a stable internal API are first-class concerns. Same for UX: every tool ships meaningful `--help`, `-h` shortcut, sensible defaults, idempotent re-runs, and error messages that name the missing thing or the bad arg with a fix suggestion. A tool that needs a wiki page to operate has failed.

Variant: a *preset wrapper* (a thin shortcut into another tool's Python) ships `tools/bin/name.{ps1,sh}` without its own `.py` — `tools/bin/run_baseline_profile.{ps1,sh}` and `tools/bin/run_cluster.{ps1,sh}` are the established examples. PowerShell here is fine for typed parameters; bash mirrors it for non-Windows hosts. Don't introduce this variant when a fresh tool would do, but keep the door open for "shortcut into existing driver."

## CI workflows (`.github/workflows/`)

- `cmake.yml` — Windows + Linux × Debug + Release. Uses sccache, NuGet + `_deps` caching, paths-ignore for docs / tooling. Runs unit + integration tests on Debug.
- `clang-format.yml` — clang-format 19.1.5 dry-run on `.cc/.cpp/.h/.hpp` changes only.
- `sanitizers.yml` — `workflow_dispatch` + weekly schedule. asan + ubsan on Linux. Off the PR critical path; tsan deferred (atlas heap pool produces noisy false positives).

Concurrency cancellation is enabled on every workflow.

## Performance profiling

Tracy-instrumented; `profile` preset includes the viewer + CLI helpers in `bin/profile/`.

```bash
# Build profile (needs to be re-built before every baseline — stale binaries lie)
tools\build.bat profile

# Stress baseline (200 clients, 120 s by default; override with -Clients / -DurationSec)
.\tools\cluster_control\run_baseline_profile.ps1     # Windows
bash tools/cluster_control/run_baseline_profile.sh   # Linux / Git Bash

# Compare two cellapp captures (mean / p95 / p99 / max with regression flags)
python tools/profile/compare_tracy.py \
    .tmp/prof/baseline/cellapp_<old>.tracy \
    .tmp/prof/baseline/cellapp_<new>.tracy
```

Captures land in `.tmp/prof/baseline/` named `<process>_<git-short>_<timestamp>.tracy`. A healthy cellapp capture for 120 s is ≥ 50 MB; smaller usually means tracy-capture disconnected early (overload — check log for `Slow tick`).

### Memory profiling (callstack-attributed allocations)

Allocator hooks in `src/lib/foundation/profiler.h` use `TracyAllocS` / `TracyFreeS` (depth 16). In Tracy's Memory view: **Active allocations**, **Top callstack tree**, **Allocations**. Stack capture costs ~1–3 µs/alloc on Windows and inflates traces ~3–5×; downgrade to `TracyAlloc` / `TracyFree` for long captures if needed.

### Key cellapp Tracy zones

| Zone | Notes |
|---|---|
| `Tick` | Overall tick wall time; budget = 100 ms at 10 Hz |
| `CellApp::TickWitnesses` | Dominant cost; should be < 80 % of `Tick` mean |
| `Witness::Update::Pump` | Per-observer serialization loop |
| `Witness::SendEntityUpdate` | Per-peer delta/snapshot send |
| `Script.OnTick` | Total C# script time per tick |
| `Script.EntityTickAll` | Entity `OnTick` dispatch |
| `Script.PublishReplicationAll` | Replication frame pump; watch for GC spikes |
