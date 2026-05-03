#!/usr/bin/env bash
set -euo pipefail
exec python3 "$(dirname "$0")/../cluster_control/test_cluster.py" "$@"
