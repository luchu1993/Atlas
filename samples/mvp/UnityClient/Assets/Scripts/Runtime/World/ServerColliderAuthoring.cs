using UnityEngine;

namespace Atlas.Mvp.Unity
{
    /// <summary>
    /// Marks the attached Collider as a candidate for export into an Atlas
    /// collision asset. Defaults to <c>exportToServer = false</c> so adding
    /// the component is opt-in only — the command-line exporter skips
    /// colliders without this component or with the flag off, avoiding
    /// accidental shipping of client-only visual occluders, doors or
    /// trigger volumes.
    ///
    /// Current exporter supports primitive Box / Sphere / Capsule colliders.
    /// Mesh and terrain colliders, plus a named layer enum, are planned
    /// follow-ups.
    /// </summary>
    [DisallowMultipleComponent]
    [RequireComponent(typeof(Collider))]
    public sealed class ServerColliderAuthoring : MonoBehaviour
    {
        [Tooltip("Send this collider through the Atlas export pipeline. " +
                 "OFF by default; opt in per collider.")]
        public bool exportToServer = false;

        [Tooltip("Atlas physics layer ID (0..31). The named layer table is " +
                 "defined in Phase 14.3; for now match the integer your " +
                 "server layer config expects.")]
        [Range(0, 31)]
        public int layer = 0;

        [Tooltip("Free-form authoring note. Not exported; for cook diagnostics " +
                 "and level-design hints.")]
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
            // Outline the collider's bounds so the marker is visually
            // distinct from Unity's stock collider gizmo without
            // re-implementing each shape exactly.
            var b = col.bounds;
            Gizmos.DrawWireCube(b.center, b.size);
            Gizmos.color = prev;
        }
#endif
    }
}
