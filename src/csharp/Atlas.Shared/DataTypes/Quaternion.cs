using System;

namespace Atlas.DataTypes;

public readonly struct Quaternion : IEquatable<Quaternion>
{
    public readonly float X;
    public readonly float Y;
    public readonly float Z;
    public readonly float W;

    public Quaternion(float x, float y, float z, float w)
    {
        X = x;
        Y = y;
        Z = z;
        W = w;
    }

    public static Quaternion Identity => new(0f, 0f, 0f, 1f);

    public float LengthSquared => X * X + Y * Y + Z * Z + W * W;
    public float Length => (float)Math.Sqrt(LengthSquared);

    public Quaternion Normalized
    {
        get
        {
            float len = Length;
            if (len < 1e-8f) return Identity;
            return new Quaternion(X / len, Y / len, Z / len, W / len);
        }
    }

    public static Quaternion operator *(Quaternion a, Quaternion b)
        => new(a.W * b.X + a.X * b.W + a.Y * b.Z - a.Z * b.Y,
               a.W * b.Y - a.X * b.Z + a.Y * b.W + a.Z * b.X,
               a.W * b.Z + a.X * b.Y - a.Y * b.X + a.Z * b.W,
               a.W * b.W - a.X * b.X - a.Y * b.Y - a.Z * b.Z);

    public static bool operator ==(Quaternion a, Quaternion b) => a.Equals(b);
    public static bool operator !=(Quaternion a, Quaternion b) => !a.Equals(b);

    public bool Equals(Quaternion other)
        => X == other.X && Y == other.Y && Z == other.Z && W == other.W;

    public override bool Equals(object? obj)
        => obj is Quaternion other && Equals(other);

    public override int GetHashCode()
        => HashCode.Combine(X, Y, Z, W);

    public override string ToString()
        => $"({X}, {Y}, {Z}, {W})";
}
