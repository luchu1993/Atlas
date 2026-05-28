#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import io
import json
import tempfile
import unittest
from argparse import Namespace
from unittest import mock
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "cluster_control" / "verify_cellapp_rehome.py"
SPEC = importlib.util.spec_from_file_location("verify_cellapp_rehome", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
verify_cellapp_rehome = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verify_cellapp_rehome)


def snapshot(
    notifications: int = 0,
    scheduled: int = 0,
    payload_scheduled: int | None = None,
    ghost_backup_scheduled: int = 0,
    restored: int = 0,
    lost: int = 0,
    timeouts: int = 0,
    pending: int = 0,
    last_elapsed_ms: int = 0,
    max_elapsed_ms: int = 0,
) -> dict[str, dict[str, int]]:
    if payload_scheduled is None:
        payload_scheduled = scheduled
    return {
        "BaseApp01": {
            "notifications": notifications,
            "scheduled": scheduled,
            "payload_scheduled": payload_scheduled,
            "ghost_backup_scheduled": ghost_backup_scheduled,
            "restored": restored,
            "lost": lost,
            "timeouts": timeouts,
            "pending": pending,
            "last_elapsed_ms": last_elapsed_ms,
            "max_elapsed_ms": max_elapsed_ms,
        }
    }


def cell_snapshot(
    total: int = 0,
    payload: int = 0,
    ghost_backup: int = 0,
    empty: int = 0,
    failures: int = 0,
    promoted: int = 0,
) -> dict[str, dict[str, int]]:
    return {
        "cellapp_01": {
            "total": total,
            "payload": payload,
            "ghost_backup": ghost_backup,
            "empty": empty,
            "failures": failures,
            "promoted": promoted,
        }
    }


class BaseAppRestoreHealthTest(unittest.TestCase):
    def test_accepts_notification_without_entity_restore_work(self) -> None:
        ok, detail = verify_cellapp_rehome.baseapp_restore_health_detail(
            snapshot(), snapshot(notifications=1)
        )

        self.assertTrue(ok, detail)

    def test_accepts_completed_scheduled_restores(self) -> None:
        ok, detail = verify_cellapp_rehome.baseapp_restore_health_detail(
            snapshot(notifications=3, scheduled=7, restored=7),
            snapshot(notifications=4, scheduled=9, restored=9),
        )

        self.assertTrue(ok, detail)
        self.assertIn("scheduled:2 restored:2", detail)
        self.assertIn("payload_scheduled:2 ghost_backup_scheduled:0", detail)

    def test_rejects_scheduled_restore_source_mismatch(self) -> None:
        ok, detail = verify_cellapp_rehome.baseapp_restore_health_detail(
            snapshot(),
            snapshot(
                notifications=1,
                scheduled=2,
                payload_scheduled=1,
                ghost_backup_scheduled=0,
                restored=2,
            ),
        )

        self.assertFalse(ok)
        self.assertIn("scheduled:2 restored:2 payload_scheduled:1", detail)

    def test_rejects_lost_or_timed_out_restore(self) -> None:
        ok, detail = verify_cellapp_rehome.baseapp_restore_health_detail(
            snapshot(), snapshot(notifications=1, lost=1, timeouts=1)
        )

        self.assertFalse(ok)
        self.assertIn("lost:1 timeouts:1", detail)

    def test_rejects_pending_restore(self) -> None:
        ok, detail = verify_cellapp_rehome.baseapp_restore_health_detail(
            snapshot(), snapshot(notifications=1, scheduled=1, pending=1)
        )

        self.assertFalse(ok)
        self.assertIn("pending:1", detail)

    def test_rejects_unrestored_scheduled_restore(self) -> None:
        ok, detail = verify_cellapp_rehome.baseapp_restore_health_detail(
            snapshot(), snapshot(notifications=1, scheduled=1)
        )

        self.assertFalse(ok)
        self.assertIn("scheduled:1 restored:0", detail)

    def test_rejects_missing_death_notification(self) -> None:
        ok, detail = verify_cellapp_rehome.baseapp_restore_health_detail(snapshot(), snapshot())

        self.assertFalse(ok)
        self.assertIn("notif:0", detail)

    def test_accepts_restore_elapsed_under_limit_with_historical_max(self) -> None:
        ok, detail = verify_cellapp_rehome.baseapp_restore_health_detail(
            snapshot(max_elapsed_ms=9000),
            snapshot(
                notifications=1,
                scheduled=1,
                restored=1,
                last_elapsed_ms=250,
                max_elapsed_ms=9000,
            ),
            max_restore_ms=500,
        )

        self.assertTrue(ok, detail)

    def test_rejects_restore_elapsed_over_limit(self) -> None:
        ok, detail = verify_cellapp_rehome.baseapp_restore_health_detail(
            snapshot(),
            snapshot(
                notifications=1,
                scheduled=1,
                restored=1,
                last_elapsed_ms=750,
                max_elapsed_ms=750,
            ),
            max_restore_ms=500,
        )

        self.assertFalse(ok)
        self.assertIn("last_ms:750 max_ms:750", detail)


class BaseAppRestoreElapsedTest(unittest.TestCase):
    def test_uses_only_baseapps_with_scheduled_delta(self) -> None:
        before = {
            "BaseApp01": snapshot(scheduled=1)["BaseApp01"],
            "BaseApp02": snapshot(scheduled=5)["BaseApp01"],
        }
        after = {
            "BaseApp01": snapshot(
                scheduled=2, restored=2, last_elapsed_ms=250
            )["BaseApp01"],
            "BaseApp02": snapshot(
                scheduled=5, restored=5, last_elapsed_ms=9000
            )["BaseApp01"],
        }

        elapsed_ms = verify_cellapp_rehome.baseapp_restore_cycle_elapsed_ms(before, after)

        self.assertEqual(elapsed_ms, 250)


