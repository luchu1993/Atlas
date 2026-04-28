# 数据管线（Data Pipeline）

> **用途**：定义 Atlas 游戏配置数据的真源、构建管线、产物格式、热更新策略。这是策划与工程之间的契约——决定策划能否独立产出内容。
>
> **读者**：工程（必读）、策划（§3、§7、§9 必读）、工具开发（必读）、运维（§10 必读）。
>
> **状态**：草案 v0.1 — 待团队评审。
>
> **前置文档**：`OVERVIEW.md`、`DETERMINISM_CONTRACT.md`
>
> **下游文档**：所有战斗文档（SKILL/BUFF/...）的 Excel schema 都依赖本文规范

---

## 1. 设计目标与边界

### 1.1 核心目标

1. **真源唯一**：所有游戏数据有且仅有一个权威来源（Excel），杜绝"双写不同步"
2. **策划自主**：80%+ 数值/技能配置策划独立完成，无需工程介入
3. **构建可重现**：相同 Excel 输入 → 相同二进制产物（hash 一致）
4. **端同**：服务端 / Unity 客户端用同一份产物（不为不同端做不同表）
5. **快速校验**：错误在构建期暴露（外键、范围、DSL 编译），不到运行时炸服
6. **支持热更**：上线后能不停服更新数值

### 1.2 非目标

- **不做"运行时配置编辑"**：玩家版客户端不能改配置（避免作弊）
- **不做实时双向同步**：Excel ↔ Unity SO ↔ 服务端不做实时同步，单向编译
- **不做数据库存配置**：配置走文件管线，玩家进度才用数据库
- **不做版本控制系统**：复用 git，不自研

### 1.3 规模目标

按 OVERVIEW 项目愿景估算：
- 25 职业 × 60 技能 = 1500 技能
- 每技能 20 timeline event = **30000 行 SkillTimelines.xlsx**
- 1000+ buff 定义
- 5000+ 怪物配置
- 数十张其他表

工具必须能轻松处理这个规模。

---

## 2. 总体架构

### 2.1 数据流

```
┌─────────────────────────────────────────────────────────────┐
│  策划侧                                                      │
│  data/source/                                                │
│    ├── Skills.xlsx                                          │
│    ├── SkillTimelines.xlsx                                  │
│    ├── Buffs.xlsx                                           │
│    ├── ...（数十张）                                          │
│    └── dsl/*.dsl                                             │
└────────────────────────┬────────────────────────────────────┘
                         │ git commit
                         ▼
                  [CI / 本地构建]
                         │
                         ▼
┌─────────────────────────────────────────────────────────────┐
│  Build Pipeline (DataBuild.exe)                             │
│  ├── Excel Reader (EPPlus)                                  │
│  ├── Validator (外键 / 范围 / 类型)                          │
│  ├── DSL Compiler (text → bytecode)                         │
│  ├── Code Generator (生成 C# 数据类)                        │
│  └── FlatBuffers Serializer                                  │
└────────────────────────┬────────────────────────────────────┘
                         │
        ┌────────────────┼────────────────┐
        ▼                ▼                ▼
   data/generated/   csharp/Atlas    Unity/AssetBundles/
     ├── *.bytes      .CombatCore.    └── *.bytes
     │  (FBS 二进制)    Data/
     └── *.json        └── Generated/
        (debug)            *.cs
                          (数据类定义)
```

### 2.2 三类产物

| 产物 | 路径 | 用途 |
|---|---|---|
| **二进制数据** | `data/generated/*.bytes` | 服务端 + 客户端运行时加载（FlatBuffers） |
| **C# 数据类** | `csharp/Atlas.CombatCore.Data/Generated/*.cs` | 端共享 typed access |
| **DSL bytecode** | `data/generated/dsl.bytecode` | DSL VM 加载 |
| **debug JSON**（可选） | `data/generated/*.json` | 人类可读，仅 debug |

