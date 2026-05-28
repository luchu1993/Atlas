#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "cluster_control" / "run_world_stress.py"
SPEC = importlib.util.spec_from_file_location("run_world_stress", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
run_world_stress = importlib.util.module_from_spec(SPEC)
sys.modules["run_world_stress"] = run_world_stress
SPEC.loader.exec_module(run_world_stress)


class ReviverSpecTest(unittest.TestCase):
    def args(self) -> SimpleNamespace:
        return SimpleNamespace(
            cellappmgr_reviver_count=3,
            reviver_port=27001,
            reviver_port_stride=10,
            reviver_update_hertz=10,
            cellappmgr_port=25001,
            cellappmgr_update_hertz=10,
            reviver_heartbeat_timeout_ms=4000,
            reviver_restart_delay_ms=1000,
            reviver_max_restarts=3,
            reviver_leader_lock_mode="local",
            reviver_leader_lock_ttl_ms=8000,
            reviver_leader_lock_renew_ms=3000,
            cellappmgr_snapshot_interval_ms=250,
        )

    def test_builds_unique_reviver_specs(self) -> None:
        specs = run_world_stress.build_reviver_specs(self.args())

        self.assertEqual(
            specs,
            [
                {"index": 0, "name": "reviver", "log_name": "reviver", "internal_port": 27001},
                {
                    "index": 1,
                    "name": "reviver_01",
                    "log_name": "reviver_01",
                    "internal_port": 27011,
                },
                {
                    "index": 2,
                    "name": "reviver_02",
                    "log_name": "reviver_02",
                    "internal_port": 27021,
                },
            ],
        )

    def test_reviver_args_share_snapshot_and_leader_lock(self) -> None:
        args = self.args()
        specs = run_world_stress.build_reviver_specs(args)

        reviver_args = run_world_stress.build_reviver_args(
            args,
            specs[1],
            "127.0.0.1:20018",
            Path("atlas_cellappmgr.exe"),
            Path("ha/cellappmgr.bin"),
            Path("ha/reviver.lock"),
            Path("logs/cellappmgr_revived.log"),
        )

        self.assertIn("reviver_01", reviver_args)
        self.assertIn("27011", reviver_args)
        self.assertEqual(
            reviver_args.count("--revive-cellappmgr-snapshot-path"),
            1,
        )
        self.assertIn(str(Path("ha/cellappmgr.bin")), reviver_args)
        self.assertIn(str(Path("ha/reviver.lock")), reviver_args)
        self.assertIn(str(Path("logs/cellappmgr_revived.log")), reviver_args)
        # leader lock mode defaults preserved
        self.assertIn("--revive-leader-lock-mode", reviver_args)
        self.assertIn("local", reviver_args)

    def test_machined_lease_mode_passes_through(self) -> None:
        args = self.args()
        args.reviver_leader_lock_mode = "machined"
        args.reviver_leader_lock_ttl_ms = 9500
        args.reviver_leader_lock_renew_ms = 3200
        specs = run_world_stress.build_reviver_specs(args)

        reviver_args = run_world_stress.build_reviver_args(
            args, specs[0], "127.0.0.1:20018",
            Path("atlas_cellappmgr.exe"), Path("ha/cellappmgr.bin"),
            Path("ha/reviver.lock"), Path("logs/cellappmgr_revived.log"),
        )

        # mode + ttl + renew all flow through to the spawned reviver CLI.
        self.assertIn("--revive-leader-lock-mode", reviver_args)
        self.assertIn("machined", reviver_args)
        self.assertIn("--revive-leader-lock-ttl-ms", reviver_args)
        self.assertIn("9500", reviver_args)
        self.assertIn("--revive-leader-lock-renew-ms", reviver_args)
        self.assertIn("3200", reviver_args)


if __name__ == "__main__":
    unittest.main()
