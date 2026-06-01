#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "cluster_control" / "verify_baseappmgr_ha.py"
SPEC = importlib.util.spec_from_file_location("verify_baseappmgr_ha", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
verify_baseappmgr_ha = importlib.util.module_from_spec(SPEC)
# Register before exec_module so @dataclass-decorated classes can resolve
# their __module__ via sys.modules during construction (Python 3.14 hardening).
sys.modules["verify_baseappmgr_ha"] = verify_baseappmgr_ha
SPEC.loader.exec_module(verify_baseappmgr_ha)


class SummaryFieldsTest(unittest.TestCase):
    def test_parses_status_into_ordered_fields(self) -> None:
        fields = verify_baseappmgr_ha.summary_fields(
            "active=true active_pid=42 heartbeat_acks=5 priority=255")
        self.assertEqual(fields["active"], "true")
        self.assertEqual(fields["active_pid"], "42")
        self.assertTrue(verify_baseappmgr_ha.summary_has(fields, "priority", "255"))
        self.assertFalse(verify_baseappmgr_ha.summary_has(fields, "active", "false"))


class WatcherValueTest(unittest.TestCase):
    def test_returns_empty_when_only_target_name_present(self) -> None:
        with mock.patch.object(verify_baseappmgr_ha, "run_atlas_tool",
                               return_value="baseappmgr\n"):
            value = verify_baseappmgr_ha.watcher_value(
                Path("atlas_tool"), "127.0.0.1:20018", "baseappmgr:baseappmgr",
                "reviver/baseappmgr/last_error")
        self.assertEqual(value, "")

    def test_returns_value_when_populated(self) -> None:
        with mock.patch.object(verify_baseappmgr_ha, "run_atlas_tool",
                               return_value="baseappmgr               42\n"):
            value = verify_baseappmgr_ha.watcher_value(
                Path("atlas_tool"), "127.0.0.1:20018", "baseappmgr:baseappmgr",
                "baseappmgr/baseapp_count")
        self.assertEqual(value, "42")

    def test_rejects_unexpected_shape(self) -> None:
        with mock.patch.object(verify_baseappmgr_ha, "run_atlas_tool",
                               return_value="garbage_line\n"):
            with self.assertRaisesRegex(RuntimeError, "unexpected watcher output"):
                verify_baseappmgr_ha.watcher_value(
                    Path("atlas_tool"), "127.0.0.1:20018", "baseappmgr:baseappmgr",
                    "baseappmgr/baseapp_count")


class WorkerRebuildHealthTest(unittest.TestCase):
    def _make(self, *, count, expected):
        watchers = {"baseappmgr/baseapp_count": str(count)}
        with mock.patch.object(verify_baseappmgr_ha, "int_watcher",
                               side_effect=lambda exe, machined, target, path: int(watchers[path])):
            return verify_baseappmgr_ha.read_worker_rebuild_health(
                Path("atlas_tool"), "machined", "baseappmgr:baseappmgr", expected)

    def test_recovered_when_count_meets_expected(self) -> None:
        h = self._make(count=3, expected=3)
        self.assertTrue(h.healthy, h.detail)
        self.assertTrue(h.recovered)
        self.assertEqual(h.baseapp_count, 3)

    def test_recovered_when_count_exceeds_expected(self) -> None:
        h = self._make(count=5, expected=3)
        self.assertTrue(h.healthy, h.detail)

    def test_below_expected_is_unhealthy(self) -> None:
        h = self._make(count=1, expected=3)
        self.assertFalse(h.healthy)
        self.assertFalse(h.recovered)

    def test_empty_cluster_target_zero_always_recovers(self) -> None:
        h = self._make(count=0, expected=0)
        self.assertTrue(h.healthy, h.detail)


class ReviverHealthTest(unittest.TestCase):
    def _make(self, *, active="true", active_pid="100", launch_count="1",
              heartbeat_acks="5", age_ms="200", priority="255", active_reviver="true",
              last_error="", status="active", require_active_pid=None):
        watchers = {
            "reviver/baseappmgr/active": active,
            "reviver/baseappmgr/active_pid": active_pid,
            "reviver/baseappmgr/active_generation": "1",
            "reviver/baseappmgr/launch_count": launch_count,
            "reviver/baseappmgr/heartbeat_acks": heartbeat_acks,
            "reviver/baseappmgr/heartbeat_last_ack_age_ms": age_ms,
            "reviver/baseappmgr/priority": priority,
            "reviver/baseappmgr/active_reviver": active_reviver,
            "reviver/baseappmgr/last_error": last_error,
            "reviver/baseappmgr/status": status,
        }
        with mock.patch.object(verify_baseappmgr_ha, "watcher_value",
                               side_effect=lambda exe, machined, target, path: watchers[path]):
            with mock.patch.object(verify_baseappmgr_ha, "int_watcher",
                                   side_effect=lambda exe, machined, target, path: int(watchers[path])):
                return verify_baseappmgr_ha.read_reviver_health(
                    Path("atlas_tool"), "machined", "reviver:reviver",
                    require_active_pid=require_active_pid)

    def test_healthy_reviver(self) -> None:
        h = self._make()
        self.assertTrue(h.healthy, h.detail)
        self.assertEqual(h.priority, 255)
        self.assertTrue(h.active_reviver)

    def test_inactive_reviver_unhealthy(self) -> None:
        h = self._make(active="false")
        self.assertFalse(h.healthy)

    def test_pid_mismatch_unhealthy(self) -> None:
        h = self._make(active_pid="100", require_active_pid=999)
        self.assertFalse(h.healthy)

    def test_zero_heartbeat_acks_unhealthy(self) -> None:
        h = self._make(heartbeat_acks="0")
        self.assertFalse(h.healthy)

    def test_last_error_set_unhealthy(self) -> None:
        h = self._make(last_error="restart limit reached")
        self.assertFalse(h.healthy)

    def test_standby_monitor_reports_active_reviver_false(self) -> None:
        # A live standby still answers watchers but is not the designated monitor.
        h = self._make(active_reviver="false")
        self.assertFalse(h.active_reviver)
        self.assertTrue(h.healthy, h.detail)


class ArbitrationHealthTest(unittest.TestCase):
    def _make(self, *, priority="255", active_reviver="true"):
        watchers = {
            "reviver/baseappmgr/priority": priority,
            "reviver/baseappmgr/active_reviver": active_reviver,
        }
        with mock.patch.object(verify_baseappmgr_ha, "watcher_value",
                               side_effect=lambda exe, machined, target, path: watchers[path]):
            with mock.patch.object(verify_baseappmgr_ha, "int_watcher",
                                   side_effect=lambda exe, machined, target, path: int(watchers[path])):
                return verify_baseappmgr_ha.read_arbitration_health(
                    Path("atlas_tool"), "machined", "reviver:reviver")

    def test_active_monitor_is_healthy(self) -> None:
        h = self._make()
        self.assertTrue(h.healthy, h.detail)
        self.assertEqual(h.priority, 255)

    def test_standby_is_not_active_monitor(self) -> None:
        h = self._make(active_reviver="false", priority="200")
        self.assertFalse(h.healthy)
        self.assertEqual(h.priority, 200)


class BuildSummaryTest(unittest.TestCase):
    def _args(self, **overrides):
        import argparse
        base = dict(
            no_inject=True, build="debug", machined="m", baseappmgr_name="baseappmgr",
            reviver_name="", cycles=1, min_revivers=1, min_baseapps=1, timeout_sec=60.0,
            poll_sec=1.0, stability_sec=5.0, max_takeover_ms=0, max_reviver_failover_ms=0,
            shutdown_reason=1, allow_empty_cluster=False, check_active_reviver=False,
            verify_reviver_failover=False)
        base.update(overrides)
        return argparse.Namespace(**base)

    def test_no_inject_summary_marks_healthy_when_no_failures(self) -> None:
        summary = verify_baseappmgr_ha.build_summary(
            self._args(), cycles=[], current_healthy=True, failure_stages=[], gate_failures=[])
        self.assertTrue(summary["summary"]["overall_healthy"])
        self.assertEqual(summary["summary"]["cycles"], 0)
        self.assertEqual(summary["schema_version"], 2)

    def test_gate_failure_makes_overall_unhealthy(self) -> None:
        gate = [{"name": "max_takeover_ms", "metric": "max_takeover_ms",
                 "maximum": 5000, "value": 8000, "ok": False}]
        summary = verify_baseappmgr_ha.build_summary(
            self._args(no_inject=False, max_takeover_ms=5000),
            cycles=[], current_healthy=True, failure_stages=[], gate_failures=gate)
        self.assertFalse(summary["summary"]["overall_healthy"])
        self.assertEqual(summary["gates"], gate)


if __name__ == "__main__":
    unittest.main()
