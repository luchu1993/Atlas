# ScriptPhase 0: 清理 Python + 建立抽象层

> 预估周期: 1.5–2 周 | 前置依赖: 无

---

## 目标

1. 完全删除 Python 3 (`pyscript/`) 及所有相关依赖
2. 建立语言无关的 `ScriptEngine` / `ScriptValue` 抽象接口
3. 重写 `ScriptEvents`，移除对 `PyObjectPtr` 的直接耦合
4. 确保清理后项目（除脚本功能外）可正常编译和测试

## 验收标准 (M0)

- [ ] `src/lib/pyscript/` 目录完全删除（16 个文件）
- [ ] `tests/unit/test_py_*.cpp`（7 个）+ `test_atlas_module.cpp` 共 **8 个**测试文件全部删除
- [ ] `script_events.hpp` 不再 `#include "pyscript/py_object.hpp"`
- [ ] 项目编译不需要 Python3 SDK（`cmake/AtlasFindPackages.cmake` 无 `find_package(Python3)`）
- [ ] `ScriptEngine` / `ScriptValue` 接口定义完整，可被 ScriptPhase 1 的 `ClrScriptEngine` 实现
- [ ] 所有非 Python 相关的单元测试（25 个）通过

---

## 任务 0.1: 删除 pyscript 源码

### 删除文件清单（16 个）

```
src/lib/pyscript/
├── py_interpreter.hpp        # Python 解释器生命周期
├── py_interpreter.cpp
├── py_object.hpp             # PyObjectPtr 引用计数包装
├── py_type.hpp               # PyTypeBuilder 类型注册
├── py_type.cpp
├── py_module.hpp             # PyModuleBuilder 模块创建
├── py_module.cpp
├── py_convert.hpp            # C++ ↔ Python 类型转换
├── py_pickler.hpp            # Python Pickle 序列化
├── py_pickler.cpp
├── py_gil.hpp                # GIL 守卫 (GILGuard / GILRelease)
├── py_error.hpp              # Python 异常格式化
├── py_error.cpp
├── atlas_module.hpp          # 引擎暴露的 Python 模块
├── atlas_module.cpp
└── CMakeLists.txt
```

### 工作点

- [ ] 删除 `src/lib/pyscript/` 整个目录
- [ ] 从 `src/lib/CMakeLists.txt` 移除 `add_subdirectory(pyscript)`

### 当前 `src/lib/CMakeLists.txt` 需修改

```cmake
# 修改前
add_subdirectory(pyscript)    # ← 删除此行
add_subdirectory(script)

# 修改后
add_subdirectory(script)
# add_subdirectory(clrscript)  # ScriptPhase 1 时启用
```

---

## 任务 0.2: 删除 Python 相关测试

### 删除文件清单（8 个）

| 文件 | 测试内容 |
|------|---------|
| `tests/unit/test_py_interpreter.cpp` | Python 解释器初始化/关闭 |
| `tests/unit/test_py_object.cpp` | PyObjectPtr 引用计数 |
| `tests/unit/test_py_convert.cpp` | C++ ↔ Python 类型转换 |
| `tests/unit/test_py_gil.cpp` | GIL 锁守卫 |
| `tests/unit/test_py_type.cpp` | Python 类型注册 |
| `tests/unit/test_py_module.cpp` | Python 模块创建 |
| `tests/unit/test_py_pickler.cpp` | Pickle 序列化 |
| `tests/unit/test_atlas_module.cpp` | atlas Python 模块接口 |

### 修改 `tests/unit/CMakeLists.txt`

移除以下 8 个 test 条目：

```cmake
# 以下全部删除
atlas_add_test(test_py_interpreter ...)
atlas_add_test(test_py_object ...)
atlas_add_test(test_py_convert ...)
atlas_add_test(test_py_gil ...)
atlas_add_test(test_py_type ...)
atlas_add_test(test_py_module ...)
atlas_add_test(test_py_pickler ...)
atlas_add_test(test_atlas_module ...)
```