二进制是真正运行时数据，C# 是访问 API。

### 2.3 三个角色

| 角色 | 工具 | 产出 |
|---|---|---|
| **策划** | Excel | `.xlsx` 文件 |
| **DataBuild.exe** | C# console app | 校验 + 转换 |
| **运行时** | 服务端 + Unity | 加载 `.bytes` 用 |

---

## 3. Excel 表规范

### 3.1 文件组织

```
data/source/
├── core/
│   ├── Skills.xlsx
│   ├── SkillTimelines.xlsx
│   ├── SkillStates.xlsx
│   ├── SkillTransitions.xlsx
│   ├── SkillVariables.xlsx
│   ├── Buffs.xlsx
│   ├── BuffModifiers.xlsx
│   ├── BuffHandlers.xlsx
│   └── HitboxDefs.xlsx
├── actions/
│   ├── HitboxActions.xlsx
│   ├── DashActions.xlsx
│   ├── ApplyBuffActions.xlsx
│   ├── DamageTargetActions.xlsx
│   ├── PlayVFXActions.xlsx
│   ├── HitPauseActions.xlsx
│   └── ...（每种 ActionType 一张）
├── entities/
│   ├── Monsters.xlsx
│   ├── BossPhases.xlsx
│   └── NpcDefs.xlsx
├── items/
│   ├── Items.xlsx
│   ├── Equipment.xlsx
│   └── Materials.xlsx
├── world/
│   ├── Maps.xlsx
│   ├── Spawns.xlsx
│   └── Cells.xlsx
├── localization/
│   └── Strings.xlsx
└── dsl/
    ├── damage_formulas.dsl
    ├── target_selectors.dsl
    └── on_hit_callbacks.dsl
```

### 3.2 表结构标准

每张 Excel 文件的第一个 sheet 名 = 表名（如 `Skills`），后续 sheet 可作辅助/注释。

**第一行**：英文字段名（codegen 用）
**第二行**：类型声明（`int` / `float` / `string` / `bool` / `enum:EnumName` / `ref:OtherTable.id`）
**第三行**：中文注释（仅人类阅读）
**第四行起**：数据

示例 `Skills.xlsx`:

| skill_id | name | category | mode | cooldown_ms | mp_cost | max_level | icon |
|---|---|---|---|---|---|---|---|
| int | string | enum:SkillCategory | enum:SkillMode | int | int | int | string |
| 技能ID | 内部名 | 类别 | 模式 | 冷却毫秒 | 魔法消耗 | 最大等级 | 图标 |
| 1001 | warrior_slash | Active | Timeline | 3000 | 30 | 10 | icon_slash |

### 3.3 字段命名约定

- **小写下划线** (`skill_id`, `mp_cost`)
- 时间字段以 `_ms` 结尾（`cooldown_ms`, `duration_ms`）
- DSL 引用以 `_dsl_ref` 结尾
- 外键以 `_id` 结尾且类型为 `ref:`
- 枚举类型 `enum:EnumName` 必须在 `Enums.xlsx` 注册

### 3.4 类型系统

| 类型 | Excel 表示 | C# 生成 | 备注 |
|---|---|---|---|
| `int` | 数字 | `int` | 32-bit |
| `long` | 数字 | `long` | 64-bit |
| `float` | 数字 | `float` | IEEE 754 |
| `bool` | `true` / `false` / `1` / `0` | `bool` | |
| `string` | 文本 | `string` | UTF-8 |
| `enum:E` | 枚举值名（字符串） | `E` | 编译期值替换 |
| `ref:T.id` | 数字 | `int` | 必须存在于 T 表 |
| `vec3` | `"x,y,z"` 字符串 | `Vector3` | 解析 |
| `list:T` | `"a;b;c"` 字符串 | `List<T>` | 分号分隔 |
| `dsl_ref` | 字符串（dsl_id） | `int` | 必须存在于 DSL 库 |

