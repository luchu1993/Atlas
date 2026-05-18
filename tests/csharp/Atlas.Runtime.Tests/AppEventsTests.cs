using System;
using System.Reflection;
using Xunit;

namespace Atlas.Tests;

// AppEvents holds static state; reset it between tests so order doesn't matter.
public class AppEventsTests : IDisposable
{
    public AppEventsTests() => Reset();
    public void Dispose() => Reset();

    [Fact]
    public void TryFire_InvokesHandlerOnce()
    {
        int calls = 0;
        AppEvents.AppInit += () => calls++;
        AppEvents.TryFireAppInit();
        AppEvents.TryFireAppInit();
        Assert.Equal(1, calls);
    }

    [Fact]
    public void DeferAppInit_RefiresHandlerNextTick()
    {
        int calls = 0;
        AppEvents.AppInit += () =>
        {
            calls++;
            if (calls == 1) AppEvents.DeferAppInit();
        };
        AppEvents.TryFireAppInit();
        AppEvents.TryFireAppInit();
        AppEvents.TryFireAppInit();
        Assert.Equal(2, calls);
    }

    [Fact]
    public void NoHandlers_FireIsNoop()
    {
        AppEvents.TryFireAppInit();
        AppEvents.TryFireAppInit();
    }

    // AppEvents has no public reset; clear handler list + flags via reflection
    // so each test starts from a clean slate.
    private static void Reset()
    {
        var t = typeof(AppEvents);
        var ev = t.GetField("AppInit",
            BindingFlags.Static | BindingFlags.NonPublic | BindingFlags.Public);
        ev?.SetValue(null, null);
        t.GetField("s_appInitFired", BindingFlags.Static | BindingFlags.NonPublic)
            ?.SetValue(null, false);
        t.GetField("s_appInitDeferred", BindingFlags.Static | BindingFlags.NonPublic)
            ?.SetValue(null, false);
    }
}