同时修改 `test_script_events`，使其依赖变更后的 `atlas_script`（不再传递依赖 `atlas_pyscript`）。此测试需重写以使用 mock `ScriptObject`，见任务 0.5。

---

## 任务 0.3: 移除 CMake 中的 Python 依赖

### 修改 `cmake/AtlasFindPackages.cmake`

```cmake
# 删除以下行
find_package(Python3 REQUIRED COMPONENTS Development Interpreter)
```

### 修改 `src/lib/script/CMakeLists.txt`

```cmake
# 修改前
atlas_add_library(atlas_script
    SOURCES
        script_events.cpp
    DEPS
        atlas_foundation
        atlas_pyscript          # ← 移除此依赖
)

# 修改后
atlas_add_library(atlas_script
    SOURCES
        script_events.cpp
        script_value.cpp        # ← 新增
    DEPS
        atlas_foundation
)
```

---

## 任务 0.4: 新建 ScriptEngine 抽象接口

### 新建文件: `src/lib/script/script_engine.hpp`

```cpp
#pragma once

#include "foundation/error.hpp"
#include "script/script_object.hpp"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace atlas
{

class ScriptValue;

// ============================================================================
// ScriptEngine — Language-agnostic script runtime interface
// ============================================================================
//
// Concrete implementations:
//   - ClrScriptEngine (.NET 10 / CoreCLR)  [ScriptPhase 1-3]
//
// Lifecycle: initialize() → [load_module()* → on_tick()*] → finalize()

class ScriptEngine
{
public:
    virtual ~ScriptEngine() = default;

    // Initialize the scripting runtime.
    [[nodiscard]] virtual auto initialize() -> Result<void> = 0;

    // Shut down the scripting runtime and release all resources.
    virtual void finalize() = 0;

    // Load a script module/assembly from the given path.
    [[nodiscard]] virtual auto load_module(
        const std::filesystem::path& path) -> Result<void> = 0;

    // Called each server tick.
    virtual void on_tick(float dt) = 0;

    // Called on server initialization (after all modules loaded).
    virtual void on_init(bool is_reload = false) = 0;

    // Called before shutdown.
    virtual void on_shutdown() = 0;

    // Call a global function by name.
    [[nodiscard]] virtual auto call_function(
        std::string_view module_name,
        std::string_view function_name,
        std::span<const ScriptValue> args) -> Result<ScriptValue> = 0;

    // Runtime name for diagnostics ("CLR", "Python", etc.)
    [[nodiscard]] virtual auto runtime_name() const -> std::string_view = 0;
};

} // namespace atlas
```

### 设计要点

- 纯虚基类，不包含任何语言特定头文件
- 生命周期方法: `initialize()` → `on_init()` → `on_tick()` (循环) → `on_shutdown()` → `finalize()`
- `load_module()` 语义: Python 对应 `import module`；CLR 对应加载 `.dll` 程序集
- 返回值使用项目标准的 `Result<T>` 模式

---

## 任务 0.5: 新建 ScriptValue 类型擦除容器

### 新建文件: `src/lib/script/script_value.hpp`

> **循环依赖处理**: `ScriptValue` 包含 `shared_ptr<ScriptObject>`，`ScriptObject` 的方法使用 `ScriptValue` 作为参数/返回值。
> 解决方案: `script_value.hpp` 仅前向声明 `ScriptObject`（不 include），`script_object.hpp` include `script_value.hpp`。
> `ScriptValue` 内部使用 `shared_ptr<ScriptObject>` 不需要完整类型定义（只需前向声明）。

