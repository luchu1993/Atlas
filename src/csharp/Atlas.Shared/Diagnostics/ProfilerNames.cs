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

        // Client-side dispatch zones. Names mirror the server's
        // Channel::HandleMessage path so end-to-end traces align —
        // a "ClientCallbacks.Dispatch*" zone follows the corresponding
        // server "Channel::Send" zone on the timeline once a viewer
        // can correlate the two processes.
        public const string ClientDispatchRpc = "ClientCallbacks.DispatchRpc";
        public const string ClientDispatchEnter = "ClientCallbacks.DispatchEnter";
        public const string ClientDispatchPropertyUpdate = "ClientCallbacks.DispatchPropertyUpdate";
        public const string ClientDispatchPositionUpdate = "ClientCallbacks.DispatchPositionUpdate";
        public const string ClientDispatchBaseline = "ClientCallbacks.DispatchBaseline";

        // Per-entity apply zones. These run inside the dispatch zone above
        // and break out the per-entity work from the routing layer.
        public const string ClientApplyOwnerSnapshot = "ClientEntity.ApplyOwnerSnapshot";
        public const string ClientApplyOtherSnapshot = "ClientEntity.ApplyOtherSnapshot";
        public const string ClientApplyReplicatedDelta = "ClientEntity.ApplyReplicatedDelta";
        public const string ClientApplyPositionUpdate = "ClientEntity.ApplyPositionUpdate";
    }
}
