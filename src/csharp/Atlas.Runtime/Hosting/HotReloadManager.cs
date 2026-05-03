using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using Atlas.Core;
using Atlas.Coro;
using Atlas.Coro.Hosting;
using Atlas.Entity;
using Atlas.Serialization;

namespace Atlas.Hosting;

/// <summary>
/// C#-side hot reload coordinator. Provides [UnmanagedCallersOnly] entry points
/// for C++ ClrHotReload to call during the reload sequence.
/// </summary>
internal static class HotReloadManager
{
    private static byte[]? _stateSnapshot;
    private static ScriptHost? _scriptHost;

    /// <summary>Current script host (set after first load).</summary>
    internal static ScriptHost? CurrentHost => _scriptHost;

    // Caps drain so a runaway continuation chain can't stall reload.
    private const int kDrainPasses = 16;

    /// <summary>
    /// Serializes all entity state and unloads the script assembly. First half
    /// of hot-reload; caller must follow up with LoadAndRestore to bring the
    /// process back to a runnable state. Called by C++ via ClrHotReload::do_reload().
    /// </summary>
    [UnmanagedCallersOnly]
    public static int SerializeAndUnload()
    {
        try
        {
            var entities = EntityManager.Instance.GetAllEntities();

            CancelLifecyclesAndDrain(entities);

            var writer = new SpanWriter(64 * 1024);
            try
            {
                writer.WriteInt32(entities.Count);
                foreach (var entity in entities)
                {
                    writer.WriteString(entity.TypeName);
                    writer.WriteUInt32(entity.EntityId);

                    // Serialize to a temp buffer so we can prefix the byte length.
                    // This allows SkipEntityData to skip deleted entity types.
                    var entityWriter = new SpanWriter(4096);
                    try
                    {
                        entity.Serialize(ref entityWriter);
                        writer.WriteInt32(entityWriter.Length);
                        if (entityWriter.Length > 0)
                        {
                            writer.WriteRawBytes(entityWriter.WrittenSpan);
                        }
                    }
                    finally { entityWriter.Dispose(); }
                }
                _stateSnapshot = writer.WrittenSpan.ToArray();
            }
            finally { writer.Dispose(); }

            EntityManager.Instance.Clear();
            _scriptHost?.Unload(TimeSpan.FromSeconds(5));
            _scriptHost = null;

            Log.Info($"Hot reload: serialized {entities.Count} entities ({_stateSnapshot.Length} bytes)");
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    /// <summary>
    /// Loads a new script assembly and restores entity state from the snapshot
    /// captured by SerializeAndUnload. Second half of hot-reload; the process
    /// is unusable between the two calls. Called by C++ via ClrHotReload::do_reload().
    /// </summary>
    [UnmanagedCallersOnly]
    public static unsafe int LoadAndRestore(byte* pathUtf8, int pathLen)
    {
        try
        {
            var path = Encoding.UTF8.GetString(new ReadOnlySpan<byte>(pathUtf8, pathLen));

            _scriptHost = new ScriptHost();
            _scriptHost.Load(path);

            if (_stateSnapshot != null && _stateSnapshot.Length > 0)
            {
                RestoreEntities(_stateSnapshot);
                _stateSnapshot = null;
            }

            Log.Info("Hot reload: new assembly loaded and state restored");
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    /// <summary>
    /// Initial script load (not a hot-reload). Called by C++ ClrScriptEngine::load_module().
    /// </summary>
    [UnmanagedCallersOnly]
    public static unsafe int LoadScripts(byte* pathUtf8, int pathLen)
    {
        try
        {
            var path = Encoding.UTF8.GetString(new ReadOnlySpan<byte>(pathUtf8, pathLen));
            _scriptHost = new ScriptHost();
            _scriptHost.Load(path);
            Log.Info($"Loaded game scripts from: {path}");
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    internal static void CancelLifecyclesAndDrain(IReadOnlyCollection<ServerEntity> entities)
    {
        foreach (var entity in entities)
            entity.TriggerLifecycleCancellation();
        if (AtlasLoop.Current is not ManagedAtlasLoop loop) return;
        for (int i = 0; i < kDrainPasses; i++)
        {
            if (loop.Drain() == 0) return;
        }
        Log.Warning($"Hot reload: continuation queue still draining after {kDrainPasses} passes; user code likely awaiting an RPC without LifecycleCancellation");
    }

    private static void RestoreEntities(byte[] snapshot)
    {
        var reader = new SpanReader(snapshot);
        var count = reader.ReadInt32();
        int restored = 0;
        int failed = 0;

        for (int i = 0; i < count; i++)
        {
            var typeName = reader.ReadString();
            var entityId = reader.ReadUInt32();
            var dataLength = reader.ReadInt32();

            var entity = EntityFactory.Create(typeName);
            if (entity == null)
            {
                Log.Warning($"Hot reload: entity type '{typeName}' no longer exists, skipping entity {entityId}");
                reader.Advance(dataLength);
                failed++;
                continue;
            }

            entity.EntityId = entityId;
            var startPos = reader.Position;
            try
            {
                entity.Deserialize(ref reader);

                // Ensure we consumed exactly dataLength bytes
                int consumed = reader.Position - startPos;
                if (consumed < dataLength)
                    reader.Advance(dataLength - consumed);

                EntityManager.Instance.Register(entity);
                restored++;
            }
            catch (Exception ex)
            {
                Log.Warning($"Hot reload: failed to deserialize entity '{typeName}' (id={entityId}): {ex.Message}. Using defaults.");

                // Skip remaining data for this entity
                int consumed = reader.Position - startPos;
                if (consumed < dataLength)
                    reader.Advance(dataLength - consumed);

                EntityManager.Instance.Register(entity);
                failed++;
            }
        }

        Log.Info($"Hot reload: restored {restored} entities, {failed} failed/skipped");
    }
}
