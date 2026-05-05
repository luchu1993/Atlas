using System;

namespace Atlas.Client.Native;

// Hosts (Unity AtlasNetworkManager, Desktop bootstrap, integration tests)
// implement this to observe transport-level events. AoI envelope decode and
// entity lifecycle live in Atlas.Client.ClientCallbacks.DeliverFromServer.
public interface IAtlasNetEvents
{
    void OnDisconnect(int reason);

    // Forwarded raw from net_client.dll's on_deliver. msgId is the wire id;
    // payload is a view valid only for the call.
    void OnDeliver(ushort msgId, ReadOnlySpan<byte> payload);
}
