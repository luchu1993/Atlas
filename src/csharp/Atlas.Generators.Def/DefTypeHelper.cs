namespace Atlas.Generators.Def;

internal static class DefTypeHelper
{
    /// <summary>Map .def type name to C# type name.</summary>
    public static string ToCSharpType(string defType)
    {
        return defType.ToLowerInvariant() switch
        {
            "bool" => "bool",
            "int8" => "sbyte",
            "uint8" => "byte",
            "int16" => "short",
            "uint16" => "ushort",
            "int32" => "int",
            "uint32" => "uint",
            "int64" => "long",
            "uint64" => "ulong",
            "float" => "float",
            "double" => "double",
            "string" => "string",
            "bytes" => "byte[]",
            "vector3" => "Atlas.DataTypes.Vector3",
            "quaternion" => "Atlas.DataTypes.Quaternion",
            _ => defType,
        };
    }

    /// <summary>SpanWriter write method for a .def type.</summary>
    public static string WriteMethod(string defType)
    {
        return defType.ToLowerInvariant() switch
        {
            "bool" => "WriteBool",
            "int8" => "WriteInt8",
            "uint8" => "WriteUInt8",
            "int16" => "WriteInt16",
            "uint16" => "WriteUInt16",
            "int32" => "WriteInt32",
            "uint32" => "WriteUInt32",
            "int64" => "WriteInt64",
            "uint64" => "WriteUInt64",
            "float" => "WriteFloat",
            "double" => "WriteDouble",
            "string" => "WriteString",
            "bytes" => "WriteBytes",
            "vector3" => "WriteVector3",
            "quaternion" => "WriteQuaternion",
            _ => "WriteInt32",
        };
    }

    /// <summary>SpanReader read method for a .def type.</summary>
    public static string ReadMethod(string defType)
    {
        return defType.ToLowerInvariant() switch
        {
            "bool" => "ReadBool",
            "int8" => "ReadInt8",
            "uint8" => "ReadUInt8",
            "int16" => "ReadInt16",
            "uint16" => "ReadUInt16",
            "int32" => "ReadInt32",
            "uint32" => "ReadUInt32",
            "int64" => "ReadInt64",
            "uint64" => "ReadUInt64",
            "float" => "ReadFloat",
            "double" => "ReadDouble",
            "string" => "ReadString",
            "bytes" => "ReadBytes",
            "vector3" => "ReadVector3",
            "quaternion" => "ReadQuaternion",
            _ => "ReadInt32",
        };
    }

    /// <summary>Convert .def property name (snake_case) to C# property name (PascalCase).</summary>
    public static string ToPropertyName(string defName)
    {
        if (string.IsNullOrEmpty(defName)) return defName;
        // Simple PascalCase: capitalize first letter
        return char.ToUpperInvariant(defName[0]) + defName.Substring(1);
    }

    /// <summary>Convert .def property name to C# backing field name (_camelCase).</summary>
    public static string ToFieldName(string defName)
    {
        if (string.IsNullOrEmpty(defName)) return defName;
        return "_" + defName;
    }

    /// <summary>Default value expression for a .def type in C#.</summary>
    public static string DefaultValue(string defType)
    {
        return defType.ToLowerInvariant() switch
        {
            "string" => "\"\"",
            "bytes" => "System.Array.Empty<byte>()",
            "vector3" => "default",
            "quaternion" => "default",
            _ => "default",
        };
    }

    /// <summary>Whether the type needs a non-default initializer in field declaration.</summary>
    public static bool NeedsFieldInitializer(string defType)
    {
        return defType.ToLowerInvariant() switch
        {
            "string" => true,
            "bytes" => true,
            _ => false,
        };
    }

    /// <summary>C++ PropertyDataType enum value for binary descriptor serialization.</summary>
    public static byte DataTypeId(string defType)
    {
        return defType.ToLowerInvariant() switch
        {
            "bool" => 0,
            "int8" => 1,
            "uint8" => 2,
            "int16" => 3,
            "uint16" => 4,
            "int32" => 5,
            "uint32" => 6,
            "int64" => 7,
            "uint64" => 8,
            "float" => 9,
            "double" => 10,
            "string" => 11,
            "bytes" => 12,
            "vector3" => 13,
            "quaternion" => 14,
            _ => 15, // Custom
        };
    }
}