```cpp
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace atlas
{

// 前向声明，不 #include "script_object.hpp"，避免循环依赖
class ScriptObject;

// ============================================================================
// ScriptValue — Type-erased value container for script ↔ engine data exchange
// ============================================================================
//
// Covers the common types that cross the script boundary.
// Complex/custom types should use ScriptObject or raw bytes.

class ScriptValue
{
public:
    using Bytes = std::vector<std::byte>;
    using ObjectPtr = std::shared_ptr<ScriptObject>;

    // Constructors
    ScriptValue() : data_(std::monostate{}) {}
    explicit ScriptValue(bool v) : data_(v) {}
    explicit ScriptValue(int64_t v) : data_(v) {}
    explicit ScriptValue(double v) : data_(v) {}
    explicit ScriptValue(std::string v) : data_(std::move(v)) {}
    explicit ScriptValue(Bytes v) : data_(std::move(v)) {}
    explicit ScriptValue(ObjectPtr v) : data_(std::move(v)) {}

    // Convenience factory for common C++ types
    static auto from_int(int32_t v) -> ScriptValue { return ScriptValue(static_cast<int64_t>(v)); }
    static auto from_float(float v) -> ScriptValue { return ScriptValue(static_cast<double>(v)); }

    // Type queries
    [[nodiscard]] auto is_none() const -> bool
    {
        return std::holds_alternative<std::monostate>(data_);
    }

    [[nodiscard]] auto is_bool() const -> bool
    {
        return std::holds_alternative<bool>(data_);
    }

    [[nodiscard]] auto is_int() const -> bool
    {
        return std::holds_alternative<int64_t>(data_);
    }

    [[nodiscard]] auto is_double() const -> bool
    {
        return std::holds_alternative<double>(data_);
    }

    [[nodiscard]] auto is_string() const -> bool
    {
        return std::holds_alternative<std::string>(data_);
    }

    [[nodiscard]] auto is_bytes() const -> bool
    {
        return std::holds_alternative<Bytes>(data_);
    }

    [[nodiscard]] auto is_object() const -> bool
    {
        return std::holds_alternative<ObjectPtr>(data_);
    }

    // Value extraction (caller must check type first)
    [[nodiscard]] auto as_bool() const -> bool { return std::get<bool>(data_); }
    [[nodiscard]] auto as_int() const -> int64_t { return std::get<int64_t>(data_); }
    [[nodiscard]] auto as_double() const -> double { return std::get<double>(data_); }
    [[nodiscard]] auto& as_string() const -> const std::string& { return std::get<std::string>(data_); }
    [[nodiscard]] auto& as_bytes() const -> const Bytes& { return std::get<Bytes>(data_); }
    [[nodiscard]] auto& as_object() const -> const ObjectPtr& { return std::get<ObjectPtr>(data_); }

private:
    std::variant<
        std::monostate,     // None / null
        bool,
        int64_t,
        double,
        std::string,
        Bytes,
        ObjectPtr
    > data_;
};

} // namespace atlas
```

### 新建文件: `src/lib/script/script_value.cpp`

```cpp
#include "script/script_value.hpp"

// ScriptValue is currently header-only (inline variant storage).
// This translation unit ensures the library has at least one object file
// and serves as a home for any future non-inline methods.

namespace atlas
{
} // namespace atlas
```

---

## 任务 0.6: 扩展 ScriptObject 接口

### 修改文件: `src/lib/script/script_object.hpp`

当前接口已具备 `is_none()`、`type_name()`、`get_attr()`、`as_int()`、`as_double()`、`as_string()`、`as_bool()` 方法。需要新增属性设置、可调用支持、字节提取和调试字符串能力：

