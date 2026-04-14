using System;
using System.Collections.Generic;
using System.Xml.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Text;

namespace Atlas.Generators.Def;

internal static class DefParser
{
    public static EntityDefModel? Parse(SourceText text, string filePath,
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
        if (root == null || root.Name.LocalName != "entity")
        {
            reportDiagnostic?.Invoke(Diagnostic.Create(
                DefDiagnosticDescriptors.DEF006, Location.None, filePath, "Root element must be <entity>"));
            return null;
        }

        var model = new EntityDefModel
        {
            Name = root.Attribute("name")?.Value ?? ""
        };

        var idAttr = root.Attribute("id");
        if (idAttr != null && int.TryParse(idAttr.Value, out var id))
            model.ExplicitTypeId = id;

        // Properties
        var propsEl = root.Element("properties");
        if (propsEl != null)
        {
            foreach (var propEl in propsEl.Elements("property"))
            {
                model.Properties.Add(ParseProperty(propEl));
            }
        }

        // Client methods
        var clientMethodsEl = root.Element("client_methods");
        if (clientMethodsEl != null)
        {
            foreach (var methodEl in clientMethodsEl.Elements("method"))
            {
                var method = ParseMethod(methodEl);
                // Validate: client_methods must not have exposed
                if (method.Exposed != ExposedScope.None)
                {
                    reportDiagnostic?.Invoke(Diagnostic.Create(
                        DefDiagnosticDescriptors.DEF002, Location.None, method.Name));
                    method.Exposed = ExposedScope.None;
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
                model.CellMethods.Add(ParseMethod(methodEl));
            }
        }

        // Base methods
        var baseMethodsEl = root.Element("base_methods");
        if (baseMethodsEl != null)
        {
            foreach (var methodEl in baseMethodsEl.Elements("method"))
            {
                var method = ParseMethod(methodEl);
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

        return model;
    }

    private static PropertyDefModel ParseProperty(XElement el)
    {
        var scopeStr = el.Attribute("scope")?.Value ?? "cell_private";
        return new PropertyDefModel
        {
            Name = el.Attribute("name")?.Value ?? "",
            Type = el.Attribute("type")?.Value ?? "int32",
            Scope = ParseScope(scopeStr),
            Persistent = el.Attribute("persistent")?.Value == "true",
        };
    }

    private static MethodDefModel ParseMethod(XElement el)
    {
        var method = new MethodDefModel
        {
            Name = el.Attribute("name")?.Value ?? "",
            Exposed = ParseExposed(el.Attribute("exposed")?.Value),
        };

        foreach (var argEl in el.Elements("arg"))
        {
            method.Args.Add(new ArgDefModel
            {
                Name = argEl.Attribute("name")?.Value ?? "",
                Type = argEl.Attribute("type")?.Value ?? "int32",
            });
        }

        return method;
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
