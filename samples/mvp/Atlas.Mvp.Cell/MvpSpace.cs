using Atlas.DataTypes;
using Atlas.Diagnostics;
using Atlas.Entity;
using Atlas.Space;

namespace Atlas.Mvp.Cell;

[Entity("MvpSpace")]
public partial class MvpSpace : CellSpaceEntity
{
    private const int kInitialCount = 150;
    private const int kMaxCount = 250;
    private const int kLowWaterMark = 150;
    private const float kRespawnIntervalSeconds = 2.0f;
    private const float kWorldHalf = 100f;

    private int _scatterIndex;
    private int _liveCount;
    private long _timerHandle;

    protected override void OnSpaceInit(bool isReload)
    {
        if (isReload) return;

        int requested = 0;
        for (int i = 0; i < kInitialCount; i++)
        {
            if (TrySpawnOne()) requested++;
        }
        Log.Info($"[Mvp.Cell] MvpSpace: seeded {requested}/{kInitialCount} NPCs in space {SpaceId}");
        ArmTimer();
    }

    protected override void OnSpaceDestroy()
    {
        if (_timerHandle != 0) { CancelTimer(_timerHandle); _timerHandle = 0; }
    }

    internal void NotifyNpcAlive()
    {
        ++_liveCount;
        PublishCount();
    }

    internal void NotifyNpcDead()
    {
        if (_liveCount > 0) --_liveCount;
        PublishCount();
        if (_liveCount <= kLowWaterMark) ArmTimer();
    }

    private void OnTimer()
    {
        _timerHandle = 0;
        if (_liveCount >= kMaxCount) return;
        TrySpawnOne();
        if (_liveCount < kMaxCount) ArmTimer();
    }

    private void ArmTimer()
    {
        if (_timerHandle != 0) return;
        _timerHandle = StartTimer(kRespawnIntervalSeconds, /*repeat=*/false, OnTimer);
    }

    private bool TrySpawnOne()
    {
        var pos = ScatterFor(_scatterIndex++);
        // Vector3.Zero so NpcAi.OnAttached picks a random first target; a non-zero
        // default would make every fresh NPC walk the same axis until first retarget.
        return EntityFactory.CreateLocalCell("Npc", SpaceId, pos, Vector3.Zero,
                                             onGround: false) != null;
    }

    private void PublishCount() =>
        SpaceData.SetInt32(SpaceId, SpaceDataKeys.NpcCount, _liveCount);

    private static Vector3 ScatterFor(int index)
    {
        uint h = (uint)(index + 1) * 2654435761u;
        float x = (h & 0xFFFFu) / 65535f * (kWorldHalf * 2f) - kWorldHalf;
        float z = ((h >> 16) & 0xFFFFu) / 65535f * (kWorldHalf * 2f) - kWorldHalf;
        return new Vector3(x, 0f, z);
    }
}
