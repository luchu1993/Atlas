using System;

namespace Atlas.Client.Native;

// Hosts implement this to observe transport-level events. Session-level code
// decodes AoI envelopes and entity lifecycle after OnDeliver forwards payloads.
public interface IAtlasNetEvents
{
    void OnDisconnect(int reason);

    // Forwarded raw from net_client.dll's on_deliver. msgId is the wire id;
    // payload is a view valid only for the call.
    void OnDeliver(ushort msgId, ReadOnlySpan<byte> payload);
}
