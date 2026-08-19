#!/usr/bin/env bash

set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mode="${1:-evaluation}"
if [ "$#" -gt 0 ]; then
    shift
fi

case "${mode}" in
    evaluation)
        ;;
    training)
        echo "Replay is disabled for training" >&2
        exit 2
        ;;
    *)
        echo "unknown replay mode: ${mode}" >&2
        exit 2
        ;;
esac

: "${RL_VIZ_OUTPUT_DIR:?Replay directory must come from Client effective config}"
: "${RL_REPLAY_PORT:?Replay port must come from Client effective config}"
replay_dir="${RL_VIZ_OUTPUT_DIR}"
replay_host="${RL_REPLAY_HOST:-0.0.0.0}"
replay_port="${RL_REPLAY_PORT}"
validation_id="${RL_VALIDATION_ID:-local-validation}"

exec python3 -u "${repo_dir}/tools/viz_player/maze_viz_server.py" \
    --dir "${replay_dir}" \
    --host "${replay_host}" \
    --port "${replay_port}" \
    --mode "${mode}" \
    --validation-id "${validation_id}" \
    "$@"
