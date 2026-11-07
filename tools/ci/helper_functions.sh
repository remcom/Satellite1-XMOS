#!/usr/bin/env bash

# Collection of helper functions that can be used in the different continuous
# integration scripts.

# A small utility to run a command and only print logs if the command fails.
# On success, all logs are hidden.
function log_errors {
    if log=$("$@" 2>&1); then
        echo "'$@' completed successfully!"
    else
        echo "'$@' failed!"
        echo "$log"
        return 1
    fi
}

# Get the system timeout command
function get_timeout {
    uname=$(uname)
    if [[ "$uname" == "Linux" ]] || [[ -n "$MSYSTEM" ]]; then
        echo "timeout"
    elif [[ "$uname" == 'Darwin' ]]; then
        echo "gtimeout"
    fi
}

function export_tools_version {
    xcc_version_output_string=`cat "$XMOS_TOOL_PATH"/doc/version.txt`
    # Find the semantic version substring
    prefix=${xcc_version_output_string%%" "*}
    space_position=${#prefix}
    xcc_semver_substring=`echo $xcc_version_output_string | cut -c1-$space_position`
    # Split semver substring into fields
    IFS='.' read -ra FIELDS <<< "$xcc_semver_substring"
    export XTC_VERSION_MAJOR=${FIELDS[0]}
    export XTC_VERSION_MINOR=${FIELDS[1]}
    export XTC_VERSION_PATCH=${FIELDS[2]}
}

# If the environment has set the $CI_PREFERRED_CMAKE_GENERATOR variable, then
# use this for the build configuration; otherwise default to one of the
# configuration states below (based on other environment factors).
# NOTE: $MSYSTEM is set by MSYS-based environments.
function export_ci_build_vars {
    if [ -n "$CI_PREFERRED_CMAKE_GENERATOR" ]; then
        export CI_CMAKE_GENERATOR=$CI_PREFERRED_CMAKE_GENERATOR
    elif [ -z "$MSYSTEM" ]; then
        export CI_CMAKE_GENERATOR="Unix Makefiles"
    elif [ -x "$(command -v ninja)" ]; then
        export CI_CMAKE_GENERATOR="Ninja"
    else
        export CI_CMAKE_GENERATOR="MinGW Makefiles"
    fi

    if [ "$CI_CMAKE_GENERATOR" = "Ninja" ]; then
        export CI_BUILD_TOOL="ninja"
        export CI_BUILD_TOOL_ARGS=""
    else
        export CI_BUILD_TOOL="make"
        export CI_BUILD_TOOL_ARGS="-j"
    fi
}

# Python version for the build venv. xmos-ai-tools needs numpy<2.0, and numpy
# 1.26 has wheels up to Python 3.12 only, so 3.12 is the newest usable version.
CI_PYTHON_VERSION="${CI_PYTHON_VERSION:-3.12}"

# Print the major.minor version of a python interpreter, or nothing on failure.
function python_minor_version() {
    "$1" -c 'import sys; print("%d.%d" % sys.version_info[:2])' 2>/dev/null
}

# Create a venv on CI_PYTHON_VERSION at $1. Uses python<version> when the system
# has it; otherwise uv (bootstrapped with the system python3) downloads a
# standalone build into build_python/, which works on old glibc such as the
# Debian 11 based xcore_builder image.
function create_python_venv() {
    local venv_dir=$1
    local root=$2
    local system_python="python${CI_PYTHON_VERSION}"

    if command -v "${system_python}" >/dev/null 2>&1; then
        echo "Creating Python ${CI_PYTHON_VERSION} virtual environment in ${venv_dir}..."
        "${system_python}" -m venv "${venv_dir}"
        return
    fi

    local tools_dir="${root}/build_python"
    export UV_PYTHON_INSTALL_DIR="${tools_dir}/pythons"
    export UV_CACHE_DIR="${tools_dir}/cache"
    if [ ! -x "${tools_dir}/uv/bin/uv" ]; then
        echo "Python ${CI_PYTHON_VERSION} not installed; bootstrapping uv in ${tools_dir}..."
        python3 -m venv "${tools_dir}/uv"
        "${tools_dir}/uv/bin/pip" install --quiet --no-cache-dir uv
    fi
    echo "Creating Python ${CI_PYTHON_VERSION} virtual environment in ${venv_dir} (via uv)..."
    "${tools_dir}/uv/bin/uv" venv --seed --python "${CI_PYTHON_VERSION}" "${venv_dir}"
}

# Function to set up Python environment and install dependencies.
# The venv is recreated when it is on another Python version, and the
# requirements are reinstalled whenever requirements.txt changes (tracked by
# its hash in the venv), so an existing build_venv picks up version bumps.
function setup_python_env() {
    local root=$1
    local venv_dir="${root}/build_venv"
    local req_file="${root}/requirements.txt"
    local stamp_file="${venv_dir}/.requirements.sha256"

    if [ -d "${venv_dir}" ] && [ "$(python_minor_version "${venv_dir}/bin/python")" != "${CI_PYTHON_VERSION}" ]; then
        echo "Existing ${venv_dir} is not on Python ${CI_PYTHON_VERSION}; recreating it..."
        rm -rf "${venv_dir}"
    fi
    if [ ! -d "${venv_dir}" ]; then
        create_python_venv "${venv_dir}" "${root}"
    fi

    echo "Activating virtual environment in ${venv_dir}..."
    source "${venv_dir}/bin/activate"
    echo "Using $(python --version)"

    local req_hash
    req_hash=$(python -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "${req_file}")
    if [ ! -f "${stamp_file}" ] || [ "$(cat "${stamp_file}")" != "${req_hash}" ]; then
        echo "Installing ${req_file}..."
        python -m pip install --upgrade pip
        pip install --no-cache-dir -r "${req_file}"
        echo "${req_hash}" > "${stamp_file}"
        echo "Python environment setup complete."
    else
        echo "Python requirements unchanged; skipping install."
    fi
}