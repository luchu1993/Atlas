# 设计: C++ / C# 序列化格式对齐

> 归属阶段: ScriptPhase 3 (SpanWriter/SpanReader) + ScriptPhase 4 (Source Generator 序列化生成)

---

## 1. 问题

序列化贯穿引擎的每一个环节：

| 场景 | 数据流 | 要求 |
|------|--------|------|
| 属性同步 | C# → byte[] → C++ 网络层 → 客户端 | C++ 可透传，客户端可读 |
| RPC / Mailbox | C# → byte[] → C++ Bundle → TCP/UDP → 对端 C# | 双端 C# 格式一致 |
| 数据库持久化 | C# → byte[] → C++ DBApp → MySQL BLOB | C++ 可选读，C# 可读写 |
| 实体迁移 | C# → byte[] → C++ 进程间传输 → 对端 C# | C++ 透传，C# 可读写 |
| 热重载快照 | C# → byte[] → 内存 → C# | 纯 C# 闭环 |
| C++ 空间管理 | C# 属性变更 → C++ 需读取 Position | C++ 需能解析特定字段 |

**核心约束：C# `SpanWriter`/`SpanReader` 产出的字节流必须与 C++ `BinaryWriter`/`BinaryReader` 字节级兼容。**

## 2. 分层架构

```
Layer 3: C# 业务层（Source Generator 生成）
    entity.Serialize(ref SpanWriter writer)
    entity.Client.ShowDamage(100f, pos)
            │
            ▼
Layer 2: C# SpanWriter/SpanReader
    与 C++ BinaryStream 格式完全对齐的 C# 实现
            │
            ▼  byte[] 通过互操作传给 C++
Layer 1: C++ 网络层（已有 Bundle / Channel）
    Bundle.start_message() → writer.write_bytes(csharp_payload) → Bundle.end_message()
            │
            ▼
Layer 0: C++ 底层传输（已有 Socket / TCP / UDP）
```

关键设计：**C++ 网络层将 C# 产出的 byte[] 视为不透明 payload 直接嵌入 Bundle，不做解析。**

## 3. 格式对齐规范

### 3.1 当前 C++ BinaryWriter 格式

基于 `src/lib/serialization/binary_stream.hpp` 的现有实现：

- 字节序: **小端 (Little Endian)**
- Trivial 类型: `memcpy` + `endian::to_little()`
- 字符串: `write_packed_int(length) + write_bytes(utf8_data)`
- packed int: `value < 0xFF` → 1 byte; `value >= 0xFF` → `0xFF` + 4 bytes LE uint32

### 3.2 类型对齐对照表

| 数据类型 | C++ BinaryWriter | C# SpanWriter | 字节格式 |
|----------|-----------------|---------------|---------|
| bool | `write<uint8_t>(v ? 1 : 0)` | `WriteBool(v)` | 1 byte: 0x00/0x01 |
| int8/uint8 | `write<uint8_t>(v)` | `WriteByte(v)` | 1 byte |
| int16/uint16 | `write<uint16_t>(v)` | `WriteInt16(v)` / `WriteUInt16(v)` | 2 bytes LE |
| int32 | `write<int32_t>(v)` | `WriteInt32(v)` | 4 bytes LE |
| uint32 | `write<uint32_t>(v)` | `WriteUInt32(v)` | 4 bytes LE |
| int64 | `write<int64_t>(v)` | `WriteInt64(v)` | 8 bytes LE |
| uint64 | `write<uint64_t>(v)` | `WriteUInt64(v)` | 8 bytes LE |
| float | `write<float>(v)` | `WriteFloat(v)` | 4 bytes LE IEEE 754 |
| double | `write<double>(v)` | `WriteDouble(v)` | 8 bytes LE IEEE 754 |
| string | `write_string(sv)` | `WriteString(s)` | packed_int(byte_len) + UTF-8 |
| bytes | `write_packed_int(len)` + `write_bytes(data)` | `WriteBytes(span)` | packed_int(len) + raw |
| Vector3 | `write<float>` × 3 | `WriteVector3(v)` | 12 bytes (x,y,z float LE) |
| Quaternion | `write<float>` × 4 | `WriteQuaternion(v)` | 16 bytes (x,y,z,w float LE) |
| packed int | `write_packed_int(v)` | `WritePackedUInt32(v)` | v<0xFF: 1 byte; else: 0xFF + 4B LE |

