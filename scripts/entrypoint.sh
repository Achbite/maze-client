#!/usr/bin/env bash

set -euo pipefail

cd /opt/rl/maze-client
exec ./run.sh --config configs/client_config.yaml
