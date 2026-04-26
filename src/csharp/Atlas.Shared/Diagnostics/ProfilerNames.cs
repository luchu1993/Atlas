namespace Atlas.Diagnostics
{
    /// <summary>
    /// Shared zone-name constants. Anything instrumented on both server and
    /// client (e.g. damage application visible in server Tracy and client
    /// Unity Profiler) must use the same literal so the trace UIs line up
    /// when correlating across the network. Adding a typo on either side
    /// would silently produce two unrelated zones with similar names.
    /// </summary>
    public static class ProfilerNames
    {
        // Frame markers (per process). The server fills these via
        // ServerConfig.frame_name; the client uses ClientTick on its own
        // tick boundary.
        public const string ClientTick = "ClientTick";

        // Cross-side zones — keep these in sync with C++ literals in the
        // matching server subsystems.
        public const string ScriptOnTick = "Script.OnTick";
        public const string PublishReplicationFrame = "Script.PublishReplicationFrame";
    }
}