```cpp
#pragma once

#include "foundation/error.hpp"
#include "script/script_value.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace atlas
{

// ============================================================================
// ScriptObject — Language-agnostic script object interface
// ============================================================================
//
// Concrete implementations:
//   - ClrObject (.NET GCHandle wrapper)  [ScriptPhase 2]
//
// Thread safety: depends on the concrete implementation.

class ScriptObject
{
public:
    virtual ~ScriptObject() = default;

    // --- 已有方法（保持不变）---
    [[nodiscard]] virtual auto is_none() const -> bool = 0;
    [[nodiscard]] virtual auto type_name() const -> std::string = 0;
    [[nodiscard]] virtual auto get_attr(
        std::string_view name) -> std::unique_ptr<ScriptObject> = 0;
    [[nodiscard]] virtual auto as_int() const -> Result<int64_t> = 0;
    [[nodiscard]] virtual auto as_double() const -> Result<double> = 0;
    [[nodiscard]] virtual auto as_string() const -> Result<std::string> = 0;
    [[nodiscard]] virtual auto as_bool() const -> Result<bool> = 0;

    // --- 新增方法 ---

    // Attribute write
    [[nodiscard]] virtual auto set_attr(
        std::string_view name, const ScriptValue& value) -> Result<void> = 0;

    // Callable support
    [[nodiscard]] virtual auto is_callable() const -> bool = 0;
    [[nodiscard]] virtual auto call(
        std::span<const ScriptValue> args = {}) -> Result<ScriptValue> = 0;

    // Binary data extraction
    [[nodiscard]] virtual auto as_bytes() const -> Result<std::vector<std::byte>> = 0;

    // Debug / diagnostics
    [[nodiscard]] virtual auto to_debug_string() const -> std::string
    {
        return std::string(type_name());  // 默认实现，子类可覆写提供更丰富信息
    }
};

} // namespace atlas
```

### 新增方法说明

| 新方法 | 用途 | 替代的 Python 功能 |
|--------|------|-------------------|
| `set_attr()` | 设置对象属性 | `PyObject_SetAttrString()` |
| `is_callable()` | 判断是否可调用 | `PyCallable_Check()` |
| `call()` | 调用脚本对象 | `PyObject_Call()` |
| `as_bytes()` | 提取二进制数据 | `PyBytes_AsString()` |
| `to_debug_string()` | 调试诊断字符串 | `PyObjectPtr::repr()` / `str()` |

---

## 任务 0.7: 重写 ScriptEvents

### 重写文件: `src/lib/script/script_events.hpp`

将 `PyObjectPtr` 全部替换为 `std::shared_ptr<ScriptObject>` 和 `ScriptValue`：

```cpp
#pragma once

#include "script/script_object.hpp"
#include "script/script_value.hpp"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace atlas
{

// ============================================================================
// ScriptEvents — Language-agnostic event system
// ============================================================================
//
// Thread safety: NOT thread-safe. Call from dispatcher thread only.

class ScriptEvents
{
public:
    using Callback = std::shared_ptr<ScriptObject>;

    explicit ScriptEvents(std::shared_ptr<ScriptObject> personality_module);
    ~ScriptEvents() = default;

    void on_init(bool is_reload = false);
    void on_tick(float dt);
    void on_shutdown();

    void register_listener(std::string_view event, Callback callback);
    void fire_event(std::string_view event,
                    std::span<const ScriptValue> args = {});

    [[nodiscard]] auto module() const -> const std::shared_ptr<ScriptObject>&
    {
        return module_;
    }

private:
    void call_module_method(std::string_view name,
                            std::span<const ScriptValue> args = {});

    std::shared_ptr<ScriptObject> module_;
    std::unordered_map<std::string, std::vector<Callback>> listeners_;
};

} // namespace atlas
```

### 重写文件: `src/lib/script/script_events.cpp`

