using System;
using Atlas.DataTypes;
using Atlas.Shared.Protocol;

namespace Atlas.Client;

internal sealed class MovementCommandPlayback
{
    const float kMinDirectionLengthSq = 0.0001f;

    readonly IMovementCurveSampler _curves;
    ClientMovementCommand _command;
    uint _elapsedMs;

    public MovementCommandPlayback(IMovementCurveSampler? curves = null)
    {
        _curves = curves ?? MovementCurves.Default;
    }

    public bool IsActive { get; private set; }
    public Vector3 Position { get; private set; }
    public uint CommandId => _command.CommandId;
    public MovementCommandInputPolicy InputPolicy => _command.InputPolicy;
    public bool AllowsTurnInput => InputPolicy == MovementCommandInputPolicy.AllowTurn;

    public bool Start(ClientMovementCommand command)
    {
        if (!IsCommandValid(command)) return false;

        _command = command;
        _elapsedMs = command.ElapsedMs;
        IsActive = true;
        Position = SamplePosition(command, _elapsedMs);
        if (_elapsedMs >= _command.DurationMs) IsActive = false;
        return true;
    }

    public void Clear()
    {
        _command = default;
        _elapsedMs = 0;
        IsActive = false;
        Position = Vector3.Zero;
    }

    public void AdvanceMs(uint deltaMs)
    {
        if (!IsActive) return;

        uint nextElapsedMs = _elapsedMs + deltaMs;
        _elapsedMs = nextElapsedMs > _command.DurationMs ? _command.DurationMs : nextElapsedMs;
        Position = SamplePosition(_command, _elapsedMs);
        if (_elapsedMs >= _command.DurationMs) IsActive = false;
    }

    public bool TryGetDirection(out Vector3 direction)
    {
        direction = _command.TargetPosition - _command.StartPosition;
        if (direction.LengthSquared <= kMinDirectionLengthSq)
        {
            direction = default;
            return false;
        }

        direction = direction.Normalized;
        return true;
    }

    public void AlignToPosition(Vector3 position)
    {
        if (!IsActive) return;

        Vector3 path = _command.TargetPosition - _command.StartPosition;
        float pathLengthSq = path.LengthSquared;
        if (pathLengthSq <= kMinDirectionLengthSq) return;

        float progress = Vector3.Dot(position - _command.StartPosition, path) / pathLengthSq;
        float normalizedTime = _curves.TimeAtProgress(_command.CurveId, progress);
        uint elapsedMs = (uint)MathF.Round(normalizedTime * _command.DurationMs);
        if (elapsedMs > _elapsedMs)
        {
            _elapsedMs = elapsedMs;
            Position = SamplePosition(_command, _elapsedMs);
        }
        if (_elapsedMs >= _command.DurationMs) IsActive = false;
    }

    static bool IsCommandValid(ClientMovementCommand command) =>
        command.CommandId != 0 &&
        command.DurationMs > 0 && command.ElapsedMs <= command.DurationMs &&
        IsSupportedInputPolicy(command.InputPolicy) &&
        IsFinite(command.StartPosition) && IsFinite(command.TargetPosition);

    static bool IsSupportedInputPolicy(MovementCommandInputPolicy policy) =>
        policy is MovementCommandInputPolicy.Suppress or MovementCommandInputPolicy.AllowTurn;

    Vector3 SamplePosition(ClientMovementCommand command, uint elapsedMs)
    {
        float normalizedTime = Math.Clamp(elapsedMs / (float)command.DurationMs, 0.0f, 1.0f);
        float progress = _curves.Sample(command.CurveId, normalizedTime);
        return Vector3.Lerp(command.StartPosition, command.TargetPosition, progress);
    }

    static bool IsFinite(Vector3 value) =>
        !float.IsNaN(value.X) && !float.IsInfinity(value.X) &&
        !float.IsNaN(value.Y) && !float.IsInfinity(value.Y) &&
        !float.IsNaN(value.Z) && !float.IsInfinity(value.Z);
}
