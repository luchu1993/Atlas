#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON="${PYTHON:-python3}"

exec "${PYTHON}" "${SCRIPT_DIR}/../cluster_control/verify_cellapp_rehome.py" "$@"
