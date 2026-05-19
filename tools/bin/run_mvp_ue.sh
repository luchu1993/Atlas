#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON="${PYTHON:-python3}"
exec "${PYTHON}" "${SCRIPT_DIR}/../run_mvp_ue.py" "$@"
