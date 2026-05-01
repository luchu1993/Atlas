#!/usr/bin/env bash
# Cluster preset: run_world_stress.py with --clients 0 --keep-cluster.
set -euo pipefail

usage() {
    cat <<USAGE >&2
Usage: $0 [--build DIR] [--config CFG] [--baseapp-count N] [--cellapp-count N] [--login-port N] [extra args...]

  --build           build directory (default: build/debug)
  --config          CMake configuration (default: Debug)
  --baseapp-count   number of BaseApp instances (default: 1)
  --cellapp-count   number of CellApp instances (default: 1)
  --login-port      external LoginApp port (default: 20013)

Stop with Ctrl+C; orphaned server processes need pkill -f atlas_.
USAGE
}

BUILD="build/debug"
CONFIG="Debug"
BASEAPP_COUNT=1
CELLAPP_COUNT=1
LOGIN_PORT=20013
EXTRA=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build)         BUILD="$2";          shift 2 ;;
        --config)        CONFIG="$2";         shift 2 ;;
        --baseapp-count) BASEAPP_COUNT="$2";  shift 2 ;;
        --cellapp-count) CELLAPP_COUNT="$2";  shift 2 ;;
        --login-port)    LOGIN_PORT="$2";     shift 2 ;;
        -h|--help)       usage; exit 0 ;;
        *)               EXTRA+=("$1");       shift ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON="${PYTHON:-python3}"

exec "${PYTHON}" "${SCRIPT_DIR}/../cluster_control/run_world_stress.py" \
    --build-dir       "${BUILD}" \
    --config          "${CONFIG}" \
    --baseapp-count   "${BASEAPP_COUNT}" \
    --cellapp-count   "${CELLAPP_COUNT}" \
    --login-port      "${LOGIN_PORT}" \
    --clients         0 \
    --keep-cluster \
    "${EXTRA[@]}"
