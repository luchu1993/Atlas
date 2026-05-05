# Repository Guidelines

## Project Structure & Module Organization
Atlas is a distributed MMO server framework. `src/server/` contains processes such as `machined`, `loginapp`, `baseapp`, `cellapp`, `dbapp`, their manager apps, and `reviver`. Shared C++20 code lives in `src/lib/` modules such as `platform`, `foundation`, `network`, `serialization`, `script`, `entitydef`, `db_*`, and `server`. `src/client_sdk/` contains the client SDK, `src/csharp/` the .NET scripting/runtime layer, and `src/tools/` developer tools. Tests live in `tests/unit/`, `tests/integration/`, and `tests/csharp/`; runtime assets and configs are under `data/` and `runtime/`.

## Build, Test, and Development Commands
Use CMake presets from the repository root.

```powershell
cmake --preset debug-windows
cmake --build --preset debug-windows
ctest --preset debug-windows
dotnet test tests/csharp
```

Cross-platform CI also uses Ninja presets:

```bash
cmake --preset debug-ninja
cmake --build --preset debug-ninja
ctest --preset debug-ninja -L unit
ctest --preset debug-ninja -L integration
```

Use `Debug` for day-to-day work, `Release` for optimized builds, and `RelWithDebInfo` when you need symbols with optimization. Set `-DATLAS_BUILD_TESTS=OFF` or similar only when you intentionally want a reduced build.

## Coding Style & Naming Conventions
Formatting is enforced by `.clang-format`: 4-space indentation, no tabs, 100-column limit, Allman braces, and left-aligned pointers. Follow `docs/CODING_STYLE.md`: `PascalCase` for types, `snake_case` for functions, locals, namespaces, and file names, `snake_case_` for members, and `kPascalCase` for constants. Use the `atlas::` namespace, keep headers beside sources under `src/lib/<module>/`, and name platform-specific files with `_windows.cpp` or `_linux.cpp`.

Prefer `std::format` for formatting, `Result<T, E>` / `std::expected`-style returns for recoverable errors, and RAII with `std::unique_ptr`, `std::shared_ptr`, or `IntrusivePtr<T>`. Avoid exceptions in core framework code unless guarding third-party boundaries.

Run format checks before pushing:

```bash
clang-format --dry-run --Werror <changed files>
```

## Testing Guidelines
Add or update tests with every behavior change. Name C++ tests `test_<feature>.cpp` and keep them in the matching suite under `tests/unit/` or `tests/integration/`. Prefer targeted runs while iterating, then finish with the relevant `ctest` preset. For scripting/runtime work, add coverage in `tests/csharp/*Tests.cs` and run `dotnet test tests/csharp`. Do not commit unless the relevant tests pass and `clang-format --dry-run --Werror` is clean.

## Commit & Pull Request Guidelines
Recent history uses short conventional subjects such as `network: drain buffered frames` and `docs: add login stress testing guide`. Keep commit messages imperative and scoped: `<area>: <change>`. PRs should summarize the behavioral change, list the commands you ran, and link the relevant issue or roadmap doc. Include logs or protocol notes for distributed/server changes; attach screenshots only when UI or tooling output meaningfully changed.
