# Atlas Engine — Coding Style Guide

> Based on the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html).
> Project-specific additions are marked with **(Atlas)**.

---

## 1. Language Standard

- **C++20** is the baseline. Use C++20 features freely: concepts, `constexpr`, `std::format`, `std::jthread`, `std::source_location`, `std::span`, `std::byte`, `[[likely]]`/`[[unlikely]]`, three-way comparison, designated initializers.
- **No exceptions** in framework code. Use `Result<T, E>` for error propagation. Exceptions may only appear in catch blocks that guard third-party library calls. **(Atlas)**
- **No RTTI** (`dynamic_cast`, `typeid`) in core libraries. Prefer concepts and templates for polymorphism where possible.

---

## 2. Formatting

Enforced by `.clang-format` (`BasedOnStyle: Google`). Key rules:

| Rule | Value |
|------|-------|
| Indent | 2 spaces, no tabs |
| Column limit | 100 characters **(Atlas: wider than Google's 80)** |
| Brace style | **Attached** (opening brace on same line) |
| Pointer alignment | Left (`int* ptr`) |
| Namespace indent | None |
| Access modifier offset | -1 (one space less than member indent) |
| Short functions | Inline only |

```cpp
// Correct: Attached braces (Google style)
void Foo() {
  if (condition) {
    Bar();
  }
}

// Wrong: Allman braces
void Foo()
{
    if (condition)
    {
        Bar();
    }
}
```

---

## 3. Naming Conventions

Follows the [Google C++ naming rules](https://google.github.io/styleguide/cppguide.html#Naming).

| Element | Style | Example |
|---------|-------|---------|
| Namespaces | `snake_case` | `atlas`, `atlas::math`, `atlas::string_utils` |
| Types (class/struct/enum) | `PascalCase` | `GameClock`, `TimerQueue`, `IOPoller` |
| Functions & methods | `PascalCase` | `AddRef()`, `TimeUntilNext()`, `ReadString()` |
| Accessors / mutators | `snake_case` matching member | `port()` for `port_`, `set_port()` |
| Local variables | `snake_case` | `fire_time`, `block_size`, `num_threads` |
| Class member variables | `snake_case_` (trailing underscore) | `mutex_`, `ref_count_`, `heap_` |
| Struct data members | `snake_case` (no trailing underscore) | `host`, `port`, `timeout` |
| Constants | `kPascalCase` | `kEpsilon`, `kCompileTimeMinLevel`, `kInvalidFd` |
| Enum values | `kPascalCase` | `LogLevel::kWarning`, `IOEvent::kReadable` |
| Macros | `ATLAS_UPPER_SNAKE` | `ATLAS_ASSERT`, `ATLAS_LOG_INFO`, `ATLAS_PLATFORM_WINDOWS` |
| Template parameters | `PascalCase` | `template <typename T>`, `template <IntrusiveRefCounted T>` |
| Concept names | `PascalCase` | `IntrusiveRefCounted`, `Trivial` |
| File names | `snake_case` | `timer_queue.h`, `binary_stream.cc` |

### Exemptions

The following names **must** remain `snake_case` because they implement C++ language or library protocols:

- **Coroutine protocol**: `await_ready`, `await_suspend`, `await_resume`, `initial_suspend`, `final_suspend`, `return_void`, `return_value`, `unhandled_exception`, `get_return_object`, `promise_type`
- **Container/iterator interface**: `begin`, `end`, `size`, `empty`, `data`, `swap`, `push_back`, etc.
- **C-linkage exports**: `Atlas*` functions follow PascalCase (e.g., `AtlasLogMessage`)

---

## 4. File Organization

### Source layout

```
src/lib/<module>/
    <file>.h        # Public header
    <file>.cc        # Implementation
    <file>_windows.cc  # Platform-specific (Windows)
    <file>_linux.cc    # Platform-specific (Linux)
```

No separate `include/` directory — headers live alongside sources. Include paths are relative to `src/lib/`:

```cpp
#include "foundation/error.h"      // Local project header
#include "platform/platform_config.h"
```

### Header structure

```cpp
#pragma once

// 1. C system headers
#include <unistd.h>

// 2. C++ standard library headers (alphabetical)
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

// 3. Third-party headers
#include <pugixml.h>

// 4. Project headers (quoted includes)
#include "foundation/error.h"

namespace atlas {

// Declarations...

}  // namespace atlas
```

### Include order (enforced by `.clang-format`)

Follows the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html#Names_and_Order_of_Includes):

1. Corresponding header (e.g., `foo.cc` includes `foo.h` first)
2. C system headers (`<unistd.h>`, `<windows.h>`)
3. C++ standard library headers (`<vector>`, `<cstdint>`)
4. Third-party headers (`<pugixml.h>`, `<rapidjson/...>`, `<gtest/gtest.h>`)
5. Project headers (`"foundation/error.h"`, `"network/socket.h"`)

---

## 5. Functions & Methods

### Trailing return types

Prefer trailing return types for non-trivial signatures:

```cpp
// Preferred
[[nodiscard]] auto ReadFile(const stdfs::path& path) -> Result<std::vector<std::byte>>;
[[nodiscard]] auto Instance() -> Logger&;

// Also acceptable for simple returns
void SetLevel(LogLevel level);
```

### Attributes

- **`[[nodiscard]]`** on all functions that return values the caller must use: factory functions, query methods, `Result<T>`, `std::optional<T>`.
- **`noexcept`** on move constructors, destructors, swap, lock/unlock, and trivial accessors.
- **`constexpr`** wherever the function can be evaluated at compile time.

```cpp
[[nodiscard]] constexpr auto IsValid() const -> bool { return id_ != 0; }
void Swap(IntrusivePtr& other) noexcept { std::swap(ptr_, other.ptr_); }
```

### Parameter passing

| Type | Convention |
|------|-----------|
| Small trivials (int, float, enum, pointer) | By value |
| Strings (read-only) | `std::string_view` |
| Large types (read-only) | `const T&` |
| Output / sink | `T&&` (move) or return by value |
| Spans of data | `std::span<const T>` / `std::span<T>` |

---

## 6. Error Handling

### Result<T, E>

Framework functions that can fail return `Result<T, Error>`:

```cpp
[[nodiscard]] auto ReadFile(const path& p) -> Result<std::vector<std::byte>>;

// Caller:
auto result = ReadFile(path);
if (!result) {
  ATLAS_LOG_ERROR("Failed: {}", result.error().message());
  return result.error();
}
auto& data = result.value();
```

### Assertions

- **`ATLAS_ASSERT(expr)`** — Debug-only. For invariant violations that indicate programmer error. Compiled out in Release.
- **`ATLAS_ASSERT_MSG(expr, msg)`** — Debug-only with message.
- **`ATLAS_CHECK(expr, error)`** — Always active. Returns error from the current function if expression is false.

```cpp
void Process(const Data* data) {
  ATLAS_ASSERT(data != nullptr);  // Programmer error if null
  // ...
}

auto Parse(std::string_view input) -> Result<Value> {
  ATLAS_CHECK(!input.empty(), Error{ErrorCode::kInvalidArgument, "Empty input"});
  // ...
}
```

### Third-party exception boundaries

When calling third-party code that throws, catch at the boundary:

```cpp
auto ParseXml(std::string_view xml) -> Result<DataSection::Ptr> {
  try {
    // ... pugixml calls ...
  } catch (const std::exception& e) {
    return Error{ErrorCode::kInternalError, e.what()};
  }
}
```

---

## 7. Classes & Types

### RAII

All resources must be managed by RAII wrappers. No naked `new`/`delete` except in allocator internals.

```cpp
// Good
auto ptr = MakeIntrusive<Entity>();
auto lib = DynamicLibrary::Load(path);

// Bad
Entity* e = new Entity();
```

### Rule of Five

If a class manages a resource, implement or `= delete` all five special members. Prefer `= default` when possible:

```cpp
class DynamicLibrary {
 public:
  ~DynamicLibrary();
  DynamicLibrary(const DynamicLibrary&) = delete;
  DynamicLibrary& operator=(const DynamicLibrary&) = delete;
  DynamicLibrary(DynamicLibrary&& other) noexcept;
  DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;
};
```

### Pimpl idiom

Use for classes with complex implementation details or platform-specific internals:

```cpp
class ThreadPool {
 public:
  explicit ThreadPool(uint32_t num_threads = 0);
  ~ThreadPool();
  // ...
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
```

### Singletons

Use Meyer's singleton (static local) when a true singleton is needed:

```cpp
auto Logger::Instance() -> Logger& {
  static Logger s_instance;
  return s_instance;
}
```

Private constructor, no copy/move.

---

## 8. Concurrency

### Thread safety rules

- **Single-threaded by default.** Classes are NOT thread-safe unless documented.
- **Mutable shared state** must be protected by `std::mutex` (preferred) or `std::atomic`.
- **Lock ordering** must be consistent to avoid deadlock. Never hold a lock while invoking callbacks.
- **`std::jthread`** over `std::thread` for automatic join and stop_token support.

```cpp
// Good: copy sinks, then invoke outside lock
void Logger::Log(...) {
  std::vector<std::shared_ptr<LogSink>> sinks_copy;
  {
    std::lock_guard lock(mutex_);
    sinks_copy = sinks_;
  }
  for (auto& sink : sinks_copy) {
    sink->Write(...);
  }
}
```

### Atomics

Use `std::atomic` for simple counters and flags. Specify memory ordering explicitly:

```cpp
ref_count_.fetch_add(1, std::memory_order_relaxed);      // increment
ref_count_.fetch_sub(1, std::memory_order_acq_rel);       // decrement (release semantics)
stopped_.exchange(true, std::memory_order_acq_rel);       // flag set
```

---

## 9. Templates & Concepts

### Concepts

Use C++20 concepts to constrain template parameters:

```cpp
template <typename T>
concept IntrusiveRefCounted = requires(const T& obj) {
  { obj.AddRef() } -> std::same_as<uint32_t>;
  { obj.Release() } -> std::same_as<uint32_t>;
};

template <IntrusiveRefCounted T>
class IntrusivePtr { ... };
```

### Header-only templates

Template classes and functions are header-only. Place them entirely in `.h` files. Non-template implementation goes in `.cc`.

---

## 10. Platform Abstraction

### Compile-time detection

Use CMake-provided macros (`ATLAS_PLATFORM_WINDOWS`, `ATLAS_PLATFORM_LINUX`) for `#if` guards. Use `atlas::platform::` constexpr bools for `if constexpr`:

```cpp
// Preprocessor: for conditional compilation of entire blocks/files
#if ATLAS_PLATFORM_WINDOWS
#include <windows.h>
#endif

// constexpr: for template/inline decisions
if constexpr (atlas::platform::is_windows) {
  // Windows-specific logic
}
```

### Platform-specific source files

Use suffix naming: `threading_windows.cc`, `threading_linux.cc`. Entire file content wrapped in `#if ATLAS_PLATFORM_*`:

```cpp
// threading_windows.cc
#include "platform/threading.h"

#if ATLAS_PLATFORM_WINDOWS
#include <windows.h>

namespace atlas {
// Windows implementation
}  // namespace atlas
#endif
```

### Abstract interfaces for platform-varying behavior

Use the Strategy pattern with a factory:

```cpp
class IOPoller {
 public:
  virtual ~IOPoller() = default;
  virtual auto Poll(Duration max_wait) -> Result<int> = 0;
  [[nodiscard]] static auto Create() -> std::unique_ptr<IOPoller>;
};
```

---

## 11. Testing

- **Framework**: Google Test + Google Mock
- **Location**: `tests/unit/test_<module>.cc`
- **Registration**: `atlas_add_test()` CMake macro
- **Naming**: `TEST(SuiteName, TestName)` — Suite matches class/module, Test describes behavior
- **Float comparison**: Use `EXPECT_NEAR(a, b, tolerance)` with tolerance `1e-5f`, never `EXPECT_EQ` for floats
- **`[[nodiscard]]` in tests**: Use `(void)handle;` or assign to a variable

```cpp
TEST(TimerQueueTest, OneShotFires) {
  TimerQueue queue;
  auto base = Clock::Now();
  int fired = 0;
  auto handle = queue.Schedule(base + Milliseconds(10),
      [&](TimerHandle) { ++fired; });
  EXPECT_TRUE(handle.IsValid());
  queue.Process(base + Milliseconds(10));
  EXPECT_EQ(fired, 1);
}
```

---

## 12. Documentation

- **No mandatory Doxygen** on every function. Code should be self-documenting through clear naming.
- **Required comments** for:
  - Non-obvious algorithms (e.g., Shepperd's method in `FromMatrix()`)
  - Thread safety guarantees and requirements
  - Preconditions not enforced by assertions
  - Euler angle conventions, coordinate system assumptions
  - Performance characteristics (e.g., "O(log n) insert")
- **No TODO comments** in committed code. Track tasks externally.

---

## 13. Dependencies

- **Core libraries** (foundation, platform, math) must not depend on third-party libraries other than the C++ standard library.
- **Serialization** may depend on vetted third-party parsers (pugixml, rapidjson).
- **Third-party warnings**: Suppress with `#pragma warning(push/pop)` at include boundaries, never globally.
- **FetchContent** for dependency management. Pin exact versions (tags, not branches).

---

## 14. Build & CMake

- **CMake 3.20+** required
- Use `atlas_add_library()`, `atlas_add_executable()`, `atlas_add_test()` macros
- Platform-conditional sources via `if(WIN32)` / `if(UNIX AND NOT APPLE)` in CMakeLists.txt
- Build configurations: Debug (`ATLAS_DEBUG=1`), Release (`NDEBUG`), RelWithDebInfo
- Warnings as errors: `/WX` (MSVC), `-Werror` (GCC/Clang)

---

## Quick Reference Checklist

Before submitting code, verify:

- [ ] Compiles with zero warnings on MSVC (`/W4 /WX`) and GCC (`-Wall -Wextra -Werror`)
- [ ] All new public functions have `[[nodiscard]]` where appropriate
- [ ] `noexcept` on move operations, destructors, trivial accessors
- [ ] `constexpr` on functions that can be compile-time evaluated
- [ ] No raw `new`/`delete` outside allocator internals
- [ ] Error cases return `Result<T>`, not exceptions
- [ ] Thread-unsafe classes documented as single-threaded
- [ ] Float comparisons use `AlmostEqual()` or `EXPECT_NEAR`, not `==`
- [ ] Bounds checks (assert) on array/container accessors
- [ ] Unit tests cover happy path, error path, and edge cases
- [ ] All existing tests still pass
