# ScriptPhase 0: 清理 Python + 建立抽象层

> 预估周期: 1.5–2 周 | 前置依赖: 无 | **状态: ✅ 完成**

---

## 目标

1. 完全删除 Python 3 (`pyscript/`) 及所有相关依赖。
2. 建立语言无关的 `ScriptEngine` / `ScriptValue` 抽象接口。
3. 重写 `ScriptEvents`，移除对 `PyObjectPtr` 的直接耦合。
4. 确保清理后项目（除脚本功能外）可正常编译和测试。

## 验收标准 (M0)

- [x] `src/lib/pyscript/` 目录完全删除（16 个文件）。
- [x] `tests/unit/test_py_*.cpp`（7 个）+ `test_atlas_module.cpp` 共 **8 个**测试文件全部删除。
- [x] `script_events.h` 不再 `#include "pyscript/py_object.hpp"`。
- [x] 项目编译不需要 Python 3 SDK；`cmake/` 下不再存在 `AtlasFindPackages.cmake` / `find_package(Python3)`。
- [x] `ScriptEngine` / `ScriptValue` 接口定义完整，可被 `ClrScriptEngine` 实现。
- [x] 所有非 Python 相关的单元测试通过。

## 实际落地（2026-04-18 核对）

| 产出 | 当前文件 | 说明 |
|------|---------|------|
| 语言无关接口 | `src/lib/script/script_engine.h` | 纯虚基类；`Initialize / Finalize / LoadModule / OnInit / OnTick / OnShutdown / CallFunction / RuntimeName`；`Result<T>` 错误传递 |
| 类型擦除容器 | `src/lib/script/script_value.h` + `.cc` | `std::variant` 承载 none/bool/int64/double/string/bytes/`ScriptObject` 共 7 种类型 |
| 对象抽象 | `src/lib/script/script_object.h` | 新增 `set_attr / is_callable / call / as_bytes / to_debug_string` |
| 事件系统 | `src/lib/script/script_events.h` + `.cc` | 使用 `shared_ptr<ScriptObject>` + `span<const ScriptValue>`；Python 依赖归零 |
| 单元测试 | `tests/unit/test_script_value.cpp`、`test_script_events.cpp`、`test_script_app.cpp` | 以 Mock `ScriptObject` 验证生命周期，无需 CLR |

### 与原始设计的差异

- 项目统一采用 `.h` / `.cc` 扩展名与 `PascalCase` 成员函数；本文件内早期代码片段的 `.hpp` / `snake_case` 仅作为设计参考。
- `cmake/AtlasFindPackages.cmake` 未被保留，.NET 发现由 `cmake/FindDotNet.cmake` + `cmake/Dependencies.cmake` 完成。
- Python 删除清单的最终结果请参见 [README.md §7](README.md#7-python-删除清单已完成核对于-2026-04-18)。