class CellAppRestoreHealthTest(unittest.TestCase):
    def test_accepts_payload_and_ghost_backup_covering_scheduled_restores(self) -> None:
        ok, detail = verify_cellapp_rehome.cellapp_restore_health_detail(
            cell_snapshot(),
            cell_snapshot(total=2, payload=1, ghost_backup=1, promoted=1),
            2,
            1,
            1,
        )

        self.assertTrue(ok, detail)

    def test_rejects_missing_restore_source_delta(self) -> None:
        ok, detail = verify_cellapp_rehome.cellapp_restore_health_detail(
            cell_snapshot(), cell_snapshot(total=2, payload=1), 2
        )

        self.assertFalse(ok)
        self.assertIn("scheduled:2 total:2 payload:1 ghost_backup:0", detail)

    def test_rejects_cellapp_restore_failure_delta(self) -> None:
        ok, detail = verify_cellapp_rehome.cellapp_restore_health_detail(
            cell_snapshot(), cell_snapshot(total=1, failures=1), 0
        )

        self.assertFalse(ok)
        self.assertIn("failures:1", detail)

    def test_rejects_empty_death_restore_delta(self) -> None:
        ok, detail = verify_cellapp_rehome.cellapp_restore_health_detail(
            cell_snapshot(), cell_snapshot(total=1, empty=1), 0
        )

        self.assertFalse(ok)
        self.assertIn("empty:1", detail)

    def test_rejects_missing_death_restore_request_delta(self) -> None:
        ok, detail = verify_cellapp_rehome.cellapp_restore_health_detail(
            cell_snapshot(), cell_snapshot(total=1, payload=2), 2
        )

        self.assertFalse(ok)
        self.assertIn("scheduled:2 total:1", detail)

    def test_rejects_payload_source_below_baseapp_expectation(self) -> None:
        ok, detail = verify_cellapp_rehome.cellapp_restore_health_detail(
            cell_snapshot(),
            cell_snapshot(total=2, payload=1, ghost_backup=1),
            2,
            2,
            0,
        )

        self.assertFalse(ok)
        self.assertIn("payload_expected:2", detail)

    def test_rejects_ghost_backup_source_below_baseapp_expectation(self) -> None:
        ok, detail = verify_cellapp_rehome.cellapp_restore_health_detail(
            cell_snapshot(),
            cell_snapshot(total=2, payload=2),
            2,
            1,
            1,
        )

        self.assertFalse(ok)
        self.assertIn("ghost_backup_expected:1", detail)


class RestoreVolumeHealthTest(unittest.TestCase):
    def test_accepts_disabled_minimum(self) -> None:
        ok, detail = verify_cellapp_rehome.restore_volume_health_detail(0, 0)

        self.assertTrue(ok, detail)

    def test_accepts_scheduled_delta_at_minimum(self) -> None:
        ok, detail = verify_cellapp_rehome.restore_volume_health_detail(3, 3)

        self.assertTrue(ok, detail)

    def test_rejects_scheduled_delta_below_minimum(self) -> None:
        ok, detail = verify_cellapp_rehome.restore_volume_health_detail(1, 2)

        self.assertFalse(ok)
        self.assertEqual(detail, "scheduled:1 min:2")


class GhostBackupVolumeHealthTest(unittest.TestCase):
    def test_accepts_disabled_minimum(self) -> None:
        ok, detail = verify_cellapp_rehome.ghost_backup_volume_health_detail(0, 0)

        self.assertTrue(ok, detail)

    def test_accepts_ghost_backup_delta_at_minimum(self) -> None:
        ok, detail = verify_cellapp_rehome.ghost_backup_volume_health_detail(2, 2)

        self.assertTrue(ok, detail)

    def test_rejects_ghost_backup_delta_below_minimum(self) -> None:
        ok, detail = verify_cellapp_rehome.ghost_backup_volume_health_detail(1, 2)

        self.assertFalse(ok)
        self.assertEqual(detail, "ghost_backup:1 min:2")


class PayloadVolumeHealthTest(unittest.TestCase):
    def test_accepts_disabled_minimum(self) -> None:
        ok, detail = verify_cellapp_rehome.payload_volume_health_detail(0, 0)

        self.assertTrue(ok, detail)

    def test_accepts_payload_delta_at_minimum(self) -> None:
        ok, detail = verify_cellapp_rehome.payload_volume_health_detail(2, 2)

        self.assertTrue(ok, detail)

    def test_rejects_payload_delta_below_minimum(self) -> None:
        ok, detail = verify_cellapp_rehome.payload_volume_health_detail(1, 2)

        self.assertFalse(ok)
        self.assertEqual(detail, "payload:1 min:2")


class PromotedVolumeHealthTest(unittest.TestCase):
    def test_accepts_disabled_minimum(self) -> None:
        ok, detail = verify_cellapp_rehome.promoted_volume_health_detail(0, 0)

        self.assertTrue(ok, detail)

    def test_accepts_promoted_delta_at_minimum(self) -> None:
        ok, detail = verify_cellapp_rehome.promoted_volume_health_detail(2, 2)

        self.assertTrue(ok, detail)

    def test_rejects_promoted_delta_below_minimum(self) -> None:
        ok, detail = verify_cellapp_rehome.promoted_volume_health_detail(1, 2)

        self.assertFalse(ok)
        self.assertEqual(detail, "promoted:1 min:2")


