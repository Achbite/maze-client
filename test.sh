#!/usr/bin/env bash

set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/maze-client-test.XXXXXX")"
trap 'rm -rf "${build_dir}"' EXIT

if [ "$#" -ne 0 ]; then
    echo "usage: bash ./test.sh" >&2
    exit 2
fi

PYTHONDONTWRITEBYTECODE=1 \
PYTHONPATH="${repo_dir}${PYTHONPATH:+:${PYTHONPATH}}" \
python3 -m unittest -v \
    tests.test_replay_service.ReplayServiceLifecycleTest.test_start_receipt_background_process_and_stop

contract_cpp_dir=""
if [ -n "${RL_CONTRACT_DEV_ARTIFACT_DIR:-}" ]; then
    contract_cpp_dir="${RL_CONTRACT_DEV_ARTIFACT_DIR}/cpp"
fi

cmake_args=(
    -S "${repo_dir}"
    -B "${build_dir}"
    -G Ninja
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_TESTING=ON
)
if [ -n "${contract_cpp_dir}" ]; then
    cmake_args+=("-DCONTRACT_CPP_DIR=${contract_cpp_dir}")
fi
if command -v ccache >/dev/null 2>&1; then
    cmake_args+=("-DCMAKE_CXX_COMPILER_LAUNCHER=$(command -v ccache)")
fi

cmake "${cmake_args[@]}"
cmake --build "${build_dir}" --parallel \
    --target client_command_exchange_development_test
ctest --test-dir "${build_dir}" \
    --output-on-failure \
    -R '^client_command_exchange_development_contract$'