### 3.5 注释行处理

任何以 `#` 开头的行被忽略（用于策划自己写注释）：
```
1001 | warrior_slash | ...
# 以下是高级技能
2001 | warrior_combo | ...
```

### 3.6 多 sheet 合并

某些表数据量大需要分 sheet（Excel 单 sheet 上限 1M 行）。
约定：所有 sheet 名相同前缀（如 `Skills_001`, `Skills_002`），DataBuild 自动合并。

---

## 4. DataBuild Pipeline

### 4.1 入口与流程

```csharp
// tools/DataBuild/Program.cs
public static int Main(string[] args) {
  var config = LoadBuildConfig();
  var collector = new ExcelCollector(config.SourceDir);
  
  // 1. 读取所有 Excel
  var tables = collector.ReadAllTables();
  
  // 2. 校验
  var validator = new TableValidator();
  var errors = validator.Validate(tables);
  if (errors.Count > 0) {
    foreach (var e in errors) Console.Error.WriteLine(e);
    return 1;
  }
  
  // 3. DSL 编译
  var dslCompiler = new DslCompiler();
  var dslBytecode = dslCompiler.CompileAll(config.DslDir);
  
  // 4. Codegen C# 数据类
  var codegen = new CSharpCodegen();
  codegen.Generate(tables, config.CodegenDir);
  
  // 5. 序列化 FlatBuffers
  var serializer = new FbsSerializer();
  serializer.Serialize(tables, dslBytecode, config.OutputDir);
  
  return 0;
}
```

### 4.2 性能目标

整个 pipeline（30000 行技能 + 1000 buff + 5000 怪物）：
- **冷启动构建**：< 30 秒（笔记本）
- **增量构建**（仅一张表改动）：< 5 秒
- **CI 完整 build**：< 60 秒

需要并行化（多 Excel 文件并行读、codegen 并行写）。

### 4.3 增量构建

每张 Excel 输出文件加 hash：
```
data/generated/
├── Skills.bytes
├── Skills.bytes.hash         # 输入 hash
├── ...
```

构建时：
- 读 Excel 计算 hash
- 与已有 hash 对比
- 一致 → 跳过（用旧 .bytes）
- 不一致 → 重新生成

DSL bytecode 同理（变化时全部重编）。

---

## 5. 校验规则

### 5.1 严格性原则

**任何校验失败 → build 失败**（不是 warning）。原因：
- 数据错误是策划工作流早期可发现的，越早暴露越省事
- 上线后才发现 = 数据热修代价高
- "warning 多了大家会忽略"

### 5.2 校验项

| 类型 | 规则 |
|---|---|
| **类型** | 字段值必须能解析为声明类型 |
| **必填** | 标记 `required` 的字段不能为空 |
| **外键** | `ref:T.id` 必须存在于 T 表 |
| **唯一** | `id` 字段不能重复 |
| **范围** | 字段可声明 `range: [min, max]`（如 `range: [0, 1]` 用于概率） |
| **枚举** | `enum:E` 值必须在 E 中定义 |
| **DSL 编译** | DSL 引用必须能编译（语法 + 类型 + 资源限制） |
| **逻辑校验** | 自定义规则（如 `cooldown_ms > 0`） |

### 5.3 自定义校验规则

每张表可写 C# 校验规则：

```csharp
public sealed class SkillsValidator : ITableValidator {
  public IEnumerable<ValidationError> Validate(Table skills, AllTables ctx) {
    foreach (var row in skills.Rows) {
      // 规则：mode = Timeline 时不能有 custom_handler
      if (row.GetEnum("mode") == "Timeline" 
          && !string.IsNullOrEmpty(row.GetString("custom_handler"))) {
        yield return new ValidationError {
          Table = "Skills",
          Row = row.RowNumber,
          Field = "custom_handler",
          Message = "Timeline mode skill must have empty custom_handler"
        };
      }
      
      // 规则：mp_cost <= 100 if max_level <= 5
      if (row.GetInt("max_level") <= 5 && row.GetInt("mp_cost") > 100) {
        yield return new ValidationError { /* ... */ };
      }
    }
  }
}
```