class TargetSelectionTest(unittest.TestCase):
    def test_parses_cellapp_entities_by_app(self) -> None:
        entities = verify_cellapp_rehome.parse_cellapp_entities_by_app(
            "app=1 addr=127.0.0.1:30001 load=0.1 entities=0 retiring=0 "
            "app=2 addr=127.0.0.1:30002 load=0.2 entities=7 retiring=0"
        )

        self.assertEqual(entities, {1: 0, 2: 7})

    def test_parses_baseapp_routes(self) -> None:
        routes = verify_cellapp_rehome.parse_baseapp_routes(
            "routes=2 addr=127.0.0.1:30001 entities=2 "
            "payload_candidates=1 ghost_backup_candidates=1 "
            "addr=127.0.0.1:30002 entities=7 "
            "payload_candidates=5 ghost_backup_candidates=2"
        )

        self.assertEqual(routes, {"127.0.0.1:30001": 2, "127.0.0.1:30002": 7})

    def test_parses_baseapp_restore_candidate_routes(self) -> None:
        routes = verify_cellapp_rehome.parse_baseapp_route_summary(
            "routes=2 addr=127.0.0.1:30001 entities=2 "
            "payload_candidates=1 ghost_backup_candidates=1 "
            "addr=127.0.0.1:30002 entities=7 "
            "payload_candidates=5 ghost_backup_candidates=2"
        )

        self.assertEqual(
            routes["127.0.0.1:30002"],
            {"entities": 7, "payload_candidates": 5, "ghost_backup_candidates": 2},
        )

    def test_prefers_base_routed_target_when_min_restores_is_enabled(self) -> None:
        args = Namespace(
            target_name="",
            target_app_id=0,
            min_restores=1,
            min_payload_restores=0,
            min_ghost_backup_restores=0,
        )
        apps = [
            {"name": "cellapp_empty", "addr": "127.0.0.1:30001"},
            {"name": "cellapp_busy", "addr": "127.0.0.1:30002"},
        ]

        target = verify_cellapp_rehome.choose_target(
            args,
            apps,
            leaf_counts={1: 5, 2: 2},
            app_ids_by_addr={"127.0.0.1:30001": 1, "127.0.0.1:30002": 2},
            entities_by_app={1: 9, 2: 3},
            base_routes_by_addr={"127.0.0.1:30001": 0, "127.0.0.1:30002": 3},
            base_payload_routes_by_addr={"127.0.0.1:30001": 0, "127.0.0.1:30002": 3},
            base_ghost_routes_by_addr={"127.0.0.1:30001": 0, "127.0.0.1:30002": 0},
        )

        self.assertEqual(target["name"], "cellapp_busy")

    def test_prefers_payload_candidate_target_when_minimum_is_enabled(self) -> None:
        args = Namespace(
            target_name="",
            target_app_id=0,
            min_restores=0,
            min_payload_restores=1,
            min_ghost_backup_restores=0,
        )
        apps = [
            {"name": "cellapp_ghost", "addr": "127.0.0.1:30001"},
            {"name": "cellapp_payload", "addr": "127.0.0.1:30002"},
        ]

        target = verify_cellapp_rehome.choose_target(
            args,
            apps,
            leaf_counts={1: 5, 2: 2},
            app_ids_by_addr={"127.0.0.1:30001": 1, "127.0.0.1:30002": 2},
            entities_by_app={1: 9, 2: 3},
            base_routes_by_addr={"127.0.0.1:30001": 9, "127.0.0.1:30002": 3},
            base_payload_routes_by_addr={"127.0.0.1:30001": 0, "127.0.0.1:30002": 2},
            base_ghost_routes_by_addr={"127.0.0.1:30001": 3, "127.0.0.1:30002": 0},
        )

        self.assertEqual(target["name"], "cellapp_payload")

    def test_prefers_ghost_backup_candidate_target_when_minimum_is_enabled(self) -> None:
        args = Namespace(
            target_name="",
            target_app_id=0,
            min_restores=0,
            min_payload_restores=0,
            min_ghost_backup_restores=1,
        )
        apps = [
            {"name": "cellapp_payload", "addr": "127.0.0.1:30001"},
            {"name": "cellapp_ghost", "addr": "127.0.0.1:30002"},
        ]

        target = verify_cellapp_rehome.choose_target(
            args,
            apps,
            leaf_counts={1: 5, 2: 2},
            app_ids_by_addr={"127.0.0.1:30001": 1, "127.0.0.1:30002": 2},
            entities_by_app={1: 9, 2: 3},
            base_routes_by_addr={"127.0.0.1:30001": 9, "127.0.0.1:30002": 3},
            base_payload_routes_by_addr={"127.0.0.1:30001": 9, "127.0.0.1:30002": 1},
            base_ghost_routes_by_addr={"127.0.0.1:30001": 0, "127.0.0.1:30002": 2},
        )

        self.assertEqual(target["name"], "cellapp_ghost")

    def test_prefers_total_routes_when_ghost_backup_minimum_is_disabled(self) -> None:
        args = Namespace(
            target_name="",
            target_app_id=0,
            min_restores=1,
            min_payload_restores=0,
            min_ghost_backup_restores=0,
        )
        apps = [
            {"name": "cellapp_more_ghosts", "addr": "127.0.0.1:30001"},
            {"name": "cellapp_more_routes", "addr": "127.0.0.1:30002"},
        ]

        target = verify_cellapp_rehome.choose_target(
            args,
            apps,
            leaf_counts={1: 5, 2: 2},
            app_ids_by_addr={"127.0.0.1:30001": 1, "127.0.0.1:30002": 2},
            entities_by_app={1: 3, 2: 9},
            base_routes_by_addr={"127.0.0.1:30001": 3, "127.0.0.1:30002": 9},
            base_payload_routes_by_addr={"127.0.0.1:30001": 0, "127.0.0.1:30002": 9},
            base_ghost_routes_by_addr={"127.0.0.1:30001": 3, "127.0.0.1:30002": 0},
        )

        self.assertEqual(target["name"], "cellapp_more_routes")

    def test_rejects_fixed_target_below_min_restore_volume(self) -> None:
        args = Namespace(
            target_name="cellapp_empty",
            target_app_id=0,
            min_restores=1,
            min_payload_restores=0,
            min_ghost_backup_restores=0,
        )
        apps = [{"name": "cellapp_empty", "addr": "127.0.0.1:30001"}]

        with self.assertRaisesRegex(RuntimeError, "base_routes=0"):
            verify_cellapp_rehome.choose_target(
                args,
                apps,
                leaf_counts={1: 1},
                app_ids_by_addr={"127.0.0.1:30001": 1},
                entities_by_app={1: 5},
                base_routes_by_addr={"127.0.0.1:30001": 0},
                base_payload_routes_by_addr={"127.0.0.1:30001": 0},
                base_ghost_routes_by_addr={"127.0.0.1:30001": 0},
            )

    def test_rejects_fixed_target_below_min_payload_volume(self) -> None:
        args = Namespace(
            target_name="cellapp_ghost",
            target_app_id=0,
            min_restores=0,
            min_payload_restores=1,
            min_ghost_backup_restores=0,
        )
        apps = [{"name": "cellapp_ghost", "addr": "127.0.0.1:30001"}]

        with self.assertRaisesRegex(RuntimeError, "payload_candidates=0"):
            verify_cellapp_rehome.choose_target(
                args,
                apps,
                leaf_counts={1: 1},
                app_ids_by_addr={"127.0.0.1:30001": 1},
                entities_by_app={1: 5},
                base_routes_by_addr={"127.0.0.1:30001": 5},
                base_payload_routes_by_addr={"127.0.0.1:30001": 0},
                base_ghost_routes_by_addr={"127.0.0.1:30001": 5},
            )

    def test_rejects_fixed_target_below_min_ghost_backup_volume(self) -> None:
        args = Namespace(
            target_name="cellapp_payload",
            target_app_id=0,
            min_restores=0,
            min_payload_restores=0,
            min_ghost_backup_restores=1,
        )
        apps = [{"name": "cellapp_payload", "addr": "127.0.0.1:30001"}]

        with self.assertRaisesRegex(RuntimeError, "ghost_backup_candidates=0"):
            verify_cellapp_rehome.choose_target(
                args,
                apps,
                leaf_counts={1: 1},
                app_ids_by_addr={"127.0.0.1:30001": 1},
                entities_by_app={1: 5},
                base_routes_by_addr={"127.0.0.1:30001": 5},
                base_payload_routes_by_addr={"127.0.0.1:30001": 5},
                base_ghost_routes_by_addr={"127.0.0.1:30001": 0},
            )

    def test_rejects_automatic_target_when_no_leaf_owner_has_enough_base_routes(self) -> None:
        args = Namespace(
            target_name="",
            target_app_id=0,
            min_restores=2,
            min_payload_restores=0,
            min_ghost_backup_restores=0,
        )
        apps = [{"name": "cellapp_empty", "addr": "127.0.0.1:30001"}]

        with self.assertRaisesRegex(RuntimeError, "target and restore gates"):
            verify_cellapp_rehome.choose_target(
                args,
                apps,
                leaf_counts={1: 1},
                app_ids_by_addr={"127.0.0.1:30001": 1},
                entities_by_app={1: 1},
                base_routes_by_addr={"127.0.0.1:30001": 1},
                base_payload_routes_by_addr={"127.0.0.1:30001": 1},
                base_ghost_routes_by_addr={"127.0.0.1:30001": 0},
            )

    def test_prefers_target_meeting_entity_and_leaf_minimums(self) -> None:
        args = Namespace(
            target_name="",
            target_app_id=0,
            min_target_entities=5,
            min_target_leaves=2,
            min_restores=0,
            min_payload_restores=0,
            min_ghost_backup_restores=0,
        )
        apps = [
            {"name": "cellapp_small", "addr": "127.0.0.1:30001"},
            {"name": "cellapp_large", "addr": "127.0.0.1:30002"},
        ]

        target = verify_cellapp_rehome.choose_target(
            args,
            apps,
            leaf_counts={1: 4, 2: 2},
            app_ids_by_addr={"127.0.0.1:30001": 1, "127.0.0.1:30002": 2},
            entities_by_app={1: 3, 2: 9},
            base_routes_by_addr={"127.0.0.1:30001": 20, "127.0.0.1:30002": 0},
            base_payload_routes_by_addr={},
            base_ghost_routes_by_addr={},
        )

        self.assertEqual(target["name"], "cellapp_large")

    def test_rejects_fixed_target_below_min_target_entities(self) -> None:
        args = Namespace(
            target_name="cellapp_small",
            target_app_id=0,
            min_target_entities=6,
            min_target_leaves=0,
            min_restores=0,
            min_payload_restores=0,
            min_ghost_backup_restores=0,
        )
        apps = [{"name": "cellapp_small", "addr": "127.0.0.1:30001"}]

        with self.assertRaisesRegex(RuntimeError, "entities=5"):
            verify_cellapp_rehome.choose_target(
                args,
                apps,
                leaf_counts={1: 1},
                app_ids_by_addr={"127.0.0.1:30001": 1},
                entities_by_app={1: 5},
                base_routes_by_addr={},
                base_payload_routes_by_addr={},
                base_ghost_routes_by_addr={},
            )

    def test_rejects_fixed_target_below_min_target_leaves(self) -> None:
        args = Namespace(
            target_name="cellapp_small",
            target_app_id=0,
            min_target_entities=0,
            min_target_leaves=2,
            min_restores=0,
            min_payload_restores=0,
            min_ghost_backup_restores=0,
        )
        apps = [{"name": "cellapp_small", "addr": "127.0.0.1:30001"}]

        with self.assertRaisesRegex(RuntimeError, "leaves=1"):
            verify_cellapp_rehome.choose_target(
                args,
                apps,
                leaf_counts={1: 1},
                app_ids_by_addr={"127.0.0.1:30001": 1},
                entities_by_app={1: 9},
                base_routes_by_addr={},
                base_payload_routes_by_addr={},
                base_ghost_routes_by_addr={},
            )


