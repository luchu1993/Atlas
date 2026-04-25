using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.CodeAnalysis;

namespace Atlas.Generators.Def;

// Second-pass resolution across all EntityDefModel instances produced by
// DefParser. The parser only captures struct names; DefLinker assigns a
// deterministic uint16 id to each distinct struct and walks every
// DataTypeRefModel tree so kind==Struct nodes carry a concrete id by the
// time the emitter needs to serialise them.
//
// Struct ids are globally unique for now — entity-local naming would
// collide on the wire (the C++ registry is a flat map). Duplicate struct
// names across entities are rejected; aliases are resolved into their
// target DataTypeRef so no later stage needs to chase them.
//
// Deterministic ordering: structs are sorted by name (ordinal) and
// assigned ids starting at 1. 0 is reserved as "unassigned / invalid" to
// match the -1 sentinel the parser leaves in StructId.
internal static class DefLinker
{
    public const ushort FirstStructId = 1;

    // Depth cap for alias-of-alias chains. A legitimate graph reaches at
    // most a handful of hops; 16 is a comfortable ceiling that still
    // surfaces pathological/cyclic graphs quickly.
    public const int MaxAliasDepth = 16;

    public static LinkedDefs? Link(List<EntityDefModel> entities,
                                   Action<Diagnostic>? reportDiagnostic) =>
        Link(entities, new List<ComponentDefModel>(), reportDiagnostic);

    public static LinkedDefs? Link(List<EntityDefModel> entities,
                                   List<ComponentDefModel> standaloneComponents,
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
                // A struct must not shadow an alias declared earlier — the
                // downstream TypeRef resolver prefers structs over aliases
                // and would silently drop the alias entry.
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

            // Component slot indices are 1-based; slot 0 stays reserved
            // for the entity body. Synced components occupy contiguous
            // 1..N in declaration order; local components don't take
            // slots (SlotIdx = -1). Stable indices come for free since
            // the .def declaration order is deterministic.
            int nextSlot = 1;
            foreach (var c in e.Components)
            {
                if (c.Locality == ComponentLocality.Synced)
                    c.SlotIdx = nextSlot++;
                else
                    c.SlotIdx = -1;
            }
        }

        // Standalone component registry. Inheritance resolution + propIdx
        // numbering happen here so emitters can rely on Base + PropIdxBase
        // being populated.
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

        // Resolve `extends` and assign hierarchy-flat propIdx bases.
        // Process in topological order: a derived component is computed
        // after its base. A simple memoized recursion does the trick;
        // cycles surface via depth cap.
        foreach (var sc in standaloneComponents)
        {
            if (!ResolveComponentHierarchy(sc, standaloneByName, depth: 0, reportDiagnostic))
                return null;
        }

        // Resolve each entity-side component reference: copy-by-share
        // properties from the standalone definition when no inline body
        // was given, and validate scope subset rules.
        foreach (var e in entities)
        {
            foreach (var c in e.Components)
            {
                if (c.Locality != ComponentLocality.Synced) continue;
                if (c.Properties.Count == 0 && standaloneByName.TryGetValue(c.TypeName, out var sc))
                {
                    // Pull the type definition from the standalone def.
                    // Properties + RPC lists are duplicated here because
                    // entity-level emitters (DispatcherEmitter,
                    // TypeRegistryEmitter) iterate `def.Components` to wire
                    // dispatch / register the entity's blob; the
                    // canonical class-level emit (ComponentEmitter) keeps
                    // reading from the standalone via the dedup pass.
                    c.Base = sc.Base;
                    c.BaseTypeName = sc.BaseTypeName;
                    foreach (var p in sc.Properties) c.Properties.Add(p);
                    foreach (var m in sc.ClientMethods) c.ClientMethods.Add(m);
                    foreach (var m in sc.CellMethods) c.CellMethods.Add(m);
                    foreach (var m in sc.BaseMethods) c.BaseMethods.Add(m);
                    c.PropIdxBase = sc.PropIdxBase;
                }
                // Validate P.scope ⊆ slot scope after resolution.
                foreach (var p in c.Properties)
                {
                    // Scope check: property must be visible to a subset
                    // of observers compared to the slot. (Same rule as
                    // inline-checked in DefParser.)
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

    // Walks the `extends` chain bottom-up to assign PropIdxBase. Caps
    // depth to surface accidental cycles and pathological hierarchies.
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

    // Walks a DataTypeRef tree in-place:
    //   kind == Struct with StructName set but StructId unresolved:
    //     * if name matches a declared struct → assign its id.
    //     * if name matches an alias → replace this node with the alias target
    //       (mutates parent). Returns true to continue the parent's walk.
    //     * otherwise → DEF009 + return false.
    //   kind == List / Dict → recurse into children.
    //
    // `aliasDepth` bounds alias-of-alias chains so a pathological cycle
    // (A → B → A, built either by mistake or by a malformed .def) can't
    // blow the stack.
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

// Result of the link pass. `Structs` is the de-duplicated, id-assigned set
// (sorted by name for deterministic output); `Entities` share the same
// instances the parser produced with DataTypeRef nodes now carrying valid
// StructIds. `StructsByName` is the matching name → descriptor lookup the
// emitters need; building it here (rather than letting callers re-collect
// from Entities) keeps DefLinker the single source of truth for which
// structs are declared and addressable.
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
