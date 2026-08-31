#!/usr/bin/env bash

set -u

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
default_client_bin="${repo_dir}/build/maze_client"
if [ -x "${repo_dir}/bin/maze_client" ]; then
    default_client_bin="${repo_dir}/bin/maze_client"
fi
client_bin="${RL_CLIENT_BIN:-${default_client_bin}}"
training_admission_path="/run/rl/training-admission.v1.json"
execution_identity_path="/run/rl/execution-identity.v1.json"
training_admitted_marker="/run/rl/client-training-admitted"
managed=0
if [ -n "${RL_CONFIG_PATH:-}" ]; then
    managed=1
    if [[ "${RL_CONFIG_PATH}" != /* ]]; then
        echo "RL_CONFIG_PATH must be absolute" >&2
        exit 2
    fi
    rm -f /run/rl/readiness.json /run/rl/client-managed-ready "${training_admitted_marker}"
fi

if [ ! -x "${client_bin}" ]; then
    echo "Client executable is missing: ${client_bin}" >&2
    exit 1
fi

# Help is a meta operation: it must not enter the Client business lifecycle.
# Business arguments remain byte-for-byte inputs to the C++ config layer below.
if [ "$#" -eq 1 ]; then
    case "$1" in
        --help|-h)
            cd "${repo_dir}"
            exec "${client_bin}" "$@"
            ;;
    esac
fi

client_pid=""
stopping=0

terminate_process() {
    local pid="$1"
    local timeout_seconds="$2"
    if [ -z "${pid}" ] || ! kill -0 "${pid}" 2>/dev/null; then
        return
    fi
    kill -TERM "${pid}" 2>/dev/null || true
    local waited=0
    while kill -0 "${pid}" 2>/dev/null &&
          [ "${waited}" -lt "${timeout_seconds}" ]; do
        sleep 1
        waited=$((waited + 1))
    done
    if kill -0 "${pid}" 2>/dev/null; then
        kill -KILL "${pid}" 2>/dev/null || true
    fi
    wait "${pid}" 2>/dev/null || true
}

shutdown() {
    if [ "${stopping}" -eq 1 ]; then
        return
    fi
    stopping=1
    terminate_process "${client_pid}" 4
    client_pid=""
    if [ "${managed}" -eq 1 ]; then
        rm -f /run/rl/readiness.json /run/rl/client-managed-ready "${training_admitted_marker}"
    fi
}

on_signal() {
    shutdown
    exit 0
}

trap shutdown EXIT
trap on_signal TERM INT

cd "${repo_dir}"
"${client_bin}" "$@" &
client_pid=$!

if [ "${managed}" -eq 1 ]; then
    managed_ready=0
    for _ in $(seq 1 100); do
        if [ -s /run/rl/client-managed-ready ]; then
            managed_ready=1
            break
        fi
        if ! kill -0 "${client_pid}" 2>/dev/null; then
            break
        fi
        sleep 0.1
    done
    if [ "${managed_ready}" -ne 1 ]; then
        echo "Client managed readiness timeout" >&2
        exit 1
    fi
    aiserver_alias="$(awk 'index($0, "aiserver_alias=") == 1 { print substr($0, 16); exit }' /run/rl/client-managed-ready)"
    if [[ ! "${aiserver_alias}" =~ ^aiserver-[0-9]+$ ]]; then
        echo "Client managed AIServer alias is invalid" >&2
        exit 1
    fi
    python3 scripts/publish_readiness.py \
        --component maze-client \
        --config "${RL_CONFIG_PATH}" \
        --fact grpc_transport=connected \
        --fact aiserver_alias="${aiserver_alias}"

    while kill -0 "${client_pid}" 2>/dev/null; do
        if [ -s "${training_admission_path}" ]; then
            python3 scripts/validate_training_admission.py \
                --execution "${execution_identity_path}" \
                --token "${training_admission_path}" \
                --marker "${training_admitted_marker}"
            break
        fi
        sleep 0.1
    done
    if [ ! -s "${training_admitted_marker}" ]; then
        if wait "${client_pid}"; then
            client_status=0
        else
            client_status=$?
        fi
        client_pid=""
        echo "Client exited before receiving exact training admission" >&2
        if [ "${client_status}" -eq 0 ]; then
            client_status=1
        fi
        exit "${client_status}"
    fi
    if wait "${client_pid}"; then
        client_status=0
    else
        client_status=$?
    fi
    client_pid=""
    exit "${client_status}"
fi

if wait "${client_pid}"; then
    client_status=0
else
    client_status=$?
fi
client_pid=""
exit "${client_status}"
