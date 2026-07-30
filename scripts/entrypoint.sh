#!/usr/bin/env bash

set -euo pipefail

cd /opt/rl/maze-client
workload="${MAZE_WORKLOAD:-${MAZE_RUN_MODE:-training}}"
if [ "${workload}" = "local-model-validation" ]; then
    workload="model-evaluation"
fi

exec ./run.sh "${workload}" --config configs/client_config.yaml
