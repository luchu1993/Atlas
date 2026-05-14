using Atlas.DataTypes;
using Atlas.Diagnostics;
using Atlas.Entity;

namespace Atlas.Mvp.Base;

internal static class WorldBootstrap
{
    private const int kNpcCount = 50;
    // 200 × 200 m world; scattering matches the NpcAi clamp half-extent.
    private const float kWorldHalf = 100f;
    private static bool s_spawned;

    public static void EnsureSpawned(uint spaceId)
    {
        if (s_spawned) return;
        s_spawned = true;

        // Scatter at spawn time so the AoI Enter envelope carries the
        // scattered position; otherwise NPCs flash through (0,0,0) first.
        int requested = 0;
        for (int i = 0; i < kNpcCount; i++)
        {
            var pos = ScatterFor(i);
            if (EntityFactory.SpawnCellOnly("Npc", spaceId, pos, Vector3.Forward,
                                            onGround: false))
                requested++;
        }
        Log.Info(
            $"[Mvp.Base] WorldBootstrap: queued {requested}/{kNpcCount} NPC spawns in space {spaceId}");
    }

    private static Vector3 ScatterFor(int index)
    {
        uint h = (uint)(index + 1) * 2654435761u;
        float x = (h & 0xFFFFu) / 65535f * (kWorldHalf * 2f) - kWorldHalf;
        float z = ((h >> 16) & 0xFFFFu) / 65535f * (kWorldHalf * 2f) - kWorldHalf;
        return new Vector3(x, 0f, z);
    }
}
