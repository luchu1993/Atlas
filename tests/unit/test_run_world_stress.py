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
            reviver_priority=255,
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

    def test_reviver_args_carry_identity_output_and_priority(self) -> None:
        args = self.args()
        specs = run_world_stress.build_reviver_specs(args)

        reviver_args = run_world_stress.build_reviver_args(
            args,
            specs[1],
            "127.0.0.1:20018",
            Path("atlas_cellappmgr.exe"),
            Path("logs/cellappmgr_revived.log"),
        )

        self.assertIn("reviver_01", reviver_args)
        self.assertIn("27011", reviver_args)
        self.assertIn(str(Path("logs/cellappmgr_revived.log")), reviver_args)
        # ReviverPriority drives the subject's arbitration (no leader lock).
        self.assertIn("--revive-cellappmgr-priority", reviver_args)
        self.assertIn("255", reviver_args)
        # The removed snapshot / lease flags must not resurface.
        self.assertNotIn("--revive-cellappmgr-snapshot-path", reviver_args)
        self.assertNotIn("--revive-leader-lock-mode", reviver_args)

    def test_custom_priority_flows_through(self) -> None:
        args = self.args()
        args.reviver_priority = 200
        specs = run_world_stress.build_reviver_specs(args)

        reviver_args = run_world_stress.build_reviver_args(
            args, specs[0], "127.0.0.1:20018",
            Path("atlas_cellappmgr.exe"), Path("logs/cellappmgr_revived.log"),
        )

        self.assertIn("--revive-cellappmgr-priority", reviver_args)
        self.assertIn("200", reviver_args)


if __name__ == "__main__":
    unittest.main()
