#!/usr/bin/env bash
set -euo pipefail
exec python3 "$(dirname "$0")/../run_mvp_unity_bots.py" "$@"
