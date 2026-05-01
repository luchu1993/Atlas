# C++ / C# 序列化格式对齐

> **状态**:✅ 已落地。`SpanWriter` / `SpanReader` 与 C++
> `BinaryStream` 字节级对齐,`SerializationEmitter` /
> `DeltaSyncEmitter` / `PropertyCodec` / `RpcArgCodec` 共同负责生成
> 端;`SpanWriterReaderTests`、`EntitySerializationTests`、
> `ListSyncTests` / `DictSyncTests` / `NestedContainerSyncTests` /
> `ContainerReuseTests` / `FieldStructSyncTests` / `StructSyncTests` /
> `ComponentReplicationFrameTests` 覆盖各场景。

序列化贯穿引擎各环节,要求 C# 产出的字节流与 C++ `BinaryWriter` /
`BinaryReader` 字节级兼容:

| 场景 | 数据流 | 要求 |
|---|---|---|
| 属性同步 | C# → `byte[]` → C++ 网络层 → 客户端 | C++ 透传,客户端可读 |
| RPC / Mailbox | C# → `byte[]` → C++ Bundle → TCP/UDP → 对端 C# | 双端 C# 格式一致 |
| 数据库持久化 | C# → `byte[]` → C++ DBApp → MySQL BLOB | C++ 可选读,C# 可读写 |
| 实体迁移 | C# → `byte[]` → C++ 进程间传输 → 对端 C# | C++ 透传,C# 可读写 |
| 热重载快照 | C# → `byte[]` → 内存 → C# | 纯 C# 闭环 |
| 空间管理 | C# 属性变更 → C++ 需读取 Position | C++ 需能解析特定字段 |

## 1. 分层

```
L3: C# 业务层(DefGenerator 生成)
      entity.Serialize(ref SpanWriter writer)
      entity.Client.ShowDamage(100f, pos)
                │
                ▼
L2: Atlas.Shared.Serialization.SpanWriter / SpanReader
      与 C++ BinaryStream 字节级对齐
                │
                ▼  byte[] 经 [LibraryImport] 传给 C++
L1: C++ 网络层 Bundle / Channel
      Bundle.start_message() → writer.write_bytes(payload) → Bundle.end_message()
                │
                ▼
L0: C++ 底层传输 Socket / TCP / UDP
```

C++ 网络层将 C# 产出的 `byte[]` 视为不透明 payload,仅负责封帧和路由,
不解析业务字段。

## 2. 字节格式

基于 `src/lib/serialization/binary_stream.{h,cc}` 的 `BinaryWriter` /
`BinaryReader`:

- 字节序:**小端**。
- Trivial 类型:`memcpy` + `endian::to_little()`。
- `packed_int`:`value < 0xFF` → 1 字节;否则 → `0xFF` + 4 字节 LE
  `uint32`。
- 字符串:`packed_int(byte_len) + UTF-8 bytes`。

| 类型 | C++ `BinaryWriter` | C# `SpanWriter` | 字节格式 |
|---|---|---|---|
| `bool` | `write<uint8_t>(v ? 1 : 0)` | `WriteBool(v)` | 1 byte 0x00/0x01 |
| `int8/uint8` | `write<uint8_t>(v)` | `WriteByte(v)` | 1 byte |
| `int16/uint16` | `write<int16_t/uint16_t>(v)` | `WriteInt16(v)` / `WriteUInt16(v)` | 2 bytes LE |
| `int32/uint32` | `write<int32_t/uint32_t>(v)` | `WriteInt32(v)` / `WriteUInt32(v)` | 4 bytes LE |
| `int64/uint64` | `write<int64_t/uint64_t>(v)` | `WriteInt64(v)` / `WriteUInt64(v)` | 8 bytes LE |
| `float` | `write<float>(v)` | `WriteFloat(v)` | 4 bytes LE IEEE 754 |
| `double` | `write<double>(v)` | `WriteDouble(v)` | 8 bytes LE IEEE 754 |
| `string` | `write_string(sv)` | `WriteString(s)` | packed_int(byte_len) + UTF-8 |
| `bytes` | `write_packed_int(len)` + `write_bytes(data)` | `WriteBytes(span)` | packed_int(len) + raw |
| `Vector3` | `write<float> × 3` | `WriteVector3(v)` | 12 bytes |
| `Quaternion` | `write<float> × 4` | `WriteQuaternion(v)` | 16 bytes |
| `packed uint` | `write_packed_int(v)` | `WritePackedUInt32(v)` | v<0xFF: 1 byte; else: 0xFF + 4B LE |
| 量化 yaw (BigWorld) | — | `WriteQuantizedYaw(radians)` | 1 byte |
| 量化 pitch (BigWorld) | — | `WriteQuantizedPitch(radians)` | 1 byte |
| 原始字节拷贝 | `write_bytes(ptr, len)` | `WriteRawBytes(span)` | 不带长度前缀,调用方自行约定边界 |