```cpp
#include "script/script_events.hpp"
#include "script/script_value.hpp"
#include "foundation/log.hpp"

namespace atlas
{

ScriptEvents::ScriptEvents(std::shared_ptr<ScriptObject> personality_module)
    : module_(std::move(personality_module))
{
}

void ScriptEvents::call_module_method(std::string_view name,
                                      std::span<const ScriptValue> args)
{
    if (!module_ || module_->is_none()) return;

    auto method = module_->get_attr(name);
    if (!method || !method->is_callable())
    {
        return;
    }

    auto result = method->call(args);
    if (!result)
    {
        ATLAS_LOG_ERROR("Script callback '{}' failed: {}", name,
                        result.error().message());
    }
}

void ScriptEvents::on_init(bool is_reload)
{
    ScriptValue args[] = { ScriptValue(is_reload) };
    call_module_method("onInit", args);
}

void ScriptEvents::on_tick(float dt)
{
    ScriptValue args[] = { ScriptValue::from_float(dt) };
    call_module_method("onTick", args);
}

void ScriptEvents::on_shutdown()
{
    call_module_method("onShutdown");
}

void ScriptEvents::register_listener(std::string_view event, Callback callback)
{
    if (!callback || !callback->is_callable())
    {
        ATLAS_LOG_WARNING("ScriptEvents: ignoring non-callable listener for '{}'",
                          event);
        return;
    }
    listeners_[std::string(event)].push_back(std::move(callback));
}

void ScriptEvents::fire_event(std::string_view event,
                              std::span<const ScriptValue> args)
{
    auto it = listeners_.find(std::string(event));
    if (it == listeners_.end())
    {
        return;
    }

    for (auto& cb : it->second)
    {
        auto result = cb->call(args);
        if (!result)
        {
            ATLAS_LOG_WARNING("Event '{}' listener failed: {}", event,
                              result.error().message());
        }
    }
}

} // namespace atlas
```

---

## 任务 0.8: 重写 ScriptEvents 测试

### 修改文件: `tests/unit/test_script_events.cpp`

使用 `MockScriptObject` 替代 Python 依赖:

```cpp
#include <gtest/gtest.h>

#include "script/script_events.hpp"
#include "script/script_value.hpp"

namespace atlas::test
{

// Minimal mock implementing ScriptObject for testing
class MockScriptObject : public ScriptObject
{
public:
    bool is_none() const override { return false; }
    std::string type_name() const override { return "MockObject"; }

    auto get_attr(std::string_view name)
        -> std::unique_ptr<ScriptObject> override
    {
        auto it = methods_.find(std::string(name));
        if (it != methods_.end()) return it->second();
        return nullptr;
    }

    auto set_attr(std::string_view, const ScriptValue&)
        -> Result<void> override
    {
        return {};
    }

    bool is_callable() const override { return callable_; }

    auto call(std::span<const ScriptValue> args)
        -> Result<ScriptValue> override
    {
        call_count_++;
        last_args_.assign(args.begin(), args.end());
        return ScriptValue{};
    }

    auto as_int() const -> Result<int64_t> override { return Error{ErrorCode::ScriptTypeError, "not int"}; }
    auto as_double() const -> Result<double> override { return Error{ErrorCode::ScriptTypeError, "not double"}; }
    auto as_string() const -> Result<std::string> override { return Error{ErrorCode::ScriptTypeError, "not string"}; }
    auto as_bool() const -> Result<bool> override { return Error{ErrorCode::ScriptTypeError, "not bool"}; }
    auto as_bytes() const -> Result<std::vector<std::byte>> override { return Error{ErrorCode::ScriptTypeError, "not bytes"}; }

    bool callable_ = true;
    int call_count_ = 0;
    std::vector<ScriptValue> last_args_;

    using MethodFactory = std::function<std::unique_ptr<ScriptObject>()>;
    std::unordered_map<std::string, MethodFactory> methods_;
};

TEST(ScriptEventsTest, OnInitCallsModule)
{
    auto module = std::make_shared<MockScriptObject>();
    auto method = std::make_shared<MockScriptObject>();

    module->methods_["onInit"] = [method]() {
        auto copy = std::make_unique<MockScriptObject>();
        copy->callable_ = true;
        // Real method would delegate to shared state; simplified for test.
        return copy;
    };

    ScriptEvents events(module);
    events.on_init(false);
    // Verifies no crash; detailed call tracking requires shared state mock.
}

TEST(ScriptEventsTest, MissingMethodSilentlySucceeds)
{
    auto module = std::make_shared<MockScriptObject>();
    ScriptEvents events(module);
    events.on_tick(0.016f);
    events.on_shutdown();
}

} // namespace atlas::test
```

