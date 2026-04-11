using System;
using Atlas.Hosting;
using Xunit;

namespace Atlas.Tests;

public class ScriptHostTests
{
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
}
