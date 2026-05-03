using System.Collections.Generic;
using System.Text;
using System.Text.RegularExpressions;

namespace Atlas.Generators.Tests;

internal static class TestManifest
{
    private static readonly Regex EntityNameRegex =
        new(@"<entity\s+name=""([^""]+)""", RegexOptions.Compiled);

    public static string Derive(string defXml)
    {
        var sb = new StringBuilder();
        sb.Append("<entity_ids>");
        int id = 1;
        var seen = new HashSet<string>();
        foreach (Match m in EntityNameRegex.Matches(defXml))
        {
            var name = m.Groups[1].Value;
            if (!seen.Add(name)) continue;
            sb.Append($"<entity name=\"{name}\" id=\"{id++}\"/>");
        }
        sb.Append("</entity_ids>");
        return sb.ToString();
    }
}
