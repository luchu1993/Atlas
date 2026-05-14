#!/usr/bin/env bash
# Cluster preset for samples/mvp; wraps run_world_stress.py with --clients 0
# --keep-cluster and Mvp BaseApp / CellApp script assemblies.
set -euo pipefail

usage() {
    cat <<USAGE >&2
Usage: $0 [--build DIR] [--config CFG] [--login-port N] [extra args...]

  --build       build directory (default: build/debug)
  --config      CMake configuration (default: Debug)
  --login-port  external LoginApp port (default: 20018, matches UnityClient default)

Stop with Ctrl+C; orphaned server processes need pkill -f atlas_.
USAGE
}

BUILD="build/debug"
CONFIG="Debug"
LOGIN_PORT=20018
EXTRA=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build)         BUILD="$2";          shift 2 ;;
        --config)        CONFIG="$2";         shift 2 ;;
        --login-port)    LOGIN_PORT="$2";     shift 2 ;;
        -h|--help)       usage; exit 0 ;;
        *)               EXTRA+=("$1");       shift ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
PYTHON="${PYTHON:-python3}"

BIN_NAME="$(basename "${BUILD}")"
BASE_DLL="${REPO_ROOT}/bin/${BIN_NAME}/Atlas.Mvp.Base.dll"
CELL_DLL="${REPO_ROOT}/bin/${BIN_NAME}/Atlas.Mvp.Cell.dll"

exec "${PYTHON}" "${SCRIPT_DIR}/../cluster_control/run_world_stress.py" \
    --build-dir       "${BUILD}" \
    --config          "${CONFIG}" \
    --baseapp-count   1 \
    --cellapp-count   1 \
    --login-port      "${LOGIN_PORT}" \
    --base-assembly   "${BASE_DLL}" \
    --cell-assembly   "${CELL_DLL}" \
    --cellapp-update-hertz 20 \
    --baseapp-update-hertz 20 \
    --clients         0 \
    --keep-cluster \
    --load-refresh-sec 0 \
    "${EXTRA[@]}"
