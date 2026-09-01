#!/usr/bin/env sh

set -eu

if [ "${RL_INFRA_MANAGED:-}" = "true" ]; then
    test -f /run/rl/readiness.json && test -s /run/rl/readiness.json
else
    pgrep -x maze_client >/dev/null
fi
