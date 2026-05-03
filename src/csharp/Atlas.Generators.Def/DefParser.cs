using System;
using System.Collections.Generic;
using System.Xml.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Text;

namespace Atlas.Generators.Def;

// Either an entity model (root <entity>) or a standalone component
// type model (root <component>). DefParser.Parse dispatches on the
// root element so a single .def file pipeline handles both kinds.
internal sealed class ParsedDef
{
    public EntityDefModel? Entity { get; set; }
    public ComponentDefModel? StandaloneComponent { get; set; }
}

internal static class DefParser
{
    // Legacy entry point — returns just the entity model. Test fixtures
    // call this directly. The generator pipeline calls ParseAny so it
    // can also handle standalone <component> .def files.
    public static EntityDefModel? Parse(SourceText text, string filePath,
                                        Action<Diagnostic>? reportDiagnostic)
    {
        var any = ParseAny(text, filePath, reportDiagnostic);
        return any?.Entity;
    }

    public static ParsedDef? ParseAny(SourceText text, string filePath,
                                      Action<Diagnostic>? reportDiagnostic)
    {
        XDocument doc;
        try
        {
            doc = XDocument.Parse(text.ToString());
        }
        catch (Exception ex)
        {
            reportDiagnostic?.Invoke(Diagnostic.Create(
                DefDiagnosticDescriptors.DEF006, Location.None, filePath, ex.Message));
            return null;
        }

        var root = doc.Root;
        if (root == null)
        {
            reportDiagnostic?.Invoke(Diagnostic.Create(
                DefDiagnosticDescriptors.DEF006, Location.None, filePath, "empty document"));
            return null;
        }

        if (root.Name.LocalName == "component")
        {
            var comp = ParseStandaloneComponent(root, filePath, reportDiagnostic);
            return comp == null ? null : new ParsedDef { StandaloneComponent = comp };
        }

        if (root.Name.LocalName != "entity")
        {
            reportDiagnostic?.Invoke(Diagnostic.Create(
                DefDiagnosticDescriptors.DEF006, Location.None, filePath,
                "Root element must be <entity> or <component>"));
            return null;
        }

        var entity = ParseEntity(root, filePath, reportDiagnostic);
        return entity == null ? null : new ParsedDef { Entity = entity };
    }

