using Atlas.Diagnostics;
using Xunit;

namespace Atlas.Tests;

public class TraceContextTests
{
    [Fact]
    public void Default_IsZero()
    {
        Assert.Equal(0L, TraceContext.Current);
    }

    [Fact]
    public void Push_RestoresOnDispose()
    {
        Assert.Equal(0L, TraceContext.Current);
        using (TraceContext.Push(0xCAFEL))
        {
            Assert.Equal(0xCAFEL, TraceContext.Current);
        }
        Assert.Equal(0L, TraceContext.Current);
    }

    [Fact]
    public void Push_NestsAndRestoresOuter()
    {
        using var outer = TraceContext.Push(0x1L);
        Assert.Equal(0x1L, TraceContext.Current);
        using (TraceContext.Push(0x2L))
        {
            Assert.Equal(0x2L, TraceContext.Current);
        }
        Assert.Equal(0x1L, TraceContext.Current);
    }

    [Fact]
    public void BeginInbound_NonZero_PassesThrough()
    {
        using var _ = TraceContext.BeginInbound(0x123L);
        Assert.Equal(0x123L, TraceContext.Current);
    }

    [Fact]
    public void BeginInbound_Zero_MintsFresh()
    {
        using var _ = TraceContext.BeginInbound(0L);
        Assert.NotEqual(0L, TraceContext.Current);
    }

    [Fact]
    public void SnowflakeGen_NextIsMonotonicWithinSecond()
    {
        long a = SnowflakeGen.Next();
        long b = SnowflakeGen.Next();
        Assert.NotEqual(a, b);
        Assert.True(b > a, $"expected b > a, got a=0x{a:X16} b=0x{b:X16}");
    }
}
