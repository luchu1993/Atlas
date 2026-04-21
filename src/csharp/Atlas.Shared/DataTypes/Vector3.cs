using System;

namespace Atlas.DataTypes;

public readonly struct Vector3 : IEquatable<Vector3>
{
    public readonly float X;
    public readonly float Y;
    public readonly float Z;

    public Vector3(float x, float y, float z)
    {
        X = x;
        Y = y;
        Z = z;
    }

    public static Vector3 Zero => default;
    public static Vector3 One => new(1f, 1f, 1f);
    public static Vector3 Up => new(0f, 1f, 0f);
    public static Vector3 Forward => new(0f, 0f, 1f);
    public static Vector3 Right => new(1f, 0f, 0f);

    public float LengthSquared => X * X + Y * Y + Z * Z;
    public float Length => (float)Math.Sqrt(LengthSquared);

    public Vector3 Normalized
    {
        get
        {
            float len = Length;
            if (len < 1e-8f) return Zero;
            return new Vector3(X / len, Y / len, Z / len);
        }
    }

    public static float Dot(Vector3 a, Vector3 b)
        => a.X * b.X + a.Y * b.Y + a.Z * b.Z;

    public static Vector3 Cross(Vector3 a, Vector3 b)
        => new(a.Y * b.Z - a.Z * b.Y,
               a.Z * b.X - a.X * b.Z,
               a.X * b.Y - a.Y * b.X);

    public static float Distance(Vector3 a, Vector3 b)
        => (a - b).Length;

    public static Vector3 Lerp(Vector3 a, Vector3 b, float t)
        => new(a.X + (b.X - a.X) * t,
               a.Y + (b.Y - a.Y) * t,
               a.Z + (b.Z - a.Z) * t);

    public static Vector3 operator +(Vector3 a, Vector3 b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
    public static Vector3 operator -(Vector3 a, Vector3 b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
    public static Vector3 operator *(Vector3 v, float s) => new(v.X * s, v.Y * s, v.Z * s);
    public static Vector3 operator *(float s, Vector3 v) => new(v.X * s, v.Y * s, v.Z * s);
    public static Vector3 operator -(Vector3 v) => new(-v.X, -v.Y, -v.Z);

    public static bool operator ==(Vector3 a, Vector3 b) => a.Equals(b);
    public static bool operator !=(Vector3 a, Vector3 b) => !a.Equals(b);

    public bool Equals(Vector3 other)
        => X == other.X && Y == other.Y && Z == other.Z;

    public override bool Equals(object? obj)
        => obj is Vector3 other && Equals(other);

    public override int GetHashCode()
        => HashCode.Combine(X, Y, Z);

    public override string ToString()
        => $"({X}, {Y}, {Z})";

#if UNITY_5_3_OR_NEWER
    // ------------------------------------------------------------------------
    // UnityEngine.Vector3 interop — only compiled when Atlas.Shared is
    // consumed from inside a Unity project (UNITY_5_3_OR_NEWER symbol is
    // defined by every Unity version since 5.3, 2015). Atlas.Shared ships
    // to server / desktop-client builds where UnityEngine is absent;
    // guarding the bridge behind the symbol keeps those builds clean.
    //
    // Implicit both ways is deliberate: game scripts tend to mix
    // UnityEngine.Vector3 (component APIs, Transform, etc.) with
    // Atlas.DataTypes.Vector3 (wire / replicated state) in the same
    // expression and explicit casting every site would be noise.
    // ------------------------------------------------------------------------
    public static implicit operator UnityEngine.Vector3(Vector3 v)
        => new UnityEngine.Vector3(v.X, v.Y, v.Z);

    public static implicit operator Vector3(UnityEngine.Vector3 v)
        => new Vector3(v.x, v.y, v.z);
#endif
}