    private static EntityDefModel? ParseEntity(XElement root, string filePath,
                                               Action<Diagnostic>? reportDiagnostic)
    {

        var model = new EntityDefModel
        {
            Name = root.Attribute("name")?.Value ?? ""
        };

        // <types> must be parsed before <properties> so property type-exprs
        // can refer to entity-local structs/aliases declared above them.
        var typesEl = root.Element("types");
        if (typesEl != null)
        {
            ParseTypesSection(typesEl, model, reportDiagnostic);
            if (!ValidateStructGraph(model, reportDiagnostic))
                return null;
        }

        // Properties
        var propsEl = root.Element("properties");
        if (propsEl != null)
        {
            var declaredPropNames = new HashSet<string>(StringComparer.Ordinal);
            foreach (var propEl in propsEl.Elements("property"))
            {
                var prop = ParseProperty(propEl, reportDiagnostic);
                if (prop is null) return null;
                if (!declaredPropNames.Add(prop.Name))
                {
                    reportDiagnostic?.Invoke(Diagnostic.Create(
                        DefDiagnosticDescriptors.DEF015, Location.None, model.Name, prop.Name));
                    return null;
                }
                // ATLAS_DEF008 — a replicable property named "position" collides
                // with the volatile channel / ClientEntity base Position. Flag
                // the property so every emitter skips it, and warn the user so
                // the silent skip isn't surprising.
                if (IsReplicableScope(prop.Scope)
                    && string.Equals(prop.Name, "position",
                                     StringComparison.OrdinalIgnoreCase))
                {
                    prop.IsReservedPosition = true;
                    reportDiagnostic?.Invoke(Diagnostic.Create(
                        DefDiagnosticDescriptors.DEF008, Location.None, model.Name));
                }
                model.Properties.Add(prop);
            }
        }

        // Client methods
        var clientMethodsEl = root.Element("client_methods");
        if (clientMethodsEl != null)
        {
            foreach (var methodEl in clientMethodsEl.Elements("method"))
            {
                var method = ParseMethod(methodEl, reportDiagnostic);
                // Validate: client_methods must not have exposed
                if (method.Exposed != ExposedScope.None)
                {
                    reportDiagnostic?.Invoke(Diagnostic.Create(
                        DefDiagnosticDescriptors.DEF002, Location.None, method.Name));
                    method.Exposed = ExposedScope.None;
                }
                if (method.HasReply)
                {
                    reportDiagnostic?.Invoke(Diagnostic.Create(
                        DefDiagnosticDescriptors.DEF018, Location.None, method.Name));
                    method.Reply = null;
                    method.ReplyTypeRef = null;
                }
                model.ClientMethods.Add(method);
            }
        }

        // Cell methods
        var cellMethodsEl = root.Element("cell_methods");
        if (cellMethodsEl != null)
        {
            foreach (var methodEl in cellMethodsEl.Elements("method"))
            {
                model.CellMethods.Add(ParseMethod(methodEl, reportDiagnostic));
            }
        }

        // Base methods
        var baseMethodsEl = root.Element("base_methods");
        if (baseMethodsEl != null)
        {
            foreach (var methodEl in baseMethodsEl.Elements("method"))
            {
                var method = ParseMethod(methodEl, reportDiagnostic);
                // Validate: base_methods must not use all_clients
                if (method.Exposed == ExposedScope.AllClients)
                {
                    reportDiagnostic?.Invoke(Diagnostic.Create(
                        DefDiagnosticDescriptors.DEF003, Location.None, method.Name));
                    method.Exposed = ExposedScope.OwnClient;
                }
                model.BaseMethods.Add(method);
            }
        }

        var componentsEl = root.Element("components");
        if (componentsEl != null)
        {
            ParseComponentsSection(componentsEl, model, reportDiagnostic);
        }

        return model;
    }

    private static void ParseComponentsSection(XElement componentsEl, EntityDefModel model,
                                               Action<Diagnostic>? reportDiagnostic)
    {
        var slotNames = new HashSet<string>(StringComparer.Ordinal);
        var typeNames = new HashSet<string>(StringComparer.Ordinal);

        foreach (var compEl in componentsEl.Elements("component"))
        {
            var slotName = compEl.Attribute("name")?.Value ?? "";
            var typeName = compEl.Attribute("type")?.Value ?? "";
            if (string.IsNullOrWhiteSpace(slotName) || string.IsNullOrWhiteSpace(typeName))
            {
                reportDiagnostic?.Invoke(Diagnostic.Create(
                    DefDiagnosticDescriptors.DEF006, Location.None,
                    model.Name, "<component> requires non-empty name and type"));
                continue;
            }
            // Slot names are script-facing C# property names — must be
            // unique within the entity. Type names must also be unique
            // across components on this entity (a single entity can't
            // have two slots of the same Component type without further
            // generator support).
            if (!slotNames.Add(slotName) || !typeNames.Add(typeName))
            {
                reportDiagnostic?.Invoke(Diagnostic.Create(
                    DefDiagnosticDescriptors.DEF015, Location.None, model.Name, slotName));
                continue;
            }

            var locality = ParseComponentLocality(compEl.Attribute("local")?.Value);
            var scope = ParseScope(compEl.Attribute("scope")?.Value ?? "all_clients");

            var comp = new ComponentDefModel
            {
                SlotName = slotName,
                TypeName = typeName,
                Scope = scope,
                Lazy = compEl.Attribute("lazy")?.Value == "true",
                Locality = locality,
            };

            // Inline <properties>. Local components reject any
            // <properties> per the design (their state lives in
            // hand-written C# fields, not the protocol).
            var propsEl = compEl.Element("properties");
            if (propsEl != null)
            {
                if (locality != ComponentLocality.Synced)
                {
                    reportDiagnostic?.Invoke(Diagnostic.Create(
                        DefDiagnosticDescriptors.DEF006, Location.None, model.Name,
                        $"local component '{slotName}' must not declare <properties>"));
                    continue;
                }
                var propNames = new HashSet<string>(StringComparer.Ordinal);
                foreach (var propEl in propsEl.Elements("property"))
                {
                    var prop = ParseProperty(propEl, reportDiagnostic);
                    if (prop is null) return;
                    if (!propNames.Add(prop.Name))
                    {
                        reportDiagnostic?.Invoke(Diagnostic.Create(
                            DefDiagnosticDescriptors.DEF015, Location.None,
                            $"{model.Name}.{slotName}", prop.Name));
                        return;
                    }
                    // P.scope ⊆ C.scope — observer must see the
                    // component before they can see its property.
                    if (!IsScopeSubset(prop.Scope, scope))
                    {
                        reportDiagnostic?.Invoke(Diagnostic.Create(
                            DefDiagnosticDescriptors.DEF006, Location.None, model.Name,
                            $"property '{slotName}.{prop.Name}' scope is wider than the component scope"));
                        return;
                    }
                    comp.Properties.Add(prop);
                }
            }

            // Component RPC sections — same parse logic as entity-level,
            // including the same exposed-scope guards. Local components
            // have no wire presence so their RPC declarations are rejected.
            ParseComponentMethodSections(compEl, comp, model.Name, reportDiagnostic);

            model.Components.Add(comp);
        }
    }

