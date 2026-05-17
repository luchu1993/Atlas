using System;
using System.Buffers.Binary;
using Atlas.Core;

namespace Atlas.Space;

// Cellapp-side SpaceData entry point. Owner cellapp applies locally + fans
// out to peers and witnesses; non-owners forward to the owner.
public static class SpaceData
{
    public static void SetInt32(uint spaceId, ushort keyId, int value)
    {
        Span<byte> buf = stackalloc byte[4];
        BinaryPrimitives.WriteInt32LittleEndian(buf, value);
        NativeApi.SetSpaceData(spaceId, keyId, buf);
    }

    public static void SetInt64(uint spaceId, ushort keyId, long value)
    {
        Span<byte> buf = stackalloc byte[8];
        BinaryPrimitives.WriteInt64LittleEndian(buf, value);
        NativeApi.SetSpaceData(spaceId, keyId, buf);
    }

    public static void SetUInt32(uint spaceId, ushort keyId, uint value)
    {
        Span<byte> buf = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32LittleEndian(buf, value);
        NativeApi.SetSpaceData(spaceId, keyId, buf);
    }

    public static void SetUInt64(uint spaceId, ushort keyId, ulong value)
    {
        Span<byte> buf = stackalloc byte[8];
        BinaryPrimitives.WriteUInt64LittleEndian(buf, value);
        NativeApi.SetSpaceData(spaceId, keyId, buf);
    }

    public static void SetFloat(uint spaceId, ushort keyId, float value)
    {
        Span<byte> buf = stackalloc byte[4];
        BinaryPrimitives.WriteSingleLittleEndian(buf, value);
        NativeApi.SetSpaceData(spaceId, keyId, buf);
    }

    public static void SetDouble(uint spaceId, ushort keyId, double value)
    {
        Span<byte> buf = stackalloc byte[8];
        BinaryPrimitives.WriteDoubleLittleEndian(buf, value);
        NativeApi.SetSpaceData(spaceId, keyId, buf);
    }

    public static void SetBool(uint spaceId, ushort keyId, bool value)
    {
        Span<byte> buf = stackalloc byte[1];
        buf[0] = (byte)(value ? 1 : 0);
        NativeApi.SetSpaceData(spaceId, keyId, buf);
    }

    public static void SetString(uint spaceId, ushort keyId, string value)
    {
        var bytes = System.Text.Encoding.UTF8.GetBytes(value);
        NativeApi.SetSpaceData(spaceId, keyId, bytes);
    }

    public static void Remove(uint spaceId, ushort keyId) =>
        NativeApi.RemoveSpaceData(spaceId, keyId);
}
