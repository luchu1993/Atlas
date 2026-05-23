# CLAUDE.md

Rules for Claude Code when working in the Atlas Engine repository.

## Operating rules

- Prefer repository wrappers over raw tool invocations when building or running local workflows; the wrappers carry platform-specific environment setup.
- Treat Windows as a first-class environment: use `.bat` wrappers for Windows-facing instructions and keep matching `.sh` wrappers for Linux / macOS when adding tools.
- Before reporting a build, test, Unity, or tooling workflow as working, run the smallest command that validates the changed surface.
- After every edit to code, docs, configs, or tool scripts, re-read the touched files against the rules below and fix any violation (stray comments, missing sibling-doc updates, fat wrappers, etc.) before reporting the task done or committing.
- Do not commit generated build outputs, Unity staged DLLs, profiling captures, logs, local databases, or temporary cluster state.
- Do not skip hooks, tests, formatting, or signing checks unless the user explicitly asks for that bypass.

## Design rules

- When designing a new mechanism or solving a cross-process problem, prefer the BigWorld-canonical approach (per-entity migration epochs, cell ghost regions, primary cell discovery, etc.) over an ad-hoc invention.
- Diverge from BigWorld only when the project has a documented reason (different threading model, no script-side state, etc.); call out the divergence explicitly in the change description.

## Code rules

- Follow Google C++ style plus project overrides: C++20, 2-space indent, attached braces, 100-column limit, no compiler extensions, namespace `atlas::`.
- Use `PascalCase` for functions, `snake_case` for variables, `kPascalCase` for enum values/constants, and trailing underscore for class members.
- Preserve STL and coroutine protocol spelling (`begin`, `end`, `await_ready`, `initial_suspend`, etc.).
- Use `std::format` for formatting and `std::expected` or project `Result<T,E>` for recoverable errors; do not introduce exceptions.
- Prefer existing ownership types and patterns: `std::unique_ptr`, `std::shared_ptr`, and project `IntrusivePtr<T>`.
- Put platform-specific C++ implementations in `_windows.cc` / `_linux.cc` files.
- New behavior needs tests at the appropriate level unless the user explicitly scopes the task to docs or tooling-only changes.

## Comment rules

- Default to no comments; add one only for a hidden invariant, non-obvious trade-off, workaround, or contract the signature cannot express.
- Keep every comment block to at most two lines in every source, build, script, and test file.
- Do not add section banners, historical notes, task IDs, phase labels, file-level narratives, XML-doc paragraphs, or comments that restate code.
- Clean up nearby stale or rule-breaking comments when editing a file.

## Test rules

- Do not skip, disable, mark `DISABLED_`, or comment out tests to make a red build green; first investigate whether your change broke a real contract.
- If careful review concludes a test itself is wrong (stale assertion, drifted expectation, racy setup), fix the test in the same change and explain why in the commit message — never silently weaken it.
- Treat new flakiness as a failure: stabilize the test or the code, do not retry-loop or quarantine without an owner.

## Refactoring rules

- Refactor touched code when the existing shape blocks a clean change; do not layer a workaround over a broken design.
- Delete dead code, obsolete config, unused fields, stale CMake options, abandoned feature flags, and orphaned tests in the same change that makes them obsolete.
- Do not leave `_unused` shims, deprecated wrappers, empty compatibility paths, or comments marking removed code.
- If the proper fix is larger than the requested scope, stop and discuss scope instead of landing a patch-style workaround.

## Documentation rules

- Keep docs lean, current, and focused on what exists and why; remove implementation diaries, phase plans, TODO checklists, and abandoned alternatives after work lands.
- When changing a user-facing workflow, update both English and Chinese docs in the same change.
- When touching MVP Unity tooling or behavior, keep the root READMEs and `samples/mvp` READMEs aligned.
- Delete obsolete design docs when a design is superseded; do not leave `_old`, `_v1`, or deprecated copies as parallel sources of truth.
- Do not add standalone file inventories such as “Files touched”, “modules changed”, or code-path catalogs.

## Tool script rules

- Put real tool logic in Python under `tools/`; wrappers in `tools/bin/` should stay thin and have matching names.
- Use only Python stdlib for developer tooling unless the stdlib path is genuinely worse and the dependency cost is justified.
- Share repeated Python helper logic through `tools/common/` on second use rather than copying it across scripts.
- Every developer-facing tool must have useful `--help`, sensible defaults, idempotent re-runs, and errors that name the missing thing or bad argument with a fix.
- For Unity MVP tools, preserve explicit `--unity` / environment-variable overrides before any auto-discovery fallback.
