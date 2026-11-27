#!/usr/bin/env bash
# Runs cppcheck over the firmware C sources and the host-side test C code.
#
# Usage: tools/lint/run_cppcheck.sh [--configure-only | --skip-configure]
#
# The firmware is analysed with the real compile flags from a CMake configure
# (no build) into build_lint/, which needs the XTC tools and xmos-ai-tools:
# XMOS_XTC_ROOT (default /Applications/XMOS_XTC_15.3.1) and the .venv from
# requirements.txt, the same as tools/build_local.sh.
#
#   --configure-only  only (re)generate build_lint/compile_commands.json
#   --skip-configure  reuse an existing build_lint/compile_commands.json
#
# CI configures inside the XMOS builder image and runs cppcheck on the host
# (.github/workflows/lint.yml). Only sources that some firmware target builds
# are in the compile database; submodule code under modules/ is not reported.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${REPO_ROOT}"
BUILD_DIR="${REPO_ROOT}/build_lint"
DB="${BUILD_DIR}/compile_commands.json"

CONFIGURE=1
ANALYSE=1
case "${1:-}" in
    --configure-only) ANALYSE=0;;
    --skip-configure) CONFIGURE=0;;
    "") ;;
    *) sed -n '2,16p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 2;;
esac

if [[ ${CONFIGURE} -eq 1 ]]; then
    if ! command -v xcc >/dev/null; then
        export XMOS_XTC_ROOT="${XMOS_XTC_ROOT:-/Applications/XMOS_XTC_15.3.1}"
        # shellcheck disable=SC1091
        source "${REPO_ROOT}/tools/env/xmos_env.sh"
        set -euo pipefail
    fi
    if [[ -z "${VIRTUAL_ENV:-}" && -x "${REPO_ROOT}/.venv/bin/python" ]]; then
        # shellcheck disable=SC1091
        source "${REPO_ROOT}/.venv/bin/activate"
    fi
    echo "Configuring ${BUILD_DIR} for compile_commands.json..."
    cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -G "Unix Makefiles" \
        -DCMAKE_TOOLCHAIN_FILE="${REPO_ROOT}/xmos_cmake_toolchain/xs3a.cmake" \
        -DPython3_EXECUTABLE="$(command -v python3)" \
        -DALLOW_DIRTY_VERSIONING=ON \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
fi

[[ ${ANALYSE} -eq 1 ]] || exit 0

if [[ ! -f "${DB}" ]]; then
    echo "ERROR: ${DB} not found; run without --skip-configure first" >&2
    exit 1
fi

COMMON_ARGS=(
    --enable=warning,performance,portability
    --inline-suppr
    --quiet
    --error-exitcode=1
    "--suppress=*:*/modules/*"
    "--template={file}:{line}: {severity}/{id}: {message}"
    -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
)

status=0

echo "cppcheck: firmware (compile_commands.json)"
cppcheck "${COMMON_ARGS[@]}" \
    --project="${DB}" \
    "--file-filter=*/satellite-xmos-firmware/*" || status=1

# Host-built unit test, compiled by tests/test_audio_frontend/test_audio_frontend.py
echo "cppcheck: tests/test_audio_frontend"
cppcheck "${COMMON_ARGS[@]}" \
    --std=c99 \
    -I satellite-xmos-firmware/src \
    -I modules/core/modules/xcore_math/lib_xcore_math/lib_xcore_math/api \
    tests/test_audio_frontend/audio_frontend_test.c || status=1

exit ${status}
