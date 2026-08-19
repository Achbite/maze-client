#!/usr/bin/env bash

set -euo pipefail

cd /opt/rl/maze-client
exec ./run.sh "$@"
