#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import sys
import unittest
from collections import OrderedDict
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
            "state=healthy saves=3 save_failures=0 error_present=0 error_detail=none")
        self.assertEqual(fields["state"], "healthy")
        self.assertEqual(fields["saves"], "3")
        self.assertTrue(verify_baseappmgr_ha.summary_has(fields, "error_present", "0"))
        self.assertFalse(verify_baseappmgr_ha.summary_has(fields, "state", "stale"))


class WatcherValueTest(unittest.TestCase):
    def test_returns_empty_when_only_target_name_present(self) -> None:
        with mock.patch.object(verify_baseappmgr_ha, "run_atlas_tool",
                               return_value="baseappmgr\n"):
            value = verify_baseappmgr_ha.watcher_value(
                Path("atlas_tool"), "127.0.0.1:20018", "baseappmgr:baseappmgr",
                "baseappmgr/ha/snapshot_last_save_error")
        self.assertEqual(value, "")

    def test_returns_value_when_populated(self) -> None:
        with mock.patch.object(verify_baseappmgr_ha, "run_atlas_tool",
                               return_value="baseappmgr               42\n"):
            value = verify_baseappmgr_ha.watcher_value(
                Path("atlas_tool"), "127.0.0.1:20018", "baseappmgr:baseappmgr",
                "baseappmgr/ha/snapshot_saves")
        self.assertEqual(value, "42")

    def test_rejects_unexpected_shape(self) -> None:
        with mock.patch.object(verify_baseappmgr_ha, "run_atlas_tool",
                               return_value="garbage_line\n"):
            with self.assertRaisesRegex(RuntimeError, "unexpected watcher output"):
                verify_baseappmgr_ha.watcher_value(
                    Path("atlas_tool"), "127.0.0.1:20018", "baseappmgr:baseappmgr",
                    "baseappmgr/ha/snapshot_saves")


class SnapshotHealthTest(unittest.TestCase):
    def _watchers(self, **overrides):
        defaults = {
            "baseappmgr/ha/snapshot_saves": "10",
            "baseappmgr/ha/snapshot_restores": "0",
            "baseappmgr/ha/snapshot_fallback_restores": "0",
            "baseappmgr/ha/snapshot_save_failures": "0",
            "baseappmgr/ha/snapshot_restore_failures": "0",
            "baseappmgr/ha/snapshot_failures": "0",
            "baseappmgr/ha/snapshot_backup_skips": "0",
            "baseappmgr/ha/snapshot_dirty": "false",
            "baseappmgr/ha/snapshot_save_stale": "false",
            "baseappmgr/ha/snapshot_status":
                "state=healthy saves=10 save_failures=0 error_present=0 error_detail=none",
        }
        defaults.update(overrides)
        return defaults

    def test_healthy_snapshot(self) -> None:
        watchers = self._watchers()
        with mock.patch.object(verify_baseappmgr_ha, "watcher_value",
                               side_effect=lambda exe, machined, target, path: watchers[path]):
            with mock.patch.object(verify_baseappmgr_ha, "int_watcher",
                                   side_effect=lambda exe, machined, target, path: int(watchers[path])):
                health = verify_baseappmgr_ha.read_snapshot_health(
                    Path("atlas_tool"), "machined", "baseappmgr:baseappmgr")
        self.assertTrue(health.healthy, health.detail)
        self.assertEqual(health.saves, 10)

    def test_dirty_snapshot_unhealthy(self) -> None:
        watchers = self._watchers(**{"baseappmgr/ha/snapshot_dirty": "true"})
        with mock.patch.object(verify_baseappmgr_ha, "watcher_value",
                               side_effect=lambda exe, machined, target, path: watchers[path]):
            with mock.patch.object(verify_baseappmgr_ha, "int_watcher",
                                   side_effect=lambda exe, machined, target, path: int(watchers[path])):
                health = verify_baseappmgr_ha.read_snapshot_health(
                    Path("atlas_tool"), "machined", "baseappmgr:baseappmgr")
        self.assertFalse(health.healthy)

    def test_failed_save_unhealthy(self) -> None:
        watchers = self._watchers(**{
            "baseappmgr/ha/snapshot_save_failures": "2",
            "baseappmgr/ha/snapshot_failures": "2",
            "baseappmgr/ha/snapshot_status":
                "state=degraded saves=10 save_failures=2 error_present=1 error_detail=disk_full",
        })
        with mock.patch.object(verify_baseappmgr_ha, "watcher_value",
                               side_effect=lambda exe, machined, target, path: watchers[path]):
            with mock.patch.object(verify_baseappmgr_ha, "int_watcher",
                                   side_effect=lambda exe, machined, target, path: int(watchers[path])):
                health = verify_baseappmgr_ha.read_snapshot_health(
                    Path("atlas_tool"), "machined", "baseappmgr:baseappmgr")
        self.assertFalse(health.healthy)
        self.assertEqual(health.save_failures, 2)


