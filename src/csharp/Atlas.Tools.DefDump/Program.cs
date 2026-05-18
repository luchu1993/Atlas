using System;
using System.IO;
using System.Reflection;
using Atlas.Serialization;

namespace Atlas.Tools.DefDump;

// DBApp has no CoreCLR; this offline tool extracts the ModuleInitializer's
// PInvoke payload to a binary file. Usage: --assembly <path> --out <path>.
public static class Program
{
    private const uint FileMagic = 0x46445441u;  // 'A''T''D''F' little-endian
    private const ushort FileVersion = 2;

    public static int Main(string[] args)
    {
        string? assemblyPath = null;
        string? outPath = null;
        for (int i = 0; i < args.Length; ++i)
        {
            switch (args[i])
            {
                case "--assembly" when i + 1 < args.Length:
                    assemblyPath = args[++i]; break;
                case "--out" when i + 1 < args.Length:
                    outPath = args[++i]; break;
                case "-h":
                case "--help":
                    PrintUsage(); return 0;
            }
        }
        if (assemblyPath is null || outPath is null)
        {
            PrintUsage();
            return 1;
        }

        // Without this, the first generated-type touch hits AssemblyResolveError.
        var assemblyDir = Path.GetDirectoryName(Path.GetFullPath(assemblyPath));
        AppDomain.CurrentDomain.AssemblyResolve += (_, e) =>
        {
            if (assemblyDir is null) return null;
            var simpleName = new AssemblyName(e.Name).Name;
            if (simpleName is null) return null;
            var candidate = Path.Combine(assemblyDir, simpleName + ".dll");
            return File.Exists(candidate) ? Assembly.LoadFrom(candidate) : null;
        };

        Assembly target;
        try
        {
            target = Assembly.LoadFrom(Path.GetFullPath(assemblyPath));
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"DefDump: cannot load assembly '{assemblyPath}': {ex.Message}");
            return 2;
        }

        // Order mirrors the runtime ModuleInitializer: structs → components → types.
        var structBlobs = TryCollectBlobs(target, "Atlas.Def.DefStructRegistry");
        var componentBlobs = TryCollectBlobs(target, "Atlas.Def.DefComponentRegistry");
        var typeBlobs = TryCollectBlobs(target, "Atlas.Def.DefEntityTypeRegistry");

        try
        {
            using var w = new SpanWriter(8 * 1024);
            try
            {
                w.WriteUInt32(FileMagic);
                w.WriteUInt16(FileVersion);
                w.WriteUInt16(0);  // flags

                w.WritePackedUInt32((uint)structBlobs.Count);
                foreach (var blob in structBlobs)
                {
                    w.WritePackedUInt32((uint)blob.Length);
                    w.WriteRawBytes(blob);
                }

                w.WritePackedUInt32((uint)componentBlobs.Count);
                foreach (var blob in componentBlobs)
                {
                    w.WritePackedUInt32((uint)blob.Length);
                    w.WriteRawBytes(blob);
                }

                w.WritePackedUInt32((uint)typeBlobs.Count);
                foreach (var blob in typeBlobs)
                {
                    w.WritePackedUInt32((uint)blob.Length);
                    w.WriteRawBytes(blob);
                }

                Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(outPath))!);
                File.WriteAllBytes(outPath, w.WrittenSpan.ToArray());
            }
            finally { w.Dispose(); }
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"DefDump: write failed: {ex.Message}");
            return 3;
        }

        Console.WriteLine(
            $"DefDump: wrote {outPath} (structs={structBlobs.Count}, components={componentBlobs.Count}, types={typeBlobs.Count})");
        return 0;
    }

    private static System.Collections.Generic.List<byte[]> TryCollectBlobs(Assembly asm, string typeName)
    {
        var result = new System.Collections.Generic.List<byte[]>();
        var t = asm.GetType(typeName, throwOnError: false);
        if (t is null)
        {
            // An empty section is a valid result (assembly may not host this registry).
            Console.Error.WriteLine($"DefDump: {typeName} not found in assembly (treating as empty)");
            return result;
        }
        var m = t.GetMethod("BuildAll", BindingFlags.Public | BindingFlags.Static);
        if (m is null)
        {
            Console.Error.WriteLine(
                $"DefDump: {typeName}.BuildAll not found — assembly built with a codegen older than the dual-emit pipeline?");
            return result;
        }
        var visit = new Action<byte[]>(blob => result.Add(blob));
        m.Invoke(null, new object[] { visit });
        return result;
    }

    private static void PrintUsage()
    {
        Console.WriteLine(
            "Atlas.Tools.DefDump --assembly <path-to-server-assembly.dll> --out <path-to-entity_defs.bin>");
    }
}
