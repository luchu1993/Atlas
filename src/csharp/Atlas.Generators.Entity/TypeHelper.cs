namespace Atlas.Generators.Entity;

/// <summary>
/// Maps C# type names to SpanWriter/SpanReader methods and PropertyDataType bytes.
/// </summary>
internal static class TypeHelper
{
    public static bool TryGetSerializationMethods(
        string typeFullName,
        out string writerMethod,
        out string readerMethod,
        out byte dataTypeByte)
    {
        switch (typeFullName)
        {
            case "System.Boolean" or "bool":
                writerMethod = "WriteBool"; readerMethod = "ReadBool"; dataTypeByte = 0; return true;
            case "System.SByte" or "sbyte":
                writerMethod = "WriteByte"; readerMethod = "ReadByte"; dataTypeByte = 1; return true;
            case "System.Byte" or "byte":
                writerMethod = "WriteByte"; readerMethod = "ReadByte"; dataTypeByte = 2; return true;
            case "System.Int16" or "short":
                writerMethod = "WriteInt16"; readerMethod = "ReadInt16"; dataTypeByte = 3; return true;
            case "System.UInt16" or "ushort":
                writerMethod = "WriteUInt16"; readerMethod = "ReadUInt16"; dataTypeByte = 4; return true;
            case "System.Int32" or "int":
                writerMethod = "WriteInt32"; readerMethod = "ReadInt32"; dataTypeByte = 5; return true;
            case "System.UInt32" or "uint":
                writerMethod = "WriteUInt32"; readerMethod = "ReadUInt32"; dataTypeByte = 6; return true;
            case "System.Int64" or "long":
                writerMethod = "WriteInt64"; readerMethod = "ReadInt64"; dataTypeByte = 7; return true;
            case "System.UInt64" or "ulong":
                writerMethod = "WriteUInt64"; readerMethod = "ReadUInt64"; dataTypeByte = 8; return true;
            case "System.Single" or "float":
                writerMethod = "WriteFloat"; readerMethod = "ReadFloat"; dataTypeByte = 9; return true;
            case "System.Double" or "double":
                writerMethod = "WriteDouble"; readerMethod = "ReadDouble"; dataTypeByte = 10; return true;
            case "System.String" or "string":
                writerMethod = "WriteString"; readerMethod = "ReadString"; dataTypeByte = 11; return true;
            case "Atlas.DataTypes.Vector3":
                writerMethod = "WriteVector3"; readerMethod = "ReadVector3"; dataTypeByte = 13; return true;
            case "Atlas.DataTypes.Quaternion":
                writerMethod = "WriteQuaternion"; readerMethod = "ReadQuaternion"; dataTypeByte = 14; return true;
            default:
                writerMethod = ""; readerMethod = ""; dataTypeByte = 15; return false;
        }
    }

    public static string FieldNameToPropertyName(string fieldName)
    {
        if (fieldName.StartsWith("_") && fieldName.Length > 1)
        {
            return char.ToUpperInvariant(fieldName[1]) + fieldName.Substring(2);
        }
        return char.ToUpperInvariant(fieldName[0]) + fieldName.Substring(1);
    }
}
