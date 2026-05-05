using System;
using Atlas.DataTypes;

namespace Atlas.Client;

public sealed class AvatarFilter
{
    public readonly struct Sample
    {
        public readonly double ServerTime;
        public readonly Vector3 Position;
        public readonly Vector3 Direction;
        public readonly bool OnGround;

        public Sample(double serverTime, Vector3 pos, Vector3 dir, bool onGround)
        {
            ServerTime = serverTime;
            Position = pos;
            Direction = dir;
            OnGround = onGround;
        }
    }

    public const int RingCapacity = 8;

    private readonly Sample[] _ring = new Sample[RingCapacity];
    private int _writeIndex;
    private int _count;

    public double LatencyFrames { get; set; } = 3.0;
    public double ServerInterval { get; set; } = 0.1;
    public double CurvePower { get; set; } = 2.0;
    public double MaxExtrapolation { get; set; } = 0.05;

    private double _latencyCurrent;
    private double _lastInputServerTime;
    private bool _initialised;

    public int SampleCount => _count;
    public double CurrentLatency => _latencyCurrent;
    public double TargetLatency => LatencyFrames * ServerInterval;

    public void Reset()
    {
        _writeIndex = 0;
        _count = 0;
        _latencyCurrent = 0;
        _initialised = false;
    }

    public void Input(double serverTime, Vector3 pos, Vector3 dir, bool onGround)
    {
        if (_count > 0 && serverTime <= _ring[NewestIndex()].ServerTime) return;

        _ring[_writeIndex] = new Sample(serverTime, pos, dir, onGround);
        _writeIndex = (_writeIndex + 1) % RingCapacity;
        if (_count < RingCapacity) _count++;

        _lastInputServerTime = serverTime;
        if (!_initialised)
        {
            _latencyCurrent = TargetLatency;
            _initialised = true;
        }
    }

    public void UpdateLatency(double dt)
    {
        if (!_initialised || dt <= 0) return;

        double target = TargetLatency;
        double diff = target - _latencyCurrent;
        double absDiff = diff >= 0 ? diff : -diff;

        // |diff|^curvePower: aggressive on big lag, smooth on small.
        double speed = Math.Pow(absDiff, CurvePower) * 4.0;
        double step = Math.Sign(diff) * Math.Min(absDiff, speed * dt);
        _latencyCurrent += step;
    }

    public bool TryEvaluate(double clientTime, out Vector3 pos, out Vector3 dir, out bool onGround)
    {
        pos = default;
        dir = default;
        onGround = false;
        if (_count == 0) return false;

        double targetTime = clientTime - _latencyCurrent;
        int newest = NewestIndex();

        if (_count == 1 || targetTime >= _ring[newest].ServerTime)
        {
            var s = _ring[newest];
            pos = ExtrapolatePosition(s, targetTime);
            dir = s.Direction;
            onGround = s.OnGround;
            return true;
        }

        int oldest = OldestIndex();
        if (targetTime <= _ring[oldest].ServerTime)
        {
            var s = _ring[oldest];
            pos = s.Position;
            dir = s.Direction;
            onGround = s.OnGround;
            return true;
        }

        int idx = oldest;
        for (int i = 0; i < _count - 1; ++i)
        {
            int next = (idx + 1) % RingCapacity;
            var a = _ring[idx];
            var b = _ring[next];
            if (targetTime >= a.ServerTime && targetTime <= b.ServerTime)
            {
                double span = b.ServerTime - a.ServerTime;
                float t = span > 0 ? (float)((targetTime - a.ServerTime) / span) : 0;
                pos = Vector3.Lerp(a.Position, b.Position, t);
                dir = Vector3.Lerp(a.Direction, b.Direction, t);
                onGround = b.OnGround;
                return true;
            }
            idx = next;
        }

        var fallback = _ring[newest];
        pos = fallback.Position;
        dir = fallback.Direction;
        onGround = fallback.OnGround;
        return true;
    }

    private int NewestIndex() => (_writeIndex - 1 + RingCapacity) % RingCapacity;
    private int OldestIndex() => _count < RingCapacity ? 0 : _writeIndex;

    private Vector3 ExtrapolatePosition(in Sample s, double targetTime)
    {
        double ahead = targetTime - s.ServerTime;
        if (ahead <= 0 || _count < 2) return s.Position;

        double cap = MaxExtrapolation;
        if (ahead > cap) ahead = cap;

        int newest = NewestIndex();
        int prev = (newest - 1 + RingCapacity) % RingCapacity;
        var a = _ring[prev];
        var b = _ring[newest];
        double span = b.ServerTime - a.ServerTime;
        if (span <= 0) return s.Position;

        float scale = (float)(ahead / span);
        return new Vector3(
            b.Position.X + (b.Position.X - a.Position.X) * scale,
            b.Position.Y + (b.Position.Y - a.Position.Y) * scale,
            b.Position.Z + (b.Position.Z - a.Position.Z) * scale);
    }
}
