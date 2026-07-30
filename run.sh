#!/usr/bin/env bash

set -u

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
workload="${1:-${MAZE_WORKLOAD:-}}"
if [ -z "${workload}" ]; then
    echo "workload is required: inference-smoke, training, or model-evaluation" >&2
    exit 2
fi
if [ "$#" -gt 0 ] && [ "$1" = "${workload}" ]; then
    shift
fi

case "${workload}" in
    inference-smoke|training|model-evaluation)
        ;;
    *)
        echo "unknown workload: ${workload}" >&2
        exit 2
        ;;
esac

if [ -n "${MAZE_DEV_PROFILE:-}" ] &&
   [ "${MAZE_DEV_PROFILE}" != "${workload}" ]; then
    echo "workload ${workload} does not match dev profile ${MAZE_DEV_PROFILE}" >&2
    exit 2
fi

default_client_bin="${repo_dir}/build/maze_client"
if [ -x "${repo_dir}/bin/maze_client" ]; then
    default_client_bin="${repo_dir}/bin/maze_client"
fi
client_bin="${MAZE_CLIENT_BIN:-${default_client_bin}}"
client_config="${MAZE_CLIENT_CONFIG:-${repo_dir}/configs/client_config.yaml}"
replay_port="${MAZE_REPLAY_PORT:-9004}"
replay_dir="${MAZE_VIZ_OUTPUT_DIR:-${repo_dir}/log/viz}"
replay_bin="${MAZE_REPLAY_BIN:-${repo_dir}/replay.sh}"
replay_enabled=""
requested_agents=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --config)
            client_config="${2:?--config requires a value}"
            shift 2
            ;;
        --aiserver)
            address="${2:?--aiserver requires host:port}"
            export MAZE_AISERVER_HOST="${address%:*}"
            export MAZE_AISERVER_PORT="${address##*:}"
            shift 2
            ;;
        --agents)
            requested_agents="${2:?--agents requires a value}"
            export MAZE_AGENT_NUM="${requested_agents}"
            shift 2
            ;;
        --episodes)
            export MAZE_MAX_EPISODES="${2:?--episodes requires a value}"
            shift 2
            ;;
        --max-steps)
            export MAZE_MAX_STEPS="${2:?--max-steps requires a value}"
            shift 2
            ;;
        --replay-dir)
            replay_dir="${2:?--replay-dir requires a value}"
            export MAZE_VIZ_OUTPUT_DIR="${replay_dir}"
            shift 2
            ;;
        --replay-port)
            replay_port="${2:?--replay-port requires a value}"
            export MAZE_REPLAY_PORT="${replay_port}"
            shift 2
            ;;
        --no-replay)
            export MAZE_VIZ_ENABLED=false
            shift
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

if [ ! -x "${client_bin}" ]; then
    echo "Client executable is missing: ${client_bin}" >&2
    exit 1
fi

export MAZE_WORKLOAD="${workload}"
if [ "${workload}" = "training" ]; then
    if [ -n "${requested_agents}" ] && [ "${requested_agents}" != "4" ]; then
        echo "training requires exactly 4 agents" >&2
        exit 2
    fi
    export MAZE_AGENT_NUM=4
    export MAZE_VIZ_ENABLED=false
else
    export MAZE_VIZ_ENABLED="${MAZE_VIZ_ENABLED:-true}"
fi
replay_enabled="${MAZE_VIZ_ENABLED}"
export MAZE_VIZ_OUTPUT_DIR="${replay_dir}"
export MAZE_REPLAY_PORT="${replay_port}"

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
    terminate_process "${replay_pid}" 3
}
trap shutdown EXIT TERM INT

validation_id="${MAZE_VALIDATION_ID:-local-validation}"
if [ "${workload}" != "training" ] &&
   [ "${replay_enabled}" = "true" ]; then
    if [ ! -f "${replay_bin}" ]; then
        echo "Replay launcher is missing: ${replay_bin}" >&2
        exit 1
    fi
    mkdir -p "${replay_dir}"
    bash "${replay_bin}" "${workload}" \
        --dir "${replay_dir}" \
        --host 0.0.0.0 \
        --port "${replay_port}" \
        --validation-id "${validation_id}" &
    replay_pid=$!

    replay_ready=0
    for _ in $(seq 1 50); do
        if ! kill -0 "${replay_pid}" 2>/dev/null; then
            wait "${replay_pid}"
            exit $?
        fi
        if (exec 3<>"/dev/tcp/127.0.0.1/${replay_port}") 2>/dev/null; then
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
fi

"${client_bin}" "${client_config}" &
client_pid=$!
wait "${client_pid}"
client_status=$?
client_pid=""

if [ "${workload}" = "training" ] ||
   [ "${replay_enabled}" != "true" ]; then
    exit "${client_status}"
fi

result_path="${MAZE_VALIDATION_RESULT_PATH:-${replay_dir}/client-result.json}"
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
