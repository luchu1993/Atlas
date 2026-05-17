using System;
using System.Collections.Generic;
using System.Xml.Linq;
using Microsoft.CodeAnalysis;

namespace Atlas.Generators.Def;

internal static class EntityIdManifestParser
{
    public const string FileName = "entity_ids.xml";
    public const int kMinId = 1;
    public const int kMaxId = 0x3FFF;

    public static EntityIdManifest? Parse(string xml, string sourcePath,
                                          Action<Diagnostic>? reportDiagnostic)
    {
        XDocument doc;
        try
        {
            doc = XDocument.Parse(xml);
        }
        catch (Exception ex)
        {
            reportDiagnostic?.Invoke(Diagnostic.Create(
                DefDiagnosticDescriptors.DEF025, Location.None, sourcePath, ex.Message));
            return null;
        }

        var root = doc.Root;
        if (root == null || root.Name.LocalName != "entity_ids")
        {
            reportDiagnostic?.Invoke(Diagnostic.Create(
                DefDiagnosticDescriptors.DEF025, Location.None, sourcePath,
                "Root element must be <entity_ids>"));
            return null;
        }

        var active = new Dictionary<string, ushort>(StringComparer.Ordinal);
        var deprecatedIds = new HashSet<ushort>();
        var deprecatedNames = new HashSet<string>(StringComparer.Ordinal);
        var ownerById = new Dictionary<ushort, string>();
        var seenNames = new HashSet<string>(StringComparer.Ordinal);

        foreach (var el in root.Elements("entity"))
        {
            var name = el.Attribute("name")?.Value;
            if (name == null || string.IsNullOrWhiteSpace(name))
            {
                reportDiagnostic?.Invoke(Diagnostic.Create(
                    DefDiagnosticDescriptors.DEF025, Location.None, sourcePath,
                    "<entity> entry missing name attribute"));
                continue;
            }

            var idStr = el.Attribute("id")?.Value;
            if (idStr == null || !int.TryParse(idStr, out var idValue))
            {
                reportDiagnostic?.Invoke(Diagnostic.Create(
                    DefDiagnosticDescriptors.DEF019, Location.None, name));
                continue;
            }
            if (idValue < kMinId || idValue > kMaxId)
            {
                reportDiagnostic?.Invoke(Diagnostic.Create(
                    DefDiagnosticDescriptors.DEF020, Location.None, name, idValue));
                continue;
            }

            var id = (ushort)idValue;
            var deprecated = string.Equals(el.Attribute("deprecated")?.Value, "true",
                                           StringComparison.OrdinalIgnoreCase);

            if (!seenNames.Add(name))
            {
                reportDiagnostic?.Invoke(Diagnostic.Create(
                    DefDiagnosticDescriptors.DEF026, Location.None, name, sourcePath));
                continue;
            }
            if (ownerById.TryGetValue(id, out var prev))
            {
                reportDiagnostic?.Invoke(Diagnostic.Create(
                    DefDiagnosticDescriptors.DEF021, Location.None, name, id, prev));
                continue;
            }

            ownerById[id] = name;
            if (deprecated)
            {
                deprecatedIds.Add(id);
                deprecatedNames.Add(name);
            }
            else
            {
                active[name] = id;
            }
        }

        return new EntityIdManifest(sourcePath, active, deprecatedIds, deprecatedNames);
    }
}
