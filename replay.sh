#!/usr/bin/env bash

set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mode="${1:-local-test}"
if [ "$#" -gt 0 ]; then
    shift
fi

case "${mode}" in
    inference-smoke)
        mode="local-test"
        ;;
    local-test|model-evaluation)
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

replay_dir="${RL_VIZ_OUTPUT_DIR:-${repo_dir}/log/viz}"
replay_host="${RL_REPLAY_HOST:-0.0.0.0}"
replay_port="${RL_REPLAY_PORT:-9004}"
validation_id="${RL_VALIDATION_ID:-local-validation}"

exec python3 -u "${repo_dir}/tools/viz_player/maze_viz_server.py" \
    --dir "${replay_dir}" \
    --host "${replay_host}" \
    --port "${replay_port}" \
    --mode "${mode}" \
    --validation-id "${validation_id}" \
    "$@"
