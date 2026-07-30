#!/usr/bin/env bash

set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mode="${1:-${MAZE_WORKLOAD:-inference-smoke}}"
if [ "$#" -gt 0 ]; then
    shift
fi

case "${mode}" in
    inference-smoke|model-evaluation)
        ;;
    training|train)
        echo "Replay is disabled for training" >&2
        exit 2
        ;;
    *)
        echo "unknown replay mode: ${mode}" >&2
        exit 2
        ;;
esac

replay_dir="${MAZE_VIZ_OUTPUT_DIR:-${repo_dir}/log/viz}"
replay_host="${MAZE_REPLAY_HOST:-0.0.0.0}"
replay_port="${MAZE_REPLAY_PORT:-9004}"
validation_id="${MAZE_VALIDATION_ID:-${MAZE_RUN_ID:-local-validation}}"

exec python3 -u "${repo_dir}/tools/viz_player/maze_viz_server.py" \
    --dir "${replay_dir}" \
    --host "${replay_host}" \
    --port "${replay_port}" \
    --mode "${mode}" \
    --validation-id "${validation_id}" \
    "$@"