### 3.3 packed int 格式详解

与 C++ `BinaryWriter::write_packed_int` 完全一致:

```
值 < 255 (0xFF):
  ┌──────────┐
  │ uint8(v) │   1 byte
  └──────────┘

值 >= 255:
  ┌──────┬────────────────┐
  │ 0xFF │ uint32_le(v)   │   5 bytes
  └──────┴────────────────┘
```

## 4. C# SpanWriter 实现

### 新建文件: `src/csharp/Atlas.Shared/Serialization/SpanWriter.cs`

```csharp
using System.Buffers;
using System.Buffers.Binary;
using System.Runtime.CompilerServices;
using System.Text;

namespace Atlas.Serialization;

public ref struct SpanWriter
{
    private byte[] _buffer;
    private int _position;

    public SpanWriter(int initialCapacity = 256)
    {
        _buffer = ArrayPool<byte>.Shared.Rent(initialCapacity);
        _position = 0;
    }

    // ---- 基本类型（小端，与 C++ endian::to_little 对齐）----

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public void WriteByte(byte value)
    {
        EnsureCapacity(1);
        _buffer[_position++] = value;
    }

    public void WriteBool(bool value) => WriteByte(value ? (byte)1 : (byte)0);

    public void WriteInt16(short value)
    {
        EnsureCapacity(2);
        BinaryPrimitives.WriteInt16LittleEndian(_buffer.AsSpan(_position), value);
        _position += 2;
    }

    public void WriteUInt16(ushort value)
    {
        EnsureCapacity(2);
        BinaryPrimitives.WriteUInt16LittleEndian(_buffer.AsSpan(_position), value);
        _position += 2;
    }

    public void WriteInt32(int value)
    {
        EnsureCapacity(4);
        BinaryPrimitives.WriteInt32LittleEndian(_buffer.AsSpan(_position), value);
        _position += 4;
    }

    public void WriteUInt32(uint value)
    {
        EnsureCapacity(4);
        BinaryPrimitives.WriteUInt32LittleEndian(_buffer.AsSpan(_position), value);
        _position += 4;
    }

    public void WriteInt64(long value)
    {
        EnsureCapacity(8);
        BinaryPrimitives.WriteInt64LittleEndian(_buffer.AsSpan(_position), value);
        _position += 8;
    }

    public void WriteUInt64(ulong value)
    {
        EnsureCapacity(8);
        BinaryPrimitives.WriteUInt64LittleEndian(_buffer.AsSpan(_position), value);
        _position += 8;
    }

    public void WriteFloat(float value)
        => WriteInt32(BitConverter.SingleToInt32Bits(value));

    public void WriteDouble(double value)
        => WriteInt64(BitConverter.DoubleToInt64Bits(value));

    // ---- packed int（与 C++ write_packed_int 完全一致）----

    public void WritePackedUInt32(uint value)
    {
        if (value < 0xFF)
        {
            WriteByte((byte)value);
        }
        else
        {
            WriteByte(0xFF);
            WriteUInt32(value);
        }
    }

    // ---- 字符串（与 C++ write_string 完全一致）----

    public void WriteString(string value)
    {
        int byteCount = Encoding.UTF8.GetByteCount(value);
        WritePackedUInt32((uint)byteCount);
        EnsureCapacity(byteCount);
        Encoding.UTF8.GetBytes(value.AsSpan(), _buffer.AsSpan(_position));
        _position += byteCount;
    }

    // ---- 复合类型 ----

    public void WriteVector3(Vector3 value)
    {
        WriteFloat(value.X);
        WriteFloat(value.Y);
        WriteFloat(value.Z);
    }

    public void WriteQuaternion(Quaternion value)
    {
        WriteFloat(value.X);
        WriteFloat(value.Y);
        WriteFloat(value.Z);
        WriteFloat(value.W);
    }

    public void WriteBytes(ReadOnlySpan<byte> data)
    {
        WritePackedUInt32((uint)data.Length);
        EnsureCapacity(data.Length);
        data.CopyTo(_buffer.AsSpan(_position));
        _position += data.Length;
    }

    // ---- 输出与生命周期 ----

    public readonly ReadOnlySpan<byte> WrittenSpan => _buffer.AsSpan(0, _position);
    public readonly int Length => _position;

    public void Dispose()
    {
        if (_buffer != null)
        {
            ArrayPool<byte>.Shared.Return(_buffer);
            _buffer = null!;
        }
    }

    private void EnsureCapacity(int additional)
    {
        int required = _position + additional;
        if (required <= _buffer.Length) return;

        int newSize = Math.Max(_buffer.Length * 2, required);
        var newBuffer = ArrayPool<byte>.Shared.Rent(newSize);
        _buffer.AsSpan(0, _position).CopyTo(newBuffer);
        ArrayPool<byte>.Shared.Return(_buffer);
        _buffer = newBuffer;
    }
}
```