### 5.4 错误格式

```
[ERROR] Skills.xlsx:42 (skill_id=1001, custom_handler):
  Timeline mode skill must have empty custom_handler. Got "DragonKingPhase2".
  
[ERROR] SkillTimelines.xlsx:158 (skill_id=2003, action_ref):
  Foreign key 'hb_does_not_exist' not found in HitboxActions.id.

[ERROR] Dsl/damage_formulas.dsl:23 (formula='complex_dmg'):
  Type error: cannot multiply 'entity' by 'float' at column 18.
```

错误信息包含**精确定位**（文件、行号、字段、原因），策划能直接定位修复。

### 5.5 警告（非阻塞）

少数情况记 warning 不阻塞（避免过严妨碍迭代）：
- `tick_interval_ms > 0` 但无 OnTick handler
- 某 buff 没被任何技能使用（可能是孤儿数据）
- 字段值在常规范围之外但没明确 `range` 限制

CI 报告 warning 总数，超过阈值告警。

---

## 6. C# 代码生成

### 6.1 生成产物

每张 Excel 表生成一个**强类型 C# 类**：

```csharp
// 自动生成 — 请勿手动修改
namespace Atlas.CombatCore.Data.Generated;

public sealed class SkillDef {
  public int           SkillId        { get; init; }
  public string        Name           { get; init; }
  public SkillCategory Category       { get; init; }
  public SkillMode     Mode           { get; init; }
  public int           CooldownMs     { get; init; }
  public int           MpCost         { get; init; }
  public int           MaxLevel       { get; init; }
  public string        Icon           { get; init; }
}

public static class SkillTable {
  static FrozenDictionary<int, SkillDef> _byId;
  
  public static SkillDef Get(int id) => _byId[id];
  public static SkillDef? TryGet(int id) => _byId.TryGetValue(id, out var v) ? v : null;
  public static IEnumerable<SkillDef> All => _byId.Values;
  
  internal static void LoadFromBytes(byte[] data) { /* FlatBuffers */ }
}
```

### 6.2 端共享程序集

生成代码放在 `csharp/Atlas.CombatCore.Data/`：
- 项目目标 `netstandard2.1`（Unity + .NET 9 都支持）
- 编译为 `Atlas.CombatCore.Data.dll`
- 服务端引用：CoreCLR 加载
- Unity 客户端引用：通过 Assembly Definition 引用

**单一程序集**保证类型唯一，避免"服务端的 SkillDef" vs "客户端的 SkillDef" 不一致。

### 6.3 生成模板

用 T4 模板（C# 内置）或 Roslyn Source Generator：

```csharp
// 模板伪代码
foreach (var table in tables) {
  emit($"public sealed class {table.ClassName} {{");
  foreach (var field in table.Fields) {
    emit($"  public {field.CSharpType} {field.PascalName} {{ get; init; }}");
  }
  emit("}");
}
```

### 6.4 生成时机

- 每次 build 重新生成（增量）
- 生成的 .cs 文件**不入 git**（产物，可重现）
- CI 检查生成代码与最终编译一致

### 6.5 不生成的部分

- 业务逻辑（仅生成数据类）
- 自定义查询（如 `GetByCategory`）—— 写在手写代码里，扩展生成的 partial class

---

## 7. FlatBuffers 序列化

### 7.1 为什么 FlatBuffers

候选对比：

| 格式 | 加载速度 | 内存占用 | 跨语言 | 备注 |
|---|---|---|---|---|
| JSON | 慢（解析） | 大（DOM） | 是 | 仅 debug |
| Protobuf | 中 | 中 | 是 | 仍需反序列化 |
| **FlatBuffers** | **快**（零拷贝） | **小**（mmap） | **是** | C++ + C# 都支持，最佳选择 |
| MessagePack | 中 | 中 | 是 | 不如 FBS |