    // Parses <client_methods> / <cell_methods> / <base_methods> on a
    // component element (inline or standalone). Same role / exposed
    // validation as entity-level; local-component RPC is rejected because
    // those flavours never go on the wire.
    private static void ParseComponentMethodSections(XElement compEl, ComponentDefModel comp,
                                                     string ownerLabel,
                                                     Action<Diagnostic>? reportDiagnostic)
    {
        bool hasAnyMethod = compEl.Element("client_methods") != null
                            || compEl.Element("cell_methods") != null
                            || compEl.Element("base_methods") != null;
        if (hasAnyMethod && comp.Locality != ComponentLocality.Synced)
        {
            reportDiagnostic?.Invoke(Diagnostic.Create(
                DefDiagnosticDescriptors.DEF006, Location.None, ownerLabel,
                $"local component '{comp.SlotName}' must not declare RPC methods"));
            return;
        }

        var clientEl = compEl.Element("client_methods");
        if (clientEl != null)
        {
            foreach (var methodEl in clientEl.Elements("method"))
            {
                var method = ParseMethod(methodEl, reportDiagnostic);
                if (method.Exposed != ExposedScope.None)
                {
                    reportDiagnostic?.Invoke(Diagnostic.Create(
                        DefDiagnosticDescriptors.DEF002, Location.None, method.Name));
                    method.Exposed = ExposedScope.None;
                }
                if (method.HasReply)
                {
                    reportDiagnostic?.Invoke(Diagnostic.Create(
                        DefDiagnosticDescriptors.DEF018, Location.None, method.Name));
                    method.Reply = null;
                    method.ReplyTypeRef = null;
                }
                comp.ClientMethods.Add(method);
            }
        }

        var cellEl = compEl.Element("cell_methods");
        if (cellEl != null)
        {
            foreach (var methodEl in cellEl.Elements("method"))
                comp.CellMethods.Add(ParseMethod(methodEl, reportDiagnostic));
        }

        var baseEl = compEl.Element("base_methods");
        if (baseEl != null)
        {
            foreach (var methodEl in baseEl.Elements("method"))
            {
                var method = ParseMethod(methodEl, reportDiagnostic);
                if (method.Exposed == ExposedScope.AllClients)
                {
                    reportDiagnostic?.Invoke(Diagnostic.Create(
                        DefDiagnosticDescriptors.DEF003, Location.None, method.Name));
                    method.Exposed = ExposedScope.OwnClient;
                }
                comp.BaseMethods.Add(method);
            }
        }
    }

