using System;
using System.IO;
using Atlas.Hosting;
using Xunit;

namespace Atlas.Tests;

[Collection("AppEvents")]
public class ScriptHostTests
{
    private const string kMarkerEnv = "ATLAS_SCRIPT_HOST_FIXTURE_MARKER";
    private const string kDeferEnv = "ATLAS_SCRIPT_HOST_FIXTURE_DEFER_ONCE";

    [Fact]
    public void InitiallyNotLoaded()
    {
        var host = new ScriptHost();
        Assert.False(host.IsLoaded);
        Assert.Null(host.ScriptAssembly);
    }

    [Fact]
    public void UnloadWhenNotLoaded_ReturnsTrue()
    {
        var host = new ScriptHost();
        Assert.True(host.Unload(TimeSpan.FromSeconds(1)));
    }

    [Fact]
    public void Load_BadPath_Throws()
    {
        var host = new ScriptHost();
        Assert.ThrowsAny<Exception>(() => host.Load("nonexistent.dll"));
    }

    [Fact]
    public void Dispose_WhenNotLoaded_NoThrow()
    {
        var host = new ScriptHost();
        host.Dispose();
    }

    [Fact]
    public void Load_RegisterAppInitializer_FiresOnNextAppInitTick()
    {
        AppEvents.Reset();
        var marker = NewMarkerPath();
        WithMarker(marker, deferOnce: false, () =>
        {
            using var host = LoadFixture();
            Assert.False(File.Exists(marker));
            AppEvents.TryFireAppInit();
            Assert.Equal("1", File.ReadAllText(marker).Trim());
        });
    }

    [Fact]
    public void Load_AfterAppInitFired_WaitsForNextTick()
    {
        AppEvents.Reset();
        AppEvents.TryFireAppInit();
        var marker = NewMarkerPath();
        WithMarker(marker, deferOnce: false, () =>
        {
            using var host = LoadFixture();
            Assert.False(File.Exists(marker));
            AppEvents.TryFireAppInit();
            Assert.Equal("1", File.ReadAllText(marker).Trim());
        });
    }

    [Fact]
    public void Load_AfterAppInitFired_CanDefer()
    {
        AppEvents.Reset();
        AppEvents.TryFireAppInit();
        var marker = NewMarkerPath();
        WithMarker(marker, deferOnce: true, () =>
        {
            using var host = LoadFixture();
            AppEvents.TryFireAppInit();
            Assert.Equal("1", File.ReadAllText(marker).Trim());
            AppEvents.TryFireAppInit();
            AppEvents.TryFireAppInit();
            Assert.Equal($"1{Environment.NewLine}2", File.ReadAllText(marker).Trim());
        });
    }

    [Fact]
    public void Unload_BeforeAppInit_RemovesInitializer()
    {
        AppEvents.Reset();
        var marker = NewMarkerPath();
        WithMarker(marker, deferOnce: false, () =>
        {
            var host = LoadFixture();
            host.Unload(TimeSpan.FromSeconds(1));
            AppEvents.TryFireAppInit();
            Assert.False(File.Exists(marker));
        });
    }

    private static ScriptHost LoadFixture()
    {
        var path = Path.Combine(AppContext.BaseDirectory, "Atlas.Runtime.ScriptHostFixture.dll");
        var host = new ScriptHost();
        host.Load(path);
        return host;
    }

    private static string NewMarkerPath() =>
        Path.Combine(Path.GetTempPath(), $"atlas-script-host-{Guid.NewGuid():N}.txt");

    private static void WithMarker(string path, bool deferOnce, Action action)
    {
        Environment.SetEnvironmentVariable(kMarkerEnv, path);
        Environment.SetEnvironmentVariable(kDeferEnv, deferOnce ? "1" : null);
        try { action(); }
        finally
        {
            Environment.SetEnvironmentVariable(kMarkerEnv, null);
            Environment.SetEnvironmentVariable(kDeferEnv, null);
            try { File.Delete(path); } catch { }
            AppEvents.Reset();
        }
    }
}
