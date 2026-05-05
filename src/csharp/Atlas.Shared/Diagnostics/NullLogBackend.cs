namespace Atlas.Diagnostics;

public sealed class NullLogBackend : ILogBackend
{
    public static readonly NullLogBackend Instance = new();

    private NullLogBackend() { }

    public void Log(int level, string message) { }
}
