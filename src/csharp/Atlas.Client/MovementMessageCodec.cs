using System;
using Atlas.DataTypes;
using Atlas.Serialization;

namespace Atlas.Client;

internal static class MovementMessageCodec
{
    public static bool TryDecodeStateAck(ReadOnlySpan<byte> body, out MovementStateAck ack,
                                         out string error)
    {
        ack = default;

        try
        {
            var reader = new SpanReader(body);
            uint entityId = reader.ReadUInt32();
            if (entityId == 0)
            {
                error = "MovementStateAck: invalid entity";
                return false;
            }
            uint ackedInputSeq = reader.ReadUInt32();
            uint serverTick = reader.ReadUInt32();
            if (!TryReadMovementState(ref reader, "MovementStateAck", out var state, out error))
                return false;
            ushort correctionFlags = reader.ReadUInt16();
            if (!MovementCorrection.IsFlagsValid(correctionFlags))
            {
                error = $"MovementStateAck: invalid flags={correctionFlags}";
                return false;
            }
            if (!TryFinish(ref reader, "MovementStateAck", out error)) return false;
            ack = new MovementStateAck(entityId, ackedInputSeq, serverTick, state,
                                       correctionFlags);
            error = string.Empty;
            return true;
        }
        catch (InvalidOperationException ex)
        {
            error = $"MovementStateAck: truncated ({ex.Message})";
            return false;
        }
    }

    public static bool TryDecodeCommandStart(ReadOnlySpan<byte> body,
                                             out MovementCommandStart start,
                                             out string error)
    {
        start = default;

        try
        {
            var reader = new SpanReader(body);
            uint entityId = reader.ReadUInt32();
            if (entityId == 0)
            {
                error = "MovementCommandStart: invalid entity";
                return false;
            }
            uint commandId = reader.ReadUInt32();
            if (commandId == 0)
            {
                error = "MovementCommandStart: invalid command id";
                return false;
            }
            ushort skillId = reader.ReadUInt16();
            byte typeValue = reader.ReadUInt8();
            if (!IsCommandType(typeValue))
            {
                error = $"MovementCommandStart: invalid type={typeValue}";
                return false;
            }
            var startPosition = reader.ReadVector3();
            var targetPosition = reader.ReadVector3();
            if (!IsFinite(startPosition) || !IsFinite(targetPosition))
            {
                error = "MovementCommandStart: non-finite position";
                return false;
            }
            ushort durationMs = reader.ReadUInt16();
            ushort elapsedMs = reader.ReadUInt16();
            if (durationMs == 0 || elapsedMs > durationMs)
            {
                error = "MovementCommandStart: invalid timing";
                return false;
            }
            ushort curveId = reader.ReadUInt16();
            byte inputPolicyValue = reader.ReadUInt8();
            byte collisionPolicyValue = reader.ReadUInt8();
            if (!IsInputPolicy(inputPolicyValue) || !IsCollisionPolicy(collisionPolicyValue))
            {
                error = "MovementCommandStart: invalid policy";
                return false;
            }
            byte priority = reader.ReadUInt8();
            uint serverTick = reader.ReadUInt32();
            if (!TryFinish(ref reader, "MovementCommandStart", out error)) return false;
            var command = new ClientMovementCommand(
                commandId, skillId, (MovementCommandType)typeValue, startPosition,
                targetPosition, durationMs, elapsedMs, curveId,
                (MovementCommandInputPolicy)inputPolicyValue,
                (MovementCommandCollisionPolicy)collisionPolicyValue, priority, serverTick);
            start = new MovementCommandStart(entityId, command);
            error = string.Empty;
            return true;
        }
        catch (InvalidOperationException ex)
        {
            error = $"MovementCommandStart: truncated ({ex.Message})";
            return false;
        }
    }

    public static bool TryDecodeCommandEnd(ReadOnlySpan<byte> body,
                                           out MovementCommandEnd end,
                                           out string error)
    {
        end = default;

        try
        {
            var reader = new SpanReader(body);
            uint entityId = reader.ReadUInt32();
            if (entityId == 0)
            {
                error = "MovementCommandEnd: invalid entity";
                return false;
            }
            uint commandId = reader.ReadUInt32();
            if (commandId == 0)
            {
                error = "MovementCommandEnd: invalid command id";
                return false;
            }
            uint serverTick = reader.ReadUInt32();
            byte reasonValue = reader.ReadUInt8();
            if (!IsEndReason(reasonValue))
            {
                error = $"MovementCommandEnd: invalid reason={reasonValue}";
                return false;
            }
            if (!TryReadMovementState(ref reader, "MovementCommandEnd", out var state,
                                      out error))
                return false;
            if (!TryFinish(ref reader, "MovementCommandEnd", out error)) return false;
            end = new MovementCommandEnd(entityId, commandId, serverTick, state,
                                         (MovementCommandEndReason)reasonValue);
            error = string.Empty;
            return true;
        }
        catch (InvalidOperationException ex)
        {
            error = $"MovementCommandEnd: truncated ({ex.Message})";
            return false;
        }
    }

    static bool TryReadMovementState(ref SpanReader reader, string label, out MovementState state,
                                     out string error)
    {
        var position = reader.ReadVector3();
        var velocity = reader.ReadVector3();
        var direction = reader.ReadVector3();
        uint flags = reader.ReadUInt32();
        uint lastProcessedInputSeq = reader.ReadUInt32();
        state = new MovementState(position, velocity, direction, flags, lastProcessedInputSeq);
        if (IsFinite(state))
        {
            error = string.Empty;
            return true;
        }
        error = $"{label}: non-finite state";
        return false;
    }

    static bool TryFinish(ref SpanReader reader, string label, out string error)
    {
        if (reader.Remaining == 0)
        {
            error = string.Empty;
            return true;
        }
        error = $"{label}: trailing bytes={reader.Remaining}";
        return false;
    }

    static bool IsCommandType(byte value) => value <= (byte)MovementCommandType.FollowEntity;

    static bool IsInputPolicy(byte value) =>
        value <= (byte)MovementCommandInputPolicy.AllowFull;

    static bool IsCollisionPolicy(byte value) =>
        value <= (byte)MovementCommandCollisionPolicy.EndSkill;

    static bool IsEndReason(byte value) => value <= (byte)MovementCommandEndReason.Invalid;

    static bool IsFinite(MovementState state) =>
        IsFinite(state.Position) && IsFinite(state.Velocity) && IsFinite(state.Direction);

    static bool IsFinite(Vector3 value) =>
        !float.IsNaN(value.X) && !float.IsInfinity(value.X) &&
        !float.IsNaN(value.Y) && !float.IsInfinity(value.Y) &&
        !float.IsNaN(value.Z) && !float.IsInfinity(value.Z);
}