`packed_int` 格式:

```
值 < 0xFF:                  值 >= 0xFF:
┌──────────┐                ┌──────┬────────────────┐
│ uint8(v) │                │ 0xFF │ uint32_le(v)   │
└──────────┘  1 byte        └──────┴────────────────┘  5 bytes
```

## 3. 实现

`src/csharp/Atlas.Shared/Serialization/SpanWriter.cs`:

- `ref struct SpanWriter(int capacity = 256)`,内部 `byte[]` 来自
  `ArrayPool<byte>.Shared.Rent(capacity)`,自动扩容
  `max(len*2, required)`。
- 写方法:`WriteByte` / `WriteBool` / `WriteInt16` / `WriteUInt16` /
  `WriteInt32` / `WriteUInt32` / `WriteInt64` / `WriteUInt64` /
  `WriteFloat` / `WriteDouble` / `WritePackedUInt32` / `WriteString` /
  `WriteVector3` / `WriteQuaternion` / `WriteBytes` / `WriteQuantizedYaw` /
  `WriteQuantizedPitch` / `WriteRawBytes`。
- 视图:`WrittenSpan` / `Length` / `Position`。
- 生命周期:`Dispose()` 归还缓冲区到 `ArrayPool`。

`src/csharp/Atlas.Shared/Serialization/SpanReader.cs`:

- `ref struct SpanReader(ReadOnlySpan<byte> data)`。
- 读方法:与 `SpanWriter` 一一对应。
- 游标:`Remaining` / `Position` / `Advance(int count)`。

与 C++ 的兼容点:

- 浮点通过 `BitConverter.SingleToInt32Bits` / `DoubleToInt64Bits` 走
  `WriteInt32 / WriteInt64`,保证 IEEE 754 小端字节序。
- 字符串长度使用 `WritePackedUInt32`,与 `BinaryWriter::write_string` 匹配。
- `WriteBytes(span)` 带 `packed_int` 长度前缀;`WriteRawBytes(span)` 不带
  前缀,供 `HotReloadManager` 等需要外部长度约定的场景使用。

## 4. Source Generator 使用

- `DefGenerator` 的 `SerializationEmitter` / `DeltaSyncEmitter` 基于
  `.def` 属性列表生成 `Serialize(ref SpanWriter)` /
  `Deserialize(ref SpanReader)` /
  `SerializeReplicatedDelta` / `ApplyReplicatedDelta` / `ClearDirty`。
- `ServerEntity` 基类声明抽象 `Serialize` / `Deserialize`,由生成代码
  实现;`SerializeForOwnerClient` / `SerializeForOtherClients` 默认
  no-op,生成代码按需 override。
- 容器 / Struct / Component 由 `PropertyCodec` / `RpcArgCodec` 共用编解码
  助手,生成代码全部走 `SpanWriter` / `SpanReader` 公开方法,无反射。

## 5. 测试

- C#:`tests/csharp/Atlas.Runtime.Tests/SpanWriterReaderTests.cs` 覆盖
  各类型往返、边界值、`packed_int` 临界、`ArrayPool` 归还、自动扩容;
  容器同步与 Component 同步另由
  `ListSyncTests / DictSyncTests / NestedContainerSyncTests / ContainerReuseTests / FieldStructSyncTests / StructSyncTests / ComponentReplicationFrameTests` 等覆盖。
- 跨语言:C++ `test_clr_marshal.cpp` 覆盖 marshal 层,
  `test_clr_callback.cpp` 覆盖 NativeApi 往返;
  `test_clr_script_engine.cpp` 覆盖实体序列化 + 热重载状态迁移。
