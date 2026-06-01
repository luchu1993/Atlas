#!/usr/bin/env python3

from __future__ import annotations

import argparse
import importlib.util
import io
import json
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "cluster_control" / "verify_cellappmgr_ha.py"
SPEC = importlib.util.spec_from_file_location("verify_cellappmgr_ha", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
verify_cellappmgr_ha = importlib.util.module_from_spec(SPEC)
# Register before exec_module so @dataclass-decorated classes can resolve
# their __module__ via sys.modules during construction (Python 3.14 hardening).
sys.modules["verify_cellappmgr_ha"] = verify_cellappmgr_ha
SPEC.loader.exec_module(verify_cellappmgr_ha)


class SummaryFieldsTest(unittest.TestCase):
    def test_parses_summary_fields_without_substring_false_positive(self) -> None:
        fields = verify_cellappmgr_ha.summary_fields(
            "state=readyish valid=1 error_present=0 error_detail=not_none"
        )

        self.assertFalse(verify_cellappmgr_ha.summary_has(fields, "state", "ready"))
        self.assertTrue(verify_cellappmgr_ha.summary_has(fields, "valid", "1"))
        self.assertFalse(verify_cellappmgr_ha.summary_has(fields, "error_detail", "none"))

    def test_preserves_first_field_when_app_segments_repeat_keys(self) -> None:
        fields = verify_cellappmgr_ha.summary_fields(
            "state=complete restored=2 pending=0 completed=1 stuck=0 completed_count=2 "
            "app=1 addr=127.0.0.1:30001 state=attached "
            "app=2 addr=127.0.0.1:30002 state=attached"
        )

        self.assertEqual(fields["app"], "1")
        self.assertEqual(fields["state"], "complete")
        self.assertTrue(verify_cellappmgr_ha.summary_has(fields, "completed_count", "2"))


class WatcherValueTest(unittest.TestCase):
    def test_returns_empty_string_when_watcher_value_is_blank(self) -> None:
        with mock.patch.object(
            verify_cellappmgr_ha, "run_atlas_tool", return_value="cellappmgr\n"
        ):
            value = verify_cellappmgr_ha.watcher_value(
                Path("atlas_tool"),
                "127.0.0.1:20018",
                "cellappmgr:cellappmgr",
                "cellappmgr/ha/snapshot_last_save_error",
            )

        self.assertEqual(value, "")

    def test_returns_value_when_watcher_value_is_populated(self) -> None:
        with mock.patch.object(
            verify_cellappmgr_ha,
            "run_atlas_tool",
            return_value="cellappmgr               373\n",
        ):
            value = verify_cellappmgr_ha.watcher_value(
                Path("atlas_tool"),
                "127.0.0.1:20018",
                "cellappmgr:cellappmgr",
                "cellappmgr/ha/snapshot_saves",
            )

        self.assertEqual(value, "373")

    def test_returns_empty_for_reviver_qualified_target(self) -> None:
        with mock.patch.object(
            verify_cellappmgr_ha, "run_atlas_tool", return_value="reviver_01\n"
        ):
            value = verify_cellappmgr_ha.watcher_value(
                Path("atlas_tool"),
                "127.0.0.1:20018",
                "reviver:reviver_01",
                "reviver/cellappmgr/last_error",
            )

        self.assertEqual(value, "")

    def test_rejects_unexpected_watcher_output_shape(self) -> None:
        with mock.patch.object(
            verify_cellappmgr_ha, "run_atlas_tool", return_value="garbage_line\n"
        ):
            with self.assertRaisesRegex(RuntimeError, "unexpected watcher output"):
                verify_cellappmgr_ha.watcher_value(
                    Path("atlas_tool"),
                    "127.0.0.1:20018",
                    "cellappmgr:cellappmgr",
                    "cellappmgr/ha/snapshot_saves",
                )


class ReviverLeadershipTest(unittest.TestCase):
    def revivers(self) -> list[dict[str, str]]:
        return [
            {"name": "reviver_a", "pid": "100", "type": "reviver", "addr": "127.0.0.1:1"},
            {"name": "reviver_b", "pid": "101", "type": "reviver", "addr": "127.0.0.1:2"},
        ]

    def test_selects_single_active_leader(self) -> None:
        leadership = verify_cellappmgr_ha.select_leader_reviver(
            self.revivers(),
            "",
            lambda reviver: reviver["name"] == "reviver_a",
        )

        self.assertEqual(leadership.leader["name"], "reviver_a")
        self.assertEqual(leadership.active_count, 1)
        self.assertEqual(leadership.standby_count, 1)
        self.assertEqual(leadership.status, "revivers=2 active=1 standby=1 leader=reviver_a")

    def test_rejects_multiple_active_leaders(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "multiple active Reviver leaders"):
            verify_cellappmgr_ha.select_leader_reviver(
                self.revivers(),
                "",
                lambda reviver: reviver["name"] in {"reviver_a", "reviver_b"},
            )

    def test_rejects_requested_standby_reviver(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "requested Reviver reviver_b"):
            verify_cellappmgr_ha.select_leader_reviver(
                self.revivers(),
                "reviver_b",
                lambda reviver: reviver["name"] == "reviver_a",
            )


class ReviverTopologyPayloadTest(unittest.TestCase):
    def test_builds_structured_topology_payload(self) -> None:
        leadership = verify_cellappmgr_ha.ReviverLeadership(
            {"name": "reviver_a", "pid": "100", "type": "reviver"},
            1,
            2,
            "revivers=3 active=1 standby=2 leader=reviver_a",
        )

        payload = verify_cellappmgr_ha.build_reviver_topology_payload(
            leadership,
            "standby_revivers=2 standby_health=ok",
            standby_health=[{"name": "reviver_b", "healthy": True}],
        )

        self.assertEqual(payload["leader"], "reviver_a")
        self.assertEqual(payload["leader_pid"], "100")
        self.assertEqual(payload["registered_revivers"], 3)
        self.assertEqual(payload["active_count"], 1)
        self.assertEqual(payload["standby_count"], 2)
        self.assertTrue(payload["standby_health_ok"])
        self.assertEqual(payload["standby_health"][0]["name"], "reviver_b")


class ReviverFailoverHealthTest(unittest.TestCase):
    def test_accepts_standby_takeover_without_manager_restart(self) -> None:
        ok, detail = verify_cellappmgr_ha.reviver_failover_health_detail(
            expected_manager_pid="200",
            current_manager_pid="200",
            active_pid="200",
            baseline_generation=0,
            generation=1,
            baseline_launch_count=0,
            launch_count=0,
            baseline_heartbeat_acks=0,
            heartbeat_acks=1,
        )

        self.assertTrue(ok, detail)
        self.assertIn("launch_count_ok=True", detail)

    def test_accepts_standby_takeover_with_unchanged_generation(self) -> None:
        # Reviver active_generation only advances when the manager address or pid
        # changes. A pure leader failover (manager still alive at same pid)
        # leaves generation untouched; launch_count and manager_pid carry the
        # restart signal instead.
        ok, detail = verify_cellappmgr_ha.reviver_failover_health_detail(
            expected_manager_pid="200",
            current_manager_pid="200",
            active_pid="200",
            baseline_generation=5,
            generation=5,
            baseline_launch_count=0,
            launch_count=0,
            baseline_heartbeat_acks=0,
            heartbeat_acks=84,
        )

        self.assertTrue(ok, detail)
        self.assertIn("generation_ok=True", detail)
        self.assertIn("launch_count_ok=True", detail)

    def test_rejects_manager_restart_during_reviver_failover(self) -> None:
        ok, detail = verify_cellappmgr_ha.reviver_failover_health_detail(
            expected_manager_pid="200",
            current_manager_pid="201",
            active_pid="201",
            baseline_generation=0,
            generation=1,
            baseline_launch_count=0,
            launch_count=1,
            baseline_heartbeat_acks=0,
            heartbeat_acks=1,
        )

        self.assertFalse(ok)
        self.assertIn("manager_pid_ok=False", detail)
        self.assertIn("launch_count_ok=False", detail)

    def test_rejects_standby_without_new_heartbeat(self) -> None:
        ok, detail = verify_cellappmgr_ha.reviver_failover_health_detail(
            expected_manager_pid="200",
            current_manager_pid="200",
            active_pid="200",
            baseline_generation=0,
            generation=1,
            baseline_launch_count=0,
            launch_count=0,
            baseline_heartbeat_acks=3,
            heartbeat_acks=3,
        )

        self.assertFalse(ok)
        self.assertIn("heartbeat_ok=False", detail)


class StandbyReviverHealthTest(unittest.TestCase):
    def test_accepts_clean_standby_reviver(self) -> None:
        ok, detail = verify_cellappmgr_ha.standby_reviver_health_detail(
            leader_active="false",
            status="standby",
            launch_pending="false",
            restart_limit="false",
        )

        self.assertTrue(ok, detail)

    def test_builds_structured_standby_record(self) -> None:
        ok, record, detail = verify_cellappmgr_ha.build_standby_reviver_health_record(
            {"name": "reviver_b", "pid": "101", "type": "reviver"},
            leader_active="false",
            status="standby",
            launch_pending="false",
            restart_limit="false",
        )

        self.assertTrue(ok, detail)
        self.assertEqual(record["name"], "reviver_b")
        self.assertEqual(record["pid"], "101")
        self.assertFalse(record["leader_active"])
        self.assertFalse(record["launch_pending"])
        self.assertTrue(record["healthy"])

    def test_rejects_active_or_limited_standby_reviver(self) -> None:
        ok, detail = verify_cellappmgr_ha.standby_reviver_health_detail(
            leader_active="true",
            status="restart_limited",
            launch_pending="true",
            restart_limit="true",
        )

        self.assertFalse(ok)
        self.assertIn("standby_leader_inactive_ok=False", detail)
        self.assertIn("standby_status_ok=False", detail)
        self.assertIn("standby_restart_limit_ok=False", detail)


class ReviverFailoverLiveHelperTest(unittest.TestCase):
    def test_refresh_reviver_topology_reads_current_registry(self) -> None:
        args = argparse.Namespace(machined="machined", timeout_sec=1.0, poll_sec=0.0)
        exe = Path("atlas_tool")
        leader = {"name": "reviver_b", "pid": "101", "type": "reviver"}
        standby = {"name": "reviver_c", "pid": "102", "type": "reviver"}

        def list_processes(_exe: Path, _machined: str, process_type: str):
            self.assertEqual(process_type, "reviver")
            return [leader, standby]

        def watcher_value(_exe: Path, _machined: str, target: str, path: str) -> str:
            values = {
                ("reviver:reviver_b", "reviver/leader/active"): "true",
                ("reviver:reviver_c", "reviver/leader/active"): "false",
                ("reviver:reviver_c", "reviver/cellappmgr/status"): "standby",
                ("reviver:reviver_c", "reviver/cellappmgr/launch_pending"): "false",
                ("reviver:reviver_c", "reviver/cellappmgr/restart_limit_reached"): "false",
            }
            return values[(target, path)]

        with (
            mock.patch.object(verify_cellappmgr_ha, "list_processes", list_processes),
            mock.patch.object(verify_cellappmgr_ha, "watcher_value", watcher_value),
        ):
            leadership, report, revivers = verify_cellappmgr_ha.refresh_reviver_topology(
                args, exe, ""
            )

        self.assertEqual(leadership.leader["name"], "reviver_b")
        self.assertEqual(leadership.standby_count, 1)
        self.assertTrue(report.ok)
        self.assertEqual(report.records[0]["name"], "reviver_c")
        self.assertEqual(len(revivers), 2)

    def test_refresh_reviver_topology_can_capture_unhealthy_standby(self) -> None:
        args = argparse.Namespace(machined="machined", timeout_sec=1.0, poll_sec=0.0)
        exe = Path("atlas_tool")
        leader = {"name": "reviver_b", "pid": "101", "type": "reviver"}
        standby = {"name": "reviver_c", "pid": "102", "type": "reviver"}

        with (
            mock.patch.object(
                verify_cellappmgr_ha,
                "list_processes",
                return_value=[leader, standby],
            ),
            mock.patch.object(
                verify_cellappmgr_ha,
                "find_leader_reviver",
                return_value=verify_cellappmgr_ha.ReviverLeadership(
                    leader, 1, 1, "revivers=2 active=1 standby=1 leader=reviver_b"
                ),
            ),
            mock.patch.object(
                verify_cellappmgr_ha,
                "wait_for_standby_reviver_health",
                side_effect=RuntimeError("standby unhealthy"),
            ),
            mock.patch.object(
                verify_cellappmgr_ha,
                "read_standby_reviver_health",
                return_value=verify_cellappmgr_ha.StandbyReviverHealthReport(
                    False,
                    "standby_revivers=1 standby_health=reviver_c:standby_status_ok=False",
                    [{"name": "reviver_c", "healthy": False}],
                ),
            ),
        ):
            _, report, revivers = verify_cellappmgr_ha.refresh_reviver_topology(
                args, exe, "", capture_unhealthy=True
            )

        self.assertFalse(report.ok)
        self.assertEqual(report.records[0]["name"], "reviver_c")
        self.assertEqual(len(revivers), 2)

    def test_wait_or_capture_standby_health_keeps_unhealthy_report(self) -> None:
        args = argparse.Namespace(machined="machined", timeout_sec=1.0, poll_sec=0.0)
        exe = Path("atlas_tool")
        leader = {"name": "reviver_b", "pid": "101", "type": "reviver"}
        standby = {"name": "reviver_c", "pid": "102", "type": "reviver"}
        leadership = verify_cellappmgr_ha.ReviverLeadership(
            leader, 1, 1, "revivers=2 active=1 standby=1 leader=reviver_b"
        )

        with (
            mock.patch.object(
                verify_cellappmgr_ha,
                "wait_for_standby_reviver_health",
                side_effect=RuntimeError("standby unhealthy"),
            ),
            mock.patch.object(
                verify_cellappmgr_ha,
                "read_standby_reviver_health",
                return_value=verify_cellappmgr_ha.StandbyReviverHealthReport(
                    False,
                    "standby_revivers=1 standby_health=reviver_c:standby_status_ok=False",
                    [{"name": "reviver_c", "healthy": False}],
                ),
            ),
        ):
            report = verify_cellappmgr_ha.wait_or_capture_standby_reviver_health(
                args, exe, leadership, [leader, standby], capture_unhealthy=True
            )

        self.assertFalse(report.ok)
        self.assertEqual(report.records[0]["name"], "reviver_c")

    def test_waits_for_standby_leader_without_restarting_manager(self) -> None:
        args = argparse.Namespace(
            machined="machined",
            min_revivers=2,
            min_post_failover_standbys=0,
            shutdown_reason=1,
            timeout_sec=1.0,
            poll_sec=0.0,
            cellappmgr_name="cellappmgr",
        )
        exe = Path("atlas_tool")
        manager = {"name": "cellappmgr", "pid": "200", "type": "cellappmgr"}
        old_leader = {"name": "reviver_a", "pid": "100", "type": "reviver"}
        new_leader = {"name": "reviver_b", "pid": "101", "type": "reviver"}
        leadership = verify_cellappmgr_ha.ReviverLeadership(old_leader, 1, 1, "leader=a")

        def list_processes(_exe: Path, _machined: str, process_type: str):
            if process_type == "reviver":
                return [new_leader]
            if process_type == "cellappmgr":
                return [manager]
            return []

        def int_watcher(_exe: Path, _machined: str, _target: str, path: str) -> int:
            values = {
                "reviver/cellappmgr/active_generation": 1,
                "reviver/cellappmgr/launch_count": 0,
                "reviver/cellappmgr/heartbeat_acks": 1,
            }
            return values[path]

        with (
            mock.patch.object(
                verify_cellappmgr_ha,
                "read_reviver_failover_baselines",
                return_value={"reviver_b": (0, 0, 0)},
            ),
            mock.patch.object(verify_cellappmgr_ha, "shutdown_process") as shutdown,
            mock.patch.object(verify_cellappmgr_ha, "list_processes", list_processes),
            mock.patch.object(
                verify_cellappmgr_ha, "active_reviver_leaders", return_value=[new_leader]
            ),
            mock.patch.object(verify_cellappmgr_ha, "watcher_value", return_value="200"),
            mock.patch.object(verify_cellappmgr_ha, "int_watcher", int_watcher),
            mock.patch.object(verify_cellappmgr_ha, "wait_for_reviver_health") as health,
        ):
            new_leadership, result = verify_cellappmgr_ha.wait_for_reviver_leader_failover(
                args,
                exe,
                leadership,
                manager,
                [old_leader, new_leader],
                1,
            )

        self.assertEqual(result.cycle, 1)
        self.assertEqual(new_leadership.leader["name"], "reviver_b")
        self.assertEqual(result.old_leader, "reviver_a")
        self.assertEqual(result.new_leader, "reviver_b")
        self.assertEqual(result.manager_pid, "200")
        self.assertEqual(result.surviving_revivers, 1)
        self.assertEqual(result.standby_after, 0)
        self.assertEqual(result.launch_count_after, 0)
        shutdown.assert_called_once_with(exe, "machined", "reviver:reviver_a", 1)
        health.assert_called_once_with(args, exe, "reviver:reviver_b", 1)

    def test_records_reviver_failover_before_standby_gate_failure(self) -> None:
        args = argparse.Namespace(
            machined="machined",
            min_revivers=2,
            min_post_failover_standbys=1,
            shutdown_reason=1,
            timeout_sec=1.0,
            poll_sec=0.0,
            cellappmgr_name="cellappmgr",
        )
        exe = Path("atlas_tool")
        manager = {"name": "cellappmgr", "pid": "200", "type": "cellappmgr"}
        old_leader = {"name": "reviver_a", "pid": "100", "type": "reviver"}
        new_leader = {"name": "reviver_b", "pid": "101", "type": "reviver"}
        leadership = verify_cellappmgr_ha.ReviverLeadership(old_leader, 1, 1, "leader=a")

        def list_processes(_exe: Path, _machined: str, process_type: str):
            if process_type == "reviver":
                return [new_leader]
            if process_type == "cellappmgr":
                return [manager]
            return []

        def int_watcher(_exe: Path, _machined: str, _target: str, path: str) -> int:
            values = {
                "reviver/cellappmgr/active_generation": 1,
                "reviver/cellappmgr/launch_count": 0,
                "reviver/cellappmgr/heartbeat_acks": 1,
            }
            return values[path]

        with (
            mock.patch.object(
                verify_cellappmgr_ha,
                "read_reviver_failover_baselines",
                return_value={"reviver_b": (0, 0, 0)},
            ),
            mock.patch.object(verify_cellappmgr_ha, "shutdown_process"),
            mock.patch.object(verify_cellappmgr_ha, "list_processes", list_processes),
            mock.patch.object(
                verify_cellappmgr_ha, "active_reviver_leaders", return_value=[new_leader]
            ),
            mock.patch.object(verify_cellappmgr_ha, "watcher_value", return_value="200"),
            mock.patch.object(verify_cellappmgr_ha, "int_watcher", int_watcher),
            mock.patch.object(verify_cellappmgr_ha, "wait_for_reviver_health"),
        ):
            _, result = verify_cellappmgr_ha.wait_for_reviver_leader_failover(
                args,
                exe,
                leadership,
                manager,
                [old_leader, new_leader],
                1,
            )

        payload = verify_cellappmgr_ha.build_summary_payload(
            "no-inject",
            [],
            {"reviver_topology": {"registered_revivers": 1}},
            [result],
            {"min_post_failover_standbys": 1},
        )

        self.assertEqual(result.standby_after, 0)
        self.assertFalse(payload["gates"]["min_post_failover_standbys"]["ok"])

    def test_captures_failed_reviver_failover_result(self) -> None:
        args = argparse.Namespace(
            machined="machined",
            min_revivers=2,
            min_post_failover_standbys=0,
            shutdown_reason=1,
            timeout_sec=1.0,
            poll_sec=0.0,
            cellappmgr_name="cellappmgr",
        )
        exe = Path("atlas_tool")
        manager = {"name": "cellappmgr", "pid": "200", "type": "cellappmgr"}
        old_leader = {"name": "reviver_a", "pid": "100", "type": "reviver"}
        standby = {"name": "reviver_b", "pid": "101", "type": "reviver"}
        leadership = verify_cellappmgr_ha.ReviverLeadership(old_leader, 1, 1, "leader=a")

        with (
            mock.patch.object(
                verify_cellappmgr_ha,
                "read_reviver_failover_baselines",
                return_value={"reviver_b": (0, 0, 0)},
            ),
            mock.patch.object(verify_cellappmgr_ha, "shutdown_process"),
            mock.patch.object(
                verify_cellappmgr_ha,
                "wait_until",
                side_effect=RuntimeError("waiting for standby Reviver"),
            ),
            mock.patch.object(verify_cellappmgr_ha, "list_processes", return_value=[standby]),
            mock.patch.object(
                verify_cellappmgr_ha, "active_reviver_leaders", return_value=[]
            ),
        ):
            new_leadership, result = verify_cellappmgr_ha.wait_for_reviver_leader_failover(
                args,
                exe,
                leadership,
                manager,
                [old_leader, standby],
                1,
                capture_unhealthy=True,
            )

        self.assertEqual(new_leadership.leader["name"], "reviver_a")
        self.assertFalse(result.healthy)
        self.assertEqual(result.old_leader, "reviver_a")
        self.assertEqual(result.new_leader, "")
        self.assertIn("reviver_failover=failed", result.status)


class ParseArgsTest(unittest.TestCase):
    def test_max_reviver_failover_ms_requires_failover_injection(self) -> None:
        with (
            mock.patch("sys.argv", ["verify_cellappmgr_ha.py", "--max-reviver-failover-ms", "1"]),
            mock.patch("sys.stderr", new_callable=io.StringIO),
            self.assertRaises(SystemExit),
        ):
            verify_cellappmgr_ha.parse_args()

    def test_rejects_negative_max_reviver_failover_ms(self) -> None:
        with (
            mock.patch(
                "sys.argv",
                [
                    "verify_cellappmgr_ha.py",
                    "--verify-reviver-failover",
                    "--max-reviver-failover-ms",
                    "-1",
                ],
            ),
            mock.patch("sys.stderr", new_callable=io.StringIO),
            self.assertRaises(SystemExit),
        ):
            verify_cellappmgr_ha.parse_args()

    def test_rejects_negative_max_load_report_age_ms(self) -> None:
        with (
            mock.patch("sys.argv", ["verify_cellappmgr_ha.py", "--max-load-report-age-ms", "-1"]),
            mock.patch("sys.stderr", new_callable=io.StringIO),
            self.assertRaises(SystemExit),
        ):
            verify_cellappmgr_ha.parse_args()

    def test_min_post_failover_standbys_requires_failover_injection(self) -> None:
        with (
            mock.patch(
                "sys.argv",
                ["verify_cellappmgr_ha.py", "--min-post-failover-standbys", "1"],
            ),
            mock.patch("sys.stderr", new_callable=io.StringIO),
            self.assertRaises(SystemExit),
        ):
            verify_cellappmgr_ha.parse_args()

    def test_min_post_failover_standbys_requires_enough_revivers(self) -> None:
        with (
            mock.patch(
                "sys.argv",
                [
                    "verify_cellappmgr_ha.py",
                    "--verify-reviver-failover",
                    "--min-revivers",
                    "2",
                    "--min-post-failover-standbys",
                    "1",
                ],
            ),
            mock.patch("sys.stderr", new_callable=io.StringIO),
            self.assertRaises(SystemExit),
        ):
            verify_cellappmgr_ha.parse_args()

    def test_reviver_failover_cycles_requires_failover_injection(self) -> None:
        with (
            mock.patch("sys.argv", ["verify_cellappmgr_ha.py", "--reviver-failover-cycles", "2"]),
            mock.patch("sys.stderr", new_callable=io.StringIO),
            self.assertRaises(SystemExit),
        ):
            verify_cellappmgr_ha.parse_args()

    def test_reviver_failover_cycles_requires_enough_revivers(self) -> None:
        with (
            mock.patch(
                "sys.argv",
                [
                    "verify_cellappmgr_ha.py",
                    "--verify-reviver-failover",
                    "--min-revivers",
                    "2",
                    "--reviver-failover-cycles",
                    "2",
                ],
            ),
            mock.patch("sys.stderr", new_callable=io.StringIO),
            self.assertRaises(SystemExit),
        ):
            verify_cellappmgr_ha.parse_args()


class MainFailureSummaryTest(unittest.TestCase):
    def test_writes_summary_json_when_preflight_fails(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            temp_path = Path(temp_dir)
            exe = temp_path / "atlas_tool.exe"
            exe.write_text("", encoding="utf-8")
            summary = temp_path / "summary.json"

            with (
                mock.patch(
                    "sys.argv",
                    [
                        "verify_cellappmgr_ha.py",
                        "--atlas-tool",
                        str(exe),
                        "--summary-json",
                        str(summary),
                    ],
                ),
                mock.patch("sys.stderr", new_callable=io.StringIO),
                mock.patch.object(
                    verify_cellappmgr_ha,
                    "list_processes",
                    side_effect=RuntimeError("machined unavailable"),
                ),
            ):
                rc = verify_cellappmgr_ha.main()

            payload = json.loads(summary.read_text(encoding="utf-8"))

        self.assertEqual(rc, 1)
        self.assertEqual(payload["mode"], "inject")
        self.assertEqual(payload["summary"]["run_unhealthy"], 1)
        self.assertEqual(payload["summary"]["gate_failures"], 1)
        self.assertFalse(payload["current"]["run_healthy"])
        self.assertEqual(payload["current"]["failure_stage"], "list_cellapps")
        self.assertEqual(payload["current"]["failure_detail"], "machined unavailable")
        self.assertEqual(payload["gates"]["run_health"]["metric"], "run.healthy")


class RecoveryHealthTest(unittest.TestCase):
    def _watchers(self, **overrides):
        defaults = {
            "cellappmgr/ha/recovery_window_active": "false",
            "cellappmgr/ha/recovery_window_status":
                "state=open active=0 remaining_ms=0 pending_geometry=0",
            "cellappmgr/lb/pending_geometry_broadcasts": "0",
            "cellappmgr/cellapp_count": "2",
            "cellappmgr/space_count": "1",
            "cellappmgr/lb/load_report_stale_count": "0",
            "cellappmgr/lb/cellapps": (
                "cellapps=2 app=1 addr=127.0.0.1:30001 load=0.100 entities=3 "
                "retiring=0 load_age_ms=15 load_stale=0 app=2 addr=127.0.0.1:30002 "
                "load=0.200 entities=4 retiring=0 load_age_ms=20 load_stale=0"
            ),
        }
        defaults.update(overrides)
        return defaults

    def _read(self, args, watchers):
        with (
            mock.patch.object(
                verify_cellappmgr_ha, "watcher_value",
                side_effect=lambda exe, machined, target, path: watchers[path]),
            mock.patch.object(
                verify_cellappmgr_ha, "int_watcher",
                side_effect=lambda exe, machined, target, path: int(watchers[path])),
        ):
            return verify_cellappmgr_ha.read_recovery_health(
                args, Path("atlas_tool"), "cellappmgr:cellappmgr", require_restored=True)

    def _args(self, min_cellapps=2):
        return argparse.Namespace(
            machined="m", min_cellapps=min_cellapps, max_load_report_age_ms=0)

    def test_recovered_when_window_closed_and_workers_rebuilt(self) -> None:
        report = self._read(self._args(), self._watchers())
        self.assertTrue(report.ok, report.status)
        self.assertTrue(report.payload["recovery_window"]["healthy"])
        self.assertTrue(report.payload["worker_rebuild"]["healthy"])
        self.assertFalse(report.stuck)

    def test_open_recovery_window_is_unhealthy(self) -> None:
        report = self._read(self._args(), self._watchers(**{
            "cellappmgr/ha/recovery_window_active": "true",
            "cellappmgr/ha/recovery_window_status":
                "state=closed active=1 remaining_ms=800 pending_geometry=0",
        }))
        self.assertFalse(report.ok)
        self.assertFalse(report.payload["recovery_window"]["healthy"])

    def test_missing_cellapps_fails_worker_rebuild(self) -> None:
        report = self._read(self._args(min_cellapps=3), self._watchers())
        self.assertFalse(report.ok)
        self.assertFalse(report.payload["worker_rebuild"]["healthy"])

    def test_pending_geometry_keeps_window_open(self) -> None:
        report = self._read(self._args(), self._watchers(**{
            "cellappmgr/lb/pending_geometry_broadcasts": "1",
        }))
        self.assertFalse(report.ok)
        self.assertFalse(report.payload["recovery_window"]["healthy"])

    def test_wait_or_capture_recovery_health_keeps_unhealthy_report(self) -> None:
        args = argparse.Namespace(machined="machined", timeout_sec=1.0, poll_sec=0.0)
        exe = Path("atlas_tool")
        report = verify_cellappmgr_ha.RecoveryHealthReport(
            False,
            "recovery window still open",
            False,
            {"healthy": False, "recovery_window": {"healthy": False}},
        )
        with (
            mock.patch.object(
                verify_cellappmgr_ha, "wait_for_recovery_health",
                side_effect=RuntimeError("recovery window still open")),
            mock.patch.object(
                verify_cellappmgr_ha, "read_recovery_health", return_value=report),
        ):
            captured = verify_cellappmgr_ha.wait_or_capture_recovery_health(
                args, exe, "cellappmgr:cellappmgr", True, capture_unhealthy=True)
        self.assertFalse(captured.ok)
        self.assertFalse(captured.payload["recovery_window"]["healthy"])


class LoadReportHealthTest(unittest.TestCase):
    def test_accepts_fresh_load_reports_for_all_cellapps(self) -> None:
        report = verify_cellappmgr_ha.build_load_report_health_report(
            0,
            (
                "cellapps=2 app=1 addr=127.0.0.1:30001 load=0.100 entities=3 "
                "retiring=0 load_age_ms=15 load_stale=0 app=2 addr=127.0.0.1:30002 "
                "load=0.200 entities=4 retiring=0 load_age_ms=20 load_stale=0"
            ),
            2,
        )

        self.assertTrue(report.ok, report.status)
        self.assertTrue(report.payload["healthy"])
        self.assertEqual(report.payload["reported_count"], 2)
        self.assertEqual(report.payload["record_count"], 2)
        self.assertIn("load_report_count=2/2", report.payload["detail"])
        self.assertEqual(report.payload["records"][0]["app_id"], 1)
        self.assertEqual(report.payload["records"][0]["load_age_ms"], 15)

    def test_rejects_stale_load_report(self) -> None:
        ok, detail = verify_cellappmgr_ha.load_report_health_detail(
            1,
            (
                "cellapps=1 app=1 addr=127.0.0.1:30001 load=0.100 entities=3 "
                "retiring=0 load_age_ms=5000 load_stale=1"
            ),
            1,
        )

        self.assertFalse(ok)
        self.assertIn("load_report_stale_apps=1", detail)

    def test_rejects_malformed_cellapp_summary(self) -> None:
        ok, detail = verify_cellappmgr_ha.load_report_health_detail(
            0, "cellapps=2 app=1 load_stale=0", 2
        )

        self.assertFalse(ok)
        self.assertIn("load_report_count_ok=False", detail)

    def test_rejects_load_report_over_age_slo(self) -> None:
        ok, detail = verify_cellappmgr_ha.load_report_health_detail(
            0,
            (
                "cellapps=1 app=1 addr=127.0.0.1:30001 load=0.100 entities=3 "
                "retiring=0 load_age_ms=250 load_stale=0"
            ),
            1,
            max_age_ms=100,
        )

        self.assertFalse(ok)
        self.assertIn("load_report_over_age_apps=1", detail)

    def test_can_defer_load_report_age_slo_to_summary_gate(self) -> None:
        report = verify_cellappmgr_ha.build_load_report_health_report(
            0,
            (
                "cellapps=1 app=1 addr=127.0.0.1:30001 load=0.100 entities=3 "
                "retiring=0 load_age_ms=250 load_stale=0"
            ),
            1,
            max_age_ms=100,
            enforce_max_age=False,
        )

        self.assertTrue(report.ok, report.status)
        self.assertFalse(report.payload["max_age_ok"])
        self.assertFalse(report.payload["max_age_enforced"])
        self.assertEqual(report.payload["over_age_apps"], ["1"])


class ReviverStabilityTest(unittest.TestCase):
    def test_wait_or_capture_reviver_stability_keeps_failure_status(self) -> None:
        args = argparse.Namespace()
        exe = Path("atlas_tool")

        with mock.patch.object(
            verify_cellappmgr_ha,
            "wait_for_reviver_stability",
            side_effect=RuntimeError("reviver status changed"),
        ):
            report = verify_cellappmgr_ha.wait_or_capture_reviver_stability(
                args,
                exe,
                "reviver:reviver",
                "100",
                2,
                3,
                capture_unhealthy=True,
            )

        self.assertFalse(report.ok)
        self.assertIn("stability=failed", report.status)


class HaSummaryPayloadTest(unittest.TestCase):
    def recovery_payload(self) -> dict[str, object]:
        return {
            "healthy": True,
            "recovery_window": {"healthy": True, "active": False},
            "worker_rebuild": {"healthy": True, "cellapp_count": 2},
            "load_report": {
                "healthy": True,
                "records": [{"app_id": 1, "load_age_ms": 15}],
            },
        }

    def results(self) -> list[verify_cellappmgr_ha.HaCycleResult]:
        return [
            verify_cellappmgr_ha.HaCycleResult(
                cycle=1,
                old_pid="100",
                new_pid="200",
                generation_before=3,
                generation_after=4,
                launch_count_before=5,
                launch_count_after=6,
                pre_topology_status="pre_topology=ok",
                restored_topology_status="restored_topology=ok",
                output_status="output_log=ok",
                stability_status="stability=5s",
                manager_restart_ms=1200,
                reviver_retarget_ms=1600,
                restore_converged_ms=2200,
                takeover_elapsed_ms=2500,
                recovery_status="recovery=ok",
                recovery=self.recovery_payload(),
                load_report_status="load_report=ok",
                load_report={
                    "healthy": True,
                    "records": [{"app_id": 1, "load_age_ms": 15}],
                    "stale_apps": [],
                    "over_age_apps": [],
                },
            ),
            verify_cellappmgr_ha.HaCycleResult(
                cycle=2,
                old_pid="200",
                new_pid="300",
                generation_before=4,
                generation_after=6,
                launch_count_before=6,
                launch_count_after=8,
                pre_topology_status="pre_topology=ok",
                restored_topology_status="restored_topology=ok",
                output_status="output_log=ok",
                stability_status="stability=5s",
                manager_restart_ms=1300,
                reviver_retarget_ms=1700,
                restore_converged_ms=2400,
                takeover_elapsed_ms=3500,
                recovery_status="recovery=ok",
                recovery=self.recovery_payload(),
                load_report_status="load_report=ok",
                load_report={
                    "healthy": True,
                    "records": [{"app_id": 2, "load_age_ms": 25}],
                    "stale_apps": [],
                    "over_age_apps": [],
                },
            ),
        ]

    def reviver_failovers(self) -> list[verify_cellappmgr_ha.ReviverFailoverResult]:
        return [
            verify_cellappmgr_ha.ReviverFailoverResult(
                cycle=1,
                old_leader="reviver_a",
                new_leader="reviver_b",
                old_pid="100",
                new_pid="101",
                manager_pid="200",
                surviving_revivers=2,
                standby_after=1,
                generation_before=0,
                generation_after=1,
                launch_count_before=0,
                launch_count_after=0,
                heartbeat_acks_before=0,
                heartbeat_acks_after=1,
                failover_elapsed_ms=450,
                leadership_status="revivers=2 active=1 standby=1 leader=reviver_b",
                standby_health_status="standby_revivers=1 standby_health=ok",
                standby_health=[{"name": "reviver_c", "healthy": True}],
                status="reviver_failover=ok",
            ),
            verify_cellappmgr_ha.ReviverFailoverResult(
                cycle=2,
                old_leader="reviver_b",
                new_leader="reviver_c",
                old_pid="101",
                new_pid="102",
                manager_pid="200",
                surviving_revivers=1,
                standby_after=0,
                generation_before=0,
                generation_after=1,
                launch_count_before=0,
                launch_count_after=0,
                heartbeat_acks_before=0,
                heartbeat_acks_after=1,
                failover_elapsed_ms=900,
                leadership_status="revivers=1 active=1 standby=0 leader=reviver_c",
                standby_health_status="standby_revivers=0 standby_health=none",
                standby_health=[],
                status="reviver_failover=ok",
            ),
        ]

    def test_summarizes_ha_cycles(self) -> None:
        summary = verify_cellappmgr_ha.summarize_ha_cycles(self.results())

        self.assertEqual(summary["cycles"], 2)
        self.assertEqual(summary["successful_cycles"], 2)
        self.assertEqual(summary["failed_cycles"], 0)
        self.assertEqual(summary["success_rate"], 1.0)
        self.assertEqual(summary["pid_changes"], 2)
        self.assertEqual(summary["generation_delta"], 3)
        self.assertEqual(summary["launch_count_delta"], 3)
        self.assertEqual(summary["takeover_health_checks"], 2)
        self.assertEqual(summary["takeover_healthy"], 2)
        self.assertEqual(summary["takeover_unhealthy"], 0)
        self.assertEqual(summary["stability_health_checks"], 2)
        self.assertEqual(summary["stability_healthy"], 2)
        self.assertEqual(summary["stability_unhealthy"], 0)
        self.assertEqual(summary["recovery_health_checks"], 2)
        self.assertEqual(summary["recovery_healthy"], 2)
        self.assertEqual(summary["recovery_unhealthy"], 0)
        self.assertEqual(summary["load_report_health_checks"], 2)
        self.assertEqual(summary["load_report_healthy"], 2)
        self.assertEqual(summary["load_report_unhealthy"], 0)
        self.assertEqual(summary["load_report_records"], 2)
        self.assertEqual(summary["max_load_report_age_ms"], 25)
        self.assertEqual(summary["load_report_stale_apps"], 0)
        self.assertEqual(summary["load_report_over_age_apps"], 0)
        self.assertEqual(summary["takeover_latency_samples"], 2)
        self.assertEqual(summary["avg_takeover_ms"], 3000.0)
        self.assertEqual(summary["p95_takeover_ms"], 3500)

    def test_summary_gates_reject_unhealthy_cycle_recovery(self) -> None:
        results = self.results()
        results[1] = results[1]._replace(
            recovery_status="recovery window still open",
            recovery={
                "healthy": False,
                "recovery_window": {"healthy": False},
                "load_report": {"healthy": True, "records": []},
            },
        )

        payload = verify_cellappmgr_ha.build_summary_payload("inject", results)
        gate = payload["gates"]["cycle_recovery_health"]

        self.assertEqual(payload["summary"]["success_rate"], 0.5)
        self.assertEqual(payload["summary"]["cycle_failure_stages"], ["cycle_recovery"])
        self.assertEqual(payload["summary"]["failure_stages"], ["cycle_recovery"])
        self.assertEqual(payload["summary"]["first_failure_stage"], "cycle_recovery")
        self.assertEqual(payload["summary"]["first_failed_gate"], "cycle_recovery_health")
        self.assertEqual(gate["metric"], "recovery_unhealthy")
        self.assertEqual(gate["value"], 1)
        self.assertEqual(gate["maximum"], 0)
        with self.assertRaisesRegex(RuntimeError, "recovery_unhealthy"):
            verify_cellappmgr_ha.validate_summary_gates(payload)

    def test_summary_gates_reject_unhealthy_cycle_load_report(self) -> None:
        results = self.results()
        results[0] = results[0]._replace(
            recovery_status="load report stale",
            recovery={
                "healthy": False,
                "recovery_window": {"healthy": True},
                "worker_rebuild": {"healthy": True},
                "load_report": {"healthy": False, "records": []},
            },
            load_report_status="load_report_stale_count=1",
            load_report={
                "healthy": False,
                "records": [{"app_id": 1, "load_age_ms": 15}],
                "stale_apps": ["1"],
                "over_age_apps": [],
            },
        )

        payload = verify_cellappmgr_ha.build_summary_payload("inject", results)
        gate = payload["gates"]["cycle_load_report_health"]

        self.assertEqual(payload["summary"]["success_rate"], 0.5)
        self.assertEqual(payload["summary"]["successful_cycles"], 1)
        self.assertEqual(payload["summary"]["failed_cycles"], 1)
        self.assertEqual(payload["summary"]["load_report_unhealthy"], 1)
        self.assertEqual(
            payload["summary"]["failed_gate_names"],
            ["cycle_load_report_health", "cycle_recovery_health"],
        )
        self.assertEqual(
            payload["summary"]["failed_gates"][0],
            {
                "name": "cycle_load_report_health",
                "metric": "load_report_unhealthy",
                "value": 1,
                "maximum": 0,
                "ok": False,
            },
        )
        self.assertEqual(payload["summary"]["cycle_failure_stages"], ["cycle_load_report"])
        self.assertEqual(payload["summary"]["failure_stages"], ["cycle_load_report"])
        self.assertEqual(payload["summary"]["first_failure_stage"], "cycle_load_report")
        self.assertFalse(payload["cycles"][0]["healthy"])
        self.assertEqual(payload["cycles"][0]["failure_stages"], ["cycle_load_report"])
        self.assertEqual(payload["cycles"][0]["first_failure_stage"], "cycle_load_report")
        self.assertEqual(
            payload["summary"]["first_failed_gate"],
            "cycle_load_report_health",
        )
        self.assertEqual(gate["metric"], "load_report_unhealthy")
        self.assertEqual(gate["value"], 1)
        self.assertEqual(gate["maximum"], 0)
        with self.assertRaisesRegex(RuntimeError, "load_report_unhealthy"):
            verify_cellappmgr_ha.validate_summary_gates(payload)

    def test_summary_gates_reject_unhealthy_cycle_stability(self) -> None:
        results = self.results()
        results[0] = results[0]._replace(
            stability_status="stability=failed detail=heartbeat timeout",
            stability_healthy=False,
        )

        payload = verify_cellappmgr_ha.build_summary_payload("inject", results)
        gate = payload["gates"]["cycle_stability_health"]

        self.assertEqual(payload["summary"]["success_rate"], 0.5)
        self.assertEqual(payload["summary"]["cycle_failure_stages"], ["cycle_stability"])
        self.assertEqual(payload["summary"]["failure_stages"], ["cycle_stability"])
        self.assertEqual(payload["summary"]["first_failure_stage"], "cycle_stability")
        self.assertEqual(payload["summary"]["first_failed_gate"], "cycle_stability_health")
        self.assertEqual(gate["metric"], "stability_unhealthy")
        self.assertEqual(gate["value"], 1)
        self.assertEqual(gate["maximum"], 0)
        with self.assertRaisesRegex(RuntimeError, "stability_unhealthy"):
            verify_cellappmgr_ha.validate_summary_gates(payload)

    def test_summary_gates_reject_unhealthy_cycle_takeover(self) -> None:
        results = self.results()
        results[0] = results[0]._replace(
            takeover_healthy=False,
            failure_stage="restart",
        )

        payload = verify_cellappmgr_ha.build_summary_payload("inject", results)
        gate = payload["gates"]["cycle_takeover_health"]

        self.assertEqual(payload["summary"]["success_rate"], 0.5)
        self.assertEqual(payload["summary"]["cycle_failure_stages"], ["restart"])
        self.assertEqual(payload["summary"]["failure_stages"], ["restart"])
        self.assertEqual(payload["summary"]["first_failure_stage"], "restart")
        self.assertEqual(payload["summary"]["first_failed_gate"], "cycle_takeover_health")
        self.assertFalse(payload["cycles"][0]["healthy"])
        self.assertEqual(payload["cycles"][0]["failure_stages"], ["restart"])
        self.assertEqual(payload["cycles"][0]["first_failure_stage"], "restart")
        self.assertEqual(gate["metric"], "takeover_unhealthy")
        self.assertEqual(gate["value"], 1)
        self.assertEqual(gate["maximum"], 0)
        with self.assertRaisesRegex(RuntimeError, "takeover_unhealthy"):
            verify_cellappmgr_ha.validate_summary_gates(payload)

    def test_failed_ha_cycle_result_records_stage(self) -> None:
        result = verify_cellappmgr_ha.failed_ha_cycle_result(
            3,
            "100",
            7,
            9,
            time.monotonic(),
            "restart",
            "timeout",
        )

        self.assertFalse(result.takeover_healthy)
        self.assertEqual(result.failure_stage, "restart")
        self.assertEqual(result.generation_after, 7)
        self.assertIn("takeover=failed", result.recovery_status)

    def test_builds_machine_readable_summary_payload(self) -> None:
        parameters = verify_cellappmgr_ha.summary_parameters(
            argparse.Namespace(
                build="debug",
                machined="127.0.0.1:20018",
                atlas_tool=Path("bin/debug/atlas_tool.exe"),
                cellappmgr_name="cellappmgr",
                reviver_name="reviver",
                min_revivers=2,
                min_cellapps=2,
                timeout_sec=90.0,
                poll_sec=0.5,
                stability_sec=5.0,
                cycles=2,
                max_takeover_ms=8000,
                max_reviver_failover_ms=6000,
                max_load_report_age_ms=2000,
                min_post_failover_standbys=1,
                shutdown_reason=1,
                allow_empty_cluster=False,
                allow_empty_output_log=False,
                allow_topology_change=False,
                no_inject=False,
                verify_reviver_failover=True,
                reviver_failover_cycles=1,
            )
        )
        payload = verify_cellappmgr_ha.build_summary_payload(
            "inject", self.results(), parameters=parameters
        )

        self.assertEqual(payload["schema_version"], 2)
        self.assertEqual(payload["mode"], "inject")
        self.assertEqual(payload["parameters"]["min_revivers"], 2)
        self.assertEqual(payload["parameters"]["max_takeover_ms"], 8000)
        self.assertEqual(payload["parameters"]["max_load_report_age_ms"], 2000)
        self.assertEqual(
            payload["parameters"]["atlas_tool"], str(Path("bin/debug/atlas_tool.exe"))
        )
        self.assertEqual(payload["summary"]["cycles"], 2)
        self.assertEqual(payload["summary"]["generation_delta"], 3)
        self.assertEqual(payload["summary"]["max_takeover_ms"], 3500)
        self.assertEqual(payload["summary"]["gate_count"], 2)
        self.assertEqual(payload["summary"]["gate_failures"], 0)
        self.assertEqual(payload["summary"]["failed_gate_names"], [])
        self.assertEqual(payload["summary"]["failed_gates"], [])
        self.assertEqual(payload["summary"]["first_failed_gate"], "")
        self.assertEqual(payload["summary"]["run_failure_stage"], "")
        self.assertEqual(payload["summary"]["current_failure_stages"], [])
        self.assertEqual(payload["summary"]["cycle_failure_stages"], [])
        self.assertEqual(payload["summary"]["reviver_failover_failure_stages"], [])
        self.assertEqual(payload["summary"]["failure_stages"], [])
        self.assertEqual(payload["summary"]["first_failure_stage"], "")
        self.assertTrue(payload["summary"]["overall_healthy"])
        self.assertEqual(payload["summary"]["overall_success_rate"], 1.0)
        takeover_gate = payload["gates"]["max_takeover_ms"]
        self.assertEqual(takeover_gate["metric"], "max_takeover_ms")
        self.assertEqual(takeover_gate["value"], 3500)
        self.assertEqual(takeover_gate["maximum"], 8000)
        self.assertTrue(takeover_gate["ok"])
        load_gate = payload["gates"]["max_load_report_age_ms"]
        self.assertEqual(load_gate["metric"], "max_load_report_age_ms")
        self.assertEqual(load_gate["value"], 25)
        self.assertEqual(load_gate["maximum"], 2000)
        self.assertTrue(load_gate["ok"])
        self.assertEqual(payload["cycles"][0]["old_pid"], "100")
        self.assertEqual(payload["cycles"][0]["manager_restart_ms"], 1200)
        self.assertTrue(payload["cycles"][0]["healthy"])
        self.assertEqual(payload["cycles"][0]["failure_stages"], [])
        self.assertEqual(payload["cycles"][0]["first_failure_stage"], "")
        self.assertTrue(payload["cycles"][0]["recovery"]["healthy"])
        self.assertEqual(payload["cycles"][0]["load_report"]["records"][0]["app_id"], 1)

    def test_summary_gates_report_reviver_failover_slo_failures(self) -> None:
        payload = verify_cellappmgr_ha.build_summary_payload(
            "no-inject",
            [],
            {"reviver_topology": {"registered_revivers": 2}},
            self.reviver_failovers(),
            {
                "max_reviver_failover_ms": 600,
                "min_post_failover_standbys": 1,
                "min_revivers": 2,
            },
        )

        failover_gate = payload["gates"]["max_reviver_failover_ms"]
        self.assertEqual(failover_gate["metric"], "max_reviver_failover_ms")
        self.assertEqual(failover_gate["value"], 900)
        self.assertEqual(failover_gate["maximum"], 600)
        self.assertFalse(failover_gate["ok"])
        standby_gate = payload["gates"]["min_post_failover_standbys"]
        self.assertEqual(standby_gate["value"], 0)
        self.assertEqual(standby_gate["minimum"], 1)
        self.assertFalse(standby_gate["ok"])
        reviver_gate = payload["gates"]["min_revivers"]
        self.assertEqual(reviver_gate["value"], 2)
        self.assertTrue(reviver_gate["ok"])

    def test_summary_gates_reject_unhealthy_reviver_failover(self) -> None:
        failovers = self.reviver_failovers()
        failovers[0] = failovers[0]._replace(
            status="reviver_failover=failed detail=timeout",
            healthy=False,
        )

        payload = verify_cellappmgr_ha.build_summary_payload(
            "no-inject",
            [],
            {},
            failovers,
        )
        gate = payload["gates"]["reviver_failover_health"]

        self.assertEqual(payload["summary"]["success_rate"], 0.0)
        self.assertEqual(
            payload["summary"]["reviver_failover_failure_stages"],
            ["reviver_failover"],
        )
        self.assertEqual(payload["summary"]["failure_stages"], ["reviver_failover"])
        self.assertEqual(payload["summary"]["first_failure_stage"], "reviver_failover")
        self.assertEqual(
            payload["summary"]["first_failed_gate"],
            "reviver_failover_health",
        )
        self.assertEqual(gate["metric"], "reviver_failover_unhealthy")
        self.assertEqual(gate["value"], 1)
        self.assertEqual(gate["maximum"], 0)
        with self.assertRaisesRegex(RuntimeError, "reviver_failover_unhealthy"):
            verify_cellappmgr_ha.validate_summary_gates(payload)

    def test_summary_gates_use_current_load_report_for_no_inject(self) -> None:
        payload = verify_cellappmgr_ha.build_summary_payload(
            "no-inject",
            [],
            {"load_report": {"records": [{"app_id": 1, "load_age_ms": 25}]}},
            parameters={"max_load_report_age_ms": 20},
        )

        gate = payload["gates"]["max_load_report_age_ms"]
        self.assertEqual(gate["value"], 25)
        self.assertEqual(gate["maximum"], 20)
        self.assertFalse(gate["ok"])

    def test_summary_gates_include_current_load_report_after_inject(self) -> None:
        payload = verify_cellappmgr_ha.build_summary_payload(
            "inject",
            self.results(),
            {"load_report": {"records": [{"app_id": 1, "load_age_ms": 80}]}},
            parameters={"max_load_report_age_ms": 50},
        )

        gate = payload["gates"]["max_load_report_age_ms"]
        self.assertEqual(gate["value"], 80)
        self.assertEqual(gate["maximum"], 50)
        self.assertFalse(gate["ok"])

    def test_validate_summary_gates_rejects_failed_gate(self) -> None:
        payload = verify_cellappmgr_ha.build_summary_payload(
            "inject",
            self.results(),
            parameters={"max_takeover_ms": 3000},
        )

        with self.assertRaises(RuntimeError) as caught:
            verify_cellappmgr_ha.validate_summary_gates(payload)
        message = str(caught.exception)
        self.assertIn("max_takeover_ms", message)
        self.assertIn("first_failed_gate=max_takeover_ms", message)
        self.assertNotIn("first_failure_stage=", message)

    def test_summary_gates_reject_unhealthy_final_standby(self) -> None:
        payload = verify_cellappmgr_ha.build_summary_payload(
            "inject",
            self.results(),
            {
                "reviver_topology": {
                    "registered_revivers": 2,
                    "standby_health_ok": False,
                    "standby_health": [{"name": "reviver_c", "healthy": False}],
                }
            },
            parameters={"min_revivers": 2},
        )

        gate = payload["gates"]["reviver_standby_health"]
        self.assertEqual(
            payload["summary"]["current_failure_stages"],
            ["reviver_standby_health"],
        )
        self.assertEqual(payload["summary"]["failure_stages"], ["reviver_standby_health"])
        self.assertEqual(
            payload["summary"]["first_failure_stage"],
            "reviver_standby_health",
        )
        self.assertEqual(
            payload["summary"]["first_failed_gate"],
            "reviver_standby_health",
        )
        self.assertEqual(gate["metric"], "standby_health_ok")
        self.assertFalse(gate["value"])
        self.assertTrue(gate["expected"])
        with self.assertRaisesRegex(RuntimeError, "standby_health_ok"):
            verify_cellappmgr_ha.validate_summary_gates(payload)

    def test_summary_gates_reject_unhealthy_current_recovery(self) -> None:
        payload = verify_cellappmgr_ha.build_summary_payload(
            "no-inject",
            [],
            {"recovery": {"healthy": False}},
        )

        gate = payload["gates"]["recovery_health"]
        self.assertEqual(payload["summary"]["success_rate"], 0.0)
        self.assertEqual(payload["summary"]["current_failure_stages"], ["recovery"])
        self.assertEqual(payload["summary"]["failure_stages"], ["recovery"])
        self.assertEqual(payload["summary"]["first_failure_stage"], "recovery")
        self.assertEqual(payload["summary"]["first_failed_gate"], "recovery_health")
        self.assertEqual(gate["metric"], "recovery.healthy")
        self.assertFalse(gate["value"])
        self.assertTrue(gate["expected"])
        with self.assertRaisesRegex(RuntimeError, "recovery\\.healthy"):
            verify_cellappmgr_ha.validate_summary_gates(payload)

    def test_summary_gates_reject_unhealthy_current_load_report(self) -> None:
        recovery = {
            "healthy": False,
            "recovery_window": {"healthy": True},
            "worker_rebuild": {"healthy": True},
            "load_report": {"healthy": False, "records": []},
        }
        payload = verify_cellappmgr_ha.build_summary_payload(
            "no-inject",
            [],
            {"recovery": recovery, "load_report": recovery["load_report"]},
        )

        gate = payload["gates"]["load_report_health"]
        self.assertEqual(payload["summary"]["success_rate"], 0.0)
        self.assertEqual(payload["summary"]["current_failure_stages"], ["load_report"])
        self.assertEqual(payload["summary"]["failure_stages"], ["load_report"])
        self.assertEqual(payload["summary"]["first_failure_stage"], "load_report")
        self.assertEqual(payload["summary"]["first_failed_gate"], "load_report_health")
        self.assertEqual(
            payload["summary"]["failed_gate_names"],
            ["load_report_health", "recovery_health"],
        )
        self.assertEqual(gate["metric"], "load_report.healthy")
        self.assertFalse(gate["value"])
        self.assertTrue(gate["expected"])
        with self.assertRaisesRegex(RuntimeError, "load_report\\.healthy"):
            verify_cellappmgr_ha.validate_summary_gates(payload)

    def test_summary_gates_reject_unhealthy_current_stability(self) -> None:
        payload = verify_cellappmgr_ha.build_summary_payload(
            "no-inject",
            [],
            {"stability_healthy": False},
        )

        gate = payload["gates"]["stability_health"]
        self.assertEqual(payload["summary"]["success_rate"], 0.0)
        self.assertEqual(payload["summary"]["current_failure_stages"], ["stability"])
        self.assertEqual(payload["summary"]["failure_stages"], ["stability"])
        self.assertEqual(payload["summary"]["first_failure_stage"], "stability")
        self.assertEqual(payload["summary"]["first_failed_gate"], "stability_health")
        self.assertEqual(gate["metric"], "stability.healthy")
        self.assertFalse(gate["value"])
        self.assertTrue(gate["expected"])
        with self.assertRaisesRegex(RuntimeError, "stability\\.healthy"):
            verify_cellappmgr_ha.validate_summary_gates(payload)

    def test_summary_gates_reject_failed_run(self) -> None:
        payload = verify_cellappmgr_ha.build_summary_payload(
            "inject",
            [],
            verify_cellappmgr_ha.failed_run_current_payload(
                "list_cellapps", "machined unavailable"
            ),
        )

        gate = payload["gates"]["run_health"]
        self.assertEqual(payload["summary"]["success_rate"], 0.0)
        self.assertEqual(payload["summary"]["run_health_checks"], 1)
        self.assertEqual(payload["summary"]["run_healthy"], 0)
        self.assertEqual(payload["summary"]["run_unhealthy"], 1)
        self.assertEqual(payload["summary"]["gate_failures"], 1)
        self.assertEqual(payload["summary"]["failed_gate_names"], ["run_health"])
        self.assertEqual(payload["summary"]["failed_gates"][0]["name"], "run_health")
        self.assertEqual(payload["summary"]["failed_gates"][0]["metric"], "run.healthy")
        self.assertEqual(payload["summary"]["failed_gates"][0]["stage"], "list_cellapps")
        self.assertEqual(payload["summary"]["run_failure_stage"], "list_cellapps")
        self.assertEqual(payload["summary"]["failure_stages"], ["list_cellapps"])
        self.assertEqual(payload["summary"]["first_failure_stage"], "list_cellapps")
        self.assertEqual(payload["summary"]["first_failed_gate"], "run_health")
        self.assertFalse(payload["summary"]["overall_healthy"])
        self.assertEqual(payload["summary"]["overall_success_rate"], 0.0)
        self.assertEqual(gate["metric"], "run.healthy")
        self.assertFalse(gate["value"])
        self.assertTrue(gate["expected"])
        self.assertEqual(gate["stage"], "list_cellapps")
        self.assertEqual(gate["detail"], "machined unavailable")
        with self.assertRaises(RuntimeError) as caught:
            verify_cellappmgr_ha.validate_summary_gates(payload)
        message = str(caught.exception)
        self.assertIn("run.healthy", message)
        self.assertIn("first_failed_gate=run_health", message)
        self.assertIn("first_failure_stage=list_cellapps", message)

    def test_overall_health_fails_even_when_cycles_succeed(self) -> None:
        payload = verify_cellappmgr_ha.build_summary_payload(
            "inject",
            self.results(),
            verify_cellappmgr_ha.failed_run_current_payload(
                "final_recovery", "recovery window stayed open"
            ),
        )

        self.assertEqual(payload["summary"]["success_rate"], 1.0)
        self.assertFalse(payload["summary"]["overall_healthy"])
        self.assertEqual(payload["summary"]["overall_success_rate"], 0.0)
        self.assertEqual(payload["summary"]["gate_failures"], 1)
        self.assertEqual(payload["summary"]["failed_gate_names"], ["run_health"])
        self.assertEqual(payload["summary"]["run_failure_stage"], "final_recovery")
        self.assertEqual(payload["summary"]["failure_stages"], ["final_recovery"])
        self.assertEqual(payload["summary"]["first_failure_stage"], "final_recovery")
        self.assertEqual(payload["summary"]["first_failed_gate"], "run_health")
        self.assertEqual(payload["gates"]["run_health"]["stage"], "final_recovery")

    def test_builds_reviver_failover_summary_payload(self) -> None:
        payload = verify_cellappmgr_ha.build_summary_payload(
            "no-inject",
            [],
            {"pid": "200"},
            self.reviver_failovers(),
        )

        self.assertEqual(payload["summary"]["reviver_failovers"], 2)
        self.assertEqual(payload["summary"]["min_surviving_revivers"], 1)
        self.assertEqual(payload["summary"]["min_post_failover_standbys"], 0)
        self.assertEqual(payload["summary"]["max_post_failover_standbys"], 1)
        self.assertEqual(payload["summary"]["reviver_failover_latency_samples"], 2)
        self.assertEqual(payload["summary"]["max_reviver_failover_ms"], 900)
        self.assertEqual(payload["reviver_failovers"][0]["cycle"], 1)
        self.assertEqual(payload["reviver_failovers"][0]["standby_after"], 1)
        self.assertEqual(payload["reviver_failovers"][0]["new_leader"], "reviver_b")

    def test_no_inject_summary_records_current_watchers(self) -> None:
        payload = verify_cellappmgr_ha.build_summary_payload(
            "no-inject",
            [],
            {
                "pid": "100",
                "stability_status": "stability=5s",
                "reviver_topology": {"registered_revivers": 2, "standby_count": 1},
            },
        )

        self.assertEqual(payload["mode"], "no-inject")
        self.assertEqual(payload["summary"]["cycles"], 0)
        self.assertEqual(payload["summary"]["success_rate"], 1.0)
        self.assertEqual(payload["summary"]["run_healthy"], 1)
        self.assertEqual(payload["summary"]["gate_count"], 0)
        self.assertEqual(payload["summary"]["gate_failures"], 0)
        self.assertEqual(payload["summary"]["failed_gate_names"], [])
        self.assertEqual(payload["summary"]["failed_gates"], [])
        self.assertEqual(payload["summary"]["first_failed_gate"], "")
        self.assertEqual(payload["summary"]["current_failure_stages"], [])
        self.assertEqual(payload["summary"]["cycle_failure_stages"], [])
        self.assertEqual(payload["summary"]["reviver_failover_failure_stages"], [])
        self.assertEqual(payload["summary"]["failure_stages"], [])
        self.assertEqual(payload["summary"]["first_failure_stage"], "")
        self.assertTrue(payload["summary"]["overall_healthy"])
        self.assertEqual(payload["summary"]["overall_success_rate"], 1.0)
        self.assertEqual(payload["current"]["pid"], "100")
        self.assertEqual(payload["current"]["reviver_topology"]["standby_count"], 1)

    def test_writes_summary_json_without_leaving_temp_file(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "nested" / "summary.json"
            path.parent.mkdir()
            path.write_text("stale\n", encoding="utf-8")

            verify_cellappmgr_ha.write_summary_json(
                path, "inject", self.results(), parameters={"max_takeover_ms": 8000}
            )

            payload = json.loads(path.read_text(encoding="utf-8"))
            temp_files = list(path.parent.glob(f".{path.name}.*.tmp"))

        self.assertEqual(payload["summary"]["launch_count_delta"], 3)
        self.assertEqual(payload["parameters"]["max_takeover_ms"], 8000)
        self.assertEqual(temp_files, [])


if __name__ == "__main__":
    unittest.main()
