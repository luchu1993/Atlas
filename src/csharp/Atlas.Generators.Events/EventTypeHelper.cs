namespace Atlas.Generators.Events;

internal static class EventTypeHelper
{
    public static bool TryGetReaderMethod(string typeFullName, out string readerMethod)
    {
        switch (typeFullName)
        {
            case "System.Boolean" or "bool": readerMethod = "ReadBool"; return true;
            case "System.SByte" or "sbyte": readerMethod = "ReadByte"; return true;
            case "System.Byte" or "byte": readerMethod = "ReadByte"; return true;
            case "System.Int16" or "short": readerMethod = "ReadInt16"; return true;
            case "System.UInt16" or "ushort": readerMethod = "ReadUInt16"; return true;
            case "System.Int32" or "int": readerMethod = "ReadInt32"; return true;
            case "System.UInt32" or "uint": readerMethod = "ReadUInt32"; return true;
            case "System.Int64" or "long": readerMethod = "ReadInt64"; return true;
            case "System.UInt64" or "ulong": readerMethod = "ReadUInt64"; return true;
            case "System.Single" or "float": readerMethod = "ReadFloat"; return true;
            case "System.Double" or "double": readerMethod = "ReadDouble"; return true;
            case "System.String" or "string": readerMethod = "ReadString"; return true;
            case "Atlas.DataTypes.Vector3": readerMethod = "ReadVector3"; return true;
            case "Atlas.DataTypes.Quaternion": readerMethod = "ReadQuaternion"; return true;
            default: readerMethod = ""; return false;
        }
    }
}