    private static ComponentLocality ParseComponentLocality(string? raw) => raw switch
    {
        "server" => ComponentLocality.ServerLocal,
        "client" => ComponentLocality.ClientLocal,
        _ => ComponentLocality.Synced,
    };

    // Property-scope ⊆ Component-scope: every observer who sees the
    // property must also see the component. Defensive subset check —
    // wider-than-component scopes are rejected at parse time.
    private static bool IsScopeSubset(PropertyScope inner, PropertyScope outer)
    {
        if (inner == outer) return true;
        // Observer set: AllClients ⊇ OwnClient ⊇ {} ; AllClients ⊇ OtherClients
        bool innerOwn = inner.IsOwnClientVisible();
        bool innerOther = inner.IsOtherClientsVisible();
        bool outerOwn = outer.IsOwnClientVisible();
        bool outerOther = outer.IsOtherClientsVisible();
        if (innerOwn && !outerOwn) return false;
        if (innerOther && !outerOther) return false;
        return true;
    }

    // Parses a standalone <component> root. Properties live on the
    // component type itself (referencable by name from any entity); no
    // slot index — that's per-entity-reference, assigned in the linker.
    private static ComponentDefModel? ParseStandaloneComponent(XElement root, string filePath,
                                                               Action<Diagnostic>? reportDiagnostic)
    {
        var typeName = root.Attribute("name")?.Value ?? "";
        if (string.IsNullOrWhiteSpace(typeName))
        {
            reportDiagnostic?.Invoke(Diagnostic.Create(
                DefDiagnosticDescriptors.DEF006, Location.None, filePath,
                "<component> requires non-empty name"));
            return null;
        }
        var comp = new ComponentDefModel
        {
            // Standalone components live on the type, not on a slot;
            // SlotName stays empty until an entity references it.
            TypeName = typeName,
            BaseTypeName = root.Attribute("extends")?.Value,
            Locality = ParseComponentLocality(root.Attribute("local")?.Value),
            IsStandalone = true,
        };

        var propsEl = root.Element("properties");
        if (propsEl != null)
        {
            var propNames = new HashSet<string>(StringComparer.Ordinal);
            foreach (var propEl in propsEl.Elements("property"))
            {
                var prop = ParseProperty(propEl, reportDiagnostic);
                if (prop is null) return null;
                if (!propNames.Add(prop.Name))
                {
                    reportDiagnostic?.Invoke(Diagnostic.Create(
                        DefDiagnosticDescriptors.DEF015, Location.None, typeName, prop.Name));
                    return null;
                }
                comp.Properties.Add(prop);
            }
        }

        // Standalone component RPC sections — same shape and validation as
        // inline. Slot index is per-entity-reference and resolved in the
        // linker; ComponentDefModel.SlotIdx stays -1 here.
        ParseComponentMethodSections(root, comp, typeName, reportDiagnostic);
        return comp;
    }

    private static PropertyDefModel? ParseProperty(XElement el,
                                                   Action<Diagnostic>? reportDiagnostic)
    {
        var scopeStr = el.Attribute("scope")?.Value ?? "cell_private";
        var typeText = el.Attribute("type")?.Value ?? "int32";
        var typeRef = DefTypeExprParser.Parse(typeText, reportDiagnostic);
        if (typeRef is null) return null;

        var prop = new PropertyDefModel
        {
            Name = el.Attribute("name")?.Value ?? "",
            Type = typeText,
            Scope = ParseScope(scopeStr),
            Persistent = el.Attribute("persistent")?.Value == "true",
            Reliable = el.Attribute("reliable")?.Value == "true",
        };

        if (typeRef.IsContainer)
        {
            prop.TypeRef = typeRef;
        }

        var maxSizeAttr = el.Attribute("max_size");
        if (maxSizeAttr != null && uint.TryParse(maxSizeAttr.Value, out var maxSize))
        {
            prop.MaxSize = maxSize;
        }

        return prop;
    }

