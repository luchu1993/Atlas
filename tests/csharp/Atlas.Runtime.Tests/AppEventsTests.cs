using System;
using Xunit;

namespace Atlas.Tests;

public class AppEventsTests : IDisposable
{
    private sealed class TestInitializer : IAtlasAppInitializer
    {
        public int Calls;
        public bool DeferFirstCall;

        public void OnAppInit()
        {
            Calls++;
            if (DeferFirstCall && Calls == 1) AppEvents.DeferAppInit();
        }
    }

    public AppEventsTests() => AppEvents.Reset();
    public void Dispose() => AppEvents.Reset();

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

    [Fact]
    public void RegisteredScriptInitializer_FiresOnce()
    {
        var initializer = new TestInitializer();
        AppEvents.RegisterScriptInitializer(initializer);
        AppEvents.TryFireAppInit();
        AppEvents.TryFireAppInit();
        Assert.Equal(1, initializer.Calls);
    }

    [Fact]
    public void RegisteredScriptInitializer_CanDeferAppInit()
    {
        var initializer = new TestInitializer { DeferFirstCall = true };
        AppEvents.RegisterScriptInitializer(initializer);
        AppEvents.TryFireAppInit();
        AppEvents.TryFireAppInit();
        AppEvents.TryFireAppInit();
        Assert.Equal(2, initializer.Calls);
    }

    [Fact]
    public void UnregisteredScriptInitializer_DoesNotFire()
    {
        var initializer = new TestInitializer();
        AppEvents.RegisterScriptInitializer(initializer);
        AppEvents.UnregisterScriptInitializer(initializer);
        AppEvents.TryFireAppInit();
        Assert.Equal(0, initializer.Calls);
    }

    [Fact]
    public void RegisterAfterFire_InvokesImmediately()
    {
        AppEvents.TryFireAppInit();
        var initializer = new TestInitializer();
        AppEvents.RegisterScriptInitializer(initializer);
        Assert.Equal(1, initializer.Calls);
    }
}
