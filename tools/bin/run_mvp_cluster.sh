#!/usr/bin/env bash
# Cluster preset for samples/mvp; wraps run_world_stress.py with --clients 0
# --keep-cluster and Mvp BaseApp / CellApp script assemblies.
set -euo pipefail

usage() {
    cat <<USAGE >&2
Usage: $0 [--build DIR] [--config CFG] [--login-port N] [--cellapp-count N] [extra args...]

  --build           build directory (default: build/debug)
  --config          CMake configuration (default: Debug)
  --login-port      external LoginApp port (default: 20018, matches UnityClient default)
  --cellapp-count   number of cellapps to launch (default: 4; lower to test single-cell mode)

Stop with Ctrl+C; orphaned server processes need pkill -f atlas_.
USAGE
}

BUILD="build/debug"
CONFIG="Debug"
LOGIN_PORT=20018
CELLAPP_COUNT=4
EXTRA=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build)          BUILD="$2";          shift 2 ;;
        --config)         CONFIG="$2";         shift 2 ;;
        --login-port)     LOGIN_PORT="$2";     shift 2 ;;
        --cellapp-count)  CELLAPP_COUNT="$2";  shift 2 ;;
        -h|--help)        usage; exit 0 ;;
        *)                EXTRA+=("$1");       shift ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
PYTHON="${PYTHON:-python3}"

BIN_NAME="$(basename "${BUILD}")"
BASE_DLL="${REPO_ROOT}/bin/${BIN_NAME}/Atlas.Mvp.Base.dll"
CELL_DLL="${REPO_ROOT}/bin/${BIN_NAME}/Atlas.Mvp.Cell.dll"

# Cook the collision map (best-effort); MvpSpace falls back to flat ground if absent.
ATLAS_TOOL="${REPO_ROOT}/bin/${BIN_NAME}/atlas_tool"
MAP_SRC="${REPO_ROOT}/samples/mvp/maps/main.collision.json"
MAP_CACHE="${REPO_ROOT}/samples/mvp/maps/main.collisioncache"
if [[ -x "${ATLAS_TOOL}" && -f "${MAP_SRC}" ]]; then
    "${ATLAS_TOOL}" cook_collision "${MAP_SRC}" -o "${MAP_CACHE}" || true
fi

exec "${PYTHON}" "${SCRIPT_DIR}/../cluster_control/run_world_stress.py" \
    --build-dir       "${BUILD}" \
    --config          "${CONFIG}" \
    --baseapp-count   1 \
    --cellapp-count   "${CELLAPP_COUNT}" \
    --login-port      "${LOGIN_PORT}" \
    --base-assembly   "${BASE_DLL}" \
    --cell-assembly   "${CELL_DLL}" \
    --cellapp-update-hertz 20 \
    --baseapp-update-hertz 20 \
    --clients         0 \
    --keep-cluster \
    --load-refresh-sec 0 \
    "${EXTRA[@]}"
