using UnityEngine;
using UnityEngine.UIElements;

namespace Atlas.Mvp.Unity
{
    public sealed partial class GameHud
    {
        public void BindDebugOverlays(AoiBoxOverlay aoiBoxes, BspGizmo bspGizmo)
        {
            _aoiBoxes = aoiBoxes;
            _bspGizmo = bspGizmo;
            if (_aoiCheckbox != null)
                _aoiCheckbox.EnableInClassList("checked", _aoiBoxes.Visible);
            if (_bspCheckbox != null)
                _bspCheckbox.EnableInClassList("checked", _bspGizmo.Visible);
        }

        int ComputeQuadrant(Vector3 pos)
        {
            bool east = pos.x >= 0f;
            bool north = pos.z >= 0f;
            if (east && north) return 1;
            if (!east && north) return 2;
            if (!east && !north) return 3;
            return 4;
        }

        static string QuadrantLabel(int q) => q switch
        {
            1 => "Q1 (+X,+Z)",
            2 => "Q2 (-X,+Z)",
            3 => "Q3 (-X,-Z)",
            4 => "Q4 (+X,-Z)",
            _ => "--",
        };

        void RefreshCellIndicator()
        {
            if (_owner == null || _owner.IsDestroyed || _ownerTransform == null)
            {
                _cellValue.text = "--";
                return;
            }
            int q = ComputeQuadrant(_ownerTransform.position);
            if (_lastQuadrant != 0 && q != _lastQuadrant)
            {
                _crossCount++;
                _crossValue.text = _crossCount.ToString();
            }
            _lastQuadrant = q;
            _cellValue.text = QuadrantLabel(q);
        }

        void UpdateNetStats(float window)
        {
            uint bytesSent = 0, bytesRecv = 0, queue = 0;
            if (_net != null && _net.TryGetStats(out var s))
            {
                _rttMs = s.RttMs;
                _pingValue.text = $"{_rttMs} MS";
                bytesSent = s.BytesSent;
                bytesRecv = s.BytesRecv;
                queue = s.SendQueueSize;
            }
            uint rpcOut = _net?.RpcOutCount ?? 0;
            uint aoiEnter = _views?.AoiEnterCount ?? 0;
            uint aoiLeave = _views?.AoiLeaveCount ?? 0;

            if (!_ratesPrimed)
            {
                _ratesPrimed = true;
            }
            else
            {
                float invWin = window > 0f ? 1f / window : 0f;
                float kbUp = DeltaWrap(bytesSent, _lastBytesSent) * invWin / 1024f;
                float kbDown = DeltaWrap(bytesRecv, _lastBytesRecv) * invWin / 1024f;
                float rpcPerSec = DeltaWrap(rpcOut, _lastRpcOut) * invWin;
                float enterPerSec = DeltaWrap(aoiEnter, _lastAoiEnter) * invWin;
                float leavePerSec = DeltaWrap(aoiLeave, _lastAoiLeave) * invWin;
                _bwUpValue.text = $"{kbUp:F1} KB/S";
                _bwDownValue.text = $"{kbDown:F1} KB/S";
                _rpcOutValue.text = $"{rpcPerSec:F1} /S";
                _aoiEnterValue.text = $"{enterPerSec:F1} /S";
                _aoiLeaveValue.text = $"{leavePerSec:F1} /S";
            }
            _queueValue.text = queue.ToString();
            _lastBytesSent = bytesSent;
            _lastBytesRecv = bytesRecv;
            _lastRpcOut = rpcOut;
            _lastAoiEnter = aoiEnter;
            _lastAoiLeave = aoiLeave;
        }

        static uint DeltaWrap(uint cur, uint prev) => unchecked(cur - prev);
    }
}