class ParseArgsTest(unittest.TestCase):
    def test_rejects_fixed_target_with_multiple_cycles(self) -> None:
        with mock.patch(
            "sys.argv",
            [str(SCRIPT), "--cycles", "2", "--target-name", "cellapp_01"],
        ), mock.patch("sys.stderr", new_callable=io.StringIO):
            with self.assertRaises(SystemExit):
                verify_cellapp_rehome.parse_args()

    def test_rejects_negative_min_restores(self) -> None:
        with mock.patch("sys.argv", [str(SCRIPT), "--min-restores", "-1"]), mock.patch(
            "sys.stderr", new_callable=io.StringIO
        ):
            with self.assertRaises(SystemExit):
                verify_cellapp_rehome.parse_args()

    def test_rejects_negative_min_target_entities(self) -> None:
        with mock.patch(
            "sys.argv", [str(SCRIPT), "--min-target-entities", "-1"]
        ), mock.patch("sys.stderr", new_callable=io.StringIO):
            with self.assertRaises(SystemExit):
                verify_cellapp_rehome.parse_args()

    def test_rejects_negative_min_target_leaves(self) -> None:
        with mock.patch(
            "sys.argv", [str(SCRIPT), "--min-target-leaves", "-1"]
        ), mock.patch("sys.stderr", new_callable=io.StringIO):
            with self.assertRaises(SystemExit):
                verify_cellapp_rehome.parse_args()

    def test_rejects_negative_min_ghost_backup_restores(self) -> None:
        with mock.patch(
            "sys.argv", [str(SCRIPT), "--min-ghost-backup-restores", "-1"]
        ), mock.patch("sys.stderr", new_callable=io.StringIO):
            with self.assertRaises(SystemExit):
                verify_cellapp_rehome.parse_args()

    def test_rejects_negative_min_payload_restores(self) -> None:
        with mock.patch(
            "sys.argv", [str(SCRIPT), "--min-payload-restores", "-1"]
        ), mock.patch("sys.stderr", new_callable=io.StringIO):
            with self.assertRaises(SystemExit):
                verify_cellapp_rehome.parse_args()

    def test_rejects_negative_min_promoted_restores(self) -> None:
        with mock.patch(
            "sys.argv", [str(SCRIPT), "--min-promoted-restores", "-1"]
        ), mock.patch("sys.stderr", new_callable=io.StringIO):
            with self.assertRaises(SystemExit):
                verify_cellapp_rehome.parse_args()

    def test_rejects_negative_min_total_scheduled_restores(self) -> None:
        with mock.patch(
            "sys.argv", [str(SCRIPT), "--min-total-scheduled-restores", "-1"]
        ), mock.patch("sys.stderr", new_callable=io.StringIO):
            with self.assertRaises(SystemExit):
                verify_cellapp_rehome.parse_args()

    def test_rejects_negative_min_restore_latency_samples(self) -> None:
        with mock.patch(
            "sys.argv", [str(SCRIPT), "--min-restore-latency-samples", "-1"]
        ), mock.patch("sys.stderr", new_callable=io.StringIO):
            with self.assertRaises(SystemExit):
                verify_cellapp_rehome.parse_args()

    def test_rejects_min_payload_restore_share_above_one(self) -> None:
        with mock.patch(
            "sys.argv", [str(SCRIPT), "--min-payload-restore-share", "1.1"]
        ), mock.patch("sys.stderr", new_callable=io.StringIO):
            with self.assertRaises(SystemExit):
                verify_cellapp_rehome.parse_args()

    def test_rejects_negative_min_ghost_backup_restore_share(self) -> None:
        with mock.patch(
            "sys.argv", [str(SCRIPT), "--min-ghost-backup-restore-share", "-0.1"]
        ), mock.patch("sys.stderr", new_callable=io.StringIO):
            with self.assertRaises(SystemExit):
                verify_cellapp_rehome.parse_args()

    def test_rejects_min_restores_without_baseapp_verification(self) -> None:
        with mock.patch(
            "sys.argv",
            [str(SCRIPT), "--allow-no-baseapp", "--min-restores", "1"],
        ), mock.patch("sys.stderr", new_callable=io.StringIO):
            with self.assertRaises(SystemExit):
                verify_cellapp_rehome.parse_args()

    def test_rejects_min_ghost_backup_restores_without_baseapp_verification(self) -> None:
        with mock.patch(
            "sys.argv",
            [str(SCRIPT), "--allow-no-baseapp", "--min-ghost-backup-restores", "1"],
        ), mock.patch("sys.stderr", new_callable=io.StringIO):
            with self.assertRaises(SystemExit):
                verify_cellapp_rehome.parse_args()

    def test_rejects_min_payload_restores_without_baseapp_verification(self) -> None:
        with mock.patch(
            "sys.argv",
            [str(SCRIPT), "--allow-no-baseapp", "--min-payload-restores", "1"],
        ), mock.patch("sys.stderr", new_callable=io.StringIO):
            with self.assertRaises(SystemExit):
                verify_cellapp_rehome.parse_args()

    def test_rejects_min_promoted_restores_without_baseapp_verification(self) -> None:
        with mock.patch(
            "sys.argv",
            [str(SCRIPT), "--allow-no-baseapp", "--min-promoted-restores", "1"],
        ), mock.patch("sys.stderr", new_callable=io.StringIO):
            with self.assertRaises(SystemExit):
                verify_cellapp_rehome.parse_args()

    def test_rejects_min_total_restores_without_baseapp_verification(self) -> None:
        with mock.patch(
            "sys.argv",
            [str(SCRIPT), "--allow-no-baseapp", "--min-total-payload-restores", "1"],
        ), mock.patch("sys.stderr", new_callable=io.StringIO):
            with self.assertRaises(SystemExit):
                verify_cellapp_rehome.parse_args()

    def test_rejects_min_restore_share_without_baseapp_verification(self) -> None:
        with mock.patch(
            "sys.argv",
            [str(SCRIPT), "--allow-no-baseapp", "--min-promoted-restore-share", "0.1"],
        ), mock.patch("sys.stderr", new_callable=io.StringIO):
            with self.assertRaises(SystemExit):
                verify_cellapp_rehome.parse_args()

    def test_rejects_restore_latency_without_baseapp_verification(self) -> None:
        with mock.patch(
            "sys.argv",
            [str(SCRIPT), "--allow-no-baseapp", "--max-restore-ms", "5000"],
        ), mock.patch("sys.stderr", new_callable=io.StringIO):
            with self.assertRaises(SystemExit):
                verify_cellapp_rehome.parse_args()


