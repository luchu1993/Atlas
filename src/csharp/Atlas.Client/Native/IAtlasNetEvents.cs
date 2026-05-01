using System;

namespace Atlas.Client.Native
{
    public interface IAtlasNetEvents
    {
        void OnDisconnect(int reason);
        void OnPlayerBaseCreate(uint entityId, ushort typeId, ReadOnlySpan<byte> baseProps);
        void OnPlayerCellCreate(uint spaceId, float px, float py, float pz,
                                float dx, float dy, float dz,
                                ReadOnlySpan<byte> cellProps);
        void OnResetEntities();
        void OnEntityEnter(uint entityId, ushort typeId,
                           float px, float py, float pz,
                           float dx, float dy, float dz,
                           ReadOnlySpan<byte> properties);
        void OnEntityLeave(uint entityId);
        void OnEntityPosition(uint entityId,
                              float px, float py, float pz,
                              float dx, float dy, float dz,
                              bool onGround);
        void OnEntityProperty(uint entityId, byte scope, ReadOnlySpan<byte> delta);
        void OnForcedPosition(uint entityId,
                              float px, float py, float pz,
                              float dx, float dy, float dz);
        void OnRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload);
    }
}
