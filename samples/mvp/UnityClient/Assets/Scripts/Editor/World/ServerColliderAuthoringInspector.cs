using Atlas.Mvp.Unity;
using UnityEditor;
using UnityEngine;

namespace Atlas.Mvp.Editor
{
    [CustomEditor(typeof(ServerColliderAuthoring))]
    [CanEditMultipleObjects]
    public sealed class ServerColliderAuthoringInspector : UnityEditor.Editor
    {
        public override void OnInspectorGUI()
        {
            serializedObject.Update();
            EditorGUILayout.PropertyField(serializedObject.FindProperty("exportToServer"));
            EditorGUILayout.PropertyField(serializedObject.FindProperty("layer"));
            EditorGUILayout.PropertyField(serializedObject.FindProperty("note"));
            serializedObject.ApplyModifiedProperties();

            // Per-instance shape/scale checks are misleading on mixed targets.
            if (serializedObject.isEditingMultipleObjects) return;

            var authoring = (ServerColliderAuthoring)target;
            var col = authoring.GetComponent<Collider>();
            if (col == null)
            {
                EditorGUILayout.HelpBox(
                    "ServerColliderAuthoring requires a Collider on the same GameObject.",
                    MessageType.Error);
                return;
            }

            if (col is WheelCollider)
            {
                EditorGUILayout.HelpBox(
                    $"{col.GetType().Name} is not supported by the exporter yet. " +
                    "Box / Sphere / Capsule / Mesh / Terrain colliders are exported.",
                    MessageType.Warning);
            }

            if (col is CapsuleCollider capsule && capsule.direction != 1)
            {
                EditorGUILayout.HelpBox(
                    "Atlas capsules are vertical; only a Y-Axis CapsuleCollider is exported.",
                    MessageType.Warning);
            }

            var scale = authoring.transform.lossyScale;
            bool nonUniform = !Mathf.Approximately(scale.x, scale.y) ||
                              !Mathf.Approximately(scale.x, scale.z);
            if ((col is SphereCollider || col is CapsuleCollider) && nonUniform)
            {
                EditorGUILayout.HelpBox(
                    $"{col.GetType().Name} has non-uniform lossy scale " +
                    $"({scale.x:F3}, {scale.y:F3}, {scale.z:F3}). " +
                    "Server rejects this; use BoxCollider or normalise the scale.",
                    MessageType.Warning);
            }

            if (scale.x < 0f || scale.y < 0f || scale.z < 0f)
            {
                EditorGUILayout.HelpBox(
                    "Negative lossy scale is not exportable.",
                    MessageType.Warning);
            }

            if (!authoring.exportToServer)
            {
                EditorGUILayout.HelpBox(
                    "exportToServer is OFF — this collider stays client-only.",
                    MessageType.Info);
            }
        }
    }
}