FlatBuffers 优势：
- **零拷贝**：直接 mmap 文件，无反序列化开销
- **随机访问**：表内任意行 O(log n)
- **schema 演化**：可加字段保持兼容
- **跨语言**：C++ / C# / Java 等

### 7.2 schema 文件

`tools/DataBuild/schemas/skills.fbs`:

```
namespace Atlas.CombatCore.Fbs;

table SkillDef {
  skill_id: int;
  name: string;
  category: ubyte;     // 枚举数值
  mode: ubyte;
  cooldown_ms: int;
  mp_cost: int;
  max_level: int;
  icon: string;
}

table SkillTable {
  defs: [SkillDef];    // 排序保证，binary search 可用
}

root_type SkillTable;
```

### 7.3 序列化流程

```csharp
public byte[] Serialize(SkillTable table) {
  var fbb = new FlatBufferBuilder(1024);
  
  // 排序保证可二分
  var sortedRows = table.Rows.OrderBy(r => r.SkillId);
  
  var defOffsets = new List<Offset<FbsSkillDef>>();
  foreach (var row in sortedRows) {
    var nameOff = fbb.CreateString(row.Name);
    FbsSkillDef.StartSkillDef(fbb);
    FbsSkillDef.AddSkillId(fbb, row.SkillId);
    FbsSkillDef.AddName(fbb, nameOff);
    // ...
    defOffsets.Add(FbsSkillDef.EndSkillDef(fbb));
  }
  
  var defsOff = FbsSkillTable.CreateDefsVector(fbb, defOffsets.ToArray());
  FbsSkillTable.StartSkillTable(fbb);
  FbsSkillTable.AddDefs(fbb, defsOff);
  fbb.Finish(FbsSkillTable.EndSkillTable(fbb).Value);
  
  return fbb.SizedByteArray();
}
```

### 7.4 加载（运行时）

```csharp
public static class SkillTable {
  static byte[] _bytes;
  static FbsSkillTable _table;
  static FrozenDictionary<int, int> _idToIndex;
  
  public static void LoadFromFile(string path) {
    _bytes = File.ReadAllBytes(path);
    _table = FbsSkillTable.GetRootAsSkillTable(new ByteBuffer(_bytes));
    
    var dict = new Dictionary<int, int>();
    for (int i = 0; i < _table.DefsLength; i++) {
      var def = _table.Defs(i).Value;
      dict[def.SkillId] = i;
    }
    _idToIndex = dict.ToFrozenDictionary();
  }
  
  public static SkillDef Get(int id) {
    int idx = _idToIndex[id];
    var fbsDef = _table.Defs(idx).Value;
    return new SkillDef {
      SkillId = fbsDef.SkillId,
      Name = fbsDef.Name,
      // ... 转 typed
    };
  }
}
```

**优化**：热路径直接用 FbsSkillDef 不转 typed（避免分配），仅在边界转。

### 7.5 文件大小

实测预期：
- 1500 技能 + 30000 timeline event：~3 MB
- 1000 buff：~500 KB
- 5000 怪物：~2 MB
- 总计 < 20 MB

加载时间（SSD）：< 100 ms。

---

## 8. DSL 编译集成

### 8.1 DSL 文件组织

```
data/source/dsl/
├── damage_formulas/
│   ├── basic.dsl
│   ├── elemental.dsl
│   └── boss_specific.dsl
├── target_selectors/
│   └── default.dsl
└── on_hit_callbacks/
    ├── basic.dsl
    └── special.dsl
```

每个 `.dsl` 文件可以包含多个命名片段：

```
# damage_formulas/basic.dsl

@id basic_phys_dmg
@returns float
return caster.atk_power * 1.0;

@id phys_with_def_pen
@returns float
let pen = caster.armor_penetration;
return caster.atk_power * (1 + pen);

@id ice_shatter_bonus
@returns float
return if target.has_status(Frozen) then 2.5 else 1.5;
```

