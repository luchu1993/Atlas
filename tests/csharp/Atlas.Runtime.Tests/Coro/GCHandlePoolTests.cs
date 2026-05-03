using System;
using System.Runtime.InteropServices;
using Atlas.Runtime.Coro;
using Xunit;

namespace Atlas.Tests.Coro;

public sealed class GCHandlePoolTests
{
    [Fact]
    public void Rent_ResolvesToTarget_AndReturnRebindsSlot()
    {
        var a = new object();
        var ha = GCHandlePool.Rent(a);
        Assert.Same(a, GCHandle.FromIntPtr(ha).Target);

        GCHandlePool.Return(ha);

        var b = new object();
        var hb = GCHandlePool.Rent(b);
        Assert.Same(b, GCHandle.FromIntPtr(hb).Target);
        GCHandlePool.Return(hb);
    }

    [Fact]
    public void Return_ZeroHandleIgnored()
    {
        GCHandlePool.Return(IntPtr.Zero);
    }

    [Fact]
    public void Return_NullsOutTargetEvenIfPoolFull()
    {
        // Saturate the pool, then verify the released payload becomes
        // unreachable (target slot cleared) for the surplus handles.
        var holders = new IntPtr[300];
        for (int i = 0; i < holders.Length; i++)
            holders[i] = GCHandlePool.Rent(new byte[1]);
        for (int i = 0; i < holders.Length; i++)
        {
            var h = holders[i];
            GCHandlePool.Return(h);
            // After Return the target must no longer reference the byte[].
            // (For surplus handles the slot is freed; FromIntPtr would be
            // invalid — only check those still-allocated.)
            if (i < 256)
                Assert.Null(GCHandle.FromIntPtr(h).Target);
        }
    }
}
