#if UNITY_2022_3_OR_NEWER

using Atlas.Client;
using UnityEngine;

namespace Atlas.Client.Unity
{
    public sealed class UnityClientLogger : IClientLogger
    {
        public void Info(string message)  => Debug.Log(message);
        public void Warn(string message)  => Debug.LogWarning(message);
        public void Error(string message) => Debug.LogError(message);
    }
}

#endif