### 8.2 编译

```csharp
public sealed class DslCompiler {
  public DslBytecodeBundle CompileAll(string dslDir) {
    var bundle = new DslBytecodeBundle();
    foreach (var file in Directory.GetFiles(dslDir, "*.dsl", SearchOption.AllDirectories)) {
      var snippets = ParseFile(file);
      foreach (var s in snippets) {
        var ast = Parser.Parse(s.Content);
        TypeChecker.Check(ast, s.ReturnType);
        var bytecode = Emitter.Emit(ast);
        bundle.Add(s.Id, bytecode);
      }
    }
    return bundle;
  }
}
```

### 8.3 引用解析

Excel 中 `dsl_ref` 字段：

```
SkillTimelines.xlsx:
  damage_dsl_ref: "phys_with_def_pen"
```

构建期：
- 检查 `phys_with_def_pen` 在 DSL bundle 存在
- 不存在 → 校验失败

运行时：
- 通过 `dsl_id` 查 bundle 取 bytecode 执行

### 8.4 资源限制

DSL 编译期严格执行（参见 `COMBAT_ACTIONS.md §8.4`）：
- 栈深 ≤ 64 → 编译期拒绝
- bytecode ≤ 1024 ops → 编译期拒绝
- gas limit 1000 → 运行时强制

---

## 9. 策划工作流

### 9.1 日常迭代

```
1. 策划在 Excel 改技能数值/添加新 buff
2. 本地跑 ./tools/build_data.bat
   → DataBuild 校验 + 生成
   → 失败：错误信息明确指出问题
   → 成功：data/generated/*.bytes 更新
3. 启动本地服务器 + Unity Editor 测试
4. 满意后 git commit Excel 文件
5. 推到 git，CI 自动跑 DataBuild + 单元测试 + 上传到测试服
```

### 9.2 CI 集成

```yaml
# .gitlab-ci.yml 示例
stages:
  - data_build
  - test

data_build:
  stage: data_build
  script:
    - dotnet run --project tools/DataBuild
    - git diff --exit-code data/generated/  # 确保提交者已更新
  artifacts:
    paths:
      - data/generated/

test:
  stage: test
  script:
    - dotnet test
```

### 9.3 多人协作

冲突解决：
- Excel 文件冲突难合并 → **小步提交**，频繁 pull
- 同一表多人修改 → 拆分到不同 sheet（合并工具自动处理）
- 大改动开 feature branch，merge 时由专人 review

### 9.4 与代码协作

新增 Action 类型 / 新增 BuffEvent / 新增 StatType：
- 工程改 C# 代码 + DataBuild schema
- 策划在 Excel 加对应行
- 两者**必须同 PR 提交**（避免半成品）

---

## 10. 热更新

### 10.1 哪些可以热更

| 数据类型 | 热更可行 | 备注 |
|---|---|---|
| 数值（伤害系数、CD） | ✅ | 简单换值 |
| 新增技能 / buff | ✅ | 加新 ID 即可 |
| 修改 timeline 事件顺序 | ⚠️ | 进行中的 SkillInstance 可能不一致，详见 §10.3 |
| 修改 DSL 公式 | ✅ | 新调用用新公式，进行中的不变 |
| 新增 ActionType | ❌ | 需要 C# 代码更新 |
| 修改 schema（字段类型） | ❌ | 需要客户端版本升级 |
| 修改地图布局 | ❌ | 玩家所在 cell 数据冲突 |

### 10.2 热更流程

```
策划/数值改 Excel → CI build → 产生新 .bytes
   ↓
运维触发"热更广播"：
  ├── 服务端 CellApp 检测新数据，加载到候补缓冲
  ├── 等待新 session 启动时使用新数据
  └── Unity 客户端通过 AssetBundle 下载新 .bytes
```

