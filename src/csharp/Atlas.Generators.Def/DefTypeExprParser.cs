using System;
using Microsoft.CodeAnalysis;

namespace Atlas.Generators.Def;

// Parses the type-expression grammar embedded in a .def <property type="...">
// or <field type="..."> attribute. Container delimiters are square brackets
// so the .def XML doesn't need &lt; / &gt; escaping:
//
//   <type-expr> := <scalar>
//                | "list[" <type-expr> "]"
//                | "dict[" <key-type> "," <type-expr> "]"
//                | <struct-or-alias-name>
//   <scalar>    := bool | int8 | uint8 | int16 | uint16
//                | int32 | uint32 | int64 | uint64
//                | float | double | string | bytes
//                | vector3 | quaternion
//   <key-type>  := string | int{8,16,32,64} | uint{8,16,32,64}
//
// Struct/alias lookups are deferred to DefLinker — the parser records the
// name as StructName and leaves StructId = -1.
internal static class DefTypeExprParser
{
    // Mirrors atlas::kMaxDataTypeDepth.
    public const int MaxDepth = 8;

    // Returns null on malformed input / depth overflow / bad dict key. When
    // non-null, reportDiagnostic has NOT been invoked. Callers that want to
    // distinguish legal scalars from custom names should inspect Kind.
    public static DataTypeRefModel? Parse(string text, Action<Diagnostic>? reportDiagnostic)
    {
        return ParseInner(text, depth: 0, reportDiagnostic);
    }

    private static DataTypeRefModel? ParseInner(string text, int depth,
                                                Action<Diagnostic>? reportDiagnostic)
    {
        if (depth > MaxDepth)
        {
            reportDiagnostic?.Invoke(Diagnostic.Create(
                DefDiagnosticDescriptors.DEF010, Location.None, text, depth, MaxDepth));
            return null;
        }

        var trimmed = text.Trim();
        if (trimmed.Length == 0)
        {
            reportDiagnostic?.Invoke(Diagnostic.Create(
                DefDiagnosticDescriptors.DEF009, Location.None, text, "empty"));
            return null;
        }

        // Case-insensitive `list[...]` / `dict[...,...]` prefix matching so
        // .def authors don't trip over capitalisation.
        if (HasPrefixIgnoreCase(trimmed, "list[") && trimmed.EndsWith("]"))
        {
            var inner = trimmed.Substring(5, trimmed.Length - 6);
            var elem = ParseInner(inner, depth + 1, reportDiagnostic);
            if (elem is null) return null;
            return new DataTypeRefModel { Kind = PropertyDataKind.List, Elem = elem };
        }

        if (HasPrefixIgnoreCase(trimmed, "dict[") && trimmed.EndsWith("]"))
        {
            var inner = trimmed.Substring(5, trimmed.Length - 6);
            var commaIdx = FindTopLevelComma(inner);
            if (commaIdx < 0)
            {
                reportDiagnostic?.Invoke(Diagnostic.Create(
                    DefDiagnosticDescriptors.DEF009, Location.None, text,
                    "dict[K,V] requires exactly one top-level comma"));
                return null;
            }
            var keyText = inner.Substring(0, commaIdx);
            var valText = inner.Substring(commaIdx + 1);
            var key = ParseInner(keyText, depth + 1, reportDiagnostic);
            if (key is null) return null;
            if (!IsValidDictKey(key.Kind))
            {
                reportDiagnostic?.Invoke(Diagnostic.Create(
                    DefDiagnosticDescriptors.DEF011, Location.None, keyText.Trim()));
                return null;
            }
            var value = ParseInner(valText, depth + 1, reportDiagnostic);
            if (value is null) return null;
            return new DataTypeRefModel { Kind = PropertyDataKind.Dict, Key = key, Elem = value };
        }

        var scalar = TryParseScalar(trimmed);
        if (scalar.HasValue)
        {
            return new DataTypeRefModel { Kind = scalar.Value };
        }

        // Anything else is assumed to be a struct or alias name; DefLinker
        // will reject unknowns. Names follow the usual XML identifier rules
        // so containing "[" / "," / whitespace would have landed in a
        // container branch above — if we get here with those chars, the
        // expression is malformed.
        if (ContainsReservedChar(trimmed))
        {
            reportDiagnostic?.Invoke(Diagnostic.Create(
                DefDiagnosticDescriptors.DEF009, Location.None, text,
                "unexpected character in type name"));
            return null;
        }

        return new DataTypeRefModel { Kind = PropertyDataKind.Struct, StructName = trimmed };
    }

    private static bool HasPrefixIgnoreCase(string s, string prefix)
    {
        if (s.Length < prefix.Length) return false;
        for (int i = 0; i < prefix.Length; ++i)
        {
            if (char.ToLowerInvariant(s[i]) != prefix[i]) return false;
        }
        return true;
    }

    private static int FindTopLevelComma(string inner)
    {
        int depth = 0;
        for (int i = 0; i < inner.Length; ++i)
        {
            char c = inner[i];
            if (c == '[') depth++;
            else if (c == ']') depth--;
            else if (c == ',' && depth == 0) return i;
        }
        return -1;
    }

    private static bool IsValidDictKey(PropertyDataKind kind) => kind switch
    {
        PropertyDataKind.String => true,
        PropertyDataKind.Int8 => true,
        PropertyDataKind.UInt8 => true,
        PropertyDataKind.Int16 => true,
        PropertyDataKind.UInt16 => true,
        PropertyDataKind.Int32 => true,
        PropertyDataKind.UInt32 => true,
        PropertyDataKind.Int64 => true,
        PropertyDataKind.UInt64 => true,
        _ => false,
    };

    private static PropertyDataKind? TryParseScalar(string text) =>
        text.ToLowerInvariant() switch
        {
            "bool" => PropertyDataKind.Bool,
            "int8" => PropertyDataKind.Int8,
            "uint8" => PropertyDataKind.UInt8,
            "int16" => PropertyDataKind.Int16,
            "uint16" => PropertyDataKind.UInt16,
            "int32" => PropertyDataKind.Int32,
            "uint32" => PropertyDataKind.UInt32,
            "int64" => PropertyDataKind.Int64,
            "uint64" => PropertyDataKind.UInt64,
            "float" => PropertyDataKind.Float,
            "double" => PropertyDataKind.Double,
            "string" => PropertyDataKind.String,
            "bytes" => PropertyDataKind.Bytes,
            "vector3" => PropertyDataKind.Vector3,
            "quaternion" => PropertyDataKind.Quaternion,
            _ => null,
        };

    private static bool ContainsReservedChar(string s)
    {
        foreach (var c in s)
        {
            if (c == '[' || c == ']' || c == ',' || char.IsWhiteSpace(c)) return true;
        }
        return false;
    }
}
