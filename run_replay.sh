#!/usr/bin/env bash

set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
exec python3 "${repo_dir}/scripts/replay_service.py" \
    --repo-dir "${repo_dir}" \
    "$@"
