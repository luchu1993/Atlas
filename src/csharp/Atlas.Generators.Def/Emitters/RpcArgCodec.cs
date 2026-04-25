using System.Collections.Generic;
using System.Text;

namespace Atlas.Generators.Def.Emitters;

// Centralised emission helpers for RPC argument types. RPC args with a
// non-null TypeRef route here (containers + structs); scalar args still
// flow through DefTypeHelper.{ToCSharpType, WriteMethod, ReadMethod}.
//
// Containers are emitted as plain `List<T>` / `Dictionary<K,V>` (not
// the property-side ObservableList / ObservableDict) — RPC args are
// transient method parameters with no observability requirements, and
// using the plain types avoids forcing callers to allocate Observable
// wrappers just to invoke a method.
internal static class RpcArgCodec
{
    // C# parameter type for a method signature.
    public static string CsType(ArgDefModel arg)
    {
        if (arg.TypeRef is null) return DefTypeHelper.ToCSharpType(arg.Type);
        return PropertyCodec.CSharpTypeForStructField(arg.TypeRef);
    }

    // Serialize an argument value into `writerVar`. Container / struct
    // args go through EmitElementWrite which already handles nested
    // shapes recursively; scalar args use the cached WriteMethod path.
    public static void EmitWrite(StringBuilder sb, ArgDefModel arg,
                                 string writerVar, string indent)
    {
        if (arg.TypeRef is not null)
        {
            var names = new PropertyCodec.NameGen();
            PropertyCodec.EmitElementWrite(sb, arg.TypeRef, writerVar, arg.Name, indent, names);
            return;
        }
        sb.AppendLine($"{indent}{writerVar}.{DefTypeHelper.WriteMethod(arg.Type)}({arg.Name});");
    }

    // Deserialize an argument from `readerVar` into a fresh local; returns
    // the local's name. Container / struct args use EmitElementRead with
    // plainContainers=true so the result is a vanilla List/Dictionary.
    public static string EmitRead(StringBuilder sb, ArgDefModel arg,
                                  string readerVar, string indent)
    {
        if (arg.TypeRef is not null)
        {
            var names = new PropertyCodec.NameGen();
            return PropertyCodec.EmitElementRead(sb, arg.TypeRef, readerVar, indent, names,
                                                 plainContainers: true);
        }
        sb.AppendLine($"{indent}var {arg.Name} = {readerVar}.{DefTypeHelper.ReadMethod(arg.Type)}();");
        return arg.Name;
    }

    // PropertyDataType byte for the registry blob. Containers and
    // structs surface as kList=16 / kDict=17 / kStruct=18; scalar args
    // delegate to DefTypeHelper. C++ reads param_types but never
    // validates payload bytes against them today, so the body of an arg
    // (elem type, struct id, etc.) is intentionally not on the wire.
    public static byte WireKind(ArgDefModel arg)
    {
        if (arg.TypeRef is null) return DefTypeHelper.DataTypeId(arg.Type);
        return arg.TypeRef.Kind switch
        {
            PropertyDataKind.List => 16,
            PropertyDataKind.Dict => 17,
            PropertyDataKind.Struct => 18,
            _ => DefTypeHelper.DataTypeId(arg.Type),
        };
    }

    public static string BuildParamList(List<ArgDefModel> args)
    {
        if (args.Count == 0) return "";
        var parts = new string[args.Count];
        for (int i = 0; i < args.Count; i++)
            parts[i] = $"{CsType(args[i])} {args[i].Name}";
        return string.Join(", ", parts);
    }
}
