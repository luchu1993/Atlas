using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;

namespace Atlas.Generators.Def;

// Assigns globally-unique ids (1+, 0 reserved) to structs in ordinal-name
// order; rejects cross-entity name collisions and resolves alias trees.
internal static class DefLinker
{
    public const ushort FirstStructId = 1;

    // Stops pathological alias-of-alias cycles fast.
    public const int MaxAliasDepth = 16;

    public static LinkedDefs? Link(List<EntityDefModel> entities,
                                   Action<Diagnostic>? reportDiagnostic) =>
        Link(entities, new List<ComponentDefModel>(), null, reportDiagnostic);

    public static LinkedDefs? Link(List<EntityDefModel> entities,
                                   List<ComponentDefModel> standaloneComponents,
                                   Action<Diagnostic>? reportDiagnostic) =>
        Link(entities, standaloneComponents, null, reportDiagnostic);

    public static LinkedDefs? Link(List<EntityDefModel> entities,
                                   List<ComponentDefModel> standaloneComponents,
                                   ComponentIdManifest? componentManifest,
                                   Action<Diagnostic>? reportDiagnostic)
    {
        var allStructs = new Dictionary<string, StructDefModel>(StringComparer.Ordinal);
        var aliases = new Dictionary<string, DataTypeRefModel>(StringComparer.Ordinal);

        foreach (var e in entities)
        {
            foreach (var s in e.Structs)
            {
                if (allStructs.ContainsKey(s.Name))
                {
                    reportDiagnostic?.Invoke(Diagnostic.Create(
                        DefDiagnosticDescriptors.DEF012, Location.None, s.Name, e.Name));
                    return null;
                }
                // TypeRef resolver prefers structs; an alias here would be dropped silently.
                if (aliases.ContainsKey(s.Name))
                {
                    reportDiagnostic?.Invoke(Diagnostic.Create(
                        DefDiagnosticDescriptors.DEF016, Location.None, s.Name, e.Name));
                    return null;
                }
                allStructs[s.Name] = s;
            }
            foreach (var a in e.Aliases)
            {
                if (aliases.ContainsKey(a.Name))
                {
                    reportDiagnostic?.Invoke(Diagnostic.Create(
                        DefDiagnosticDescriptors.DEF012, Location.None, a.Name, e.Name));
                    return null;
                }
                if (allStructs.ContainsKey(a.Name))
                {
                    reportDiagnostic?.Invoke(Diagnostic.Create(
                        DefDiagnosticDescriptors.DEF016, Location.None, a.Name, e.Name));
                    return null;
                }
                aliases[a.Name] = a.Target;
            }
        }

        var ordered = allStructs.Values.OrderBy(s => s.Name, StringComparer.Ordinal).ToList();
        for (int i = 0; i < ordered.Count; ++i)
        {
            ordered[i].Id = FirstStructId + i;
        }

        var nameToId = ordered.ToDictionary(s => s.Name, s => (ushort)s.Id, StringComparer.Ordinal);

        foreach (var e in entities)
        {
            foreach (var s in e.Structs)
            {
                foreach (var f in s.Fields)
                {
                    if (!ResolveTypeRef(f.Type, nameToId, aliases, e.Name, f.Name,
                                        reportDiagnostic, aliasDepth: 0))
                        return null;
                }
            }
            foreach (var p in e.Properties)
            {
                if (p.TypeRef is null) continue;
                if (!ResolveTypeRef(p.TypeRef, nameToId, aliases, e.Name, p.Name,
                                    reportDiagnostic, aliasDepth: 0))
                    return null;
            }

            // Slot 0 reserved for entity body; synced get 1..N in declaration
            // order. Local components stay SlotIdx = -1.
            int nextSlot = 1;
            foreach (var c in e.Components)
            {
                if (c.Locality == ComponentLocality.Synced)
                    c.SlotIdx = nextSlot++;
                else
                    c.SlotIdx = -1;
            }
        }

        // Inheritance + propIdx resolution; emitters rely on Base / PropIdxBase.
        var standaloneByName = new Dictionary<string, ComponentDefModel>(StringComparer.Ordinal);
        foreach (var sc in standaloneComponents)
        {
            if (standaloneByName.ContainsKey(sc.TypeName))
            {
                reportDiagnostic?.Invoke(Diagnostic.Create(
                    DefDiagnosticDescriptors.DEF012, Location.None, sc.TypeName, "<standalone>"));
                return null;
            }
            standaloneByName[sc.TypeName] = sc;
        }

        // Memoized recursion handles topological order; depth cap catches cycles.
        foreach (var sc in standaloneComponents)
        {
            if (!ResolveComponentHierarchy(sc, standaloneByName, depth: 0, reportDiagnostic))
                return null;
        }

        // Standalone components must have a component_ids.xml entry; ATDF
        // consumers (UE client, DBApp) key on the assigned id.
        if (componentManifest != null)
        {
            foreach (var sc in standaloneComponents)
            {
                if (componentManifest.ActiveByName.TryGetValue(sc.TypeName, out var id))
                {
                    sc.ComponentTypeId = id;
                }
                else if (!componentManifest.DeprecatedNames.Contains(sc.TypeName))
                {
                    reportDiagnostic?.Invoke(Diagnostic.Create(
                        DefDiagnosticDescriptors.DEF019, Location.None, sc.TypeName));
                }
            }
        }

        // Copy-by-share from standalone for empty inline; validate scope subset.
        foreach (var e in entities)
        {
            foreach (var c in e.Components)
            {
                if (c.Locality != ComponentLocality.Synced) continue;
                if (c.Properties.Count == 0 && standaloneByName.TryGetValue(c.TypeName, out var sc))
                {
                    // Duplicated so entity-level emitters can iterate
                    // def.Components; class-level emit dedupes via standaloneByName.
                    c.Base = sc.Base;
                    c.BaseTypeName = sc.BaseTypeName;
                    foreach (var p in sc.Properties) c.Properties.Add(p);
                    foreach (var m in sc.ClientMethods) c.ClientMethods.Add(m);
                    foreach (var m in sc.CellMethods) c.CellMethods.Add(m);
                    foreach (var m in sc.BaseMethods) c.BaseMethods.Add(m);
                    c.PropIdxBase = sc.PropIdxBase;
                    c.ComponentTypeId = sc.ComponentTypeId;
                }
                // Validate P.scope ⊆ slot scope after resolution.
                foreach (var p in c.Properties)
                {
                    // Same scope-subset rule DefParser enforces inline.
                    if (!IsScopeSubsetForLinker(p.Scope, c.Scope))
                    {
                        reportDiagnostic?.Invoke(Diagnostic.Create(
                            DefDiagnosticDescriptors.DEF006, Location.None, e.Name,
                            $"property '{c.SlotName}.{p.Name}' scope is wider than slot scope"));
                        return null;
                    }
                    if (p.TypeRef is null) continue;
                    if (!ResolveTypeRef(p.TypeRef, nameToId, aliases, e.Name,
                                        $"{c.SlotName}.{p.Name}", reportDiagnostic,
                                        aliasDepth: 0))
                        return null;
                }
            }
        }

        return new LinkedDefs(entities, ordered, nameToId, standaloneComponents);
    }