    private static void ParseTypesSection(XElement typesEl, EntityDefModel model,
                                          Action<Diagnostic>? reportDiagnostic)
    {
        var declaredNames = new HashSet<string>(StringComparer.Ordinal);

        foreach (var structEl in typesEl.Elements("struct"))
        {
            var structName = structEl.Attribute("name")?.Value ?? "";
            if (string.IsNullOrWhiteSpace(structName)) continue;

            if (!declaredNames.Add(structName))
            {
                reportDiagnostic?.Invoke(Diagnostic.Create(
                    DefDiagnosticDescriptors.DEF012, Location.None, structName, model.Name));
                continue;
            }

            var sm = new StructDefModel
            {
                Name = structName,
                SyncMode = ParseStructSyncMode(structEl.Attribute("sync")?.Value),
            };
            foreach (var fieldEl in structEl.Elements("field"))
            {
                var fieldName = fieldEl.Attribute("name")?.Value ?? "";
                var fieldType = fieldEl.Attribute("type")?.Value ?? "int32";
                var typeRef = DefTypeExprParser.Parse(fieldType, reportDiagnostic);
                if (typeRef is null) return;
                sm.Fields.Add(new FieldDefModel { Name = fieldName, Type = typeRef });
            }
            model.Structs.Add(sm);
        }

        foreach (var aliasEl in typesEl.Elements("alias"))
        {
            var aliasName = aliasEl.Attribute("name")?.Value ?? "";
            var aliasType = aliasEl.Attribute("type")?.Value ?? "";
            if (string.IsNullOrWhiteSpace(aliasName) || string.IsNullOrWhiteSpace(aliasType))
                continue;

            if (!declaredNames.Add(aliasName))
            {
                reportDiagnostic?.Invoke(Diagnostic.Create(
                    DefDiagnosticDescriptors.DEF012, Location.None, aliasName, model.Name));
                continue;
            }

            var target = DefTypeExprParser.Parse(aliasType, reportDiagnostic);
            if (target is null) return;
            model.Aliases.Add(new AliasDefModel { Name = aliasName, Target = target });
        }
    }

    // Rejects A → B → A reference cycles among struct fields. We don't
    // attempt to resolve aliases into the graph — aliases point at
    // DataTypeRef trees, and DefLinker (a later step) is responsible for
    // expanding them. At the parser stage only direct struct → struct
    // references are known, which is exactly what can form a cycle.
    private static bool ValidateStructGraph(EntityDefModel model,
                                            Action<Diagnostic>? reportDiagnostic)
    {
        // Build name → struct map and adjacency list of struct-name edges.
        var byName = new Dictionary<string, StructDefModel>(StringComparer.Ordinal);
        foreach (var s in model.Structs) byName[s.Name] = s;

        // DFS with three-colour marking (0 = unseen, 1 = on stack, 2 = done).
        var colour = new Dictionary<string, int>(StringComparer.Ordinal);
        var stack = new List<string>();

        foreach (var root in model.Structs)
        {
            if (colour.TryGetValue(root.Name, out var c) && c == 2) continue;
            if (!Visit(root, byName, colour, stack, reportDiagnostic))
                return false;
        }
        return true;
    }

    private static bool Visit(StructDefModel s,
                              Dictionary<string, StructDefModel> byName,
                              Dictionary<string, int> colour,
                              List<string> stack,
                              Action<Diagnostic>? reportDiagnostic)
    {
        colour[s.Name] = 1;
        stack.Add(s.Name);

        foreach (var f in s.Fields)
        {
            foreach (var referenced in EnumerateStructRefs(f.Type))
            {
                if (!byName.TryGetValue(referenced, out var next)) continue;
                colour.TryGetValue(referenced, out var nc);
                if (nc == 1)
                {
                    var cycleStart = stack.IndexOf(referenced);
                    var cyclePath = cycleStart < 0
                        ? referenced
                        : string.Join(" → ", stack.GetRange(cycleStart, stack.Count - cycleStart))
                          + " → " + referenced;
                    reportDiagnostic?.Invoke(Diagnostic.Create(
                        DefDiagnosticDescriptors.DEF013, Location.None, referenced, cyclePath));
                    return false;
                }
                if (nc == 2) continue;
                if (!Visit(next, byName, colour, stack, reportDiagnostic))
                    return false;
            }
        }

        colour[s.Name] = 2;
        stack.RemoveAt(stack.Count - 1);
        return true;
    }