### 10.3 进行中 session 的处理

**保守策略**（推荐）：
- 已存在的 SkillInstance / BuffInstance 持有旧 def 引用
- 新创建的实例使用新 def
- 战斗 session 自然结束后切换

**激进策略**（仅紧急修复）：
- 立即重新映射所有 instance 到新 def
- 风险：进行中战斗的状态可能不一致
- 仅用于"快速 hotfix bug"

策略选择写在 BuffDef / SkillDef 的 `hot_reload_strategy` 字段。

### 10.4 客户端热更

Unity 客户端通过 AssetBundle 下载：
- `.bytes` 打包为 AssetBundle，从 CDN 下载
- 启动时检查版本，需要则下载最新
- DSL bytecode 同理打包

不需要客户端整包更新——只更新数据资源。

### 10.5 热更失败回滚

新数据加载失败：
- 服务端记日志，继续用旧数据
- 不影响进行中的战斗
- 运维收到告警，手动回滚 Excel

不会因为热更失败导致服务挂掉。

---

## 11. 性能与监控

### 11.1 启动加载

| 项 | 时间 |
|---|---|
| 读所有 .bytes（mmap） | < 50 ms |
| 构建 id → index 字典 | < 100 ms |
| DSL bundle 加载 | < 50 ms |
| **总启动加载** | **< 200 ms** |

### 11.2 运行时查询

- `Get(id)` 命中：< 100 ns（FrozenDictionary 查 + FBS 字段访问）
- `All` 遍历：完整顺序访问
- 缓存策略：**不主动缓存 typed object**，按需转换（避免 GC 压力）

### 11.3 监控

- 数据加载失败次数（应为 0）
- 每个 session 加载耗时
- DSL 调用频率分布（识别热点）
- 热更操作日志

---

## 12. FAQ 与反模式

### Q1: 为什么真源是 Excel 不是 JSON 直接编辑？

数值策划日常工作流以 Excel 为主：
- 公式（拉表关联、Σ 求和）
- 大量行批量编辑
- 数据透视、筛选、排序
- 团队熟悉，不需要培训

JSON 优势在 diff 友好，但策划手编 5000 行 JSON 不现实。Excel 是性价比最优。

代价是 Excel 二进制格式不易 diff —— 解决：导出为 stable CSV/JSON 同时入库，diff 工具读 CSV。

### Q2: 为什么不用 ScriptableObject？

参见 `OVERVIEW.md` 的早期讨论：
- ScriptableObject 是 Unity 专有
- 服务端不能加载
- .asset 格式 Unity 内部，不易跨工程
- 策划无法批量 / 公式编辑

ScriptableObject 仅在 Unity Editor 作为**编辑包装层**（可选）：策划用编辑器打开 .bytes 反向 import 为 SO，编辑后导出回 Excel。

### Q3: 为什么不用 Google Sheets / Notion 在线协作？

- 离线工作流方便（无网仍可改）
- git 集成成熟（diff / merge / branch）
- 不依赖第三方服务
- 数据量大时云端工具卡

但**前期试用阶段**可以用 Google Sheets，定期导出到 Excel + 入库。

### Q4: 多人同时改同一表怎么办？

- 频繁 pull 减少冲突窗口
- 大改动拆 feature branch
- 表结构合理时（窄表，每行一个独立实体）冲突少
- 极端情况用 csv 中间格式手动 diff

工具支持：`tools/excel_diff.exe` 把两版 .xlsx diff 成可读格式。

### Q5: 上线后改装备数值玩家会感知吗？

会。所以热更前必须公告：
- 重大数值改动（攻击/防御 ±10% 以上）：维护公告 + 补偿
- 小改动（< 5%）：版本更新公告
- 紧急 hotfix bug：立即上线 + 事后说明

### Q6: 策划改 Excel 改错了怎么办？

