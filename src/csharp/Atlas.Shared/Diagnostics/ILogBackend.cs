namespace Atlas.Diagnostics
{
    public interface ILogBackend
    {
        // Levels: 0=Trace 1=Debug 2=Info 3=Warning 4=Error 5=Critical.
        void Log(int level, string message);
    }
}