    private static IEnumerable<string> EnumerateStructRefs(DataTypeRefModel t)
    {
        if (t.Kind == PropertyDataKind.Struct && t.StructName is not null)
        {
            yield return t.StructName;
            yield break;
        }
        if (t.Elem is not null)
        {
            foreach (var s in EnumerateStructRefs(t.Elem)) yield return s;
        }
        if (t.Key is not null)
        {
            foreach (var s in EnumerateStructRefs(t.Key)) yield return s;
        }
    }

    private static MethodDefModel ParseMethod(XElement el,
                                              Action<Diagnostic>? reportDiagnostic = null)
    {
        var method = new MethodDefModel
        {
            Name = el.Attribute("name")?.Value ?? "",
            Exposed = ParseExposed(el.Attribute("exposed")?.Value),
        };

        var replyAttr = el.Attribute("reply")?.Value;
        if (!string.IsNullOrWhiteSpace(replyAttr))
        {
            method.Reply = replyAttr;
            method.ReplyTypeRef = DefTypeExprParser.Parse(replyAttr!, reportDiagnostic);
        }

        foreach (var argEl in el.Elements("arg"))
        {
            var typeText = argEl.Attribute("type")?.Value ?? "int32";
            var arg = new ArgDefModel
            {
                Name = argEl.Attribute("name")?.Value ?? "",
                Type = typeText,
            };
            // Containers / structs carry a resolvable TypeRef; scalar args
            // leave TypeRef null and the emitters use DefTypeHelper directly.
            var parsed = DefTypeExprParser.Parse(typeText, reportDiagnostic: null);
            if (parsed != null && IsCompositeKind(parsed.Kind))
                arg.TypeRef = parsed;
            method.Args.Add(arg);
        }

        return method;
    }

    private static bool IsCompositeKind(PropertyDataKind kind) =>
        kind == PropertyDataKind.List ||
        kind == PropertyDataKind.Dict ||
        kind == PropertyDataKind.Struct;

    private static StructSyncMode ParseStructSyncMode(string? value)
    {
        if (value is null) return StructSyncMode.Auto;
        return value.ToLowerInvariant() switch
        {
            "whole" => StructSyncMode.Whole,
            "field" => StructSyncMode.Field,
            _ => StructSyncMode.Auto,
        };
    }

    private static ExposedScope ParseExposed(string? value)
    {
        if (value == null)
            return ExposedScope.None;
        return value.ToLowerInvariant() switch
        {
            "own_client" => ExposedScope.OwnClient,
            "true" => ExposedScope.OwnClient,
            "all_clients" => ExposedScope.AllClients,
            _ => ExposedScope.None,
        };
    }

    // Replicable = anything the client can see. PropertyScopeExtensions is
    // the single source of truth; this indirection exists so DEF008 detection
    // (reserved-name "position" guard) uses the same predicate the emitters
    // use when deciding whether to emit a backing field at all.
    private static bool IsReplicableScope(PropertyScope scope) => scope.IsClientVisible();

    private static PropertyScope ParseScope(string value)
    {
        return value.ToLowerInvariant() switch
        {
            "cell_private" => PropertyScope.CellPrivate,
            "cell_public" => PropertyScope.CellPublic,
            "own_client" => PropertyScope.OwnClient,
            "other_clients" => PropertyScope.OtherClients,
            "all_clients" => PropertyScope.AllClients,
            "cell_public_and_own" => PropertyScope.CellPublicAndOwn,
            "base" => PropertyScope.Base,
            "base_and_client" => PropertyScope.BaseAndClient,
            _ => PropertyScope.CellPrivate,
        };
    }
}