    // Depth cap surfaces cycles in the `extends` chain quickly.
    private const int MaxComponentDepth = 16;

    private static bool ResolveComponentHierarchy(
        ComponentDefModel sc, Dictionary<string, ComponentDefModel> registry,
        int depth, Action<Diagnostic>? reportDiagnostic)
    {
        if (sc.Base != null) return true;             // already resolved
        if (sc.BaseTypeName == null) { sc.PropIdxBase = 0; return true; }
        if (depth > MaxComponentDepth)
        {
            reportDiagnostic?.Invoke(Diagnostic.Create(
                DefDiagnosticDescriptors.DEF017, Location.None,
                sc.TypeName, MaxComponentDepth));
            return false;
        }
        if (!registry.TryGetValue(sc.BaseTypeName, out var baseDef))
        {
            reportDiagnostic?.Invoke(Diagnostic.Create(
                DefDiagnosticDescriptors.DEF009, Location.None,
                sc.TypeName, sc.BaseTypeName));
            return false;
        }
        if (!ResolveComponentHierarchy(baseDef, registry, depth + 1, reportDiagnostic))
            return false;
        sc.Base = baseDef;
        sc.PropIdxBase = baseDef.PropIdxBase + baseDef.Properties.Count;
        return true;
    }

    private static bool IsScopeSubsetForLinker(PropertyScope inner, PropertyScope outer)
    {
        if (inner == outer) return true;
        bool innerOwn = inner.IsOwnClientVisible();
        bool innerOther = inner.IsOtherClientsVisible();
        bool outerOwn = outer.IsOwnClientVisible();
        bool outerOther = outer.IsOtherClientsVisible();
        if (innerOwn && !outerOwn) return false;
        if (innerOther && !outerOther) return false;
        return true;
    }

