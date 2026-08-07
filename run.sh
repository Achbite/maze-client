#!/usr/bin/env bash

set -u

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
default_client_bin="${repo_dir}/build/maze_client"
if [ -x "${repo_dir}/bin/maze_client" ]; then
    default_client_bin="${repo_dir}/bin/maze_client"
fi
client_bin="${RL_CLIENT_BIN:-${default_client_bin}}"
client_config="${RL_CLIENT_CONFIG:-${repo_dir}/configs/client_config.yaml}"
replay_port="${RL_REPLAY_PORT:-9004}"
replay_dir="${RL_VIZ_OUTPUT_DIR:-${repo_dir}/log/viz}"
replay_bin="${RL_REPLAY_BIN:-${repo_dir}/replay.sh}"
session_policy_path="${RL_SESSION_POLICY_PATH:-/tmp/rl-client-session-policy.$$}"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --config)
            client_config="${2:?--config requires a value}"
            shift 2
            ;;
        --aiserver)
            address="${2:?--aiserver requires host:port}"
            export RL_AISERVER_HOST="${address%:*}"
            export RL_AISERVER_PORT="${address##*:}"
            shift 2
            ;;
        --replay-dir)
            replay_dir="${2:?--replay-dir requires a value}"
            shift 2
            ;;
        --replay-port)
            replay_port="${2:?--replay-port requires a value}"
            shift 2
            ;;
        *)
            echo "unknown argument: $1" >&2
            echo "Client workload is negotiated with AIServer through OpenSession" >&2
            exit 2
            ;;
    esac
done

if [ ! -x "${client_bin}" ]; then
    echo "Client executable is missing: ${client_bin}" >&2
    exit 1
fi

export RL_VIZ_OUTPUT_DIR="${replay_dir}"
export RL_REPLAY_PORT="${replay_port}"
export RL_SESSION_POLICY_PATH="${session_policy_path}"
rm -f "${session_policy_path}"

client_pid=""
replay_pid=""
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
    terminate_process "${replay_pid}" 3
    replay_pid=""
    rm -f "${session_policy_path}"
}
trap shutdown EXIT TERM INT

"${client_bin}" "${client_config}" &
client_pid=$!

policy_ready=0
for _ in $(seq 1 100); do
    if [ -s "${session_policy_path}" ]; then
        policy_ready=1
        break
    fi
    if ! kill -0 "${client_pid}" 2>/dev/null; then
        wait "${client_pid}"
        status=$?
        client_pid=""
        echo "Client exited before publishing OpenSession policy" >&2
        if [ "${status}" -eq 0 ]; then
            status=1
        fi
        exit "${status}"
    fi
    sleep 0.1
done
if [ "${policy_ready}" -ne 1 ]; then
    echo "OpenSession policy readiness timeout" >&2
    exit 1
fi

workload="$(
    awk -F= '$1 == "workload" { print $2; exit }' \
        "${session_policy_path}"
)"
replay_policy="$(
    awk -F= '$1 == "replay_policy" { print $2; exit }' \
        "${session_policy_path}"
)"
behavior_policy_scope="$(
    awk -F= '$1 == "behavior_policy_scope" { print $2; exit }' \
        "${session_policy_path}"
)"
model_version="$(
    awk -F= '$1 == "model_version" { print $2; exit }' \
        "${session_policy_path}"
)"
model_checksum="$(
    awk -F= '$1 == "model_artifact_digest" { print $2; exit }' \
        "${session_policy_path}"
)"
case "${workload}:${replay_policy}" in
    training:disabled)
        if [ "${behavior_policy_scope}" != "training-fragment" ]; then
            echo "Training requires fragment-scoped behavior policy" >&2
            exit 1
        fi
        ;;
    map-validation:disabled)
        if [ "${behavior_policy_scope}" != "none" ]; then
            echo "Map validation must not bind a behavior policy" >&2
            exit 1
        fi
        ;;
    local-test:record-and-serve|\
    model-evaluation:record-and-serve)
        if [ "${behavior_policy_scope}" != "evaluation-episode" ]; then
            echo "Evaluation requires an episode-scoped behavior policy" >&2
            exit 1
        fi
        if [[ ! "${model_version}" =~ ^[0-9]+$ ]] ||
           [[ ! "${model_checksum}" =~ ^[0-9a-f]{64}$ ]]; then
            echo "Replay requires a valid model identity" >&2
            exit 1
        fi
        if [ ! -f "${replay_bin}" ]; then
            echo "Replay launcher is missing: ${replay_bin}" >&2
            exit 1
        fi
        mkdir -p "${replay_dir}"
        validation_id="${RL_VALIDATION_ID:-local-validation}"
        if [[ ! "${validation_id}" =~ ^[A-Za-z0-9._-]+$ ]]; then
            echo "Invalid validation ID" >&2
            exit 1
        fi
        validation_manifest="${replay_dir}/validation-manifest.json"
        validation_manifest_temp="${validation_manifest}.tmp.$$"
        if ! printf '{"schema_version":1,"validation_id":"%s","model":{"version":%s,"sha256":"%s"},"parameters":{"workload":"%s"}}\n' \
            "${validation_id}" "${model_version}" "${model_checksum}" \
            "${workload}" > "${validation_manifest_temp}"; then
            rm -f "${validation_manifest_temp}"
            exit 1
        fi
        if ! mv "${validation_manifest_temp}" "${validation_manifest}"; then
            rm -f "${validation_manifest_temp}"
            exit 1
        fi
        bash "${replay_bin}" "${workload}" &
        replay_pid=$!

        replay_ready=0
        for _ in $(seq 1 50); do
            if ! kill -0 "${replay_pid}" 2>/dev/null; then
                wait "${replay_pid}"
                exit $?
            fi
            if (exec 3<>"/dev/tcp/127.0.0.1/${replay_port}") \
                2>/dev/null; then
                exec 3>&-
                exec 3<&-
                replay_ready=1
                break
            fi
            sleep 0.1
        done
        if [ "${replay_ready}" -ne 1 ]; then
            echo "Replay server readiness timeout" >&2
            exit 1
        fi
        ;;
    *)
        echo "Invalid OpenSession policy: ${workload}:${replay_policy}" >&2
        exit 1
        ;;
esac

wait "${client_pid}"
client_status=$?
client_pid=""

if [ "${replay_policy}" = "disabled" ]; then
    exit "${client_status}"
fi

validation_id="${RL_VALIDATION_ID:-local-validation}"
result_path="${RL_VALIDATION_RESULT_PATH:-${replay_dir}/client-result.json}"
completed_ts="$(date +%s)"
mkdir -p "$(dirname "${result_path}")"
result_temp="${result_path}.tmp.$$"
if ! printf '{"schema_version":1,"validation_id":"%s","workload":"%s","exit_code":%d,"completed_ts":%s}\n' \
    "${validation_id}" "${workload}" "${client_status}" "${completed_ts}" \
    > "${result_temp}"; then
    rm -f "${result_temp}"
    exit 1
fi
if ! mv "${result_temp}" "${result_path}"; then
    rm -f "${result_temp}"
    exit 1
fi

while [ "${stopping}" -eq 0 ]; do
    if ! kill -0 "${replay_pid}" 2>/dev/null; then
        wait "${replay_pid}"
        exit $?
    fi
    sleep 0.5
done
