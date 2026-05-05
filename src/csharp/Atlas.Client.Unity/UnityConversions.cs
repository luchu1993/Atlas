using AtlasVector3 = Atlas.DataTypes.Vector3;
using AtlasQuaternion = Atlas.DataTypes.Quaternion;

namespace Atlas.Client.Unity
{
    public static class UnityConversions
    {
        public static UnityEngine.Vector3 ToUnity(this AtlasVector3 v)
            => new UnityEngine.Vector3(v.X, v.Y, v.Z);

        public static AtlasVector3 ToAtlas(this UnityEngine.Vector3 v)
            => new AtlasVector3(v.x, v.y, v.z);

        public static UnityEngine.Quaternion ToUnity(this AtlasQuaternion q)
            => new UnityEngine.Quaternion(q.X, q.Y, q.Z, q.W);

        public static AtlasQuaternion ToAtlas(this UnityEngine.Quaternion q)
            => new AtlasQuaternion(q.x, q.y, q.z, q.w);
    }
}