### 新建文件: `src/csharp/Atlas.Shared/Serialization/SpanReader.cs`

```csharp
using System.Buffers.Binary;
using System.Runtime.CompilerServices;
using System.Text;

namespace Atlas.Serialization;

public ref struct SpanReader
{
    private readonly ReadOnlySpan<byte> _data;
    private int _position;

    public SpanReader(ReadOnlySpan<byte> data)
    {
        _data = data;
        _position = 0;
    }

    public byte ReadByte()
    {
        CheckRemaining(1);
        return _data[_position++];
    }

    public bool ReadBool() => ReadByte() != 0;

    public short ReadInt16()
    {
        CheckRemaining(2);
        var v = BinaryPrimitives.ReadInt16LittleEndian(_data.Slice(_position));
        _position += 2;
        return v;
    }

    public ushort ReadUInt16()
    {
        CheckRemaining(2);
        var v = BinaryPrimitives.ReadUInt16LittleEndian(_data.Slice(_position));
        _position += 2;
        return v;
    }

    public int ReadInt32()
    {
        CheckRemaining(4);
        var v = BinaryPrimitives.ReadInt32LittleEndian(_data.Slice(_position));
        _position += 4;
        return v;
    }

    public uint ReadUInt32()
    {
        CheckRemaining(4);
        var v = BinaryPrimitives.ReadUInt32LittleEndian(_data.Slice(_position));
        _position += 4;
        return v;
    }

    public long ReadInt64()
    {
        CheckRemaining(8);
        var v = BinaryPrimitives.ReadInt64LittleEndian(_data.Slice(_position));
        _position += 8;
        return v;
    }

    public ulong ReadUInt64()
    {
        CheckRemaining(8);
        var v = BinaryPrimitives.ReadUInt64LittleEndian(_data.Slice(_position));
        _position += 8;
        return v;
    }

    public float ReadFloat()
        => BitConverter.Int32BitsToSingle(ReadInt32());

    public double ReadDouble()
        => BitConverter.Int64BitsToDouble(ReadInt64());

    public uint ReadPackedUInt32()
    {
        byte tag = ReadByte();
        return tag < 0xFF ? tag : ReadUInt32();
    }

    public string ReadString()
    {
        int length = (int)ReadPackedUInt32();
        CheckRemaining(length);
        var str = Encoding.UTF8.GetString(_data.Slice(_position, length));
        _position += length;
        return str;
    }

    public Vector3 ReadVector3()
        => new(ReadFloat(), ReadFloat(), ReadFloat());

    public Quaternion ReadQuaternion()
        => new(ReadFloat(), ReadFloat(), ReadFloat(), ReadFloat());

    public ReadOnlySpan<byte> ReadBytes()
    {
        int length = (int)ReadPackedUInt32();
        CheckRemaining(length);
        var data = _data.Slice(_position, length);
        _position += length;
        return data;
    }

    public readonly int Remaining => _data.Length - _position;
    public readonly int Position => _position;

    // ---- 边界检查 ----

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void CheckRemaining(int count)
    {
        if (_position + count > _data.Length)
            throw new InvalidOperationException(
                $"SpanReader underflow: need {count} bytes at position {_position}, " +
                $"but only {_data.Length - _position} remaining");
    }
}
```

## 5. 数据流示例：一次 RPC 调用

`entity.Client.ShowDamage(100f, new Vector3(1, 2, 3))` 的完整字节流：

