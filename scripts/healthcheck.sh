#!/usr/bin/env sh

set -eu

if [ -n "${RL_CONFIG_PATH:-}" ]; then
    test -f /run/rl/readiness.json && test -s /run/rl/readiness.json
else
    test -f /tmp/rl-client-session-policy && test -s /tmp/rl-client-session-policy
fi