class ReattachHealthTest(unittest.TestCase):
    def _make(self, restored, pending, completed, stuck, state, status, require_restored=True,
              min_baseapps=1):
        watchers = {
            "baseappmgr/ha/restored_baseapps": str(restored),
            "baseappmgr/ha/reattach_pending": str(pending),
            "baseappmgr/ha/reattach_completed_count": str(completed),
            "baseappmgr/ha/reattach_stuck": str(stuck),
            "baseappmgr/ha/reattach_state": state,
            "baseappmgr/ha/reattach_status": status,
        }
        with mock.patch.object(verify_baseappmgr_ha, "watcher_value",
                               side_effect=lambda exe, machined, target, path: watchers[path]):
            with mock.patch.object(verify_baseappmgr_ha, "int_watcher",
                                   side_effect=lambda exe, machined, target, path: int(watchers[path])):
                return verify_baseappmgr_ha.read_reattach_health(
                    Path("atlas_tool"), "machined", "baseappmgr:baseappmgr",
                    require_restored, min_baseapps)

    def test_completed_state(self) -> None:
        h = self._make(restored=1, pending=0, completed=1, stuck=0, state="complete",
                       status="state=complete restored=1 pending=0 stuck=0 completed=1"
                              " completed_count=1")
        self.assertTrue(h.healthy, h.detail)

    def test_pending_state_is_unhealthy(self) -> None:
        h = self._make(restored=1, pending=1, completed=0, stuck=0, state="pending",
                       status="state=pending restored=1 pending=1 stuck=0 completed=0"
                              " completed_count=0")
        self.assertFalse(h.healthy)

    def test_stuck_state_is_unhealthy(self) -> None:
        h = self._make(restored=1, pending=1, completed=0, stuck=1, state="stuck",
                       status="state=stuck restored=1 pending=1 stuck=1 completed=0"
                              " completed_count=0")
        self.assertFalse(h.healthy)

    def test_require_restored_demands_min_baseapps(self) -> None:
        h = self._make(restored=0, pending=0, completed=0, stuck=0, state="idle",
                       status="state=idle restored=0 pending=0 stuck=0 completed=1"
                              " completed_count=0",
                       require_restored=True, min_baseapps=1)
        self.assertFalse(h.healthy)


class ReviverHealthTest(unittest.TestCase):
    def _make(self, *, active="true", active_pid="100", launch_count="1",
              heartbeat_acks="5", age_ms="200", last_error="", status="active",
              require_active_pid=None):
        watchers = {
            "reviver/baseappmgr/active": active,
            "reviver/baseappmgr/active_pid": active_pid,
            "reviver/baseappmgr/active_generation": "1",
            "reviver/baseappmgr/launch_count": launch_count,
            "reviver/baseappmgr/heartbeat_acks": heartbeat_acks,
            "reviver/baseappmgr/heartbeat_last_ack_age_ms": age_ms,
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


class BuildSummaryTest(unittest.TestCase):
    def test_no_inject_summary_marks_healthy_when_no_failures(self) -> None:
        import argparse
        args = argparse.Namespace(
            no_inject=True, build="debug", machined="m", baseappmgr_name="baseappmgr",
            reviver_name="", cycles=1, min_revivers=1, min_baseapps=1, timeout_sec=60.0,
            poll_sec=1.0, stability_sec=5.0, max_takeover_ms=0, shutdown_reason=1,
            allow_empty_snapshot=False)
        summary = verify_baseappmgr_ha.build_summary(args, cycles=[], current_healthy=True,
                                                     failure_stages=[], gate_failures=[])
        self.assertTrue(summary["summary"]["overall_healthy"])
        self.assertEqual(summary["summary"]["cycles"], 0)

    def test_gate_failure_makes_overall_unhealthy(self) -> None:
        import argparse
        args = argparse.Namespace(
            no_inject=False, build="debug", machined="m", baseappmgr_name="baseappmgr",
            reviver_name="", cycles=1, min_revivers=1, min_baseapps=1, timeout_sec=60.0,
            poll_sec=1.0, stability_sec=5.0, max_takeover_ms=5000, shutdown_reason=1,
            allow_empty_snapshot=False)
        gate = [{"name": "max_takeover_ms", "metric": "max_takeover_ms",
                 "maximum": 5000, "value": 8000, "ok": False}]
        summary = verify_baseappmgr_ha.build_summary(
            args, cycles=[], current_healthy=True, failure_stages=[], gate_failures=gate)
        self.assertFalse(summary["summary"]["overall_healthy"])
        self.assertEqual(summary["gates"], gate)


if __name__ == "__main__":
    unittest.main()
