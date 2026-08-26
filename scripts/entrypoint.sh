#!/usr/bin/env bash

set -euo pipefail

cd /opt/rl/maze-client
if [ -n "${RL_CONFIG_PATH:-}" ]; then
    if [ "$#" -ne 0 ] &&
       { [ "$#" -ne 2 ] || [ "$1" != "--config" ] ||
         [ "$2" != "configs/client_config.yaml" ]; }; then
        echo "managed Client accepts only RL_CONFIG_PATH" >&2
        exit 2
    fi
    exec ./run.sh --config "${RL_CONFIG_PATH}"
fi
exec ./run.sh "$@"
