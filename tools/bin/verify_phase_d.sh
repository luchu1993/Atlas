#!/usr/bin/env bash
set -euo pipefail
exec python3 "$(dirname "$0")/../cluster_control/verify_phase_d.py" "$@"