    // In-place: Struct → assign id or expand alias; List/Dict → recurse;
    // unknown name → DEF009 + return false. aliasDepth bounds cycles.
    private static bool ResolveTypeRef(DataTypeRefModel t,
                                       Dictionary<string, ushort> nameToId,
                                       Dictionary<string, DataTypeRefModel> aliases,
                                       string entityName, string holderName,
                                       Action<Diagnostic>? reportDiagnostic,
                                       int aliasDepth)
    {
        if (t.Kind == PropertyDataKind.Struct)
        {
            if (t.StructName is null)
            {
                // Shouldn't happen — parser always sets the name for Struct kind.
                return true;
            }
            if (nameToId.TryGetValue(t.StructName, out var id))
            {
                t.StructId = id;
                return true;
            }
            if (aliases.TryGetValue(t.StructName, out var target))
            {
                if (aliasDepth >= MaxAliasDepth)
                {
                    reportDiagnostic?.Invoke(Diagnostic.Create(
                        DefDiagnosticDescriptors.DEF017, Location.None,
                        t.StructName, MaxAliasDepth));
                    return false;
                }
                // Collapse the alias into this node, then resolve whatever the
                // alias points to.
                t.Kind = target.Kind;
                t.StructName = target.StructName;
                t.StructId = target.StructId;
                t.Elem = target.Elem;
                t.Key = target.Key;
                return ResolveTypeRef(t, nameToId, aliases, entityName, holderName,
                                      reportDiagnostic, aliasDepth + 1);
            }

            reportDiagnostic?.Invoke(Diagnostic.Create(
                DefDiagnosticDescriptors.DEF009, Location.None,
                $"{entityName}.{holderName}",
                $"unknown struct/alias '{t.StructName}'"));
            return false;
        }

        if (t.Elem is not null && !ResolveTypeRef(t.Elem, nameToId, aliases, entityName,
                                                   holderName, reportDiagnostic, aliasDepth))
            return false;
        if (t.Key is not null && !ResolveTypeRef(t.Key, nameToId, aliases, entityName,
                                                  holderName, reportDiagnostic, aliasDepth))
            return false;
        return true;
    }
}

internal sealed class LinkedDefs
{
    public List<EntityDefModel> Entities { get; }
    public List<StructDefModel> Structs { get; }
    public Dictionary<string, ushort> StructIdByName { get; }
    public Dictionary<string, StructDefModel> StructsByName { get; }
    public List<ComponentDefModel> StandaloneComponents { get; }

    public LinkedDefs(List<EntityDefModel> entities, List<StructDefModel> structs,
                     Dictionary<string, ushort> structIdByName)
        : this(entities, structs, structIdByName, new List<ComponentDefModel>()) { }

    public LinkedDefs(List<EntityDefModel> entities, List<StructDefModel> structs,
                     Dictionary<string, ushort> structIdByName,
                     List<ComponentDefModel> standaloneComponents)
    {
        Entities = entities;
        Structs = structs;
        StructIdByName = structIdByName;
        StandaloneComponents = standaloneComponents;
        StructsByName = new Dictionary<string, StructDefModel>(
            structs.Count, StringComparer.Ordinal);
        foreach (var s in structs) StructsByName[s.Name] = s;
    }
}
