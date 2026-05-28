using UnityEngine;

namespace Atlas.Mvp.Unity
{
    // Opt-in marker for the Atlas collision exporter: a Collider is exported
    // only when this component is attached AND exportToServer is set.
    [DisallowMultipleComponent]
    [RequireComponent(typeof(Collider))]
    public sealed class ServerColliderAuthoring : MonoBehaviour
    {
        [Tooltip("Send this collider through the Atlas export pipeline. " +
                 "OFF by default; opt in per collider.")]
        public bool exportToServer = false;

        [Tooltip("Atlas physics layer ID (0..31). Match the integer the " +
                 "server layer config expects.")]
        [Range(0, 31)]
        public int layer = 0;

        [Tooltip("Free-form authoring note. Not exported.")]
        public string note = string.Empty;

        void Reset()
        {
            exportToServer = false;
            layer = 0;
            note = string.Empty;
        }

#if UNITY_EDITOR
        static readonly Color kExportColor = new(0.20f, 0.85f, 0.30f, 0.90f);
        static readonly Color kSkipColor = new(0.85f, 0.30f, 0.30f, 0.60f);

        void OnDrawGizmos()
        {
            if (!exportToServer) return;
            DrawHint(kExportColor);
        }

        void OnDrawGizmosSelected()
        {
            DrawHint(exportToServer ? kExportColor : kSkipColor);
        }

        void DrawHint(Color color)
        {
            var col = GetComponent<Collider>();
            if (col == null) return;
            var prev = Gizmos.color;
            Gizmos.color = color;
            var b = col.bounds;
            Gizmos.DrawWireCube(b.center, b.size);
            Gizmos.color = prev;
        }
#endif
    }
}
