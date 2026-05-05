namespace Atlas.Diagnostics;

// Zone names instrumented on both server and client must match literal-for-
// literal so the trace UIs line up when correlating across the network.
public static class ProfilerNames
{
    public const string ClientTick = "ClientTick";

    public const string ScriptOnTick = "Script.OnTick";
    public const string PublishReplicationFrame = "Script.PublishReplicationFrame";

    public const string ScriptSyncContextFlush = "Script.SyncContextFlush";
    public const string ScriptEntityTickAll = "Script.EntityTickAll";
    public const string ScriptPublishReplicationAll = "Script.PublishReplicationAll";
    public const string ScriptComponentTickAll = "Script.ComponentTickAll";

    public const string ClientDispatchRpc = "ClientCallbacks.DispatchRpc";
    public const string ClientDispatchEnter = "ClientCallbacks.DispatchEnter";
    public const string ClientDispatchPropertyUpdate = "ClientCallbacks.DispatchPropertyUpdate";
    public const string ClientDispatchPositionUpdate = "ClientCallbacks.DispatchPositionUpdate";
    public const string ClientDispatchBaseline = "ClientCallbacks.DispatchBaseline";

    public const string ClientApplyPositionUpdate = "ClientEntity.ApplyPositionUpdate";
}