class CycleSummaryTest(unittest.TestCase):
    def results(self) -> list[verify_cellapp_rehome.RehomeCycleResult]:
        return [
            verify_cellapp_rehome.RehomeCycleResult(
                detail="first",
                scheduled=3,
                payload_scheduled=2,
                ghost_backup_scheduled=1,
                restored=3,
                lost=0,
                timeouts=0,
                cell_total=3,
                cell_payload=2,
                cell_ghost_backup=1,
                cell_empty=0,
                cell_failures=0,
                cell_promoted=1,
                restore_elapsed_ms=120,
                cycle=1,
                target_name="cellapp_01",
                target_app_id=2,
                target_pid="101",
                target_addr="127.0.0.1:30001",
                target_leaf_count=4,
                target_entities=30,
                target_base_routes=3,
                target_payload_candidates=2,
                target_ghost_backup_candidates=1,
            ),
            verify_cellapp_rehome.RehomeCycleResult(
                detail="second",
                scheduled=2,
                payload_scheduled=1,
                ghost_backup_scheduled=1,
                restored=2,
                lost=0,
                timeouts=0,
                cell_total=2,
                cell_payload=1,
                cell_ghost_backup=1,
                cell_empty=0,
                cell_failures=0,
                cell_promoted=2,
                restore_elapsed_ms=80,
                cycle=2,
                target_name="cellapp_02",
                target_app_id=3,
                target_pid="102",
                target_addr="127.0.0.1:30002",
                target_leaf_count=2,
                target_entities=20,
                target_base_routes=2,
                target_payload_candidates=1,
                target_ghost_backup_candidates=1,
            ),
        ]

    def test_summarizes_rehome_cycle_results(self) -> None:
        summary = verify_cellapp_rehome.summarize_cycles(self.results())

        self.assertIn("cycles=2 target_leaves=6 target_entities=50", summary)
        self.assertIn("target_base_routes=5 target_payload_candidates=3", summary)
        self.assertIn("target_ghost_backup_candidates=2 scheduled=5 restored=5", summary)
        self.assertIn("payload_scheduled=3 ghost_backup_scheduled=2", summary)
        self.assertIn("cell_ghost_backup=2 cell_promoted=3", summary)
        self.assertIn("restore_completion_rate=1.0", summary)
        self.assertIn("restore_source_coverage_rate=1.0", summary)
        self.assertIn("payload_expected_coverage_rate=1.0", summary)
        self.assertIn("ghost_backup_expected_coverage_rate=1.0", summary)
        self.assertIn("payload_restore_share=0.6", summary)
        self.assertIn("ghost_backup_restore_share=0.4", summary)
        self.assertIn("promoted_restore_share=0.6", summary)
        self.assertIn("restore_latency_samples=2", summary)
        self.assertIn("avg_restore_ms=100.0", summary)
        self.assertIn("p50_restore_ms=80", summary)
        self.assertIn("p95_restore_ms=120", summary)
        self.assertIn("max_restore_ms=120", summary)

    def test_summarizes_restore_latency_samples_only_for_scheduled_cycles(self) -> None:
        results = [
            verify_cellapp_rehome.RehomeCycleResult(
                detail="topology-only",
                scheduled=0,
                payload_scheduled=0,
                ghost_backup_scheduled=0,
                restored=0,
                lost=0,
                timeouts=0,
                cell_total=0,
                cell_payload=0,
                cell_ghost_backup=0,
                cell_empty=0,
                cell_failures=0,
                cell_promoted=0,
                restore_elapsed_ms=0,
            ),
            *self.results(),
        ]

        summary = verify_cellapp_rehome.restore_latency_summary(results)

        self.assertEqual(summary["restore_latency_samples"], 2)
        self.assertEqual(summary["min_restore_ms"], 80)
        self.assertEqual(summary["avg_restore_ms"], 100.0)
        self.assertEqual(summary["p50_restore_ms"], 80)
        self.assertEqual(summary["p95_restore_ms"], 120)
        self.assertEqual(summary["max_restore_ms"], 120)

    def test_summarizes_topology_only_coverage_without_zero_division(self) -> None:
        result = verify_cellapp_rehome.RehomeCycleResult(
            detail="topology-only",
            scheduled=0,
            payload_scheduled=0,
            ghost_backup_scheduled=0,
            restored=0,
            lost=0,
            timeouts=0,
            cell_total=0,
            cell_payload=0,
            cell_ghost_backup=0,
            cell_empty=0,
            cell_failures=0,
            cell_promoted=0,
            restore_elapsed_ms=0,
        )

        summary = verify_cellapp_rehome.summarize_cycle_metrics([result])

        self.assertEqual(summary["restore_completion_rate"], 1.0)
        self.assertEqual(summary["restore_source_coverage_rate"], 1.0)
        self.assertEqual(summary["payload_expected_coverage_rate"], 1.0)
        self.assertEqual(summary["ghost_backup_expected_coverage_rate"], 1.0)
        self.assertEqual(summary["payload_restore_share"], 0.0)
        self.assertEqual(summary["ghost_backup_restore_share"], 0.0)
        self.assertEqual(summary["promoted_restore_share"], 0.0)

    def test_accepts_summary_gates_at_minimum(self) -> None:
        args = Namespace(
            min_total_scheduled_restores=5,
            min_total_payload_restores=3,
            min_total_ghost_backup_restores=2,
            min_total_promoted_restores=3,
            min_restore_latency_samples=2,
            min_payload_restore_share=0.6,
            min_ghost_backup_restore_share=0.4,
            min_promoted_restore_share=0.6,
        )

        verify_cellapp_rehome.validate_summary_gates(args, self.results())

    def test_rejects_summary_share_below_minimum(self) -> None:
        args = Namespace(
            min_total_scheduled_restores=0,
            min_total_payload_restores=0,
            min_total_ghost_backup_restores=0,
            min_total_promoted_restores=0,
            min_restore_latency_samples=0,
            min_payload_restore_share=0.0,
            min_ghost_backup_restore_share=0.5,
            min_promoted_restore_share=0.0,
        )

        with self.assertRaisesRegex(RuntimeError, "ghost_backup_restore_share"):
            verify_cellapp_rehome.validate_summary_gates(args, self.results())

    def test_rejects_summary_volume_below_minimum(self) -> None:
        args = Namespace(
            min_total_scheduled_restores=6,
            min_total_payload_restores=0,
            min_total_ghost_backup_restores=0,
            min_total_promoted_restores=0,
            min_restore_latency_samples=0,
            min_payload_restore_share=0.0,
            min_ghost_backup_restore_share=0.0,
            min_promoted_restore_share=0.0,
        )

        with self.assertRaisesRegex(RuntimeError, "scheduled"):
            verify_cellapp_rehome.validate_summary_gates(args, self.results())

    def test_summary_gate_evaluations_report_failed_active_gates(self) -> None:
        summary = verify_cellapp_rehome.summarize_cycle_metrics(self.results())
        gates = verify_cellapp_rehome.summary_gate_evaluations(
            summary,
            {
                "min_total_scheduled_restores": 6,
                "min_ghost_backup_restore_share": 0.5,
                "min_payload_restore_share": 0.0,
            },
        )

        self.assertFalse(gates["min_total_scheduled_restores"]["ok"])
        self.assertEqual(gates["min_total_scheduled_restores"]["metric"], "scheduled")
        self.assertEqual(gates["min_total_scheduled_restores"]["value"], 5)
        self.assertEqual(gates["min_total_scheduled_restores"]["minimum"], 6)
        self.assertFalse(gates["min_ghost_backup_restore_share"]["ok"])
        self.assertEqual(
            gates["min_ghost_backup_restore_share"]["metric"],
            "ghost_backup_restore_share",
        )
        self.assertNotIn("min_payload_restore_share", gates)

    def test_percentile_nearest_rank_uses_upper_tail_for_p95(self) -> None:
        value = verify_cellapp_rehome.percentile_nearest_rank_ms([10, 20, 30], 95)

        self.assertEqual(value, 30)

    def test_builds_machine_readable_summary_payload(self) -> None:
        parameters = verify_cellapp_rehome.summary_parameters(
            Namespace(
                build="debug",
                machined="127.0.0.1:20018",
                atlas_tool=Path("bin/debug/atlas_tool.exe"),
                target_app_id=0,
                target_name="",
                min_cellapps=3,
                min_spaces=2,
                timeout_sec=90.0,
                poll_sec=0.5,
                cycles=2,
                max_restore_ms=5000,
                min_target_entities=10,
                min_target_leaves=2,
                min_restores=2,
                min_ghost_backup_restores=1,
                min_payload_restores=1,
                min_promoted_restores=1,
                min_total_scheduled_restores=5,
                min_total_payload_restores=3,
                min_total_ghost_backup_restores=2,
                min_total_promoted_restores=3,
                min_restore_latency_samples=2,
                min_payload_restore_share=0.6,
                min_ghost_backup_restore_share=0.4,
                min_promoted_restore_share=0.6,
                shutdown_reason=1,
                allow_no_baseapp=False,
            )
        )
        payload = verify_cellapp_rehome.build_summary_payload(self.results(), parameters)

        self.assertEqual(payload["schema_version"], 1)
        self.assertEqual(payload["parameters"]["min_cellapps"], 3)
        self.assertEqual(payload["parameters"]["min_target_entities"], 10)
        self.assertEqual(payload["parameters"]["min_target_leaves"], 2)
        self.assertEqual(payload["parameters"]["min_restores"], 2)
        self.assertEqual(payload["parameters"]["min_total_scheduled_restores"], 5)
        self.assertEqual(payload["parameters"]["min_total_payload_restores"], 3)
        self.assertEqual(payload["parameters"]["min_total_ghost_backup_restores"], 2)
        self.assertEqual(payload["parameters"]["min_total_promoted_restores"], 3)
        self.assertEqual(payload["parameters"]["min_restore_latency_samples"], 2)
        self.assertEqual(payload["parameters"]["min_ghost_backup_restore_share"], 0.4)
        self.assertEqual(payload["parameters"]["max_restore_ms"], 5000)
        self.assertEqual(
            payload["parameters"]["atlas_tool"], str(Path("bin/debug/atlas_tool.exe"))
        )
        self.assertEqual(payload["summary"]["cycles"], 2)
        self.assertEqual(payload["summary"]["success_rate"], 1.0)
        self.assertEqual(payload["summary"]["target_leaves"], 6)
        self.assertEqual(payload["summary"]["target_entities"], 50)
        self.assertEqual(payload["summary"]["target_base_routes"], 5)
        self.assertEqual(payload["summary"]["scheduled"], 5)
        self.assertEqual(payload["summary"]["cell_promoted"], 3)
        self.assertEqual(payload["summary"]["restore_completion_rate"], 1.0)
        self.assertEqual(payload["summary"]["payload_restore_share"], 0.6)
        self.assertEqual(payload["summary"]["ghost_backup_restore_share"], 0.4)
        self.assertEqual(payload["summary"]["promoted_restore_share"], 0.6)
        self.assertEqual(payload["summary"]["restore_latency_samples"], 2)
        self.assertEqual(payload["summary"]["avg_restore_ms"], 100.0)
        self.assertEqual(payload["summary"]["p95_restore_ms"], 120)
        self.assertEqual(payload["summary"]["max_restore_ms"], 120)
        scheduled_gate = payload["gates"]["min_total_scheduled_restores"]
        self.assertEqual(scheduled_gate["metric"], "scheduled")
        self.assertEqual(scheduled_gate["value"], 5)
        self.assertEqual(scheduled_gate["minimum"], 5)
        self.assertTrue(scheduled_gate["ok"])
        ghost_share_gate = payload["gates"]["min_ghost_backup_restore_share"]
        self.assertEqual(ghost_share_gate["metric"], "ghost_backup_restore_share")
        self.assertEqual(ghost_share_gate["value"], 0.4)
        self.assertEqual(ghost_share_gate["minimum"], 0.4)
        self.assertTrue(ghost_share_gate["ok"])
        self.assertEqual(payload["cycles"][0]["detail"], "first")
        self.assertEqual(payload["cycles"][0]["target_name"], "cellapp_01")
        self.assertEqual(payload["cycles"][0]["target_payload_candidates"], 2)

    def test_writes_summary_json(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "nested" / "summary.json"

            verify_cellapp_rehome.write_summary_json(
                path, self.results(), {"min_restores": 2}
            )

            payload = json.loads(path.read_text(encoding="utf-8"))

        self.assertEqual(payload["summary"]["restored"], 5)
        self.assertEqual(payload["parameters"]["min_restores"], 2)
        self.assertEqual(payload["cycles"][1]["cell_payload"], 1)

    def test_writes_summary_json_without_leaving_temp_file(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "nested" / "summary.json"
            path.parent.mkdir()
            path.write_text("stale\n", encoding="utf-8")

            verify_cellapp_rehome.write_summary_json(path, self.results())

            payload = json.loads(path.read_text(encoding="utf-8"))
            temp_files = list(path.parent.glob(f".{path.name}.*.tmp"))

        self.assertEqual(payload["summary"]["scheduled"], 5)
        self.assertEqual(temp_files, [])

    def test_main_writes_summary_json_before_summary_gate_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "summary.json"
            args = Namespace(
                build="debug",
                machined="127.0.0.1:20018",
                atlas_tool=SCRIPT,
                target_app_id=0,
                target_name="",
                min_cellapps=2,
                min_spaces=1,
                timeout_sec=60.0,
                poll_sec=1.0,
                cycles=1,
                max_restore_ms=0,
                min_target_entities=0,
                min_target_leaves=0,
                min_restores=0,
                min_ghost_backup_restores=0,
                min_payload_restores=0,
                min_promoted_restores=0,
                min_total_scheduled_restores=4,
                min_total_payload_restores=0,
                min_total_ghost_backup_restores=0,
                min_total_promoted_restores=0,
                min_restore_latency_samples=0,
                min_payload_restore_share=0.0,
                min_ghost_backup_restore_share=0.0,
                min_promoted_restore_share=0.0,
                shutdown_reason=1,
                summary_json=path,
                allow_no_baseapp=False,
            )

            with mock.patch.object(
                verify_cellapp_rehome, "parse_args", return_value=args
            ), mock.patch.object(
                verify_cellapp_rehome,
                "run_rehome_cycle",
                return_value=self.results()[0],
            ), mock.patch("sys.stderr", new_callable=io.StringIO):
                exit_code = verify_cellapp_rehome.main()

            payload = json.loads(path.read_text(encoding="utf-8"))

        scheduled_gate = payload["gates"]["min_total_scheduled_restores"]
        self.assertEqual(exit_code, 1)
        self.assertEqual(payload["summary"]["scheduled"], 3)
        self.assertEqual(scheduled_gate["minimum"], 4)
        self.assertFalse(scheduled_gate["ok"])


if __name__ == "__main__":
    unittest.main()