CI 校验通常能拦截大部分错误（外键、类型、范围）。漏网的：
- 数值不合理（伤害 999999）→ 在 range 校验加更严上限
- 平衡问题 → 上线前 review + 测试服试玩
- 上线后发现 → 数据回滚（git revert + redeploy）

不要让运行时校验"宽容错误"——CI 严格校验是保护策划的。

### Q7: 数据 hash 如何保证一致？

DataBuild 输出二进制后计算 SHA-256：
- `data/generated/Skills.bytes.sha256`
- CI 检查 hash 与本地一致（避免提交者忘记 build）
- 客户端启动验证 hash（确保数据完整）

### Q8: DSL 文件改了但没引用，会触发 build 失败吗？

不会——DSL 编译失败会立即报错，但**未被引用的 DSL 也会编译**。

可选：CI 报告"未被引用的 DSL"作为 warning，便于清理孤儿数据。

### Q9: Excel 里能直接写公式（如 `=A2*1.5`）吗？

**不建议**。原因：
- DataBuild 读单元格的"公式结果"还是"公式文本" 取决于读法
- 公式可能引用其他文件，跨文件依赖混乱
- 数值结果应该明确写出，便于 review

例外：策划自己在工作 sheet 里用公式辅助计算 → 在最终数据 sheet 用"值粘贴"。

### Q10: 一张表 30000 行 Excel 还能用吗？

可以——Excel 单 sheet 上限 100 万行。30000 行是中等规模，性能没问题。但：
- 加载慢（首次打开 ~5 秒）
- 编辑卡顿（可能）

策略：拆 sheet（按 skill 类别分），DataBuild 自动合并。

---

### 反模式清单

- ❌ 双写代码 + Excel（应只在 Excel）
- ❌ 在客户端运行时编辑配置（破坏权威）
- ❌ 用 wall clock 命名生成文件（破坏可重现性）
- ❌ 跳过校验"先上线再说"（数据 bug 修复成本指数级）
- ❌ 让生成代码进 git（产物，不入库）
- ❌ 修改生成代码（手动改会被下次 build 覆盖）
- ❌ 手编 .bytes 二进制文件（绕过流程）
- ❌ 不同端用不同数据格式（破坏端同）
- ❌ 把代码逻辑藏在 DSL 里（DSL 应只是表达式 + emit）
- ❌ 大改动不开 feature branch（中途 broken state 阻塞团队）

---

## 13. 里程碑

| 阶段 | 交付 |
|---|---|
| P0 末 | DataBuild MVP（读 Excel + 校验 + 生成 C# + 序列化 FBS） |
| P1 中 | 完整校验规则；DSL 编译集成 |
| P1 末 | 增量构建；CI 集成 |
| P2 中 | 热更新支持；新 ActionType / BuffEvent 端到端 |
| P2 末 | 全套战斗表 schema 落地；策划独立产出技能 |
| P3 | 与 Frame Data Editor 集成；可视化预览 |
| P4+ | 工具优化（Excel diff / 错误提示增强） |

---

## 14. 文档维护

- **Owner**：Tech Lead + Tools Engineer
- **关联文档**：
  - `OVERVIEW.md`
  - `SKILL_SYSTEM.md`、`BUFF_SYSTEM.md` 等所有引用 Excel schema 的文档
  - `COMBAT_ACTIONS.md`（DSL 编译规范）
  - `09_tools/FRAME_DATA_EDITOR.md`（编辑工具）
  - `10_liveops/HOTFIX_PLAYBOOK.md`（热更操作手册，待写）

---

**文档结束。**

**核心纪律重申**：
1. **Excel 是真源，不是工程的 input**：策划权威
2. **校验严格，build 阻塞**：错误越早暴露越好
3. **端共享 Atlas.CombatCore.Data.dll**：服务端和 Unity 引用同一份
4. **热更基于 session 切换**：进行中战斗保守不动
5. **不为不同端做不同表**：单一来源单一产物
