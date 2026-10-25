#!/usr/bin/env bash
# Builds the Satellite1 XMOS firmware on a local machine (macOS or Linux).
#
# Usage: tools/build_local.sh [options] [variant ...]
#
# Variants: fixed_delay (default), bypass, all
#
# Options:
#   -b <dir>   CMake build directory (default: build_local)
#   -c         Clean: delete the build directory before configuring
#   -x         Build the .xe only (skip factory and upgrade images)
#   -d <dir>   Copy the outputs into <dir> (default: dist_local)
#   -j <n>     Parallel jobs (default: number of CPUs)
#   -h         Show this help
#
# The XTC tools are taken from $XMOS_XTC_ROOT (also read from .env in the repo
# root), falling back to /Applications/XMOS_XTC_15.3.1. xcore-opt is taken
# from the Python venv in .venv, which is created from requirements.txt if it
# does not exist.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

BUILD_DIR="build_local"
DIST_DIR="dist_local"
CLEAN=0
XE_ONLY=0
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"

help()
{
    sed -n '2,19p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while getopts "b:cxd:j:h" option; do
    case "${option}" in
        b) BUILD_DIR="${OPTARG}";;
        c) CLEAN=1;;
        x) XE_ONLY=1;;
        d) DIST_DIR="${OPTARG}";;
        j) JOBS="${OPTARG}";;
        h) help; exit 0;;
        *) help; exit 1;;
    esac
done
shift $((OPTIND - 1))

VARIANTS=("$@")
if [[ ${#VARIANTS[@]} -eq 0 ]]; then
    VARIANTS=(fixed_delay)
elif [[ "${VARIANTS[0]}" == "all" ]]; then
    VARIANTS=(fixed_delay bypass)
fi
for variant in "${VARIANTS[@]}"; do
    case "${variant}" in
        fixed_delay|bypass) ;;
        *) echo "ERROR: unknown variant '${variant}' (expected fixed_delay, bypass or all)" >&2; exit 1;;
    esac
done

[[ "${BUILD_DIR}" = /* ]] || BUILD_DIR="${REPO_ROOT}/${BUILD_DIR}"
[[ "${DIST_DIR}" = /* ]] || DIST_DIR="${REPO_ROOT}/${DIST_DIR}"

#**********************
# XTC tools
#**********************
if [[ -z "${XMOS_XTC_ROOT:-}" ]] && ! grep -qs "^XMOS_XTC_ROOT=" "${REPO_ROOT}/.env"; then
    export XMOS_XTC_ROOT="/Applications/XMOS_XTC_15.3.1"
fi
# shellcheck disable=SC1091
source "${REPO_ROOT}/tools/env/xmos_env.sh"
set -euo pipefail

if ! command -v xcc >/dev/null; then
    echo "ERROR: xcc not found in ${XMOS_XTC_ROOT}/bin" >&2
    exit 1
fi

#**********************
# Python venv
#**********************
VENV_DIR="${REPO_ROOT}/.venv"
if [[ ! -x "${VENV_DIR}/bin/python" ]]; then
    echo "Creating Python virtual environment in ${VENV_DIR}..."
    python3 -m venv "${VENV_DIR}"
    "${VENV_DIR}/bin/python" -m pip install --upgrade pip
    "${VENV_DIR}/bin/python" -m pip install -r "${REPO_ROOT}/requirements.txt"
fi
# shellcheck disable=SC1091
source "${VENV_DIR}/bin/activate"

if ! command -v xcore-opt >/dev/null; then
    echo "ERROR: xcore-opt not found; run: ${VENV_DIR}/bin/pip install -r requirements.txt" >&2
    exit 1
fi

#**********************
# Sources
#**********************
if git -C "${REPO_ROOT}" submodule status --recursive | grep -q "^-"; then
    echo "Initialising submodules..."
    git -C "${REPO_ROOT}" submodule update --init --recursive
fi

#**********************
# Configure
#**********************
CACHE_SOURCE_DIR="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "${BUILD_DIR}/CMakeCache.txt" 2>/dev/null || true)"
if [[ -n "${CACHE_SOURCE_DIR}" && "${CACHE_SOURCE_DIR}" != "${REPO_ROOT}" ]]; then
    # e.g. a build directory configured inside the Docker builder (/ws)
    echo "Build dir was configured for ${CACHE_SOURCE_DIR}; starting it from scratch"
    CLEAN=1
fi
if [[ ${CLEAN} -eq 1 ]]; then
    rm -rf "${BUILD_DIR}"
fi

if command -v ninja >/dev/null; then
    GENERATOR="Ninja"
else
    GENERATOR="Unix Makefiles"
fi
if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    # Reuse the generator of an existing build directory
    GENERATOR="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "${BUILD_DIR}/CMakeCache.txt")"
fi

echo "XTC tools:    ${XMOS_XTC_ROOT}"
echo "Build dir:    ${BUILD_DIR}"
echo "Firmware ver: $(head -n 1 "${REPO_ROOT}/firmware_version.txt")"
echo "Variants:     ${VARIANTS[*]}"

cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -G "${GENERATOR}" \
    -DCMAKE_TOOLCHAIN_FILE="${REPO_ROOT}/xmos_cmake_toolchain/xs3a.cmake" \
    -DPython3_EXECUTABLE="${VENV_DIR}/bin/python" \
    -DALLOW_DIRTY_VERSIONING=ON

#**********************
# Build
#**********************
mkdir -p "${DIST_DIR}"
for variant in "${VARIANTS[@]}"; do
    app="satellite1_firmware_${variant}"
    targets=("${app}")
    outputs=("${app}.xe")
    if [[ ${XE_ONLY} -eq 0 ]]; then
        targets+=("create_flash_img_${app}" "create_upgrade_img_${app}")
        outputs+=("${app}.factory.bin" "${app}.factory.md5" "${app}.upgrade.bin" "${app}.upgrade.md5")
    fi

    echo '******************************************************'
    echo "* Building ${app}"
    echo '******************************************************'
    for target in "${targets[@]}"; do
        cmake --build "${BUILD_DIR}" --target "${target}" -j "${JOBS}"
    done
    for output in "${outputs[@]}"; do
        cp "${BUILD_DIR}/${output}" "${DIST_DIR}/"
    done
done

echo
echo "Outputs in ${DIST_DIR}:"
ls -l "${DIST_DIR}"
