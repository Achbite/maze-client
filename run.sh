#!/usr/bin/env bash

set -u

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
default_client_bin="${repo_dir}/build/maze_client"
if [ -x "${repo_dir}/bin/maze_client" ]; then
    default_client_bin="${repo_dir}/bin/maze_client"
fi
client_bin="${RL_CLIENT_BIN:-${default_client_bin}}"
replay_bin="${RL_REPLAY_BIN:-${repo_dir}/replay.sh}"
session_policy_path="/tmp/rl-client-session-policy"
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

# Help is a meta operation: it must not enter the OpenSession/Replay
# supervision lifecycle. Business arguments remain byte-for-byte inputs to the
# C++ config layer below.
if [ "$#" -eq 1 ]; then
    case "$1" in
        --help|-h)
            cd "${repo_dir}"
            exec "${client_bin}" "$@"
            ;;
    esac
fi

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

policy_value() {
    awk -v key="$1" \
        'index($0, key "=") == 1 { print substr($0, length(key) + 2); exit }' \
        "${session_policy_path}"
}

workload="$(policy_value workload)"
replay_policy="$(policy_value replay_policy)"
behavior_policy_scope="$(policy_value behavior_policy_scope)"
model_step="$(policy_value model_step)"
model_lineage_id="$(policy_value model_lineage_id)"
model_checksum="$(policy_value model_artifact_digest)"
replay_dir="$(policy_value replay_output_dir)"
replay_port="$(policy_value replay_server_port)"
if [[ "${replay_dir}" != /* ]] ||
   [[ ! "${replay_port}" =~ ^[0-9]+$ ]] ||
   [ "${replay_port}" -le 0 ] || [ "${replay_port}" -gt 65535 ]; then
    echo "Client effective Replay config handoff is invalid" >&2
    exit 1
fi
case "${workload}:${replay_policy}" in
    training:disabled)
        if [ "${behavior_policy_scope}" != "training-agent-segment" ]; then
            echo "Training requires Agent-segment-scoped behavior policy" >&2
            exit 1
        fi
        if [[ ! "${model_step}" =~ ^[0-9]+$ ]] ||
           [ -z "${model_lineage_id}" ]; then
            echo "Training requires lineage and an explicit model step" >&2
            exit 1
        fi
        ;;
    evaluation:record-and-serve)
        if [ "${behavior_policy_scope}" != "evaluation-episode" ]; then
            echo "Evaluation requires an episode-scoped behavior policy" >&2
            exit 1
        fi
        if [ -n "${model_step}" ] || [ -n "${model_lineage_id}" ] ||
           [[ ! "${model_checksum}" =~ ^[0-9a-f]{64}$ ]]; then
            echo "Evaluation Replay requires a digest-only model binding" >&2
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
        if ! printf '{"schema_version":2,"validation_id":"%s","model":{"sha256":"%s"},"parameters":{"workload":"%s"}}\n' \
            "${validation_id}" "${model_checksum}" \
            "${workload}" > "${validation_manifest_temp}"; then
            rm -f "${validation_manifest_temp}"
            exit 1
        fi
        if ! mv "${validation_manifest_temp}" "${validation_manifest}"; then
            rm -f "${validation_manifest_temp}"
            exit 1
        fi
        RL_VIZ_OUTPUT_DIR="${replay_dir}" \
        RL_REPLAY_PORT="${replay_port}" \
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
