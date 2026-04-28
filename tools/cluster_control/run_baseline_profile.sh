#!/usr/bin/env bash
# Run a world_stress baseline against the profile build.
#
# Prerequisites:
#   cmake --preset profile && cmake --build build/profile --config RelWithDebInfo
#
# Usage:
#   ./tools/cluster_control/run_baseline_profile.sh [--clients N] [--duration-sec N] [extra args...]
#
# Defaults: 100 clients, 120 s, 1 baseapp, 1 cellapp, 1 space.
# Pass --keep-cluster to leave the cluster running after world_stress exits.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

PYTHON="${PYTHON:-python}"
exec "${PYTHON}" "${SCRIPT_DIR}/run_world_stress.py" \
    --build-dir   build/profile \
    --config      RelWithDebInfo \
    --clients     200 \
    --account-pool 200 \
    --duration-sec 120 \
    --ramp-per-sec 20 \
    --rpc-rate-hz 2 \
    --move-rate-hz 10 \
    --spread-radius 500 \
    --walk-step-meters 5 \
    --walk-range-meters 200 \
    --teleport-pct 5 \
    --space-count 1 \
    --shortline-pct 0 \
    --hold-min-ms 5000 \
    --hold-max-ms 30000 \
    --login-rate-limit-per-ip 0 \
    --login-rate-limit-global 10000 \
    --capture-dir "${REPO_ROOT}/.tmp/prof/baseline" \
    --capture-procs "loginapp,dbapp,baseappmgr,baseapp,cellappmgr,cellapp" \
    "$@"
