using UnityEngine;

namespace Atlas.Mvp.Unity
{
    public enum BotPattern { Random, Pingpong }

    public sealed class BotPilot : MonoBehaviour
    {
        public float DurationSec = 60f;
        public BotPattern Pattern = BotPattern.Random;

        public Vector2 Joystick { get; private set; }
        bool _firePending;

        float _heading;
        float _headingChangeAt;
        float _fireAt;
        float _exitAt;
        float _pingpongDir = 1f;
        float _pingpongFlipAt;

        public bool ConsumeFire()
        {
            if (!_firePending) return false;
            _firePending = false;
            return true;
        }

        void Start()
        {
            float t = Time.time;
            _heading = Random.Range(0f, 2f * Mathf.PI);
            _headingChangeAt = t + Random.Range(1.5f, 3f);
            _fireAt = t + Random.Range(2f, 5f);
            _exitAt = t + DurationSec;
            // Stagger initial direction so concurrent bots split East/West;
            // each crossing of x=0 fires an Offload between BSP cells.
            _pingpongDir = (Random.value < 0.5f) ? -1f : 1f;
            _pingpongFlipAt = t + Random.Range(6f, 9f);
        }

        void Update()
        {
            float t = Time.time;
            if (t >= _exitAt)
            {
                Debug.Log($"[BotPilot] duration {DurationSec:F1}s elapsed; quitting");
                Application.Quit();
                return;
            }
            if (Pattern == BotPattern.Pingpong)
            {
                if (t >= _pingpongFlipAt)
                {
                    _pingpongDir = -_pingpongDir;
                    _pingpongFlipAt = t + Random.Range(6f, 9f);
                }
                Joystick = new Vector2(_pingpongDir, 0f);
            }
            else
            {
                if (t >= _headingChangeAt)
                {
                    _heading = Random.Range(0f, 2f * Mathf.PI);
                    _headingChangeAt = t + Random.Range(2f, 5f);
                }
                Joystick = new Vector2(Mathf.Sin(_heading), Mathf.Cos(_heading));
            }
            if (t >= _fireAt)
            {
                _firePending = true;
                _fireAt = t + Random.Range(2f, 5f);
            }
        }
    }
}
