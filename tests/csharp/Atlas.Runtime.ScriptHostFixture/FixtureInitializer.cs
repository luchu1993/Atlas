using System;
using System.IO;
using Atlas;

namespace Atlas.Runtime.ScriptHostFixture;

public sealed class FixtureInitializer : IAtlasAppInitializer
{
    private static int s_calls;

    public void OnAppInit()
    {
        int calls = ++s_calls;
        var marker = Environment.GetEnvironmentVariable("ATLAS_SCRIPT_HOST_FIXTURE_MARKER");
        if (!string.IsNullOrEmpty(marker))
            File.AppendAllText(marker, calls + Environment.NewLine);
        if (calls == 1 &&
            Environment.GetEnvironmentVariable("ATLAS_SCRIPT_HOST_FIXTURE_DEFER_ONCE") == "1")
            AppEvents.DeferAppInit();
    }
}