---

## 任务 0.9: 新建 ScriptValue 单元测试

### 新建文件: `tests/unit/test_script_value.cpp`

```cpp
#include <gtest/gtest.h>
#include "script/script_value.hpp"

namespace atlas::test
{

TEST(ScriptValueTest, DefaultIsNone)
{
    ScriptValue v;
    EXPECT_TRUE(v.is_none());
    EXPECT_FALSE(v.is_int());
    EXPECT_FALSE(v.is_string());
}

TEST(ScriptValueTest, BoolValue)
{
    ScriptValue v(true);
    EXPECT_TRUE(v.is_bool());
    EXPECT_TRUE(v.as_bool());
}

TEST(ScriptValueTest, IntValue)
{
    ScriptValue v(int64_t{42});
    EXPECT_TRUE(v.is_int());
    EXPECT_EQ(v.as_int(), 42);
}

TEST(ScriptValueTest, DoubleValue)
{
    ScriptValue v(3.14);
    EXPECT_TRUE(v.is_double());
    EXPECT_DOUBLE_EQ(v.as_double(), 3.14);
}

TEST(ScriptValueTest, StringValue)
{
    ScriptValue v(std::string("hello"));
    EXPECT_TRUE(v.is_string());
    EXPECT_EQ(v.as_string(), "hello");
}

TEST(ScriptValueTest, BytesValue)
{
    ScriptValue::Bytes data = { std::byte{0x01}, std::byte{0x02} };
    ScriptValue v(data);
    EXPECT_TRUE(v.is_bytes());
    EXPECT_EQ(v.as_bytes().size(), 2u);
}

TEST(ScriptValueTest, FromIntFactory)
{
    auto v = ScriptValue::from_int(7);
    EXPECT_TRUE(v.is_int());
    EXPECT_EQ(v.as_int(), 7);
}

TEST(ScriptValueTest, FromFloatFactory)
{
    auto v = ScriptValue::from_float(1.5f);
    EXPECT_TRUE(v.is_double());
    EXPECT_DOUBLE_EQ(v.as_double(), static_cast<double>(1.5f));
}

TEST(ScriptValueTest, MoveSemantics)
{
    ScriptValue a(std::string("move me"));
    ScriptValue b(std::move(a));
    EXPECT_TRUE(b.is_string());
    EXPECT_EQ(b.as_string(), "move me");
}

} // namespace atlas::test
```

### CMakeLists.txt 新增

```cmake
atlas_add_test(test_script_value
    SOURCES test_script_value.cpp
    DEPS atlas_script
)
```

---

## 任务依赖图

```
0.1 删除 pyscript 源码
        │
0.2 删除 Python 测试 ──────────────────────────┐
        │                                        │
0.3 移除 CMake Python 依赖                       │
        │                                        │
0.4 新建 ScriptEngine 接口 ──┐                   │
        │                    │                   │
0.5 新建 ScriptValue ────────┤                   │
        │                    │                   │
0.6 扩展 ScriptObject ───────┤                   │
                             │                   │
0.7 重写 ScriptEvents ───────┘                   │
        │                                        │
0.8 重写 ScriptEvents 测试 ──────────────────────┤
        │                                        │
0.9 新建 ScriptValue 测试 ──────────────────────┘
```

**建议执行顺序**:
1. 先做 0.1 + 0.2 + 0.3（纯删除操作，一次性清理）
2. 再做 0.4 + 0.5 + 0.6（新建/扩展接口，不涉及已有代码）
3. 然后 0.7（重写 ScriptEvents 实现）
4. 最后 0.8 + 0.9（测试，确认一切编译通过）

**预计代码变更量**: 删除 ~2500 行 (pyscript + tests)，新增 ~500 行 (接口 + ScriptValue + 重写 ScriptEvents)