```
C# SpanWriter 写入:
  WriteFloat(100f)           → [00 00 C8 42]              (4 bytes, LE IEEE 754)
  WriteVector3(1, 2, 3)     → [00 00 80 3F]              (x=1.0f)
                                [00 00 00 40]              (y=2.0f)
                                [00 00 40 40]              (z=3.0f)

  WrittenSpan = 16 bytes: [00 00 C8 42 00 00 80 3F 00 00 00 40 00 00 40 40]
        │
        ▼ NativeApi.SendClientRpc(entityId, rpcId, target, payload)
C++ 侧嵌入 Bundle:
  Bundle.start_message(client_rpc_desc)
  writer.write<uint16_t>(rpcId)    → [01 00]              (RPC ID, LE)
  writer.write<uint32_t>(entityId) → [07 00 00 00]        (Entity ID, LE)
  writer.write<uint8_t>(target)    → [00]                 (OwnerClient)
  writer.write_bytes(payload, 16)  → [00 00 C8 42 ...]    (C# payload 原样嵌入)
  Bundle.end_message()
        │
        ▼ Channel → Socket → TCP → 客户端
客户端 C# SpanReader 读取:
  ReadFloat()    → 100.0f  ✓
  ReadVector3()  → (1, 2, 3)  ✓
```

## 6. 开发任务

### 任务 S.1: 实现 SpanWriter

**新建**: `src/csharp/Atlas.Shared/Serialization/SpanWriter.cs`

- [ ] 全部基本类型方法（见上述代码）
- [ ] `WritePackedUInt32` 与 C++ `write_packed_int` 逐字节对齐
- [ ] `WriteString` 使用 `packed_int(byte_len) + UTF-8` 格式
- [ ] `ArrayPool<byte>` 缓冲区管理，`Dispose()` 归还
- [ ] `EnsureCapacity` 自动扩容

### 任务 S.2: 实现 SpanReader

**新建**: `src/csharp/Atlas.Shared/Serialization/SpanReader.cs`

- [ ] 全部基本类型方法
- [ ] `ReadPackedUInt32` 与 C++ `read_packed_int` 逐字节对齐
- [ ] `ReadString` 格式匹配
- [ ] 边界检查 (`Remaining`)

### 任务 S.3: C++ RPC 导出函数实现

**修改**: `src/lib/clrscript/clr_native_api.cpp`

RPC 发送导出函数已在 ScriptPhase 2 文档的 `clr_native_api.hpp` 中声明（参见 [script_phase2_interop_layer.md](script_phase2_interop_layer.md) 任务 2.4）。此任务需实现这些函数的 C++ 侧逻辑:

```cpp
// 已在 clr_native_api.hpp 中声明的 extern "C" ATLAS_EXPORT 导出函数:
ATLAS_NATIVE_API void atlas_send_client_rpc(
    uint32_t entity_id, uint32_t rpc_id,
    uint8_t target, const uint8_t* payload, int32_t len);
ATLAS_NATIVE_API void atlas_send_cell_rpc(
    uint32_t entity_id, uint32_t rpc_id,
    const uint8_t* payload, int32_t len);
ATLAS_NATIVE_API void atlas_send_base_rpc(
    uint32_t entity_id, uint32_t rpc_id,
    const uint8_t* payload, int32_t len);
```

### 任务 S.4: 跨语言往返测试

**新建**: `tests/unit/test_serialization_alignment.cpp`

- [ ] C++ `BinaryWriter` 写 → C# `SpanReader` 读 → 值一致
- [ ] C# `SpanWriter` 写 → C++ `BinaryReader` 读 → 值一致
- [ ] 覆盖全部类型: bool/int/float/double/string/bytes/Vector3/Quaternion
- [ ] 边界值: 空字符串、Unicode 字符串、packed int 临界值 (254/255/256)
- [ ] 大数据: 1MB bytes、超长字符串

### 任务 S.5: C# 侧单元测试

**新建**: `tests/csharp/Atlas.Shared.Tests/SpanWriterReaderTests.cs`

- [ ] 各类型写入/读取往返
- [ ] packed int 边界: 0, 1, 254, 255, 256, uint.MaxValue
- [ ] 空字符串、emoji 字符串
- [ ] ArrayPool 缓冲区归还验证
- [ ] 自动扩容正确性
